// Implementation of the web server. See net/web_server.hpp for the design, and
// net/web_policy.hpp for the bind rules this file obeys rather than restates.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <set>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <httplib.h>

#include "radar/radar_assets.hpp"

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

// EVERY dump() IN THIS FILE PASSES error_handler_t::replace, and that is a
// correctness requirement rather than a nicety.
//
// nlohmann's default behaviour is to THROW on a string that is not valid
// UTF-8. Much of what this server serialises is text produced by decoder
// plugins from radio noise — POCSAG and RTTY turn whatever is on the air into
// characters, and the Inmarsat-C decoder is published knowing its constants
// are unverified, so producing rubbish is its expected behaviour. A single
// byte above 127 in one decoded line would therefore throw inside the status
// handler and take the ENTIRE browser interface down — not that one line, the
// whole readout — until the offending line aged out of the log. Third-party
// plugin output must never be able to do that, so invalid sequences become
// U+FFFD and the response still goes out.

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

<section id="login" class="hidden">
  <div class="loginbox">
    <h1>FoxSDR</h1>
    <h2>Sign in</h2>
    <form id="loginForm">
      <label>User <input id="user" autocomplete="username" required></label>
      <label>Password <input id="pass" type="password" autocomplete="current-password" required></label>
      <button type="submit" class="primary">Sign in</button>
    </form>
    <p id="loginError" class="error"></p>
  </div>
</section>

<div id="app" class="hidden">

  <header id="toolbar">
    <span class="brand">FoxSDR</span>
    <button id="playstop" class="primary">Start</button>
    <button id="listen">Listen</button>
    <div id="freqDigits" title="scroll a digit to tune, like the desktop readout"></div>
    <span class="unit">Hz</span>
    <label class="inline">Vol <input id="vol" type="range" min="0" max="1" step="0.01"><span id="volVal" class="val"></span></label>
    <span class="spacer"></span>
    <span id="mutedBy" class="badge warn hidden"></span>
    <span id="remote" class="badge hidden">remote access</span>
    <span id="link" class="badge warn hidden">disconnected</span>
    <button id="logout" class="hidden">Sign out</button>
  </header>

  <aside id="side">
    <div id="sideGrip" title="drag to resize the control column"></div>

    <details open><summary>Source</summary><div class="sec">
      <label>Device<select id="srcSel"></select></label>
      <div class="row">
        <button id="scan">Scan for devices</button>
        <span id="srcBusy" class="dim"></span>
      </div>
      <div id="devRow" class="hidden colgap">
        <label>Antenna<select id="antenna"></select></label>
        <label>Sample rate<select id="srate">
          <option value="1000000">1 MS/s</option><option value="2000000">2 MS/s</option>
          <option value="4000000">4 MS/s</option><option value="8000000">8 MS/s</option>
        </select></label>
        <label class="check"><input id="agc" type="checkbox"> Hardware AGC</label>
      </div>
      <div id="gainRow" class="hidden colgap"></div>
      <p id="srcError" class="error"></p>
    </div></details>

    <details open><summary>Radio</summary><div class="sec">
      <div id="modes" class="modes"></div>
      <div class="row tight">
        <label class="grow">Centre (MHz)<input id="centre" type="number" step="0.000001"></label>
        <button id="tune">Tune</button>
      </div>
      <label>VFO offset <span id="vfoVal" class="val"></span><input id="vfo" type="range" min="-500" max="500" step="1"></label>
      <label>Bandwidth <span id="bwVal" class="val"></span><input id="bw" type="range" min="0" max="5" step="1"></label>
      <label>Squelch <span id="sqVal" class="val"></span><input id="sq" type="range" min="-120" max="0" step="1"></label>
      <label>De-emphasis<select id="deemph">
        <option value="0">50 us</option><option value="1">75 us</option><option value="2">off</option>
      </select></label>
      <label class="check"><input id="stereo" type="checkbox"> Stereo</label>
      <p id="ctlError" class="error"></p>
    </div></details>

    <details open><summary>Display</summary><div class="sec">
      <label>Min dB <span id="dbMinVal" class="val"></span><input id="dbMin" type="range" min="-160" max="-20" step="1"></label>
      <label>Max dB <span id="dbMaxVal" class="val"></span><input id="dbMax" type="range" min="-100" max="20" step="1"></label>
    </div></details>

    <details><summary>Audio filters</summary><div class="sec">
      <label class="check"><input id="nr" type="checkbox"> Noise reduction</label>
      <label>Strength <span id="nrVal" class="val"></span><input id="nrStrength" type="range" min="0" max="1" step="0.01"></label>
      <label class="check"><input id="notch" type="checkbox"> Notch</label>
      <label>Notch <span id="notchVal" class="val"></span><input id="notchFreq" type="range" min="10" max="8000" step="10"></label>
      <label class="check"><input id="autoNotch" type="checkbox"> Auto-notch <span id="anState" class="dim"></span></label>
    </div></details>

    <details><summary>Recorder</summary><div class="sec">
      <div class="row"><button id="recIq">Record IQ</button><button id="recAudio">Record audio</button></div>
      <div id="recInfo" class="dim"></div>
      <p id="recError" class="error"></p>
    </div></details>

    <details><summary>Bookmarks</summary><div class="sec">
      <div class="row tight">
        <label class="grow">Name<input id="bmName" placeholder="name this frequency"></label>
        <button id="bmAdd">Add</button>
      </div>
      <div id="bookmarks"></div>
    </div></details>

    <details><summary>Scanner</summary><div class="sec">
      <div class="row tight">
        <label>From (MHz)<input id="scanFrom" type="number" step="0.001"></label>
        <label>To (MHz)<input id="scanTo" type="number" step="0.001"></label>
      </div>
      <div class="row tight">
        <label class="grow">Step (kHz)<input id="scanStep" type="number" step="1"></label>
        <button id="scanToggle">Start scan</button>
      </div>
      <span id="scanState" class="dim"></span>
    </div></details>

    <details><summary>Plugins</summary><div class="sec">
      <div id="plugins"></div>
      <div class="row"><button id="pluginFetch">Get plugins</button><span id="catStatus" class="dim"></span></div>
      <p id="catError" class="error"></p>
      <p id="installReport" class="ok"></p>
      <div id="catalogue"></div>
    </div></details>

  </aside>

  <main id="main">
    <div id="scope">
      <button class="pop" id="popScope" data-view="scope"
              title="open the spectrum in its own window">&#x29c9;</button>
      <canvas id="spectrum" title="click to tune the VFO here"></canvas>
      <div id="splitScope" class="hsplit" title="drag to resize"></div>
      <canvas id="waterfall"></canvas>
    </div>
    <div id="rds" class="hidden"></div>
    <div id="splitPanels" class="hsplit hidden" title="drag to resize"></div>
    <div id="panels">
      <div id="mapWrap" class="panel hidden">
        <h3>Map <button class="pop" data-view="map"
                        title="open the map in its own window">&#x29c9;</button></h3>
        <div id="mapRow">
          <canvas id="map"></canvas>
          <div id="trackList" title="drag the left edge to resize"></div>
        </div>
      </div>
      <div id="imagesWrap" class="panel hidden">
        <h3>Pictures <button class="pop" data-view="images"
                             title="open the pictures in their own window">&#x29c9;</button></h3>
        <div id="images"></div>
      </div>
      <div id="decodedWrap" class="panel hidden">
        <h3 id="decodedHead">Decoded <button class="pop" data-view="decoded"
                title="open the decoder output in its own window">&#x29c9;</button></h3>
        <pre id="decoded"></pre>
      </div>
    </div>
  </main>

  <footer id="status"></footer>
</div>

<script src="/app.js"></script>
</body>
</html>
)HTML";

// THE STYLE SHEET IS SPLIT ACROSS TWO LITERALS, for the same reason the client
// script below is split across eight: MSVC caps a SINGLE string literal at
// 16380 bytes, and this sheet reached 16073 of them when the whole page was
// brought onto the bench palette. Three hundred bytes of headroom is not
// headroom, it is a fuse - the next rule anybody adds here would have failed
// the build with C2026, an error that names the limit and not the file.
//
// The split point is arbitrary and chosen for readability: layout above,
// controls and everything after below. Nothing may rely on either piece being
// independently valid CSS, and they are joined once, at first request, by
// appCss().
constexpr char kAppCss1[] = R"CSS(
/* THE 1960s BENCH, from src/gui/theme.hpp - the palette the desktop window
   draws from, so this is a third view of one instrument and not a second
   product. These tokens GOVERN: a colour below that is not a var() is drift.
   Fourteen of the fifteen are theme.hpp values exactly. The fifteenth is
   --glass, which is spectrum_view.cpp's kBackground: the cool near-black green
   the three display tubes share, and the one colour on the bench with no name
   in theme.hpp (the palette's own darkest ground, kVoid, is a WARM near-black
   for brass and would put the wrong cast behind a trace). It is a literal
   there and a literal here, and both say so. */
:root { color-scheme: dark;
  /* Brass is panel, key and rail; enamel and glass are anything recessed. */
  --bg:#1f1b14; --panel:#4a4234; --edge:#6e6552; --key:#6e6552;
  --accent:#8b8069; --well:#2a251c; --glass:#050a06;
  /* Ivory letters a CONTROL; engraved ink is a caption. */
  --fg:#efe7d2; --dim:#9c9078; --faint:#7d7360;
  /* Amber is a READING, phosphor is WHAT THE RADIO HEARD, rust is TROUBLE. */
  --read:#f0a840; --read-dim:#8a5a2a; --trace:#8fd9a0;
  --fault:#b8552f; --fault-ink:#e07a4e;
  --side:310px; --bar:52px; }
* { box-sizing: border-box; }
html, body { height:100%; }
body { margin:0; background:var(--bg); color:var(--fg);
       font:13px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; overflow:hidden; }
.hidden { display:none !important; }

/* The desktop's shape: a toolbar across the top, a fixed control column down
   the left, the spectrum filling everything else, a status strip beneath. */
#app { display:grid; height:100vh;
       grid-template-columns: var(--side) 1fr;
       grid-template-rows: var(--bar) 1fr auto;
       grid-template-areas: "bar bar" "side main" "foot foot"; }

#toolbar { grid-area:bar; display:flex; align-items:center; gap:.6rem;
           padding:0 .75rem; background:var(--panel);
           border-bottom:1px solid var(--edge); }
.brand { font-weight:700; letter-spacing:.02em; margin-right:.25rem; }
.spacer { flex:1 1 auto; }
.unit { color:var(--dim); font-size:.75rem; margin-left:-.3rem; }
label.inline { flex-direction:row; align-items:center; gap:.4rem; color:var(--dim); }
label.inline input[type=range] { width:8rem; }

#side { grid-area:side; background:var(--panel); border-right:1px solid var(--edge);
        overflow-y:auto; overflow-x:hidden; position:relative; }
/* The control column's own width handle, on its right edge. */
#sideGrip { position:sticky; top:0; float:right; width:7px; height:100vh;
            margin-bottom:-100vh; cursor:ew-resize; z-index:5; }
#sideGrip:hover, #sideGrip.active { background:var(--accent); }

/* Open-in-its-own-window buttons. Small and quiet: they decorate headers, and
   the header is not about them. */
button.pop { background:transparent; border:0; color:var(--dim); font-size:.85rem;
             padding:0 .25rem; cursor:pointer; line-height:1; }
button.pop:hover { color:var(--fg); background:transparent; }
#scope { position:relative; }
#popScope { position:absolute; top:.3rem; right:.35rem; z-index:5; }

/* A pop-out window shows ONE region full-height and nothing else. The page is
   the same page - same script, same polling, same session - just dressed down
   to a single panel, which is what makes "open the map on the other monitor"
   free instead of a second application. */
body[data-view] #toolbar, body[data-view] #side, body[data-view] #status,
body[data-view] #rds, body[data-view] .hsplit, body[data-view] button.pop
  { display:none !important; }
body[data-view] #app { grid-template-columns:1fr; grid-template-rows:1fr;
                       grid-template-areas:"main"; }
body[data-view="scope"] #panels { display:none !important; }
body[data-view="map"] #scope, body[data-view="map"] #imagesWrap,
body[data-view="map"] #decodedWrap { display:none !important; }
body[data-view="images"] #scope, body[data-view="images"] #mapWrap,
body[data-view="images"] #decodedWrap { display:none !important; }
body[data-view="decoded"] #scope, body[data-view="decoded"] #mapWrap,
body[data-view="decoded"] #imagesWrap { display:none !important; }
body[data-view="map"] #panels, body[data-view="images"] #panels,
body[data-view="decoded"] #panels
  { flex:1 1 auto; max-height:none; height:auto; }
body[data-view="map"] #map { flex:1 1 auto; }
body[data-view="decoded"] #decodedWrap { display:flex; flex-direction:column;
                                         flex:1 1 auto; min-height:0; }
body[data-view="decoded"] #decoded { flex:1 1 auto; max-height:none; }
#side details { border-bottom:1px solid var(--edge); }
#side summary { cursor:pointer; padding:.5rem .75rem; font-weight:600; font-size:.85rem;
                letter-spacing:.02em; user-select:none; }
#side summary:hover { background:var(--key); }
.sec { padding:.15rem .75rem .7rem; display:flex; flex-direction:column; gap:.5rem; }
.colgap { display:flex; flex-direction:column; gap:.5rem; }

#main { grid-area:main; display:flex; flex-direction:column; min-width:0;
        min-height:0; overflow:hidden; padding:.5rem; gap:.5rem; }
/* Spectrum above waterfall, sharing the frequency axis - the pair fills
   whatever height is left, exactly as the desktop's centre panel does. */
/* min-height:0 on every one of these is load-bearing. A canvas's INTRINSIC
   size comes from its pixel buffer, and fitCanvas() sets that buffer from the
   rendered box - so with flex's default min-height:auto the two feed each
   other and the canvases grow without bound (measured: 972 px and 1360 px
   tall inside an 818 px panel). min-height:0 lets flex shrink them below
   their content size, which breaks the loop. */
#scope { flex:1 1 auto; display:flex; flex-direction:column; min-height:0; }
#spectrum { flex:0 0 42%; width:100%; min-height:0; display:block; background:var(--glass);
            border:1px solid var(--edge); border-bottom:0;
            border-radius:3px 3px 0 0; cursor:crosshair; }
#waterfall { flex:1 1 auto; width:100%; min-height:0; display:block; background:var(--glass);
             border:1px solid var(--edge); border-radius:0 0 3px 3px; }
#panels { flex:0 0 auto; max-height:42%; overflow-y:auto; display:flex;
          flex-direction:column; gap:.5rem; }
/* Drag handles between the stacked regions, the desktop's splitters. The
   handle is 7px of grab area with a short visible pill in the middle. */
.hsplit { flex:0 0 7px; cursor:ns-resize; position:relative; user-select:none; }
.hsplit::after { content:''; position:absolute; left:44%; right:44%; top:2px;
                 bottom:2px; border-radius:2px; background:var(--edge); }
.hsplit:hover::after, .hsplit.active::after { background:var(--accent); }
/* Panels must not SHRINK inside their scroll area - when the area is smaller
   than the content, the right answer is the scrollbar, not squashed panels.
   The map panel alone also GROWS, so dragging the splitter up gives the extra
   height to the map rather than to a gap. */
.panel { background:var(--panel); border:1px solid var(--edge); border-radius:3px;
         padding:.5rem .6rem; flex:0 0 auto; }
#mapWrap { display:flex; flex-direction:column; flex:1 0 auto; min-height:0; }
.panel h3 { margin:0 0 .4rem; font-size:.8rem; color:var(--dim);
            text-transform:uppercase; letter-spacing:.06em; }

#status { grid-area:foot; display:flex; flex-wrap:wrap; gap:.9rem;
          padding:.35rem .75rem; background:var(--panel);
          border-top:1px solid var(--edge); font-size:.78rem; }
#status .cell { display:flex; gap:.35rem; align-items:baseline; }
#status .k { color:var(--dim); text-transform:uppercase; letter-spacing:.05em;
             font-size:.68rem; }
#status .v { color:var(--read); font-variant-numeric:tabular-nums; }
)CSS";

// The second half: type, form controls, panels and the responsive rules.
constexpr char kAppCss2[] = R"CSS(
h1 { font-size:1.1rem; margin:0 0 .5rem; }
h2 { font-size:.95rem; margin:0 0 .6rem; }
label { display:flex; flex-direction:column; gap:.15rem; color:var(--dim);
        font-size:.75rem; }
label .val { color:var(--read); font-variant-numeric:tabular-nums; }
input, select { background:var(--well); border:1px solid var(--edge); color:var(--fg);
                padding:.3rem; border-radius:3px; font:inherit; width:100%;
                accent-color:var(--read); }
input[type=range] { padding:0; }
input[type=checkbox] { width:auto; }
button { background:var(--key); color:var(--fg); border:1px solid var(--edge);
         padding:.32rem .7rem; border-radius:3px; font:inherit; cursor:pointer; }
button:hover:not(:disabled) { background:var(--accent); }
button:disabled { opacity:.5; cursor:default; }
button.primary { background:var(--accent); color:var(--bg); border-color:var(--accent);
                 font-weight:600; }
button.on { background:var(--fault); border-color:var(--fault); color:var(--fg); }
.row { display:flex; gap:.5rem; align-items:flex-end; flex-wrap:wrap; }
.row.tight { flex-wrap:nowrap; }
.grow { flex:1 1 auto; }
label.check { flex-direction:row; align-items:center; gap:.4rem; color:var(--fg);
              font-size:.8rem; }
.dim { color:var(--dim); font-size:.75rem; }
.error { color:var(--fault-ink); margin:0; font-size:.78rem; }
.ok { color:var(--trace); margin:0; font-size:.78rem; }
.badge { background:var(--fault); color:var(--fg); padding:.15rem .5rem; border-radius:3px;
         font-size:.7rem; text-transform:uppercase; letter-spacing:.06em; }
.badge.warn { background:var(--read); color:var(--bg); }

/* Frequency readout: big, tabular, one element per digit so each can be
   scrolled independently. */
#freqDigits { display:inline-flex; gap:1px; font-variant-numeric:tabular-nums;
              font-size:1.5rem; font-weight:600; letter-spacing:.01em;
              cursor:ns-resize; user-select:none; color:var(--read); }
#freqDigits span { padding:0 .04em; border-radius:2px; }
#freqDigits span.d:hover { background:var(--accent); }
#freqDigits span.lead { color:var(--read-dim); }

.modes { display:grid; grid-template-columns:repeat(4,1fr); gap:.25rem; }
.modes button { padding:.28rem 0; font-size:.78rem; }
.modes button.on { background:var(--accent); border-color:var(--accent);
                   color:var(--bg); font-weight:600; }

#rds { flex:0 0 auto; background:var(--panel); border:1px solid var(--edge);
       border-radius:3px; padding:.4rem .6rem; display:grid; gap:.2rem;
       grid-template-columns:repeat(auto-fit,minmax(10rem,1fr)); font-size:.78rem;
       color:var(--trace); }
#rds .rt { grid-column:1/-1; font-variant-numeric:tabular-nums; }

#bookmarks { display:flex; flex-direction:column; gap:.2rem; }
.bm { display:flex; align-items:center; gap:.4rem; background:var(--well);
      border:1px solid var(--edge); border-radius:3px; padding:.25rem .4rem;
      font-size:.75rem; }
.bm .f { font-variant-numeric:tabular-nums; color:var(--dim); }
.bm .n { flex:1 1 auto; color:var(--fg); overflow:hidden; text-overflow:ellipsis;
         white-space:nowrap; }
.bm button { padding:.15rem .4rem; font-size:.72rem; }
.bm button.del, button.del { background:var(--fault); border-color:var(--fault); color:var(--fg); }

/* THE TRACK LIST SITS BESIDE THE MAP, not under it — the desktop's layout,
   and the one that makes a wide window useful instead of leaving the list
   pushed off the bottom of a short panel. */
#mapRow { display:flex; gap:.5rem; flex:1 1 auto; min-height:0; min-width:0; }

/* A FIXED flex-basis, never content-derived: the canvas's intrinsic size is
   its pixel buffer, which fitCanvas() sets from the rendered box, and letting
   the two feed each other is the runaway documented at #scope. min-width:0 is
   the same guard on the OTHER axis, and it is load-bearing now the canvas
   flexes horizontally: without it the canvas's own buffer width becomes its
   min-content width, so it can grow and never shrink. Grow soaks up whatever
   the splitter grants the panel area. */
/* touch-action:none because the pan is OURS. Without it a finger dragged
   across the map is claimed by the browser as a page scroll before a single
   pointermove reaches the page, so on a phone - which is what this UI is for -
   the map cannot be panned at all. */
#map { flex:1 1 auto; min-width:0; min-height:180px; background:var(--glass);
       border:1px solid var(--edge); border-radius:3px; touch-action:none; }
/* Beside the map, scrolling on its own, and resizable by its edge —
   resize:horizontal puts the browser's own grab handle on it, so the split
   between map and list needs no splitter of its own. width (not flex-basis)
   because that is what the resize handle writes. */
#trackList { display:flex; flex-direction:column; gap:.15rem;
             flex:0 0 auto; width:23rem; min-width:12rem; max-width:60%;
             overflow:auto; resize:horizontal; }
/* Narrow windows (a phone held upright) have no room for two columns, so the
   list goes back under the map and the map keeps a usable height. */
@media (max-width: 900px) {
  #mapRow { flex-direction:column; }
  #trackList { width:auto; max-width:none; height:11rem; resize:vertical; }
}
.tr { display:flex; flex-wrap:wrap; gap:.15rem .5rem; background:var(--well);
      border:1px solid var(--edge); border-radius:3px; padding:.2rem .45rem;
      font-size:.75rem; cursor:pointer; }
.tr.sel { border-color:var(--accent); background:var(--panel); }
/* Followed: the map re-centres on this one every poll, so it gets a stronger
   mark than a selection — a persistent state deserves to look persistent. */
.tr.fol { border-color:var(--trace); }
.tr.fol .id::before { content:'\25c9\00a0'; color:var(--trace); }
.tr .ac { flex:1 1 100%; color:var(--dim); font-size:.72rem; }
.tr .id { font-variant-numeric:tabular-nums; color:var(--dim); min-width:5.5rem; }
.tr .pos { margin-left:auto; color:var(--dim); font-variant-numeric:tabular-nums; }

#images { display:flex; flex-wrap:wrap; gap:.6rem; }
.img { background:var(--well); border:1px solid var(--edge); border-radius:3px; padding:.35rem; }
.img img { display:block; max-width:100%; image-rendering:pixelated; background:var(--glass); }
.img .cap { color:var(--dim); font-size:.72rem; margin-top:.2rem; }

#decoded { background:var(--glass); color:var(--trace);
           border:1px solid var(--edge); border-radius:3px;
           padding:.45rem; height:14rem; max-height:none; overflow:auto;
           font-size:.76rem; white-space:pre-wrap; word-break:break-word;
           margin:0; resize:vertical; }

#plugins { display:flex; flex-direction:column; gap:.25rem; }
.pl { background:var(--well); border:1px solid var(--edge); border-radius:3px;
      padding:.3rem .45rem; font-size:.78rem; }
.pl.bad { border-color:var(--fault); }
.pl .meta, .cat .meta { color:var(--faint); font-size:.72rem; }
.pl button { margin-top:.25rem; padding:.15rem .45rem; font-size:.72rem; }
.pl button.preset { display:block; width:100%; text-align:left;
                    background:var(--well); border-color:var(--edge); color:var(--fg); }
.pl button.preset:hover { background:var(--key); }
#catalogue { display:flex; flex-direction:column; gap:.3rem; }
.cat { background:var(--well); border:1px solid var(--edge); border-radius:3px; padding:.4rem .5rem; }
.cat .hd { display:flex; gap:.4rem; align-items:baseline; flex-wrap:wrap; }
.cat .hd .nm { font-weight:600; font-size:.8rem; }
.cat .notice { background:var(--well); border:1px solid var(--read-dim); color:var(--read);
               border-radius:3px; padding:.3rem .45rem; margin:.3rem 0; font-size:.75rem; }
.cat button { margin-top:.3rem; padding:.18rem .5rem; font-size:.75rem; }

#login { display:grid; place-items:center; height:100vh; }
.loginbox { background:var(--panel); border:1px solid var(--edge); border-radius:4px;
            padding:1.5rem; width:min(22rem,90vw); }
.loginbox form { display:flex; flex-direction:column; gap:.6rem; }

/* Narrow screens - a phone gets the spectrum first and the controls below it,
   because on a phone the picture is the point and the panel is a menu. */
@media (max-width: 820px) {
  body { overflow:auto; }
  #app { height:auto; grid-template-columns:1fr;
         grid-template-rows:auto auto auto auto;
         grid-template-areas:"bar" "main" "side" "foot"; }
  #side { border-right:0; border-top:1px solid var(--edge); overflow:visible; }
  #main { height:auto; overflow:visible; }
  #scope { min-height:300px; }
  #panels { max-height:none; overflow:visible; }
  #toolbar { flex-wrap:wrap; height:auto; padding:.4rem .75rem; }
  #freqDigits { font-size:1.2rem; }
  label.inline input[type=range] { width:6rem; }
}
)CSS";

// The joined style sheet, built once on first use - the pieces are
// compile-time constants, so there is nothing to invalidate.
const std::string& appCss() {
    static const std::string joined = std::string(kAppCss1) + kAppCss2;
    return joined;
}

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

// EVERY POLL IS TIMED OUT, and that is not belt-and-braces. fetch() has no
// timeout of its own: when the application goes away mid-request - which it
// does on every upgrade, and which the receiver being an app the user can
// close makes ordinary - the browser can leave the promise pending
// indefinitely. A poller that awaits it while holding a "one at a time" guard
// then never runs again, so the spectrum and waterfall freeze for good while
// the status poll keeps updating and the page still looks connected. Measured
// responses here are 2-5 ms, so four seconds is far beyond any real answer and
// still recovers quickly.
const POLL_TIMEOUT_MS = 4000;
async function getJson(url) {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), POLL_TIMEOUT_MS);
  try {
    const r = await fetch(url, { credentials: 'same-origin', signal: ac.signal });
    if (r.status === 401) { stopPolling(); await refreshSession(); return null; }
    setLinkUp(true);
    if (!r.ok) return null;
    return await r.json();
  } catch (e) {
    setLinkUp(false);
    return null;
  } finally {
    clearTimeout(timer);
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

// The device list, the open device's ports and gains, and whichever of them
// the receiver currently holds. Rebuilt only when the underlying list CHANGES,
// so a dropdown the user has open is not torn down under them four times a
// second.
let lastDevKey = '', lastAntKey = '', lastGainKey = '';

function reflectSource(s) {
  $('srcBusy').textContent = s.sourceBusy ? 'working...' : '';
  $('scan').disabled = s.sourceBusy;
  $('srcError').textContent = s.sourceError || '';

  const devKey = s.devices.map(d => d.args).join('|') + '#' + s.sourceKind + '#' + s.soapyArgs;
  if (devKey !== lastDevKey) {
    lastDevKey = devKey;
    const sel = $('srcSel');
    sel.innerHTML = '';
    const gen = document.createElement('option');
    gen.value = ''; gen.textContent = 'Signal generator';
    sel.appendChild(gen);
    s.devices.forEach((d) => {
      const o = document.createElement('option');
      o.value = d.args; o.textContent = d.label;
      sel.appendChild(o);
    });
    // An IQ file may be OPEN (restored from the config, or chosen in the
    // desktop window); it simply cannot be chosen from here. Showing it keeps
    // the readout honest rather than implying the generator is running.
    if (s.sourceKind === 'file') {
      const o = document.createElement('option');
      o.value = '__file__'; o.textContent = 'IQ file (choose in the app)';
      o.disabled = true;
      sel.appendChild(o);
      sel.value = '__file__';
    } else {
      sel.value = s.sourceKind === 'soapy' ? s.soapyArgs : '';
    }
  }

  const isDevice = s.sourceKind === 'soapy';
  $('devRow').classList.toggle('hidden', !isDevice);
  $('gainRow').classList.toggle('hidden', !isDevice);
  if (!isDevice) return;

  const antKey = s.antennas.join('|') + '#' + s.antenna;
  if (antKey !== lastAntKey) {
    lastAntKey = antKey;
    const a = $('antenna');
    a.innerHTML = '';
    s.antennas.forEach((n) => {
      const o = document.createElement('option');
      o.value = n; o.textContent = n;
      a.appendChild(o);
    });
    a.value = s.antenna;
  }
  syncControl($('srate'), String(Math.round(s.sampleRateHz)));
  $('agc').disabled = !s.agcSupported;
  if (document.activeElement !== $('agc')) $('agc').checked = s.agc;

  const gainKey = s.gains.map(g => g.name).join('|');
  if (gainKey !== lastGainKey) {
    lastGainKey = gainKey;
    const row = $('gainRow');
    row.innerHTML = '';
    s.gains.forEach((g) => {
      const label = document.createElement('label');
      label.innerHTML = g.name + ' <span class="gv"></span>';
      const inp = document.createElement('input');
      inp.type = 'range'; inp.min = '0'; inp.max = '76'; inp.step = '1';
      inp.dataset.gain = g.name;
      inp.addEventListener('input', () => {
        label.querySelector('.gv').textContent = inp.value + ' dB';
      });
      inp.addEventListener('change', () => {
        control({ gainName: g.name, gainDb: parseFloat(inp.value) });
      });
      label.appendChild(inp);
      row.appendChild(label);
    });
  }
  s.gains.forEach((g) => {
    const inp = document.querySelector('#gainRow input[data-gain="' + g.name + '"]');
    if (!inp) return;
    syncControl(inp, Math.round(g.db));
    const gv = inp.parentElement.querySelector('.gv');
    if (gv) gv.textContent = g.db.toFixed(0) + ' dB';
  });
}

)JS";

// The map itself. Split from kAppJs2: MSVC caps a string literal at 16380
// bytes and the tile + icon work pushed the section over it.
constexpr char kAppJs2t[] = R"JS(
// EQUIRECTANGULAR, matching the desktop's default projection. Mercator would
// misplace polar orbits, and this one inverts cheaply — which is what a
// click-to-centre would need later.
const TRACK_COLOURS = { 1: '#ff5a4a', 2: '#4fd08a', 3: '#4fb0ff', 4: '#ffd24f' };

function reflectTracks(s) {
  mapStatus = s;   // so a drag can redraw between polls
  const bm = s.basemap || { active: false };
  const has = s.tracks.length > 0;
  $('mapWrap').classList.toggle('hidden', !has);
  if (!has) { mapHits = []; mapProj = null; return; }
  // A selection whose track has gone is over, not remembered.
  if (selectedTrackId && !s.tracks.some((t) => t.id === selectedTrackId)) {
    selectedTrackId = '';
  }

  const c = $('map');
  fitCanvas(c);
  const ctx = c.getContext('2d');
  const w = c.width, h = c.height;
  ctx.clearRect(0, 0, w, h);

  // Fit the view to what is actually being heard, with a margin, so a single
  // aircraft is not a dot in the middle of an empty world map — UNLESS the
  // user has taken the view with a drag or the wheel, in which case it is
  // theirs until a double-click hands it back.
  if (!mapView.user) {
    let minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    s.tracks.forEach((t) => {
      minLat = Math.min(minLat, t.latDeg); maxLat = Math.max(maxLat, t.latDeg);
      minLon = Math.min(minLon, t.lonDeg); maxLon = Math.max(maxLon, t.lonDeg);
    });
    const padLat = Math.max(0.5, (maxLat - minLat) * 0.25);
    const padLon = Math.max(0.5, (maxLon - minLon) * 0.25);
    minLat -= padLat; maxLat += padLat; minLon -= padLon; maxLon += padLon;
    mapView.centreLat = (minLat + maxLat) / 2;
    mapView.centreLon = (minLon + maxLon) / 2;
    // The span that shows both extents, in whichever vertical scale is about
    // to draw the frame.
    const needVert = bm.active ? (mercYn(minLat) - mercYn(maxLat)) * 360 * (w / h)
                               : (maxLat - minLat) * (w / h);
    mapView.lonSpan = Math.min(360, Math.max(0.2, maxLon - minLon, needVert));
  }
  // FOLLOW, before the projection is set up so the whole frame uses the new
  // centre — a followed target that only re-centred on the next poll would
  // visibly lag its own marker. A followed track that stops being reported
  // releases the follow rather than pinning the map to a ghost.
  if (followTrackId) {
    const f = s.tracks.find((t) => t.id === followTrackId);
    if (f) {
      mapView.user = true;
      mapView.centreLat = f.latDeg;
      mapView.centreLon = f.lonDeg;
    } else {
      followTrackId = '';
    }
  }
  const centreLat = mapView.centreLat, centreLon = mapView.centreLon;
  const lonSpan = mapView.lonSpan;
  const wrapLon = (lon) => {
    let dx = lon - centreLon;
    if (dx > 180) dx -= 360;
    if (dx < -180) dx += 360;
    return dx;
  };

  // WEB MERCATOR while the application has a basemap plugin supplying tiles —
  // tiles are Mercator by construction, exactly as on the desktop map — and
  // centre-plus-span equirectangular otherwise.
  let xOf, yOf, haveTiles = false;
  if (bm.active) {
    const pixPerWorld = w * 360 / lonSpan;
    const cy = mercYn(centreLat);
    xOf = (lon) => wrapLon(lon) / lonSpan * w + w / 2;
    yOf = (lat) => (mercYn(lat) - cy) * pixPerWorld + h / 2;
    mapProj = { merc: true, w: w, h: h, lonSpan: lonSpan, pixPerWorld: pixPerWorld };
    haveTiles = drawMapTiles(ctx, w, h, { lonSpan, centreLon, cy, xOf }, bm);
    pruneTiles();
  } else {
    const latSpan = lonSpan * h / w;
    xOf = (lon) => wrapLon(lon) / lonSpan * w + w / 2;
    yOf = (lat) => (centreLat - lat) / latSpan * h + h / 2;
    mapProj = { merc: false, w: w, h: h, lonSpan: lonSpan, latSpan: latSpan };
  }

  ctx.font = '11px system-ui, sans-serif';
  if (!bm.active) {
    // A graticule, so the picture has a scale rather than being floating
    // dots. Not drawn over tiles: the imagery is its own reference.
    const latSpan = lonSpan * h / w;
    ctx.strokeStyle = '#3b3529'; ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = (i / 4) * h, x = (i / 4) * w;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    }
    ctx.fillStyle = '#7d7360';
    ctx.fillText((centreLat + latSpan / 2).toFixed(2) + 'N', 4, 12);
    ctx.fillText((centreLat - latSpan / 2).toFixed(2) + 'N', 4, h - 4);
    ctx.fillText((centreLon - lonSpan / 2).toFixed(2) + 'E', 4, h - 18);
    ctx.fillText((centreLon + lonSpan / 2).toFixed(2) + 'E', w - 60, h - 18);
  }

  const hits = [];
  s.tracks.forEach((t) => {
    // Faded targets still draw and are still clickable; a fully dropped one is
    // skipped ENTIRELY - no marker, no hit, no row - so what can be clicked is
    // exactly what can be seen.
    const alpha = trackAlpha(t.ageMs, t.kind);
    if (alpha <= 0) return;
    const x = xOf(t.lonDeg), y = yOf(t.latDeg);
    hits.push({ id: t.id, x: x, y: y });
    ctx.globalAlpha = alpha;
    const col = (t.flags & 1) ? '#ff2020' : (TRACK_COLOURS[t.kind] || '#c8d0dc');
    const sel = t.id === selectedTrackId;
    if (t.kind === 1) {
      // Aircraft are a plane silhouette rotated to the course; the selected
      // one is the silhouette knocked out of a filled disc.
      if (sel) {
        ctx.fillStyle = col;
        ctx.beginPath(); ctx.arc(x, y, 13, 0, Math.PI * 2); ctx.fill();
        tracePlane(ctx, x, y, t.courseDeg, 8);
        ctx.fillStyle = '#0d0b07'; ctx.fill();
      } else {
        tracePlane(ctx, x, y, t.courseDeg, 9);
        ctx.fillStyle = col; ctx.fill();
        // A dark edge so the pale icon stays an icon over pale street tiles.
        ctx.strokeStyle = 'rgba(0,0,0,0.55)'; ctx.lineWidth = 1; ctx.stroke();
      }
    } else {
      ctx.fillStyle = col;
      ctx.beginPath(); ctx.arc(x, y, 4, 0, Math.PI * 2); ctx.fill();
      // Course, when the source reported one — null means not reported, and a
      // heading line drawn for an unknown course would be an invention.
      if (t.courseDeg !== null && isFinite(t.courseDeg)) {
        const a = (t.courseDeg - 90) * Math.PI / 180;
        ctx.strokeStyle = col;
        ctx.beginPath(); ctx.moveTo(x, y);
        ctx.lineTo(x + Math.cos(a) * 14, y + Math.sin(a) * 14); ctx.stroke();
      }
      if (sel) {
        ctx.strokeStyle = col; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.arc(x, y, 9, 0, Math.PI * 2); ctx.stroke();
      }
    }
    // Labels are outlined so they read over pale tiles as well as dark ocean.
    const lbl = t.label || t.id, lx = x + (sel ? 16 : 8);
    ctx.lineWidth = 3; ctx.strokeStyle = 'rgba(0,0,0,0.7)';
    ctx.strokeText(lbl, lx, y - 6);
    ctx.fillStyle = '#efe7d2';
    ctx.fillText(lbl, lx, y - 6);
    // Restored per target: the attribution box and the next frame's tiles draw
    // through this same context and must not inherit one aircraft's staleness.
    ctx.globalAlpha = 1;
  });
  mapHits = hits;

  // The attribution travels with the tiles or the tiles do not travel: shown
  // whenever imagery is actually on screen (ODbL requires it, and the desktop
  // refuses a basemap without one for exactly this reason).
  if (haveTiles && bm.attribution) {
    ctx.font = '10px system-ui, sans-serif';
    const tw = ctx.measureText(bm.attribution).width;
    ctx.fillStyle = 'rgba(13,11,7,0.72)';
    ctx.fillRect(w - tw - 10, h - 16, tw + 10, 16);
    ctx.fillStyle = '#9c9078';
    ctx.fillText(bm.attribution, w - tw - 5, h - 5);
  }

  const box = $('trackList');
  box.innerHTML = '';
  // Filtered BEFORE the cap, not after: sixty rows of which some are dropped
  // would hide live targets behind ones the map is no longer drawing.
  s.tracks.filter((t) => trackAlpha(t.ageMs, t.kind) > 0).slice(0, 60).forEach((t) => {
    const row = document.createElement('div');
    row.className = 'tr' + (t.id === selectedTrackId ? ' sel' : '') +
                    (t.id === followTrackId ? ' fol' : '');
    // The row fades with its marker, so the list and the map say the same
    // thing about how current each target is.
    row.style.opacity = trackAlpha(t.ageMs, t.kind).toFixed(3);
    row.title = 'click to go to it on the map, double-click to follow it';
    const alt = (t.altM === null || !isFinite(t.altM)) ? '-' : Math.round(t.altM) + ' m';
    const spd = (t.speedMps === null || !isFinite(t.speedMps)) ? '-'
                                                              : Math.round(t.speedMps) + ' m/s';
    row.innerHTML = '<span class="id">' + t.id + '</span>' +
                    '<span>' + (t.label || '') + '</span>' +
                    '<span class="pos">' + t.latDeg.toFixed(4) + ', ' +
                    t.lonDeg.toFixed(4) + '  ' + alt + '  ' + spd + '</span>';
    // Who it is, when the track-info plugin has answered. textContent, never
    // innerHTML: these strings come from a third-party registry.
    if (t.infoState === 1) {
      const ac = document.createElement('span');
      ac.className = 'ac';
      ac.textContent = [t.reg, t.acType, t.acOperator, t.acCountry]
          .filter((x) => x).join('  ·  ');
      if (ac.textContent) row.appendChild(ac);
    }
    // Click SELECTS and goes to it — deliberately not a toggle, because a
    // double-click fires two clicks first and a toggle would leave the
    // followed aircraft deselected. Clear a selection by clicking the map
    // background instead.
    row.addEventListener('click', () => {
      selectedTrackId = t.id;
      goToTrack(t);
    });
    row.addEventListener('dblclick', () => {
      followTrackId = (t.id === followTrackId) ? '' : t.id;
      redrawMap();
    });
    box.appendChild(row);
  });
}

// Decoded pictures. The <img> src carries the REVISION, so the browser's own
// cache does the work: it refetches once per change and not once per poll,
// which is what keeps a megapixel SSTV frame off the 4 Hz status path.
let lastImageKey = '';
function reflectImages(s) {
  const key = s.images.map(i => i.plugin + i.width + 'x' + i.height + '@' + i.revision).join('|');
  if (key === lastImageKey) return;
  lastImageKey = key;
  const has = s.images.length > 0;
  $('imagesWrap').classList.toggle('hidden', !has);
  if (!has) return;
  const box = $('images');
  box.innerHTML = '';
  s.images.forEach((im, i) => {
    const d = document.createElement('div');
    d.className = 'img';
    const el = document.createElement('img');
    el.src = '/api/image/' + i + '?rev=' + im.revision;
    el.alt = im.plugin + ' picture';
    el.width = Math.min(im.width || 320, 480);
    const cap = document.createElement('div');
    cap.className = 'cap';
    cap.textContent = im.plugin + '  ' + im.width + 'x' + im.height +
                      (im.complete ? '' : '  (receiving)');
    d.appendChild(el); d.appendChild(cap);
    box.appendChild(d);
  });
}

// PER-DIGIT WHEEL TUNING, the desktop readout's gesture. Each digit is its own
// element carrying its decade, so scrolling one moves the centre by exactly
// that decade — which is the only way to get from 100 MHz to 433 MHz in a
// sane number of notches.
const FREQ_DIGITS = 10;   // 9,999,999,999 Hz covers everything this receives
let freqShownHz = -1;

function renderFreqDigits(hz) {
  const box = $('freqDigits');
  const whole = Math.max(0, Math.round(hz));
  const text = String(whole).padStart(FREQ_DIGITS, '0');
  if (box.childElementCount !== FREQ_DIGITS + 3) {
    box.innerHTML = '';
    for (let i = 0; i < FREQ_DIGITS; i++) {
      const decade = FREQ_DIGITS - 1 - i;
      const d = document.createElement('span');
      d.className = 'd';
      d.dataset.decade = decade;
      d.addEventListener('wheel', (e) => {
        e.preventDefault();
        const step = Math.pow(10, parseInt(d.dataset.decade, 10));
        control({ centerHz: Math.max(0, Math.round(freqShownHz + (e.deltaY < 0 ? step : -step))) });
      }, { passive: false });
      box.appendChild(d);
      // Thousands separators, so the readout groups like the desktop's.
      if (decade === 9 || decade === 6 || decade === 3) {
        const sep = document.createElement('span');
        sep.textContent = ',';
        box.appendChild(sep);
      }
    }
  }
  freqShownHz = whole;
  let leading = true;
  let k = 0;
  Array.from(box.children).forEach((el) => {
    if (!el.classList.contains('d')) return;
    const ch = text[k++];
    if (ch !== '0') leading = false;
    el.textContent = ch;
    // Dim the leading zeros rather than hiding them: the digit still has to be
    // scrollable, which is how you tune UP into an empty decade.
    el.classList.toggle('lead', leading);
  });
}

let lastCatKey = '';
function reflectCatalogue(s) {
  $('catStatus').textContent = s.catalogueBusy ? 'working...' : (s.catalogueStatus || '');
  $('pluginFetch').disabled = s.catalogueBusy;
  $('catError').textContent = s.catalogueError || s.installError || '';
  $('installReport').textContent = s.installReport || '';

  const key = s.catalogue.map(e => e.id + e.version + e.installed + e.blockedReason).join('|');
  if (key === lastCatKey) return;
  lastCatKey = key;
  const box = $('catalogue');
  box.innerHTML = '';
  s.catalogue.forEach((e) => {
    const d = document.createElement('div');
    d.className = 'cat';
    const hd = document.createElement('div');
    hd.className = 'hd';
    hd.innerHTML = '<span class="nm">' + e.name + '</span>' +
                   '<span class="meta">' + e.version + '  ' + (e.licence || '') + '</span>';
    d.appendChild(hd);
    if (e.summary) {
      const sm = document.createElement('div');
      sm.className = 'meta';
      sm.textContent = e.summary;
      d.appendChild(sm);
    }
    let ack = null;
    if (e.legalNotice) {
      const n = document.createElement('div');
      n.className = 'notice';
      n.textContent = e.legalNotice;
      d.appendChild(n);
      const lab = document.createElement('label');
      lab.className = 'check';
      ack = document.createElement('input');
      ack.type = 'checkbox';
      lab.appendChild(ack);
      lab.appendChild(document.createTextNode(' I have read this'));
      d.appendChild(lab);
    }
    const btn = document.createElement('button');
    if (e.installed) {
      btn.textContent = 'Installed';
      btn.disabled = true;
    } else if (e.blockedReason) {
      btn.textContent = 'Unavailable';
      btn.disabled = true;
      btn.title = e.blockedReason;
    } else {
      btn.textContent = 'Install';
      btn.addEventListener('click', () => {
        lastCatKey = '';
        control({ pluginInstall: e.id, acknowledgeNotice: !ack || ack.checked });
      });
    }
    d.appendChild(btn);
    if (e.blockedReason && !e.installed) {
      const why = document.createElement('div');
      why.className = 'meta';
      why.textContent = e.blockedReason;
      d.appendChild(why);
    }
    box.appendChild(d);
  });
}

)JS";

constexpr char kAppJs2c[] = R"JS(
let lastPluginKey = '';
function reflectPlugins(s) {
  const key = s.plugins.map(p => p.name + p.version + p.loaded + p.stopped + p.error +
                                 p.tuneAllowed + (p.presets || []).length).join('|');
  if (key === lastPluginKey) return;
  lastPluginKey = key;
  const box = $('plugins');
  box.innerHTML = '';
  if (!s.plugins.length) {
    box.innerHTML = '<div class="pl"><span class="meta">no plugins installed</span></div>';
    return;
  }
  s.plugins.forEach((p) => {
    const d = document.createElement('div');
    d.className = 'pl' + (p.loaded ? '' : ' bad');
    const head = document.createElement('div');
    head.textContent = (p.name || '(unnamed)') + (p.version ? ' ' + p.version : '');
    const meta = document.createElement('div');
    meta.className = 'meta';
    meta.textContent = p.loaded ? (p.licence || 'licence not stated')
                                : ('refused: ' + (p.error || 'reason not reported'));
    d.appendChild(head);
    d.appendChild(meta);

    // STOPPED, said plainly. Without it a plugin the user stopped in the
    // window reads here as a normal running one, and the browser would be
    // claiming output that cannot arrive. There is no Stop/Start BUTTON here —
    // that stays in the window — but the preset buttons below take the same
    // path the desktop's do, and that path starts a stopped plugin before it
    // tunes. Hence the label: pressing a preset on a row that says this will
    // clear it, and the page must not have implied otherwise.
    if (p.loaded && p.stopped) {
      const st = document.createElement('div');
      st.className = 'meta';
      st.style.color = '#f0a840';
      st.textContent = 'stopped - loaded but running nothing';
      d.appendChild(st);
    }

    // RECEIVER CONTROL. Only offered for a plugin that declares it can ask;
    // the grant is DENIED by default and this is the only way to give it, so
    // without this row a satellite tracker's Doppler correction is unreachable
    // from the browser exactly as it was from the window before 0.20.0.
    if (p.canRequestTune) {
      const lab = document.createElement('label');
      lab.className = 'check';
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = p.tuneAllowed;
      cb.addEventListener('change', () => {
        lastPluginKey = '';
        // The MODULE FILE, not the display name: the grant is keyed on the
        // one thing a plugin cannot change by renaming itself.
        control({ pluginTuneName: p.fileName, pluginTuneAllowed: cb.checked });
      });
      lab.appendChild(cb);
      lab.appendChild(document.createTextNode(' may tune the receiver'));
      d.appendChild(lab);
    }

    // One button per declared preset — the browser's copy of the desktop's
    // one-click rows.
    (p.presets || []).forEach((ps, i) => {
      const b = document.createElement('button');
      b.className = 'preset';
      b.textContent = ps.label || ((ps.frequencyHz / 1e6).toFixed(4) + ' MHz');
      b.title = 'Tune to ' + (ps.frequencyHz / 1e6).toFixed(4) +
                ' MHz and set up this plugin';
      b.addEventListener('click', () => {
        control({ pluginPresetName: p.name, pluginPresetIndex: i });
      });
      d.appendChild(b);
    });

    if (p.fileName) {
      const rm = document.createElement('button');
      rm.className = 'del';
      rm.textContent = 'Remove';
      rm.addEventListener('click', () => {
        // Two clicks, like the desktop's confirm row: removing a plugin
        // deletes a file, and a single stray click should not.
        if (rm.dataset.armed === '1') {
          lastPluginKey = '';
          control({ pluginRemove: p.fileName });
        } else {
          rm.dataset.armed = '1';
          rm.textContent = 'Really remove?';
        }
      });
      d.appendChild(rm);
    }
    box.appendChild(d);
  });
}

)JS";

// The browser map's basemap tiles and aircraft icons. Its own literal: MSVC
// caps a string literal at 16380 bytes and the neighbours are close to it.
constexpr char kAppJs2d[] = R"JS(
// --- staleness --------------------------------------------------------------
// THE SAME RULE THE DESKTOP MAP APPLIES, with the same numbers - see
// core/plugin_ui.hpp for why each cadence is what it is. The host has already
// dropped anything past its kind's drop threshold before the snapshot is
// built, so what is left for the browser is the FADE half: a target the host
// still publishes but has not heard from recently reads as faint rather than
// as current. Without it every marker on this page looks equally live, which
// is the one thing a target display must never do.
//
// Pure, and no clock of its own: the answer depends only on the ageMs the
// snapshot carries, so a target heard again reports a small ageMs on the very
// next poll and is back at full strength on that frame - reappearance needs no
// "I faded this one" memory to undo.
const TRACK_FADE_MS = { 1: 30000, 2: 300000, 3: 1800000, 4: 120000 };
const TRACK_DROP_MS = { 1: 60000, 2: 600000, 3: 3600000, 4: 600000 };
const TRACK_MIN_ALPHA = 0.30;
function trackAlpha(ageMs, kind) {
  // An unrecognised kind gets the STATION rule, the most forgiving one there
  // is, for the host's reason: the page cannot know the reporting cadence of
  // something it has no name for, and dimming it on an aircraft's schedule
  // would fade a source that is behaving perfectly.
  const fade = TRACK_FADE_MS[kind] !== undefined ? TRACK_FADE_MS[kind] : TRACK_FADE_MS[3];
  const drop = TRACK_DROP_MS[kind] !== undefined ? TRACK_DROP_MS[kind] : TRACK_DROP_MS[3];
  if (ageMs >= drop) return 0;
  if (ageMs < fade) return 1;
  // Linear, because the quantity being shown is elapsed silence and a curve
  // would make equal silences look unequal.
  return 1 - (ageMs - fade) / (drop - fade) * (1 - TRACK_MIN_ALPHA);
}

// --- basemap tiles ----------------------------------------------------------
// Normalised Web Mercator: the whole world is 0..1 in both axes, so a tile
// index is the coordinate times 2^z — the same form the desktop uses.
function mercYn(lat) {
  const l = Math.max(-85.05112878, Math.min(85.05112878, lat));
  const s = Math.sin(l * Math.PI / 180);
  return 0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI);
}

// Tiles the page holds, keyed 'z/x/y'. A 202 from the server means "asked the
// radio, ask me again" — the entry is KEPT with a retry time, and the next
// draw after that time refetches. TIME-based, never draw-based: a drag
// redraws at 60 fps, and an early version that deleted the entry on 202
// refetched every pending tile every frame — hundreds of requests a second,
// until Chrome's per-origin queue filled and EVERY fetch on the page
// (spectrum and waterfall included) died with ERR_INSUFFICIENT_RESOURCES.
// The in-flight cap bounds the damage of any future mistake of that shape.
// 404 means the tile will never exist and is remembered so nobody asks again.
const tileCache = new Map();
let tileFrame = 0;
let tileInFlight = 0;
function tileFor(z, x, y) {
  const key = z + '/' + x + '/' + y;
  let e = tileCache.get(key);
  if (e) {
    e.used = tileFrame;
    if (e.state === 'ready') return e.bmp;
    if (e.state !== 'pending' || performance.now() < e.retryAt) return null;
    // Pending and due: fall through into a fresh fetch of the same entry.
  } else {
    e = { state: 'pending', bmp: null, used: tileFrame, retryAt: 0 };
    tileCache.set(key, e);
  }
  // Chrome allows SIX concurrent connections to one HTTP/1.1 origin, and the
  // audio stream holds one for as long as anyone listens. Tiles get three, so
  // status + spectrum always have a free lane - a map that starves the
  // readouts of their connections is 30 seconds of waterfall lag, measured.
  if (tileInFlight >= 3) return null;   // a later draw will get to it
  e.state = 'loading';
  tileInFlight++;
  fetch('/api/tile/' + z + '/' + x + '/' + y, { credentials: 'same-origin' })
    .then(async (r) => {
      if (r.status === 200) {
        const blob = await r.blob();
        e.bmp = await createImageBitmap(blob);
        e.state = 'ready';
      } else if (r.status === 404) {
        e.state = 'missing';
      } else {
        e.state = 'pending';
        e.retryAt = performance.now() + 500;
      }
    })
    .catch(() => {
      e.state = 'pending';
      e.retryAt = performance.now() + 2500;   // the server is away; be calm
    })
    .finally(() => { tileInFlight = Math.max(0, tileInFlight - 1); });
  return null;
}
function pruneTiles() {
  tileFrame++;
  if (tileCache.size <= 384) return;
  const dead = [];
  tileCache.forEach((e, k) => { if (e.used < tileFrame - 4) dead.push(k); });
  dead.forEach((k) => {
    const e = tileCache.get(k);
    if (e && e.bmp && e.bmp.close) e.bmp.close();
    tileCache.delete(k);
  });
}

// Draws every visible tile at the zoom whose pixels land nearest native size.
// Returns whether anything was actually drawn, because the attribution must be
// shown exactly when imagery is on screen.
function drawMapTiles(ctx, w, h, view, bm) {
  const pixPerWorld = w * 360 / view.lonSpan;
  let z = Math.round(Math.log2(pixPerWorld / (bm.tileSize || 256)));
  z = Math.max(bm.minZoom, Math.min(bm.maxZoom, z));
  const n = Math.pow(2, z);
  const y0 = view.cy - (h / 2) / pixPerWorld, y1 = view.cy + (h / 2) / pixPerWorld;
  const lonL = view.centreLon - view.lonSpan / 2;
  const lonR = view.centreLon + view.lonSpan / 2;
  let tx0 = Math.floor((lonL + 180) / 360 * n), tx1 = Math.floor((lonR + 180) / 360 * n);
  let ty0 = Math.max(0, Math.floor(y0 * n)), ty1 = Math.min(n - 1, Math.floor(y1 * n));
  if (tx1 - tx0 > 32) tx1 = tx0 + 32;   // a degenerate view must not ask for thousands
  if (ty1 - ty0 > 32) ty1 = ty0 + 32;
  let drew = false;
  for (let ty = ty0; ty <= ty1; ty++) {
    for (let tr = tx0; tr <= tx1; tr++) {
      const tx = ((tr % n) + n) % n;   // wrap x across the antimeridian; y ends at the poles
      const bmp = tileFor(z, tx, ty);
      if (!bmp) continue;
      // Placed from the tile's own edges, rounded, so neighbours share pixels
      // instead of leaving hairline gaps.
      const ax = Math.round(view.xOf(tr / n * 360 - 180));
      const bx = Math.round(view.xOf((tr + 1) / n * 360 - 180));
      const ay = Math.round((ty / n - view.cy) * pixPerWorld + h / 2);
      const by = Math.round(((ty + 1) / n - view.cy) * pixPerWorld + h / 2);
      ctx.drawImage(bmp, ax, ay, bx - ax, by - ay);
      drew = true;
    }
  }
  return drew;
}

// --- aircraft icon ----------------------------------------------------------
// Right half of a plane silhouette, nose up, unit box; the left half is the
// mirror walked backwards. Same table as the desktop's, so the two maps agree.
const PLANE_HALF = [
  [0.00, -1.00], [0.13, -0.70], [0.13, -0.26], [0.98, 0.16], [0.98, 0.38],
  [0.13, 0.20], [0.13, 0.62], [0.48, 0.88], [0.48, 1.02], [0.08, 0.94],
  [0.00, 0.96]];
function tracePlane(ctx, x, y, courseDeg, scale) {
  const a = (courseDeg === null || !isFinite(courseDeg)) ? 0 : courseDeg * Math.PI / 180;
  ctx.save();
  ctx.translate(x, y); ctx.rotate(a); ctx.scale(scale, scale);
  ctx.beginPath();
  PLANE_HALF.forEach((p, i) => { if (i) ctx.lineTo(p[0], p[1]); else ctx.moveTo(p[0], p[1]); });
  for (let i = PLANE_HALF.length - 2; i >= 1; i--) ctx.lineTo(-PLANE_HALF[i][0], PLANE_HALF[i][1]);
  ctx.closePath();
  // Restored BEFORE fill/stroke so the path is in device pixels and a stroke
  // is one pixel wide, not one times the icon scale.
  ctx.restore();
}

function mercLatN(y) {
  return Math.atan(Math.sinh(Math.PI * (1 - 2 * y))) * 180 / Math.PI;
}

)JS";

// Navigation and the splitters. Split from kAppJs2d for the same reason
// kAppJs2t was split from kAppJs2: MSVC caps a string literal at 16380 bytes,
// and the pointer-event pan pushed the previous section past it (C2026,
// "string too big, trailing characters truncated"). Keep every one of these
// sections well clear of the cap - the boundaries are arbitrary, so the right
// answer to a section that has grown is another split, never a squeeze.
constexpr char kAppJs2e[] = R"JS(
// --- view state and navigation ----------------------------------------------
// The view auto-fits to the targets until the user takes it with a drag or
// the wheel; from then on it is theirs. Double-click hands it back to the
// auto-fit. mapProj is the projection of the LAST DRAWN frame, which is what
// the handlers must convert pixels through — using anything newer would move
// the map against a picture the user is not looking at yet.
const mapView = { user: false, centreLat: 54, centreLon: -2, lonSpan: 20 };
let mapProj = null;
let mapStatus = null;
let mapRedraw = 0;
// The track the map is following, empty when none. While set, every status
// poll re-centres on it, so a target being watched stays put instead of
// flying off the edge — the desktop's double-click behaviour.
let followTrackId = '';

// GO TO ONE TARGET: centre on it, and tighten the zoom if the view is wider
// than `spanDeg`. TIGHTENED ONLY, never loosened — someone zoomed right into
// an approach path who then clicks a flight in that same area wants to go to
// it, not to be yanked back out to a county view.
function goToTrack(t, spanDeg) {
  if (!t) return;
  mapView.user = true;          // this is an explicit instruction about where
  mapView.centreLat = t.latDeg; // to look, so it overrides the auto-fit
  mapView.centreLon = t.lonDeg;
  const want = spanDeg || 2;
  if (mapView.lonSpan > want) mapView.lonSpan = want;
  redrawMap();
}
function redrawMap() {
  if (mapRedraw) return;
  mapRedraw = requestAnimationFrame(() => {
    mapRedraw = 0;
    if (mapStatus) reflectTracks(mapStatus);
  });
}

// Selecting is a highlight: the circled icon on the map and the lit row in the
// list. Click either to select; click again to clear. A drag is not a click —
// the moved flag is what keeps letting go of a pan from toggling a selection.
let selectedTrackId = '';
let mapHits = [];   // [{id,x,y}] in canvas pixels, from the last draw
let mapDrag = null;
let mapDragMoved = false;

// POINTER events, not mouse events, and that is the whole of the touch story:
// a browser synthesises mousedown/mouseup from a TAP, but a one-finger DRAG it
// keeps for itself as a scroll and synthesises nothing at all - so a map wired
// to mousemove pans under a mouse and is inert under a finger. Measured on the
// page: a ten-step touch drag across the canvas left mapView bit-identical.
// One pointer path covers mouse, finger and pen, and setPointerCapture also
// fixes the mouse case where the button is released outside the window.
$('map').addEventListener('pointerdown', (ev) => {
  if (ev.button !== 0 || !ev.isPrimary) return;
  mapDrag = { x: ev.clientX, y: ev.clientY, moved: false, id: ev.pointerId };
  // Capture so the rest of the gesture is delivered here even when it leaves
  // the canvas - and so the pointerup arrives, which is what clears the drag.
  try { ev.target.setPointerCapture(ev.pointerId); } catch (_) {}
});
window.addEventListener('pointermove', (ev) => {
  if (!mapDrag || !mapProj || ev.pointerId !== mapDrag.id) return;
  const dx = ev.clientX - mapDrag.x, dy = ev.clientY - mapDrag.y;
  // A few pixels of slop, so an ordinary click on a target does not count as
  // a pan and lose itself.
  if (!mapDrag.moved && Math.hypot(dx, dy) < 4) return;
  mapDrag.moved = true;
  const c = $('map');
  const r = c.getBoundingClientRect();
  const px = dx * c.width / r.width, py = dy * c.height / r.height;
  mapView.user = true;
  mapView.centreLon -= px / mapProj.w * mapProj.lonSpan;
  if (mapView.centreLon > 180) mapView.centreLon -= 360;
  if (mapView.centreLon < -180) mapView.centreLon += 360;
  // Vertical pixels convert through the projection that drew the frame: under
  // tiles a degree of latitude is not a constant number of pixels.
  if (mapProj.merc) {
    mapView.centreLat = mercLatN(mercYn(mapView.centreLat) - py / mapProj.pixPerWorld);
  } else {
    mapView.centreLat += py / mapProj.h * mapProj.latSpan;
  }
  mapView.centreLat = Math.max(-85, Math.min(85, mapView.centreLat));
  mapDrag.x = ev.clientX; mapDrag.y = ev.clientY;
  redrawMap();
});
// pointercancel as well as pointerup: a gesture the browser takes away (a
// system edge swipe, a second finger) ends with a cancel and no up, and a drag
// left armed would then follow the pointer with nothing held down.
['pointerup', 'pointercancel'].forEach((t) => {
  window.addEventListener(t, (ev) => {
    if (mapDrag && ev.pointerId !== mapDrag.id) return;
    mapDragMoved = !!(mapDrag && mapDrag.moved);
    mapDrag = null;
  });
});

$('map').addEventListener('wheel', (ev) => {
  if (!mapProj) return;
  ev.preventDefault();
  const c = $('map');
  const r = c.getBoundingClientRect();
  const mx = (ev.clientX - r.left) * c.width / r.width;
  const my = (ev.clientY - r.top) * c.height / r.height;
  const p = mapProj;
  // Zoom about the CURSOR, exactly as the desktop does: the world point under
  // it before the zoom must still be under it after.
  const lonB = mapView.centreLon + (mx - p.w / 2) / p.w * p.lonSpan;
  const latB = p.merc
      ? mercLatN(mercYn(mapView.centreLat) + (my - p.h / 2) / p.pixPerWorld)
      : mapView.centreLat - (my - p.h / 2) / p.h * p.latSpan;
  mapView.user = true;
  mapView.lonSpan = Math.max(0.02, Math.min(360,
      mapView.lonSpan * Math.pow(0.85, ev.deltaY < 0 ? 1 : -1)));
  const lonSpan2 = mapView.lonSpan;
  const lonA = mapView.centreLon + (mx - p.w / 2) / p.w * lonSpan2;
  mapView.centreLon += lonB - lonA;
  if (p.merc) {
    const ppw2 = p.w * 360 / lonSpan2;
    const atY = mercYn(mapView.centreLat) + (my - p.h / 2) / ppw2;
    mapView.centreLat = mercLatN(mercYn(mapView.centreLat) + (mercYn(latB) - atY));
  } else {
    const latSpan2 = lonSpan2 * p.h / p.w;
    const latA = mapView.centreLat - (my - p.h / 2) / p.h * latSpan2;
    mapView.centreLat += latB - latA;
  }
  mapView.centreLat = Math.max(-85, Math.min(85, mapView.centreLat));
  redrawMap();
}, { passive: false });

$('map').addEventListener('dblclick', () => {
  mapView.user = false;   // back to following the targets
  redrawMap();
});

// --- splitters ---------------------------------------------------------------
// The desktop's draggable dividers, for the browser: one between spectrum and
// waterfall, one between the waterfall and the panel area (drag UP to give the
// map the height the waterfall loses). Sizes are a per-device display
// preference, so they live in localStorage, not in the radio's config.
function dragSplit(handle, getStart, onMove) {
  let st = null;
  handle.addEventListener('mousedown', (ev) => {
    if (ev.button !== 0) return;
    ev.preventDefault();
    st = { y: ev.clientY, v: getStart() };
    handle.classList.add('active');
  });
  window.addEventListener('mousemove', (ev) => {
    if (!st) return;
    onMove(ev.clientY - st.y, st.v);
  });
  window.addEventListener('mouseup', () => {
    if (st) { st = null; handle.classList.remove('active'); }
  });
}
(function setupSplitters() {
  const spec = $('spectrum'), scope = $('scope'), panels = $('panels');
  const mapC = $('map');
  const savedSpec = localStorage.getItem('foxsdr.splitSpec');
  if (savedSpec) spec.style.flexBasis = savedSpec;
  const takePanels = (px) => {
    panels.style.flexBasis = px + 'px';
    panels.style.height = px + 'px';
    panels.style.maxHeight = 'none';
  };
  const savedPan = localStorage.getItem('foxsdr.splitPanels');
  if (savedPan) takePanels(parseInt(savedPan, 10));
  const savedMap = localStorage.getItem('foxsdr.mapH');
  if (savedMap) mapC.style.flexBasis = parseInt(savedMap, 10) + 'px';
  dragSplit($('splitScope'),
    () => spec.getBoundingClientRect().height,
    (dy, start) => {
      // Bounded so neither trace can be dragged out of existence.
      const px = Math.max(60, Math.min(scope.clientHeight - 80, start + dy));
      const pct = (px / scope.clientHeight * 100).toFixed(1) + '%';
      spec.style.flexBasis = pct;
      localStorage.setItem('foxsdr.splitSpec', pct);
    });
  dragSplit($('splitPanels'),
    () => ({ p: panels.getBoundingClientRect().height,
             m: mapC.getBoundingClientRect().height }),
    (dy, start) => {
      // Dragging UP (negative dy) grows the panel area at the scope's
      // expense — and the MAP CANVAS grows by the same amount, because the
      // area is a scroll container: with other panels below, spare height
      // becomes scroll room, so growing the area alone never grows the map,
      // which is the thing the user is dragging for.
      const mainEl = $('main');
      const px = Math.round(Math.max(120, Math.min(mainEl.clientHeight - 160, start.p - dy)));
      takePanels(px);
      localStorage.setItem('foxsdr.splitPanels', String(px));
      const mapPx = Math.round(Math.max(180, start.m + (px - start.p)));
      mapC.style.flexBasis = mapPx + 'px';
      localStorage.setItem('foxsdr.mapH', String(mapPx));
      redrawMap();
    });
})();
// The panel splitter only exists when there is a panel to resize.
function reflectSplitters() {
  const any = ['mapWrap', 'imagesWrap', 'decodedWrap']
      .some((id) => !$(id).classList.contains('hidden'));
  $('splitPanels').classList.toggle('hidden', !any);
}

// --- pop-out windows ---------------------------------------------------------
// ?view=<region> dresses this same page down to one region; the buttons open
// that in a fresh browser window. Same origin, same session cookie, same
// polling - the pop-out is not a second client, just a second viewport.
const POPOUT_VIEW = new URLSearchParams(location.search).get('view') || '';
if (['scope', 'map', 'images', 'decoded'].indexOf(POPOUT_VIEW) >= 0) {
  document.body.dataset.view = POPOUT_VIEW;
  document.title = 'FoxSDR - ' + POPOUT_VIEW;
}
document.querySelectorAll('button.pop').forEach((b) => {
  b.addEventListener('click', (ev) => {
    ev.stopPropagation();
    window.open('/?view=' + b.dataset.view, 'foxsdr-' + b.dataset.view,
                'width=1000,height=700,noopener');
  });
});

// --- the control column's width ----------------------------------------------
(function setupSideGrip() {
  const saved = localStorage.getItem('foxsdr.sideW');
  if (saved) document.documentElement.style.setProperty('--side', saved);
  const grip = $('sideGrip');
  if (!grip) return;
  let st = null;
  grip.addEventListener('mousedown', (ev) => {
    if (ev.button !== 0) return;
    ev.preventDefault();
    st = { x: ev.clientX, w: document.getElementById('side').getBoundingClientRect().width };
    grip.classList.add('active');
  });
  window.addEventListener('mousemove', (ev) => {
    if (!st) return;
    const w = Math.round(Math.max(220, Math.min(window.innerWidth * 0.6, st.w + (ev.clientX - st.x))));
    document.documentElement.style.setProperty('--side', w + 'px');
    localStorage.setItem('foxsdr.sideW', w + 'px');
  });
  window.addEventListener('mouseup', () => {
    if (st) { st = null; grip.classList.remove('active'); }
  });
})();

$('map').addEventListener('click', (ev) => {
  if (mapDragMoved) { mapDragMoved = false; return; }
  const c = $('map');
  const r = c.getBoundingClientRect();
  const mx = (ev.clientX - r.left) * c.width / r.width;
  const my = (ev.clientY - r.top) * c.height / r.height;
  let best = null, bestD = 18 * (c.width / r.width);
  mapHits.forEach((p) => {
    const d = Math.hypot(p.x - mx, p.y - my);
    if (d < bestD) { bestD = d; best = p; }
  });
  selectedTrackId = (best && best.id !== selectedTrackId) ? best.id : '';
  redrawMap();
});
)JS";

constexpr char kAppJs2b[] = R"JS(
function fmtBytes(n) {
  if (n >= 1e9) return (n / 1e9).toFixed(2) + ' GB';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + ' MB';
  if (n >= 1e3) return (n / 1e3).toFixed(0) + ' kB';
  return n + ' B';
}

let lastBmKey = '', lastDecodedKey = '';

function reflectExtras(s) {
  // WHY THE STREAM IS SILENT, beside the Listen button that started it. A
  // browser hearing nothing has no other way to tell a muted radio from a
  // dead stream, and this page has no Stop button for a plugin - that stays
  // in the window - so the badge says who, and the window is where it is
  // undone.
  const mb = $('mutedBy');
  mb.textContent = s.audioMutedBy ? ('muted by ' + s.audioMutedBy) : '';
  mb.classList.toggle('hidden', !s.audioMutedBy);

  // Recorder. The buttons carry their own state, so one control both starts
  // and stops — the same shape the desktop's Record/Stop pair has.
  $('recIq').textContent = s.iqRecording ? 'Stop IQ' : 'Record IQ';
  $('recIq').classList.toggle('on', s.iqRecording);
  $('recAudio').textContent = s.audioRecording ? 'Stop audio' : 'Record audio';
  $('recAudio').classList.toggle('on', s.audioRecording);
  const parts = [];
  if (s.iqRecording) parts.push('IQ ' + fmtBytes(s.iqBytes));
  if (s.audioRecording) parts.push('audio ' + fmtBytes(s.audioBytes));
  if (!parts.length && s.recordDir) parts.push('into ' + s.recordDir);
  $('recInfo').textContent = parts.join('  |  ');
  $('recError').textContent = s.recordError || '';

  // Bookmarks, rebuilt only when the list changes so a click is never
  // intercepted by a row being replaced underneath it.
  // The COUNT is part of the key on purpose. Without it, an empty list keys to
  // the empty string — which is exactly what the delete handler resets the
  // cached key to, so the rebuild was skipped and the deleted row stayed on
  // screen even though the server had removed it.
  const bmKey = s.bookmarks.length + '#' +
                s.bookmarks.map(b => b.name + '@' + b.freqHz).join('|');
  if (bmKey !== lastBmKey) {
    lastBmKey = bmKey;
    const box = $('bookmarks');
    box.innerHTML = '';
    s.bookmarks.forEach((b, i) => {
      const row = document.createElement('div');
      row.className = 'bm';
      const f = document.createElement('span');
      f.className = 'f';
      f.textContent = (b.freqHz / 1e6).toFixed(4) + ' MHz ' + b.mode;
      const n = document.createElement('span');
      n.className = 'n';
      n.textContent = b.name;
      const go = document.createElement('button');
      go.textContent = 'Tune';
      go.addEventListener('click', () => control({ bookmarkTune: i }));
      const del = document.createElement('button');
      del.className = 'del';
      del.textContent = 'Delete';
      del.addEventListener('click', () => { lastBmKey = ''; control({ bookmarkRemove: i }); });
      row.appendChild(f); row.appendChild(n); row.appendChild(go); row.appendChild(del);
      box.appendChild(row);
    });
  }

  // Scanner.
  syncControl($('scanFrom'), (s.scanStartHz / 1e6).toFixed(3));
  syncControl($('scanTo'), (s.scanStopHz / 1e6).toFixed(3));
  syncControl($('scanStep'), (s.scanStepHz / 1e3).toFixed(0));
  $('scanToggle').textContent = s.scannerActive ? 'Stop scan' : 'Start scan';
  $('scanToggle').classList.toggle('on', s.scannerActive);
  $('scanState').textContent = s.scannerActive ? s.scannerState : '';

  // Decoder output.
  const decKey = s.decoded.length + '#' + (s.decoded.length ? s.decoded[s.decoded.length-1].text : '');
  if (decKey !== lastDecodedKey) {
    lastDecodedKey = decKey;
    const has = s.decoded.length > 0;
    $('decodedWrap').classList.toggle('hidden', !has);
    if (has) {
      const el = $('decoded');
      const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
      el.textContent = s.decoded.map(d => '[' + d.plugin + '] ' + d.text).join('\n');
      // Follow the tail only if the reader was already at it — yanking the
      // view while someone is reading back is the classic log-panel bug.
      if (atBottom) el.scrollTop = el.scrollHeight;
    }
  }
}

function reflect(s) {
  $('playstop').textContent = s.running ? 'Stop' : 'Start';
  syncControl($('centre'), (s.centerHz / 1e6).toFixed(6));
  renderFreqDigits(s.centerHz);
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

  reflectSource(s);
  reflectExtras(s);
  reflectTracks(s);
  reflectPlugins(s);
  reflectImages(s);
  reflectCatalogue(s);
  reflectSplitters();

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
$('scan').addEventListener('click', () => control({ scanDevices: true }));
$('srcSel').addEventListener('change', () => {
  const v = $('srcSel').value;
  if (v === '' ) control({ sourceKind: 'siggen' });
  else if (v !== '__file__') control({ sourceKind: 'soapy', soapyArgs: v });
});
$('antenna').addEventListener('change', () => control({ antenna: $('antenna').value }));
$('srate').addEventListener('change', () => {
  control({ sampleRateHz: parseFloat($('srate').value) });
});
$('agc').addEventListener('change', () => control({ agc: $('agc').checked }));
$('pluginFetch').addEventListener('click', () => {
  // The ONLY thing that contacts the catalogue origin, and it still happens
  // only because somebody pressed this.
  lastCatKey = '';
  control({ pluginFetch: true });
});
$('recIq').addEventListener('click', () => {
  control({ recordIq: $('recIq').textContent === 'Record IQ' });
});
$('recAudio').addEventListener('click', () => {
  control({ recordAudio: $('recAudio').textContent === 'Record audio' });
});
$('bmAdd').addEventListener('click', () => {
  const name = $('bmName').value.trim();
  if (!name) { $('ctlError').textContent = 'Give the bookmark a name first.'; return; }
  lastBmKey = '';
  control({ bookmarkAdd: name }).then(() => { $('bmName').value = ''; });
});
$('bmName').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); $('bmAdd').click(); }
});
$('scanToggle').addEventListener('click', () => {
  const on = $('scanToggle').textContent === 'Start scan';
  const patch = { scannerActive: on };
  if (on) {
    const a = parseFloat($('scanFrom').value), b = parseFloat($('scanTo').value);
    const st = parseFloat($('scanStep').value);
    if (isFinite(a)) patch.scanStartHz = Math.round(a * 1e6);
    if (isFinite(b)) patch.scanStopHz = Math.round(b * 1e6);
    if (isFinite(st)) patch.scanStepHz = Math.round(st * 1e3);
  }
  control(patch);
});

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

// The canvases are SIZED BY CSS (they fill the centre panel), so their pixel
// buffers have to be matched to their rendered size. Without this the browser
// stretches the 300x150 default across the panel and the trace looks soft and
// the waterfall smears. Re-run on resize, and account for device pixel ratio
// so it stays sharp on a phone or a scaled display.
function fitCanvas(c) {
  const r = c.getBoundingClientRect();
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = Math.max(1, Math.round(r.width * dpr));
  const h = Math.max(1, Math.round(r.height * dpr));
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; return true; }
  return false;
}
function fitCanvases() {
  fitCanvas($('spectrum'));
  // Resizing clears the waterfall's history; there is nothing to preserve it
  // from, since the pixels ARE the history.
  fitCanvas($('waterfall'));
}
window.addEventListener('resize', fitCanvases);

function binToUnit(q, wireMin, wireMax) {
  const db = wireMin + (q / 255) * (wireMax - wireMin);
  const span = (viewDbMax - viewDbMin) || 1;
  const t = (db - viewDbMin) / span;
  return t < 0 ? 0 : (t > 1 ? 1 : t);
}

// PHOSPHOR, the same tube the desktop's waterfall and spectrum are drawn on
// (src/gui/waterfall_view.cpp) - quiet phosphor at the floor, through green
// and yellow-green, to a warm cream at the top. These five are that ramp
// resampled at t = 0, 1/4, 1/2, 3/4, 1, because colormap() below spaces its
// stops EVENLY and the desktop's table does not; resampling matches the ramp
// without touching the interpolation.
//
// THE PROPERTY THIS TABLE EXISTS FOR IS UNCHANGED, and it is not a matter of
// taste: a waterfall is a MEASUREMENT, so a stronger signal must never render
// darker than a weaker one. The anchors' perceived brightness (BT.601 luma)
// strictly increases - 14.7 -> 58.1 -> 109.8 -> 169.1 -> 230.2 - so linear
// interpolation between them is monotone, exactly as the blue-to-red table
// this replaced was (4.6 -> 41.7 -> 105.1 -> 141.9 -> 145.2). The floor is
// lifted off true black so a signal a few dB above the noise stays visible.
const WF_STOPS = [[6,20,10],[14,84,40],[50,150,60],[150,200,60],[240,235,180]];
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
    ctx.fillStyle = 'rgba(255,255,255,0.11)';
    ctx.fillRect(x - 1, 0, 3, h);
    ctx.strokeStyle = 'rgba(240,168,64,0.78)';
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  }

  ctx.strokeStyle = '#8fd9a0';
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

let specBusy = false;
async function pollSpectrum() {
  // Never overlapped: on a slow link a 66 ms cadence must fall behind
  // gracefully (fewer rows), not stack requests until the pool is starved.
  if (specBusy) return;
  specBusy = true;
  try { await pollSpectrumOnce(); } finally { specBusy = false; }
}
async function pollSpectrumOnce() {
  const s = await getJson('/api/spectrum?since=' + seq);
  if (!s || !s.bins) return;
  seq = s.seq;
  if (s.spanHz) lastSpanHz = s.spanHz;
  const raw = atob(s.bins);
  const bins = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bins[i] = raw.charCodeAt(i);
  fitCanvases();
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

// TWO CADENCES. Status at 4 Hz is plenty for readouts and lists - but the
// waterfall grows ONE ROW per spectrum poll, so its row rate IS the poll
// rate. At 4 Hz the picture a little way down the panel is half a minute
// old while the desktop's is seconds old (user-reported as "30 s behind");
// ~15 Hz reads as live. Each response is ~2 KB on a kept-alive connection,
// so the faster cadence costs ~30 KB/s and no extra connections.
let specTimer = null;
function startPolling() {
  if (timer) return;
  timer = setInterval(() => { pollStatus(); }, 250);
  specTimer = setInterval(() => { pollSpectrum(); }, 66);
  pollStatus(); pollSpectrum();
}
function stopPolling() {
  if (timer) { clearInterval(timer); timer = null; }
  if (specTimer) { clearInterval(specTimer); specTimer = null; }
}

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
        std::string(kAppJs1) + kAppJs2 + kAppJs2t + kAppJs2b + kAppJs2c + kAppJs2d +
        kAppJs2e + kAppJs3;
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

    void setImages(std::vector<WebImage> imgs) {
        std::lock_guard<std::mutex> lock(imageMutex_);
        images_ = std::move(imgs);
    }
    bool imageAt(std::size_t i, std::vector<std::uint8_t>& out) const {
        std::lock_guard<std::mutex> lock(imageMutex_);
        if (i >= images_.size() || images_[i].bmp.empty()) {
            return false;
        }
        out = images_[i].bmp;
        return true;
    }

    // --- Basemap tiles (see web_server.hpp for the shape of the whole thing) --

    std::vector<WebServer::TileRequest> takePendingTileRequests() {
        std::lock_guard<std::mutex> lock(tileMutex_);
        std::vector<WebServer::TileRequest> out(pendingTiles_.begin(), pendingTiles_.end());
        pendingTiles_.clear();
        pendingTileKeys_.clear();
        return out;
    }

    void setTile(std::uint32_t z, std::uint32_t x, std::uint32_t y,
                 std::vector<std::uint8_t> bmp) {
        std::lock_guard<std::mutex> lock(tileMutex_);
        TileEntry& e = tiles_[tileKey(z, x, y)];
        e.bmp = std::move(bmp);
        e.lastUsed = ++tileTick_;
        if (tiles_.size() <= WebServer::kMaxStoredTiles) {
            return;
        }
        // Evict the least recently served. One over the cap means one out, so
        // this is a min-scan, not a sort.
        auto oldest = tiles_.begin();
        for (auto it = tiles_.begin(); it != tiles_.end(); ++it) {
            if (it->second.lastUsed < oldest->second.lastUsed) {
                oldest = it;
            }
        }
        tiles_.erase(oldest);
    }

    void clearTiles() {
        std::lock_guard<std::mutex> lock(tileMutex_);
        tiles_.clear();
        pendingTiles_.clear();
        pendingTileKeys_.clear();
    }

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

    // Decoded pictures, already BMP-encoded by the application.
    mutable std::mutex imageMutex_;
    std::vector<WebImage> images_;

    // Basemap tiles published by the application, and the requests waiting for
    // it. An entry with an empty bmp means the plugin said MISSING — a real
    // answer, kept so the browser can be told to stop asking.
    struct TileEntry {
        std::vector<std::uint8_t> bmp;
        std::uint64_t lastUsed = 0;
    };
    static std::uint64_t tileKey(std::uint32_t z, std::uint32_t x, std::uint32_t y) {
        return (static_cast<std::uint64_t>(z) << 58) |
               (static_cast<std::uint64_t>(y) << 29) | static_cast<std::uint64_t>(x);
    }
    // What the tile route found, mapped onto what HTTP can say.
    enum class TileLookup { Served, Missing, Pending };
    TileLookup lookupTile(std::uint32_t z, std::uint32_t x, std::uint32_t y,
                          std::vector<std::uint8_t>& out) {
        std::lock_guard<std::mutex> lock(tileMutex_);
        auto it = tiles_.find(tileKey(z, x, y));
        if (it != tiles_.end()) {
            it->second.lastUsed = ++tileTick_;
            if (it->second.bmp.empty()) {
                return TileLookup::Missing;
            }
            out = it->second.bmp;
            return TileLookup::Served;
        }
        // Not held: record the want, deduplicated, bounded. A full queue drops
        // the recording, not the response — the browser retries anyway.
        const std::uint64_t k = tileKey(z, x, y);
        if (pendingTileKeys_.insert(k).second) {
            if (pendingTiles_.size() < WebServer::kMaxQueuedTileRequests) {
                pendingTiles_.push_back({z, x, y});
            } else {
                pendingTileKeys_.erase(k);
            }
        }
        return TileLookup::Pending;
    }

    mutable std::mutex tileMutex_;
    std::unordered_map<std::uint64_t, TileEntry> tiles_;
    std::uint64_t tileTick_ = 0;
    std::deque<WebServer::TileRequest> pendingTiles_;
    std::set<std::uint64_t> pendingTileKeys_;

    // Live audio for listening browsers, and how many are currently streaming.
    mutable AudioRing audio_;
    std::atomic<int> listeners_{0};

    // Highest spectrum sequence this process has served. Zero at start-up,
    // which is exactly why a cursor left over from a previous run reads as
    // impossible and gets reset — see the spectrum handler.
    mutable std::atomic<std::uint64_t> maxSeenSeq_{0};

    std::unique_ptr<httplib::Server> svr_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

void WebServer::Impl::deny(httplib::Response& res, const std::string& message,
                           int status) {
    nlohmann::json j;
    j["error"] = message;
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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
        res.set_content(appCss(), "text/css; charset=utf-8");
    });

    svr.Get("/app.js", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(appJs(), "text/javascript; charset=utf-8");
    });

    // THE RADAR UNIT - a separate interface, not a page of this one. It is
    // served here because the desktop shell that hosts it (foxsdr-radar.exe)
    // has to reach the same tiles and the same /api/status as this server
    // already publishes, and serving it from the same origin is what makes
    // that need no cross-origin exception. The shell renders it in a real
    // window; nothing about it is reachable by wandering in with a browser
    // that has not signed in, because every /api route below still checks.
    //
    // Three routes, not one page with the style and script inline, for the
    // same reason "/" is split: the policy below forbids inline script and
    // inline style outright, and that is the property worth keeping.
    const auto radarCsp = [](httplib::Response& res) {
        res.set_header("Content-Security-Policy",
                       "default-src 'none'; script-src 'self'; style-src 'self'; "
                       "connect-src 'self'; img-src 'self' data:; "
                       "form-action 'none'; frame-ancestors 'none'; base-uri 'none'");
    };
    svr.Get("/radar", [radarCsp](const httplib::Request&, httplib::Response& res) {
        radarCsp(res);
        res.set_content(reinterpret_cast<const char*>(cascade::radar::kRadarHtml),
                        cascade::radar::kRadarHtmlLen, "text/html; charset=utf-8");
    });
    svr.Get("/radar.css", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(reinterpret_cast<const char*>(cascade::radar::kRadarCss),
                        cascade::radar::kRadarCssLen, "text/css; charset=utf-8");
    });
    svr.Get("/radar.js", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(reinterpret_cast<const char*>(cascade::radar::kRadarJs),
                        cascade::radar::kRadarJsLen, "text/javascript; charset=utf-8");
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
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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
        // verifyLogin, not `user == expectedUser && verifyPassword(...)`: that
        // spelling short-circuits, and answering a wrong user name without
        // paying the derivation makes the two cases tell themselves apart by
        // TIMING, which is exactly what the one shared message below exists to
        // prevent.
        const bool ok = verifyLogin(user, password, expectedUser, record, verifyError);
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
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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
        j["sourceKind"] = s.sourceKind;
        j["soapyArgs"] = s.soapyArgs;
        j["antenna"] = s.antenna;
        j["antennas"] = s.antennas;
        j["agcSupported"] = s.agcSupported;
        j["agc"] = s.agc;
        j["sourceBusy"] = s.sourceBusy;
        j["sourceError"] = s.sourceError;
        {
            nlohmann::json devices = nlohmann::json::array();
            for (const RadioStatus::SoapyDevice& d : s.devices) {
                devices.push_back({{"label", d.label}, {"args", d.args}});
            }
            j["devices"] = std::move(devices);
            nlohmann::json gains = nlohmann::json::array();
            for (const RadioStatus::GainStage& g : s.gains) {
                gains.push_back({{"name", g.name}, {"db", g.db}});
            }
            j["gains"] = std::move(gains);

            nlohmann::json marks = nlohmann::json::array();
            for (const RadioStatus::Bookmark& b : s.bookmarks) {
                marks.push_back({{"name", b.name},
                                 {"freqHz", b.freqHz},
                                 {"mode", b.mode},
                                 {"bandwidthHz", b.bandwidthHz}});
            }
            j["bookmarks"] = std::move(marks);

            nlohmann::json lines = nlohmann::json::array();
            for (const RadioStatus::DecodedLine& d : s.decoded) {
                lines.push_back({{"plugin", d.plugin}, {"text", d.text}});
            }
            j["decoded"] = std::move(lines);

            // NaN is not representable in JSON, and nlohmann writes it as
            // null. That is exactly the wanted meaning here — "not reported" —
            // but it must be DELIBERATE rather than incidental, so it is done
            // explicitly: a number would claim an altitude, course or speed the
            // decoder never heard.
            const auto orNull = [](double v) {
                return std::isfinite(v) ? nlohmann::json(v) : nlohmann::json(nullptr);
            };
            nlohmann::json tracks = nlohmann::json::array();
            for (const RadioStatus::Track& t : s.tracks) {
                tracks.push_back({{"id", t.id},
                                  {"label", t.label},
                                  {"plugin", t.plugin},
                                  {"latDeg", t.latDeg},
                                  {"lonDeg", t.lonDeg},
                                  {"altM", orNull(t.altM)},
                                  {"courseDeg", orNull(t.courseDeg)},
                                  {"speedMps", orNull(t.speedMps)},
                                  {"ageMs", t.ageMs},
                                  {"kind", t.kind},
                                  {"flags", t.flags},
                                  {"infoState", t.infoState},
                                  {"reg", t.registration},
                                  {"acType", t.acType},
                                  {"acOperator", t.acOperator},
                                  {"acCountry", t.acCountry}});
            }
            j["tracks"] = std::move(tracks);
            j["rxPositionSet"] = s.rxPositionSet;
            j["rxLatDeg"] = s.rxPositionSet ? nlohmann::json(s.rxLatDeg) : nlohmann::json(nullptr);
            j["rxLonDeg"] = s.rxPositionSet ? nlohmann::json(s.rxLonDeg) : nlohmann::json(nullptr);

            // What the browser's map needs to fetch and CREDIT the imagery —
            // the attribution requirement travels with the tiles or the tiles
            // do not travel.
            j["basemap"] = {{"active", s.basemap.active},
                            {"attribution", s.basemap.attribution},
                            {"minZoom", s.basemap.minZoom},
                            {"maxZoom", s.basemap.maxZoom},
                            {"tileSize", s.basemap.tileSize}};

            nlohmann::json plugins = nlohmann::json::array();
            for (const RadioStatus::Plugin& p : s.plugins) {
                plugins.push_back({{"name", p.name},
                                   {"version", p.version},
                                   {"licence", p.licence},
                                   {"fileName", p.fileName},
                                   {"loaded", p.loaded},
                                   {"stopped", p.stopped},
                                   {"error", p.error},
                                   {"idleReason", p.idleReason},
                                   {"canRequestTune", p.canRequestTune},
                                   {"tuneAllowed", p.tuneAllowed},
                                   {"presets", [&p]() {
                                        nlohmann::json a = nlohmann::json::array();
                                        for (const auto& ps : p.presets) {
                                            a.push_back({{"label", ps.label},
                                                         {"frequencyHz", ps.frequencyHz},
                                                         {"bandwidthHz", ps.bandwidthHz},
                                                         {"sampleRateHz", ps.sampleRateHz}});
                                        }
                                        return a;
                                    }()}});
            }
            j["plugins"] = std::move(plugins);

            nlohmann::json cat = nlohmann::json::array();
            for (const RadioStatus::CatalogEntry& e : s.catalogue) {
                cat.push_back({{"id", e.id},
                               {"name", e.name},
                               {"version", e.version},
                               {"licence", e.licence},
                               {"summary", e.summary},
                               {"legalNotice", e.legalNotice},
                               {"installed", e.installed},
                               {"blockedReason", e.blockedReason}});
            }
            j["catalogue"] = std::move(cat);
        }
        j["catalogueStatus"] = s.catalogueStatus;
        j["catalogueError"] = s.catalogueError;
        j["catalogueBusy"] = s.catalogueBusy;
        j["installReport"] = s.installReport;
        j["installError"] = s.installError;
        {

            nlohmann::json images = nlohmann::json::array();
            for (const RadioStatus::Image& im : s.images) {
                images.push_back({{"plugin", im.plugin},
                                  {"width", im.width},
                                  {"height", im.height},
                                  {"complete", im.complete},
                                  {"revision", im.revision}});
            }
            j["images"] = std::move(images);
        }
        j["iqRecording"] = s.iqRecording;
        j["audioMutedBy"] = s.audioMutedBy;
        j["audioRecording"] = s.audioRecording;
        j["iqBytes"] = s.iqBytes;
        j["audioBytes"] = s.audioBytes;
        j["recordDir"] = s.recordDir;
        j["recordError"] = s.recordError;
        j["scannerActive"] = s.scannerActive;
        j["scannerState"] = s.scannerState;
        j["scanStartHz"] = s.scanStartHz;
        j["scanStopHz"] = s.scanStopHz;
        j["scanStepHz"] = s.scanStepHz;
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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

    // One decoded picture as a 24-bit BMP. Separate from the status poll
    // because a picture is hundreds of kilobytes; the browser refetches only
    // when the revision in the status changes.
    svr.Get(R"(/api/image/(\d+))", [this](const httplib::Request& req,
                                          httplib::Response& res) {
        if (!authorised(req)) {
            deny(res, "sign in first");
            return;
        }
        std::size_t index = 0;
        try {
            index = static_cast<std::size_t>(std::stoul(req.matches[1].str()));
        } catch (const std::exception&) {
            deny(res, "bad image index", 400);
            return;
        }
        std::vector<std::uint8_t> bmp;
        if (!imageAt(index, bmp)) {
            deny(res, "no such image", 404);
            return;
        }
        res.set_content(reinterpret_cast<const char*>(bmp.data()), bmp.size(),
                        "image/bmp");
    });

    // One basemap tile as a 24-bit BMP. Three honest answers: 200 with the
    // tile, 202 "not here yet — ask again" (which is also what RECORDS the
    // want; see the header), and 404 for a tile the plugin said will never
    // exist, so the browser can stop asking.
    svr.Get(R"(/api/tile/(\d+)/(\d+)/(\d+))",
            [this](const httplib::Request& req, httplib::Response& res) {
        if (!authorised(req)) {
            deny(res, "sign in first");
            return;
        }
        std::uint32_t z = 0, x = 0, y = 0;
        try {
            z = static_cast<std::uint32_t>(std::stoul(req.matches[1].str()));
            x = static_cast<std::uint32_t>(std::stoul(req.matches[2].str()));
            y = static_cast<std::uint32_t>(std::stoul(req.matches[3].str()));
        } catch (const std::exception&) {
            deny(res, "bad tile address", 400);
            return;
        }
        // The XYZ grid is 2^z tiles each way, and the key packs z into 6 bits
        // with x and y into 29 each — z above 22 is beyond every real tile
        // server anyway and would not fit either constraint.
        if (z > 22u || x >= (1u << z) || y >= (1u << z)) {
            deny(res, "bad tile address", 400);
            return;
        }
        std::vector<std::uint8_t> bmp;
        switch (lookupTile(z, x, y, bmp)) {
            case TileLookup::Served:
                res.set_content(reinterpret_cast<const char*>(bmp.data()), bmp.size(),
                                "image/bmp");
                return;
            case TileLookup::Missing:
                deny(res, "no such tile", 404);
                return;
            case TileLookup::Pending: {
                nlohmann::json j;
                j["pending"] = true;
                res.status = 202;
                res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
                return;
            }
        }
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
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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
        // A CURSOR FROM THE FUTURE CAN ONLY BE A CURSOR FROM A PREVIOUS RUN.
        // Frame sequence numbers restart at zero when the application does, so
        // a browser left open across a restart - an upgrade, a crash, or the
        // user simply closing and reopening the receiver - would otherwise go
        // on asking for a frame newer than any that will ever be produced. The
        // status poll keeps working, so the page looks alive while the
        // spectrum and waterfall stay frozen for ever, and only a manual
        // reload fixes it. Treating an impossible cursor as "start again" is
        // what makes every already-open page recover by itself.
        if (snap.seq > maxSeenSeq_.load(std::memory_order_relaxed)) {
            snap.seq = 0;
        }
        const bool fresh = spectrum_ ? spectrum_(snap) : false;
        if (fresh) {
            // Highest sequence this PROCESS has served, which is what makes
            // the test above mean "impossible" rather than merely "large".
            std::uint64_t seen = maxSeenSeq_.load(std::memory_order_relaxed);
            while (snap.seq > seen &&
                   !maxSeenSeq_.compare_exchange_weak(seen, snap.seq,
                                                      std::memory_order_relaxed)) {
            }
        }
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
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
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
    // WITHOUT THIS, A THROWING HANDLER IS AN EMPTY 500 AND NOTHING ELSE —
    // no message, no route, nothing in any log. That is the difference
    // between a two-minute diagnosis and an afternoon: a serialisation
    // failure in the status handler takes the whole browser UI down, and
    // the only visible symptom is that the page stops updating. The message
    // goes to the application's console AND to the client, because whoever
    // is looking at the browser is the person who needs it.
    svr_->set_exception_handler([](const httplib::Request& req,
                                   httplib::Response& res,
                                   std::exception_ptr ep) {
        std::string what = "unknown exception";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        std::fprintf(stderr, "cascade: web handler threw on %s %s: %s\n",
                     req.method.c_str(), req.path.c_str(), what.c_str());
        std::fflush(stderr);
        nlohmann::json j;
        j["error"] = what;
        j["path"] = req.path;
        res.status = 500;
        res.set_content(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), "application/json");
    });
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
void WebServer::setImages(std::vector<WebImage> images) {
    impl_->setImages(std::move(images));
}
std::vector<WebServer::TileRequest> WebServer::takePendingTileRequests() {
    return impl_->takePendingTileRequests();
}
void WebServer::setTile(std::uint32_t z, std::uint32_t x, std::uint32_t y,
                        std::vector<std::uint8_t> bmp) {
    impl_->setTile(z, x, y, std::move(bmp));
}
void WebServer::clearTiles() { impl_->clearTiles(); }

}  // namespace cascade::net
