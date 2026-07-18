/* feedback/worker.js -- the feedback relay: a Cloudflare Worker that turns the in-app "Send feedback"
 * dialog's anonymous POST into a labeled issue on this project's GitHub tracker.
 *
 * Why it exists: GitHub has no anonymous write path (every API write needs a credential), and a
 * credential must never ship inside a public binary. So the app POSTs here, and this relay files the
 * issue with a credential only it holds.
 *
 * Identity: preferably a GITHUB APP owned by the org (secrets APP_ID + APP_PRIVATE_KEY) -- issues then
 * arrive from "<app-name>[bot]", not from any personal account, and tokens are minted fresh per
 * request (the app's private key never expires -> no rotation chore). Falls back to a plain
 * fine-grained PAT (secret GITHUB_TOKEN) when the app secrets are absent.
 *
 * Flow per report:
 *   validate (category / lengths / size) -> honeypot check -> compute a dedup signature ->
 *   search open issues for that signature:
 *     hit  -> append a comment to the existing issue (one issue with N confirmations, not N issues)
 *     miss -> create a new issue, labeled: category label + release channel + user-report
 *
 * Ops notes (see also feedback/README.md):
 *   - secrets: `wrangler secret put APP_ID` + `APP_PRIVATE_KEY` (PKCS#8 PEM) -- or GITHUB_TOKEN as
 *     the PAT fallback (expires -> annual rotation; the app path has no such chore).
 *   - stateless by design: no KV, no queues; GitHub Issues is the only store.
 *   - abuse posture: honeypot + size caps here; if real spam ever shows up, add a Cloudflare
 *     rate-limiting rule on the dashboard (no code change needed).
 */

const REPO = 'snaphak/open-snaphak';
const API = 'https://api.github.com';

const CATEGORIES = {
  bug:     { label: 'bug',           tag: 'Bug' },
  feature: { label: 'enhancement',   tag: 'Feature' },
  docs:    { label: 'documentation', tag: 'Docs' },
  other:   { label: 'question',      tag: 'Other' },
  /* crash reports (the in-app crash dialog): the client auto-titles them with the crash location
   * ("Crash: MODULE+0xRVA (0xCODE)"), so the signature dedup groups identical crash sites onto one
   * issue. They may carry a `logs` field -- anonymized log tails, delivered as a follow-up COMMENT
   * (collapsed) so the issue body stays readable; each dedup occurrence brings its own logs comment. */
  crash:   { label: 'crash',         tag: 'Crash' },
};

/* logs cap: well under GitHub's 65536-char comment limit once wrapped, and under the request-size
 * guard. The client already tails each log before sending. */
const LOGS_CAP = 50000;

/* Wrap attached logs for a comment: collapsed, fenced with FOUR backticks so log text containing
 * a ``` sequence cannot break out of the fence. */
function logsBlock(logs) {
  return '<details><summary>Attached logs (anonymized)</summary>\n\n````text\n' + logs + '\n````\n</details>';
}

function json(obj, status) {
  return new Response(JSON.stringify(obj), {
    status: status || 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

async function gh(bearer, path, opts) {
  const o = opts || {};
  const res = await fetch(API + path, {
    method: o.method || 'GET',
    headers: {
      'Authorization': 'Bearer ' + bearer,
      'Accept': 'application/vnd.github+json',
      'X-GitHub-Api-Version': '2022-11-28',
      'User-Agent': 'snaphak-feedback-relay',
      ...(o.body ? { 'Content-Type': 'application/json' } : {}),
    },
    body: o.body ? JSON.stringify(o.body) : undefined,
  });
  if (!res.ok) return null;
  return res.json();
}

/* ---- GitHub App auth: a short-lived RS256 JWT (signed with the app's private key) is exchanged for
 * a ~1h installation token, cached across requests in this isolate. The app's key is long-lived but
 * never leaves the Worker secret store; the tokens GitHub actually sees expire on their own. ---- */
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
    'X-GitHub-Api-Version': '2022-11-28', 'User-Agent': 'snaphak-feedback-relay',
  };
  const instRes = await fetch(API + '/repos/' + REPO + '/installation', { headers: hdrs });
  if (!instRes.ok) { console.log('app auth: installation lookup ->', instRes.status, (await instRes.text()).slice(0, 200)); return null; }
  const inst = await instRes.json();
  const tokRes = await fetch(API + '/app/installations/' + inst.id + '/access_tokens', { method: 'POST', headers: hdrs });
  if (!tokRes.ok) { console.log('app auth: token exchange ->', tokRes.status, (await tokRes.text()).slice(0, 200)); return null; }
  const tok = await tokRes.json();
  tokenCache = { token: tok.token, exp: now + 3300 };   // installation tokens live ~1h
  return tok.token;
}

/* dedup signature: category + normalized title -> first 16 hex of SHA-256. Embedded in the issue body
 * as an HTML comment; exact-match only (fuzzy "same bug, different words" stays a human call). */
async function sigHash(category, title) {
  const norm = category + '|' + title.toLowerCase().trim().replace(/\s+/g, ' ');
  const digest = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(norm));
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 16);
}

/* release channel from the reported version: a "-" (e.g. 0.2.0-beta.1) means a pre-release build;
 * a plain x.y.z means stable; anything non-semver (a dev build) gets no channel label. */
function channelOf(version) {
  if (!/^\d+\.\d+\.\d+/.test(version)) return null;
  return version.includes('-') ? 'beta' : 'stable';
}

function issueBody(details, version, channel, contact, sig) {
  const meta = [
    '',
    '---',
    '- Version: ' + version + (channel ? ' (' + channel + ')' : ''),
  ];
  if (contact) meta.push('- Contact: ' + contact);
  meta.push('', '<!-- report-sig:' + sig + ' -->');
  meta.push('<sub>Filed automatically from the in-app feedback dialog.</sub>');
  return details + '\n' + meta.join('\n');
}

function commentBody(details, version, channel, contact, logs) {
  const lines = ['Another report of this, on version ' + version + (channel ? ' (' + channel + ')' : '') + ':', '', details];
  if (contact) lines.push('', '- Contact: ' + contact);
  if (logs) lines.push('', logsBlock(logs));
  lines.push('', '<sub>Added automatically from the in-app feedback dialog (matching report signature).</sub>');
  return lines.join('\n');
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (req.method === 'GET' && url.pathname === '/') {
      return new Response('snaphak feedback relay: OK\n', { headers: { 'Content-Type': 'text/plain' } });
    }
    if (req.method !== 'POST' || url.pathname !== '/report') return json({ ok: false, error: 'not found' }, 404);

    const raw = await req.text();
    if (raw.length > 65536) return json({ ok: false, error: 'too large' }, 413);
    let body;
    try { body = JSON.parse(raw); } catch { return json({ ok: false, error: 'bad json' }, 400); }

    /* honeypot: a hidden field humans never see. A bot that filled it gets a convincing fake success
     * (nothing filed) -- a rejection would just teach it which field to skip. */
    if (body.website) return json({ ok: true, mode: 'created', number: 0 });

    const cat = CATEGORIES[body.category];
    const title = String(body.title || '').trim();
    const details = String(body.body || '').trim();
    const contact = String(body.contact || '').trim().slice(0, 200);
    const version = String(body.version || 'unknown').trim().slice(0, 40) || 'unknown';
    /* optional log attachment (crash reports only -- ignored for the other categories). */
    const logs = body.category === 'crash' ? String(body.logs || '').slice(0, LOGS_CAP).trim() : '';
    if (!cat) return json({ ok: false, error: 'bad category' }, 400);
    if (title.length < 3 || title.length > 120) return json({ ok: false, error: 'bad title' }, 400);
    if (details.length < 10 || details.length > 8000) return json({ ok: false, error: 'bad details' }, 400);

    const channel = channelOf(version);
    const sig = await sigHash(body.category, title);
    const token = await authToken(env);
    if (!token) return json({ ok: false, error: 'relay auth' }, 500);

    /* dedup (best-effort: a lookup failure falls through to plain create). Deliberately the LIST
     * endpoint + a body scan, NOT the search API: search is eventually-consistent (seconds-to-minutes
     * of index lag), so two reports of the same thing in quick succession -- or one user's double-send
     * -- would slip past it and double-file (observed live, 2026-07-18). Listing open user-report
     * issues avoids the index; oldest-first, so a match is always the ORIGINAL report and stopping at
     * the first hit is correct. Up to 3 pages (300 open reports) is far beyond a realistic tracker.
     * Still best-effort: a sub-second race (two reports arriving together) can double-file -- the
     * in-app dialog's one-in-flight guard covers the common double-click case. Closed matches are NOT
     * resurrected -- closed means resolved or rejected; a fresh report opens a fresh issue. */
    let match = null;
    const marker = 'report-sig:' + sig;
    for (let page = 1; page <= 3 && !match; page++) {
      const batch = await gh(token, '/repos/' + REPO + '/issues?state=open&labels=user-report&sort=created&direction=asc&per_page=100&page=' + page);
      if (!batch || !batch.length) break;
      match = batch.find(i => !i.pull_request && (i.body || '').includes(marker)) || null;
      if (batch.length < 100) break;
    }
    if (match) {
      const n = match.number;
      const c = await gh(token, '/repos/' + REPO + '/issues/' + n + '/comments', {
        method: 'POST',
        body: { body: commentBody(details, version, channel, contact, logs) },
      });
      if (c) return json({ ok: true, mode: 'appended', number: n });
      /* comment failed -> fall through and file a fresh issue rather than dropping the report */
    }

    const labels = [cat.label, 'user-report'];
    if (channel) labels.push(channel);
    const issue = await gh(token, '/repos/' + REPO + '/issues', {
      method: 'POST',
      body: {
        title: '[' + cat.tag + '] ' + title,
        body: issueBody(details, version, channel, contact, sig),
        labels,
      },
    });
    if (!issue || !issue.number) return json({ ok: false, error: 'upstream' }, 502);
    /* logs ride a follow-up comment, not the issue body: the body stays a readable crash summary, and
     * every occurrence (create OR dedup-append) then carries its own logs the same way. Best-effort --
     * a failed comment never fails the report (the issue exists; the response stays ok). */
    if (logs) {
      await gh(token, '/repos/' + REPO + '/issues/' + issue.number + '/comments', {
        method: 'POST',
        body: { body: 'Logs from the reporting session:\n\n' + logsBlock(logs) },
      });
    }
    return json({ ok: true, mode: 'created', number: issue.number });
  },
};
