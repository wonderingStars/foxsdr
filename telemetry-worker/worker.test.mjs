// Tests for the usage Worker (worker.js) and its reader (usage.ps1).
//
//   node --test telemetry-worker/worker.test.mjs      (ctest: telemetry_worker)
//
// No wrangler, no account and no network. The Worker is imported and its
// fetch() handler is called with a fake `env` whose two datasets record what
// would have been written to Analytics Engine; the reader is run for real
// against a stand-in for Cloudflare's SQL API on loopback, which records the
// queries the script sends and answers them with canned rows.
//
// Needs Node 22.7 or later (it imports worker.js, an ES module with no
// package.json beside it, and relies on module-syntax detection).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import http from 'node:http';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import worker from './worker.js';

const here = fileURLToPath(new URL('.', import.meta.url));

// The Worker's own vocabulary of failure events, read out of worker.js between its
// two markers (strict JSON, so this and tests/test_health_events.cpp can both read
// it): an array of { name, qualifiers: [[word, ...], ...] }.
function vocabularyFromWorker() {
  const text = readFileSync(`${here}worker.js`, 'utf8');
  const begin = text.indexOf('// BEGIN HEALTH VOCABULARY');
  const end = text.indexOf('// END HEALTH VOCABULARY');
  assert.ok(begin >= 0 && end > begin, 'the vocabulary markers are in worker.js');
  return JSON.parse(text.slice(text.indexOf('[', begin), text.lastIndexOf(']', end) + 1));
}
const ID = 'a'.repeat(32);

// An old client's record: exactly what 0.99.61 and earlier send.
function oldRecord() {
  return {
    id: ID, v: '0.99.61', os: 'Windows 10.0.22631', arch: 'x64',
    launches: 12, crashes: 1, ch: 'installer', first: '2026-09-29', fv: '0.99.47',
    sessionSec: 3600, sdr: 'uhd b200', modes: { WFM: 3000, RAW: 600 },
    panels: ['map', 'decoded'], plugins: ['ADS-B 1.0.0'],
  };
}

async function post(body, { method = 'POST', raw } = {}) {
  const usage = [];
  const beats = [];
  const env = {
    USAGE: { writeDataPoint: (p) => usage.push(p) },
    HEARTBEAT: { writeDataPoint: (p) => beats.push(p) },
  };
  const req = new Request('https://telemetry.example.test/', {
    method,
    body: method === 'POST' ? (raw !== undefined ? raw : JSON.stringify(body)) : undefined,
  });
  const res = await worker.fetch(req, env);
  return { status: res.status, usage, beats };
}

// ---- The columns that existed before stall counting must not move -----------

test('an old-format record is accepted and its existing columns are unchanged', async () => {
  const r = await post(oldRecord());
  assert.equal(r.status, 204);
  assert.equal(r.usage.length, 1);
  const p = r.usage[0];
  assert.deepEqual(p.indexes, [ID]);
  // blob1..blob10 and double1..double4 exactly as they were: a column that
  // changed meaning would corrupt the history in the dataset.
  assert.deepEqual(p.blobs.slice(0, 10), [
    '0.99.61', 'Windows 10.0.22631', 'x64', 'uhd b200', 'WFM', 'ADS-B 1.0.0',
    'map,decoded', 'installer', '2026-09-29', '0.99.47',
  ]);
  assert.deepEqual(p.doubles.slice(0, 4), [12, 1, 3600, 3000]);
});

test('an old client that sends no stall count reads as NOT REPORTED, not as zero', async () => {
  const p = (await post(oldRecord())).usage[0];
  assert.equal(p.blobs[10], '');      // blob11: no count was sent
  assert.equal(p.doubles[4], 0);      // double5 is only meaningful where blob11 = '1'
});

test('a new-format record carries its count in double5 and marks blob11', async () => {
  const withStalls = { ...oldRecord(), stalls: 3 };
  const p = (await post(withStalls)).usage[0];
  assert.equal(p.blobs[10], '1');
  assert.equal(p.doubles[4], 3);
  // ...and everything else about the record is what the old record produced.
  const base = (await post(oldRecord())).usage[0];
  assert.deepEqual(p.indexes, base.indexes);
  assert.deepEqual(p.blobs.slice(0, 10), base.blobs.slice(0, 10));
  assert.deepEqual(p.doubles.slice(0, 4), base.doubles.slice(0, 4));
});

test('a reported zero is a real zero, distinct from never reported', async () => {
  const p = (await post({ ...oldRecord(), stalls: 0 })).usage[0];
  assert.equal(p.blobs[10], '1');
  assert.equal(p.doubles[4], 0);
});

// ---- Validation -------------------------------------------------------------

test('a fraction is floored and an enormous count is clamped', async () => {
  assert.equal((await post({ ...oldRecord(), stalls: 2.7 })).usage[0].doubles[4], 2);
  const big = (await post({ ...oldRecord(), stalls: 1e9 })).usage[0];
  assert.equal(big.doubles[4], 100000);
  assert.equal(big.blobs[10], '1');
  const exactlyMax = (await post({ ...oldRecord(), stalls: 100000 })).usage[0];
  assert.equal(exactlyMax.doubles[4], 100000);
  // -0 is written as an ordinary zero.
  const raw = JSON.stringify(oldRecord()).replace(/}$/, ',"stalls":-0}');
  const negZero = (await post(null, { raw })).usage[0];
  assert.equal(negZero.blobs[10], '1');
  assert.equal(Object.is(negZero.doubles[4], 0), true);
});

test('a malformed count is treated as not reported, and the record is still taken', async () => {
  const bad = [
    '"3"',        // a numeric string: only a JSON number counts
    '"abc"',
    '-1',         // negative
    '-0.5',
    'null',
    'true',
    '[]',
    '[3]',
    '{}',
    '{"n":3}',
    '1e999',      // parses to Infinity
  ];
  for (const literal of bad) {
    const raw = JSON.stringify(oldRecord()).replace(/}$/, `,"stalls":${literal}}`);
    const r = await post(null, { raw });
    assert.equal(r.status, 204, `stalls ${literal} must not cost the record`);
    assert.equal(r.usage.length, 1, `stalls ${literal}: the record is still written`);
    const p = r.usage[0];
    assert.equal(p.blobs[10], '', `stalls ${literal} is not a usable count`);
    assert.equal(p.doubles[4], 0, `stalls ${literal} must not reach double5`);
    // The rest of the record survived intact.
    assert.deepEqual(p.doubles.slice(0, 4), [12, 1, 3600, 3000]);
  }
});

// ---- Failures that are not crashes (0.99.64): the `health` string -------------
//
// OLD CLIENT -> NEW WORKER, NEW CLIENT -> NEW WORKER, NEW CLIENT -> OLD WORKER.
// The old Worker is the file as it was at 0.99.63, kept byte for byte in
// test-fixtures/, so "an old Worker answers 204 and discards a field it does not
// know" is a measurement and not a belief.

const HEALTH = 'radio_open.rtlsdr=3,radio_data.rtlsdr=3,radio_fail.rtlsdr.busy=2,sound_ok.wasapi=1';

test('an old client (no health field) is accepted, NOT REPORTED, and every earlier column is unmoved', async () => {
  const r = await post(oldRecord());
  assert.equal(r.status, 204);
  const p = r.usage[0];
  assert.equal(p.blobs[11], '');                      // blob12: no marker
  assert.equal(p.blobs[12], '');                      // blob13: nothing
  assert.deepEqual(p.doubles.slice(5, 9), [0, 0, 0, 0]);   // double6..9 meaningful only where blob12 = '1'
  assert.deepEqual(p.doubles.slice(9, 12), [0, 0, 0]);     // double10..12 (slow frames, recoveries) likewise
  assert.deepEqual(p.blobs.slice(0, 10), [
    '0.99.61', 'Windows 10.0.22631', 'x64', 'uhd b200', 'WFM', 'ADS-B 1.0.0',
    'map,decoded', 'installer', '2026-09-29', '0.99.47',
  ]);
  assert.deepEqual(p.doubles.slice(0, 4), [12, 1, 3600, 3000]);
});

test('a stall-only client (0.99.62) still reads its stalls and has no health marker', async () => {
  const p = (await post({ ...oldRecord(), stalls: 4 })).usage[0];
  assert.equal(p.blobs[10], '1');
  assert.equal(p.doubles[4], 4);
  assert.equal(p.blobs[11], '');
});

test('a new client stores its tokens in blob13, marks blob12 and fills double6..9', async () => {
  const p = (await post({ ...oldRecord(), stalls: 0, health: HEALTH })).usage[0];
  assert.equal(p.blobs[11], '1');
  assert.equal(p.blobs[12], HEALTH);
  assert.deepEqual(p.doubles.slice(5, 9), [2, 3, 3, 1]);
  assert.deepEqual(p.doubles.slice(9, 12), [0, 0, 0]);     // a client with no slow frames and no recoveries: zeros
  // ...and nothing earlier moved.
  const base = (await post({ ...oldRecord(), stalls: 0 })).usage[0];
  assert.deepEqual(p.blobs.slice(0, 11), base.blobs.slice(0, 11));
  assert.deepEqual(p.doubles.slice(0, 5), base.doubles.slice(0, 5));
});

test('a reported "nothing failed" is a real zero, distinct from never reported', async () => {
  const p = (await post({ ...oldRecord(), health: '' })).usage[0];
  assert.equal(p.blobs[11], '1');
  assert.equal(p.blobs[12], '');
  assert.deepEqual(p.doubles.slice(5, 9), [0, 0, 0, 0]);
});

test('the headline doubles are the sums of the validated tokens, never a number the client sent', async () => {
  const rec = {
    ...oldRecord(),
    health: 'radio_fail.rtlsdr.busy=2,radio_fail.hackrf.absent=5,radio_open.soapy=4,sound_ok.alsa=1',
    radioFail: 9999, radioOpen: 9999, healthRadioFail: 9999,   // fields nobody reads
  };
  const p = (await post(rec)).usage[0];
  assert.deepEqual(p.doubles.slice(5, 9), [7, 4, 0, 1]);
});

test('the stored text is the canonical re-encoding: ordered, merged, clamped', async () => {
  const p = (await post({
    ...oldRecord(),
    health: 'sound_ok.alsa=1,radio_fail.rtlsdr.busy=2,radio_fail.rtlsdr.busy=3,scan_none=1500,rec_fail=0,upd_dl=7',
  })).usage[0];
  // vocabulary order, repeats merged (2+3), 1500 clamped to 999, a zero count dropped
  assert.equal(p.blobs[12], 'scan_none=999,radio_fail.rtlsdr.busy=5,sound_ok.alsa=1,upd_dl=7');
  // ...and a count built up by repeats is clamped too, not only a single big one
  const repeats = (await post({ ...oldRecord(), health: 'upd_dl=600,upd_dl=600,upd_dl=600' })).usage[0];
  assert.equal(repeats.blobs[12], 'upd_dl=999');
});

test('nothing outside the vocabulary can be written into the dataset', async () => {
  const hostile = [
    '<script>alert(1)</script>=3',
    'radio_fail.RTLSDR.busy=1',                 // not lower case: not a word of the vocabulary
    'radio_fail.rtlsdr=1',                      // a qualifier is missing
    'radio_fail.rtlsdr.busy.extra=1',           // one too many
    'radio_fail.my-radio-name.busy=1',          // a device name
    'radio_fail.serial0001.busy=1',             // a serial
    'radio_open.C:\\Users\\someone\\x=1',         // a path
    'scan_none= 1', 'scan_none=1x', 'scan_none=-1', 'scan_none=1.5', 'scan_none=',
    'scan_none=00001', 'sound_ok.wasapi=١', 'scan_none\n=1', 'rec_fail=1;DROP TABLE',
    'RADIO_FAIL.rtlsdr.busy=1', ' scan_none=1', 'plug_load.abi =1',
    // slow frames and recoveries (0.99.65): user-wait is not a word, a tier is one of three, a scope is a word, a recovery is one word
    'slow.user-wait.250ms=1', 'slow.rail.2s=1', 'slow.rail=1', 'slow.Rail.250ms=1', 'slow.my-panel.1s=1',
    'slow.rail.250ms.x=1', 'recovered.nope=1', 'recovered.audio.x=1', 'recovered=1', 'recovered.Audio=1',
    'recovered.C:\\Users\\someone=1',
  ];
  for (const text of hostile) {
    const r = await post({ ...oldRecord(), health: `upd_dl=1,${text}` });
    assert.equal(r.status, 204, text);
    const p = r.usage[0];
    // Whatever the second half was, the first half is all that is stored.
    assert.equal(p.blobs[12], 'upd_dl=1', `stored text for ${JSON.stringify(text)}`);
    assert.match(p.blobs[12], /^[a-z0-9_.=,-]*$/);
  }
});

test('a malformed health field is NOT REPORTED, never zero, and never costs the record', async () => {
  const bad = [
    '3', '1.5', 'null', 'true', 'false', '[]', '["scan_none=1"]', '{}', '{"scan_none":1}',  // not a string
    '"abc"', '"scan_none"', '"=1"', '"scan_none=x"', '"junk,more junk"',                    // no legal pair at all
    '"radio_fail.nobody.busy=1"',
    JSON.stringify('scan_none=1,'.repeat(100)),                                              // longer than the application writes
  ];
  for (const literal of bad) {
    const raw = JSON.stringify(oldRecord()).replace(/}$/, `,"health":${literal}}`);
    const r = await post(null, { raw });
    assert.equal(r.status, 204, `health ${literal} must not cost the record`);
    assert.equal(r.usage.length, 1);
    const p = r.usage[0];
    assert.equal(p.blobs[11], '', `health ${literal} is not usable`);
    assert.equal(p.blobs[12], '');
    assert.deepEqual(p.doubles.slice(5, 9), [0, 0, 0, 0]);
    assert.deepEqual(p.doubles.slice(0, 4), [12, 1, 3600, 3000]);   // the rest survived
  }
});

test('the vocabulary is the whole of what is accepted: every legal token is stored, in order', async () => {
  // Every event with every qualifier combination, one pair each, however many
  // records it takes (at most 24 tokens a record).
  const all = [];
  for (const ev of vocabularyFromWorker()) {
    let names = [ev.name];
    for (const words of ev.qualifiers) { names = names.flatMap((n) => words.map((w) => `${n}.${w}`)); }
    all.push(...names);
  }
  assert.ok(all.length > 100);
  const seen = [];
  for (let i = 0; i < all.length; i += 8) {
    const chunk = all.slice(i, i + 8);
    const p = (await post({ ...oldRecord(), health: chunk.map((t) => `${t}=1`).join(',') })).usage[0];
    assert.equal(p.blobs[12], chunk.map((t) => `${t}=1`).join(','));
    seen.push(...p.blobs[12].split(',').map((x) => x.split('=')[0]));
  }
  assert.deepEqual(seen, all);
});

test('the bounds hold: 24 tokens, 8 radio failures, counts to 999, a string no longer than the client writes', async () => {
  const all = [];
  for (const ev of vocabularyFromWorker()) {
    let names = [ev.name];
    for (const words of ev.qualifiers) { names = names.flatMap((n) => words.map((w) => `${n}.${w}`)); }
    all.push(...names);
  }
  // 30 distinct tokens, none a radio failure: the first 24 (in the order sent) are kept.
  const quiet = all.filter((t) => !t.startsWith('radio_fail.')).slice(0, 30);
  const p1 = (await post({ ...oldRecord(), health: quiet.map((t) => `${t}=1`).join(',') })).usage[0];
  assert.equal(p1.blobs[12].split(',').length, 24);
  // 12 distinct radio failures: only 8 kept.
  const fails = all.filter((t) => t.startsWith('radio_fail.')).slice(0, 12);
  const p2 = (await post({ ...oldRecord(), health: fails.map((t) => `${t}=1`).join(',') })).usage[0];
  assert.equal(p2.blobs[12].split(',').length, 8);
  // The longest legal string the client can write - 8 of the longest radio failures
  // and 16 of the longest of everything else - is under the cap and stored whole.
  const byLength = (a, b) => b.length - a.length;
  const failureTokens = all.filter((t) => !t.startsWith('slow.') && !t.startsWith('recovered.'));
  const worst = [
    ...failureTokens.filter((t) => t.startsWith('radio_fail.')).sort(byLength).slice(0, 8),
    ...failureTokens.filter((t) => !t.startsWith('radio_fail.')).sort(byLength).slice(0, 16),
  ];
  const longest = worst.map((t) => `${t}=999`).join(',');
  assert.ok(longest.length <= 832, `${longest.length}`);
  const stored = (await post({ ...oldRecord(), health: longest })).usage[0];
  assert.equal(stored.blobs[12].split(',').length, 24);
  assert.equal(stored.blobs[11], '1');
  // One byte over the cap is not what the application writes: not reported.
  const over = (await post({ ...oldRecord(), health: `scan_none=1,${'x'.repeat(832)}` })).usage[0];
  assert.equal(over.blobs[11], '');
});

test('NEW CLIENT -> OLD WORKER: a 204, the field discarded, and the row exactly what an old record writes', async () => {
  const old = (await import('./test-fixtures/worker-0.99.63.js')).default;
  async function postOld(body) {
    const usage = [];
    const env = {
      USAGE: { writeDataPoint: (p) => usage.push(p) },
      HEARTBEAT: { writeDataPoint: () => {} },
    };
    const res = await old.fetch(
      new Request('https://telemetry.example.test/', { method: 'POST', body: JSON.stringify(body) }), env);
    return { status: res.status, usage };
  }
  const withHealth = await postOld({ ...oldRecord(), stalls: 2, health: HEALTH });
  const without = await postOld({ ...oldRecord(), stalls: 2 });
  // THE CLIENT TAKES ANY 2xx AS "ACCEPTED" AND FORGETS THE COUNTS: this is why the
  // Worker is deployed first (README.md), and it is today's behaviour exactly.
  assert.equal(withHealth.status, 204);
  assert.deepEqual(withHealth.usage, without.usage);
  assert.equal(withHealth.usage[0].blobs.length, 11);     // the old Worker has no blob12 / blob13
  assert.equal(withHealth.usage[0].doubles.length, 5);
  // ...and the NEW Worker, given the same record, writes the same first eleven blobs and five doubles.
  const now = (await post({ ...oldRecord(), stalls: 2, health: HEALTH })).usage[0];
  assert.deepEqual(now.blobs.slice(0, 11), withHealth.usage[0].blobs);
  assert.deepEqual(now.doubles.slice(0, 5), withHealth.usage[0].doubles);
});

test('the column budget: Analytics Engine allows 20 blobs and 20 doubles, and this is how many are used', async () => {
  const p = (await post({ ...oldRecord(), stalls: 0, health: HEALTH })).usage[0];
  assert.equal(p.blobs.length, 13);      // unchanged by slow frames and recoveries: they live in blob13
  assert.equal(p.doubles.length, 12);    // double10..12 are the three new sums
  assert.ok(p.blobs.length <= 20 && p.doubles.length <= 20);
  // The 16 KB blob budget: the longest record this Worker can write is far under it.
  const biggest = p.blobs.reduce((n, b) => n + Buffer.byteLength(b), 0) + 832 + 48 + 40 + 8 + 32 + 8 + 840 + 160 + 16 + 10 + 48;
  assert.ok(biggest < 16384);
});

// ---- Slow frames and recoveries (0.99.65) ---------------------------------------
//
// The same `health` string carries two more families. Compatibility is held in both
// directions against the Worker as it was when the failure counts shipped, kept byte
// for byte in test-fixtures/worker-health-counts.js.

const SLOW = 'slow.rail.250ms=3,slow.rail.1s=2,slow.plugins-reload.5s=1,slow.events.250ms=4';
const RECOVERED = 'recovered.audio=1,recovered.srcthread=2';

async function postTo(workerModule, body) {
  const usage = [];
  const env = {
    USAGE: { writeDataPoint: (p) => usage.push(p) },
    HEARTBEAT: { writeDataPoint: () => {} },
  };
  const res = await workerModule.fetch(
    new Request('https://telemetry.example.test/', { method: 'POST', body: JSON.stringify(body) }), env);
  return { status: res.status, usage };
}
const healthCountsWorker = (await import('./test-fixtures/worker-health-counts.js')).default;

// The two clients: 0.99.65 is the first to send slow frames and recoveries, 0.99.64 sent failure counts only.
function newRecord() { return { ...oldRecord(), v: '0.99.65' }; }
function record064() { return { ...oldRecord(), v: '0.99.64' }; }

test('slow frames and recoveries are stored in blob13 in the written order, and fill double10..12', async () => {
  const sent = `${HEALTH},${SLOW},${RECOVERED}`;
  const p = (await post({ ...newRecord(), stalls: 0, health: sent })).usage[0];
  assert.equal(p.blobs[11], '1');
  // events < rail < plugins-reload in the written order; within rail, 250ms before 1s
  assert.equal(p.blobs[12], `${HEALTH},slow.events.250ms=4,slow.rail.250ms=3,slow.rail.1s=2,`
    + `slow.plugins-reload.5s=1,${RECOVERED}`);
  assert.deepEqual(p.doubles.slice(5, 9), [2, 3, 3, 1]);       // the failure sums are unmoved
  // double10: every slow frame (4+3+2+1); double11: those of a second or more (2+1); double12: 1+2
  assert.deepEqual(p.doubles.slice(9, 12), [10, 3, 3]);
  assert.equal(p.doubles.length, 12);
});

test('the new sums are of the validated tokens, never a number the client sent', async () => {
  const rec = {
    ...newRecord(),
    health: 'slow.rail.250ms=1,slow.user-wait.5s=500,recovered.audio=1,recovered.nope=900',
    slowFrames: 9999, slow: 9999, recovered: 9999, healthSlow: 9999,   // fields nobody reads
  };
  const p = (await post(rec)).usage[0];
  assert.equal(p.blobs[12], 'slow.rail.250ms=1,recovered.audio=1');
  assert.deepEqual(p.doubles.slice(9, 12), [1, 0, 1]);
  // a 5 s frame counts in "a second or more", and only there on top of the total
  const five = (await post({ ...newRecord(), health: 'slow.startup.5s=2' })).usage[0];
  assert.deepEqual(five.doubles.slice(9, 12), [2, 2, 0]);
});

test('the cases both implementations read: the selection of the worst, the order, the clamps', async () => {
  const cases = JSON.parse(readFileSync(`${here}test-fixtures/health-cases.json`, 'utf8'));
  assert.ok(cases.length >= 12);
  for (const c of cases) {
    assert.ok(c.input.length <= 832, c.name);
    const p = (await post({ ...newRecord(), health: c.input })).usage[0];
    if (c.expected === '') {
      assert.equal(p.blobs[11], '', `${c.name}: a text with nothing legal in it is not reported`);
      assert.equal(p.blobs[12], '', c.name);
    } else {
      assert.equal(p.blobs[11], '1', c.name);
      assert.equal(p.blobs[12], c.expected, c.name);
    }
  }
});

test('the caps: eight slow tokens and eight recoveries, over them the worst are kept', async () => {
  const scopes = vocabularyFromWorker().find((e) => e.name === 'slow').qualifiers[0];
  const recovered = vocabularyFromWorker().find((e) => e.name === 'recovered').qualifiers[0];
  // at the boundary: exactly eight slow tokens, all kept
  const eight = scopes.slice(0, 8).map((s) => `slow.${s}.250ms=1`).join(',');
  assert.equal((await post({ ...newRecord(), health: eight })).usage[0].blobs[12], eight);
  // one more, in any order: the one that goes is the last in the written order
  const nine = scopes.slice(0, 9).map((s) => `slow.${s}.250ms=1`);
  const shuffled = [...nine].reverse().join(',');
  assert.equal((await post({ ...newRecord(), health: shuffled })).usage[0].blobs[12], eight);
  // the other family has its own cap and takes nothing from the first
  const mixed = `${eight},${recovered.slice(0, 8).map((w) => `recovered.${w}=1`).join(',')}`;
  assert.equal((await post({ ...newRecord(), health: mixed })).usage[0].blobs[12], mixed);
  const many = `${mixed},recovered.${recovered[8]}=1,slow.${scopes[8]}.5s=1`;
  const stored = (await post({ ...newRecord(), health: many })).usage[0].blobs[12].split(',');
  assert.equal(stored.filter((t) => t.startsWith('slow.')).length, 8);
  assert.equal(stored.filter((t) => t.startsWith('recovered.')).length, 8);
  assert.ok(stored.includes(`slow.${scopes[8]}.5s=1`), 'the 5 s frame is kept over a stutter');
  // the failure families are not touched by either
  const withFailures = `upd_dl=1,${many}`;
  assert.ok((await post({ ...newRecord(), health: withFailures })).usage[0].blobs[12].startsWith('upd_dl=1,'));
});

test('NEW CLIENT (slow frames, recoveries) -> OLD WORKER: a 204, the failure tokens read, the new ones dropped', async () => {
  const sent = `${HEALTH},${SLOW},${RECOVERED}`;
  const old = await postTo(healthCountsWorker, { ...newRecord(), stalls: 0, health: sent });
  assert.equal(old.status, 204);
  const p = old.usage[0];
  assert.equal(p.blobs[11], '1');
  assert.equal(p.blobs[12], HEALTH);                                   // exactly what a client without them writes
  assert.deepEqual(p.doubles.slice(5, 9), [2, 3, 3, 1]);
  assert.equal(p.doubles.length, 9);                                   // it has no double10..12
  // ...and the same record as the NEW Worker writes it: the same first thirteen blobs but for the text
  const now = (await post({ ...newRecord(), stalls: 0, health: sent })).usage[0];
  assert.deepEqual(now.blobs.slice(0, 12), p.blobs.slice(0, 12));
  assert.deepEqual(now.doubles.slice(0, 9), p.doubles);
  // A record with ONLY the new families is, to the old Worker, nothing it understands: not reported,
  // never zero - and the record is still taken.
  const only = await postTo(healthCountsWorker, { ...newRecord(), health: `${SLOW},${RECOVERED}` });
  assert.equal(only.status, 204);
  assert.equal(only.usage[0].blobs[11], '');
  assert.deepEqual(only.usage[0].doubles.slice(0, 5), [12, 1, 3600, 3000, 0]);
  // THE LONGEST RECORD THE CLIENT WRITES is read whole by the old Worker: it is never more than 832
  // characters, so the old Worker's own limit does not refuse it.
  const all = [];
  for (const ev of vocabularyFromWorker()) {
    let names = [ev.name];
    for (const words of ev.qualifiers) { names = names.flatMap((n) => words.map((w) => `${n}.${w}`)); }
    all.push(...names);
  }
  const base = all.filter((t) => !t.startsWith('slow.') && !t.startsWith('recovered.'));
  const longest = (list, n) => [...list].sort((a, b) => b.length - a.length).slice(0, n);
  const worstBase = [
    ...longest(base.filter((t) => t.startsWith('radio_fail.')), 8),
    ...longest(base.filter((t) => !t.startsWith('radio_fail.')), 16),
  ].sort((a, b) => base.indexOf(a) - base.indexOf(b));
  const text = worstBase.map((t) => `${t}=999`).join(',');
  const room = 832 - text.length;
  assert.ok(room > 0);
  const extra = all.filter((t) => t.startsWith('slow.')).slice(0, 8).map((t) => `${t}=999`);
  let record = text;
  for (const e of extra) { if (record.length + 1 + e.length <= 832) { record += `,${e}`; } }
  assert.ok(record.length <= 832);
  const oldWide = (await postTo(healthCountsWorker, { ...newRecord(), health: record })).usage[0];
  assert.equal(oldWide.blobs[11], '1');
  assert.equal(oldWide.blobs[12].split(',').length, 24);              // every failure token, none lost to the new ones
  // ...and the NEW Worker keeps all of it - the failure tokens AND the slow ones that fit beside them -
  // because the cap on the failure tokens is not the cap on the families after them.
  const newWide = (await post({ ...newRecord(), health: record })).usage[0];
  assert.equal(newWide.blobs[12], record);
  assert.ok(newWide.blobs[12].split(',').length > 24, 'slow tokens beside 24 failure tokens are kept');
});

test('0.99.64 CLIENT (failure counts only) -> NEW WORKER: the row the 0.99.64 Worker wrote, and zeros in the new columns', async () => {
  for (const body of [oldRecord(), { ...record064(), stalls: 2 }, { ...record064(), stalls: 2, health: HEALTH },
    { ...record064(), health: '' }]) {
    const was = (await postTo(healthCountsWorker, body)).usage[0];
    const now = (await post(body)).usage[0];
    assert.deepEqual(now.blobs, was.blobs, JSON.stringify(body.health));
    assert.deepEqual(now.doubles.slice(0, 9), was.doubles);
    // These zeros are what the new Worker writes for a build that never measured slow frames or recoveries.
    // They mean "not measured"; the Worker cannot say so (blob12 is '1' on these rows), so the reader does,
    // by version (usage.ps1, telemetry-worker/README.md).
    assert.deepEqual(now.doubles.slice(9, 12), [0, 0, 0]);
  }
});

test('a heartbeat cannot be turned into a failure report either', async () => {
  const r = await post({ id: ID, v: '0.99.64', beat: 1, health: HEALTH });
  assert.equal(r.status, 204);
  assert.equal(r.usage.length, 0);
  assert.deepEqual(r.beats[0], { indexes: [ID], blobs: ['0.99.64'] });
});

// ---- What must keep working ------------------------------------------------

test('a heartbeat cannot be turned into a session report by carrying a count', async () => {
  const r = await post({ id: ID, v: '0.99.62', beat: 1, stalls: 5, sessionSec: 99 });
  assert.equal(r.status, 204);
  assert.equal(r.usage.length, 0);
  assert.equal(r.beats.length, 1);
  assert.deepEqual(r.beats[0], { indexes: [ID], blobs: ['0.99.62'] });
});

test('a version up to 48 characters is stored whole, and no narrower', async () => {
  const nightly = '0.57.0-nightly.20260819.b97092e';
  assert.equal(nightly.length, 31);
  const forty8 = '0.99.62-nightly.20261004.' + 'abcdef0123456789abcdef0'.slice(0, 23);
  assert.equal(forty8.length, 48);
  assert.equal((await post({ ...oldRecord(), v: nightly, stalls: 1 })).usage[0].blobs[0], nightly);
  assert.equal((await post({ ...oldRecord(), v: forty8, stalls: 1 })).usage[0].blobs[0], forty8);
  assert.equal((await post({ ...oldRecord(), v: forty8 + 'ZZZZ' })).usage[0].blobs[0], forty8);
});

test('a bad id, a bad method and a bad body are refused exactly as before', async () => {
  assert.equal((await post({ ...oldRecord(), id: 'nope', stalls: 1 })).status, 400);
  assert.equal((await post(null, { method: 'GET' })).status, 405);
  assert.equal((await post(null, { raw: '{not json' })).status, 400);
  assert.equal((await post(null, { raw: ' '.repeat(5000) })).status, 413);
});

// ---- The reader --------------------------------------------------------------

const powershell = (() => {
  const r = spawnSync('powershell.exe', ['-NoProfile', '-Command', '$PSVersionTable.PSVersion.Major'], { encoding: 'utf8' });
  return r.status === 0 ? 'powershell.exe' : null;
})();
const readerTest = powershell ? test : test.skip;

function runReader(args, env) {
  const clean = { ...process.env };
  delete clean.CLOUDFLARE_API_TOKEN;
  delete clean.CLOUDFLARE_ACCOUNT_ID;
  return spawnSync(
    powershell,
    ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', `${here}usage.ps1`, ...args],
    { encoding: 'utf8', env: { ...clean, ...env }, timeout: 90000 },
  );
}

readerTest('with no credentials the reader says so and prints no figures', () => {
  const r = runReader([]);
  assert.notEqual(r.status, 0);
  const out = `${r.stdout}\n${r.stderr}`;
  assert.match(out, /No API token/);
  // Nothing that looks like a result: no section, no table, no zero.
  assert.doesNotMatch(out, /Display stalls/);
  assert.doesNotMatch(out, /installs \(unique\)/);
  assert.doesNotMatch(out, /with stalls/);
  assert.doesNotMatch(out, /What fails/);
  assert.doesNotMatch(out, /radio fail/);
});

// A stand-in for Cloudflare's SQL API. Records every query, answers the stall
// queries from `rows`.
function standIn(rows) {
  const queries = [];
  const server = http.createServer((req, res) => {
    let sql = '';
    req.on('data', (c) => { sql += c; });
    req.on('end', () => {
      queries.push(sql);
      let data = [];
      const perToken = sql.match(/startsWith\(blob13, '([a-z0-9_.]+)='\)/);
      const perScope = sql.match(/startsWith\(blob13, 'slow\.([a-z-]+)\.'\)/);
      if (/AS radioFail/.test(sql)) {
        data = rows.healthVersions || [];
      } else if (/AS slowLong/.test(sql)) {
        data = rows.slowVersions || [];
      } else if (/double10 > 0/.test(sql)) {
        data = rows.withSlow || [];
      } else if (/blob13 AS slowHealth/.test(sql)) {
        data = rows.slowStrings || [];
      } else if (perScope) {
        data = (rows.slowInstalls || {})[perScope[1]] || [];
      } else if (/AS recoveries/.test(sql)) {
        data = rows.recVersions || [];
      } else if (/double12 > 0/.test(sql)) {
        data = rows.withRec || [];
      } else if (/blob13 AS recoveredHealth/.test(sql)) {
        data = rows.recStrings || [];
      } else if (/blob13 AS health/.test(sql)) {
        data = rows.healthStrings || [];
      } else if (perToken) {
        data = (rows.healthInstalls || {})[perToken[1]] || [];
      } else if (/^SELECT blob1 AS version FROM foxsdr_usage WHERE .*GROUP BY blob1/.test(sql) && rows.seen) {
        data = rows.seen;
      } else if (/count\(\) AS reports/.test(sql)) {
        data = [{ installs: '5', reports: '9' }];
      } else if (/blob11 = '1' AND double5 > 0/.test(sql)) {
        data = rows.stalled;
      } else if (/blob11 = '1'/.test(sql)) {
        data = rows.reporting;
      } else if (/^SELECT blob1 AS version FROM foxsdr_usage WHERE .*GROUP BY blob1/.test(sql)) {
        // every version seen in the window, whether or not it reports a count
        data = [...rows.reporting.map((r) => ({ version: r.version })), ...rows.unmeasured];
      } else if (/^SELECT count\(DISTINCT index1\) AS installs FROM/.test(sql)) {
        data = [{ installs: '3' }];
      }
      res.setHeader('content-type', 'application/json');
      res.end(JSON.stringify({ meta: [], data, rows: data.length }));
    });
  });
  return new Promise((resolve) => {
    server.listen(0, '127.0.0.1', () => resolve({ server, queries, port: server.address().port }));
  });
}

function runAgainst(port) {
  // spawnSync would block this process's event loop and the stand-in with it,
  // so the reader runs asynchronously.
  const env = { ...process.env };
  delete env.CLOUDFLARE_API_TOKEN;
  delete env.CLOUDFLARE_ACCOUNT_ID;
  return new Promise((resolve) => {
    const child = spawn(
      powershell,
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', `${here}usage.ps1`,
        '-ApiBase', `http://127.0.0.1:${port}/client/v4`, '-AccountId', 'test', '-Token', 'not-a-real-token'],
      { env },
    );
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (c) => { stdout += c; });
    child.stderr.on('data', (c) => { stderr += c; });
    child.on('close', (status) => resolve({ status, stdout, stderr }));
  });
}

readerTest('the reader prints stalls per version from count(DISTINCT index1), and lists unmeasured builds apart', async () => {
  const nightly = '0.99.62-nightly.20261004.abcdef0123456789';
  const { server, queries, port } = await standIn({
    reporting: [{ version: '0.99.62', installs: '4' }, { version: nightly, installs: '2' }],
    stalled: [{ version: '0.99.62', installs: '2', stalls: '7' }],
    unmeasured: [{ version: '0.99.50' }, { version: '0.99.61' }],
  });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /Display stalls, by version/);
    // installs that reported, installs that reported at least one stall, stalls in total
    assert.match(r.stdout, /0\.99\.62\s+4\s+2\s+7\b/);
    // a build that reports a count and had no stalls is a real zero - printed in full, 40 characters wide
    assert.ok(r.stdout.includes(nightly), 'a long pre-release version is not truncated');
    assert.match(r.stdout, new RegExp(`${nightly.replace(/\./g, '\\.')}\\s+2\\s+0\\s+0\\b`));
    // builds that send no count are named as unmeasured, not shown as zero
    assert.match(r.stdout, /Not measured[^\n]*0\.99\.50[^\n]*0\.99\.61/);

    const stallQueries = queries.filter((q) => /blob11/.test(q));
    for (const q of queries) {
      assert.doesNotMatch(q, /uniq\s*\(/i, 'approximate uniq() must never be used for installs');
    }
    // The two questions that count anything ask only about builds that report a
    // count, and count installs with count(DISTINCT index1).
    assert.equal(stallQueries.length, 2, 'installs that report, and installs with stalls');
    for (const q of stallQueries) {
      assert.match(q, /blob11 = '1'/);
      assert.match(q, /count\(DISTINCT index1\) AS installs/);
    }
    assert.ok(stallQueries.some((q) => /count\(DISTINCT index1\) AS installs, sum\(double5\) AS stalls/.test(q) && /double5 > 0/.test(q)));
    // ...and the unmeasured builds come from every version seen, not from a
    // condition on how an unwritten column reads back.
    assert.ok(queries.some((q) => /^SELECT blob1 AS version FROM foxsdr_usage WHERE .*GROUP BY blob1/.test(q)));
  } finally {
    server.close();
  }
});

readerTest('when no build reports a count the reader says unmeasured and prints no zero rows', async () => {
  const { server, port } = await standIn({ reporting: [], stalled: [], unmeasured: [{ version: '0.99.61' }] });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /Display stalls, by version/);
    assert.match(r.stdout, /No build in this window reports a stall count yet/);
    assert.match(r.stdout, /not zero stalls/);
    assert.doesNotMatch(r.stdout, /with stalls/);
  } finally {
    server.close();
  }
});

readerTest('the reader prints what fails per version: sessions and installs per event, only rows that SENT the counts, the rest unmeasured', async () => {
  const { server, queries, port } = await standIn({
    reporting: [], stalled: [], unmeasured: [],
    healthVersions: [
      { version: '0.99.64', installs: '4', sessions: '9', radioFail: '3', radioOpen: '11', radioData: '10', sound: '8' },
      { version: '0.99.65', installs: '2', sessions: '2', radioFail: '0', radioOpen: '2', radioData: '2', sound: '2' },
    ],
    healthStrings: [
      { version: '0.99.64', health: 'radio_open.rtlsdr=1,radio_fail.rtlsdr.busy=2', sessions: '2' },
      { version: '0.99.64', health: 'radio_fail.rtlsdr.busy=1', sessions: '1' },
      // a count string with something in it that is not the vocabulary: never reaches a query
      { version: '0.99.64', health: "radio_fail.rtlsdr.busy=1,evil'--=1", sessions: '1' },
    ],
    healthInstalls: {
      'radio_fail.rtlsdr.busy': [{ version: '0.99.64', installs: '3' }],
      'radio_open.rtlsdr': [{ version: '0.99.64', installs: '1' }],
    },
    seen: [{ version: '0.99.50' }, { version: '0.99.64' }, { version: '0.99.65' }],
  });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /What fails, by version/);
    // per version: installs, sessions, radio fail, radio open, samples, with sound
    assert.match(r.stdout, /0\.99\.64\s+4\s+9\s+3\s+11\s+10\s+8\b/);
    // a reporting version with no failures is a real zero, printed
    assert.match(r.stdout, /0\.99\.65\s+2\s+2\s+0\s+2\s+2\s+2\b/);
    // per event: installs (count(DISTINCT index1)), sessions, total count
    assert.match(r.stdout, /0\.99\.64\s+radio_fail\.rtlsdr\.busy\s+3\s+4\s+6\b/);
    assert.match(r.stdout, /0\.99\.64\s+radio_open\.rtlsdr\s+1\s+2\s+2\b/);
    // a build that sends no failure counts is named as unmeasured, not shown as zero
    assert.match(r.stdout, /Not measured \(these builds send no failure counts\)[^\n]*0\.99\.50/);
    assert.doesNotMatch(r.stdout, /0\.99\.50\s+\d/);

    const failQueries = queries.filter((q) => /blob12|blob13/.test(q));
    assert.ok(failQueries.length >= 4, 'versions, count strings, one install count per event');
    for (const q of queries) {
      assert.doesNotMatch(q, /uniq\s*\(/i, 'approximate uniq() must never be used for installs');
      assert.doesNotMatch(q, /evil/, 'text that is not the vocabulary never reaches a query');
    }
    // Every question about failures asks only about builds that SENT the counts.
    for (const q of failQueries) { assert.match(q, /blob12 = '1'/); }
    // The installs come from count(DISTINCT index1), once per event seen.
    const perEvent = failQueries.filter((q) => /startsWith\(blob13/.test(q));
    assert.equal(perEvent.length, 2);
    for (const q of perEvent) { assert.match(q, /count\(DISTINCT index1\) AS installs/); }
  } finally {
    server.close();
  }
});

readerTest('when no build sends failure counts the reader says unmeasured and prints no zero rows', async () => {
  const { server, port } = await standIn({
    reporting: [], stalled: [], unmeasured: [{ version: '0.99.63' }], healthVersions: [], seen: [{ version: '0.99.63' }],
  });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /What fails, by version/);
    assert.match(r.stdout, /No build in this window reports failure counts yet/);
    assert.match(r.stdout, /not zero failures/);
    assert.doesNotMatch(r.stdout, /radio fail/);
  } finally {
    server.close();
  }
});

readerTest('the reader prints slow frames per version: installs with any, per 1,000 records, the top scopes, only rows that SENT counts', async () => {
  // A BUILD THAT MEASURES slow frames is 0.99.65 or later. Every row below with a version before that - the
  // 0.99.64 ones carry blob12 = '1' and ZEROS in double10..12 because that build never measured them -
  // must come out as not measured, and must move no figure of the rows that did.
  const { server, queries, port } = await standIn({
    reporting: [], stalled: [], unmeasured: [], healthVersions: [],
    seen: [{ version: '0.99.50' }, { version: '0.99.64' }, { version: '0.99.65' }, { version: '0.99.100' }, { version: '0.99.9' },
      { version: '0.99.65-rc1' }, { version: 'junk' }, { version: '' }],
    slowVersions: [
      { version: '0.99.64', installs: '9', sessions: '100', slow: '0', slowLong: '0' },
      { version: '0.99.65', installs: '4', sessions: '20', slow: '30', slowLong: '6' },
      { version: '0.99.100', installs: '2', sessions: '4', slow: '2', slowLong: '0' },
      { version: '0.99.9', installs: '1', sessions: '7', slow: '0', slowLong: '0' },        // text sorts above 0.99.65; it is older
      { version: '0.99.65-rc1', installs: '1', sessions: '3', slow: '0', slowLong: '0' },   // a pre-release is never assumed
      { version: 'junk', installs: '1', sessions: '3', slow: '0', slowLong: '0' },
      { version: '', installs: '1', sessions: '3', slow: '0', slowLong: '0' },
    ],
    withSlow: [{ version: '0.99.65', installs: '3' }, { version: '0.99.100', installs: '2' }],
    slowStrings: [
      { version: '0.99.65', slowHealth: 'slow.rail.250ms=3,slow.rail.1s=1,slow.plugins-reload.5s=1', sessions: '4' },
      { version: '0.99.65', slowHealth: 'slow.rail.250ms=2', sessions: '5' },
      // text that is not the vocabulary - an injection, and the one scope that is not a word - never reaches a query
      { version: '0.99.65', slowHealth: "slow.rail.250ms=1,slow.evil'--.1s=1,slow.user-wait.250ms=9", sessions: '1' },
      // a row of a build that does not measure carrying a slow token anyway is not read as a measurement
      { version: '0.99.64', slowHealth: 'slow.rail.250ms=9', sessions: '1' },
    ],
    slowInstalls: {
      rail: [{ version: '0.99.65', installs: '3' }, { version: '0.99.64', installs: '1' }],
      'plugins-reload': [{ version: '0.99.65', installs: '1' }],
    },
  });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /Slow frames, by version/);
    const section = r.stdout.slice(r.stdout.indexOf('Slow frames, by version'), r.stdout.indexOf('Recovered from, by version'));
    // version, installs, sessions, installs with any slow frame, slow per 1,000 records, 1 s+ per 1,000
    assert.match(section, /0\.99\.65\s+4\s+20\s+3\s+1500\.0\s+300\.0\b/);
    // (the 1500.0 above is per version: the unmeasured 0.99.64's 100 sessions of zeros moved nothing)
    // versions are compared as VERSIONS: 0.99.100 measures, 0.99.9 does not
    assert.match(section, /0\.99\.100\s+2\s+4\s+2\s+500\.0\s+0\.0\b/);
    // none of these is a row of the table, and none is shown as a zero
    for (const v of ['0\\.99\\.64', '0\\.99\\.9', '0\\.99\\.65-rc1', 'junk', '0\\.99\\.50']) {
      assert.doesNotMatch(section, new RegExp(`^\\s+${v}\\s+\\d`, 'm'), `${v} must not be a table row`);
    }
    const notMeasured = section.split('\n').find((l) => /Not measured/.test(l)) || '';
    // exactly the versions that do not measure - and neither 0.99.65 nor 0.99.100, which do
    assert.deepEqual(notMeasured.slice(notMeasured.lastIndexOf('): ') + 3).split(', '),
      ['0.99.50', '0.99.64', '0.99.9', '0.99.65-rc1', 'junk', '(not reported)']);
    // top scopes: installs (count(DISTINCT index1)), records carrying it, slow frames, of a second or more
    assert.match(section, /0\.99\.65\s+rail\s+3\s+10\s+27\s+4\b/);
    assert.match(section, /0\.99\.65\s+plugins-reload\s+1\s+4\s+4\s+4\b/);
    assert.doesNotMatch(section, /0\.99\.64\s+rail/);
    assert.doesNotMatch(r.stdout, /user-wait/);

    const slowQueries = queries.filter((q) => /double10|slowHealth|slow\./.test(q));
    assert.ok(slowQueries.length >= 5, 'versions, installs with any, count strings, one install count per scope');
    for (const q of queries) {
      assert.doesNotMatch(q, /uniq\s*\(/i, 'approximate uniq() must never be used for installs');
      assert.doesNotMatch(q, /evil|user-wait/, 'text that is not the vocabulary never reaches a query');
    }
    for (const q of slowQueries) {
      assert.match(q, /blob12 = '1'/);                                  // only builds that SENT the counts
      assert.match(q, /count\(DISTINCT index1\) AS installs|count\(\) AS sessions/);
    }
    const perScope = slowQueries.filter((q) => /startsWith\(blob13, 'slow\./.test(q));
    assert.equal(perScope.length, 2);
    for (const q of perScope) { assert.match(q, /count\(DISTINCT index1\) AS installs/); }
  } finally {
    server.close();
  }
});

readerTest('when no build sends slow frames the reader says unmeasured; a reporting build with none says so', async () => {
  const none = await standIn({ reporting: [], stalled: [], unmeasured: [], healthVersions: [], seen: [], slowVersions: [] });
  try {
    const r = await runAgainst(none.port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /Slow frames, by version/);
    assert.match(r.stdout, /No build in this window reports slow frames yet/);
    assert.match(r.stdout, /not zero slow frames/);
    assert.doesNotMatch(r.stdout, /w\/ slow/);
  } finally {
    none.server.close();
  }
  // ONLY a build that does not measure it (0.99.64: blob12 = '1', zeros in double10..12): unmeasured, never zero.
  const early = await standIn({
    reporting: [], stalled: [], unmeasured: [], healthVersions: [], seen: [{ version: '0.99.64' }],
    slowVersions: [{ version: '0.99.64', installs: '3', sessions: '3', slow: '0', slowLong: '0' }],
    slowStrings: [],
  });
  try {
    const r = await runAgainst(early.port);
    assert.equal(r.status, 0, r.stderr);
    const section = r.stdout.slice(r.stdout.indexOf('Slow frames, by version'), r.stdout.indexOf('Recovered from, by version'));
    assert.match(section, /No build in this window reports slow frames yet/);
    assert.match(section, /not zero slow frames/);
    assert.match(section, /Not measured[^\n]*0\.99\.64/);
    assert.doesNotMatch(section, /0\.99\.64\s+3\s+3/);
    assert.doesNotMatch(section, /w\/ slow/);
    assert.doesNotMatch(section, /No slow frame was reported/);
  } finally {
    early.server.close();
  }
  // A build that measures it and had none: a real zero, printed - beside an unmeasured 0.99.64 that is not.
  const zero = await standIn({
    reporting: [], stalled: [], unmeasured: [], healthVersions: [], seen: [{ version: '0.99.64' }, { version: '0.99.65' }],
    slowVersions: [
      { version: '0.99.64', installs: '3', sessions: '3', slow: '0', slowLong: '0' },
      { version: '0.99.65', installs: '3', sessions: '3', slow: '0', slowLong: '0' },
    ],
    slowStrings: [],
  });
  try {
    const r = await runAgainst(zero.port);
    assert.equal(r.status, 0, r.stderr);
    const section = r.stdout.slice(r.stdout.indexOf('Slow frames, by version'), r.stdout.indexOf('Recovered from, by version'));
    assert.match(section, /0\.99\.65\s+3\s+3\s+0\s+0\.0\s+0\.0\b/);
    assert.doesNotMatch(section, /^\s+0\.99\.64\s+\d/m);
    assert.match(section, /Not measured[^\n]*0\.99\.64/);
    assert.match(section, /No slow frame was reported by any build in this window/);
  } finally {
    zero.server.close();
  }
});

readerTest('the reader prints what was recovered from per version: installs with any, the top words, only rows that SENT counts', async () => {
  // As for slow frames: recoveries are measured from 0.99.65; a 0.99.64 row (blob12 = '1', double12 = 0) is not zero.
  const { server, queries, port } = await standIn({
    reporting: [], stalled: [], unmeasured: [], healthVersions: [],
    seen: [{ version: '0.99.64' }, { version: '0.99.65' }, { version: '0.99.65-rc1' }, { version: 'junk' }],
    recVersions: [
      { version: '0.99.64', installs: '9', sessions: '100', recoveries: '0' },
      { version: '0.99.65', installs: '4', sessions: '20', recoveries: '7' },
      { version: '0.99.65-rc1', installs: '1', sessions: '2', recoveries: '0' },
      { version: 'junk', installs: '1', sessions: '2', recoveries: '0' },
    ],
    withRec: [{ version: '0.99.65', installs: '2' }],
    recStrings: [
      { version: '0.99.65', recoveredHealth: 'recovered.audio=1,recovered.srcthread=2', sessions: '3' },
      { version: '0.99.65', recoveredHealth: 'recovered.audio=1', sessions: '2' },
      { version: '0.99.65', recoveredHealth: "recovered.evil'--=1,recovered.cfgsave=1", sessions: '1' },
      { version: '0.99.64', recoveredHealth: 'recovered.audio=5', sessions: '1' },   // not read as a measurement
    ],
    healthInstalls: {
      'recovered.audio': [{ version: '0.99.65', installs: '2' }, { version: '0.99.64', installs: '1' }],
      'recovered.srcthread': [{ version: '0.99.65', installs: '1' }],
      'recovered.cfgsave': [{ version: '0.99.65', installs: '1' }],
    },
  });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /Recovered from, by version/);
    const section = r.stdout.slice(r.stdout.indexOf('Recovered from, by version'));
    assert.match(section, /0\.99\.65\s+4\s+20\s+2\s+7\b/);
    // word: installs, records, count
    assert.match(section, /0\.99\.65\s+recovered\.audio\s+2\s+5\s+5\b/);
    assert.match(section, /0\.99\.65\s+recovered\.srcthread\s+1\s+3\s+6\b/);
    assert.match(section, /0\.99\.65\s+recovered\.cfgsave\s+1\s+1\s+1\b/);
    // the unmeasured builds are not rows, not zeros and not words of the table - they are named apart
    for (const v of ['0\\.99\\.64', '0\\.99\\.65-rc1', 'junk']) {
      assert.doesNotMatch(section, new RegExp(`^\\s+${v}\\s+\\S`, 'm'), `${v} must not be a row`);
    }
    const notMeasured = section.split('\n').find((l) => /Not measured/.test(l)) || '';
    for (const v of ['0.99.64', '0.99.65-rc1', 'junk']) { assert.ok(notMeasured.includes(v), `${v}: ${notMeasured}`); }
    const recQueries = queries.filter((q) => /double12|recoveredHealth|recovered\./.test(q));
    assert.ok(recQueries.length >= 6, 'versions, installs with any, count strings, one install count per word');
    for (const q of queries) {
      assert.doesNotMatch(q, /uniq\s*\(/i);
      assert.doesNotMatch(q, /evil/, 'text that is not the vocabulary never reaches a query');
    }
    for (const q of recQueries) { assert.match(q, /blob12 = '1'/); }
    const perWord = recQueries.filter((q) => /startsWith\(blob13, 'recovered\./.test(q));
    assert.equal(perWord.length, 3);
    for (const q of perWord) { assert.match(q, /count\(DISTINCT index1\) AS installs/); }
  } finally {
    server.close();
  }
});

readerTest('when no build sends recoveries the reader says unmeasured; a reporting build with none says so', async () => {
  const none = await standIn({ reporting: [], stalled: [], unmeasured: [], healthVersions: [], seen: [], recVersions: [] });
  try {
    const r = await runAgainst(none.port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /No build in this window reports recoveries yet/);
    assert.match(r.stdout, /not zero recoveries/);
    assert.doesNotMatch(r.stdout, /w\/ recovery/);
  } finally {
    none.server.close();
  }
  // ONLY a build that does not measure it (0.99.64: blob12 = '1', double12 = 0): unmeasured, never zero.
  const early = await standIn({
    reporting: [], stalled: [], unmeasured: [], healthVersions: [], seen: [{ version: '0.99.64' }],
    recVersions: [{ version: '0.99.64', installs: '3', sessions: '3', recoveries: '0' }], recStrings: [],
  });
  try {
    const r = await runAgainst(early.port);
    assert.equal(r.status, 0, r.stderr);
    const section = r.stdout.slice(r.stdout.indexOf('Recovered from, by version'));
    assert.match(section, /No build in this window reports recoveries yet/);
    assert.match(section, /not zero recoveries/);
    assert.match(section, /Not measured[^\n]*0\.99\.64/);
    assert.doesNotMatch(section, /0\.99\.64\s+3\s+3/);
    assert.doesNotMatch(section, /w\/ recovery|No recovery was reported/);
  } finally {
    early.server.close();
  }
  // A build that measures it and recovered from nothing: a real zero, beside an unmeasured 0.99.64 that is not.
  const zero = await standIn({
    reporting: [], stalled: [], unmeasured: [], healthVersions: [], seen: [{ version: '0.99.64' }, { version: '0.99.65' }],
    recVersions: [
      { version: '0.99.64', installs: '3', sessions: '3', recoveries: '0' },
      { version: '0.99.65', installs: '3', sessions: '3', recoveries: '0' },
    ],
    recStrings: [],
  });
  try {
    const r = await runAgainst(zero.port);
    assert.equal(r.status, 0, r.stderr);
    const section = r.stdout.slice(r.stdout.indexOf('Recovered from, by version'));
    assert.match(section, /0\.99\.65\s+3\s+3\s+0\s+0\b/);
    assert.doesNotMatch(section, /^\s+0\.99\.64\s+\d/m);
    assert.match(section, /Not measured[^\n]*0\.99\.64/);
    assert.match(section, /No recovery was reported by any build in this window/);
  } finally {
    zero.server.close();
  }
});

readerTest('the rule that decides which builds measure slow frames and recoveries is one named constant, and the README states it', () => {
  const reader = readFileSync(`${here}usage.ps1`, 'utf8');
  const constants = reader.match(/^\$FirstVersionThatMeasuresSlowFramesAndRecoveries\s*=\s*'([0-9.]+)'/m);
  assert.ok(constants, 'one named constant');
  assert.equal(constants[1], '0.99.65');
  const readme = readFileSync(`${here}README.md`, 'utf8').replace(/\s+/g, ' ');
  assert.match(readme, /slow frames and recoveries are measured from 0\.99\.65; a row from an earlier build is unmeasured whatever its doubles say/);
});

readerTest('a reporting build with no failure at all says so, and prints no event rows', async () => {
  const { server, port } = await standIn({
    reporting: [], stalled: [], unmeasured: [],
    healthVersions: [{ version: '0.99.64', installs: '3', sessions: '3', radioFail: '0', radioOpen: '3', radioData: '3', sound: '3' }],
    healthStrings: [], seen: [{ version: '0.99.64' }],
  });
  try {
    const r = await runAgainst(port);
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /0\.99\.64\s+3\s+3\s+0\s+3\s+3\s+3\b/);
    assert.match(r.stdout, /No failure was reported by any build in this window/);
  } finally {
    server.close();
  }
});
