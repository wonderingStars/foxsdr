import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { test } from 'node:test';
import defaultWorker, { makeWorker } from '../src/worker.js';
import { fakeKv, registry, registryText, row, scriptedFetch, SRC_DIR } from './helpers.mjs';

const TOKEN = 'a-long-test-token-1234567890';
const NOW = Date.UTC(2026, 10, 12, 12, 0);
const A = 'https://a.example/page|';
const B = 'https://b.example/page|';

const REG = registry([
  row({ id: 'alpha', name: 'Alpha <b>lib</b>', publisher: 'Pub & Co', page_url: 'https://a.example/page', allowed_hosts: ['a.example'] }),
  row({ id: 'bravo', page_url: 'https://b.example/page', allowed_hosts: ['b.example'] }),
  row({ id: 'nothing', status: 'unverified', page_url: null, allowed_hosts: [] }),
]);
const TEXT = registryText(REG);

function make(over = {}) {
  return makeWorker({ registryText: TEXT, now: () => NOW, ...over });
}
const get = (w, path, env = {}, init = {}) => w.fetch(new Request(`https://links.test${path}`, init), env);

test('GET /health answers without touching storage', async () => {
  const kv = fakeKv();
  const res = await get(make(), '/health', { LINKS_STATE: kv });
  assert.equal(res.status, 200);
  assert.deepEqual(await res.json(), { ok: true, registry_version: 3 });
  assert.equal(kv.gets, 0);
});

test('GET /v1/libraries.json serves the registry BYTE FOR BYTE with ETag and cache headers; 304 on a match', async () => {
  const w = make();
  const res = await get(w, '/v1/libraries.json');
  assert.equal(res.status, 200);
  assert.equal(await res.text(), TEXT);
  assert.match(res.headers.get('etag'), /^"sha256-[0-9a-f]{32}"$/);
  assert.equal(res.headers.get('cache-control'), 'public, max-age=3600');
  assert.equal(res.headers.get('x-foxsdr-schema'), '1');
  assert.match(res.headers.get('content-type'), /application\/json/);
  const again = await get(w, '/v1/libraries.json', {}, { headers: { 'If-None-Match': res.headers.get('etag') } });
  assert.equal(again.status, 304);
  const stale = await get(w, '/v1/libraries.json', {}, { headers: { 'If-None-Match': '"sha256-0000"' } });
  assert.equal(stale.status, 200);
});

test('GET /v1/libraries.signed.json: 404 until signed, then the envelope verbatim', async () => {
  assert.equal((await get(make(), '/v1/libraries.signed.json')).status, 404);
  const env = '{"format":"foxsdr-link-registry-sig"}';
  const res = await get(make({ signedText: env }), '/v1/libraries.signed.json');
  assert.equal(res.status, 200);
  assert.equal(await res.text(), env);
});

test('the public routes set no cookie, read no query string and carry no identifier', async () => {
  const w = make();
  for (const p of ['/health', '/v1/libraries.json', '/v1/status.json', '/v1/go/alpha', '/v1/go/zzz', '/nope']) {
    const res = await get(w, p + '?id=abc&install=123', { LINKS_STATE: fakeKv() }, { headers: { Cookie: 'sid=1' } });
    assert.equal(res.headers.get('set-cookie'), null, p);
  }
  const a = await (await get(w, '/v1/libraries.json?x=1')).text();
  const b = await (await get(w, '/v1/libraries.json')).text();
  assert.equal(a, b);
  const goA = await get(w, '/v1/go/alpha?utm=evil');
  assert.equal(goA.headers.get('location'), 'https://a.example/page');
});

test('GET /v1/go/<id>: 302 to the official page for an ok row', async () => {
  const res = await get(make(), '/v1/go/alpha', { LINKS_STATE: fakeKv() });
  assert.equal(res.status, 302);
  assert.equal(res.headers.get('location'), 'https://a.example/page');
  assert.equal(res.headers.get('cache-control'), 'no-store');
});

test('GET /v1/go/<id>: a row the checker found moved or broken, or the registry marks so, gets the "being updated" page, escaped', async () => {
  for (const live of ['moved', 'broken']) {
    const kv = fakeKv({ v: 1, last_run: new Date(NOW).toISOString(), ids: { alpha: { addr: A, status: live, fails: 3, history: [] } } });
    const res = await get(make(), '/v1/go/alpha', { LINKS_STATE: kv });
    assert.equal(res.status, 503, live);
    assert.equal(res.headers.get('location'), null);
    const html = await res.text();
    assert.match(html, /This link is being updated/);
    assert.match(html, /Alpha &lt;b&gt;lib&lt;\/b&gt;/);
    assert.match(html, /Pub &amp; Co/);
    assert.doesNotMatch(html, /<b>lib/);
  }
  const marked = makeWorker({ registryText: registryText(registry([row({ id: 'alpha', status: 'moved' })])), now: () => NOW });
  assert.equal((await get(marked, '/v1/go/alpha')).status, 503);
  assert.equal((await get(make(), '/v1/go/nothing')).status, 503, 'an unverified row has no address to send anyone to');
});

test('after a link is updated and deployed, /v1/go works again at once, without waiting for the next daily run', async () => {
  const kv = fakeKv({ v: 1, last_run: new Date(NOW).toISOString(), ids: { alpha: { addr: 'https://a.example/OLD-page|', status: 'broken', fails: 3, history: [] } } });
  const res = await get(make(), '/v1/go/alpha', { LINKS_STATE: kv });
  assert.equal(res.status, 302);
});

test('a failing KV read does not break /v1/go: it falls back to what the registry says', async () => {
  const kv = fakeKv();
  kv.failGet = true;
  assert.equal((await get(make(), '/v1/go/alpha', { LINKS_STATE: kv })).status, 302);
});

test('unknown ids, odd paths and other routes are 404', async () => {
  const w = make();
  for (const p of ['/v1/go/unknown', '/v1/go/', '/v1/go', '/v1/go/ALPHA', '/v1/go/alpha/', '/v1/go/../admin/links.json', '/v1/go/alpha%2F..', '/v1/go/alpha%00', '/', '/v1', '/v1/status', '/admin', '/admin/links', '/favicon.ico']) {
    const res = await get(w, p, { ADMIN_TOKEN: TOKEN });
    assert.equal(res.status, 404, p);
  }
});

test('only GET and HEAD (and OPTIONS) are answered', async () => {
  const w = make();
  for (const m of ['POST', 'PUT', 'DELETE', 'PATCH']) {
    const res = await w.fetch(new Request('https://links.test/v1/libraries.json', { method: m, body: m === 'DELETE' ? undefined : 'x' }), {});
    assert.equal(res.status, 405, m);
  }
  assert.equal((await w.fetch(new Request('https://links.test/v1/libraries.json', { method: 'OPTIONS' }), {})).status, 204);
});

test('GET /v1/status.json: per id the combined status and last check, nothing else', async () => {
  const kv = fakeKv({ v: 1, last_run: new Date(NOW - 3600e3).toISOString(), ids: { alpha: { addr: A, status: 'broken', last_check: '2026-11-12T04:17:00.000Z', fails: 3, moved_host: 'x.example', history: [{ t: 'x' }] } } });
  const res = await get(make(), '/v1/status.json', { LINKS_STATE: kv });
  const body = await res.json();
  assert.deepEqual(body.libraries.alpha, { status: 'broken', checked: '2026-11-12T04:17:00.000Z' });
  assert.deepEqual(body.libraries.bravo, { status: 'ok', checked: null });
  assert.deepEqual(body.libraries.nothing, { status: 'unverified', checked: null });
  assert.equal(body.stale, false);
  assert.ok(!JSON.stringify(body).includes('x.example'), 'nothing but status and time per id');
  const none = await (await get(make(), '/v1/status.json', {})).json();
  assert.equal(none.stale, true, 'no state at all: the checker has never run');
});

test('ADMIN: refused without the secret, with a wrong one, with the token in the query, and when unconfigured', async () => {
  const w = make();
  const env = { ADMIN_TOKEN: TOKEN, LINKS_STATE: fakeKv() };
  assert.equal((await get(w, '/admin/links.json', env)).status, 401);
  assert.equal((await get(w, '/admin/links.json', env, { headers: { Authorization: 'Bearer wrong-wrong-wrong-wrong' } })).status, 401);
  assert.equal((await get(w, '/admin/links.json', env, { headers: { Authorization: `Basic ${TOKEN}` } })).status, 401);
  assert.equal((await get(w, `/admin/links.json?token=${TOKEN}`, env)).status, 401);
  assert.equal((await get(w, '/admin/links.json', env, { headers: { Authorization: `Bearer ${TOKEN}x` } })).status, 401);
  assert.equal((await get(w, '/admin/links.json', env, { headers: { 'X-Admin-Token': TOKEN } })).status, 401);
  const refused = await get(w, '/admin/links.json', env);
  assert.equal(refused.headers.get('www-authenticate'), 'Bearer');
  assert.equal(refused.headers.get('cache-control'), 'no-store');
  assert.equal(await refused.text(), 'unauthorised', 'the refusal says nothing about why');
  // fail closed: no secret configured, or one too short to be a secret
  for (const e of [{}, { ADMIN_TOKEN: '' }, { ADMIN_TOKEN: 'short' }]) {
    const res = await get(w, '/admin/links.json', e, { headers: { Authorization: `Bearer ${e.ADMIN_TOKEN ?? ''}` } });
    assert.equal(res.status, 503, JSON.stringify(e));
  }
});

test('ADMIN: with the secret it lists every row that is not ok, with what was seen', async () => {
  const kv = fakeKv({
    v: 1,
    last_run: new Date(NOW - 3600e3).toISOString(),
    last_run_stats: { rows_checked: 2, rows_skipped: 0, requests: 2 },
    ids: {
      alpha: { addr: A, status: 'moved', fails: 0, last_check: '2026-11-12T04:17:00.000Z', last_ok: '2026-11-11T04:17:00.000Z', moved_host: 'hijacked.example', page: { o: 'foreign-host', http: 302, host: 'hijacked.example' }, download: null, changed: null, changed_seen: null, history: [{ t: 'a' }, { t: 'b' }] },
      bravo: { addr: B, status: 'ok', fails: 0, last_check: '2026-11-12T04:17:00.000Z', changed: null, changed_seen: null, history: [] },
    },
  });
  const res = await get(make(), '/admin/links.json', { ADMIN_TOKEN: TOKEN, LINKS_STATE: kv }, { headers: { Authorization: `Bearer ${TOKEN}` } });
  assert.equal(res.status, 200);
  assert.equal(res.headers.get('cache-control'), 'no-store');
  const body = await res.json();
  const ids = body.rows.map((r) => r.id).sort();
  assert.deepEqual(ids, ['alpha', 'nothing'], 'bravo is fine and is not listed; the unverified row is');
  const alpha = body.rows.find((r) => r.id === 'alpha');
  assert.equal(alpha.status, 'moved');
  assert.equal(alpha.moved_to_host, 'hijacked.example');
  assert.equal(alpha.last_seen.page.o, 'foreign-host');
  assert.equal(body.rows_ok, 1);
  assert.equal(body.checker_stale, false);
});

test('ADMIN: a checker that has not run for over 36 hours says so, and a never-checked row is listed', async () => {
  const kv = fakeKv({ v: 1, last_run: new Date(NOW - 40 * 3600e3).toISOString(), ids: {} });
  const body = await (await get(make(), '/admin/links.json', { ADMIN_TOKEN: TOKEN, LINKS_STATE: kv }, { headers: { Authorization: `Bearer ${TOKEN}` } })).json();
  assert.equal(body.checker_stale, true);
  assert.ok(body.rows.find((r) => r.id === 'alpha').never_checked);
});

test('the scheduled run reads once, requests each address once, writes the state once', async () => {
  const kv = fakeKv();
  const fetchImpl = scriptedFetch({ 'HEAD https://a.example/page': { status: 200 }, 'HEAD https://b.example/page': { status: 404 } });
  const w = make({ fetchImpl });
  await w.scheduled({ scheduledTime: NOW }, { LINKS_STATE: kv, CHECK_BUDGET: '10' });
  assert.equal(kv.gets, 1);
  assert.equal(kv.puts, 1);
  assert.equal(fetchImpl.calls.length, 2);
  const state = JSON.parse(kv.store.get('state'));
  assert.equal(state.ids.alpha.status, 'ok');
  assert.equal(state.ids.bravo.fails, 1);
  assert.equal(state.last_run, new Date(NOW).toISOString());
});

test('a KV error during the daily run stops it: the history is never overwritten by an empty state', async () => {
  const kv = fakeKv({ v: 1, last_run: null, ids: { alpha: { status: 'ok', fails: 2, history: [{ t: 1 }] } } });
  kv.failGet = true;
  const fetchImpl = scriptedFetch({});
  await assert.rejects(() => make({ fetchImpl }).scheduled({ scheduledTime: NOW }, { LINKS_STATE: kv }), /KV unavailable/);
  assert.equal(kv.puts, 0);
  assert.equal(fetchImpl.calls.length, 0);
});

test('an invalid registry is never served: the Worker refuses to start', () => {
  assert.throws(() => makeWorker({ registryText: '{"nope":1}' }), /links registry is invalid/);
  const bad = registry([row({ page_url: 'http://demo.example/' })]);
  assert.throws(() => makeWorker({ registryText: registryText(bad) }), /links registry is invalid/);
});

test('the default export is the real bundled registry and answers', async () => {
  const res = await get(defaultWorker, '/v1/libraries.json');
  assert.equal(res.status, 200);
  assert.equal(JSON.parse(await res.text()).schema_version, 1);
  assert.equal((await get(defaultWorker, '/v1/go/zadig', {})).status, 302);
});

test('source scan: no logging, no connection data, no cookies, no secrets in src/', () => {
  const files = readdirSync(SRC_DIR).filter((f) => f.endsWith('.js'));
  assert.ok(files.includes('worker.js'));
  for (const f of files) {
    const code = readFileSync(SRC_DIR + f, 'utf8').split('\n').filter((l) => !l.trim().startsWith('//')).join('\n');
    assert.doesNotMatch(code, /console\./, f);
    assert.doesNotMatch(code, /cf-connecting-ip|request\.cf\b|x-forwarded-for/i, f);
    assert.doesNotMatch(code, /set-cookie|document\.cookie/i, f);
    assert.doesNotMatch(code, /BEGIN (RSA |EC |OPENSSH |)PRIVATE KEY|seed/i, f);
    assert.doesNotMatch(code, /searchParams|URLSearchParams|url\.search\b/, f, 'no query parameter is read');
  }
});
