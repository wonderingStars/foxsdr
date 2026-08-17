// Implementation of the web server. See net/web_server.hpp for the design, and
// net/web_policy.hpp for the bind rules this file obeys rather than restates.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <thread>

#include <nlohmann/json.hpp>

#include <httplib.h>

namespace cascade::net {
namespace {

// The session cookie. SameSite=Strict rather than Lax because every request
// this API serves is same-origin by construction, and Strict is what stops a
// page on another origin navigating the browser into a state-changing request
// with the cookie attached. HttpOnly keeps it away from any script that manages
// to run on the page. Secure is deliberately ABSENT: this server has no TLS, so
// marking the cookie Secure would stop the browser sending it at all.
constexpr char kCookieName[] = "foxsdr_session";

// Bodies this API accepts are small JSON objects. A cap this low means a
// hostile client cannot make the server buffer anything worth mentioning.
constexpr std::size_t kMaxPayloadBytes = 8 * 1024;

std::string cookieAttributes() {
    return std::string("; Path=/; HttpOnly; SameSite=Strict");
}

// Extracts one cookie value from a Cookie header. Deliberately small and
// strict: it matches "<name>=" only at a value boundary, so a cookie called
// "evil_foxsdr_session" cannot be read as "foxsdr_session".
std::string cookieValue(const std::string& header, const std::string& name) {
    std::size_t pos = 0;
    while (pos < header.size()) {
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == ';')) {
            ++pos;
        }
        const std::size_t eq = header.find('=', pos);
        if (eq == std::string::npos) {
            return std::string();
        }
        const std::string key = header.substr(pos, eq - pos);
        std::size_t end = header.find(';', eq + 1);
        if (end == std::string::npos) {
            end = header.size();
        }
        if (key == name) {
            return header.substr(eq + 1, end - eq - 1);
        }
        pos = end + 1;
    }
    return std::string();
}

// Quantises a dB spectrum to one byte per bin over the fixed display range.
// One byte is exactly what a waterfall needs, and it keeps a 1024-bin frame at
// about 1.4 KB base64 — small enough to poll at video rates over a LAN and
// still reasonable through a tunnel.
std::vector<std::uint8_t> quantiseSpectrum(const std::vector<float>& db) {
    std::vector<std::uint8_t> out;
    out.reserve(db.size());
    const float span = kSpectrumDbMax - kSpectrumDbMin;
    for (float v : db) {
        float t = (v - kSpectrumDbMin) / span;
        if (!(t > 0.0f)) {  // also catches NaN, which must not become 255
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
        out.push_back(static_cast<std::uint8_t>(std::lround(t * 255.0f)));
    }
    return out;
}

// --- Static client -----------------------------------------------------------
// Served from three routes rather than one inlined blob so the page can carry a
// strict Content-Security-Policy: with the script and stylesheet at their own
// origins-relative URLs, neither 'unsafe-inline' nor 'unsafe-eval' is needed.

constexpr char kIndexHtml[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FoxSDR</title>
<link rel="stylesheet" href="/app.css">
</head>
<body>
<header><h1>FoxSDR</h1><span id="remote" class="badge hidden">remote access</span><span id="link" class="badge warn hidden">disconnected</span></header>
<section id="login" class="hidden">
  <h2>Sign in</h2>
  <form id="loginForm">
    <label>User <input id="user" autocomplete="username" required></label>
    <label>Password <input id="pass" type="password" autocomplete="current-password" required></label>
    <button type="submit">Sign in</button>
  </form>
  <p id="loginError" class="error"></p>
</section>
<section id="app" class="hidden">
  <div id="status"></div>
  <canvas id="spectrum" width="1024" height="256" title="click to tune the VFO here"></canvas>
  <canvas id="waterfall" width="1024" height="220"></canvas>
  <div id="rds" class="hidden"></div>
  <div id="controls">
    <div class="row">
      <button id="playstop">Start</button>
      <button id="listen">Listen</button>
      <label>Centre (MHz)<input id="centre" type="number" step="0.000001"></label>
      <button id="tune">Tune</button>
    </div>
    <div id="modes" class="row"></div>
    <div class="row">
      <label>VFO offset <span id="vfoVal"></span><input id="vfo" type="range" min="-500" max="500" step="1"></label>
      <label>Bandwidth <span id="bwVal"></span><input id="bw" type="range" min="0" max="5" step="1"></label>
    </div>
    <div class="row">
      <label>Squelch <span id="sqVal"></span><input id="sq" type="range" min="-120" max="0" step="1"></label>
      <label>Volume <span id="volVal"></span><input id="vol" type="range" min="0" max="1" step="0.01"></label>
    </div>
    <div class="row">
      <label>Min dB <span id="dbMinVal"></span><input id="dbMin" type="range" min="-160" max="-20" step="1"></label>
      <label>Max dB <span id="dbMaxVal"></span><input id="dbMax" type="range" min="-100" max="20" step="1"></label>
    </div>
    <div class="row" id="fmRow">
      <label>De-emphasis<select id="deemph">
        <option value="0">50 us</option><option value="1">75 us</option><option value="2">off</option>
      </select></label>
      <label class="check"><input id="stereo" type="checkbox"> Stereo</label>
    </div>
    <div class="row">
      <label class="check"><input id="nr" type="checkbox"> Noise reduction</label>
      <label>Strength <span id="nrVal"></span><input id="nrStrength" type="range" min="0" max="1" step="0.01"></label>
    </div>
    <div class="row">
      <label class="check"><input id="notch" type="checkbox"> Notch</label>
      <label>Notch Hz <span id="notchVal"></span><input id="notchFreq" type="range" min="10" max="8000" step="10"></label>
      <label class="check"><input id="autoNotch" type="checkbox"> Auto-notch <span id="anState" class="dim"></span></label>
    </div>
    <p id="ctlError" class="error"></p>
  </div>
  <button id="logout" class="hidden">Sign out</button>
</section>
<script src="/app.js"></script>
</body>
</html>
)HTML";

constexpr char kAppCss[] = R"CSS(
:root { color-scheme: dark; --bg:#12151a; --fg:#e6e9ee; --dim:#8a93a0; --accent:#4fb0ff; }
* { box-sizing: border-box; }
body { margin:0; padding:1rem; background:var(--bg); color:var(--fg);
       font:14px/1.5 system-ui, sans-serif; }
header { display:flex; align-items:center; gap:.75rem; margin-bottom:1rem; }
h1 { font-size:1.1rem; margin:0; letter-spacing:.02em; }
h2 { font-size:1rem; margin:0 0 .75rem; }
.hidden { display:none; }
.badge { background:#7a2020; color:#ffd7d7; padding:.15rem .5rem; border-radius:3px;
         font-size:.75rem; text-transform:uppercase; letter-spacing:.06em; }
.badge.warn { background:#5a4a10; color:#ffe9a8; }
.error { color:#ff9b9b; min-height:1.5em; }
form { display:flex; flex-direction:column; gap:.5rem; max-width:20rem; }
label { display:flex; flex-direction:column; gap:.2rem; color:var(--dim); font-size:.8rem; }
input { background:#1c2029; border:1px solid #2c3240; color:var(--fg);
        padding:.4rem; border-radius:3px; font:inherit; }
button { background:var(--accent); color:#04121f; border:0; padding:.45rem .9rem;
         border-radius:3px; font:inherit; font-weight:600; cursor:pointer; }
canvas { width:100%; height:auto; background:#080a0d; border:1px solid #232833;
         border-radius:3px; image-rendering:pixelated; }
#status { display:grid; grid-template-columns:repeat(auto-fit,minmax(9rem,1fr));
          gap:.5rem; margin-bottom:.75rem; }
.cell { background:#171b22; border:1px solid #232833; border-radius:3px; padding:.4rem .6rem; }
.cell .k { color:var(--dim); font-size:.7rem; text-transform:uppercase;
           letter-spacing:.05em; display:block; }
.cell .v { font-variant-numeric:tabular-nums; }
#controls { margin-top:.75rem; display:flex; flex-direction:column; gap:.6rem; }
.row { display:flex; flex-wrap:wrap; gap:.75rem; align-items:flex-end; }
.row label { flex:1 1 10rem; }
.row input[type=range] { width:100%; }
#modes button { flex:0 0 auto; background:#1c2029; color:var(--fg); font-weight:400; }
#modes button.on { background:var(--accent); color:#04121f; font-weight:600; }
#centre { width:11rem; font-variant-numeric:tabular-nums; }
#spectrum { border-bottom:0; border-radius:3px 3px 0 0; cursor:crosshair; }
#waterfall { border-top:0; border-radius:0 0 3px 3px; }
.row label.check { flex:0 0 auto; flex-direction:row; align-items:center; gap:.35rem;
                   color:var(--fg); font-size:.85rem; }
.row label.check input { width:auto; }
select { background:#1c2029; border:1px solid #2c3240; color:var(--fg);
         padding:.35rem; border-radius:3px; font:inherit; }
.dim { color:var(--dim); font-size:.75rem; }
#rds { background:#171b22; border:1px solid #232833; border-radius:3px;
       padding:.5rem .6rem; margin-top:.5rem; display:grid; gap:.25rem;
       grid-template-columns:repeat(auto-fit,minmax(11rem,1fr)); }
#rds .rt { grid-column:1/-1; font-variant-numeric:tabular-nums; }
)CSS";

// THE CLIENT SCRIPT IS SPLIT ACROSS SEVERAL LITERALS because MSVC caps a single
// string literal at 16380 bytes and this one outgrew it. The split points are
// arbitrary — chosen at section boundaries for readability — and the pieces are
// joined once, at first request, by appJs() below. Nothing may rely on a piece
// being independently valid JavaScript.
constexpr char kAppJs1[] = R"JS(
'use strict';
const $ = (id) => document.getElementById(id);
let seq = 0, timer = null;

function showError(msg) { $('loginError').textContent = msg || ''; }

// EVERY network call goes through here or through control()/startListening(),
// and every one of them catches. The receiver is an application the user can
// close, so "the server went away" is an ORDINARY state of this page, not an
// exceptional one: without the catch, fetch rejects and — because the pollers
// run from setInterval, which does not await anything — each failure became an
// unhandled promise rejection, four a second, for as long as the tab stayed
// open. The page now simply shows "disconnected" and keeps trying, so it
// recovers by itself when the application comes back.
function setLinkUp(up) {
  const el = $('link');
  if (el) el.classList.toggle('hidden', up);
}

async function getJson(url) {
  try {
    const r = await fetch(url, { credentials: 'same-origin' });
    if (r.status === 401) { stopPolling(); await refreshSession(); return null; }
    setLinkUp(true);
    if (!r.ok) return null;
    return await r.json();
  } catch (e) {
    setLinkUp(false);
    return null;
  }
}

function fmtHz(hz) {
  if (!isFinite(hz)) return '-';
  if (Math.abs(hz) >= 1e6) return (hz / 1e6).toFixed(4) + ' MHz';
  if (Math.abs(hz) >= 1e3) return (hz / 1e3).toFixed(2) + ' kHz';
  return hz.toFixed(0) + ' Hz';
}

function cell(k, v) {
  return '<div class="cell"><span class="k"></span><span class="v"></span></div>'
    .replace('<span class="k"></span>', '<span class="k">' + k + '</span>')
    .replace('<span class="v"></span>', '<span class="v">' + v + '</span>');
}

)JS";

constexpr char kAppJs2[] = R"JS(
// --- Controls ---------------------------------------------------------------
// The wire vocabulary, which the server validates against dsp::modeFromName —
// so a name that drifts out of step here comes back as an explicit 400 rather
// than a button that quietly does nothing.
const MODES = ['NFM', 'WFM', 'AM', 'DSB', 'USB', 'LSB', 'CW', 'RAW'];
// The bandwidth slider steps through the same presets the desktop panel offers,
// widest first, so the two surfaces expose the same choices.
const BWS = [200000, 150000, 12500, 10000, 6000, 3000];
let currentMode = '';

async function control(patch) {
  $('ctlError').textContent = '';
  let r;
  try {
    r = await fetch('/api/control', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(patch),
    });
  } catch (e) {
    setLinkUp(false);
    $('ctlError').textContent = 'Not connected to the receiver.';
    return false;
  }
  setLinkUp(true);
  if (r.status === 401) { stopPolling(); await refreshSession(); return false; }
  if (!r.ok) {
    let msg = 'Rejected.';
    try { const j = await r.json(); if (j && j.error) msg = j.error; } catch (_) {}
    $('ctlError').textContent = msg;
    return false;
  }
  // Accepted, not applied: the radio moves on the application's next frame and
  // the change shows up through the ordinary status poll. Nudging it here keeps
  // the page feeling immediate without pretending we know the outcome.
  setTimeout(pollStatus, 120);
  return true;
}

function buildModes() {
  const box = $('modes');
  if (box.childElementCount) return;
  MODES.forEach((m) => {
    const b = document.createElement('button');
    b.textContent = m;
    b.dataset.mode = m;
    b.addEventListener('click', () => control({ mode: m }));
    box.appendChild(b);
  });
}

// Only write a control the user is not currently holding: pushing a slider back
// under the thumb mid-drag is the classic remote-UI bug.
function syncControl(el, value) {
  if (document.activeElement === el) return;
  el.value = value;
}

function reflect(s) {
  $('playstop').textContent = s.running ? 'Stop' : 'Start';
  syncControl($('centre'), (s.centerHz / 1e6).toFixed(6));
  syncControl($('vfo'), Math.round(s.vfoOffsetHz / 1000));
  $('vfoVal').textContent = (s.vfoOffsetHz / 1000).toFixed(0) + ' kHz';
  let bwIdx = 0, best = Infinity;
  BWS.forEach((b, i) => {
    const d = Math.abs(b - s.bandwidthHz);
    if (d < best) { best = d; bwIdx = i; }
  });
  syncControl($('bw'), bwIdx);
  $('bwVal').textContent = (s.bandwidthHz / 1000).toFixed(1) + ' kHz';
  syncControl($('sq'), Math.round(s.squelchDb));
  $('sqVal').textContent = s.squelchDb.toFixed(0) + ' dB';
  syncControl($('vol'), s.volume);
  $('volVal').textContent = Math.round(s.volume * 100) + '%';

  // Display range drives the client-side colour mapping directly.
  viewDbMin = s.dbMin; viewDbMax = s.dbMax;
  syncControl($('dbMin'), Math.round(s.dbMin));
  syncControl($('dbMax'), Math.round(s.dbMax));
  $('dbMinVal').textContent = s.dbMin.toFixed(0);
  $('dbMaxVal').textContent = s.dbMax.toFixed(0);
  lastVfoHz = s.vfoOffsetHz;

  // De-emphasis and stereo are only meaningful in the FM modes; shown greyed
  // elsewhere so the setting stays discoverable without implying it does
  // anything to SSB — the same choice the desktop panel makes.
  const fm = (s.mode === 'NFM' || s.mode === 'WFM');
  $('deemph').disabled = !fm;
  $('stereo').disabled = !fm;
  syncControl($('deemph'), String(s.deemphasisIndex));
  if (document.activeElement !== $('stereo')) $('stereo').checked = s.stereoEnabled;

  if (document.activeElement !== $('nr')) $('nr').checked = s.nrEnabled;
  syncControl($('nrStrength'), s.nrStrength);
  $('nrVal').textContent = Math.round(s.nrStrength * 100) + '%';
  if (document.activeElement !== $('notch')) $('notch').checked = s.notchEnabled;
  syncControl($('notchFreq'), Math.round(s.notchFreqHz));
  $('notchVal').textContent = s.notchFreqHz.toFixed(0) + ' Hz';
  if (document.activeElement !== $('autoNotch')) $('autoNotch').checked = s.autoNotch;
  $('anState').textContent =
    s.autoNotch && s.autoNotchEngaged ? '(on ' + s.autoNotchFreqHz.toFixed(0) + ' Hz)' : '';

  // RDS, shown only when there is something to show.
  const showRds = s.mode === 'WFM' && (s.rdsSynced || s.pilotLocked);
  $('rds').classList.toggle('hidden', !showRds);
  if (showRds) {
    const bits = [];
    bits.push('<div><span class="dim">Pilot</span> ' + (s.pilotLocked ? 'locked' : '-') + '</div>');
    bits.push('<div><span class="dim">Stereo</span> ' + (s.stereoActive ? 'yes' : 'no') + '</div>');
    bits.push('<div><span class="dim">PS</span> ' + (s.rdsPsValid ? s.rdsPs : '-') + '</div>');
    bits.push('<div><span class="dim">PI</span> ' +
      (s.rdsPiValid ? '0x' + s.rdsPi.toString(16).toUpperCase() : '-') + '</div>');
    // The raw programme-type CODE, not a name: the code-to-name table differs
    // between RDS and RBDS and the receiver deliberately does not pick one.
    bits.push('<div><span class="dim">PTY</span> ' + s.rdsPty + '</div>');
    bits.push('<div><span class="dim">Groups</span> ' + s.rdsGroups +
      ' <span class="dim">err</span> ' + s.rdsErrors + '</div>');
    if (s.rdsRadioText) {
      bits.push('<div class="rt"><span class="dim">RT</span> ' + s.rdsRadioText + '</div>');
    }
    $('rds').innerHTML = bits.join('');
  }
  if (s.mode !== currentMode) {
    currentMode = s.mode;
    Array.from($('modes').children).forEach((b) => {
      b.classList.toggle('on', b.dataset.mode === s.mode);
    });
  }
}

)JS";

constexpr char kAppJs3[] = R"JS(
// --- Audio ------------------------------------------------------------------
// Raw 16-bit PCM arrives as an endless chunked response; Web Audio plays it.
// Scheduling is explicit rather than handing the stream to an <audio> element,
// because an <audio> element buffers for smoothness and would put the sound
// seconds behind the spectrum next to it.
let audioCtx = null, audioAbort = null, nextPlayTime = 0, pcmTail = null;

// How far ahead of the clock to keep the queue. Under this and any jitter
// becomes a dropout; much over it and the audio lags the waterfall visibly.
const AUDIO_LEAD = 0.25;

function stopListening() {
  if (audioAbort) { audioAbort.abort(); audioAbort = null; }
  if (audioCtx) { audioCtx.close(); audioCtx = null; }
  pcmTail = null;
  $('listen').textContent = 'Listen';
}

function playChunk(bytes) {
  // A chunk can split a sample across two reads; carry the odd byte over.
  let data = bytes;
  if (pcmTail && pcmTail.length) {
    data = new Uint8Array(pcmTail.length + bytes.length);
    data.set(pcmTail, 0);
    data.set(bytes, pcmTail.length);
  }
  const usable = data.length - (data.length % 2);
  pcmTail = usable < data.length ? data.slice(usable) : null;
  if (!usable) return;

  const view = new DataView(data.buffer, data.byteOffset, usable);
  const frames = usable / 2;
  const buf = audioCtx.createBuffer(1, frames, 48000);
  const ch = buf.getChannelData(0);
  for (let i = 0; i < frames; i++) ch[i] = view.getInt16(i * 2, true) / 32767;

  const src = audioCtx.createBufferSource();
  src.buffer = buf;
  src.connect(audioCtx.destination);
  const now = audioCtx.currentTime;
  // If we have fallen behind (a stall, a backgrounded tab), restart the
  // schedule at now + lead rather than piling up buffers that are already late.
  if (nextPlayTime < now + 0.02) nextPlayTime = now + AUDIO_LEAD;
  src.start(nextPlayTime);
  nextPlayTime += buf.duration;
}

async function startListening() {
  // An AudioContext may only be created from a user gesture, which is why
  // this is a button and not something the page does on load.
  audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  await audioCtx.resume();
  nextPlayTime = 0;
  pcmTail = null;
  audioAbort = new AbortController();
  $('listen').textContent = 'Stop audio';
  try {
    const r = await fetch('/api/audio', {
      credentials: 'same-origin',
      signal: audioAbort.signal,
    });
    if (!r.ok) {
      let msg = 'Audio unavailable.';
      try { const j = await r.json(); if (j && j.error) msg = j.error; } catch (_) {}
      $('ctlError').textContent = msg;
      stopListening();
      return;
    }
    const reader = r.body.getReader();
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      if (audioCtx) playChunk(value);
    }
  } catch (e) {
    if (e.name !== 'AbortError') $('ctlError').textContent = 'Audio stream ended.';
  }
  stopListening();
}

$('listen').addEventListener('click', () => {
  if (audioCtx) stopListening(); else startListening();
});

$('playstop').addEventListener('click', () => {
  control({ running: $('playstop').textContent === 'Start' });
});
$('tune').addEventListener('click', () => {
  const mhz = parseFloat($('centre').value);
  if (isFinite(mhz)) control({ centerHz: Math.round(mhz * 1e6) });
});
$('centre').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); $('tune').click(); }
});
$('vfo').addEventListener('change', () => {
  control({ vfoOffsetHz: parseFloat($('vfo').value) * 1000 });
});
$('vfo').addEventListener('input', () => {
  $('vfoVal').textContent = $('vfo').value + ' kHz';
});
$('bw').addEventListener('change', () => {
  control({ bandwidthHz: BWS[parseInt($('bw').value, 10)] });
});
$('sq').addEventListener('change', () => {
  control({ squelchDb: parseFloat($('sq').value) });
});
$('sq').addEventListener('input', () => {
  $('sqVal').textContent = $('sq').value + ' dB';
});
$('vol').addEventListener('change', () => {
  control({ volume: parseFloat($('vol').value) });
});
$('vol').addEventListener('input', () => {
  $('volVal').textContent = Math.round($('vol').value * 100) + '%';
});
$('dbMin').addEventListener('change', () => {
  control({ dbMin: parseFloat($('dbMin').value) });
});
$('dbMin').addEventListener('input', () => { $('dbMinVal').textContent = $('dbMin').value; });
$('dbMax').addEventListener('change', () => {
  control({ dbMax: parseFloat($('dbMax').value) });
});
$('dbMax').addEventListener('input', () => { $('dbMaxVal').textContent = $('dbMax').value; });
$('deemph').addEventListener('change', () => {
  control({ deemphasisIndex: parseInt($('deemph').value, 10) });
});
$('stereo').addEventListener('change', () => control({ stereoEnabled: $('stereo').checked }));
$('nr').addEventListener('change', () => control({ nrEnabled: $('nr').checked }));
$('nrStrength').addEventListener('change', () => {
  control({ nrStrength: parseFloat($('nrStrength').value) });
});
$('nrStrength').addEventListener('input', () => {
  $('nrVal').textContent = Math.round($('nrStrength').value * 100) + '%';
});
$('notch').addEventListener('change', () => control({ notchEnabled: $('notch').checked }));
$('notchFreq').addEventListener('change', () => {
  control({ notchFreqHz: parseFloat($('notchFreq').value) });
});
$('notchFreq').addEventListener('input', () => {
  $('notchVal').textContent = $('notchFreq').value + ' Hz';
});
$('autoNotch').addEventListener('change', () => control({ autoNotch: $('autoNotch').checked }));

async function pollStatus() {
  const s = await getJson('/api/status');
  if (!s) return;
  buildModes();
  reflect(s);
  $('status').innerHTML = [
    cell('Centre', fmtHz(s.centerHz)),
    cell('VFO offset', fmtHz(s.vfoOffsetHz)),
    cell('Mode', s.mode || '-'),
    cell('Bandwidth', fmtHz(s.bandwidthHz)),
    cell('Sample rate', fmtHz(s.sampleRateHz)),
    cell('Signal', s.signalDb.toFixed(1) + ' dB'),
    cell('Source', s.sourceName || '-'),
    cell('State', s.faulted ? 'FAULT' : (s.running ? 'running' : 'stopped')),
  ].join('');
}

// The wire quantises dB over a FIXED range so the encoding never has to change;
// the user's dbMin/dbMax is a DISPLAY preference. Mapping wire -> dB -> the
// user's range happens here, on the client, so moving the sliders re-colours
// what is already on screen instead of waiting for the server to re-encode.
let viewDbMin = -110, viewDbMax = 0;
let lastSpanHz = 0, lastVfoHz = 0;

function binToUnit(q, wireMin, wireMax) {
  const db = wireMin + (q / 255) * (wireMax - wireMin);
  const span = (viewDbMax - viewDbMin) || 1;
  const t = (db - viewDbMin) / span;
  return t < 0 ? 0 : (t > 1 ? 1 : t);
}

// Black -> blue -> cyan -> yellow -> red, the conventional waterfall ramp.
const WF_STOPS = [[0,0,0],[0,0,140],[0,180,200],[255,230,80],[255,60,40]];
function colormap(t) {
  const s = t * (WF_STOPS.length - 1);
  const i = Math.min(WF_STOPS.length - 2, Math.floor(s));
  const f = s - i, a = WF_STOPS[i], b = WF_STOPS[i + 1];
  return [a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f];
}

function drawSpectrum(bins, wireMin, wireMax) {
  const c = $('spectrum'), ctx = c.getContext('2d');
  const w = c.width, h = c.height, n = bins.length;
  ctx.clearRect(0, 0, w, h);

  // The VFO band, drawn first so the trace sits on top of it — the same
  // reading the desktop window gives: this is the slice being demodulated.
  if (lastSpanHz > 0) {
    const x = ((lastVfoHz / lastSpanHz) + 0.5) * w;
    ctx.fillStyle = 'rgba(79,176,255,0.16)';
    ctx.fillRect(x - 1, 0, 3, h);
    ctx.strokeStyle = 'rgba(79,176,255,0.55)';
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  }

  ctx.strokeStyle = '#4fb0ff';
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const x = (i / (n - 1)) * w;
    const y = h - binToUnit(bins[i], wireMin, wireMax) * h;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

function drawWaterfall(bins, wireMin, wireMax) {
  const c = $('waterfall'), ctx = c.getContext('2d');
  const w = c.width;
  ctx.drawImage(c, 0, 1);            // scroll everything down one row
  const row = ctx.createImageData(w, 1);
  for (let x = 0; x < w; x++) {
    const t = binToUnit(bins[Math.floor((x * bins.length) / w)], wireMin, wireMax);
    const rgb = colormap(t);
    row.data[x * 4] = rgb[0];
    row.data[x * 4 + 1] = rgb[1];
    row.data[x * 4 + 2] = rgb[2];
    row.data[x * 4 + 3] = 255;
  }
  ctx.putImageData(row, 0, 0);
}

async function pollSpectrum() {
  const s = await getJson('/api/spectrum?since=' + seq);
  if (!s || !s.bins) return;
  seq = s.seq;
  if (s.spanHz) lastSpanHz = s.spanHz;
  const raw = atob(s.bins);
  const bins = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bins[i] = raw.charCodeAt(i);
  drawSpectrum(bins, s.dbMin, s.dbMax);
  drawWaterfall(bins, s.dbMin, s.dbMax);
}

// Click the spectrum to move the VFO there — the desktop's click-to-tune. The
// x position maps to an offset from the CENTRE, which is exactly what
// vfoOffsetHz means, so no absolute-frequency arithmetic is needed.
$('spectrum').addEventListener('click', (e) => {
  if (!lastSpanHz) return;
  const r = e.currentTarget.getBoundingClientRect();
  const f = (e.clientX - r.left) / r.width;
  control({ vfoOffsetHz: Math.round((f - 0.5) * lastSpanHz) });
});

function startPolling() {
  if (timer) return;
  timer = setInterval(() => { pollStatus(); pollSpectrum(); }, 250);
  pollStatus(); pollSpectrum();
}
function stopPolling() { if (timer) { clearInterval(timer); timer = null; } }

async function refreshSession() {
  const s = await getJson('/api/session');
  if (!s) return;
  $('remote').classList.toggle('hidden', !s.reachableOffMachine);
  const needsLogin = s.authRequired && !s.authenticated;
  $('login').classList.toggle('hidden', !needsLogin);
  $('app').classList.toggle('hidden', needsLogin);
  $('logout').classList.toggle('hidden', !s.authRequired || !s.authenticated);
  if (needsLogin) stopPolling(); else startPolling();
}

$('loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  showError('');
  const r = await fetch('/api/login', {
    method: 'POST',
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: $('user').value, password: $('pass').value }),
  });
  if (r.ok) { $('pass').value = ''; await refreshSession(); return; }
  let msg = 'Sign-in failed.';
  try { const j = await r.json(); if (j && j.error) msg = j.error; } catch (_) {}
  showError(msg);
});

$('logout').addEventListener('click', async () => {
  await fetch('/api/logout', { method: 'POST', credentials: 'same-origin' });
  stopPolling();
  await refreshSession();
});

refreshSession();
)JS";

// The joined client script. Built once on first use — the pieces are compile
// -time constants, so there is nothing to invalidate.
const std::string& appJs() {
    static const std::string joined =
        std::string(kAppJs1) + kAppJs2 + kAppJs3;
    return joined;
}

}  // namespace

std::vector<std::string> localInterfaceAddresses() {
    std::vector<std::string> out;
    char host[256] = {0};
    if (::gethostname(host, sizeof(host) - 1) != 0) {
        return out;
    }
    ::addrinfo hints{};
    hints.ai_family = AF_INET;  // IPv4 only: this is a hint for a phone browser
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* result = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &result) != 0 || result == nullptr) {
        return out;
    }
    for (const ::addrinfo* a = result; a != nullptr; a = a->ai_next) {
        if (a->ai_family != AF_INET || a->ai_addr == nullptr) {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {0};
        const auto* sin = reinterpret_cast<const ::sockaddr_in*>(a->ai_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        const std::string addr(text);
        if (addr.rfind("127.", 0) == 0) {
            continue;  // loopback is never the answer to "reach me from my phone"
        }
        if (std::find(out.begin(), out.end(), addr) == out.end()) {
            out.push_back(addr);
        }
    }
    ::freeaddrinfo(result);
    return out;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class WebServer::Impl {
public:
    Impl() : clock_([]() { return static_cast<std::int64_t>(std::time(nullptr)); }) {}

    ~Impl() { stop(); }

    bool setStatusProvider(StatusProvider fn) {
        if (running()) {
            return false;
        }
        status_ = std::move(fn);
        return true;
    }

    bool setSpectrumProvider(SpectrumProvider fn) {
        if (running()) {
            return false;
        }
        spectrum_ = std::move(fn);
        return true;
    }

    bool setClock(Clock fn) {
        if (running() || !fn) {
            return false;
        }
        clock_ = std::move(fn);
        return true;
    }

    bool start(const WebServerConfig& cfg, std::string& error);
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    int boundPort() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return boundPort_;
    }

    BindDecision decision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return decision_;
    }

    std::size_t sessionCount() const { return sessions_.size(); }
    void revokeAllSessions() { sessions_.revokeAll(); }

    void pushAudio(const float* samples, std::size_t n) { audio_.write(samples, n); }
    std::size_t audioListeners() const {
        return static_cast<std::size_t>(listeners_.load(std::memory_order_relaxed));
    }

    std::vector<ControlRequest> takePendingControls() {
        std::lock_guard<std::mutex> lock(controlMutex_);
        std::vector<ControlRequest> out(pending_.begin(), pending_.end());
        pending_.clear();
        return out;
    }

private:
    void installRoutes(httplib::Server& svr);

    // True when this request carries a live session, or when the binding does
    // not require authentication at all.
    bool authorised(const httplib::Request& req) const;

    // Writes a 401 with a JSON body. One place, so no handler can invent a
    // different shape the client does not understand.
    static void deny(httplib::Response& res, const std::string& message,
                     int status = 401);

    std::int64_t now() const { return clock_(); }

    StatusProvider status_;
    SpectrumProvider spectrum_;
    Clock clock_;

    mutable std::mutex mutex_;
    WebServerConfig cfg_;
    BindDecision decision_;
    int boundPort_ = -1;
    PasswordRecord record_;
    bool hasRecord_ = false;

    mutable SessionStore sessions_;
    mutable LoginThrottle throttle_;

    // Accepted control requests awaiting the application's next frame.
    mutable std::mutex controlMutex_;
    std::deque<ControlRequest> pending_;

    // Live audio for listening browsers, and how many are currently streaming.
    mutable AudioRing audio_;
    std::atomic<int> listeners_{0};

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

void WebServer::Impl::deny(httplib::Response& res, const std::string& message,
                           int status) {
    nlohmann::json j;
    j["error"] = message;
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

bool WebServer::Impl::authorised(const httplib::Request& req) const {
    BindDecision d;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        d = decision_;
    }
    if (!d.authRequired) {
        return true;
    }
    const std::string token = cookieValue(req.get_header_value("Cookie"), kCookieName);
    return sessions_.validate(token, now());
}

void WebServer::Impl::installRoutes(httplib::Server& svr) {
    // Headers applied to every response. nosniff because several routes serve
    // caller-influenced JSON, and no-store because nothing here is worth a
    // cache entry and a cached /api/status is a stale readout.
    svr.set_post_routing_handler(
        [](const httplib::Request&, httplib::Response& res) {
            res.set_header("X-Content-Type-Options", "nosniff");
            res.set_header("Cache-Control", "no-store");
            res.set_header("Referrer-Policy", "no-referrer");
            res.set_header("X-Frame-Options", "DENY");
        });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        // The strict policy the three-route split above buys: no inline script,
        // no inline style, no external origins of any kind, and no framing.
        res.set_header("Content-Security-Policy",
                       "default-src 'none'; script-src 'self'; style-src 'self'; "
                       "connect-src 'self'; img-src 'self' data:; "
                       "form-action 'none'; frame-ancestors 'none'; base-uri 'none'");
        res.set_content(kIndexHtml, "text/html; charset=utf-8");
    });

    svr.Get("/app.css", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kAppCss, "text/css; charset=utf-8");
    });

    svr.Get("/app.js", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(appJs(), "text/javascript; charset=utf-8");
    });

    // What the client needs before it can decide whether to show a login form.
    // Unauthenticated by necessity, and it deliberately reveals nothing beyond
    // whether a credential is needed and whether this one is held.
    svr.Get("/api/session", [this](const httplib::Request& req, httplib::Response& res) {
        BindDecision d;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            d = decision_;
        }
        nlohmann::json j;
        j["authRequired"] = d.authRequired;
        j["authenticated"] = authorised(req);
        j["reachableOffMachine"] = d.reachableOffMachine;
        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/api/login", [this](const httplib::Request& req, httplib::Response& res) {
        // Requiring JSON is a CSRF defence as much as a parsing convenience: a
        // cross-origin HTML form can only send urlencoded, plain-text or
        // multipart bodies, so a request that reaches here with this
        // content-type came from script bound by the same-origin policy.
        if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
            deny(res, "expected a JSON body", 415);
            return;
        }

        const std::string client = req.remote_addr;
        const std::int64_t t = now();
        if (!throttle_.allow(client, t)) {
            res.set_header("Retry-After",
                           std::to_string(throttle_.retryAfterSeconds(client, t)));
            deny(res, "too many failed sign-in attempts; try again later", 429);
            return;
        }

        std::string user;
        std::string password;
        try {
            const nlohmann::json j = nlohmann::json::parse(req.body);
            user = j.value("username", std::string());
            password = j.value("password", std::string());
        } catch (const std::exception&) {
            deny(res, "malformed request", 400);
            return;
        }

        PasswordRecord record;
        std::string expectedUser;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!hasRecord_) {
                deny(res, "no password is set on this receiver", 409);
                return;
            }
            record = record_;
            expectedUser = cfg_.username;
        }

        std::string verifyError;
        const bool ok = (user == expectedUser) && verifyPassword(password, record, verifyError);
        if (!ok) {
            throttle_.recordFailure(client, t);
            // One message for a wrong user AND a wrong password: telling them
            // apart confirms which account names exist.
            deny(res, "wrong user name or password", 401);
            return;
        }

        std::string token;
        std::string issueError;
        if (!sessions_.issue(t, token, issueError)) {
            deny(res, "could not create a session (" + issueError + ")", 500);
            return;
        }
        throttle_.recordSuccess(client);
        res.set_header("Set-Cookie", std::string(kCookieName) + "=" + token +
                                         cookieAttributes());
        nlohmann::json j;
        j["ok"] = true;
        res.set_content(j.dump(), "application/json");
    });

    svr.Post("/api/logout", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string token = cookieValue(req.get_header_value("Cookie"), kCookieName);
        sessions_.revoke(token);
        // Expire the cookie regardless, so a token this server has never heard
        // of does not linger in the browser.
        res.set_header("Set-Cookie", std::string(kCookieName) + "=" +
                                         cookieAttributes() +
                                         "; Max-Age=0");
        nlohmann::json j;
        j["ok"] = true;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authorised(req)) {
            deny(res, "sign in first");
            return;
        }
        RadioStatus s;
        if (status_) {
            s = status_();
        }
        nlohmann::json j;
        j["running"] = s.running;
        j["faulted"] = s.faulted;
        j["faultMessage"] = s.faultMessage;
        j["centerHz"] = s.centerHz;
        j["sampleRateHz"] = s.sampleRateHz;
        j["vfoOffsetHz"] = s.vfoOffsetHz;
        j["bandwidthHz"] = s.bandwidthHz;
        j["mode"] = s.mode;
        j["sourceName"] = s.sourceName;
        j["signalDb"] = s.signalDb;
        j["stereoActive"] = s.stereoActive;
        j["squelchDb"] = s.squelchDb;
        j["volume"] = s.volume;
        j["dbMin"] = s.dbMin;
        j["dbMax"] = s.dbMax;
        j["deemphasisIndex"] = s.deemphasisIndex;
        j["nrEnabled"] = s.nrEnabled;
        j["nrStrength"] = s.nrStrength;
        j["notchEnabled"] = s.notchEnabled;
        j["notchFreqHz"] = s.notchFreqHz;
        j["notchQ"] = s.notchQ;
        j["autoNotch"] = s.autoNotch;
        j["autoNotchEngaged"] = s.autoNotchEngaged;
        j["autoNotchFreqHz"] = s.autoNotchFreqHz;
        j["stereoEnabled"] = s.stereoEnabled;
        j["pilotLocked"] = s.pilotLocked;
        j["rdsSynced"] = s.rdsSynced;
        j["rdsPiValid"] = s.rdsPiValid;
        j["rdsPi"] = s.rdsPi;
        j["rdsPsValid"] = s.rdsPsValid;
        j["rdsPs"] = s.rdsPs;
        j["rdsRadioText"] = s.rdsRadioText;
        j["rdsPty"] = s.rdsPty;
        j["rdsTp"] = s.rdsTp;
        j["rdsTa"] = s.rdsTa;
        j["rdsGroups"] = s.rdsGroups;
        j["rdsErrors"] = s.rdsErrors;
        res.set_content(j.dump(), "application/json");
    });

    // Live audio as an endless chunked response of 16-bit little-endian PCM.
    //
    // WHY RAW PCM AND NOT A CODEC. Every codec worth streaming is either
    // patent-encumbered (AAC) or a new vendored dependency with its own
    // licence to clear, and the standing posture on this project is to stay
    // well clear of that line. Raw 48 kHz mono at 16 bits is 96 kB/s, which is
    // nothing on a LAN — the case this feature is actually for — and the
    // browser needs no decoder at all, only Web Audio.
    svr.Get("/api/audio", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authorised(req)) {
            deny(res, "sign in first");
            return;
        }
        // Claim a slot BEFORE committing to the stream; each listener parks an
        // HTTP worker thread for as long as it listens.
        const int before = listeners_.fetch_add(1, std::memory_order_acq_rel);
        if (before >= static_cast<int>(WebServer::kMaxAudioListeners)) {
            listeners_.fetch_sub(1, std::memory_order_acq_rel);
            deny(res, "too many listeners", 503);
            return;
        }

        // Start at LIVE, not at the oldest buffered sample: a listener joining
        // a radio wants what is being received now, not two seconds of history
        // it then stays behind by for ever.
        auto cursor = std::make_shared<std::uint64_t>(audio_.written());

        res.set_chunked_content_provider(
            "audio/L16;rate=48000;channels=1",
            [this, cursor](std::size_t /*offset*/, httplib::DataSink& sink) {
                if (!running_.load(std::memory_order_acquire)) {
                    return false;  // server stopping: end the stream cleanly
                }
                // 100 ms per read. Small enough to keep latency low, large
                // enough that the per-chunk overhead is irrelevant.
                constexpr std::size_t kChunkSamples = 4800;
                float samples[kChunkSamples];
                std::uint64_t dropped = 0;
                const std::size_t got =
                    audio_.read(*cursor, samples, kChunkSamples, dropped);
                if (got == 0) {
                    // Nothing new yet. Sleeping here is correct rather than
                    // lazy: this is the listener's OWN worker thread, and
                    // spinning would burn a core per listener to no purpose.
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    return true;
                }
                std::vector<std::uint8_t> pcm(2 * got);
                floatToPcm16le(samples, got, pcm.data());
                return sink.write(reinterpret_cast<const char*>(pcm.data()), pcm.size());
            },
            [this](bool) { listeners_.fetch_sub(1, std::memory_order_acq_rel); });
    });

    svr.Post("/api/control", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authorised(req)) {
            deny(res, "sign in first");
            return;
        }
        // Same CSRF reasoning as /api/login, and it matters more here: this is
        // the endpoint that moves the radio.
        if (req.get_header_value("Content-Type").rfind("application/json", 0) != 0) {
            deny(res, "expected a JSON body", 415);
            return;
        }
        ControlRequest cr;
        std::string error;
        if (!parseControlRequest(req.body, cr, error)) {
            deny(res, error, 400);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            while (pending_.size() >= WebServer::kMaxQueuedControls) {
                pending_.pop_front();
            }
            pending_.push_back(cr);
        }
        // 202, not 200: this has been accepted, not carried out. The radio is
        // moved by the application on its next frame, and the browser sees the
        // result through /api/status like every other observer — which is also
        // what keeps the page honest if a clamp changes the value it asked for.
        res.status = 202;
        nlohmann::json j;
        j["accepted"] = true;
        res.set_content(j.dump(), "application/json");
    });

    svr.Get("/api/spectrum", [this](const httplib::Request& req, httplib::Response& res) {
        if (!authorised(req)) {
            deny(res, "sign in first");
            return;
        }
        SpectrumSnapshot snap;
        if (req.has_param("since")) {
            try {
                snap.seq = std::stoull(req.get_param_value("since"));
            } catch (const std::exception&) {
                snap.seq = 0;  // an unparseable cursor means "send me anything"
            }
        }
        const bool fresh = spectrum_ ? spectrum_(snap) : false;
        nlohmann::json j;
        j["seq"] = snap.seq;
        j["fresh"] = fresh;
        j["dbMin"] = kSpectrumDbMin;
        j["dbMax"] = kSpectrumDbMax;
        if (fresh) {
            j["centerHz"] = snap.centerHz;
            j["spanHz"] = snap.spanHz;
            const std::vector<std::uint8_t> q = quantiseSpectrum(snap.dbBins);
            j["bins"] = base64Encode(q);
        }
        res.set_content(j.dump(), "application/json");
    });
}

bool WebServer::Impl::start(const WebServerConfig& cfg, std::string& error) {
    stop();
    error.clear();

    const BindDecision d = evaluateBind(cfg);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg_ = cfg;
        decision_ = d;
        boundPort_ = -1;
        hasRecord_ = false;
        record_ = PasswordRecord{};
        if (!cfg.passwordRecord.empty()) {
            std::string parseError;
            hasRecord_ = PasswordRecord::parse(cfg.passwordRecord, record_, parseError);
        }
    }
    if (!d.allowed()) {
        error = d.reason;
        return false;
    }

    // Sessions do not survive a restart: the binding, and possibly the
    // password, may have just changed, and anyone holding a token should have
    // to prove themselves against the new configuration.
    sessions_.revokeAll();
    throttle_.clear();
    // Nothing buffered from a previous run: its samples are stale by however
    // long the server was down, and a cursor from then must not find them
    // readable.
    audio_.reset();

    svr_ = std::make_unique<httplib::Server>();
    svr_->set_payload_max_length(kMaxPayloadBytes);
    // Without these a single stalled peer holds a worker thread indefinitely.
    svr_->set_read_timeout(10, 0);
    svr_->set_write_timeout(10, 0);
    installRoutes(*svr_);

    const int port = svr_->bind_to_port(d.effectiveAddress.c_str(), cfg.port) ? cfg.port : -1;
    if (port < 0) {
        error = "could not bind " + d.effectiveAddress + ":" + std::to_string(cfg.port) +
                " — another program is probably already using that port";
        svr_.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        boundPort_ = port;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { svr_->listen_after_bind(); });
    svr_->wait_until_ready();
    return true;
}

void WebServer::Impl::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        // Not running. A half-constructed server from a failed start still
        // needs releasing, which the reset below does.
        svr_.reset();
        return;
    }
    if (svr_) {
        svr_->stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    svr_.reset();
    sessions_.revokeAll();
    std::lock_guard<std::mutex> lock(mutex_);
    boundPort_ = -1;
}

// ---------------------------------------------------------------------------
// WebServer — thin forwarding shell, so httplib stays in this file alone
// ---------------------------------------------------------------------------

WebServer::WebServer() : impl_(std::make_unique<Impl>()) {}
WebServer::~WebServer() = default;

bool WebServer::setStatusProvider(StatusProvider fn) {
    return impl_->setStatusProvider(std::move(fn));
}
bool WebServer::setSpectrumProvider(SpectrumProvider fn) {
    return impl_->setSpectrumProvider(std::move(fn));
}
bool WebServer::setClock(Clock fn) { return impl_->setClock(std::move(fn)); }
bool WebServer::start(const WebServerConfig& cfg, std::string& error) {
    return impl_->start(cfg, error);
}
void WebServer::stop() { impl_->stop(); }
bool WebServer::running() const { return impl_->running(); }
int WebServer::boundPort() const { return impl_->boundPort(); }
BindDecision WebServer::decision() const { return impl_->decision(); }
std::size_t WebServer::sessionCount() const { return impl_->sessionCount(); }
void WebServer::revokeAllSessions() { impl_->revokeAllSessions(); }
std::vector<ControlRequest> WebServer::takePendingControls() {
    return impl_->takePendingControls();
}
void WebServer::pushAudio(const float* samples, std::size_t n) {
    impl_->pushAudio(samples, n);
}
std::size_t WebServer::audioListeners() const { return impl_->audioListeners(); }

}  // namespace cascade::net
