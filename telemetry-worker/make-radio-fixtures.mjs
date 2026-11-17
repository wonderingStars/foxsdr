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

// ---- must-fix lane E: the words the application sends that the contract had no place for (CONTRACT ISSUES 1 to 4, 9 to 12, 15 to 17) ----
const failCanon = (over) => ({ schema: 2, kind: 'radio_fail', installId: INSTALL, sessionId: SESSION, seq: 3, t: 412, radioKey: FAILKEY, body: { ...failCanonicalBody, ...over } });
const failMsg = (over) => ({ schema: 2, kind: 'radio_fail', installId: INSTALL, sessionId: SESSION, seq: 3, t: 412, radioKey: FAILKEY, body: { ...failBody, ...over } });
valid('valid-radio-fail-class-transfer', 'issue 15: a USB transfer that failed while the radio was being opened has its own class (it was sent as `control`)',
  failMsg({ reason: 'the radio stopped answering while it was being opened: a bulk read failed (Windows error 31)', reasonClass: 'transfer' }),
  { canonical: failCanon({ reason: 'the radio stopped answering while it was being opened: a bulk read failed (Windows error 31)', reasonClass: 'transfer' }) });
valid('valid-radio-fail-class-tuner', 'issue 16: the tuner or demodulator inside an RTL-SDR would not initialise (it was sent as `control`)',
  failMsg({ reason: 'the tuner would not initialise: its PLL did not lock', reasonClass: 'tuner' }),
  { canonical: failCanon({ reason: 'the tuner would not initialise: its PLL did not lock', reasonClass: 'tuner' }) });
valid('valid-session-end-no-radio', 'issue 2: the session ended with no radio opened (L1); what the scan listed (models only), how many scans, how many opens failed; no radio key',
  env('session_end', 20, 1800, { v: '0.99.109', opened: false, scans: 3, openFailures: 1, listed: ['Generic RTL2832U'], sessionSec: 1800 }, { radioKey: '' }));
valid('valid-session-end-with-dropped', 'issue 4: a message with no radio key may say how many journal lines the folder\'s bounds dropped (the daily bound counts per radio key, this counts the rest)',
  env('session_end', 21, 40, { v: '0.99.109', opened: false, scans: 0, openFailures: 0, listed: [], sessionSec: 40, dropped: 7 }, { radioKey: '' }));
valid('valid-session-env-with-dropped', 'issue 4: the environment record may carry the same count',
  env('session_env', 2, 1, { v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', launches: 2, cpu: 'AMD Ryzen 5 3600', audioApi: 'mme', diagOn: false, dropped: 12 }, { radioKey: '' }));
valid('valid-session-fault-service', 'issue 12: the SDRplay API service is not running and no radio is open to carry it; no radio key',
  env('session_fault', 22, 9, { v: '0.99.109', service: 'sdrplay', phase: 'gave-up', reasonClass: 'driver', reason: 'the SDRplay API service is not running' }, { radioKey: '' }));
valid('valid-radio-settings-refused-frequency', 'issue 9: the radio refused a retune (the frequency itself is never sent: only the word)',
  env('radio_settings', 9, 8, { ...base, why: 'user', refused: 'frequency', refusedClass: 'radio', reason: 'refused: frequency: the tuner would not lock' }));
valid('valid-radio-settings-refused-other', 'issue 9: a control with no word of its own',
  env('radio_settings', 9, 8, { ...base, why: 'auto', refused: 'other', refusedClass: 'radio', reason: 'refused: other: the radio refused a setting' }));
valid('valid-radio-settings-refused-range', 'issue 10: a value the user typed out of range, which says nothing about the radio',
  env('radio_settings', 9, 8, { ...base, why: 'user', refused: 'gain', refusedClass: 'range', reason: 'out of range: gain' }));
valid('valid-radio-recovery-degraded', 'issue 11: the radio keeps streaming but is deaf (the tuner\'s PLL did not lock): degraded, not gave-up',
  env('radio_recovery', 13, 140, { ...base, phase: 'degraded', what: 'pllunlocked', attempt: 1, reasonClass: 'tuner', reason: 'the tuner\'s PLL did not lock' }));
valid('valid-radio-recovery-service-words', 'issue 11: the words the application sends for `what` besides reopen, usbstuck and streamrestart',
  env('radio_recovery', 14, 141, { ...base, phase: 'ok', what: 'servicestate', attempt: 2 }));
valid('valid-radio-close-without-open', 'issue 17: a radio that stopped under the HackRF sweeper has a close and never had an open (the sweeper journals none)',
  env('radio_close', 16, 200, { driver: 'hackrf', model: 'HackRF One', serial: '0000000000000000a06', v: '0.99.109', cause: 'died', openSec: 0, reason: 'the sweep stopped: the radio stopped answering' }, { radioKey: 'hackrf-one:0000000000000000A06' }));
const dropFx = (name, about, request) => write(name, { about, expect: 'drop', status: 204, request });
dropFx('drop-synthetic-radio-fail', 'issue 3: a failure report from a test, CI or the owner\'s own run (`synthetic: 1`) is checked, answered 204 and then dropped: not forwarded, not counted',
  { ...failMsg({}), synthetic: 1 });
dropFx('drop-synthetic-journal-event', 'issue 3: the same for a journal event',
  { ...open(), synthetic: 1 });
dropFx('drop-synthetic-first-session', 'issue 3: and for the first-session record (it must not reach foxsdr_firstsession)',
  env('first_session', 5, 60, { v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', launches: 1, trigger: 'after-minute' }, { radioKey: '', synthetic: 1 }));
dropFx('drop-synthetic-usage-record', 'issue 3: a usage record carries the marker too (it has no kind, so it is not a radio-report message)',
  { id: INSTALL, v: '0.99.109', os: 'Windows 10.0.22631', arch: 'x64', launches: 3, crashes: 0, sessionSec: 100, synthetic: 1 });
dropFx('drop-synthetic-beat', 'issue 3: and a beat',
  { id: INSTALL, v: '0.99.109', beat: 1, radios: [KEY], synthetic: 1 });
refuse('refuse-synthetic-is-not-one', 'the marker is exactly the number 1: anything else is a malformed field', 'bad-field', { ...failMsg({}), synthetic: 'yes' });
refuse('refuse-ring-line-with-level-word', 'issue 1: a raw ring line still carries its level word (`info`, `warn`); the sender removes it and the contract\'s pattern has none', 'bad-field',
  failMsg({ logLines: ['12:01:07.331 info soapy: airspyhf open failed'] }));
refuse('refuse-ring-line-two-word-prefix', 'issue 1: a two-word prefix (`radio setup:`) is not a radio line; the sender leaves such lines out', 'bad-field',
  failMsg({ logLines: ['12:01:07.331 radio setup: the first-run check found 1 radio'] }));
refuse('refuse-session-end-with-radio-key', 'a session_end names a radio', 'bad-field',
  { ...env('session_end', 20, 1800, { v: '0.99.109', opened: false, scans: 3, openFailures: 1, listed: [], sessionSec: 1800 }), radioKey: KEY });
refuse('refuse-session-end-opened-is-not-a-bool', 'a session_end whose `opened` is not true or false', 'bad-field',
  env('session_end', 20, 1800, { v: '0.99.109', opened: 'no', scans: 3, openFailures: 1, listed: [], sessionSec: 1800 }, { radioKey: '' }));
refuse('refuse-session-end-nine-listed', 'a session_end listing nine models (the most is eight)', 'bad-field',
  env('session_end', 20, 1800, { v: '0.99.109', opened: false, scans: 3, openFailures: 1, listed: Array.from({ length: 9 }, (_, i) => `Model ${i}`), sessionSec: 1800 }, { radioKey: '' }));
refuse('refuse-session-end-address-in-listed', 'a session_end whose listed model is an address', 'bad-field',
  env('session_end', 20, 1800, { v: '0.99.109', opened: false, scans: 3, openFailures: 1, listed: ['10.1.2.3'], sessionSec: 1800 }, { radioKey: '' }));
refuse('refuse-session-fault-with-radio-key', 'a session_fault names a radio (a fault of one radio is a radio_recovery)', 'bad-field',
  { ...env('session_fault', 22, 9, { v: '0.99.109', service: 'sdrplay', phase: 'gave-up' }), radioKey: KEY });
refuse('refuse-session-fault-path-in-reason', 'a path in a service fault\'s reason', 'path-in-text',
  env('session_fault', 22, 9, { v: '0.99.109', service: 'sdrplay', phase: 'start', reason: 'could not start C:\\Program Files\\SDRplay\\API\\service.exe' }, { radioKey: '' }));
refuse('refuse-settings-refused-class-unknown', 'a refusedClass that is not radio or range', 'bad-field',
  env('radio_settings', 9, 8, { ...base, why: 'user', refused: 'gain', refusedClass: 'maybe' }));
refuse('refuse-recovery-phase-unknown', 'a recovery phase that is not one of the four', 'bad-field',
  env('radio_recovery', 13, 140, { ...base, phase: 'limping', what: 'reopen', attempt: 1 }));

// ---- the manifest, and the JSON copy of the field tables ---------------------------------------------------------------------
// ---- the installer's report (contract section 13) ----------------------------------------------------------------------------
const inst = (body, over = {}) => ({ schema: 2, kind: 'install_outcome', messageId: '0123456789abcdef', t: 0, body: { outcome: 'failed', appVersion: '0.99.109', windows: '10.0.22631', ...body }, ...over });
valid('valid-install-outcome-failed', 'the installer began copying and did not finish: no install id (the program never ran), a random message id, three words', inst({}));
valid('valid-install-outcome-cancelled', 'the person cancelled Setup', inst({ outcome: 'cancelled' }));
valid('valid-install-outcome-blocked', 'Setup stopped before copying anything', inst({ outcome: 'blocked', windows: '6.1.7601' }));
refuse('refuse-install-outcome-succeeded', 'a successful install is never reported (the first run\'s usage report is its trace)', 'bad-field', inst({ outcome: 'succeeded' }));
refuse('refuse-install-outcome-with-install-id', 'the installer has no install id; a message that carries one is refused', 'bad-field', inst({}, { installId: INSTALL }));
refuse('refuse-install-outcome-with-session-id', 'nor a session id', 'bad-field', inst({}, { sessionId: SESSION }));
refuse('refuse-install-outcome-with-radio-key', 'nor a radio key', 'bad-field', inst({}, { radioKey: KEY }));
refuse('refuse-install-outcome-extra-body-key', 'this kind carries nothing but its three words: an unknown body key is refused, not dropped', 'bad-field', inst({ machine: 'x' }));
refuse('refuse-install-outcome-bad-version', 'an app version that is not three dotted numbers', 'bad-field', inst({ appVersion: '0.99.109-beta' }));
refuse('refuse-install-outcome-bad-windows', 'a Windows version that is not three dotted numbers', 'bad-field', inst({ windows: 'Windows 11' }));
refuse('refuse-install-outcome-nonzero-time', 'the installer has no session: t is exactly 0', 'bad-field', inst({}, { t: 5 }));
refuse('refuse-install-outcome-without-message-id', 'a random message id is required (a retry whose answer was lost counts once)', 'missing-field',
  (() => { const m = inst({}); delete m.messageId; return m; })());
refuse('refuse-install-outcome-bad-message-id', 'a message id that is not 16 lower-case hex characters', 'bad-field', inst({}, { messageId: 'NOT-HEX-AT-ALL!!' }));
dropFx('drop-synthetic-install-outcome', 'the marker drops the installer\'s report too (a CI run of the installer is not a person)', inst({}, { synthetic: 1 }));

const names = readdirSync(DIR).filter((n) => n.endsWith('.json')).sort();
const lines = names.map((n) => {
  const text = readFileSync(join(DIR, n), 'utf8').replace(/\r\n/g, '\n');
  return `${createHash('sha256').update(text).digest('hex')}  ${n}`;
});
writeFileSync(join(DIR, 'MANIFEST.sha256'), `${lines.join('\n')}\n`);
writeFileSync(join(DIR, '..', 'radio-journal-spec.json'), `${JSON.stringify(SPEC, null, 1)}\n`);
console.log(`${names.length} fixtures and the manifest written`);
