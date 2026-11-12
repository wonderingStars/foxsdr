// Checks ONE address: HEAD (GET of the first byte when a server refuses HEAD), following
// redirects BY HAND so that a redirect to a host that is not in the row's allowed hosts is
// recorded and NEVER requested. PURE apart from the fetch it is given: no platform API, so it
// runs unchanged in a Worker and under node --test with a stand-in.
//
// WHY BY HAND. A hijacked or sold domain can answer the old address with a redirect to
// something else. With redirect:"follow" the checker would contact that host before it could
// judge it, and the host it ended on would be learned only afterwards. Here the Location is read,
// its host is compared with the allowed list, and only then is the next request made.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

export const USER_AGENT = 'FoxSDR-link-check';   // no contact details, by design
export const MAX_HOPS = 5;
export const DEFAULT_TIMEOUT_MS = 8000;

// A server that refuses HEAD says so with one of these; then ONE ranged GET asks for byte 0.
const HEAD_REFUSED = new Set([400, 403, 405, 501]);

// Outcomes. 'ok' and 'rate-limited' are not failures; the next four are; 'foreign-host' is the
// moved-host rule; 'skipped-budget' means the run ran out of subrequests before this address.
export const OUTCOMES = ['ok', 'http-error', 'network-error', 'timeout', 'too-many-redirects', 'foreign-host', 'rate-limited', 'skipped-budget'];

// One run shares one budget: Workers count every fetch and every KV call as a subrequest, and
// the free plan allows 50 per invocation (developers.cloudflare.com/workers/platform/limits).
export function makeBudget(limit) {
  return { left: Math.max(0, Math.floor(limit)), used: 0 };
}
function take(budget) {
  if (budget.left <= 0) return false;
  budget.left -= 1;
  budget.used += 1;
  return true;
}

function clampText(v, max) {
  return typeof v === 'string' ? v.slice(0, max) : null;
}

function sizeOf(res) {
  const range = res.headers.get('content-range');
  if (res.status === 206 && range) {
    const m = /\/(\d+)\s*$/.exec(range);
    if (m) return Number(m[1]);
  }
  const len = res.headers.get('content-length');
  if (len !== null && /^\d+$/.test(len)) return Number(len);
  return null;
}

async function dropBody(res) {
  try { await res.body?.cancel(); } catch { /* nothing to free */ }
}

function isTimeout(e) {
  return e && (e.name === 'TimeoutError' || e.name === 'AbortError');
}

// -> { outcome, http, host, size, etag, requests, reason? }
//   outcome  one of OUTCOMES
//   http     the last HTTP status seen, or null
//   host     the host the chain ended on (for 'foreign-host': the host that was NOT requested)
export async function probeAddress(url, allowedHosts, { fetchImpl, budget, timeoutMs = DEFAULT_TIMEOUT_MS, allowHttp = false }) {
  const obs = { outcome: 'ok', http: null, host: null, size: null, etag: null, requests: 0 };
  let current;
  try { current = new URL(url); } catch { return { ...obs, outcome: 'network-error', reason: 'bad address' }; }
  let method = 'HEAD';
  let hops = 0;

  for (;;) {
    obs.host = current.hostname;
    if ((current.protocol !== 'https:' && !(allowHttp && current.protocol === 'http:'))) {
      return { ...obs, outcome: 'foreign-host', reason: 'not https' };
    }
    // The host test comes BEFORE the request. For the registry's own address it can only fail
    // if the registry were invalid; for a redirect target it is the moved-host rule.
    const hostKey = allowHttp ? current.host : current.hostname;
    // A port other than 443 is a different service on the same name: foreign.
    if ((!allowHttp && current.port !== '') || !allowedHosts.includes(hostKey)) {
      return { ...obs, outcome: 'foreign-host', reason: hops === 0 ? 'address host not allowed' : 'redirected to a host not in allowed_hosts' };
    }
    if (!take(budget)) return { ...obs, outcome: 'skipped-budget' };
    obs.requests += 1;

    const headers = { 'User-Agent': USER_AGENT, Accept: '*/*' };
    if (method === 'GET') headers.Range = 'bytes=0-0';
    let res;
    try {
      res = await fetchImpl(current.href, { method, headers, redirect: 'manual', signal: AbortSignal.timeout(timeoutMs) });
    } catch (e) {
      return { ...obs, outcome: isTimeout(e) ? 'timeout' : 'network-error' };
    }
    obs.http = res.status;

    if (res.status >= 300 && res.status < 400) {
      await dropBody(res);
      const loc = res.headers.get('location');
      if (!loc) return { ...obs, outcome: 'http-error', reason: 'redirect without a Location' };
      hops += 1;
      if (hops > MAX_HOPS) return { ...obs, outcome: 'too-many-redirects' };
      try { current = new URL(loc, current); } catch { return { ...obs, outcome: 'http-error', reason: 'bad Location' }; }
      continue;
    }
    if (method === 'HEAD' && HEAD_REFUSED.has(res.status)) {
      await dropBody(res);
      method = 'GET';
      continue;      // the same address, one ranged GET
    }
    obs.size = sizeOf(res);
    obs.etag = clampText(res.headers.get('etag'), 200);
    await dropBody(res);
    if (res.status === 429) return { ...obs, outcome: 'rate-limited' };
    if (res.status >= 200 && res.status < 300) return obs;
    return { ...obs, outcome: 'http-error' };
  }
}
