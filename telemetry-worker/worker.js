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
      ],
      doubles: [
        num(body.launches, 1e6),    // double1  launches since install
        num(body.crashes, 1e6),     // double2  unclean exits since last report
        num(body.sessionSec, 86400 * 30),  // double3  last session length
        topSec,                     // double4  seconds in the top mode
      ],
    });

    // 204: the client neither needs nor is given anything back. No cookie, no
    // identifier issued by the server, nothing to correlate against.
    return new Response(null, { status: 204 });
  },
};
