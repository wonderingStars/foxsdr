import assert from 'node:assert/strict';
import { test } from 'node:test';
import { probeAddress, makeBudget, USER_AGENT, MAX_HOPS } from '../src/probe.js';
import { scriptedFetch } from './helpers.mjs';

const HOSTS = ['demo.example', 'cdn.demo.example'];
const run = (url, script, extra = {}) => {
  const fetchImpl = scriptedFetch(script);
  const budget = makeBudget(extra.budget ?? 20);
  return probeAddress(url, HOSTS, { fetchImpl, budget, timeoutMs: extra.timeoutMs ?? 2000 }).then((obs) => ({ obs, fetchImpl, budget }));
};

test('good path: one HEAD, ok, size and etag recorded, generic user agent, no cookies', async () => {
  const { obs, fetchImpl } = await run('https://demo.example/p', {
    'HEAD https://demo.example/p': { status: 200, headers: { 'content-length': '1234', etag: '"abc"' } },
  });
  assert.deepEqual(obs, { outcome: 'ok', http: 200, host: 'demo.example', size: 1234, etag: '"abc"', requests: 1 });
  assert.equal(fetchImpl.calls.length, 1);
  assert.equal(fetchImpl.calls[0].method, 'HEAD');
  assert.equal(fetchImpl.calls[0].headers['User-Agent'], USER_AGENT);
  assert.equal(USER_AGENT, 'FoxSDR-link-check');
  assert.equal(fetchImpl.calls[0].headers.Cookie, undefined);
});

test('a server that refuses HEAD gets ONE ranged GET, size from Content-Range', async () => {
  for (const refusal of [405, 403, 501, 400]) {
    const { obs, fetchImpl } = await run('https://demo.example/p', {
      'HEAD https://demo.example/p': { status: refusal },
      'GET https://demo.example/p': { status: 206, headers: { 'content-range': 'bytes 0-0/99999' } },
    });
    assert.equal(obs.outcome, 'ok', String(refusal));
    assert.equal(obs.size, 99999);
    assert.equal(obs.requests, 2);
    assert.equal(fetchImpl.calls[1].headers.Range, 'bytes=0-0');
  }
});

test('a redirect that stays on an allowed host is followed; the end host is recorded', async () => {
  const { obs, fetchImpl } = await run('https://demo.example/old', {
    'HEAD https://demo.example/old': { status: 301, headers: { location: 'https://cdn.demo.example/new' } },
    'HEAD https://cdn.demo.example/new': { status: 200, headers: { 'content-length': '5' } },
  });
  assert.equal(obs.outcome, 'ok');
  assert.equal(obs.host, 'cdn.demo.example');
  assert.equal(fetchImpl.calls.length, 2);
});

test('a relative redirect resolves against the current address', async () => {
  const { obs } = await run('https://demo.example/old', {
    'HEAD https://demo.example/old': { status: 302, headers: { location: '/moved/here' } },
    'HEAD https://demo.example/moved/here': { status: 200 },
  });
  assert.equal(obs.outcome, 'ok');
});

test('a redirect to a FOREIGN host is recorded and that host is NEVER requested', async () => {
  const { obs, fetchImpl } = await run('https://demo.example/old', {
    'HEAD https://demo.example/old': { status: 302, headers: { location: 'https://evil.example/download' } },
    'HEAD https://evil.example/download': { status: 200 },
    'GET https://evil.example/download': { status: 200 },
  });
  assert.equal(obs.outcome, 'foreign-host');
  assert.equal(obs.host, 'evil.example');
  assert.equal(fetchImpl.calls.length, 1, 'only the registry address was requested');
  assert.ok(fetchImpl.calls.every((c) => !c.url.includes('evil.example')));
});

test('look-alike hosts are foreign: suffix and prefix tricks do not pass', async () => {
  for (const loc of ['https://evildemo.example/', 'https://demo.example.evil.example/', 'https://demo.example@evil.example/', 'https://demo.example:8443/x']) {
    const { obs } = await run('https://demo.example/old', { 'HEAD https://demo.example/old': { status: 302, headers: { location: loc } } });
    assert.equal(obs.outcome, 'foreign-host', loc);
  }
});

test('a redirect down to http is not followed', async () => {
  const { obs, fetchImpl } = await run('https://demo.example/old', {
    'HEAD https://demo.example/old': { status: 302, headers: { location: 'http://demo.example/plain' } },
  });
  assert.equal(obs.outcome, 'foreign-host');
  assert.equal(obs.reason, 'not https');
  assert.equal(fetchImpl.calls.length, 1);
});

test('redirect loops stop at the hop limit', async () => {
  const { obs } = await run('https://demo.example/a', {
    '* https://demo.example/a': { status: 302, headers: { location: 'https://demo.example/b' } },
    '* https://demo.example/b': { status: 302, headers: { location: 'https://demo.example/a' } },
  });
  assert.equal(obs.outcome, 'too-many-redirects');
  assert.ok(obs.requests <= MAX_HOPS + 1);
});

test('a redirect without a Location is an error', async () => {
  const { obs } = await run('https://demo.example/a', { 'HEAD https://demo.example/a': { status: 302 } });
  assert.equal(obs.outcome, 'http-error');
});

test('timeout: a server that never answers ends as "timeout" and the run moves on', async () => {
  const t0 = Date.now();
  const { obs } = await run('https://demo.example/slow', { 'HEAD https://demo.example/slow': { hangs: true } }, { timeoutMs: 60 });
  assert.equal(obs.outcome, 'timeout');
  assert.ok(Date.now() - t0 < 1500);
});

test('network errors, 404, 410, 500 are failures; 429 is neither pass nor failure', async () => {
  const net = await run('https://demo.example/p', { 'HEAD https://demo.example/p': { throws: new TypeError('connection refused') } });
  assert.equal(net.obs.outcome, 'network-error');
  for (const status of [404, 410, 500, 502]) {
    const { obs } = await run('https://demo.example/p', { 'HEAD https://demo.example/p': { status } });
    assert.equal(obs.outcome, 'http-error', String(status));
    assert.equal(obs.http, status);
  }
  const limited = await run('https://demo.example/p', { 'HEAD https://demo.example/p': { status: 429 } });
  assert.equal(limited.obs.outcome, 'rate-limited');
});

test('a registry address on a host that is not allowed is never requested', async () => {
  const fetchImpl = scriptedFetch({});
  const obs = await probeAddress('https://elsewhere.example/', HOSTS, { fetchImpl, budget: makeBudget(5) });
  assert.equal(obs.outcome, 'foreign-host');
  assert.equal(fetchImpl.calls.length, 0);
});

test('the subrequest budget is honoured: no request is made once it is spent', async () => {
  const fetchImpl = scriptedFetch({ '* https://demo.example/p': { status: 200 } });
  const budget = makeBudget(1);
  const first = await probeAddress('https://demo.example/p', HOSTS, { fetchImpl, budget });
  const second = await probeAddress('https://demo.example/p', HOSTS, { fetchImpl, budget });
  assert.equal(first.outcome, 'ok');
  assert.equal(second.outcome, 'skipped-budget');
  assert.equal(fetchImpl.calls.length, 1);
  assert.equal(budget.used, 1);
});

test('a garbage address is a network error, not a crash', async () => {
  const obs = await probeAddress('not an address', HOSTS, { fetchImpl: scriptedFetch({}), budget: makeBudget(3) });
  assert.equal(obs.outcome, 'network-error');
});
