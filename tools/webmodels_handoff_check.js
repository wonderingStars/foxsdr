// Run: node tools/webmodels_handoff_check.js   (exit 0 = pass). Not part of ctest: the hand-off text is not shipped code until the web page takes it.
// Extracts the JavaScript blocks from docs/webmodels-handoff.md and runs them in node against fake browser objects.
// Checks: the viewport functions behave as the C++ model's tests say; the media session and wake lock snippets run, register handlers,
// re-acquire on visibilitychange, and degrade silently when the APIs are missing or throw.
const fs = require('fs');
const md = fs.readFileSync(require('path').join(__dirname, '..', 'docs', 'webmodels-handoff.md'), 'utf8');
const blocks = [...md.matchAll(/```js\n([\s\S]*?)```/g)].map(m => m[1]);
let fails = 0, checks = 0;
function check(c, msg) { checks++; if (!c) { fails++; console.log('FAIL', msg); } }
const near = (a, b, t = 1e-9) => Math.abs(a - b) <= t;

// ---- block 0: viewport ----
const vp = new Function(blocks[0] + '; return {clampView, zoomAt, wheelFactor, panByPixels, pointerToOffsetHz};')();
const N = 4096;
let v = vp.clampView({ first: -5, count: 100 }, N); check(v.first === 0 && v.count === 100, 'clamp low');
v = vp.clampView({ first: 4000, count: 500 }, N); check(v.first === 3596 && v.count === 500, 'clamp high');
v = vp.clampView({ first: NaN, count: 100 }, N); check(v.first === 0 && v.count === N, 'clamp NaN');
v = vp.clampView({ first: 0, count: 1 }, N); check(v.count === 16, 'clamp narrow');
let z = vp.zoomAt({ first: 0, count: N }, N, 0.5, 4); check(near(z.count, 1024) && near(z.first, 1536), 'zoom anchor 0.5');
let mid = { first: 1000, count: 2000 };
let back = vp.zoomAt(vp.zoomAt(mid, N, 0.5, 2), N, 0.5, 0.5); check(near(back.first, 1000, 1e-6) && near(back.count, 2000, 1e-6), 'zoom round trip');
check(vp.zoomAt(mid, N, 0.5, NaN).first === 1000, 'zoom NaN factor');
check(near(vp.wheelFactor(-100), 1.25) && near(vp.wheelFactor(100), 0.8) && vp.wheelFactor(-1e6) === 4 && near(vp.wheelFactor(1e6), 0.25) && vp.wheelFactor(NaN) === 1, 'wheel factor');
let p = vp.panByPixels({ first: 1000, count: 1000 }, N, 100, 400); check(near(p.first, 750), 'pan right shows lower bins');
p = vp.panByPixels({ first: 1000, count: 1000 }, N, -1e9, 400); check(p.first === N - 1000, 'pan clamps');
check(vp.panByPixels({ first: 1000, count: 1000 }, N, 10, 0).first === 1000, 'pan zero width');
check(near(vp.pointerToOffsetHz({ first: 0, count: N }, N, 2.4e6, 400, 800), 0), 'pointer middle = centre');
check(near(vp.pointerToOffsetHz({ first: 1024, count: 1024 }, N, 2.4e6, 0, 800), (0.25 - 0.5) * 2.4e6), 'pointer zoomed left edge');
check(near(vp.pointerToOffsetHz({ first: 0, count: N }, N, 2.4e6, 5000, 800), 0.5 * 2.4e6), 'pointer clamped');
// random walk stays legal
let w = { first: 0, count: N }, bad = 0, s = 12345;
const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff; };
const specials = [NaN, Infinity, -Infinity, 0, 1e300, -1e300];
for (let i = 0; i < 20000; i++) {
  const a = rnd() < 0.09 ? specials[Math.floor(rnd() * 6)] : (rnd() - 0.5) * 6000;
  const k = Math.floor(rnd() * 3);
  w = k === 0 ? vp.zoomAt(w, N, rnd(), vp.wheelFactor(a)) : k === 1 ? vp.panByPixels(w, N, a, 800) : vp.clampView({ first: a, count: (rnd() - 0.5) * 6000 }, N);
  if (!(isFinite(w.first) && isFinite(w.count) && w.first >= 0 && w.count >= 16 && w.first + w.count <= N + 1e-9)) bad++;
}
check(bad === 0, 'random walk legal (' + bad + ')');

// ---- blocks 1 (media session + wake lock) ----
const code = blocks.find(b => b.includes('setupMediaSession'));
function world(opts) {
  const log = [];
  const handlers = {};
  const listeners = {};
  const nav = {};
  if (opts.mediaSession !== false) nav.mediaSession = { metadata: null, playbackState: 'none', setActionHandler: (n, f) => { if (opts.throwOn === n) throw new Error('unsupported'); handlers[n] = f; } };
  let held = 0;
  const sentinels = [];
  if (opts.wakeLock !== false) nav.wakeLock = { request: async (t) => { if (opts.denyWake) throw new Error('NotAllowedError'); held++; const sl = {}; const l = []; sl.addEventListener = (e, f) => l.push(f); sl.release = async () => { held--; l.forEach(f => f()); }; sl.fire = () => { held--; l.forEach(f => f()); }; sentinels.push(sl); return sl; } };
  const btn = { hidden: false, textContent: '', attrs: {}, setAttribute(k, v) { this.attrs[k] = v; } };
  const doc = { visibilityState: 'visible', addEventListener: (e, f) => { (listeners[e] = listeners[e] || []).push(f); } };
  const store = {};
  const ls = { setItem: (k, v) => { if (opts.lsThrows) throw new Error('blocked'); store[k] = v; }, getItem: k => store[k] };
  const clicks = [];
  const g = { navigator: nav, document: doc, localStorage: ls, MediaMetadata: function (o) { Object.assign(this, o); }, $: id => id === 'wakebtn' ? btn : { click: () => clicks.push(id) }, lastRunning: false };
  const fn = new Function(...Object.keys(g), code + '; return {setupMediaSession, updateMediaSession, setWakeWanted, acquireWakeLock, refreshWakeButton, wl: () => wakeLock};');
  return { api: fn(...Object.values(g)), nav, handlers, listeners, doc, held: () => held, btn, sentinels, store, clicks };
}
(async () => {
  // normal case
  let W = world({});
  W.api.setupMediaSession();
  check(W.nav.mediaSession.metadata && W.nav.mediaSession.metadata.title === 'FoxSDR', 'metadata set');
  check(typeof W.handlers.play === 'function' && typeof W.handlers.pause === 'function' && typeof W.handlers.stop === 'function', 'handlers registered');
  W.handlers.play(); check(W.clicks.length === 1, 'play clicks Start when stopped');
  W.api.updateMediaSession({ freqHz: 7100000, mode: 'LSB', running: true });
  check(W.nav.mediaSession.metadata.title === '7.1000 MHz LSB' && W.nav.mediaSession.playbackState === 'playing', 'metadata follows status');
  W.api.updateMediaSession({ freqHz: 7100000, mode: 'LSB', running: false });
  check(W.nav.mediaSession.playbackState === 'paused', 'paused state');
  await W.api.setWakeWanted(true);
  check(W.held() === 1 && W.btn.attrs['aria-pressed'] === 'true' && W.btn.textContent === 'Screen stays on', 'wake lock held');
  // the page goes hidden: the system releases it; on visible it is re-acquired
  W.sentinels[0].fire(); check(W.held() === 0 && W.btn.attrs['aria-pressed'] === 'false', 'released by the system');
  W.doc.visibilityState = 'hidden'; for (const f of W.listeners.visibilitychange) await f(); check(W.held() === 0, 'hidden: not requested');
  W.doc.visibilityState = 'visible'; for (const f of W.listeners.visibilitychange) await f(); check(W.held() === 1, 'visible again: re-acquired');
  await W.api.setWakeWanted(false); check(W.held() === 0, 'turned off releases');
  check(W.store.wakeWanted === '0', 'preference stored');
  // not wanted: becoming visible does not request
  for (const f of W.listeners.visibilitychange) await f(); check(W.held() === 0, 'not wanted: nothing requested');
  // degrade silently
  W = world({ mediaSession: false, wakeLock: false });
  W.api.setupMediaSession(); W.api.updateMediaSession({ freqHz: 1, mode: 'AM', running: true });
  await W.api.setWakeWanted(true); check(W.btn.hidden === true && W.held() === 0, 'no APIs: button hidden, no throw');
  W = world({ throwOn: 'stop' }); W.api.setupMediaSession(); check(typeof W.handlers.play === 'function', 'one unsupported action does not stop the others');
  W = world({ denyWake: true }); await W.api.setWakeWanted(true); check(W.held() === 0 && W.api.wl() === null, 'denied wake lock leaves no lock and no throw');
  W = world({ lsThrows: true }); await W.api.setWakeWanted(true); check(W.held() === 1, 'localStorage blocked does not break the lock');
  console.log('webmodels-jscheck: ' + checks + ' checks, ' + fails + ' failed');
  process.exit(fails ? 1 : 0);
})();
