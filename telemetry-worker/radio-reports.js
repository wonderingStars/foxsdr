// Validation of the radio-report family of messages (docs/radio-reports-contract.md).
//
// Pure functions, no I/O and no clock: worker.js calls them, worker.test.mjs drives them with the
// shared fixture files (test-fixtures/radio-reports/), and the site (radiofail.go) implements the SAME
// rules in Go against the SAME files. The checks are written without regular-expression look-around
// (RE2, which Go uses, has none), so that a rule means the same thing on both sides.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

import { BEAT_KEY_RE, BEAT_MAX_RADIOS, KIND_SPECS, LISTS } from './radio-journal-spec.js';

export const SCHEMA = 2;
export const MAX_RADIO_FAIL_BODY = 16384;
export const MAX_FIRST_SESSION_BODY = 8192;
export const MAX_JOURNAL_BODY = 4096;
export const MAX_BEAT_BODY = 1024;

export const DRIVERS = LISTS.drivers;
export const REASON_CLASSES = LISTS.reasonClasses;
export const TRIGGERS = LISTS.triggers;
export const CHANNELS = LISTS.channels;
export const UPDATE_CHANNELS = LISTS.updateChannels;
export const FIRST_SESSION_TRIGGERS = ['after-minute', 'on-failure'];
// Named for a later schema (a batched envelope, contract section 3.9) and refused until it is defined.
export const RESERVED_KINDS = ['radio_batch'];
export const JOURNAL_KINDS = Object.keys(KIND_SPECS);
export const KINDS = ['radio_fail', 'first_session', 'radio_beat', 'install_outcome', ...JOURNAL_KINDS];

// The radio lines of the log ring: the first word (after an optional time stamp) of a line.
const RADIO_LINE_RE = /^(?:[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} )?(?:source|soapy|vendor|radio|rtltcp|aor|rx888|pluto|mirisdr|hackrf|airspy|airspyhf|rtlsdr|sdrplay|hydrasdr|usb|patch): /;

const ID_RE = /^[0-9a-f]{32}$/;
const SESSION_RE = /^[0-9a-f]{16}$/;
const SERIAL_RE = /^[A-Za-z0-9._-]{1,40}$/;
const VERSION_RE = /^[A-Za-z0-9._+ -]{1,48}$/;

// Section 4 of the contract, verbatim.
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
// The text rules (contract section 5). Each returns true when the text is NOT acceptable.
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
const isNum = (v, lo, hi) => typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi;
const isStr = (v, lo, hi) => typeof v === 'string' && v.length >= lo && v.length <= hi;
const refuse = (code) => ({ ok: false, code });

const reCache = new Map();
function re(pattern) {
  let r = reCache.get(pattern);
  if (!r) { r = new RegExp(pattern); reCache.set(pattern, r); }
  return r;
}
const BEAT_KEY = re(BEAT_KEY_RE);

// One value against one field (or array item) of radio-journal-spec.js. `ctx.texts` collects the free text the text rules are
// applied to afterwards; `ctx.scrubModel` is worker.js's scrubModel.
function valueOk(f, v, ctx) {
  switch (f.type) {
    case 'enum': return typeof v === 'string' && (f.values || LISTS[f.list]).includes(v);
    case 'int': return isInt(v, f.lo, f.hi);
    case 'num': return isNum(v, f.lo, f.hi);
    case 'bool': return typeof v === 'boolean';
    case 'str': return isStr(v, f.lo, f.hi) && (!f.re || re(f.re).test(v));
    case 'text':
      if (!isStr(v, f.lo, f.hi)) { return false; }
      ctx.texts.push(v);
      return true;
    case 'model':
      if (!isStr(v, 1, 32) || ctx.scrubModel(v) !== v) { return false; }
      ctx.texts.push(v);
      return true;
    case 'serial': return typeof v === 'string' && (v === '' || SERIAL_RE.test(v));
    case 'arr': return Array.isArray(v) && v.length <= f.max && v.every((x) => valueOk(f.item, x, ctx));
    default: return false;
  }
}

// `msg` is the parsed JSON. `opts.scrubModel` is worker.js's scrubModel. Returns { ok: true, kind, canonical }
// or { ok: false, code }. The caller has already enforced the size limit and parsed the JSON.
export function validateRadioMessage(msg, opts) {
  const v = validateRadioMessageInner(msg, opts);
  if (v.ok && msg.synthetic === 1) { v.synthetic = true; }
  return v;
}

function validateRadioMessageInner(msg, opts) {
  if (msg === null || typeof msg !== 'object' || Array.isArray(msg)) { return refuse('bad-json'); }
  if (typeof msg.kind !== 'string') { return refuse('unknown-kind'); }
  if (RESERVED_KINDS.includes(msg.kind)) { return refuse('reserved-kind'); }
  if (!KINDS.includes(msg.kind)) { return refuse('unknown-kind'); }
  if (msg.schema !== SCHEMA) { return refuse('bad-schema'); }
  if (hasForbiddenKey(msg)) { return refuse('forbidden-field'); }
  // THE TEST MARKER (contract 1.3): an envelope member `synthetic` that is exactly the number 1 marks a message from a test, CI or the
  // owner's own run. It is checked like any field and then the caller DROPS the message (`synthetic: true` in the answer): it is
  // never forwarded, stored or counted. Any other value is a malformed field.
  if (msg.synthetic !== undefined && msg.synthetic !== 1) { return refuse('bad-field'); }
  if (msg.kind === 'radio_beat') { return validateBeat(msg); }
  if (msg.kind === 'install_outcome') { return validateInstallOutcome(msg); }

  // THE INSTALL ID. Every kind carries one except a radio_fail from an install with Usage reporting off: that one has
  // no install id, no session id and no sequence number (nothing that links two of its reports), and a random
  // per-message id so a retry whose answer was lost can be told from a new failure (contract section 1.2).
  const anonymous = msg.kind === 'radio_fail' && msg.installId === undefined;
  if (!anonymous) {
    if (msg.installId === undefined) { return refuse('missing-field'); }
    if (typeof msg.installId !== 'string' || !ID_RE.test(msg.installId)) { return refuse('bad-install-id'); }
  }
  const need = anonymous ? ['messageId', 't', 'body'] : ['sessionId', 'seq', 't', 'body'];
  for (const f of need) {
    if (msg[f] === undefined) { return refuse('missing-field'); }
  }
  if (anonymous) {
    if (typeof msg.messageId !== 'string' || !SESSION_RE.test(msg.messageId)) { return refuse('bad-field'); }
    if (msg.sessionId !== undefined || msg.seq !== undefined) { return refuse('bad-field'); }
  } else {
    if (typeof msg.sessionId !== 'string' || !SESSION_RE.test(msg.sessionId)) { return refuse('bad-field'); }
    if (!isInt(msg.seq, 0, 1000000)) { return refuse('bad-field'); }
    if (msg.messageId !== undefined) { return refuse('bad-field'); }
  }
  if (!isInt(msg.t, 0, 2592000)) { return refuse('bad-field'); }
  if (msg.age !== undefined && !isInt(msg.age, 0, 2592000)) { return refuse('bad-field'); }
  const body = msg.body;
  if (body === null || typeof body !== 'object' || Array.isArray(body)) { return refuse('bad-field'); }
  if (msg.kind === 'radio_fail') { return validateRadioFail(msg, body, opts, anonymous); }
  if (msg.kind === 'first_session') { return validateFirstSession(msg, body); }
  return validateJournal(msg, body, opts);
}

// The envelope of a canonical message: only the fields the contract defines, in a fixed order.
function envelopeOf(msg, anonymous, radioKeyValue) {
  const out = { schema: SCHEMA, kind: msg.kind };
  if (anonymous) { out.messageId = msg.messageId; } else {
    out.installId = msg.installId; out.sessionId = msg.sessionId; out.seq = msg.seq;
  }
  out.t = msg.t;
  if (msg.age !== undefined) { out.age = msg.age; }
  out.radioKey = radioKeyValue;
  return out;
}

function validateRadioFail(msg, body, opts, anonymous) {
  for (const f of ['driver', 'model', 'serial', 'reason', 'reasonClass', 'trigger', 'attempt', 'diagOn', 'logLines', 'v', 'os', 'arch']) {
    if (body[f] === undefined) { return refuse('missing-field'); }
  }
  if (msg.radioKey === undefined) { return refuse('missing-field'); }
  if (!LISTS.drivers.includes(body.driver)) { return refuse('bad-field'); }
  if (!isStr(body.model, 1, 32) || opts.scrubModel(body.model) !== body.model) { return refuse('bad-field'); }
  if (typeof body.serial !== 'string' || (body.serial !== '' && !SERIAL_RE.test(body.serial))) { return refuse('bad-field'); }
  for (const f of ['firmware', 'library']) {
    if (body[f] !== undefined && (typeof body[f] !== 'string' || (body[f] !== '' && !VERSION_RE.test(body[f])))) {
      return refuse('bad-field');
    }
  }
  if (!isStr(body.reason, 1, 400)) { return refuse('bad-field'); }
  if (!LISTS.reasonClasses.includes(body.reasonClass) || !LISTS.triggers.includes(body.trigger)) { return refuse('bad-field'); }
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
  if (body.ch !== undefined && !LISTS.channels.includes(body.ch)) { return refuse('bad-field'); }
  if (body.uch !== undefined && !LISTS.updateChannels.includes(body.uch)) { return refuse('bad-field'); }
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
  return { ok: true, kind: 'radio_fail', anonymous, canonical: { ...envelopeOf(msg, anonymous, msg.radioKey), body: out } };
}

// The six journal kinds and session_env: every field is read from the table in radio-journal-spec.js.
function validateJournal(msg, body, opts) {
  const spec = KIND_SPECS[msg.kind];
  if (spec.radio) {
    if (msg.radioKey === undefined) { return refuse('missing-field'); }
  } else if (msg.radioKey !== undefined && msg.radioKey !== '') {
    return refuse('bad-field');
  }
  for (const f of spec.fields) {
    if (f.req && body[f.name] === undefined) { return refuse('missing-field'); }
  }
  const ctx = { texts: [], scrubModel: opts.scrubModel };
  for (const f of spec.fields) {
    if (body[f.name] !== undefined && !valueOk(f, body[f.name], ctx)) { return refuse('bad-field'); }
  }
  if (spec.radio && msg.radioKey !== radioKey(body.model, body.serial)) { return refuse('key-mismatch'); }
  const own = typeof body.serial === 'string' ? body.serial : '';
  for (const text of ctx.texts) {
    const code = textCode(text, own);
    if (code) { return refuse(code); }
  }
  const out = {};
  for (const f of spec.fields) {
    if (body[f.name] !== undefined) { out[f.name] = body[f.name]; }
  }
  return { ok: true, kind: msg.kind, anonymous: false, canonical: { ...envelopeOf(msg, false, spec.radio ? msg.radioKey : ''), body: out } };
}

// The running-now beat's extension (contract section 3.10): the keys of the radios open right now. The Worker builds this
// message from a legacy beat that carries `radios`; the site takes the same message.
export function validateBeat(msg) {
  if (msg.installId === undefined) { return refuse('missing-field'); }
  if (typeof msg.installId !== 'string' || !ID_RE.test(msg.installId)) { return refuse('bad-install-id'); }
  if (msg.v === undefined || msg.radios === undefined) { return refuse('missing-field'); }
  if (!isStr(msg.v, 1, 48)) { return refuse('bad-field'); }
  if (!Array.isArray(msg.radios) || msg.radios.length < 1 || msg.radios.length > BEAT_MAX_RADIOS) { return refuse('bad-field'); }
  for (const k of msg.radios) {
    if (typeof k !== 'string' || !BEAT_KEY.test(k)) { return refuse('bad-field'); }
  }
  return {
    ok: true, kind: 'radio_beat', anonymous: false,
    canonical: { schema: SCHEMA, kind: 'radio_beat', installId: msg.installId, v: msg.v, radios: [...new Set(msg.radios)] },
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
  return { ok: true, kind: 'first_session', anonymous: false, canonical: { ...envelopeOf(msg, false, ''), body: out } };
}

// The installer's report (contract section 13): the anonymous envelope with only a random message id, and a body of exactly three
// words. It is the one kind that REFUSES an unknown body key: it must carry nothing but these three.
const OUTCOMES = ['failed', 'cancelled', 'blocked'];
const APP_VERSION_RE = /^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,5}$/;
const WINDOWS_RE = /^[0-9]{1,2}\.[0-9]{1,2}\.[0-9]{1,6}$/;

function validateInstallOutcome(msg) {
  for (const f of ['messageId', 't', 'body']) {
    if (msg[f] === undefined) { return refuse('missing-field'); }
  }
  for (const f of ['installId', 'sessionId', 'seq', 'radioKey', 'age']) {
    if (msg[f] !== undefined) { return refuse('bad-field'); }
  }
  if (typeof msg.messageId !== 'string' || !SESSION_RE.test(msg.messageId)) { return refuse('bad-field'); }
  if (msg.t !== 0) { return refuse('bad-field'); }
  const body = msg.body;
  if (body === null || typeof body !== 'object' || Array.isArray(body)) { return refuse('bad-field'); }
  for (const f of ['outcome', 'appVersion', 'windows']) {
    if (body[f] === undefined) { return refuse('missing-field'); }
  }
  if (Object.keys(body).some((k) => !['outcome', 'appVersion', 'windows'].includes(k))) { return refuse('bad-field'); }
  if (typeof body.outcome !== 'string' || !OUTCOMES.includes(body.outcome)) { return refuse('bad-field'); }
  if (typeof body.appVersion !== 'string' || !APP_VERSION_RE.test(body.appVersion)) { return refuse('bad-field'); }
  if (typeof body.windows !== 'string' || !WINDOWS_RE.test(body.windows)) { return refuse('bad-field'); }
  return {
    ok: true, kind: 'install_outcome', anonymous: true,
    canonical: { schema: SCHEMA, kind: 'install_outcome', messageId: msg.messageId, t: 0, body: { outcome: body.outcome, appVersion: body.appVersion, windows: body.windows } },
  };
}
