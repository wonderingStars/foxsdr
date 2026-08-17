// Implementation of the web server. See net/web_server.hpp for the design, and
// net/web_policy.hpp for the bind rules this file obeys rather than restates.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_server.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

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
<header><h1>FoxSDR</h1><span id="remote" class="badge hidden">remote access</span></header>
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
  <canvas id="spectrum" width="1024" height="256"></canvas>
  <div id="controls">
    <div class="row">
      <button id="playstop">Start</button>
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
)CSS";

constexpr char kAppJs[] = R"JS(
'use strict';
const $ = (id) => document.getElementById(id);
let seq = 0, timer = null;

function showError(msg) { $('loginError').textContent = msg || ''; }

async function getJson(url) {
  const r = await fetch(url, { credentials: 'same-origin' });
  if (r.status === 401) { stopPolling(); await refreshSession(); return null; }
  if (!r.ok) return null;
  return r.json();
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
  const r = await fetch('/api/control', {
    method: 'POST',
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(patch),
  });
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
  if (s.mode !== currentMode) {
    currentMode = s.mode;
    Array.from($('modes').children).forEach((b) => {
      b.classList.toggle('on', b.dataset.mode === s.mode);
    });
  }
}

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

function drawSpectrum(bins, dbMin, dbMax) {
  const c = $('spectrum'), ctx = c.getContext('2d');
  const w = c.width, h = c.height, n = bins.length;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = '#4fb0ff';
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let i = 0; i < n; i++) {
    const x = (i / (n - 1)) * w;
    const y = h - (bins[i] / 255) * h;
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  }
  ctx.stroke();
}

async function pollSpectrum() {
  const s = await getJson('/api/spectrum?since=' + seq);
  if (!s || !s.bins) return;
  seq = s.seq;
  const raw = atob(s.bins);
  const bins = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) bins[i] = raw.charCodeAt(i);
  drawSpectrum(bins, s.dbMin, s.dbMax);
}

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
        res.set_content(kAppJs, "text/javascript; charset=utf-8");
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
        res.set_content(j.dump(), "application/json");
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

}  // namespace cascade::net
