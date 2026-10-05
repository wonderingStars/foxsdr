// FoxSDR anonymous usage endpoint (Cloudflare Worker + Analytics Engine).
//
// Receives one report per application launch, describing the session that
// just ended. See PRIVACY.md for the payload; the short version is that it
// carries no personal data, and this Worker is written so that it cannot
// start doing so by accident.
//
// WHAT THIS DELIBERATELY DOES NOT DO:
//
//   - It never reads or writes request.headers.get('cf-connecting-ip'),
//     request.cf.country, or anything else derived from the connection. The
//     IP necessarily reaches the edge because that is how TCP works; nothing
//     here records it, and Analytics Engine rows contain no field that could.
//   - It writes no logs. A console.log of the request would put the IP into
//     the tail buffer, which is the usual way "we don't store IPs" turns out
//     not to be true.
//   - It stores nothing per user beyond the counters below, keyed by an
//     install id the user's machine generated at random and can delete.
//
// COUNTING USERS: index1 is the install id, and unique installs are read with
//   SELECT count(DISTINCT index1) FROM foxsdr_usage
// NOT with uniq(), which is approximate and reads low on small samples.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

const MAX_BODY = 4096;          // a report is a few hundred bytes
const ID_RE = /^[0-9a-f]{32}$/; // exactly what newInstallId() produces

// Bounded, so a hostile client cannot write unbounded strings into the
// dataset. Everything is a coarse label; none of it is free text from a user.
function clamp(v, max) {
  return typeof v === 'string' ? v.slice(0, max) : '';
}
function num(v, max) {
  const n = Number(v);
  return Number.isFinite(n) && n >= 0 ? Math.min(Math.floor(n), max) : 0;
}

// DISPLAY STALLS: how many times the client's hang watchdog classified a freeze
// as the display driver's (not the application's) and the server has not yet
// been told. One bare integer; see PRIVACY.md "Display stalls".
//
// Returns the count, or null when the client did not send a USABLE one - and
// null is "not reported", which is not zero. Old clients never send the field,
// and a record whose count is the wrong shape is treated the same way rather
// than rejected (the rest of the record is still good, and a bad optional field
// must not cost a session's length) or coerced to 0 (which would read as "this
// install measured its stalls and had none"). Deliberately STRICT, unlike
// num() above: only a JSON number counts, so "3", true, [], {} and null are all
// "not reported". Fractions are floored and anything above MAX_STALLS is
// clamped to it, so a hostile client cannot write an unbounded number.
const MAX_STALLS = 100000;  // the application's own cap (StallLedger::kMaxCount)
function stallCount(v) {
  if (typeof v !== 'number' || !Number.isFinite(v) || v < 0) { return null; }
  return Math.min(Math.floor(v), MAX_STALLS) || 0;  // || 0 turns -0 into 0
}

// FAILURES THAT ARE NOT CRASHES (0.99.64): a radio that would not open, no sound
// output, an update or a plugin install that failed - counted, never described.
// The client sends ONE string of `token=count` pairs, e.g.
// "radio_fail.rtlsdr.busy=2,sound_ok.wasapi=1" (PRIVACY.md, "Failures that are not
// crashes"). THIS FILE IS WHAT KEEPS ARBITRARY TEXT OUT OF THE DATASET: a token is
// stored only if it is exactly an event of the vocabulary below followed by one
// word from each of that event's lists, and a hostile client can therefore write
// nothing here but combinations of those words and counts of 1..999.
//
// The vocabulary is held to the application's own (src/core/health_events.cpp)
// and to PRIVACY.md by tests/test_health_events.cpp, in both directions: it is
// STRICT JSON between the two markers so that test can read it. A word added to
// the application is added here FIRST - a word this file does not know is
// dropped, so deploy this Worker before shipping a client that sends one.
// BEGIN HEALTH VOCABULARY
const HEALTH_EVENTS = [
  { "name": "scan_none", "qualifiers": [] },
  { "name": "radio_open", "qualifiers": [["rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr", "rx888", "pluto", "aor", "soapy", "soundcard", "other"]] },
  { "name": "radio_data", "qualifiers": [["rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr", "rx888", "pluto", "aor", "soapy", "soundcard", "other"]] },
  { "name": "radio_fail", "qualifiers": [["rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr", "rx888", "pluto", "aor", "soapy", "soundcard", "other"], ["busy", "driver", "bind", "absent", "timeout", "vendor", "rate", "other"]] },
  { "name": "sound_ok", "qualifiers": [["mme", "dsound", "wasapi", "wdmks", "asio", "alsa", "jack", "oss", "coreaudio", "none", "other"]] },
  { "name": "sound_fail", "qualifiers": [["mme", "dsound", "wasapi", "wdmks", "asio", "alsa", "jack", "oss", "coreaudio", "none", "other"], ["nodevice", "busy", "format", "host", "other"]] },
  { "name": "upd_check", "qualifiers": [] },
  { "name": "upd_dl", "qualifiers": [] },
  { "name": "upd_verify", "qualifiers": [] },
  { "name": "upd_run", "qualifiers": [] },
  { "name": "plug_cat", "qualifiers": [] },
  { "name": "plug_inst", "qualifiers": [["net", "hash", "write", "other"]] },
  { "name": "plug_load", "qualifiers": [["abi", "retired", "load"]] },
  { "name": "rec_fail", "qualifiers": [] }
];
// END HEALTH VOCABULARY

// The application's own bounds (health::kMaxTokens, kMaxRadioFailTokens,
// kMaxCount, kMaxEncodedBytes): a record that exceeds them was not written by it.
const MAX_HEALTH_CHARS = 832;
const MAX_HEALTH_TOKENS = 24;
const MAX_HEALTH_RADIO_FAIL_TOKENS = 8;
const MAX_HEALTH_COUNT = 999;

// Every legal token, expanded, in written order -> its rank in that order. The
// rank is the sort key, so what is stored is always the canonical text.
const HEALTH_RANK = (() => {
  const rank = new Map();
  for (const ev of HEALTH_EVENTS) {
    let names = [ev.name];
    for (const words of ev.qualifiers) {
      names = names.flatMap((n) => words.map((w) => `${n}.${w}`));
    }
    for (const n of names) { rank.set(n, rank.size); }
  }
  return rank;
})();

// The record's `health` string, strictly parsed. Returns null when the client
// did not send a USABLE one - "not reported", which is not "reported nothing" -
// else { text, counts }: `text` the canonical re-encoding of what was legal in
// it, `counts` token -> n. Only a JSON string counts (a number, null, an array,
// an object or a boolean is not reported); an empty string is a real "nothing
// failed"; a string with no legal pair in it, or longer than the application
// ever writes, is not reported. A string with SOME legal pairs keeps those and
// drops the rest, so a client one vocabulary ahead still reports what this
// Worker understands. Repeats are merged, counts are clamped to 1..999, and no
// more than MAX_HEALTH_TOKENS distinct tokens (MAX_HEALTH_RADIO_FAIL_TOKENS of
// them radio failures) are kept.
function healthParse(v) {
  if (typeof v !== 'string' || v.length > MAX_HEALTH_CHARS) { return null; }
  if (v === '') { return { text: '', counts: new Map() }; }
  const counts = new Map();
  let radioFails = 0;
  for (const piece of v.split(',')) {
    const eq = piece.indexOf('=');
    if (eq < 0) { continue; }
    const token = piece.slice(0, eq);
    const digits = piece.slice(eq + 1);
    if (!HEALTH_RANK.has(token) || !/^[0-9]{1,4}$/.test(digits)) { continue; }
    const n = Math.min(Number(digits), MAX_HEALTH_COUNT);
    if (n < 1) { continue; }
    if (!counts.has(token)) {
      if (counts.size >= MAX_HEALTH_TOKENS) { continue; }
      if (token.startsWith('radio_fail.')) {
        if (radioFails >= MAX_HEALTH_RADIO_FAIL_TOKENS) { continue; }
        radioFails += 1;
      }
    }
    counts.set(token, Math.min((counts.get(token) || 0) + n, MAX_HEALTH_COUNT));
  }
  if (counts.size === 0) { return null; }
  const text = [...counts.keys()]
    .sort((a, b) => HEALTH_RANK.get(a) - HEALTH_RANK.get(b))
    .map((t) => `${t}=${counts.get(t)}`)
    .join(',');
  return { text, counts };
}

// The sum of the counts of every token of one event (all its drivers and reasons).
function healthSum(h, event) {
  if (h === null) { return 0; }
  let total = 0;
  for (const [token, n] of h.counts) {
    if (token === event || token.startsWith(`${event}.`)) { total += n; }
  }
  return total;
}

export default {
  async fetch(request, env) {
    if (request.method === 'OPTIONS') {
      return new Response(null, { status: 204 });
    }
    if (request.method !== 'POST') {
      return new Response('POST only', { status: 405 });
    }

    let body;
    try {
      const text = await request.text();
      if (text.length > MAX_BODY) {
        return new Response('too large', { status: 413 });
      }
      body = JSON.parse(text);
    } catch (e) {
      return new Response('bad json', { status: 400 });
    }

    // A report with no valid install id is refused rather than counted under
    // a made-up one - a bogus id would inflate the unique-install figure,
    // which is the number the whole thing exists to produce.
    const id = clamp(body.id, 32);
    if (!ID_RE.test(id)) {
      return new Response('bad id', { status: 400 });
    }

    // A heartbeat: "this install is running right now". Counted in its OWN
    // dataset, never in foxsdr_usage - every reader of the usage dataset
    // treats one row as one launch, and a beat every five minutes would
    // multiply launches, stability and daily actives by ~12 per running
    // hour. Nothing but the id, the version and the marker is accepted: a
    // beat must not be able to grow into a second session report.
    // "Running now" is then read with
    //   SELECT count(DISTINCT index1) FROM foxsdr_heartbeat
    //   WHERE timestamp > NOW() - INTERVAL '10' MINUTE
    if (body.beat === 1) {
      if (env.HEARTBEAT) {
        env.HEARTBEAT.writeDataPoint({
          indexes: [id],
          blobs: [clamp(body.v, 48)],
        });
      }
      return new Response(null, { status: 204 });
    }

    // The modes map is flattened to the single most-used mode plus its
    // seconds. Storing the whole map would need a blob per mode, and the
    // question it answers ("which demodulator do people actually use") is
    // answered by the top one.
    let topMode = '', topSec = 0;
    if (body.modes && typeof body.modes === 'object') {
      for (const [k, v] of Object.entries(body.modes)) {
        const s = num(v, 86400 * 30);
        if (s > topSec) { topSec = s; topMode = clamp(k, 8); }
      }
    }
    const plugins = Array.isArray(body.plugins)
      ? body.plugins.slice(0, 20).map((p) => clamp(p, 48)).join(',')
      : '';
    const panels = Array.isArray(body.panels)
      ? body.panels.slice(0, 10).map((p) => clamp(p, 16)).join(',')
      : '';
    const stalls = stallCount(body.stalls);
    const health = healthParse(body.health);

    env.USAGE.writeDataPoint({
      // index1 is the install id: the ONLY field that distinguishes one
      // installation from another, and it identifies a copy of the software,
      // not a person.
      indexes: [id],
      blobs: [
        // 48, not 16: a nightly version is
        // "0.57.0-nightly.20260819.b97092e" - 31 characters - and at 16 it
        // arrived in the dataset as "0.56.0-nightly.2", which identifies
        // neither the date nor the commit and collapses every nightly into one
        // bucket. Found by reading the first real usage report.
        clamp(body.v, 48),      // blob1  app version
        clamp(body.os, 40),     // blob2  OS and build
        clamp(body.arch, 8),    // blob3  architecture
        clamp(body.sdr, 32),    // blob4  SDR model, serial already stripped
        topMode,                // blob5  most-used demodulator
        plugins,                // blob6  installed plugins
        panels,                 // blob7  panels opened
        // 0.99.47: where the copy came from and when it first reported.
        // Empty from older clients; readers must treat '' as "not reported".
        clamp(body.ch, 12),     // blob8  install channel (store/installer/appimage/tarball/android)
        clamp(body.first, 10),  // blob9  first-run day, UTC, YYYY-MM-DD
        clamp(body.fv, 48),     // blob10 version that created the install id
        // Display stalls: blob11 says whether this client SENT a count at all,
        // and double5 is the count. Both, because a row written before this
        // existed, and a row from an old client, have no value in either column,
        // and a count alone cannot tell "reported zero" from "never reported".
        // (What an unwritten column reads back as is not something this file
        // relies on - Cloudflare's documentation does not say - which is why
        // the marker is a value that is only ever WRITTEN when the field was
        // sent: a reader asks for blob11 = '1' and treats every other row as
        // unmeasured.) Appended AFTER the existing columns: no earlier column
        // changed meaning.
        stalls === null ? '' : '1',  // blob11 '1' = the client reported a stall count
        // 0.99.64: failures that are not crashes. blob12 is the marker, exactly as
        // blob11 is for stalls ('1' only when the client SENT a usable `health`
        // string, '' for an old client and for a malformed field - never
        // "reported nothing"), and blob13 the canonical `token=count` text of
        // what was legal in it ('' when nothing failed). Appended AFTER the
        // existing columns: no earlier column changed meaning.
        health === null ? '' : '1',        // blob12 '1' = the client reported failure counts
        health === null ? '' : health.text,  // blob13 the tokens, vocabulary only
      ],
      doubles: [
        num(body.launches, 1e6),    // double1  launches since install
        num(body.crashes, 1e6),     // double2  unclean exits since install (lifetime, never reset)
        num(body.sessionSec, 86400 * 30),  // double3  last session length
        topSec,                     // double4  seconds in the top mode
        // double5  display stalls this record carries: stalls the client has
        // counted and the server has not yet been told (carried until a record
        // is accepted, so a failed send does not lose them). Per ROW, so a
        // window's total is sum(double5) - not a lifetime figure like double2.
        // Meaningful only where blob11 = '1'.
        stalls === null ? 0 : stalls,
        // double6..double9 (0.99.64): the headline sums of the tokens in blob13,
        // computed HERE from the validated tokens - never a number the client
        // sent beside them, so they cannot disagree with the text. Per ROW, like
        // double5. Meaningful only where blob12 = '1'.
        healthSum(health, 'radio_fail'),  // double6  radio opens that failed, all drivers and reasons
        healthSum(health, 'radio_open'),  // double7  radio opens that succeeded
        healthSum(health, 'radio_data'),  // double8  opened radios whose samples arrived
        healthSum(health, 'sound_ok'),    // double9  sessions in which the speakers played
      ],
    });

    // 204: the client neither needs nor is given anything back. No cookie, no
    // identifier issued by the server, nothing to correlate against.
    return new Response(null, { status: 204 });
  },
};
