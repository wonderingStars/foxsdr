import assert from 'node:assert/strict';
import { test } from 'node:test';
import { runChecks } from '../src/check-run.js';
import { registry, row, scriptedFetch } from './helpers.mjs';

const NOW = Date.UTC(2026, 10, 12, 4, 17);
const page = (host, path = '/p') => `https://${host}${path}`;

function reg() {
  return registry([
    row({ id: 'alpha', page_url: page('a.example'), allowed_hosts: ['a.example'] }),
    row({ id: 'bravo', page_url: page('b.example'), allowed_hosts: ['b.example'], download: { url: page('b.example', '/f.exe'), size_bytes: 500, sha256: 'c'.repeat(64) } }),
    row({ id: 'nothing', status: 'unverified', page_url: null, allowed_hosts: [] }),
  ]);
}
const goodScript = {
  'HEAD https://a.example/p': { status: 200 },
  'HEAD https://b.example/p': { status: 200 },
  'HEAD https://b.example/f.exe': { status: 200, headers: { 'content-length': '500', etag: '"x"' } },
};

test('a good run checks every row that has an address, skips unverified ones, and requests each address once', async () => {
  const fetchImpl = scriptedFetch(goodScript);
  const next = await runChecks({ registry: reg(), state: null, fetchImpl, now: NOW });
  assert.equal(fetchImpl.calls.length, 3);
  assert.deepEqual(Object.keys(next.ids).sort(), ['alpha', 'bravo']);
  assert.equal(next.ids.alpha.status, 'ok');
  assert.equal(next.ids.bravo.last_check, new Date(NOW).toISOString());
  assert.equal(next.last_run, new Date(NOW).toISOString());
  assert.deepEqual(next.last_run_stats, { rows_checked: 2, rows_skipped: 0, requests: 3 });
  assert.equal(next.ids.bravo.changed, null, 'size equals the pinned size');
});

test('two rows that name the same address under the same hosts share one request', async () => {
  const same = registry([
    row({ id: 'one', page_url: page('a.example'), allowed_hosts: ['a.example'] }),
    row({ id: 'two', page_url: page('a.example'), allowed_hosts: ['a.example'] }),
  ]);
  const fetchImpl = scriptedFetch({ 'HEAD https://a.example/p': { status: 200 } });
  const next = await runChecks({ registry: same, state: null, fetchImpl, now: NOW });
  assert.equal(fetchImpl.calls.length, 1);
  assert.equal(next.ids.one.status, 'ok');
  assert.equal(next.ids.two.status, 'ok');
});

test('the strikes carry over between runs through the state; the third failed run breaks the row', async () => {
  const bad = scriptedFetch({ 'HEAD https://a.example/p': { status: 404 }, 'HEAD https://b.example/p': { status: 200 }, 'HEAD https://b.example/f.exe': { status: 200, headers: { 'content-length': '500' } } });
  let state = null;
  const seen = [];
  for (let day = 0; day < 3; day += 1) {
    state = await runChecks({ registry: reg(), state: JSON.parse(JSON.stringify(state)), fetchImpl: bad, now: NOW + day * 86400000 });
    seen.push(state.ids.alpha.status);
  }
  assert.deepEqual(seen, ['ok', 'ok', 'broken']);
  assert.equal(state.ids.bravo.status, 'ok');
});

test('a redirect to a foreign host marks the row moved and the registry ADDRESS IS NEVER CHANGED', async () => {
  const frozen = reg();
  const before = JSON.stringify(frozen);
  Object.freeze(frozen.libraries[0]);
  const fetchImpl = scriptedFetch({
    ...goodScript,
    'HEAD https://a.example/p': { status: 301, headers: { location: 'https://hijacked.example/steal' } },
    '* https://hijacked.example/steal': { status: 200 },
  });
  const next = await runChecks({ registry: frozen, state: null, fetchImpl, now: NOW });
  assert.equal(next.ids.alpha.status, 'moved');
  assert.equal(next.ids.alpha.moved_host, 'hijacked.example');
  assert.equal(JSON.stringify(frozen), before, 'the registry object is untouched');
  assert.ok(fetchImpl.calls.every((c) => !c.url.includes('hijacked')));
});

test('a budget too small for everything: the rest waits, and goes FIRST on the next run', async () => {
  const fetchImpl = scriptedFetch(goodScript);
  const run1 = await runChecks({ registry: reg(), state: null, fetchImpl, now: NOW, budgetLimit: 2, concurrency: 1 });
  assert.equal(run1.last_run_stats.requests, 1, 'bravo needs two requests, only one was left: it was not started');
  assert.equal(run1.last_run_stats.rows_checked, 1);
  assert.equal(run1.last_run_stats.rows_skipped, 1);
  const checked = Object.keys(run1.ids);
  assert.equal(checked.length, 1, 'only the row that was actually checked has a state');
  // second run: the row never checked comes first, so the other one is covered
  const fetch2 = scriptedFetch(goodScript);
  const run2 = await runChecks({ registry: reg(), state: JSON.parse(JSON.stringify(run1)), fetchImpl: fetch2, now: NOW + 86400000, budgetLimit: 2, concurrency: 1 });
  assert.equal(Object.keys(run2.ids).length, 2, 'the second run covered the row the first skipped');
});

test('a row the budget cannot cover is not started, and concludes nothing', async () => {
  const fetchImpl = scriptedFetch(goodScript);
  // bravo needs two requests; with a budget of 1 it is not started at all (no half-spent budget)
  const r = registry([reg().libraries[1]]);
  const next = await runChecks({ registry: r, state: null, fetchImpl, now: NOW, budgetLimit: 1 });
  assert.equal(next.ids.bravo, undefined);
  assert.equal(next.last_run_stats.rows_skipped, 1);
  assert.equal(fetchImpl.calls.length, 0);
});

test('rows removed from the registry are forgotten; a timeout counts as a failed run', async () => {
  const state = { v: 1, last_run: null, ids: { gone: { status: 'broken', fails: 9, history: [] } } };
  const fetchImpl = scriptedFetch({ ...goodScript, 'HEAD https://a.example/p': { hangs: true } });
  const next = await runChecks({ registry: reg(), state, fetchImpl, now: NOW, timeoutMs: 40 });
  assert.equal(next.ids.gone, undefined);
  assert.equal(next.ids.alpha.fails, 1);
  assert.equal(next.ids.alpha.page.o, 'timeout');
});
