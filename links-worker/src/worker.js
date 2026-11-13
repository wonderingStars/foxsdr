// FoxSDR link registry Worker (Cloudflare Worker + one KV namespace + one daily cron trigger).
//
// WHAT IT IS FOR. A plugin or a radio setup page that needs a library the user installs says which
// one and shows a button "Open the download page". The ADDRESS behind that button must not be
// frozen into a released program, and nothing may send a user to a dead link. So the addresses
// live in registry/libraries.json (the only place they are written), this Worker serves it, and a
// daily check notices when an address dies or starts redirecting somewhere it should not.
//
//   GET /v1/libraries.json         the registry, verbatim (ETag, cache headers)
//   GET /v1/libraries.signed.json  the same bytes in the owner's signature envelope (404 until signed)
//   GET /v1/status.json            per id: combined status and last check time
//   GET /v1/go/<id>                302 to the official page, or a "being updated" page
//   GET /health                    liveness, no storage touched
//   GET /admin/links.json          every row that is not ok (Authorization: Bearer <ADMIN_TOKEN>)
//   cron                           the daily checker (src/check-run.js)
//
// WHAT IT DELIBERATELY DOES NOT DO (same stance as telemetry-worker/worker.js):
//   - It reads no query parameter, no cookie, no header of the public routes, and sets no cookie.
//     The registry request carries no install id and none is accepted.
//   - It never reads cf-connecting-ip, request.cf or anything else about the connection, and it
//     writes no logs: nothing here puts a request into the tail buffer.
//   - It NEVER changes an address. The checker's findings are an overlay; the registry is changed
//     by a person, validated, signed and deployed (UPDATING-A-LINK.md).
//   - It holds no private key and verifies no signature: it serves the envelope as a file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import { REGISTRY_TEXT, SIGNED_TEXT } from './registry-bundle.js';
import { validateRegistryText } from './validate.js';
import { runChecks, DEFAULT_BUDGET } from './check-run.js';
import { adminReport, effectiveStatus, publicStatus } from './decide.js';

const ID_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const STATE_KEY = 'state';

const BASE_HEADERS = { 'X-Content-Type-Options': 'nosniff' };

function respond(body, status, extra) {
  return new Response(body, { status, headers: { ...BASE_HEADERS, ...extra } });
}
function json(obj, status, cache) {
  return respond(JSON.stringify(obj), status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': cache });
}
function text(body, status) {
  return respond(body, status, { 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' });
}
const PUBLIC_CORS = { 'Access-Control-Allow-Origin': '*' };

async function sha256(str) {
  return new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(str)));
}
// Both inputs are 32-byte digests, so the loop always runs the same length: no early exit.
function constantTimeEqual(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i += 1) diff |= a[i] ^ b[i];
  return diff === 0;
}
async function etagOf(str) {
  const d = await sha256(str);
  return `"sha256-${[...d.subarray(0, 16)].map((x) => x.toString(16).padStart(2, '0')).join('')}"`;
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function updatingPage(row) {
  const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex"><title>This link is being updated</title></head><body style="font-family:system-ui,sans-serif;max-width:40em;margin:3em auto;padding:0 1em"><h1>This link is being updated</h1><p>The download page for <strong>${escapeHtml(row.name)}</strong>, published by ${escapeHtml(row.publisher)}, may have moved. FoxSDR has stopped sending people to the old address until it has been checked.</p><p>Search for ${escapeHtml(row.publisher)} and ${escapeHtml(row.name)} on your search engine, and download only from the publisher's own site. The written guide in FoxSDR's manual has the file names to look for.</p><p>Please try this link again in a day or two.</p></body></html>`;
  return respond(html, 503, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store', 'Retry-After': '86400', 'Referrer-Policy': 'no-referrer' });
}

// `strict` is for the daily run: a KV ERROR must stop the run (the history is not overwritten by
// an empty state because one read failed). A request that cannot read the state just shows what
// the registry alone says.
async function readState(env, strict = false) {
  if (!env || !env.LINKS_STATE) return null;
  try {
    const s = await env.LINKS_STATE.get(STATE_KEY, 'json');
    return s && typeof s === 'object' && s.v === 1 && s.ids && typeof s.ids === 'object' ? s : null;
  } catch (e) {
    if (strict) throw e;
    return null;
  }
}

// 'ok' | 'denied' | 'unconfigured'. Fails CLOSED: with no secret set nothing is ever allowed.
async function authorise(request, env) {
  const secret = env && env.ADMIN_TOKEN;
  if (typeof secret !== 'string' || secret.length < 16) return 'unconfigured';
  const header = request.headers.get('authorization') || '';
  const m = /^Bearer (.+)$/.exec(header);
  const given = m ? m[1] : '';
  const [a, b] = await Promise.all([sha256(given), sha256(secret)]);
  return constantTimeEqual(a, b) ? 'ok' : 'denied';
}

export function makeWorker({ registryText, signedText = null, fetchImpl = null, now = () => Date.now() }) {
  const checked = validateRegistryText(registryText);
  if (!checked.ok) {
    // A registry that does not validate is never served: fail at start-up (and tools/predeploy.mjs
    // refuses to deploy one), so no request ever sees a half-valid list.
    throw new Error(`links registry is invalid: ${checked.errors.slice(0, 5).join('; ')}`);
  }
  const registry = checked.registry;
  const byId = new Map(registry.libraries.map((r) => [r.id, r]));
  let etag = null;

  return {
    async fetch(request, env) {
      const method = request.method;
      if (method === 'OPTIONS') return respond(null, 204, { Allow: 'GET, HEAD, OPTIONS', ...PUBLIC_CORS });
      if (method !== 'GET' && method !== 'HEAD') return respond('GET only', 405, { Allow: 'GET, HEAD, OPTIONS', 'Cache-Control': 'no-store' });

      // pathname only: the query string is never read.
      const path = new URL(request.url).pathname;

      if (path === '/health') return json({ ok: true, registry_version: registry.registry_version }, 200, 'no-store');

      if (path === '/v1/libraries.json') {
        etag = etag || (await etagOf(registryText));
        const inm = request.headers.get('if-none-match');
        const headers = { ETag: etag, 'Cache-Control': 'public, max-age=3600', 'X-FoxSDR-Schema': String(registry.schema_version), ...PUBLIC_CORS };
        if (inm && inm.split(',').some((t) => t.trim().replace(/^W\//, '') === etag)) return respond(null, 304, headers);
        return respond(registryText, 200, { ...headers, 'Content-Type': 'application/json; charset=utf-8' });
      }

      if (path === '/v1/libraries.signed.json') {
        if (!signedText) return text('the registry has not been signed yet', 404);
        return respond(signedText, 200, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'public, max-age=3600', 'X-FoxSDR-Schema': String(registry.schema_version), ...PUBLIC_CORS });
      }

      if (path === '/v1/status.json') {
        const state = await readState(env);
        return json(publicStatus(registry, state, now()), 200, 'public, max-age=300');
      }

      if (path.startsWith('/v1/go/')) {
        const id = path.slice('/v1/go/'.length);
        const row = ID_RE.test(id) ? byId.get(id) : undefined;
        if (!row) return text('unknown library', 404);
        const state = await readState(env);
        const status = effectiveStatus(row, state?.ids?.[row.id]);
        if (status === 'ok' && row.page_url) {
          return respond(null, 302, { Location: row.page_url, 'Cache-Control': 'no-store', 'Referrer-Policy': 'no-referrer' });
        }
        return updatingPage(row);
      }

      if (path === '/admin/links.json') {
        const verdict = await authorise(request, env);
        if (verdict === 'unconfigured') return text('admin endpoint is not configured', 503);
        if (verdict !== 'ok') return respond('unauthorised', 401, { 'WWW-Authenticate': 'Bearer', 'Cache-Control': 'no-store', 'Content-Type': 'text/plain; charset=utf-8' });
        const state = await readState(env);
        return json(adminReport(registry, state, now()), 200, 'no-store');
      }

      return text('not found', 404);
    },

    // The daily check. One KV read and one KV write; every address is requested at most once.
    async scheduled(controller, env) {
      const state = await readState(env, true);
      const budgetLimit = Number.parseInt(env && env.CHECK_BUDGET, 10);
      const next = await runChecks({
        registry,
        state,
        fetchImpl: fetchImpl || ((...a) => fetch(...a)),
        now: controller && controller.scheduledTime ? controller.scheduledTime : now(),
        budgetLimit: Number.isFinite(budgetLimit) && budgetLimit > 0 ? budgetLimit : DEFAULT_BUDGET,
      });
      await env.LINKS_STATE.put(STATE_KEY, JSON.stringify(next));
    },
  };
}

export default makeWorker({ registryText: REGISTRY_TEXT, signedText: SIGNED_TEXT });
