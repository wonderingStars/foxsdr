// The decisions: what a run of observations means for a row, the three-strikes rule, the
// moved-host rule, the changed-download flag, the combined status the program and the website
// see, and the admin report. PURE: no fetch, no KV, no clock (the caller passes `now`).
//
// THE LIVE STATE NEVER EDITS THE REGISTRY. The registry is changed by a person, validated and
// deployed. What this file produces is an OVERLAY - "what the daily check saw" - and the status
// anyone is shown is the WORSE of the registry's own status and the overlay's (effectiveStatus).
// So a redirect to a foreign host can only ever make a link LESS offered, never steer it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

export const FAIL_LIMIT = 3;            // failed runs in a row before a row is `broken`
export const HISTORY_MAX = 30;          // results kept per id
export const STALE_AFTER_MS = 36 * 3600 * 1000;   // a checker that has not run for this long is itself a problem
export const CHANGED_SHOWN_DAYS = 14;

const SEVERITY = { ok: 0, unverified: 1, moved: 2, broken: 3 };

export function worseStatus(a, b) {
  return (SEVERITY[b] ?? 0) > (SEVERITY[a] ?? 0) ? b : a;
}

// What the overlay is about: the addresses a row had when it was checked. When the registry gives a
// row a NEW address (an update was deployed), what was seen at the old one says nothing about it:
// the overlay is ignored until the next daily run has looked at the new address.
export function addrKey(row) {
  return `${row.page_url ?? ''}|${row.download ? row.download.url : ''}`;
}

export function liveFor(row, live) {
  return live && live.addr === addrKey(row) ? live : null;
}

// The status to show and to act on: the registry's, made worse by what the checker saw.
export function effectiveStatus(row, live) {
  return worseStatus(row.status, liveFor(row, live)?.status ?? 'ok');
}

const FAILURES = new Set(['http-error', 'network-error', 'timeout', 'too-many-redirects']);

// What one run (the page observation, and the download observation when the row has one) means.
//   moved         a redirect ended on a host that is not allowed
//   failed        an address could not be reached or answered with an error
//   skipped       the run ran out of budget before finishing this row: nothing is concluded
//   inconclusive  an address asked us to slow down (HTTP 429): neither a pass nor a failure
//   ok            every address answered 2xx on an allowed host
export function summariseRun(observations) {
  const outcomes = observations.map((o) => o.outcome);
  if (outcomes.includes('foreign-host')) return 'moved';
  if (outcomes.some((o) => FAILURES.has(o))) return 'failed';
  if (outcomes.includes('skipped-budget')) return 'skipped';
  if (outcomes.includes('rate-limited')) return 'inconclusive';
  return 'ok';
}

export function newRowState() {
  return { status: 'ok', fails: 0, last_check: null, last_ok: null, moved_host: null, changed: null, changed_seen: null, baseline: { size: null, etag: null }, page: null, download: null, history: [] };
}

function compact(o) {
  return o ? { o: o.outcome, http: o.http, host: o.host, size: o.size, etag: o.etag } : null;
}

// Applies one run to a row's state. `row` is the registry row (for the pinned checksum and size),
// `page` and `download` the observations (download null when the row has none).
export function applyRun(prev, row, page, download, nowIso) {
  const observations = download ? [page, download] : [page];
  const result = summariseRun(observations);
  if (result === 'skipped') return prev ?? newRowState();   // nothing concluded, nothing recorded

  const addr = addrKey(row);
  const st = prev && prev.addr === addr ? JSON.parse(JSON.stringify(prev)) : newRowState();
  st.addr = addr;
  st.last_check = nowIso;
  st.page = compact(page);
  st.download = compact(download);

  if (result === 'moved') {
    st.status = 'moved';
    st.moved_host = (observations.find((o) => o.outcome === 'foreign-host') || {}).host ?? null;
  } else if (result === 'failed') {
    st.fails += 1;
    if (st.fails >= FAIL_LIMIT) st.status = 'broken';
  } else if (result === 'ok') {
    st.fails = 0;
    st.status = 'ok';
    st.moved_host = null;
    st.last_ok = nowIso;
  }

  // A changed download, judged only where the registry pins a checksum (then a changed file
  // means the pin is stale, or the file was swapped).
  if (download && download.outcome === 'ok' && row.download && row.download.sha256) {
    const refSize = row.download.size_bytes ?? st.baseline.size;
    const sizeDiff = download.size !== null && refSize !== null && download.size !== refSize;
    const etagDiff = !!download.etag && !!st.baseline.etag && download.etag !== st.baseline.etag;
    if (sizeDiff || etagDiff) {
      st.changed = { at: nowIso, size: download.size, etag: download.etag, expected_size: refSize };
      st.changed_seen = nowIso;
    } else {
      st.changed = null;
    }
    st.baseline = { size: download.size ?? st.baseline.size, etag: download.etag ?? st.baseline.etag };
  }

  st.history.push({ t: nowIso, r: result, page: compact(page), download: compact(download) });
  if (st.history.length > HISTORY_MAX) st.history = st.history.slice(st.history.length - HISTORY_MAX);
  return st;
}

export function isStale(state, nowMs) {
  if (!state || !state.last_run) return true;
  return nowMs - Date.parse(state.last_run) > STALE_AFTER_MS;
}

// GET /v1/status.json: per id, the combined status and the time of the last check. Nothing else.
export function publicStatus(registry, state, nowMs) {
  const libraries = {};
  for (const row of registry.libraries) {
    const live = liveFor(row, state?.ids?.[row.id]);
    libraries[row.id] = { status: effectiveStatus(row, live), checked: live?.last_check ?? null };
  }
  return {
    schema_version: 1,
    registry_version: registry.registry_version,
    last_run: state?.last_run ?? null,
    stale: isStale(state, nowMs),
    libraries,
  };
}

// GET /admin/links.json: every row that is not fine, with what was seen.
export function adminReport(registry, state, nowMs) {
  const rows = [];
  let ok = 0;
  for (const row of registry.libraries) {
    const live = liveFor(row, state?.ids?.[row.id]);
    const status = effectiveStatus(row, live);
    const recentlyChanged = live?.changed_seen && nowMs - Date.parse(live.changed_seen) < CHANGED_SHOWN_DAYS * 86400000;
    const hasAddress = row.page_url !== null;
    const neverChecked = hasAddress && !live?.last_check;
    const problem = status !== 'ok' || (live && live.fails > 0) || !!live?.changed || !!recentlyChanged || neverChecked;
    if (!problem) { ok += 1; continue; }
    rows.push({
      id: row.id,
      name: row.name,
      publisher: row.publisher,
      status,
      registry_status: row.status,
      registry_note: row.status_note ?? null,
      live_status: live?.status ?? null,
      failed_runs_in_a_row: live?.fails ?? 0,
      never_checked: neverChecked,
      last_check: live?.last_check ?? null,
      last_ok: live?.last_ok ?? null,
      moved_to_host: live?.moved_host ?? null,
      changed: live?.changed ?? null,
      changed_seen: live?.changed_seen ?? null,
      page_url: row.page_url,
      download_url: row.download ? row.download.url : null,
      allowed_hosts: row.allowed_hosts,
      last_seen: { page: live?.page ?? null, download: live?.download ?? null },
      recent_history: (live?.history ?? []).slice(-5),
    });
  }
  return {
    schema_version: 1,
    generated: new Date(nowMs).toISOString(),
    registry_version: registry.registry_version,
    last_run: state?.last_run ?? null,
    last_run_stats: state?.last_run_stats ?? null,
    checker_stale: isStale(state, nowMs),
    rows_ok: ok,
    rows_needing_a_look: rows.length,
    rows,
  };
}
