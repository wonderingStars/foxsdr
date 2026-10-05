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
import { fileURLToPath } from 'node:url';

import worker from './worker.js';

const here = fileURLToPath(new URL('.', import.meta.url));
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
      if (/count\(\) AS reports/.test(sql)) {
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
