// web_touch_drive.mjs - LANE X, row G5-17: the browser page on a phone-sized touch screen, driven through the Chrome DevTools Protocol of a HEADLESS browser (no window exists).
//
//   node web_touch_drive.mjs <port> <profile-dir> [<edge or chrome path>]
//
// Prints ONE line of JSON on stdout: what the page did under real touch input (Input.dispatchTouchEvent), measured in the page itself. test_web_touch.cpp starts the server, runs this, and asserts
// on the line. Nothing leaves this machine: the page is the test's own server on 127.0.0.1 and the browser is the one already installed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';

const [, , portArg, profile, browserArg] = process.argv;
const port = Number(portArg);
const candidates = [browserArg, 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Google/Chrome/Application/chrome.exe'].filter(Boolean);
const exe = candidates.find((p) => existsSync(p));
if (!exe) { console.log(JSON.stringify({ skipped: 'no browser installed' })); process.exit(0); }

const dbg = 9300 + Math.floor(Math.random() * 400);
const child = spawn(exe, ['--headless=new', `--remote-debugging-port=${dbg}`, `--user-data-dir=${profile}`, '--no-first-run', '--disable-gpu', '--no-default-browser-check', 'about:blank'], { stdio: 'ignore' });
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const out = {};
let ws = null;
try {
  let wsUrl = null;
  for (let i = 0; i < 100 && !wsUrl; ++i) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${dbg}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) { wsUrl = page.webSocketDebuggerUrl; }
    } catch (_) { /* not up yet */ }
    if (!wsUrl) { await sleep(100); }
  }
  if (!wsUrl) { throw new Error('the browser did not come up'); }
  ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('websocket')); });
  let id = 0;
  const pending = new Map();
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && pending.has(d.id)) { pending.get(d.id)(d); pending.delete(d.id); }
  };
  const send = (method, params = {}) => new Promise((res) => { const n = ++id; pending.set(n, res); ws.send(JSON.stringify({ id: n, method, params })); });
  const ev = async (expr) => {
    const r = await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
    if (r.result && r.result.exceptionDetails) { throw new Error('page error: ' + JSON.stringify(r.result.exceptionDetails.exception || r.result.exceptionDetails)); }
    return r.result && r.result.result ? r.result.result.value : undefined;
  };
  const touch = async (type, x, y) => send('Input.dispatchTouchEvent', { type, touchPoints: type === 'touchEnd' || type === 'touchCancel' ? [] : [{ x, y, id: 1 }] });
  const tap = async (x, y) => { await touch('touchStart', x, y); await sleep(40); await touch('touchEnd', x, y); await sleep(120); };
  const swipe = async (x, y0, y1) => {
    await touch('touchStart', x, y0);
    const steps = 10;
    for (let i = 1; i <= steps; ++i) { await sleep(15); await touch('touchMove', x, y0 + ((y1 - y0) * i) / steps); }
    await touch('touchEnd', x, y1);
    await sleep(150);
  };

  await send('Page.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await sleep(1500);
  // a spy on the one way the page talks to the receiver: what the finger made the page ask for
  await ev(`window.__ctl = []; const f0 = window.fetch; window.fetch = function (u, o) { if (String(u).indexOf('/api/control') >= 0 && o && o.body) { window.__ctl.push(JSON.parse(o.body)); } return f0.apply(this, arguments); }; true`);
  out.coarse = await ev(`matchMedia('(pointer: coarse)').matches`);
  out.width = await ev(`window.innerWidth`);
  out.overflowX = await ev(`document.documentElement.scrollWidth - window.innerWidth`);

  // target sizes: every visible button and field is at least 44 px each way (a key hidden by the page is not a target)
  out.small = await ev(`(() => { const bad = []; document.querySelectorAll('button, select, input[type=text], input[type=password]').forEach((e) => { const r = e.getBoundingClientRect(); if (r.width === 0 || r.height === 0) return; const cs = getComputedStyle(e); if (cs.visibility === 'hidden') return; if (r.height < 43.5 || (e.tagName === 'BUTTON' && r.width < 43.5)) bad.push((e.id || e.className || e.tagName) + ':' + Math.round(r.width) + 'x' + Math.round(r.height)); }); return bad.slice(0, 12); })()`);

  // THE DIGITS: a tap on the upper half of a digit steps it up, the lower half down, a swipe up steps up once per 14 px
  const centreBefore = await ev(`freqShownHz`);
  out.centreBefore = centreBefore;
  const digit = await ev(`(() => { const d = document.querySelectorAll('#freqDigits span.d')[3]; d.scrollIntoView({ block: 'center' }); const r = d.getBoundingClientRect(); return { x: r.left + r.width / 2, top: r.top, h: r.height, decade: d.dataset.decade }; })()`);
  out.digitDecade = digit.decade;
  await tap(digit.x, digit.top + digit.h * 0.2);
  await tap(digit.x, digit.top + digit.h * 0.8);
  await swipe(digit.x, digit.top + digit.h * 0.5, digit.top + digit.h * 0.5 - 35);   // 35 px up = two steps
  out.digitControls = await ev(`window.__ctl.filter((c) => 'centerHz' in c).map((c) => c.centerHz)`);

  // THE DIVIDER between the spectrum and the waterfall: a finger drag moves it
  const split = await ev(`(() => { const h = document.getElementById('splitScope'); h.scrollIntoView({ block: 'center' }); const r = h.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2, h: r.height, spec: document.getElementById('spectrum').getBoundingClientRect().height }; })()`);
  out.splitHandleH = split.h;
  await swipe(split.x, split.y, split.y - 40);
  out.specBefore = split.spec;
  out.specAfter = await ev(`document.getElementById('spectrum').getBoundingClientRect().height`);

  // TAPPING THE SPECTRUM: a tap makes the click the page's click-to-tune listens for (it needs a spectrum frame to act on it, which this stand-in server has none of)
  await ev(`window.__clicks = 0; document.getElementById('spectrum').addEventListener('click', () => { window.__clicks += 1; }); true`);
  const sp = await ev(`(() => { const c = document.getElementById('spectrum'); c.scrollIntoView({ block: 'center' }); const r = c.getBoundingClientRect(); return { x: r.left + r.width * 0.75, y: r.top + r.height / 2 }; })()`);
  await tap(sp.x, sp.y);
  out.spectrumTapClicks = await ev(`window.__clicks`);

  // THE COLUMN'S GRIP exists in the wide layout only (below 820 px the column is stacked under the picture), so it is tried on a tablet-sized touch screen
  await send('Emulation.setDeviceMetricsOverride', { width: 1100, height: 800, deviceScaleFactor: 1, mobile: true });
  await send('Page.navigate', { url: `http://127.0.0.1:${port}/` });
  await sleep(1500);
  const grip = await ev(`(() => { const g = document.getElementById('sideGrip'); if (!g) return null; const r = g.getBoundingClientRect(); return { x: r.left + r.width / 2, y: Math.max(10, r.top + 30), w: r.width, var: getComputedStyle(document.documentElement).getPropertyValue('--side') }; })()`);
  if (grip) {
    out.gripW = grip.w;
    await touch('touchStart', grip.x, grip.y);
    for (let i = 1; i <= 8; ++i) { await sleep(15); await touch('touchMove', grip.x + i * 6, grip.y); }
    await touch('touchEnd', grip.x + 48, grip.y);
    await sleep(150);
    out.gripVarBefore = grip.var;
    out.gripVarAfter = await ev(`getComputedStyle(document.documentElement).getPropertyValue('--side')`);
  }
  out.ok = true;
} catch (e) {
  out.error = String(e && e.message ? e.message : e);
} finally {
  try { if (ws) { ws.close(); } } catch (_) { /* closing */ }
  try { spawn('taskkill', ['/PID', String(child.pid), '/T', '/F'], { stdio: 'ignore' }); } catch (_) { /* gone */ }   // the browser this script started, by its own PID, and its helpers
}
console.log(JSON.stringify(out));
process.exit(0);
