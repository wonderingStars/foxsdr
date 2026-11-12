// The probe against REAL fetch and a REAL HTTP server on 127.0.0.1 (never a wider bind, never an
// outside address): what the scripted stand-ins cannot show - that Node's fetch hands back a 3xx
// when redirect is "manual", honours the abort signal, and drops a body unread.
import assert from 'node:assert/strict';
import http from 'node:http';
import { after, before, test } from 'node:test';
import { makeBudget, probeAddress } from '../src/probe.js';

let server;
let base;
let foreignHits = 0;
const seen = [];

before(async () => {
  server = http.createServer((req, res) => {
    seen.push(`${req.method} ${req.url} UA=${req.headers['user-agent']} range=${req.headers.range ?? ''}`);
    if (req.url === '/ok') { res.writeHead(200, { 'Content-Length': '42', ETag: '"loop"' }); res.end(req.method === 'HEAD' ? undefined : 'x'.repeat(42)); }
    else if (req.url === '/nohead') {
      if (req.method === 'HEAD') { res.writeHead(405); res.end(); }
      else { res.writeHead(206, { 'Content-Range': 'bytes 0-0/7777', 'Content-Length': '1' }); res.end('x'); }
    }
    else if (req.url === '/redir') { res.writeHead(302, { Location: '/ok' }); res.end(); }
    else if (req.url === '/foreign') { res.writeHead(302, { Location: `http://localhost:${server.address().port}/trap` }); res.end(); }
    else if (req.url === '/trap') { foreignHits += 1; res.writeHead(200); res.end(); }
    else if (req.url === '/gone') { res.writeHead(404); res.end(); }
    else if (req.url === '/hang') { /* never answers */ }
    else { res.writeHead(500); res.end(); }
  });
  await new Promise((ok) => server.listen(0, '127.0.0.1', ok));
  base = `http://127.0.0.1:${server.address().port}`;
});
after(async () => {
  server.closeAllConnections();
  await new Promise((ok) => server.close(ok));
});

const probe = (path, timeoutMs = 2000) => probeAddress(`${base}${path}`, [`127.0.0.1:${server.address().port}`], { fetchImpl: fetch, budget: makeBudget(10), timeoutMs, allowHttp: true });

test('real fetch: HEAD ok with size and etag, generic user agent', async () => {
  const obs = await probe('/ok');
  assert.equal(obs.outcome, 'ok');
  assert.equal(obs.size, 42);
  assert.equal(obs.etag, '"loop"');
  assert.ok(seen.some((l) => l.startsWith('HEAD /ok UA=FoxSDR-link-check')));
});

test('real fetch: a refused HEAD falls back to one ranged GET', async () => {
  const obs = await probe('/nohead');
  assert.equal(obs.outcome, 'ok');
  assert.equal(obs.size, 7777);
  assert.ok(seen.some((l) => l.startsWith('GET /nohead') && l.includes('range=bytes=0-0')));
});

test('real fetch: a redirect inside the allowed host is followed by hand', async () => {
  const obs = await probe('/redir');
  assert.equal(obs.outcome, 'ok');
  assert.equal(obs.requests, 2);
});

test('real fetch: a redirect to another host name (localhost, not 127.0.0.1) is foreign and never requested', async () => {
  const obs = await probe('/foreign');
  assert.equal(obs.outcome, 'foreign-host');
  assert.equal(foreignHits, 0);
});

test('real fetch: 404 is an error; a server that never answers is a timeout', async () => {
  assert.equal((await probe('/gone')).outcome, 'http-error');
  const t0 = Date.now();
  assert.equal((await probe('/hang', 150)).outcome, 'timeout');
  assert.ok(Date.now() - t0 < 3000);
});
