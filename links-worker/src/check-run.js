// One daily run of the checker over the registry. PURE apart from the fetch it is given and the
// state object it is handed (the Worker reads that from KV before and writes it after).
//
// Rows are taken OLDEST-CHECKED FIRST and the run stops asking when its subrequest budget is
// spent, so a registry bigger than one run's budget is still covered completely over a few days
// instead of the same first rows being checked every night.
//
// At most one check per address per run: two rows that name the same address under the same
// allowed hosts share one observation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import { applyRun, liveFor } from './decide.js';
import { makeBudget, probeAddress, DEFAULT_TIMEOUT_MS } from './probe.js';

export const DEFAULT_BUDGET = 45;       // + 1 KV read + 1 KV write = 47, under the free plan's 50
export const DEFAULT_CONCURRENCY = 3;   // Workers allow 6 open connections; 3 leaves room for KV

export async function runChecks({ registry, state, fetchImpl, now, budgetLimit = DEFAULT_BUDGET, timeoutMs = DEFAULT_TIMEOUT_MS, concurrency = DEFAULT_CONCURRENCY, allowHttp = false }) {
  const nowIso = new Date(now).toISOString();
  const budget = makeBudget(budgetLimit);
  const next = {
    v: 1,
    last_run: nowIso,
    last_run_stats: { rows_checked: 0, rows_skipped: 0, requests: 0 },
    ids: {},
  };
  // Carry the state of every row still in the registry; a row that was removed is forgotten.
  const rows = registry.libraries;
  for (const row of rows) {
    if (state?.ids?.[row.id]) next.ids[row.id] = state.ids[row.id];
  }

  const todo = rows
    .filter((r) => r.page_url !== null)
    .map((r, i) => ({ r, i }))
    .sort((a, b) => {
      // a row whose address changed since it was last looked at counts as never checked
      const la = liveFor(a.r, next.ids[a.r.id]);
      const lb = liveFor(b.r, next.ids[b.r.id]);
      const ta = la?.last_check ? Date.parse(la.last_check) : -1;
      const tb = lb?.last_check ? Date.parse(lb.last_check) : -1;
      return ta - tb || a.i - b.i;
    })
    .map((x) => x.r);

  const cache = new Map();
  const probe = (url, hosts) => {
    const key = `${url}|${[...hosts].sort().join(',')}`;
    if (!cache.has(key)) cache.set(key, probeAddress(url, hosts, { fetchImpl, budget, timeoutMs, allowHttp }));
    return cache.get(key);
  };

  const results = new Map();
  let cursor = 0;
  async function worker() {
    while (cursor < todo.length) {
      const row = todo[cursor];
      cursor += 1;
      // A row is judged on ALL its addresses or not at all: if the budget cannot cover them, do not
      // spend part of it on a row that will then conclude nothing.
      if (budget.left < (row.download ? 2 : 1)) {
        const skipped = { outcome: 'skipped-budget', http: null, host: null, size: null, etag: null, requests: 0 };
        results.set(row.id, { page: skipped, download: row.download ? skipped : null });
        continue;
      }
      const page = await probe(row.page_url, row.allowed_hosts);
      const download = row.download ? await probe(row.download.url, row.allowed_hosts) : null;
      results.set(row.id, { page, download });
    }
  }
  await Promise.all(Array.from({ length: Math.max(1, concurrency) }, worker));

  for (const row of todo) {
    const { page, download } = results.get(row.id);
    const before = next.ids[row.id];
    const after = applyRun(before, row, page, download, nowIso);
    if (after === before || after.last_check === null) {
      next.last_run_stats.rows_skipped += 1;     // out of budget: nothing concluded, nothing stored
    } else {
      next.last_run_stats.rows_checked += 1;
      next.ids[row.id] = after;
    }
  }
  next.last_run_stats.requests = budget.used;
  return next;
}
