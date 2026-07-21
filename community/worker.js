/* community/worker.js -- the Community read proxy: a Cloudflare Worker that serves the site's
 * Community section (posts, comments, reactions) from GitHub Discussions.
 *
 * Why it exists: repository Discussions are GraphQL-only, and GitHub's GraphQL API requires a
 * token even for public data -- the site cannot fetch discussions anonymously the way the
 * changelog fetches releases. So the site calls this proxy, and the proxy queries GraphQL with a
 * credential only it holds, returning trimmed JSON the pages render.
 *
 * Identity: preferably a GITHUB APP owned by the org (secrets APP_ID + APP_PRIVATE_KEY), same
 * scheme as the feedback relay -- tokens are minted fresh per request window and expire on their
 * own. Falls back to a plain fine-grained PAT (secret GITHUB_TOKEN) when the app secrets are
 * absent. Reads of public discussions work with either.
 *
 * Endpoints (all GET, all JSON, CORS-enabled for the site origin):
 *   /                                     health check
 *   /community/categories                 discussion categories (the section tabs)
 *   /community/discussions?category&after paged post list, newest first, optional category slug
 *   /community/discussions/:number        one post: bodyHTML + comments + threaded replies + reactions
 *
 * Bodies are served as GitHub's own rendered bodyHTML (already sanitized upstream), so a post
 * written on github.com and one written in the site composer render identically.
 *
 * Ops notes:
 *   - secrets: `wrangler secret put APP_ID` + `APP_PRIVATE_KEY` (PKCS#8 PEM), or GITHUB_TOKEN.
 *   - caching: an in-isolate TTL cache (the edge Cache API is a no-op on workers.dev domains).
 *     Warm isolates answer from memory; cold starts refetch. Keeps the GraphQL point budget
 *     (5000/hr) far from exhaustion under browsing load.
 *   - stateless by design: no KV, no queues; GitHub Discussions is the only store.
 */

const REPO_OWNER = 'doom-snapmap';
const REPO_NAME = 'snapmap-plus';
const API = 'https://api.github.com';

const SITE_ORIGIN = 'https://doom-snapmap.github.io';

/* cache TTLs (seconds): categories change rarely; lists/posts should feel fresh */
const TTL_CATEGORIES = 600;
const TTL_LIST = 45;
const TTL_POST = 45;

const PAGE_SIZE = 20;
const COMMENT_PAGE = 50;
const REPLY_PAGE = 50;

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
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Authorization, Content-Type',
      'Access-Control-Max-Age': '86400',
      'Vary': 'Origin',
    },
  });
}

function json(obj, status) {
  return new Response(JSON.stringify(obj), {
    status: status || 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

/* ---------------- GitHub App auth (same scheme as the feedback relay) ---------------- */

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
    discussionCategories(first: 20) {
      nodes { id name slug description emojiHTML isAnswerable }
    }
  }
}`;

const Q_LIST = FRAG_AUTHOR + `
query($first: Int!, $after: String, $categoryId: ID) {
  repository(owner: "${REPO_OWNER}", name: "${REPO_NAME}") {
    discussions(first: $first, after: $after, categoryId: $categoryId,
                orderBy: { field: CREATED_AT, direction: DESC }) {
      totalCount
      pageInfo { endCursor hasNextPage }
      nodes {
        number title createdAt url
        author { ...authorFields }
        category { name slug }
        comments { totalCount }
        reactions { totalCount }
      }
    }
  }
}`;

const Q_POST = FRAG_AUTHOR + `
query($number: Int!) {
  repository(owner: "${REPO_OWNER}", name: "${REPO_NAME}") {
    discussion(number: $number) {
      number title bodyHTML createdAt url
      author { ...authorFields }
      category { name slug }
      reactionGroups { content reactors { totalCount } }
      comments(first: ${COMMENT_PAGE}) {
        totalCount
        nodes {
          id bodyHTML createdAt url
          author { ...authorFields }
          reactionGroups { content reactors { totalCount } }
          replies(first: ${REPLY_PAGE}) {
            totalCount
            nodes {
              id bodyHTML createdAt url
              author { ...authorFields }
              reactionGroups { content reactors { totalCount } }
            }
          }
        }
      }
    }
  }
}`;

/* ---------------- shaping ---------------- */

function shapeAuthor(a) {
  if (!a) return { login: 'ghost', avatarUrl: '', url: '' };
  return { login: a.login, avatarUrl: a.avatarUrl, url: a.url };
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
    bodyHTML: c.bodyHTML,
    createdAt: c.createdAt,
    url: c.url,
    author: shapeAuthor(c.author),
    reactions: shapeReactions(c.reactionGroups),
    replies: (c.replies ? c.replies.nodes : []).map(r => ({
      id: r.id,
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

/* ---------------- route handlers ---------------- */

async function getCategories(env) {
  const cached = cacheGet('categories');
  if (cached) return cached;
  const token = await authToken(env);
  if (!token) return { error: 'auth', status: 500 };
  const data = await gql(token, Q_CATEGORIES);
  if (!data) return { error: 'upstream', status: 502 };
  const cats = data.repository.discussionCategories.nodes.map(c => ({
    id: c.id, name: c.name, slug: c.slug, description: c.description,
    emojiHTML: c.emojiHTML, isAnswerable: c.isAnswerable,
  }));
  const out = { categories: cats };
  cachePut('categories', out, TTL_CATEGORIES);
  return out;
}

async function listDiscussions(env, categorySlug, after) {
  const key = 'list|' + (categorySlug || '') + '|' + (after || '');
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
  const data = await gql(token, Q_LIST, { first: PAGE_SIZE, after: after || null, categoryId });
  if (!data) return { error: 'upstream', status: 502 };
  const d = data.repository.discussions;
  const out = {
    totalCount: d.totalCount,
    pageInfo: d.pageInfo,
    discussions: d.nodes.map(n => ({
      number: n.number,
      title: n.title,
      createdAt: n.createdAt,
      url: n.url,
      author: shapeAuthor(n.author),
      category: n.category ? { name: n.category.name, slug: n.category.slug } : null,
      commentCount: n.comments.totalCount,
      reactionCount: n.reactions.totalCount,
    })),
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
    number: d.number,
    title: d.title,
    bodyHTML: d.bodyHTML,
    createdAt: d.createdAt,
    url: d.url,
    author: shapeAuthor(d.author),
    category: d.category ? { name: d.category.name, slug: d.category.slug } : null,
    reactions: shapeReactions(d.reactionGroups),
    commentCount: d.comments.totalCount,
    comments: d.comments.nodes.map(shapeComment),
  };
  cachePut(key, out, TTL_POST);
  return out;
}

/* ---------------- entry ---------------- */

export default {
  async fetch(req, env) {
    if (req.method === 'OPTIONS') return preflight(req);

    const url = new URL(req.url);
    const path = url.pathname.replace(/\/+$/, '') || '/';

    if (req.method !== 'GET') return withCors(req, json({ error: 'not found' }, 404));

    if (path === '/') {
      return new Response('snapmap-plus community proxy: OK\n', { headers: { 'Content-Type': 'text/plain' } });
    }

    let result = null;
    if (path === '/community/categories') {
      result = await getCategories(env);
    } else if (path === '/community/discussions') {
      result = await listDiscussions(env, url.searchParams.get('category'), url.searchParams.get('after'));
    } else {
      const m = path.match(/^\/community\/discussions\/(\d+)$/);
      if (m) result = await getDiscussion(env, parseInt(m[1], 10));
    }

    if (!result) return withCors(req, json({ error: 'not found' }, 404));
    if (result.error) return withCors(req, json({ error: result.error }, result.status || 500));
    return withCors(req, json(result));
  },
};
