import assert from 'node:assert/strict';
import { test } from 'node:test';
import * as decide from '../src/decide.js';
import * as probe from '../src/probe.js';
import { loadMutant, row, scriptedFetch } from './helpers.mjs';

const T = (n) => new Date(Date.UTC(2026, 10, 12, 4, n)).toISOString();
const OK = { outcome: 'ok', http: 200, host: 'demo.example', size: 100, etag: '"e1"', requests: 1 };
const FAIL = { outcome: 'http-error', http: 404, host: 'demo.example', size: null, etag: null, requests: 1 };
const FOREIGN = { outcome: 'foreign-host', http: 302, host: 'evil.example', size: null, etag: null, requests: 1, reason: 'redirected to a host not in allowed_hosts' };

// The three-strikes rule, stated once so it can be run on the real module AND on a mutant.
export function threeStrikesHolds(d) {
  let st;
  const r = row();
  st = d.applyRun(st, r, FAIL, null, T(1));
  assert.equal(st.status, 'ok', 'one failed run is not broken');
  assert.equal(st.fails, 1);
  st = d.applyRun(st, r, FAIL, null, T(2));
  assert.equal(st.status, 'ok', 'two failed runs are not broken');
  st = d.applyRun(st, r, FAIL, null, T(3));
  assert.equal(st.status, 'broken', 'three failed runs in a row are broken');
  assert.equal(st.fails, 3);
}

// The moved-host rule, stated once: the foreign host is judged BEFORE it is requested.
export async function movedHostRuleHolds(p) {
  const fetchImpl = scriptedFetch({
    'HEAD https://demo.example/old': { status: 302, headers: { location: 'https://evil.example/x' } },
    '* https://evil.example/x': { status: 200 },
  });
  const obs = await p.probeAddress('https://demo.example/old', ['demo.example'], { fetchImpl, budget: p.makeBudget(5), timeoutMs: 1000 });
  assert.equal(obs.outcome, 'foreign-host');
  assert.equal(fetchImpl.calls.length, 1, 'the foreign host must never be requested');
}

test('three failed runs in a row make a row broken; two do not', () => threeStrikesHolds(decide));

test('BREAK-IT three strikes: with the limit removed the rule fails', async () => {
  const mutant = await loadMutant('decide.js', 'if (st.fails >= FAIL_LIMIT) st.status = \'broken\';', 'if (st.fails >= 1000) st.status = \'broken\';');
  assert.throws(() => threeStrikesHolds(mutant), /three failed runs in a row are broken/);
});

test('BREAK-IT three strikes: a limit of two (too eager) fails the "two are not broken" check', async () => {
  const mutant = await loadMutant('decide.js', 'export const FAIL_LIMIT = 3;', 'export const FAIL_LIMIT = 2;');
  assert.throws(() => threeStrikesHolds(mutant), /two failed runs are not broken/);
});

test('the moved-host rule holds on the real probe', () => movedHostRuleHolds(probe));

test('BREAK-IT moved host: with the allowed-host test removed the foreign host IS requested', async () => {
  const mutant = await loadMutant('probe.js', "((!allowHttp && current.port !== '') || !allowedHosts.includes(hostKey))", '(false)');
  await assert.rejects(() => movedHostRuleHolds(mutant), /foreign-host|never be requested/);
});

test('a clean run resets the counter and recovers broken and moved', () => {
  const r = row();
  let st;
  for (let i = 1; i <= 3; i += 1) st = decide.applyRun(st, r, FAIL, null, T(i));
  assert.equal(st.status, 'broken');
  st = decide.applyRun(st, r, OK, null, T(4));
  assert.equal(st.status, 'ok');
  assert.equal(st.fails, 0);
  assert.equal(st.last_ok, T(4));
  st = decide.applyRun(st, r, FOREIGN, null, T(5));
  assert.equal(st.status, 'moved');
  assert.equal(st.moved_host, 'evil.example');
  st = decide.applyRun(st, r, OK, null, T(6));
  assert.equal(st.status, 'ok');
  assert.equal(st.moved_host, null);
});

test('a failure in between resets nothing: the strikes must be consecutive', () => {
  const r = row();
  let st;
  st = decide.applyRun(st, r, FAIL, null, T(1));
  st = decide.applyRun(st, r, FAIL, null, T(2));
  st = decide.applyRun(st, r, OK, null, T(3));
  st = decide.applyRun(st, r, FAIL, null, T(4));
  assert.equal(st.status, 'ok');
  assert.equal(st.fails, 1);
});

test('a moved row stays moved through failures and does not count them as a broken link', () => {
  const r = row();
  let st = decide.applyRun(undefined, r, FOREIGN, null, T(1));
  assert.equal(st.status, 'moved');
  assert.equal(st.fails, 0);
  st = decide.applyRun(st, r, FAIL, null, T(2));
  assert.equal(st.status, 'moved');
});

test('a foreign host on the DOWNLOAD address moves the row even when the page is fine', () => {
  const r = row({ download: { url: 'https://demo.example/f.exe', size_bytes: null, sha256: null } });
  const st = decide.applyRun(undefined, r, OK, FOREIGN, T(1));
  assert.equal(st.status, 'moved');
});

test('skipped (out of budget) and inconclusive (429) runs change no status', () => {
  const r = row();
  const SKIP = { outcome: 'skipped-budget', http: null, host: null, size: null, etag: null, requests: 0 };
  const LIMITED = { ...OK, outcome: 'rate-limited', http: 429 };
  assert.equal(decide.summariseRun([SKIP]), 'skipped');
  const prev = decide.applyRun(undefined, r, FAIL, null, T(1));
  assert.equal(decide.applyRun(prev, r, SKIP, null, T(2)), prev, 'skipped returns the state untouched');
  const after = decide.applyRun(prev, r, LIMITED, null, T(3));
  assert.equal(after.fails, 1, '429 neither adds a strike nor clears one');
  assert.equal(after.last_check, T(3));
});

test('summariseRun: precedence moved > failed > skipped > inconclusive > ok', () => {
  const s = (...o) => decide.summariseRun(o);
  const SKIP = { outcome: 'skipped-budget' };
  assert.equal(s(OK), 'ok');
  assert.equal(s(OK, FAIL), 'failed');
  assert.equal(s(FAIL, FOREIGN), 'moved');
  assert.equal(s(OK, SKIP), 'skipped');
  assert.equal(s(FAIL, SKIP), 'failed');
  assert.equal(s({ outcome: 'timeout' }), 'failed');
  assert.equal(s({ outcome: 'network-error' }), 'failed');
});

test('history keeps the last 30 results only', () => {
  const r = row();
  let st;
  for (let i = 0; i < 45; i += 1) st = decide.applyRun(st, r, OK, null, T(i));
  assert.equal(st.history.length, decide.HISTORY_MAX);
  assert.equal(decide.HISTORY_MAX, 30);
  assert.equal(st.history[0].t, T(15));
  assert.equal(st.history.at(-1).t, T(44));
});

test('changed: a different size or ETag on a download with a PINNED checksum is flagged; without a pin it is not', () => {
  const pinned = row({ download: { url: 'https://demo.example/f.exe', size_bytes: 100, sha256: 'a'.repeat(64) } });
  const unpinned = row({ download: { url: 'https://demo.example/f.exe', size_bytes: 100, sha256: null } });
  const same = { ...OK };
  const bigger = { ...OK, size: 250 };
  assert.equal(decide.applyRun(undefined, pinned, OK, same, T(1)).changed, null);
  const flagged = decide.applyRun(undefined, pinned, OK, bigger, T(1));
  assert.equal(flagged.changed.size, 250);
  assert.equal(flagged.changed.expected_size, 100);
  assert.equal(decide.applyRun(undefined, unpinned, OK, bigger, T(1)).changed, null);
  // ETag changing between runs, size equal to the registry's
  let st = decide.applyRun(undefined, pinned, OK, { ...OK, etag: '"v1"' }, T(1));
  assert.equal(st.changed, null);
  st = decide.applyRun(st, pinned, OK, { ...OK, etag: '"v2"' }, T(2));
  assert.equal(st.changed.etag, '"v2"');
  st = decide.applyRun(st, pinned, OK, { ...OK, etag: '"v2"' }, T(3));
  assert.equal(st.changed, null, 'the flag is per change; changed_seen remembers it');
  assert.equal(st.changed_seen, T(2));
});

test('effective status is the WORSE of registry and live; live never makes a row better', () => {
  const r = (status) => row({ status });
  const live = (status) => ({ status, addr: decide.addrKey(row()) });
  assert.equal(decide.effectiveStatus(r('ok'), undefined), 'ok');
  assert.equal(decide.effectiveStatus(r('ok'), live('broken')), 'broken');
  assert.equal(decide.effectiveStatus(r('ok'), live('moved')), 'moved');
  assert.equal(decide.effectiveStatus(r('moved'), live('ok')), 'moved');
  assert.equal(decide.effectiveStatus(r('broken'), live('moved')), 'broken');
  assert.equal(decide.effectiveStatus(r('unverified'), live('ok')), 'unverified');
});

test('a NEW address starts clean: what was seen at the old address no longer applies', () => {
  const old = row();
  let st;
  for (let i = 1; i <= 3; i += 1) st = decide.applyRun(st, old, FAIL, null, T(i));
  assert.equal(decide.effectiveStatus(old, st), 'broken');
  const fixed = row({ page_url: 'https://demo.example/new-page' });
  assert.equal(decide.effectiveStatus(fixed, st), 'ok', 'the overlay is about the old address');
  assert.equal(decide.liveFor(fixed, st), null);
  const again = decide.applyRun(st, fixed, FAIL, null, T(4));
  assert.equal(again.fails, 1, 'the strikes start again at the new address');
  assert.equal(again.history.length, 1);
});

test('the checker is stale after 36 hours, and with no state at all', () => {
  const now = Date.UTC(2026, 10, 13, 12, 0);
  assert.equal(decide.isStale(null, now), true);
  assert.equal(decide.isStale({ last_run: new Date(now - 35 * 3600e3).toISOString() }, now), false);
  assert.equal(decide.isStale({ last_run: new Date(now - 37 * 3600e3).toISOString() }, now), true);
});
