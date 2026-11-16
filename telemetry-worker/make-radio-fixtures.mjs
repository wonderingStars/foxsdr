// Writes the shared radio-report fixtures of schema 2 (docs/radio-reports-contract.md section 9) and their manifest, and the JSON
// copy of the journal's field tables:
//
//   node telemetry-worker/make-radio-fixtures.mjs
//
// Part 1's schema 1 fixtures are read from the folder, moved to schema 2 (the changes are the ones the contract lists), and the
// new ones are added. The `canonical` and `row` of an accepted case are written HERE, by hand, from the contract: they are not
// produced by the validator, so a fixture cannot agree with a validator that is wrong. Every id, serial, model and text is invented.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import { createHash } from 'node:crypto';
import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { SPEC } from './radio-journal-spec.js';

const here = fileURLToPath(new URL('.', import.meta.url));
const DIR = join(here, 'test-fixtures', 'radio-reports');
const clone = (o) => JSON.parse(JSON.stringify(o));
const write = (name, fx) => writeFileSync(join(DIR, `${name}.json`), `${JSON.stringify({ name, ...fx }, null, 2)}\n`);

const INSTALL = 'a1b2c3d4e5f60718293a4b5c6d7e8f90';
const SESSION = '0123456789abcdef';
const KEY = 'airspy-hf:HF0001A2';
const radio = { driver: 'airspyhf', model: 'Airspy HF+', serial: 'HF0001A2' };

// ---- part 1's fixtures, moved to schema 2 ---------------------------------------------------------------------------------
for (const file of readdirSync(DIR).filter((n) => n.endsWith('.json'))) {
  const fx = JSON.parse(readFileSync(join(DIR, file), 'utf8'));
  if (fx.request.schema === 1) { fx.request.schema = 2; }
  if (fx.canonical && fx.canonical.schema === 1) { fx.canonical.schema = 2; }
  if (fx.name === 'refuse-bad-schema') {
    fx.about = 'a schema other than 2 (part 1\'s schema 1 is refused: nothing of it was ever deployed)';
    fx.request.schema = 1;
  }
  if (fx.name === 'refuse-reserved-kind') {
    fx.about = 'a kind named for a later schema (a batched envelope), not defined yet';
    fx.request = { schema: 2, kind: 'radio_batch', installId: INSTALL, sessionId: SESSION, seq: 3, t: 412, messages: [] };
  }
  if (fx.request.kind === 'radio_fail' && fx.canonical === undefined) { /* the request is its own canonical form */ }
  writeFileSync(join(DIR, file), `${JSON.stringify(fx, null, 2)}\n`);
}

// ---- builders --------------------------------------------------------------------------------------------------------------
const env = (kind, seq, t, body, over = {}) => ({
  schema: 2, kind, installId: INSTALL, sessionId: SESSION, seq, t, radioKey: KEY, body, ...over,
});
const base = { ...radio, firmware: '3.1.0', library: 'libairspyhf 1.6.8', v: '0.99.109' };
const valid = (name, about, request, extra = {}) => write(name, { about, expect: 'accept', status: 204, request, ...extra });
const refuse = (name, about, code, request, extra = {}) => write(name, { about, expect: 'refuse', code, status: code === 'too-large' ? 413 : 400, request, ...extra });

// ---- valid: the six kinds -------------------------------------------------------------------------------------------------
valid('valid-radio-found-usable', 'a radio found by the start-up scan, its driver bound',
  env('radio_found', 4, 2, { ...base, binding: 'usable', source: 'scan', usbId: '03eb:800c', usbHost: 'Intel USB 3.1 eXtensible Host Controller', dropped: 0 }));
valid('valid-radio-found-wrong-driver', 'the first-run check: a known radio is present and no usable driver binds it (C3), serial unknown',
  env('radio_found', 5, 3, { driver: 'rtlsdr', model: 'RTL-SDR', serial: '', v: '0.99.109', binding: 'wrong-driver', source: 'first-run-check', usbId: '0bda:2838' }, { radioKey: 'rtl-sdr:-' }));
valid('valid-radio-open-start', 'an open began',
  env('radio_open', 6, 5, { ...base, phase: 'start', trigger: 'startup-restore', attempt: 1, sampleRateSps: 768000, band: 'fm-broadcast' }));
valid('valid-radio-open-ok', 'an open succeeded, with how long it took',
  env('radio_open', 7, 6, { ...base, phase: 'ok', trigger: 'startup-restore', attempt: 1, openMs: 910, sampleRateSps: 768000, band: 'fm-broadcast' }));
valid('valid-radio-settings-full', 'every setting the host applies after an open',
  env('radio_settings', 8, 7, { ...base, why: 'open', sampleRateSps: 768000, bandwidth: 600000, gainMode: 'manual', gainDb: 12.5, ppm: -1.5, biasTee: false, antenna: 'RX2', directSampling: 'off', decimation: 4, band: 'fm-broadcast' }));
valid('valid-radio-settings-refused', 'a setting the radio refused (the G3 control-refused path)',
  env('radio_settings', 9, 8, { ...base, why: 'user', ppm: 40, refused: 'ppm', reason: 'the radio refused the frequency correction: control transfer failed (Windows error 31)' }));
valid('valid-radio-stream-healthy', 'a minute of stream health with modes and plugins attached',
  env('radio_stream', 10, 66, { ...base, windowSec: 60, samplesPerSec: 767940, expectedSps: 768000, drops: 0, ringHighPct: 12, usbErrors: 0, stalls: 0, dspLoadPct: 21, openSec: 60, modes: ['WFM', 'NFM'], plugins: ['adsb-decoder', 'rds'], pluginFaults: 0 }));
valid('valid-radio-stream-no-samples', 'opened with no samples arriving, and the user said it sounds wrong',
  env('radio_stream', 11, 126, { ...base, windowSec: 60, samplesPerSec: 0, expectedSps: 768000, drops: 0, usbErrors: 3, stalls: 1, note: 'sounds-wrong', openSec: 120 }, { age: 4 }));
valid('valid-radio-recovery-start', 'a recovery began (the application\'s own recovered word)',
  env('radio_recovery', 12, 130, { ...base, phase: 'start', what: 'usbstuck', attempt: 1, reasonClass: 'timeout', reason: 'the stream stopped for 5 s' }));
valid('valid-radio-recovery-gave-up', 'a recovery that gave up',
  env('radio_recovery', 13, 140, { ...base, phase: 'gave-up', what: 'reopen', attempt: 3, reasonClass: 'driver', ms: 15000 }));
valid('valid-radio-close-died', 'a radio that died mid-session',
  env('radio_close', 14, 150, { ...base, cause: 'died', openSec: 144, reason: 'the radio stopped answering: a control transfer failed (Windows error 31)' }));
valid('valid-radio-close-user', 'a clean close, with the dropped count the daily bound carried',
  env('radio_close', 15, 3600, { ...base, cause: 'user', openSec: 3594, dropped: 42 }));
valid('valid-session-env-full', 'the once-a-session environment record (the D5 list)',
  env('session_env', 2, 1, {
    v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', ch: 'installer', uch: 'stable', window: 'new', launches: 7,
    cpu: 'Intel Core i7-8700K', cpuThreads: 12, ramMb: 32768, gpu: 'NVIDIA GeForce GTX 1060', gpuDriver: '552.44',
    usbHosts: ['Intel USB 3.1 eXtensible Host Controller', 'ASMedia USB 3.1 Host Controller'], audioApi: 'wasapi', audioClass: 'speakers',
    displayCount: 2, displayScalePct: [100, 150], diagOn: true,
  }, { radioKey: '' }));
valid('valid-session-env-minimal', 'the required fields only',
  env('session_env', 2, 1, { v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', launches: 1, cpu: 'AMD Ryzen 5 3600', audioApi: 'mme', diagOn: false }, { radioKey: '' }));
valid('valid-journal-drops-unknown-key', 'a journal event from a newer application: the field this schema does not know is dropped, the rest kept',
  env('radio_open', 7, 6, { ...base, phase: 'ok', trigger: 'user-pick', attempt: 2, openMs: 100, colourOfTheCase: 'grey' }),
  { canonical: env('radio_open', 7, 6, { ...base, phase: 'ok', trigger: 'user-pick', attempt: 2, openMs: 100 }) });

// ---- valid: a radio failure from an install with Usage reporting OFF (no install id; schema 2) --------------------------------
const FAILKEY = 'rtl-sdr:-';
const minimal = JSON.parse(readFileSync(join(DIR, 'valid-radio-fail-minimal.json'), 'utf8'));
const failBody = clone(minimal.request.body);
const failCanonicalBody = clone(minimal.canonical.body);
valid('valid-radio-fail-anonymous', 'a radio failure from an install with Usage reporting off: no install id, no session id, no sequence number, a random per-message id',
  { schema: 2, kind: 'radio_fail', messageId: 'f00dfacecafe0123', t: 412, radioKey: FAILKEY, body: { ...failBody, diagOn: false } },
  { canonical: { schema: 2, kind: 'radio_fail', messageId: 'f00dfacecafe0123', t: 412, radioKey: FAILKEY, body: { ...failCanonicalBody, diagOn: false } } });
valid('valid-radio-fail-anonymous-aged', 'the same, sent from the queue two hours after it happened (age), with the dropped count',
  { schema: 2, kind: 'radio_fail', messageId: 'f00dfacecafe0124', t: 12, age: 7200, radioKey: FAILKEY, body: { ...failBody, attempt: 4, dropped: 3 } },
  { canonical: { schema: 2, kind: 'radio_fail', messageId: 'f00dfacecafe0124', t: 12, age: 7200, radioKey: FAILKEY, body: { ...failCanonicalBody, attempt: 4, dropped: 3 } } });

// ---- valid: the running-now beat, as the app sends it (the Worker's legacy shape) and as the site is sent it ----------------------
const beatCanonical = { schema: 2, kind: 'radio_beat', installId: INSTALL, v: '0.99.109', radios: [KEY, 'rtl-sdr:-'] };
write('valid-beat-with-radios', {
  about: 'the extended beat: the legacy beat plus the keys of the radios open now. `request` is what the app sends the Worker, `canonical` what the Worker forwards to the site (the site is sent only that)',
  expect: 'accept', status: 204, request: { id: INSTALL, v: '0.99.109', beat: 1, radios: [KEY, 'rtl-sdr:-', KEY] }, canonical: beatCanonical,
});

// ---- refused ---------------------------------------------------------------------------------------------------------------
const open = () => env('radio_open', 7, 6, { ...base, phase: 'ok', trigger: 'startup-restore', attempt: 1, openMs: 910 });
const withBody = (kindMsg, edit) => { const m = clone(kindMsg); edit(m.body, m); return m; };
refuse('refuse-journal-frequency-field', 'a centre frequency as a field of a settings event', 'forbidden-field',
  withBody(env('radio_settings', 8, 7, { ...base, why: 'open', sampleRateSps: 768000 }), (b) => { b.centreFrequency = 98600000; }));
refuse('refuse-journal-tuned-key-in-stream', 'a tuned frequency under another name, in a stream event', 'forbidden-field',
  withBody(env('radio_stream', 10, 66, { ...base, windowSec: 60, samplesPerSec: 1, drops: 0 }), (b) => { b.tunedKhz = 98600; }));
refuse('refuse-journal-file-name', 'a recording\'s file name as a field', 'forbidden-field',
  withBody(open(), (b) => { b.fileName = 'morning-capture.wav'; }));
refuse('refuse-journal-position', 'a position as a field of the environment record', 'forbidden-field',
  withBody(env('session_env', 2, 1, { v: '0.99.109', os: 'Windows', arch: 'x64', launches: 1, cpu: 'x', audioApi: 'mme', diagOn: true }, { radioKey: '' }), (b) => { b.latitude = 51.5; }));
refuse('refuse-journal-decoded-text', 'decoded content as a field of a stream event', 'forbidden-field',
  withBody(env('radio_stream', 10, 66, { ...base, windowSec: 60, samplesPerSec: 1, drops: 0 }), (b) => { b.decodedText = 'CQ CQ'; }));
refuse('refuse-journal-path-key', 'a path as a field', 'forbidden-field', withBody(open(), (b) => { b.path = 'C:\\x'; }));
refuse('refuse-journal-path-in-reason', 'a path in a close reason', 'path-in-text',
  withBody(env('radio_close', 14, 150, { ...base, cause: 'error', openSec: 1 }), (b) => { b.reason = 'could not write C:\\Users\\bob\\capture.wav'; }));
refuse('refuse-journal-address-in-gpu', 'an address in the GPU name', 'address-in-text',
  withBody(env('session_env', 2, 1, { v: '0.99.109', os: 'Windows', arch: 'x64', launches: 1, cpu: 'x', audioApi: 'mme', diagOn: true, gpu: 'remote 10.1.2.3' }, { radioKey: '' }), () => {}));
refuse('refuse-journal-serial-in-recovery-reason', 'the radio\'s own serial in a recovery reason', 'serial-in-text',
  withBody(env('radio_recovery', 12, 130, { ...base, phase: 'start', what: 'reopen', attempt: 1 }), (b) => { b.reason = 'the unit HF0001A2 did not answer'; }));
refuse('refuse-journal-hertz-in-cpu', 'a clock speed in the CPU name (the sender strips "@ 3.70GHz"; a hertz number is a hertz number)', 'frequency-in-text',
  withBody(env('session_env', 2, 1, { v: '0.99.109', os: 'Windows', arch: 'x64', launches: 1, cpu: 'Intel Core i7 @ 3.70GHz', audioApi: 'mme', diagOn: true }, { radioKey: '' }), () => {}));
refuse('refuse-journal-band-is-a-number', 'a band given as a number, not one of the names', 'bad-field',
  withBody(open(), (b) => { b.band = '98600000'; }));
refuse('refuse-journal-key-mismatch', 'a radio key that is not the body\'s model and serial', 'key-mismatch', { ...open(), radioKey: 'airspy-hf:OTHER123' });
refuse('refuse-journal-bad-enum', 'a binding that is not one of the words', 'bad-field',
  withBody(env('radio_found', 4, 2, { ...base, binding: 'sort-of', source: 'scan' }), () => {}));
refuse('refuse-journal-missing-required', 'a stream event without its drops', 'missing-field',
  withBody(env('radio_stream', 10, 66, { ...base, windowSec: 60, samplesPerSec: 1 }), () => {}));
refuse('refuse-journal-without-install-id', 'a journal event with no install id (only a radio failure may be sent without one)', 'missing-field',
  (() => { const m = open(); delete m.installId; return m; })());
refuse('refuse-session-env-with-radio-key', 'the environment record names a radio', 'bad-field', { ...env('session_env', 2, 1, { v: '0.99.109', os: 'Windows', arch: 'x64', launches: 1, cpu: 'x', audioApi: 'mme', diagOn: true }), radioKey: KEY });
refuse('refuse-journal-oversized', 'a journal event over 4,096 bytes', 'too-large', open(), { expand: { path: 'body.library', length: 5000 } });
refuse('refuse-journal-too-many-plugins', 'seventeen plugins in a stream event', 'bad-field',
  withBody(env('radio_stream', 10, 66, { ...base, windowSec: 60, samplesPerSec: 1, drops: 0 }), (b) => { b.plugins = Array.from({ length: 17 }, (_, i) => `plug-${i}`); }));
refuse('refuse-anonymous-with-session-id', 'a failure with no install id that still carries a session id: that would link its reports', 'bad-field',
  { schema: 2, kind: 'radio_fail', messageId: 'f00dfacecafe0125', sessionId: SESSION, t: 4, radioKey: FAILKEY, body: { ...failBody } });
refuse('refuse-anonymous-with-seq', 'a failure with no install id that still carries a sequence number', 'bad-field',
  { schema: 2, kind: 'radio_fail', messageId: 'f00dfacecafe0125', seq: 4, t: 4, radioKey: FAILKEY, body: { ...failBody } });
refuse('refuse-anonymous-without-message-id', 'a failure with no install id and no message id', 'missing-field',
  { schema: 2, kind: 'radio_fail', t: 4, radioKey: FAILKEY, body: { ...failBody } });
refuse('refuse-identified-with-message-id', 'a failure with an install id must not also carry a message id', 'bad-field',
  { schema: 2, kind: 'radio_fail', installId: INSTALL, sessionId: SESSION, seq: 1, messageId: 'f00dfacecafe0125', t: 4, radioKey: FAILKEY, body: { ...failBody } });
refuse('refuse-beat-with-nine-radios', 'a beat naming nine radios (the most is eight)', 'bad-field',
  { id: INSTALL, v: '0.99.109', beat: 1, radios: Array.from({ length: 9 }, (_, i) => `radio-${i}:SN${i}`) });
refuse('refuse-beat-with-a-frequency', 'a beat whose radio is a frequency, not a radio key', 'bad-field',
  { id: INSTALL, v: '0.99.109', beat: 1, radios: ['98600000'] });
refuse('refuse-beat-radios-not-a-list', 'a beat whose radios is not a list', 'bad-field', { id: INSTALL, v: '0.99.109', beat: 1, radios: KEY });

// ---- the manifest, and the JSON copy of the field tables ---------------------------------------------------------------------
const names = readdirSync(DIR).filter((n) => n.endsWith('.json')).sort();
const lines = names.map((n) => {
  const text = readFileSync(join(DIR, n), 'utf8').replace(/\r\n/g, '\n');
  return `${createHash('sha256').update(text).digest('hex')}  ${n}`;
});
writeFileSync(join(DIR, 'MANIFEST.sha256'), `${lines.join('\n')}\n`);
writeFileSync(join(DIR, '..', 'radio-journal-spec.json'), `${JSON.stringify(SPEC, null, 1)}\n`);
console.log(`${names.length} fixtures and the manifest written`);
