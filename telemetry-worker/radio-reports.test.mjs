// Tests for the radio-report family (docs/radio-reports-contract.md): radio_fail and first_session.
//
//   node --test telemetry-worker/radio-reports.test.mjs      (ctest: telemetry_worker)
//
// The cases are the SHARED fixture files in test-fixtures/radio-reports/, which the site's tests and the app lane's tests load
// too. No wrangler, no account, no network: the Worker's fetch() is called with fake datasets and a fake global fetch standing
// in for the site.
//
// The break-it checks copy worker.js and radio-reports.js into a temporary folder, remove one behaviour from the copy, and
// assert that the same checks that pass on the real files FAIL on the copy.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';

import worker from './worker.js';
import * as rr from './radio-reports.js';

const here = fileURLToPath(new URL('.', import.meta.url));
const FIX = join(here, 'test-fixtures', 'radio-reports');

function loadFixtures() {
  return readdirSync(FIX).filter((n) => n.endsWith('.json')).sort()
    .map((n) => ({ file: n, ...JSON.parse(readFileSync(join(FIX, n), 'utf8')) }));
}
// The request text of a fixture, with its optional `expand` applied.
function requestText(fx) {
  const req = JSON.parse(JSON.stringify(fx.request));
  if (fx.expand) {
    const parts = fx.expand.path.split('.');
    let o = req;
    for (const p of parts.slice(0, -1)) { o = o[p]; }
    o[parts[parts.length - 1]] = 'a'.repeat(fx.expand.length);
  }
  return JSON.stringify(req);
}

// A world for one POST: the datasets, a stand-in for the site, and the answer the Worker gave.
async function send(mod, text, { site = { status: 204 }, relayToken, siteUrl = 'https://site.example.test/api/radiofail', noFirstSession = false } = {}) {
  const w = { usage: [], beats: [], first: [], counts: [], forwarded: [] };
  const env = {
    USAGE: { writeDataPoint: (p) => w.usage.push(p) },
    HEARTBEAT: { writeDataPoint: (p) => w.beats.push(p) },
    RADIOFAIL: { writeDataPoint: (p) => w.counts.push(p) },
  };
  if (!noFirstSession) { env.FIRSTSESSION = { writeDataPoint: (p) => w.first.push(p) }; }
  if (siteUrl) { env.SITE_INTAKE_URL = siteUrl; }
  if (relayToken) { env.RELAY_TOKEN = relayToken; }
  const realFetch = globalThis.fetch;
  globalThis.fetch = async (url, init) => {
    w.forwarded.push({ url, init });
    if (site === 'throw') { throw new Error('network down'); }
    return new Response(site.body || null, { status: site.status, headers: site.headers || {} });
  };
  let res;
  try {
    res = await (mod || worker).fetch(new Request('https://telemetry.example.test/', { method: 'POST', body: text }), env);
  } finally {
    globalThis.fetch = realFetch;
  }
  return { status: res.status, text: await res.text(), headers: res.headers, ...w };
}

// Checks every fixture against a Worker module; returns the list of failures (empty when all hold).
async function fixtureFailures(mod) {
  const bad = [];
  for (const fx of loadFixtures()) {
    const r = await send(mod, requestText(fx));
    const where = fx.file;
    if (r.status !== fx.status) { bad.push(`${where}: status ${r.status}, want ${fx.status}`); continue; }
    if (fx.expect === 'refuse') {
      if (r.text !== `refused: ${fx.code}`) { bad.push(`${where}: body "${r.text}", want refused: ${fx.code}`); }
      if (r.usage.length || r.first.length || r.counts.length || r.forwarded.length || r.beats.length) {
        bad.push(`${where}: a refused message wrote or forwarded something`);
      }
      continue;
    }
    if (r.usage.length) { bad.push(`${where}: an accepted radio-report message wrote a usage row`); }
    if (fx.request.beat === 1) {
      // the extended beat: the legacy beat row as ever, and the site is sent only the canonical radio_beat
      if (r.beats.length !== 1 || JSON.stringify(r.beats[0].blobs) !== JSON.stringify([fx.request.v])) { bad.push(`${where}: the beat row changed`); }
      if (r.forwarded.length !== 1) { bad.push(`${where}: forwarded ${r.forwarded.length} times`); continue; }
      try { assert.deepEqual(JSON.parse(r.forwarded[0].init.body), fx.canonical); } catch (e) { bad.push(`${where}: forwarded form differs from the canonical form`); }
    } else if (fx.request.kind !== 'first_session') {
      if (r.forwarded.length !== 1) { bad.push(`${where}: forwarded ${r.forwarded.length} times`); continue; }
      let sent;
      try { sent = JSON.parse(r.forwarded[0].init.body); } catch (e) { sent = null; }
      try { assert.deepEqual(sent, fx.canonical || fx.request); } catch (e) { bad.push(`${where}: forwarded form differs from the canonical form`); }
    } else {
      if (r.first.length !== 1) { bad.push(`${where}: ${r.first.length} first-session rows`); continue; }
      try { assert.deepEqual(r.first[0], fx.row); } catch (e) { bad.push(`${where}: the row differs from the fixture's`); }
    }
  }
  return bad;
}

// ---- the shared fixtures ------------------------------------------------------------------------------------------------

test('the fixture directory matches its manifest, so the site\'s copy can be checked against it', () => {
  const manifest = readFileSync(join(FIX, 'MANIFEST.sha256'), 'utf8').trim().split('\n');
  const names = readdirSync(FIX).filter((n) => n.endsWith('.json')).sort();
  assert.equal(manifest.length, names.length);
  for (const line of manifest) {
    const [sum, name] = line.split('  ');
    // the hash is of the file with CRLF turned into LF, so a Windows checkout (autocrlf) and a Linux one agree
    assert.equal(createHash('sha256').update(readFileSync(join(FIX, name), 'utf8').replace(/\r\n/g, '\n')).digest('hex'), sum, name);
  }
  assert.ok(names.length >= 80, `${names.length} fixtures`);
});

test('every fixture is accepted or refused exactly as it says, with the code it names', async () => {
  assert.deepEqual(await fixtureFailures(worker), []);
});

test('the refusal fixtures cover every refusal code of the contract, and the valid ones both kinds', () => {
  const fx = loadFixtures();
  const used = new Set(fx.filter((f) => f.expect === 'refuse').map((f) => f.code));
  const contract = readFileSync(join(here, '..', 'docs', 'radio-reports-contract.md'), 'utf8');
  const listed = contract.slice(contract.search(/## \d+\. Refusal codes/)).match(/`([a-z-]+)`/g).map((s) => s.slice(1, -1));
  for (const code of ['too-large', 'unknown-kind', 'reserved-kind', 'bad-schema', 'forbidden-field', 'bad-install-id',
    'missing-field', 'bad-field', 'key-mismatch', 'path-in-text', 'address-in-text', 'serial-in-text', 'frequency-in-text',
    'not-first-session']) {
    assert.ok(listed.includes(code), `${code} is in the contract`);
    assert.ok(used.has(code), `${code} has a refusal fixture`);
  }
  assert.ok(fx.some((f) => f.expect === 'accept' && f.request.kind === 'radio_fail'));
  assert.ok(fx.some((f) => f.expect === 'accept' && f.request.kind === 'first_session'));
});

test('the contract names the same drivers and reason classes as the failure vocabulary', () => {
  const text = readFileSync(join(here, 'worker.js'), 'utf8');
  const begin = text.indexOf('// BEGIN HEALTH VOCABULARY');
  const end = text.indexOf('// END HEALTH VOCABULARY');
  const vocab = JSON.parse(text.slice(text.indexOf('[', begin), text.lastIndexOf(']', end) + 1));
  const rf = vocab.find((e) => e.name === 'radio_fail');
  assert.deepEqual(rr.DRIVERS, rf.qualifiers[0]);
  assert.deepEqual(rr.REASON_CLASSES, rf.qualifiers[1]);
});

// ---- one usage row is one launch ----------------------------------------------------------------------------------------

function firstSessionText(over = {}) {
  const fx = loadFixtures().find((f) => f.file === 'valid-first-session-on-failure.json');
  const req = JSON.parse(JSON.stringify(fx.request));
  Object.assign(req.body, over);
  return JSON.stringify(req);
}
// The invariant, as a function so the break-it can run it on a broken copy: after any number of first-session
// records, the usage dataset (and the heartbeat dataset) hold nothing, and ONE ordinary usage record adds ONE row.
async function launchCountHolds(mod) {
  const w = { usage: [], beats: [], first: [] };
  const env = {
    USAGE: { writeDataPoint: (p) => w.usage.push(p) },
    HEARTBEAT: { writeDataPoint: (p) => w.beats.push(p) },
    FIRSTSESSION: { writeDataPoint: (p) => w.first.push(p) },
  };
  const post = (text) => mod.fetch(new Request('https://t.example.test/', { method: 'POST', body: text }), env);
  for (const trigger of ['after-minute', 'on-failure', 'on-failure']) {
    const res = await post(firstSessionText({ trigger }));
    if (res.status !== 204) { return false; }
  }
  if (w.usage.length !== 0 || w.beats.length !== 0 || w.first.length !== 3) { return false; }
  const usage = { id: 'a'.repeat(32), v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', launches: 1, sessionSec: 70 };
  await post(JSON.stringify(usage));
  return w.usage.length === 1 && w.first.length === 3;
}

test('first-session records never add a launch: three of them leave the usage dataset empty, one usage record adds one row', async () => {
  assert.equal(await launchCountHolds(worker), true);
});

test('a first-session row has the usage columns in the same places, plus trigger, session id, sequence and time', async () => {
  const r = await send(worker, firstSessionText());
  const u = (await send(worker, JSON.stringify({
    id: 'a1b2c3d4e5f60718293a4b5c6d7e8f90', v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', launches: 1, crashes: 0,
    sessionSec: 61, sdr: 'Airspy HF+', modes: { WFM: 40 }, panels: ['map'], plugins: ['ADS-B 1.0.0'], ch: 'installer',
    first: '2026-11-16', fv: '0.99.109', uch: 'stable', stalls: 0, health: 'radio_fail.airspyhf.other=1,radio_open.rtlsdr=1',
    exits_died: 0, exits_killed: 0, exits_ended: 0, exits_unknown: 0,
  }))).usage[0];
  assert.deepEqual(r.first[0].indexes, u.indexes);
  assert.deepEqual(r.first[0].blobs.slice(0, 15), u.blobs);
  assert.deepEqual(r.first[0].doubles.slice(0, 16), u.doubles);
  assert.equal(r.first[0].blobs.length, 17);
  assert.equal(r.first[0].doubles.length, 18);
});

test('the Worker that predates the contract (0.99.95) answers every valid message 400 "bad id" and writes nothing', async () => {
  const old = await import(pathToFileURL(join(FIX, '..', 'worker-0.99.95.js')).href);
  let n = 0;
  for (const fx of loadFixtures().filter((f) => f.expect === 'accept' && f.request.beat !== 1)) {
    const r = await send(old.default, requestText(fx));
    assert.equal(r.status, 400, fx.file);
    assert.equal(r.text, 'bad id', fx.file);
    assert.equal(r.usage.length + r.beats.length, 0, `${fx.file} counted as a launch or a beat on the old Worker`);
    n += 1;
  }
  assert.ok(n >= 25);
});

// ---- the radio_fail path ------------------------------------------------------------------------------------------------

const failText = () => requestText(loadFixtures().find((f) => f.file === 'valid-radio-fail-full.json'));

test('a radio failure is forwarded once, as JSON, with only a content type (and the relay secret when one is set)', async () => {
  const plain = await send(worker, failText());
  assert.equal(plain.status, 204);
  assert.equal(plain.forwarded.length, 1);
  assert.equal(plain.forwarded[0].url, 'https://site.example.test/api/radiofail');
  assert.deepEqual(plain.forwarded[0].init.headers, { 'content-type': 'application/json' });
  assert.equal(plain.forwarded[0].init.method, 'POST');
  const keyed = await send(worker, failText(), { relayToken: 'relay-secret-for-test' });
  assert.deepEqual(keyed.forwarded[0].init.headers, { 'content-type': 'application/json', 'x-foxsdr-relay': 'relay-secret-for-test' });
});

test('the counter row of a radio failure holds the driver, class, version and result - never the serial, model or text', async () => {
  const ok = await send(worker, failText());
  assert.deepEqual(ok.counts, [{
    indexes: ['a1b2c3d4e5f60718293a4b5c6d7e8f90'], blobs: ['airspyhf', 'other', '0.99.109', 'ok'], doubles: [1],
  }]);
  const flat = JSON.stringify(ok.counts);
  for (const secret of ['HF0001A2', 'Airspy', 'stopped answering', 'soapy:']) { assert.ok(!flat.includes(secret), secret); }
  const down = await send(worker, failText(), { site: { status: 500 } });
  assert.equal(down.counts[0].blobs[3], 'fail');
});

test('the site\'s answers map to what the app does next: 429 keeps its delay, 5xx and a dead network are 502, a site refusal is final', async () => {
  const limited = await send(worker, failText(), { site: { status: 429, headers: { 'retry-after': '120' } } });
  assert.equal(limited.status, 429);
  assert.equal(limited.headers.get('retry-after'), '120');
  const absurd = await send(worker, failText(), { site: { status: 429, headers: { 'retry-after': '99999999' } } });
  assert.equal(absurd.headers.get('retry-after'), '86400');
  assert.equal((await send(worker, failText(), { site: { status: 503 } })).status, 502);
  assert.equal((await send(worker, failText(), { site: 'throw' })).status, 502);
  const disagree = await send(worker, failText(), { site: { status: 400 } });
  assert.equal(disagree.status, 400);
  assert.equal(disagree.text, 'refused: site-refused');
});

test('with no site configured, or no first-session dataset, the Worker says 503 and does not pretend to have taken the message', async () => {
  assert.equal((await send(worker, failText(), { siteUrl: '' })).status, 503);
  assert.equal((await send(worker, firstSessionText(), { noFirstSession: true })).status, 503);
});

test('the Diagnostics switch does not exist at the Worker: the same failure is taken with diagOn true and false', async () => {
  for (const diagOn of [true, false]) {
    const req = JSON.parse(failText());
    req.body.diagOn = diagOn;
    assert.equal((await send(worker, JSON.stringify(req))).status, 204, `diagOn ${diagOn}`);
  }
  const source = readFileSync(join(here, 'worker.js'), 'utf8') + readFileSync(join(here, 'radio-reports.js'), 'utf8');
  assert.ok(!/request\.headers|cf-connecting-ip|request\.cf|x-forwarded-for/i.test(source.replace(/\/\/[^\n]*/g, '')), 'no connection-derived data is read');
});

test('a usage record and a beat are unchanged: no kind, same limits (4,096 bytes), same answers', async () => {
  const usage = { id: 'a'.repeat(32), v: '0.99.109', os: 'w', arch: 'x64', launches: 1, sessionSec: 5 };
  assert.equal((await send(worker, JSON.stringify(usage))).usage.length, 1);
  assert.equal((await send(worker, JSON.stringify({ id: 'a'.repeat(32), v: '1', beat: 1 }))).beats.length, 1);
  const big = { ...usage, pad: 'x'.repeat(5000) };
  assert.equal((await send(worker, JSON.stringify(big))).status, 413);
  assert.equal((await send(worker, 'x'.repeat(20000))).status, 413);
  assert.equal((await send(worker, '{not json')).status, 400);
});

// ---- the text rules, beyond the fixtures --------------------------------------------------------------------------------

test('the text rules refuse what they should and let the scrub\'s own output through', () => {
  const refused = [
    ['C:\\Program Files\\FoxSDR', 'path-in-text'], ['d:/x', 'path-in-text'], ['open /Users/bob/x', 'path-in-text'],
    ['\\\\Server\\share\\x', 'path-in-text'], ['at 8.8.8.8 port', 'address-in-text'], ['host 10.1.2.3', 'address-in-text'],
    ['serial: ABC123', 'serial-in-text'], ['S/N 99887766', 'serial-in-text'], ['SN=12345', 'serial-in-text'],
    ['airspyhf_sn:ABCDEF', 'serial-in-text'], ['serial number 0001', 'serial-in-text'],
    ['tuned 7074 kHz', 'frequency-in-text'], ['at 1.2GHz', 'frequency-in-text'], ['100 hz', 'frequency-in-text'],
  ];
  for (const [text, code] of refused) { assert.equal(rr.textCode(text, ''), code, text); }
  const fine = [
    'tcp://<host>:1234 refused', '<path>', 'serial <stripped>',
    'serial port COM# is busy', 'version 1.99.58.0', 'firmware 1.2.3.4', 'Windows 10.0.22631', 'FoxSDR_1.99.58.0_x64',
    '127.0.0.1 and 0.0.0.0', 'centre # MHz', 'a sample rate of 2.4 MS/s', 'R820T tuner, 28 dB', 'error 31', 'snapshot taken',
    'serialized state', 'the transaction number is 7',
  ];
  for (const text of fine) { assert.equal(rr.textCode(text, ''), '', text); }
  assert.equal(rr.textCode('the unit AB12-CD34 failed', 'ab12-cd34'), 'serial-in-text', 'the radio\'s own serial, any case');
  assert.equal(rr.textCode('abc failed', 'abc'), '', 'a serial shorter than four characters is not searched for');
});

test('the text rules give the verdicts of the shared parity table (the Go port on the site is held to the same file)', () => {
  const table = JSON.parse(readFileSync(join(FIX, '..', 'radio-text-rules.json'), 'utf8'));
  assert.ok(table.cases.length >= 500);
  for (const c of table.cases) { assert.equal(rr.textCode(c.text, c.serial), c.code, JSON.stringify(c.text)); }
  assert.ok(table.cases.some((c) => c.code === 'path-in-text') && table.cases.some((c) => c.code === 'address-in-text')
    && table.cases.some((c) => c.code === 'serial-in-text') && table.cases.some((c) => c.code === 'frequency-in-text'));
});

test('the radio key is the slug of the model and the upper-cased serial, a dash when there is none', () => {
  assert.equal(rr.radioKey('uhd b200', 'edr04zdb2'), 'uhd-b200:EDR04ZDB2');
  assert.equal(rr.radioKey('RTL-SDR', ''), 'rtl-sdr:-');
  assert.equal(rr.radioKey('Airspy HF+', 'HF0001A2'), 'airspy-hf:HF0001A2');
  assert.equal(rr.modelSlug('x'.repeat(50)).length, 40);
});

// ---- fuzz: the validator is total ---------------------------------------------------------------------------------------

test('fuzz: 4,000 random mutations of the valid fixtures never throw and always give a verdict', () => {
  let seed = 20261116;
  const rnd = () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; };
  const pick = (a) => { const v = a[Math.floor(rnd() * a.length)]; return v !== null && typeof v === 'object' ? structuredClone(v) : v; };
  const junk = [null, undefined, -1, 0, 1.5, 1e300, '', 'x', 'a'.repeat(500), [], {}, [1, 2], { a: 1 }, true, false, '<stripped>', 'C:\\x', '10.0.0.1'];
  const valid = loadFixtures().filter((f) => f.expect === 'accept').map((f) => f.request);
  const walk = (o, out = []) => {
    if (o !== null && typeof o === 'object') { for (const k of Object.keys(o)) { out.push([o, k]); walk(o[k], out); } }
    return out;
  };
  let verdicts = 0;
  for (let i = 0; i < 4000; i += 1) {
    const m = JSON.parse(JSON.stringify(pick(valid)));
    for (let j = 0, n = 1 + Math.floor(rnd() * 3); j < n; j += 1) {
      const slots = walk(m);
      const [o, k] = pick(slots);
      const r = rnd();
      if (r < 0.2) { delete o[k]; } else if (r < 0.3) { o[`k${i}`] = pick(junk); } else { o[k] = pick(junk); }
    }
    const v = rr.validateRadioMessage(m, { scrubModel: (s) => s });
    assert.equal(typeof v.ok, 'boolean');
    if (v.ok) { assert.ok(v.canonical && typeof v.canonical === 'object'); } else { assert.match(v.code, /^[a-z-]+$/); }
    verdicts += 1;
  }
  assert.equal(verdicts, 4000);
  for (const bad of [null, 5, 'x', [], [1]]) { assert.equal(rr.validateRadioMessage(bad, { scrubModel: (s) => s }).ok, false); }
});

// ---- break-it: remove one behaviour from a copy, and the same checks fail ------------------------------------------------

async function brokenCopy(edit) {
  const dir = mkdtempSync(join(tmpdir(), 'rr-break-'));
  try {
    let w = readFileSync(join(here, 'worker.js'), 'utf8');
    let r = readFileSync(join(here, 'radio-reports.js'), 'utf8');
    ({ w, r } = edit(w, r));
    writeFileSync(join(dir, 'worker.js'), w);
    writeFileSync(join(dir, 'radio-reports.js'), r);
    writeFileSync(join(dir, 'radio-journal-spec.js'), readFileSync(join(here, 'radio-journal-spec.js'), 'utf8'));
    return (await import(pathToFileURL(join(dir, 'worker.js')).href)).default;
  } finally {
    // the module is loaded; the files can go
    rmSync(dir, { recursive: true, force: true });
  }
}
const mustChange = (text, from, to) => {
  assert.ok(text.includes(from), `the break-it's target text is in the source: ${from}`);
  return text.replace(from, to);
};

test('BREAK-IT: if a first-session record is written to the usage dataset, the launch-count proof goes red', async () => {
  const broken = await brokenCopy((w, r) => ({ w: mustChange(w, 'env.FIRSTSESSION.writeDataPoint(', 'env.USAGE.writeDataPoint('), r }));
  assert.equal(await launchCountHolds(broken), false);
  assert.ok((await fixtureFailures(broken)).length > 0);
});

test('BREAK-IT: if the forbidden-field check is removed, the frequency fixtures go red', async () => {
  const broken = await brokenCopy((w, r) => ({ w, r: mustChange(r, "if (hasForbiddenKey(msg)) { return refuse('forbidden-field'); }", '') }));
  const bad = await fixtureFailures(broken);
  assert.ok(bad.some((b) => b.startsWith('refuse-frequency-field.json')), bad.join('\n'));
});

test('BREAK-IT: if the serial rule is removed, the unscrubbed-serial fixtures go red', async () => {
  const broken = await brokenCopy((w, r) => ({ w, r: mustChange(r, "if (serialInText(s, ownSerial)) { return 'serial-in-text'; }", '') }));
  const bad = await fixtureFailures(broken);
  assert.ok(bad.some((b) => b.startsWith('refuse-serial-in-reason.json')), bad.join('\n'));
  assert.ok(bad.some((b) => b.startsWith('refuse-serial-key-in-reason.json')), bad.join('\n'));
});

test('BREAK-IT: if the path rule is removed, the path fixtures go red', async () => {
  const broken = await brokenCopy((w, r) => ({ w, r: mustChange(r, "if (pathInText(s)) { return 'path-in-text'; }", '') }));
  assert.ok((await fixtureFailures(broken)).some((b) => b.startsWith('refuse-path-in-reason.json')));
});

test('a message that uses the usage record field `id` instead of `installId` is refused and writes nothing', async () => {
  const req = JSON.parse(failText());
  req.id = req.installId;
  delete req.installId;
  const r = await send(worker, JSON.stringify(req));
  assert.equal(r.status, 400);
  assert.equal(r.text, 'refused: missing-field');
  assert.equal(r.usage.length + r.first.length + r.forwarded.length, 0);
});

// ============================================================================================================================
// SCHEMA 2: the journal kinds, the install with Usage reporting off, the beat's radios, the window word
// ============================================================================================================================

const fixtureOf = (kind) => loadFixtures().find((f) => f.expect === 'accept' && f.request.kind === kind);
const JOURNAL = ['radio_found', 'radio_open', 'radio_settings', 'radio_stream', 'radio_recovery', 'radio_close', 'session_env'];

test('the JSON copy of the journal\'s field tables is the table the Worker reads (the site embeds the copy)', async () => {
  const { SPEC } = await import('./radio-journal-spec.js');
  assert.deepEqual(JSON.parse(readFileSync(join(FIX, '..', 'radio-journal-spec.json'), 'utf8')), JSON.parse(JSON.stringify(SPEC)));
});

test('the contract names every kind and every field of the tables, so the document cannot drift from what the Worker reads', async () => {
  const { KIND_SPECS } = await import('./radio-journal-spec.js');
  const contract = readFileSync(join(here, '..', 'docs', 'radio-reports-contract.md'), 'utf8');
  for (const [kind, spec] of Object.entries(KIND_SPECS)) {
    assert.ok(contract.includes(`\`${kind}\``), `${kind} is in the contract`);
    for (const f of spec.fields) { assert.ok(contract.includes(`\`${f.name}\``), `${kind}.${f.name} is in the contract`); }
  }
});

test('every journal kind is accepted from its valid fixture, forwarded once to the site, and writes no usage row, no first-session row and no failure counter', async () => {
  for (const kind of JOURNAL) {
    const fx = fixtureOf(kind);
    assert.ok(fx, `a valid fixture of ${kind}`);
    const r = await send(worker, requestText(fx));
    assert.equal(r.status, 204, kind);
    assert.equal(r.forwarded.length, 1, kind);
    assert.equal(r.usage.length + r.first.length + r.counts.length + r.beats.length, 0, kind);
    assert.equal(JSON.parse(r.forwarded[0].init.body).kind, kind);
  }
});

test('every journal kind refuses every forbidden field name, at the top and inside the body (and the Worker sends nothing on)', async () => {
  const one = JSON.parse(requestText(fixtureOf('radio_open')));
  one.body.fileName = 'x';
  const through = await send(worker, JSON.stringify(one));
  assert.equal(through.text, 'refused: forbidden-field');
  assert.equal(through.forwarded.length, 0);
  const names = ['freq', 'centreFreq', 'tunedMhz', 'bookmark', 'latitude', 'gps', 'ipAddr', 'hostName', 'userName', 'fileName', 'folder',
    'decodedText', 'callsign', 'password', 'secret', 'lat', 'lon', 'ip', 'user', 'path', 'file', 'token', 'bandHz'];
  for (const kind of JOURNAL) {
    for (const name of names) {
      for (const where of ['top', 'body']) {
        const req = JSON.parse(requestText(fixtureOf(kind)));
        (where === 'top' ? req : req.body)[name] = 'x';
        const v = rr.validateRadioMessage(req, { scrubModel: (m) => m });
        assert.deepEqual([v.ok, v.code], [false, 'forbidden-field'], `${kind} ${where} ${name}`);
      }
    }
  }
});

test('every free-text field of every journal kind refuses a path, an address, its own serial and a hertz number', async () => {
  const { KIND_SPECS } = await import('./radio-journal-spec.js');
  const bad = [['C:\\Users\\bob\\x.wav', 'path-in-text'], ['/home/bob/x', 'path-in-text'], ['seen at 10.20.30.40', 'address-in-text'],
    ['tuned to 7074 kHz', 'frequency-in-text'], ['at 3.7GHz', 'frequency-in-text']];
  let checked = 0;
  for (const [kind, spec] of Object.entries(KIND_SPECS)) {
    for (const f of spec.fields.filter((x) => x.type === 'text' || (x.type === 'arr' && x.item.type === 'text'))) {
      for (const [text, code] of bad) {
        const req = JSON.parse(requestText(fixtureOf(kind)));
        req.body[f.name] = f.type === 'arr' ? [text] : text;
        const r = await send(worker, JSON.stringify(req));
        assert.equal(r.text, `refused: ${code}`, `${kind}.${f.name} ${text}`);
        checked += 1;
      }
    }
    if (spec.radio) {
      const f = spec.fields.find((x) => x.type === 'text');
      if (f) {
        const req = JSON.parse(requestText(fixtureOf(kind)));
        req.body[f.name] = req.body.serial;
        assert.equal((await send(worker, JSON.stringify(req))).text, 'refused: serial-in-text', `${kind}.${f.name} own serial`);
        checked += 1;
      }
    }
  }
  assert.ok(checked >= 40, `${checked} text checks`);
});

test('a journal event over 4,096 bytes is refused as too large, whatever it is padded with', async () => {
  const req = JSON.parse(requestText(fixtureOf('radio_stream')));
  req.pad = 'x'.repeat(4200);
  const r = await send(worker, JSON.stringify(req));
  assert.equal(r.status, 413);
  assert.equal(r.text, 'refused: too-large');
});

test('a radio failure from an install with Usage reporting off carries no install id, no session id and no sequence number, and is counted under "anonymous"', async () => {
  const anon = loadFixtures().find((f) => f.file === 'valid-radio-fail-anonymous.json');
  const r = await send(worker, requestText(anon));
  assert.equal(r.status, 204);
  const sent = JSON.parse(r.forwarded[0].init.body);
  for (const linking of ['installId', 'sessionId', 'seq', 'id']) { assert.ok(!(linking in sent), `${linking} is not forwarded`); }
  assert.equal(sent.messageId, 'f00dfacecafe0123');
  assert.deepEqual(r.counts, [{ indexes: ['anonymous'], blobs: ['rtlsdr', 'busy', '0.99.109', 'ok'], doubles: [1] }]);
  // and no kind but radio_fail may do without an install id
  for (const kind of JOURNAL) {
    const req = JSON.parse(requestText(fixtureOf(kind)));
    delete req.installId;
    assert.equal((await send(worker, JSON.stringify(req))).text, 'refused: missing-field', kind);
  }
});

test('two anonymous failures of one radio share nothing but the radio: the second message differs only in its random id, time and attempt', async () => {
  const a = JSON.parse(requestText(loadFixtures().find((f) => f.file === 'valid-radio-fail-anonymous.json')));
  const b = JSON.parse(JSON.stringify(a));
  b.messageId = 'f00dfacecafe0999';
  b.body.attempt = 2;
  const ra = JSON.parse((await send(worker, JSON.stringify(a))).forwarded[0].init.body);
  const rb = JSON.parse((await send(worker, JSON.stringify(b))).forwarded[0].init.body);
  const common = Object.keys(ra).filter((k) => JSON.stringify(ra[k]) === JSON.stringify(rb[k]));
  assert.deepEqual(common.sort(), ['kind', 'radioKey', 'schema', 't'].sort());
  assert.deepEqual(Object.keys(ra.body).filter((k) => JSON.stringify(ra.body[k]) !== JSON.stringify(rb.body[k])), ['attempt']);
});

test('the running-now beat: unchanged without radios; with radios the beat row is still the id and the version, and the site is sent the keys', async () => {
  const plain = await send(worker, JSON.stringify({ id: 'a'.repeat(32), v: '0.99.109', beat: 1 }));
  assert.equal(plain.status, 204);
  assert.equal(plain.forwarded.length, 0);
  assert.deepEqual(plain.beats, [{ indexes: ['a'.repeat(32)], blobs: ['0.99.109'] }]);
  const named = await send(worker, JSON.stringify({ id: 'a'.repeat(32), v: '0.99.109', beat: 1, radios: ['airspy-hf:HF0001A2'] }));
  assert.equal(named.status, 204);
  assert.deepEqual(named.beats, plain.beats, 'the beat row holds no radio');
  assert.deepEqual(JSON.parse(named.forwarded[0].init.body), { schema: 2, kind: 'radio_beat', installId: 'a'.repeat(32), v: '0.99.109', radios: ['airspy-hf:HF0001A2'] });
  const down = await send(worker, JSON.stringify({ id: 'a'.repeat(32), v: '0.99.109', beat: 1, radios: ['airspy-hf:HF0001A2'] }), { site: 'throw' });
  assert.equal(down.status, 204, 'a site that cannot be reached does not cost the running-now count');
  assert.equal(down.beats.length, 1);
  const noSite = await send(worker, JSON.stringify({ id: 'a'.repeat(32), v: '0.99.109', beat: 1, radios: ['airspy-hf:HF0001A2'] }), { siteUrl: '' });
  assert.equal(noSite.status, 204);
  assert.equal(noSite.forwarded.length, 0);
});

test('a beat names radios only as keys of the right shape, at most eight, and carries nothing else of a report', async () => {
  const id = 'a'.repeat(32);
  for (const radios of [[], 'airspy-hf:HF0001A2', ['98600000'], ['a b:c'], [5], ['x:y/z'], Array.from({ length: 9 }, (_, i) => `r${i}:S${i}`)]) {
    const r = await send(worker, JSON.stringify({ id, v: '1', beat: 1, radios }));
    assert.equal(r.status, 400, JSON.stringify(radios));
    assert.equal(r.text, 'refused: bad-field');
    assert.equal(r.beats.length + r.forwarded.length, 0, 'a refused beat writes and forwards nothing');
  }
  const r = await send(worker, JSON.stringify({ id, v: '1', beat: 1, radios: ['a:B'], health: 'radio_fail.rtlsdr.busy=1', crashes: 3 }));
  assert.deepEqual(r.beats, [{ indexes: [id], blobs: ['1'] }]);
  assert.equal(r.usage.length, 0);
  assert.deepEqual(JSON.parse(r.forwarded[0].init.body), { schema: 2, kind: 'radio_beat', installId: id, v: '1', radios: ['a:B'] });
});

test('the Worker deployed today (0.99.95) answers a beat that names radios 204 and writes only the id and the version', async () => {
  const old = await import(pathToFileURL(join(FIX, '..', 'worker-0.99.95.js')).href);
  const r = await send(old.default, JSON.stringify({ id: 'a'.repeat(32), v: '0.99.109', beat: 1, radios: ['airspy-hf:HF0001A2'] }));
  assert.equal(r.status, 204);
  assert.deepEqual(r.beats, [{ indexes: ['a'.repeat(32)], blobs: ['0.99.109'] }]);
  assert.equal(r.forwarded.length, 0);
});

test('every kind of the family, sent to the Worker deployed today, is answered 400 "bad id" and writes nothing (so the Worker goes live first)', async () => {
  const old = await import(pathToFileURL(join(FIX, '..', 'worker-0.99.95.js')).href);
  const seen = new Set();
  for (const fx of loadFixtures().filter((f) => f.expect === 'accept' && f.request.beat !== 1)) {
    const r = await send(old.default, requestText(fx));
    assert.equal(r.status, 400, fx.file);
    assert.equal(r.text, 'bad id');
    assert.equal(r.usage.length + r.beats.length, 0);
    seen.add(fx.request.kind);
  }
  assert.deepEqual([...seen].sort(), ['first_session', 'radio_close', 'radio_fail', 'radio_found', 'radio_open', 'radio_recovery', 'radio_settings', 'radio_stream', 'session_env']);
});

test('the window word (site lane 2, view f): `new` or `legacy` becomes blob16 of a usage record; anything else, or nothing, leaves the 15 blobs as they were', async () => {
  const base = { id: 'a'.repeat(32), v: '0.99.109', os: 'w', arch: 'x64', launches: 1, sessionSec: 5 };
  const blobs = async (extra) => (await send(worker, JSON.stringify({ ...base, ...extra }))).usage[0].blobs;
  assert.equal((await blobs({ window: 'new' }))[15], 'new');
  assert.equal((await blobs({ window: 'legacy' }))[15], 'legacy');
  assert.equal((await blobs({})).length, 15);
  for (const w of ['NEW', 'both', '', 5, null, ['new'], { a: 1 }]) { assert.equal((await blobs({ window: w })).length, 15, JSON.stringify(w)); }
  const beat = await send(worker, JSON.stringify({ id: base.id, v: '1', beat: 1, window: 'new' }));
  assert.deepEqual(beat.beats, [{ indexes: [base.id], blobs: ['1'] }]);
  const fs = JSON.parse(requestText(fixtureOf('first_session')));
  fs.body.window = 'new';
  const r = await send(worker, JSON.stringify(fs));
  assert.equal(r.first[0].blobs.length, 17, 'a first-session record never carries the window word: its columns are as the contract says');
});

test('BREAK-IT: if the anonymous failure may carry a session id, the linking fixtures go red', async () => {
  const broken = await brokenCopy((w, r) => ({ w, r: mustChange(r, "if (msg.sessionId !== undefined || msg.seq !== undefined) { return refuse('bad-field'); }", '') }));
  const bad = await fixtureFailures(broken);
  assert.ok(bad.some((b) => b.startsWith('refuse-anonymous-with-session-id.json')), bad.join('\n'));
  assert.ok(bad.some((b) => b.startsWith('refuse-anonymous-with-seq.json')), bad.join('\n'));
});

test('BREAK-IT: if the text rules are not applied to the journal kinds, the journal text fixtures go red', async () => {
  const broken = await brokenCopy((w, r) => ({ w, r: mustChange(r, 'for (const text of ctx.texts) {', 'for (const text of []) {') }));
  const bad = await fixtureFailures(broken);
  for (const name of ['refuse-journal-path-in-reason', 'refuse-journal-address-in-gpu', 'refuse-journal-serial-in-recovery-reason', 'refuse-journal-hertz-in-cpu']) {
    assert.ok(bad.some((b) => b.startsWith(`${name}.json`)), `${name}: ${bad.join('\n')}`);
  }
});

test('BREAK-IT: if the beat\'s radio keys are not checked, the beat fixtures go red', async () => {
  const broken = await brokenCopy((w, r) => ({ w, r: mustChange(r, "if (typeof k !== 'string' || !BEAT_KEY.test(k)) { return refuse('bad-field'); }", '') }));
  const bad = await fixtureFailures(broken);
  assert.ok(bad.some((b) => b.startsWith('refuse-beat-with-a-frequency.json')), bad.join('\n'));
});

test('BREAK-IT: if the window word is written without being checked, the window test goes red', async () => {
  const broken = await brokenCopy((w, r) => ({ w: mustChange(w, "return v === 'new' || v === 'legacy' ? v : '';", "return typeof v === 'string' ? v : '';"), r }));
  const base = { id: 'a'.repeat(32), v: '0.99.109', os: 'w', arch: 'x64', launches: 1, sessionSec: 5, window: 'both' };
  const p = (await send(broken, JSON.stringify(base))).usage[0];
  assert.equal(p.blobs.length, 16, 'the broken copy stores a word that is not one of the two');
});

test('BREAK-IT: if the journal kinds are not size-limited, the oversized fixture goes red', async () => {
  const broken = await brokenCopy((w, r) => ({ w: mustChange(w, ": body.kind === 'radio_fail' ? MAX_RADIO_FAIL_BODY : MAX_JOURNAL_BODY;", ': MAX_RADIO_FAIL_BODY;'), r }));
  const bad = await fixtureFailures(broken);
  assert.ok(bad.some((b) => b.startsWith('refuse-journal-oversized.json')), bad.join('\n'));
});
