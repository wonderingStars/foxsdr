// Validation of the radio-report family of messages (docs/radio-reports-contract.md).
//
// Pure functions, no I/O and no clock: worker.js calls them, worker.test.mjs drives them with the
// shared fixture files (test-fixtures/radio-reports/), and the site (radiofail.go) implements the SAME
// rules in Go against the SAME files. The checks are written without regular-expression look-around
// (RE2, which Go uses, has none), so that a rule means the same thing on both sides.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

export const MAX_RADIO_FAIL_BODY = 16384;
export const MAX_FIRST_SESSION_BODY = 8192;

export const DRIVERS = ['rtlsdr', 'hackrf', 'airspy', 'airspyhf', 'sdrplay', 'mirisdr', 'rx888', 'pluto', 'aor',
  'hydrasdr', 'rtltcp', 'soapy', 'soundcard', 'other'];
export const REASON_CLASSES = ['busy', 'driver', 'bind', 'absent', 'timeout', 'vendor', 'rate', 'other'];
export const TRIGGERS = ['startup-restore', 'user-pick', 'saved-retry', 'patch-radio', 'auto-reopen', 'other'];
export const CHANNELS = ['store', 'installer', 'appimage', 'tarball', 'android', ''];
export const UPDATE_CHANNELS = ['stable', 'beta', ''];
export const FIRST_SESSION_TRIGGERS = ['after-minute', 'on-failure'];
export const RESERVED_KINDS = ['radio_found', 'radio_open', 'radio_settings', 'radio_stream', 'radio_recovery', 'radio_close'];
export const KINDS = ['radio_fail', 'first_session'];

// The radio lines of the log ring: the first word (after an optional time stamp) of a line.
const RADIO_LINE_RE = /^(?:[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} )?(?:source|soapy|vendor|radio|rtltcp|aor|rx888|pluto|mirisdr|hackrf|airspy|airspyhf|rtlsdr|sdrplay|hydrasdr|usb|patch): /;

const ID_RE = /^[0-9a-f]{32}$/;
const SESSION_RE = /^[0-9a-f]{16}$/;
const SERIAL_RE = /^[A-Za-z0-9._-]{1,40}$/;
const VERSION_RE = /^[A-Za-z0-9._+ -]{1,48}$/;

// Section 3 of the contract, verbatim.
export const FORBIDDEN_KEY_RE = /freq|hertz|hz$|centre|center|tuned|bookmark|position|latitude|longitude|gps|ipaddr|hostname|username|filename|folder|decoded|callsign|password|secret|^(lat|lon|lng|ip|user|path|file|token)$/;

export function hasForbiddenKey(value) {
  if (Array.isArray(value)) { return value.some(hasForbiddenKey); }
  if (value !== null && typeof value === 'object') {
    for (const [k, v] of Object.entries(value)) {
      if (FORBIDDEN_KEY_RE.test(k.toLowerCase()) || hasForbiddenKey(v)) { return true; }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// The text rules (contract section 4). Each returns true when the text is NOT acceptable.
// ---------------------------------------------------------------------------

const isWord = (c) => c !== undefined && /[A-Za-z0-9_]/.test(c);

export function pathInText(s) {
  // A drive path: letter, colon, slash - where the letter does not belong to a longer word ("tcp://").
  for (const m of s.matchAll(/[A-Za-z]:[\\/]/g)) {
    if (m.index === 0 || !isWord(s[m.index - 1])) { return true; }
  }
  // \\host\share (the scrub writes \\<host>\share, which is not matched)
  if (/\\\\[A-Za-z0-9_.$-]+\\[A-Za-z0-9$_.-]/.test(s)) { return true; }
  // \Users\x, /Users/x, /home/x: anything but the scrub's <user>
  for (const m of s.matchAll(/[\\/](?:users|home)[\\/]([^\\/\s]*)/gi)) {
    if (!m[1].startsWith('<user>')) { return true; }
  }
  return false;
}

export function addressInText(s) {
  for (const m of s.matchAll(/[0-9]{1,3}(?:\.[0-9]{1,3}){3}/g)) {
    const i = m.index;
    const end = i + m[0].length;
    const before = s[i - 1];
    const after = s[end];
    if (i > 0 && (isWord(before) || before === '.')) { continue; }
    if (isWord(after)) { continue; }
    if (after === '.' && /[0-9]/.test(s[end + 1] || '')) { continue; }
    const octets = m[0].split('.').map(Number);
    if (octets.some((n) => n > 255)) { continue; }
    if (octets[0] === 127 || m[0] === '0.0.0.0') { continue; }
    const lead = s.slice(Math.max(0, i - 16), i).toLowerCase().replace(/[\s:=]+$/, '');
    if (/(?:version|firmware|build|package)$/.test(lead)) { continue; }
    return true;
  }
  return false;
}

// Words that follow "serial" without being a serial number ("serial port").
const SERIAL_BENIGN = new Set(['port', 'ports', 'bus', 'interface', 'cable', 'adapter', 'line', 'lines', 'connection', 'communication']);

export function serialInText(s, ownSerial) {
  if (typeof ownSerial === 'string' && ownSerial.length >= 4 && s.toLowerCase().includes(ownSerial.toLowerCase())) {
    return true;
  }
  const re = /(serial(?:_number)?|s\/n|_sn|\bsn)\b(?:\s*(?:number|no\.?|#))?\s*[:=]?\s*([^\s,;)"']*)/gi;
  for (const m of s.matchAll(re)) {
    const value = m[2].replace(/[.!?:]+$/, '');  // "... has no serial." ends a sentence; it does not start a value
    if (value === '' || value === '<stripped>') { continue; }
    if (SERIAL_BENIGN.has(value.toLowerCase())) { continue; }
    return true;
  }
  return false;
}

export function frequencyInText(s) {
  return /[0-9][0-9.,]*\s?[kmg]?hz\b/i.test(s);
}

// First failing code for one string, or '' when it is acceptable.
export function textCode(s, ownSerial) {
  if (pathInText(s)) { return 'path-in-text'; }
  if (addressInText(s)) { return 'address-in-text'; }
  if (serialInText(s, ownSerial)) { return 'serial-in-text'; }
  if (frequencyInText(s)) { return 'frequency-in-text'; }
  return '';
}

// ---------------------------------------------------------------------------
// The radio key (contract 1.1)
// ---------------------------------------------------------------------------

export function modelSlug(model) {
  return String(model).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40);
}
export function radioKey(model, serial) {
  return `${modelSlug(model)}:${serial === '' ? '-' : serial.toUpperCase()}`;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

const isInt = (v, lo, hi) => typeof v === 'number' && Number.isInteger(v) && v >= lo && v <= hi;
const isStr = (v, lo, hi) => typeof v === 'string' && v.length >= lo && v.length <= hi;
const refuse = (code) => ({ ok: false, code });

// `msg` is the parsed JSON. `opts.scrubModel` is worker.js's scrubModel. Returns { ok: true, kind, canonical }
// or { ok: false, code }. The caller has already enforced the size limit and parsed the JSON.
export function validateRadioMessage(msg, opts) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) { return refuse('bad-json'); }
  if (typeof msg.kind !== 'string') { return refuse('unknown-kind'); }
  if (RESERVED_KINDS.includes(msg.kind)) { return refuse('reserved-kind'); }
  if (!KINDS.includes(msg.kind)) { return refuse('unknown-kind'); }
  if (msg.schema !== 1) { return refuse('bad-schema'); }
  if (hasForbiddenKey(msg)) { return refuse('forbidden-field'); }
  if (msg.installId === undefined) { return refuse('missing-field'); }
  if (typeof msg.installId !== 'string' || !ID_RE.test(msg.installId)) { return refuse('bad-install-id'); }
  for (const f of ['sessionId', 'seq', 't', 'body']) {
    if (msg[f] === undefined) { return refuse('missing-field'); }
  }
  if (typeof msg.sessionId !== 'string' || !SESSION_RE.test(msg.sessionId)) { return refuse('bad-field'); }
  if (!isInt(msg.seq, 0, 1000000) || !isInt(msg.t, 0, 2592000)) { return refuse('bad-field'); }
  const body = msg.body;
  if (body === null || typeof body !== 'object' || Array.isArray(body)) { return refuse('bad-field'); }
  return msg.kind === 'radio_fail' ? validateRadioFail(msg, body, opts) : validateFirstSession(msg, body);
}

function validateRadioFail(msg, body, opts) {
  for (const f of ['driver', 'model', 'serial', 'reason', 'reasonClass', 'trigger', 'attempt', 'diagOn', 'logLines', 'v', 'os', 'arch']) {
    if (body[f] === undefined) { return refuse('missing-field'); }
  }
  if (msg.radioKey === undefined) { return refuse('missing-field'); }
  if (!DRIVERS.includes(body.driver)) { return refuse('bad-field'); }
  if (!isStr(body.model, 1, 32) || opts.scrubModel(body.model) !== body.model) { return refuse('bad-field'); }
  if (typeof body.serial !== 'string' || (body.serial !== '' && !SERIAL_RE.test(body.serial))) { return refuse('bad-field'); }
  for (const f of ['firmware', 'library']) {
    if (body[f] !== undefined && (typeof body[f] !== 'string' || (body[f] !== '' && !VERSION_RE.test(body[f])))) {
      return refuse('bad-field');
    }
  }
  if (!isStr(body.reason, 1, 400)) { return refuse('bad-field'); }
  if (!REASON_CLASSES.includes(body.reasonClass) || !TRIGGERS.includes(body.trigger)) { return refuse('bad-field'); }
  if (!isInt(body.attempt, 1, 10000)) { return refuse('bad-field'); }
  if (body.openMs !== undefined && !isInt(body.openMs, 0, 3600000)) { return refuse('bad-field'); }
  if (body.sampleRateSps !== undefined && !isInt(body.sampleRateSps, 0, 1000000000)) { return refuse('bad-field'); }
  if (body.dropped !== undefined && !isInt(body.dropped, 0, 1000000)) { return refuse('bad-field'); }
  for (const f of ['superseded', 'diagOn']) {
    if (body[f] !== undefined && typeof body[f] !== 'boolean') { return refuse('bad-field'); }
  }
  if (!Array.isArray(body.logLines) || body.logLines.length > 40) { return refuse('bad-field'); }
  for (const line of body.logLines) {
    if (!isStr(line, 1, 240) || !RADIO_LINE_RE.test(line)) { return refuse('bad-field'); }
  }
  if (!isStr(body.v, 1, 48) || !isStr(body.os, 1, 40) || !isStr(body.arch, 1, 8)) { return refuse('bad-field'); }
  if (body.ch !== undefined && !CHANNELS.includes(body.ch)) { return refuse('bad-field'); }
  if (body.uch !== undefined && !UPDATE_CHANNELS.includes(body.uch)) { return refuse('bad-field'); }
  if (msg.radioKey !== radioKey(body.model, body.serial)) { return refuse('key-mismatch'); }
  for (const text of [body.model, body.reason, ...body.logLines, body.firmware || '', body.library || '']) {
    const code = textCode(text, body.serial);
    if (code) { return refuse(code); }
  }
  const out = {
    driver: body.driver, model: body.model, serial: body.serial,
    firmware: body.firmware || '', library: body.library || '',
    reason: body.reason, reasonClass: body.reasonClass, trigger: body.trigger, attempt: body.attempt,
  };
  for (const f of ['openMs', 'sampleRateSps', 'superseded', 'dropped']) {
    if (body[f] !== undefined) { out[f] = body[f]; }
  }
  Object.assign(out, {
    diagOn: body.diagOn, logLines: body.logLines.slice(), v: body.v, ch: body.ch || '', uch: body.uch || '',
    os: body.os, arch: body.arch,
  });
  return {
    ok: true, kind: 'radio_fail',
    canonical: {
      schema: 1, kind: 'radio_fail', installId: msg.installId, sessionId: msg.sessionId, seq: msg.seq, t: msg.t,
      radioKey: msg.radioKey, body: out,
    },
  };
}

// The first-session body keeps the usage record's own fields; worker.js hands it to the same column writer the
// usage record uses, which clamps and checks each. Here: the fields the contract requires, the one that makes
// it a first session, and the text rule on the model.
const FIRST_SESSION_FIELDS = ['v', 'os', 'arch', 'launches', 'crashes', 'sessionSec', 'sdr', 'modes', 'panels', 'plugins', 'ch',
  'first', 'fv', 'uch', 'stalls', 'health', 'exits_died', 'exits_killed', 'exits_ended', 'exits_unknown', 'trigger'];

function validateFirstSession(msg, body) {
  for (const f of ['v', 'os', 'arch', 'launches', 'trigger']) {
    if (body[f] === undefined) { return refuse('missing-field'); }
  }
  if (!isStr(body.v, 1, 48) || !isStr(body.os, 1, 40) || !isStr(body.arch, 1, 8)) { return refuse('bad-field'); }
  if (!FIRST_SESSION_TRIGGERS.includes(body.trigger)) { return refuse('bad-field'); }
  if (typeof body.launches !== 'number' || !Number.isInteger(body.launches)) { return refuse('bad-field'); }
  if (msg.radioKey !== undefined && msg.radioKey !== '') { return refuse('bad-field'); }
  if (typeof body.sdr === 'string') {
    const code = textCode(body.sdr, '');
    if (code) { return refuse(code); }
  }
  if (body.launches !== 1) { return refuse('not-first-session'); }
  const out = {};
  for (const f of FIRST_SESSION_FIELDS) {
    if (body[f] !== undefined) { out[f] = body[f]; }
  }
  return {
    ok: true, kind: 'first_session',
    canonical: {
      schema: 1, kind: 'first_session', installId: msg.installId, sessionId: msg.sessionId, seq: msg.seq, t: msg.t,
      radioKey: '', body: out,
    },
  };
}
