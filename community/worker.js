/* community/worker.js -- the Community service: a Cloudflare Worker that serves the site's
 * Community section (posts, comments, reactions) from GitHub Discussions, brokers "Sign in with
 * GitHub", and hosts screenshot uploads.
 *
 * Why it exists: repository Discussions are GraphQL-only, and GitHub's GraphQL API requires a
 * token even for public data -- the site cannot fetch discussions anonymously the way the
 * changelog fetches releases. So the site calls this service: reads are proxied with a credential
 * only it holds, and writes are performed with the SIGNED-IN USER's token so authorship is really
 * them. The GitHub client secret and user tokens never touch the browser.
 *
 * Identity model:
 *   - reads: the org-owned GitHub App (secrets APP_ID + APP_PRIVATE_KEY -> installation token),
 *     with a plain PAT fallback (secret GITHUB_TOKEN).
 *   - writes: OAuth authorization-code flow against the same App (secret CLIENT_SECRET). The
 *     browser holds only an opaque session id (localStorage, sent as a Bearer header); the user's
 *     GitHub token lives in Workers KV, server-side.
 *
 * Surface (JSON unless noted; CORS for the site origin; * = requires a Bearer session):
 *   GET  /                                      health check
 *   GET  /auth/login                            302 to GitHub authorize (KV-backed state)
 *   GET  /auth/callback?code&state              exchange code, mint session, 302 back to the site
 *   GET  /auth/me                             * session's { login, name, avatar }
 *   POST /auth/logout                         * delete the session
 *   GET  /community/categories                  discussion categories (the section tabs)
 *   GET  /community/discussions?category&after  paged post list, newest first
 *   GET  /community/discussions/:number         one post: bodyHTML + comments + reactions
 *   POST /community/discussions               * create a post { categoryId, title, body }
 *   POST /community/discussions/:number/comments * add a comment { body, replyToId? }
 *   POST /community/reactions                 * add a reaction { subjectId, content }
 *   POST /community/preview                   * render markdown -> GitHub-sanitized HTML { text }
 *   POST /media/upload?name=<filename>        * store an image in R2 (raw body) -> { url }
 *   GET  /media/<key>                           stream an uploaded image (long-cache)
 *
 * Abuse posture: session-gated writes + per-session KV rate limits + size caps; image uploads are
 * magic-byte sniffed against an allowlist. Optional Turnstile: when secret TURNSTILE_SECRET is
 * set, create endpoints also require a `turnstile` token in the JSON body (site-key pair on the
 * pages). GitHub's own moderation tools are the backstop.
 *
 * Ops notes:
 *   - secrets: APP_ID, APP_PRIVATE_KEY (PKCS#8 PEM), CLIENT_SECRET; optional GITHUB_TOKEN,
 *     TURNSTILE_SECRET. Bindings: KV SESSIONS, R2 MEDIA (wrangler.toml).
 *   - read caching: an in-isolate TTL cache (the edge Cache API is a no-op on workers.dev
 *     domains); a successful write flushes it so the writer sees their post promptly.
 */

const REPO_OWNER = 'doom-snapmap';
const REPO_NAME = 'snapmap-plus';
const API = 'https://api.github.com';

const SITE_ORIGIN = 'https://doom-snapmap.github.io';
const SITE_BASE = SITE_ORIGIN + '/snapmap-plus';
const CLIENT_ID = 'Iv23liFSVvbqF4r8a2uK';   // public identifier of the snapmap-plus-community App

/* Categories reserved for other channels (Discord) never surface on the site -- not in the
 * tabs, not in the composer, and posts filed under them stay off the site's lists. Creating a
 * new category in repo settings needs no code change: anything not on this list appears
 * automatically. */
const HIDDEN_CATEGORY_SLUGS = new Set(['polls', 'show-and-tell', 'ideas']);

/* Tab order: content categories first, housekeeping last; anything new lands in between,
 * alphabetically. */
const CATEGORY_ORDER = { 'how-to-guides': 0, 'tips-tricks': 1, 'help-questions': 2, 'general': 8, 'announcements': 9 };
function categoryRank(slug) {
  return CATEGORY_ORDER[slug] !== undefined ? CATEGORY_ORDER[slug] : 5;
}

/* cache TTLs (seconds): categories change rarely; lists/posts should feel fresh */
const TTL_CATEGORIES = 600;
const TTL_LIST = 45;
const TTL_POST = 45;

const PAGE_SIZE = 20;
const COMMENT_PAGE = 50;
const REPLY_PAGE = 50;

const SESSION_TTL = 30 * 86400;      // 30 days
const STATE_TTL = 600;               // 10 minutes to complete the GitHub round-trip

const MAX_UPLOAD = 8 * 1024 * 1024;  // 8 MB per image
/* per-session hourly write budgets -- generous for a human, a wall for a loop */
const LIMITS = { post: 10, comment: 60, reaction: 120, upload: 30, preview: 120, edit: 60 };

/* ---------------- small helpers ---------------- */

function json(obj, status) {
  return new Response(JSON.stringify(obj), {
    status: status || 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

function randomHex(bytes) {
  const buf = new Uint8Array(bytes);
  crypto.getRandomValues(buf);
  return [...buf].map(b => b.toString(16).padStart(2, '0')).join('');
}

/* ---------------- CORS ---------------- */

function corsOrigin(req) {
  const origin = req.headers.get('Origin');
  if (!origin) return null;
  if (origin === SITE_ORIGIN) return origin;
  /* local development of the static site */
  if (/^http:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/.test(origin)) return origin;
  return null;
}

function withCors(req, res) {
  const origin = corsOrigin(req);
  const h = new Headers(res.headers);
  if (origin) {
    h.set('Access-Control-Allow-Origin', origin);
    h.set('Vary', 'Origin');
  }
  return new Response(res.body, { status: res.status, headers: h });
}

function preflight(req) {
  const origin = corsOrigin(req);
  if (!origin) return new Response(null, { status: 403 });
  return new Response(null, {
    status: 204,
    headers: {
      'Access-Control-Allow-Origin': origin,
      'Access-Control-Allow-Methods': 'GET, POST, PATCH, DELETE, OPTIONS',
      'Access-Control-Allow-Headers': 'Authorization, Content-Type',
      'Access-Control-Max-Age': '86400',
      'Vary': 'Origin',
    },
  });
}

/* ---------------- GitHub App auth (installation token, for reads) ---------------- */

let tokenCache = { token: null, exp: 0 };

function b64u(bytes) {
  return btoa(String.fromCharCode(...new Uint8Array(bytes))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

async function appJwt(env) {
  const now = Math.floor(Date.now() / 1000);
  const enc = new TextEncoder();
  const head = b64u(enc.encode(JSON.stringify({ alg: 'RS256', typ: 'JWT' })));
  /* trim: a secret piped in via a shell often carries a trailing newline; GitHub rejects an iss with one */
  const pay = b64u(enc.encode(JSON.stringify({ iat: now - 60, exp: now + 540, iss: String(env.APP_ID).trim() })));
  const pem = env.APP_PRIVATE_KEY.replace(/-----[^-]+-----/g, '').replace(/\s+/g, '');
  const der = Uint8Array.from(atob(pem), c => c.charCodeAt(0));
  const key = await crypto.subtle.importKey('pkcs8', der, { name: 'RSASSA-PKCS1-v1_5', hash: 'SHA-256' }, false, ['sign']);
  const sig = await crypto.subtle.sign('RSASSA-PKCS1-v1_5', key, enc.encode(head + '.' + pay));
  return head + '.' + pay + '.' + b64u(sig);
}

async function authToken(env) {
  if (!(env.APP_ID && env.APP_PRIVATE_KEY)) return env.GITHUB_TOKEN || null;   // PAT fallback
  const now = Date.now() / 1000;
  if (tokenCache.token && now < tokenCache.exp - 120) return tokenCache.token;
  let jwt;
  try { jwt = await appJwt(env); }
  catch (e) { console.log('app auth: jwt build failed:', e.message); return null; }
  const hdrs = {
    'Authorization': 'Bearer ' + jwt, 'Accept': 'application/vnd.github+json',
    'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'snapmap-plus-community',
  };
  const instRes = await fetch(API + '/repos/' + REPO_OWNER + '/' + REPO_NAME + '/installation', { headers: hdrs });
  if (!instRes.ok) { console.log('app auth: installation lookup ->', instRes.status, (await instRes.text()).slice(0, 200)); return null; }
  const inst = await instRes.json();
  const tokRes = await fetch(API + '/app/installations/' + inst.id + '/access_tokens', { method: 'POST', headers: hdrs });
  if (!tokRes.ok) { console.log('app auth: token exchange ->', tokRes.status, (await tokRes.text()).slice(0, 200)); return null; }
  const tok = await tokRes.json();
  tokenCache = { token: tok.token, exp: now + 3300 };   // installation tokens live ~1h
  return tok.token;
}

/* ---------------- GraphQL ---------------- */

async function gql(token, query, variables) {
  const res = await fetch(API + '/graphql', {
    method: 'POST',
    headers: {
      'Authorization': 'Bearer ' + token,
      'Content-Type': 'application/json',
      'User-Agent': 'snapmap-plus-community',
    },
    body: JSON.stringify({ query, variables: variables || {} }),
  });
  if (!res.ok) { console.log('graphql http', res.status, (await res.text()).slice(0, 300)); return null; }
  const out = await res.json();
  /* a missing node (e.g. discussion number that doesn't exist) arrives as a NOT_FOUND entry in
   * `errors` ALONGSIDE partial data -- pass the data through so callers can 404 precisely; only a
   * response with no data at all is an upstream failure */
  if (out.errors) console.log('graphql errors:', JSON.stringify(out.errors).slice(0, 500));
  return out.data || null;
}

const FRAG_AUTHOR = 'fragment authorFields on Actor { login avatarUrl url }';

const Q_CATEGORIES = `
query {
  repository(owner: "${REPO_OWNER}", name: "${REPO_NAME}") {
    id
    discussionCategories(first: 20) {
      nodes { id name slug description emojiHTML isAnswerable }
    }
  }
}`;

const Q_LIST = FRAG_AUTHOR + `
query($first: Int!, $after: String, $categoryId: ID, $orderField: DiscussionOrderField!) {
  repository(owner: "${REPO_OWNER}", name: "${REPO_NAME}") {
    discussions(first: $first, after: $after, categoryId: $categoryId,
                orderBy: { field: $orderField, direction: DESC }) {
      totalCount
      pageInfo { endCursor hasNextPage }
      nodes {
        number title createdAt updatedAt answerChosenAt url
        author { ...authorFields }
        category { name slug isAnswerable }
        labels(first: 6) { nodes { name color } }
        comments { totalCount }
        reactions { totalCount }
      }
    }
  }
}`;

/* GitHub's search backend, scoped to this repo's discussions -- powers the site's search box */
const Q_SEARCH = FRAG_AUTHOR + `
query($q: String!, $first: Int!) {
  search(type: DISCUSSION, query: $q, first: $first) {
    discussionCount
    nodes {
      ... on Discussion {
        number title createdAt updatedAt answerChosenAt url
        author { ...authorFields }
        category { name slug isAnswerable }
        labels(first: 6) { nodes { name color } }
        comments { totalCount }
        reactions { totalCount }
      }
    }
  }
}`;

/* `body` (raw markdown) rides along with `bodyHTML` so the author's Edit flow can prefill the
 * original source -- rendered HTML is not reversible */
const Q_POST = FRAG_AUTHOR + `
query($number: Int!) {
  repository(owner: "${REPO_OWNER}", name: "${REPO_NAME}") {
    discussion(number: $number) {
      id number title body bodyHTML createdAt answerChosenAt url
      author { ...authorFields }
      category { name slug isAnswerable }
      labels(first: 6) { nodes { name color } }
      reactionGroups { content reactors { totalCount } }
      comments(first: ${COMMENT_PAGE}) {
        totalCount
        nodes {
          id body bodyHTML createdAt url isAnswer
          author { ...authorFields }
          reactionGroups { content reactors { totalCount } }
          replies(first: ${REPLY_PAGE}) {
            totalCount
            nodes {
              id body bodyHTML createdAt url
              author { ...authorFields }
              reactionGroups { content reactors { totalCount } }
            }
          }
        }
      }
    }
  }
}`;

const M_CREATE_DISCUSSION = `
mutation($repositoryId: ID!, $categoryId: ID!, $title: String!, $body: String!) {
  createDiscussion(input: { repositoryId: $repositoryId, categoryId: $categoryId, title: $title, body: $body }) {
    discussion { number url }
  }
}`;

const M_ADD_COMMENT = `
mutation($discussionId: ID!, $body: String!, $replyToId: ID) {
  addDiscussionComment(input: { discussionId: $discussionId, body: $body, replyToId: $replyToId }) {
    comment { id url }
  }
}`;

const M_ADD_REACTION = `
mutation($subjectId: ID!, $content: ReactionContent!) {
  addReaction(input: { subjectId: $subjectId, content: $content }) {
    reaction { content }
  }
}`;

/* author controls -- GitHub enforces authorship server-side (an author can edit/delete their own
 * discussions and comments; repo maintainers can moderate anything), so these carry no
 * authorization logic of their own: a non-author's attempt simply comes back as an error */
const M_UPDATE_DISCUSSION = `
mutation($discussionId: ID!, $title: String, $body: String, $categoryId: ID) {
  updateDiscussion(input: { discussionId: $discussionId, title: $title, body: $body, categoryId: $categoryId }) {
    discussion { number }
  }
}`;

const M_DELETE_DISCUSSION = `
mutation($id: ID!) {
  deleteDiscussion(input: { id: $id }) {
    clientMutationId
  }
}`;

const M_UPDATE_COMMENT = `
mutation($commentId: ID!, $body: String!) {
  updateDiscussionComment(input: { commentId: $commentId, body: $body }) {
    comment { id }
  }
}`;

const M_DELETE_COMMENT = `
mutation($id: ID!) {
  deleteDiscussionComment(input: { id: $id }) {
    comment { id }
  }
}`;

const REACTION_CONTENTS = ['THUMBS_UP', 'THUMBS_DOWN', 'LAUGH', 'HOORAY', 'CONFUSED', 'HEART', 'ROCKET', 'EYES'];

/* ---------------- shaping ---------------- */

function shapeAuthor(a) {
  if (!a) return { login: 'ghost', avatarUrl: '', url: '' };
  return { login: a.login, avatarUrl: a.avatarUrl, url: a.url };
}

function shapeTags(labels) {
  return (labels && labels.nodes ? labels.nodes : []).map(l => ({ name: l.name, color: l.color }));
}

function shapeListNode(n) {
  return {
    number: n.number,
    title: n.title,
    createdAt: n.createdAt,
    updatedAt: n.updatedAt,
    url: n.url,
    author: shapeAuthor(n.author),
    category: n.category ? { name: n.category.name, slug: n.category.slug } : null,
    tags: shapeTags(n.labels),
    answerable: !!(n.category && n.category.isAnswerable),
    answered: !!n.answerChosenAt,
    commentCount: n.comments.totalCount,
    reactionCount: n.reactions.totalCount,
  };
}

function shapeReactions(groups) {
  const out = [];
  for (const g of groups || []) {
    const n = g.reactors ? g.reactors.totalCount : 0;
    if (n > 0) out.push({ content: g.content, count: n });
  }
  return out;
}

function shapeComment(c) {
  return {
    id: c.id,
    body: c.body,
    bodyHTML: c.bodyHTML,
    createdAt: c.createdAt,
    url: c.url,
    isAnswer: !!c.isAnswer,
    author: shapeAuthor(c.author),
    reactions: shapeReactions(c.reactionGroups),
    replies: (c.replies ? c.replies.nodes : []).map(r => ({
      id: r.id,
      body: r.body,
      bodyHTML: r.bodyHTML,
      createdAt: r.createdAt,
      url: r.url,
      author: shapeAuthor(r.author),
      reactions: shapeReactions(r.reactionGroups),
    })),
  };
}

/* ---------------- in-isolate TTL cache ---------------- */

const memCache = new Map();

function cacheGet(key) {
  const hit = memCache.get(key);
  if (hit && Date.now() < hit.exp) return hit.value;
  if (hit) memCache.delete(key);
  return null;
}

function cachePut(key, value, ttlSec) {
  if (memCache.size > 500) memCache.clear();   // crude bound; a warm isolate never nears this
  memCache.set(key, { value, exp: Date.now() + ttlSec * 1000 });
}

/* ---------------- sessions (KV) ---------------- */

async function getSession(env, req) {
  const auth = req.headers.get('Authorization') || '';
  const m = auth.match(/^Bearer\s+([0-9a-f]{64})$/i);
  if (!m) return null;
  const raw = await env.SESSIONS.get('session:' + m[1]);
  if (!raw) return null;
  try { return { sid: m[1], user: JSON.parse(raw) }; } catch { return null; }
}

/* naive per-session counter -- KV is not atomic, but a small overshoot is harmless here */
async function rateOk(env, sid, kind) {
  const limit = LIMITS[kind];
  const key = 'rate:' + kind + ':' + sid;
  const n = parseInt((await env.SESSIONS.get(key)) || '0', 10);
  if (n >= limit) return false;
  await env.SESSIONS.put(key, String(n + 1), { expirationTtl: 3600 });
  return true;
}

/* optional Turnstile check -- active only when the secret is configured */
async function turnstileOk(env, req, token) {
  if (!env.TURNSTILE_SECRET) return true;
  if (!token) return false;
  const res = await fetch('https://challenges.cloudflare.com/turnstile/v0/siteverify', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      secret: env.TURNSTILE_SECRET,
      response: token,
      remoteip: req.headers.get('CF-Connecting-IP') || undefined,
    }),
  });
  if (!res.ok) return false;
  const out = await res.json();
  return !!out.success;
}

/* ---------------- OAuth broker ---------------- */

function workerOrigin(req) {
  return new URL(req.url).origin;
}

async function authLogin(env, req) {
  if (!env.CLIENT_SECRET) return json({ error: 'sign-in not configured yet' }, 503);
  const state = randomHex(16);
  await env.SESSIONS.put('state:' + state, '1', { expirationTtl: STATE_TTL });
  const u = new URL('https://github.com/login/oauth/authorize');
  u.searchParams.set('client_id', CLIENT_ID);
  u.searchParams.set('redirect_uri', workerOrigin(req) + '/auth/callback');
  u.searchParams.set('state', state);
  return Response.redirect(u.toString(), 302);
}

async function authCallback(env, req, url) {
  const back = SITE_BASE + '/community.html';
  const code = url.searchParams.get('code');
  const state = url.searchParams.get('state');
  if (!code || !state) return Response.redirect(back + '#login_error', 302);
  const seen = await env.SESSIONS.get('state:' + state);
  if (!seen) return Response.redirect(back + '#login_error', 302);
  await env.SESSIONS.delete('state:' + state);

  const tokRes = await fetch('https://github.com/login/oauth/access_token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'Accept': 'application/json', 'User-Agent': 'snapmap-plus-community' },
    body: JSON.stringify({ client_id: CLIENT_ID, client_secret: env.CLIENT_SECRET, code }),
  });
  if (!tokRes.ok) return Response.redirect(back + '#login_error', 302);
  const tok = await tokRes.json();
  if (!tok.access_token) { console.log('oauth exchange:', JSON.stringify(tok).slice(0, 200)); return Response.redirect(back + '#login_error', 302); }

  const userRes = await fetch(API + '/user', {
    headers: {
      'Authorization': 'Bearer ' + tok.access_token, 'Accept': 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'snapmap-plus-community',
    },
  });
  if (!userRes.ok) return Response.redirect(back + '#login_error', 302);
  const user = await userRes.json();

  const sid = randomHex(32);
  await env.SESSIONS.put('session:' + sid, JSON.stringify({
    token: tok.access_token,
    login: user.login,
    name: user.name || user.login,
    avatar: user.avatar_url,
  }), { expirationTtl: SESSION_TTL });
  /* the session id rides the URL FRAGMENT (never sent to servers/logs); the page script pockets
   * it into localStorage and scrubs the URL */
  return Response.redirect(back + '#session=' + sid, 302);
}

/* ---------------- read handlers ---------------- */

async function getCategories(env) {
  const cached = cacheGet('categories');
  if (cached) return cached;
  const token = await authToken(env);
  if (!token) return { error: 'auth', status: 500 };
  const data = await gql(token, Q_CATEGORIES);
  if (!data) return { error: 'upstream', status: 502 };
  const cats = data.repository.discussionCategories.nodes
    .filter(c => !HIDDEN_CATEGORY_SLUGS.has(c.slug))
    .map(c => ({
      id: c.id, name: c.name, slug: c.slug, description: c.description,
      emojiHTML: c.emojiHTML, isAnswerable: c.isAnswerable,
    }))
    .sort((a, b) => (categoryRank(a.slug) - categoryRank(b.slug)) || a.name.localeCompare(b.name));
  const out = { repositoryId: data.repository.id, categories: cats };
  cachePut('categories', out, TTL_CATEGORIES);
  return out;
}

async function listDiscussions(env, categorySlug, after, sort) {
  /* 'top' has no GraphQL orderBy: fetch a wide window newest-first and rank by reactions here.
   * Correct while the forum holds <100 posts per view; beyond that the tail truncates (logged). */
  const top = sort === 'top';
  const orderField = sort === 'active' ? 'UPDATED_AT' : 'CREATED_AT';
  const key = 'list|' + (categorySlug || '') + '|' + (top ? '' : (after || '')) + '|' + (top ? 'TOP' : orderField);
  const cached = cacheGet(key);
  if (cached) return cached;

  let categoryId = null;
  if (categorySlug) {
    const cats = await getCategories(env);
    if (cats.error) return cats;
    const cat = cats.categories.find(c => c.slug === categorySlug);
    if (!cat) return { error: 'unknown category', status: 404 };
    categoryId = cat.id;
  }

  const token = await authToken(env);
  if (!token) return { error: 'auth', status: 500 };
  const data = await gql(token, Q_LIST, {
    first: top ? 100 : PAGE_SIZE,
    after: top ? null : (after || null),
    categoryId,
    orderField,
  });
  if (!data) return { error: 'upstream', status: 502 };
  const d = data.repository.discussions;
  let nodes = d.nodes
    .filter(n => !(n.category && HIDDEN_CATEGORY_SLUGS.has(n.category.slug)))
    .map(shapeListNode);
  let pageInfo = d.pageInfo;
  if (top) {
    if (d.pageInfo.hasNextPage) console.log('top sort: >100 discussions, ranking truncated to the newest 100');
    nodes.sort((a, b) => (b.reactionCount - a.reactionCount) || (a.createdAt < b.createdAt ? 1 : -1));
    nodes = nodes.slice(0, PAGE_SIZE);
    pageInfo = { endCursor: null, hasNextPage: false };
  }
  const out = {
    totalCount: d.totalCount,
    pageInfo,
    discussions: nodes,
  };
  cachePut(key, out, TTL_LIST);
  return out;
}

async function searchDiscussions(env, q) {
  const term = String(q || '').trim().slice(0, 200);
  if (term.length < 2) return { error: 'query too short', status: 400 };
  const key = 'search|' + term.toLowerCase();
  const cached = cacheGet(key);
  if (cached) return cached;
  const token = await authToken(env);
  if (!token) return { error: 'auth', status: 500 };
  const data = await gql(token, Q_SEARCH, {
    q: 'repo:' + REPO_OWNER + '/' + REPO_NAME + ' in:title,body ' + term,
    first: PAGE_SIZE,
  });
  if (!data) return { error: 'upstream', status: 502 };
  const out = {
    totalCount: data.search.discussionCount,
    discussions: data.search.nodes
      .filter(n => n && n.number)
      .filter(n => !(n.category && HIDDEN_CATEGORY_SLUGS.has(n.category.slug)))
      .map(shapeListNode),
  };
  cachePut(key, out, TTL_LIST);
  return out;
}

async function getDiscussion(env, number) {
  const key = 'post|' + number;
  const cached = cacheGet(key);
  if (cached) return cached;
  const token = await authToken(env);
  if (!token) return { error: 'auth', status: 500 };
  const data = await gql(token, Q_POST, { number });
  if (!data) return { error: 'upstream', status: 502 };
  const d = data.repository.discussion;
  if (!d) return { error: 'not found', status: 404 };
  const out = {
    id: d.id,
    number: d.number,
    title: d.title,
    body: d.body,
    bodyHTML: d.bodyHTML,
    createdAt: d.createdAt,
    url: d.url,
    author: shapeAuthor(d.author),
    category: d.category ? { name: d.category.name, slug: d.category.slug } : null,
    tags: shapeTags(d.labels),
    answerable: !!(d.category && d.category.isAnswerable),
    answered: !!d.answerChosenAt,
    reactions: shapeReactions(d.reactionGroups),
    commentCount: d.comments.totalCount,
    comments: d.comments.nodes.map(shapeComment),
  };
  cachePut(key, out, TTL_POST);
  return out;
}

/* ---------------- write handlers (run as the signed-in user) ---------------- */

async function createPost(env, req, session, body) {
  if (!(await turnstileOk(env, req, body.turnstile))) return { error: 'verification failed', status: 403 };
  if (!(await rateOk(env, session.sid, 'post'))) return { error: 'rate limited', status: 429 };
  const title = String(body.title || '').trim();
  const text = String(body.body || '').trim();
  const categoryId = String(body.categoryId || '');
  if (title.length < 3 || title.length > 200) return { error: 'bad title', status: 400 };
  if (text.length < 10 || text.length > 60000) return { error: 'bad body', status: 400 };
  const cats = await getCategories(env);
  if (cats.error) return cats;
  if (!cats.categories.some(c => c.id === categoryId)) return { error: 'bad category', status: 400 };
  const data = await gql(session.user.token, M_CREATE_DISCUSSION,
    { repositoryId: cats.repositoryId, categoryId, title, body: text });
  const disc = data && data.createDiscussion && data.createDiscussion.discussion;
  if (!disc) return { error: 'upstream', status: 502 };
  memCache.clear();   // the writer should see their post on the next fetch
  return { number: disc.number, url: disc.url };
}

async function createComment(env, req, session, number, body) {
  if (!(await turnstileOk(env, req, body.turnstile))) return { error: 'verification failed', status: 403 };
  if (!(await rateOk(env, session.sid, 'comment'))) return { error: 'rate limited', status: 429 };
  const text = String(body.body || '').trim();
  if (text.length < 1 || text.length > 60000) return { error: 'bad body', status: 400 };
  const post = await getDiscussion(env, number);
  if (post.error) return post;
  const vars = { discussionId: post.id, body: text, replyToId: body.replyToId ? String(body.replyToId) : null };
  const data = await gql(session.user.token, M_ADD_COMMENT, vars);
  const comment = data && data.addDiscussionComment && data.addDiscussionComment.comment;
  if (!comment) return { error: 'upstream', status: 502 };
  memCache.delete('post|' + number);
  return { id: comment.id, url: comment.url };
}

async function addReaction(env, req, session, body) {
  if (!(await rateOk(env, session.sid, 'reaction'))) return { error: 'rate limited', status: 429 };
  const subjectId = String(body.subjectId || '');
  const content = String(body.content || '');
  if (!subjectId || !REACTION_CONTENTS.includes(content)) return { error: 'bad reaction', status: 400 };
  const data = await gql(session.user.token, M_ADD_REACTION, { subjectId, content });
  if (!(data && data.addReaction)) return { error: 'upstream', status: 502 };
  memCache.clear();
  return { ok: true };
}

async function editPost(env, req, session, number, body) {
  if (!(await rateOk(env, session.sid, 'edit'))) return { error: 'rate limited', status: 429 };
  const title = String(body.title || '').trim();
  const text = String(body.body || '').trim();
  if (title.length < 3 || title.length > 200) return { error: 'bad title', status: 400 };
  if (text.length < 10 || text.length > 60000) return { error: 'bad body', status: 400 };
  const post = await getDiscussion(env, number);
  if (post.error) return post;
  const vars = { discussionId: post.id, title, body: text, categoryId: null };
  if (body.categoryId) {
    const cats = await getCategories(env);
    if (cats.error) return cats;
    if (!cats.categories.some(c => c.id === body.categoryId)) return { error: 'bad category', status: 400 };
    vars.categoryId = body.categoryId;
  }
  const data = await gql(session.user.token, M_UPDATE_DISCUSSION, vars);
  if (!(data && data.updateDiscussion && data.updateDiscussion.discussion)) {
    return { error: 'GitHub rejected the edit (only the author can edit)', status: 403 };
  }
  memCache.clear();
  return { number };
}

async function deletePost(env, req, session, number) {
  if (!(await rateOk(env, session.sid, 'edit'))) return { error: 'rate limited', status: 429 };
  const post = await getDiscussion(env, number);
  if (post.error) return post;
  const data = await gql(session.user.token, M_DELETE_DISCUSSION, { id: post.id });
  if (!(data && data.deleteDiscussion)) {
    return { error: 'GitHub rejected the delete (only the author can delete)', status: 403 };
  }
  memCache.clear();
  return { ok: true };
}

async function editComment(env, req, session, id, body) {
  if (!(await rateOk(env, session.sid, 'edit'))) return { error: 'rate limited', status: 429 };
  const text = String(body.body || '').trim();
  if (text.length < 1 || text.length > 60000) return { error: 'bad body', status: 400 };
  const data = await gql(session.user.token, M_UPDATE_COMMENT, { commentId: id, body: text });
  if (!(data && data.updateDiscussionComment && data.updateDiscussionComment.comment)) {
    return { error: 'GitHub rejected the edit (only the author can edit)', status: 403 };
  }
  memCache.clear();
  return { ok: true };
}

async function deleteComment(env, req, session, id) {
  if (!(await rateOk(env, session.sid, 'edit'))) return { error: 'rate limited', status: 429 };
  const data = await gql(session.user.token, M_DELETE_COMMENT, { id });
  if (!(data && data.deleteDiscussionComment)) {
    return { error: 'GitHub rejected the delete (only the author can delete)', status: 403 };
  }
  memCache.clear();
  return { ok: true };
}

/* markdown -> HTML through GitHub's own renderer+sanitizer, so the composer preview matches the
 * published rendering exactly */
async function previewMarkdown(env, session, body) {
  if (!(await rateOk(env, session.sid, 'preview'))) return { error: 'rate limited', status: 429 };
  const text = String(body.text || '');
  if (text.length > 60000) return { error: 'too long', status: 413 };
  const token = await authToken(env);
  if (!token) return { error: 'auth', status: 500 };
  const res = await fetch(API + '/markdown', {
    method: 'POST',
    headers: {
      'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'snapmap-plus-community',
      'Content-Type': 'application/json',
    },
    body: JSON.stringify({ text, mode: 'gfm', context: REPO_OWNER + '/' + REPO_NAME }),
  });
  if (!res.ok) return { error: 'upstream', status: 502 };
  return { html: await res.text() };
}

/* ---------------- media (R2) ---------------- */

const IMAGE_TYPES = {
  png:  { mime: 'image/png',  sniff: b => b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4E && b[3] === 0x47 },
  jpg:  { mime: 'image/jpeg', sniff: b => b[0] === 0xFF && b[1] === 0xD8 && b[2] === 0xFF },
  gif:  { mime: 'image/gif',  sniff: b => b[0] === 0x47 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x38 },
  webp: { mime: 'image/webp', sniff: b => b[0] === 0x52 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x46 &&
                                          b[8] === 0x57 && b[9] === 0x45 && b[10] === 0x42 && b[11] === 0x50 },
};

async function uploadMedia(env, req, session) {
  if (!(await rateOk(env, session.sid, 'upload'))) return { error: 'rate limited', status: 429 };
  const len = parseInt(req.headers.get('Content-Length') || '0', 10);
  if (len > MAX_UPLOAD) return { error: 'too large (8 MB max)', status: 413 };
  const buf = new Uint8Array(await req.arrayBuffer());
  if (buf.length === 0) return { error: 'empty upload', status: 400 };
  if (buf.length > MAX_UPLOAD) return { error: 'too large (8 MB max)', status: 413 };

  /* identify by magic bytes -- the client-sent name/type is advisory only */
  let ext = null, mime = null;
  for (const [e, t] of Object.entries(IMAGE_TYPES)) {
    if (buf.length > 12 && t.sniff(buf)) { ext = e; mime = t.mime; break; }
  }
  if (!ext) return { error: 'not a supported image (png/jpg/gif/webp)', status: 415 };

  const key = 'community/' + session.user.login + '/' + randomHex(16) + '.' + ext;
  await env.MEDIA.put(key, buf, {
    httpMetadata: { contentType: mime, cacheControl: 'public, max-age=31536000, immutable' },
    customMetadata: { uploader: session.user.login },
  });
  return { url: workerOrigin(req) + '/media/' + key };
}

async function serveMedia(env, key) {
  const obj = await env.MEDIA.get(key);
  if (!obj) return json({ error: 'not found' }, 404);
  const h = new Headers();
  h.set('Content-Type', (obj.httpMetadata && obj.httpMetadata.contentType) || 'application/octet-stream');
  h.set('Cache-Control', 'public, max-age=31536000, immutable');
  h.set('etag', obj.httpEtag);
  /* defense-in-depth: never let anything under /media/ execute or be sniffed as HTML */
  h.set('X-Content-Type-Options', 'nosniff');
  h.set('Content-Security-Policy', "default-src 'none'");
  return new Response(obj.body, { headers: h });
}

/* ---------------- entry ---------------- */

export default {
  async fetch(req, env) {
    if (req.method === 'OPTIONS') return preflight(req);

    const url = new URL(req.url);
    const path = url.pathname.replace(/\/+$/, '') || '/';

    /* browser-navigation endpoints (no CORS needed) */
    if (req.method === 'GET' && path === '/') {
      return new Response('snapmap-plus community service: OK\n', { headers: { 'Content-Type': 'text/plain' } });
    }
    if (req.method === 'GET' && path === '/auth/login') return authLogin(env, req);
    if (req.method === 'GET' && path === '/auth/callback') return authCallback(env, req, url);
    if (req.method === 'GET' && path.startsWith('/media/')) {
      return serveMedia(env, url.pathname.slice('/media/'.length));
    }

    /* API endpoints (CORS) */
    let result = null;

    if (req.method === 'GET') {
      if (path === '/auth/me') {
        const s = await getSession(env, req);
        result = s ? { login: s.user.login, name: s.user.name, avatar: s.user.avatar }
                   : { error: 'not signed in', status: 401 };
      } else if (path === '/community/categories') {
        result = await getCategories(env);
      } else if (path === '/community/discussions') {
        result = await listDiscussions(env, url.searchParams.get('category'), url.searchParams.get('after'),
                                       url.searchParams.get('sort'));
      } else if (path === '/community/search') {
        result = await searchDiscussions(env, url.searchParams.get('q'));
      } else {
        const m = path.match(/^\/community\/discussions\/(\d+)$/);
        if (m) result = await getDiscussion(env, parseInt(m[1], 10));
      }
    } else if (req.method === 'POST') {
      const session = await getSession(env, req);
      if (path === '/auth/logout') {
        if (session) await env.SESSIONS.delete('session:' + session.sid);
        result = { ok: true };
      } else if (!session) {
        result = { error: 'not signed in', status: 401 };
      } else if (path === '/media/upload') {
        result = await uploadMedia(env, req, session);
      } else {
        let body;
        try { body = await req.json(); } catch { body = null; }
        if (!body) {
          result = { error: 'bad json', status: 400 };
        } else if (path === '/community/discussions') {
          result = await createPost(env, req, session, body);
        } else if (path === '/community/reactions') {
          result = await addReaction(env, req, session, body);
        } else if (path === '/community/preview') {
          result = await previewMarkdown(env, session, body);
        } else {
          const m = path.match(/^\/community\/discussions\/(\d+)\/comments$/);
          if (m) result = await createComment(env, req, session, parseInt(m[1], 10), body);
        }
      }
    } else if (req.method === 'PATCH' || req.method === 'DELETE') {
      const session = await getSession(env, req);
      if (!session) {
        result = { error: 'not signed in', status: 401 };
      } else {
        const mp = path.match(/^\/community\/discussions\/(\d+)$/);
        const mc = path.match(/^\/community\/comments\/([A-Za-z0-9_=-]+)$/);
        if (req.method === 'DELETE') {
          if (mp) result = await deletePost(env, req, session, parseInt(mp[1], 10));
          else if (mc) result = await deleteComment(env, req, session, mc[1]);
        } else {
          let body;
          try { body = await req.json(); } catch { body = null; }
          if (!body) result = { error: 'bad json', status: 400 };
          else if (mp) result = await editPost(env, req, session, parseInt(mp[1], 10), body);
          else if (mc) result = await editComment(env, req, session, mc[1], body);
        }
      }
    }

    if (!result) return withCors(req, json({ error: 'not found' }, 404));
    if (result.error) return withCors(req, json({ error: result.error }, result.status || 500));
    return withCors(req, json(result));
  },
};
