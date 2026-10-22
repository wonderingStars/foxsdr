// test_server_host.cpp - THE ENGINE OWNS THE WEB AND CAT SERVERS (engine extraction stage 5b; docs/engine-stage4.md 6.10,
// docs/api-0.3-additions.md 8.16.5). The real web server over a loopback socket and the real CAT listener over another, both started,
// restarted and stopped by FOXAPI_OP_SERVER_CONFIG on a LOCAL ADMIN session, answering from the engine's own publish, and reaching the pass
// through the sessions they hold on the table - the whole of the ownership move, with no window in the way.
//
// test_remote_key_release.cpp holds the same server through the window and the key's standing conditions; test_engine_api_05a.cpp holds the
// table's side (the strict parse, the list, the facts) with the listeners switched off; this file holds the LISTENERS:
//
//   H1  A hermetic engine (testHooks=1) hosts nothing: SERVER_CONFIG is recorded and published, no socket is opened.
//   H2  THE WEB SERVER, started by SERVER_CONFIG on a worker: the SERVERS entry is ACTIVE on the port, the state carries WEB_LISTENING, the
//       page answers, and /api/status is the engine's own publish byte for byte (statusJson over Engine::webStatusNow).
//   H3  THE WEB SESSION: a POST /api/control reaches the pass through the web server's engine session and moves the radio; the session
//       exists while the server runs and is gone when it stops; the page's spectrum is the table's frame (stage 5e), read on that session.
//   H4  CAT, started by SERVER_CONFIG: f and F over a real socket, the F inside the band moving the offset only (through the connection's
//       own engine session since stage 5c; tests/test_cat_engine.cpp holds the whole of that); the list counts the client and its
//       commands, and counts them back down.
//   H5  A CHANGE RESTARTS, AND A STOP STOPS: the port changes (the old one refuses, the new one answers), disabling closes the socket.
//   H6  A REFUSAL IS A RESULT AND A RETRY IS THE OPERATOR'S: a port someone else holds is FAULT with the bind failure's sentence, and the
//       same configuration said again after it is freed starts the server; an off-machine bind with no password never opens a socket and
//       says why (the policy's sentence in full).
//   H7  THE PASSWORD: hashed on a worker, its record restarts the server, and the page then wants the login - a wrong password is 401, the
//       right one a session.
//   H8  THE CONFIG RESTORE starts what the file switched on (Engine::applyConfig), and the engine's half of the file reads back what runs.
//   H9  THE TEARDOWN: shutdownQuiesce stops both servers, first; nothing is listening afterwards.
//   H11 THE WEB SESSION, THROUGH THE WHOLE REAL CHAIN (stage 5d): fields, the page's application ops, bookmark rows, the PTT gate over a
//       real HTTP client; one engine session for the running server, 17 cookies = 1, stop = 0, restart = 1.
//   H12 THE PAGE'S SPECTRUM, WITH NO WINDOW (stage 5e): a free-running engine and a web server and nothing else - no window exists - and the page's
//       /api/spectrum long-poll gets frame after frame, each newer than its cursor: the table's read_spectrum on the web session.
//   H13 THE PAGE'S AUDIO, THROUGH THE WHOLE CHAIN (stage 5e): GET /api/audio streams the pipeline's audio from the table's read_audio - the wire
//       format (audio/L16 at 48 kHz mono, chunked, 16-bit samples), the panel's listener count, the cap of four with a 503 beyond, the slots given
//       back - with no window to push anything.
//   H14 THE PASS WITH A LISTENER, AND THE AUDIO LOCK HELD (stage 5e; the ledger's "browser remote no longer makes the window wait for the signal
//       thread", engine-merge-0.99.84.md:284): the control pass's figures with no listener and with one attached, and with the pipeline's audio
//       mutex - the one the DSP thread holds across a block - held for 1.5 s by the test seam while the listener is attached: the audio feed is the
//       pass's now (and every reader's), so it must not queue behind the block.
//   H15 NO WINDOW, NO TILES (stage 5e): the basemap's tiles are the one feed only the window's plugin client can give; a headless engine's status
//       says the basemap is not active, and a tile asked for anyway is answered "pending" (202) - never a picture, never an error.
//   H10 NEVER IN THE PASS: with a browser holding a request open (so the web server's stop must wait for it), the SERVER_CONFIG that
//       disables the server is applied by the free-running pass and the pass goes on at its rate while the WORKER is inside the stop. The
//       gap between two passes stays a small fraction of the second a stalled pass unkeys the transmitter at.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "test_check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/server_host.hpp"
#include "engine_api_test_env.hpp"
#include "foxsdr_api.h"
#include "net/status_compose.hpp"
#include "net/web_server.hpp"

// (test_check.hpp has CHECK only: a comparison that prints what it saw)
template <class T>
std::string showVal(const T& v) {
    if constexpr (std::is_convertible_v<const T&, std::string>) {
        return "\"" + std::string(v) + "\"";
    } else {
        return std::to_string(v);
    }
}
#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto va_ = (a);                                                                            \
        const auto vb_ = (b);                                                                            \
        CHECK(va_ == vb_);                                                                               \
        if (!(va_ == vb_)) {                                                                             \
            std::printf("      %s = %s, %s = %s\n", #a, showVal(va_).c_str(), #b, showVal(vb_).c_str()); \
        }                                                                                                \
    } while (0)

namespace cascade::gui {
struct AppWindowTestAccess {
    static std::uint64_t passes(cascade::engine::Engine& e) { return e.controlPasses_.load(); }
    // The pass's own figures (the measurement run's), over the window since the last mark.
    static void markPassStats(cascade::engine::Engine& e) { e.markPassStats(); }
    static std::string passStats(cascade::engine::Engine& e) { return e.passStatsJson(); }
    // The pipeline's audio mutex held for `ms` from the calling thread (the test seam FOXSDR_HOLD_AUDIO_LOCK stands on), `acquired` set when it is.
    static void holdAudioLock(cascade::engine::Engine& e, int ms, std::atomic<bool>* acquired) {
        e.pipeline_.holdLockForTest(cascade::core::Pipeline::LockForTest::Audio, ms, acquired);
    }
};
}  // namespace cascade::gui

using cascade::engine::Engine;
using Access = cascade::gui::AppWindowTestAccess;
namespace cmd = cascade::core::cmd;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

constexpr std::uint64_t kAllGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT |
                                     FOXAPI_GRANT_ADMIN | FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_IQ;

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ---- sockets (the shape test_cat_server.cpp uses) --------------------------------------------------------------------------------

void closeSock(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) { ::closesocket(s); }
#else
    if (s != INVALID_SOCKET) { ::close(s); }
#endif
}

void sockInit() {
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
#endif
}

SockT connectTo(int port, int rcvTimeoutMs = 250) {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { return INVALID_SOCKET; }
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(rcvTimeoutMs);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = rcvTimeoutMs / 1000;
    tv.tv_usec = (rcvTimeoutMs % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool sendRaw(SockT s, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int n = static_cast<int>(::send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (n <= 0) { return false; }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string readLines(SockT s, int lines, int timeoutMs = 3000) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int have = 0;
        for (char c : out) {
            if (c == '\n') { ++have; }
        }
        if (have >= lines) { break; }
        char buf[256];
        const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return out;
}

// A port nobody is listening on right now: bound, read back, closed.
int freePort() {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int port = 0;
    if (s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        socklen_t len = sizeof(a);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
    closeSock(s);
    return port;
}

// A listener the test holds, so that port is taken (the "something else has it" of H6).
struct PortHolder {
    SockT s = INVALID_SOCKET;
    int port = 0;
    explicit PortHolder(int p) : port(p) {
        sockInit();
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = ::htons(static_cast<unsigned short>(p));
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
#if defined(_WIN32)
        // (the default on Windows lets a second socket share a port; this one is exclusive, as a real rigctld's or a web server's is)
        const int excl = 1;
        ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
        held = s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && ::listen(s, 4) == 0;
    }
    ~PortHolder() { release(); }
    void release() {
        closeSock(s);
        s = INVALID_SOCKET;
    }
    bool held = false;
};

bool refuses(int port) {
    SockT s = connectTo(port, 200);
    if (s == INVALID_SOCKET) { return true; }
    closeSock(s);
    return false;
}

// ---- the engine ------------------------------------------------------------------------------------------------------------------

// A hermetic engine that HOSTS its servers (servers=1), passes by hand or on a thread. (Nothing is armed for the browser's control: since stage
// 5d the web server holds its own engine session while it runs, and the pass drains it.)
struct Rig {
    Engine e;
    bool threaded;
    FoxSession* op = nullptr;
    explicit Rig(const std::string& options, bool thread = false) : threaded(thread) {
        e.configureApi(options);
        e.initialise();
        (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
        e.controlPass();
        if (thread) { e.startControlThread(); }
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = kAllGrants;
        p.clientName = "the operator";
        (void)api()->open_session(e.apiHandle(), &p, &op);
    }
    ~Rig() {
        if (op != nullptr) { api()->close_session(op); }
        e.shutdown();
    }
    void pump(int n = 2) {
        for (int i = 0; i < n; ++i) { e.pump(); }
    }
    // SERVER_CONFIG, waiting for the answer and for the host's worker to have acted on it.
    FoxCommandResult config(const std::string& json, bool settle = true) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SERVER_CONFIG;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        if (api()->submit_text(op, &c, json.data(), static_cast<uint32_t>(json.size()), &sr) < 0) { return out; }
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            if (!threaded) { pump(1); }
            FoxCommandResult r[8];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(op, r, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    out = r[i];
                    if (settle) { (void)e.servers().waitIdle(std::chrono::seconds(30)); }
                    if (!threaded) { pump(2); }  // the host's report is published by the pass that follows it
                    return out;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    FoxListItem server(std::uint32_t index) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        (void)api()->read_list(op, FOXAPI_LIST_SERVERS, index, &it);
        return it;
    }
    std::string fact(const char* key) {
        char buf[2048];
        const int32_t n = api()->get_setting(op, key, buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string();
    }
    FoxReceiverState state() {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        for (int i = 0; i < 100 && api()->read_state(op, &st) != FOXAPI_OK; ++i) {}
        return st;
    }
};

bool active(const FoxListItem& it) { return (it.flags & FOXAPI_ITEM_ACTIVE) != 0u; }
bool fault(const FoxListItem& it) { return (it.flags & FOXAPI_ITEM_FAULT) != 0u; }

std::string webConfig(int port, const char* bind = "127.0.0.1") {
    return std::string("{\"web\":{\"enabled\":true,\"bindAddress\":\"") + bind + "\",\"port\":" + std::to_string(port) + "}}";
}
std::string catConfig(int port) { return "{\"cat\":{\"enabled\":true,\"bindAll\":false,\"port\":" + std::to_string(port) + "}}"; }

// ===================================================================================================================================

void testHermeticEngineHostsNothing() {
    Rig r("testHooks=1");  // no servers=1
    const int port = freePort();
    const FoxCommandResult a = r.config(webConfig(port));
    CHECK(a.status == FOXAPI_OK);
    CHECK_EQ(r.fact("fact.web.enabled"), std::string("1"));
    CHECK_EQ(r.fact("fact.web.port"), std::to_string(port));
    r.pump(3);
    CHECK(!r.e.servers().webServer().running());
    CHECK(refuses(port));
    CHECK(!active(r.server(0)));
    std::printf("H1: a hermetic engine records the web server's configuration (port %d) and opens no socket\n", port);
}

void testWebServerStartedBySessionConfig() {
    Rig r("testHooks=1;servers=1");
    const int port = freePort();
    CHECK(refuses(port));
    const FoxCommandResult a = r.config(webConfig(port));
    CHECK(a.status == FOXAPI_OK);
    CHECK(r.e.servers().webServer().running());
    const FoxListItem w = r.server(0);
    CHECK(active(w) && !fault(w));
    CHECK_EQ(w.value[0], static_cast<double>(port));
    CHECK((r.state().flags & FOXAPI_RX_WEB_LISTENING) != 0u);
    CHECK(r.e.servers().webListening());
    // The page answers, and /api/status is the engine's own publish, byte for byte.
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    const auto page = cli.Get("/");
    CHECK(page && page->status == 200);
    r.pump(2);  // settle the publish, then read both from the same one
    const auto st = cli.Get("/api/status");
    CHECK(st && st->status == 200);
    if (st) {
        const std::string want = cascade::net::statusJson(r.e.webStatusNow(), [](std::size_t) { return false; });
        CHECK(st->body == want);
        if (st->body != want) { std::printf("   status body %zu bytes, engine's %zu bytes\n", st->body.size(), want.size()); }
    }
    std::printf("H2: web server on :%d started by SERVER_CONFIG - SERVERS ACTIVE, WEB_LISTENING set, /api/status = the engine's publish (%zu bytes)\n",
                port, st ? st->body.size() : std::size_t{0});

    // H3: a browser's request reaches the pass through the web server's engine session; the spectrum the window feeds is what the page polls.
    CHECK_EQ(r.e.api().sessionCountForTest(cascade::engine::InternalKind::Web), std::size_t{1});   // one session for the running server
    const auto ctl = cli.Post("/api/control", "{\"vfoOffsetHz\":12345}", "application/json");
    CHECK(ctl && ctl->status == 202);
    r.pump(2);
    CHECK_EQ(r.state().vfoOffsetHz, 12345.0);
    // THE SPECTRUM IS THE TABLE'S (stage 5e): a frame in the engine's ring (no pass runs between the publish and the GET, so it is the newest) is
    // what the page's long-poll is answered with - read on the web server's own session, with nothing fed to the server by anyone.
    std::vector<float> bins(64, -80.0f);
    r.e.api().publishSpectrumForTest(bins.data(), bins.size(), 100.0e6, 2.0e6);
    FoxSpectrumInfo want{};
    want.structSize = sizeof(want);
    std::vector<float> tableBins(64);
    CHECK(api()->read_spectrum(r.op, 0, &want, tableBins.data(), static_cast<std::uint32_t>(tableBins.size())) == FOXAPI_OK);
    const auto sp = cli.Get("/api/spectrum?since=0");
    CHECK(sp && sp->status == 200);
    if (sp) {
        const nlohmann::json j = nlohmann::json::parse(sp->body, nullptr, false);
        CHECK(!j.is_discarded() && j.value("fresh", false));
        CHECK_EQ(j.value("seq", std::uint64_t{0}), want.seq);
        CHECK_EQ(j.value("centerHz", 0.0), 100.0e6);
        CHECK_EQ(j.value("spanHz", 0.0), 2.0e6);
    }
    std::printf("H3: POST /api/control moved the radio through the web server's session (vfoOffsetHz 12345); /api/spectrum served the table's frame (seq %llu)\n",
                static_cast<unsigned long long>(want.seq));
}

void testCatServer() {
    Rig r("testHooks=1;servers=1");
    const int port = freePort();
    const FoxCommandResult a = r.config(catConfig(port));
    CHECK(a.status == FOXAPI_OK);
    CHECK(r.e.servers().catServer().running());
    const FoxListItem c = r.server(1);
    CHECK(active(c));
    CHECK_EQ(c.value[0], static_cast<double>(port));
    SockT s = connectTo(port, 2000);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) { return; }
    CHECK(waitUntil(3000, [&] { return r.server(1).value[1] == 1.0; }));
    CHECK(sendRaw(s, "f\n"));
    const std::string hz = readLines(s, 1);
    const FoxReceiverState st = r.state();
    const long long want = static_cast<long long>(st.centreHz + st.vfoOffsetHz + 0.5);
    CHECK_EQ(hz, std::to_string(want) + "\n");
    // An F inside the band moves the offset only (planTune); the pass applies it from the connection's session (stage 5c: the internal tier).
    const long long target = static_cast<long long>(st.centreHz) + 40000;
    CHECK(sendRaw(s, "F " + std::to_string(target) + "\n"));
    CHECK_EQ(readLines(s, 1), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] {
        r.pump(1);
        return r.state().vfoOffsetHz == 40000.0;
    }));
    CHECK_EQ(r.state().centreHz, st.centreHz);
    CHECK(r.server(1).value[3] >= 2.0);  // two commands served
    closeSock(s);
    CHECK(waitUntil(3000, [&] { return r.server(1).value[1] == 0.0; }));
    std::printf("H4: CAT on :%d - f answered %lld, F inside the band moved the offset (centre kept), the list counted the client in and out\n",
                port, want);
}

void testChangeRestartsStopStops() {
    Rig r("testHooks=1;servers=1");
    const int p1 = freePort();
    CHECK(r.config(webConfig(p1)).status == FOXAPI_OK);
    CHECK(!refuses(p1));
    int p2 = freePort();
    while (p2 == p1) { p2 = freePort(); }
    CHECK(r.config(webConfig(p2)).status == FOXAPI_OK);
    CHECK(!refuses(p2));
    CHECK(refuses(p1));
    CHECK_EQ(r.server(0).value[0], static_cast<double>(p2));
    // The same configuration said again restarts nothing (the server stays the one running; no session of a browser is lost).
    CHECK(r.config(webConfig(p2)).status == FOXAPI_NO_CHANGE);
    CHECK(!refuses(p2));
    CHECK(r.config("{\"web\":{\"enabled\":false}}").status == FOXAPI_OK);
    CHECK(refuses(p2));
    CHECK(!active(r.server(0)));
    CHECK(!r.e.servers().webListening());
    CHECK((r.state().flags & FOXAPI_RX_WEB_LISTENING) == 0u);
    // CAT likewise.
    const int c1 = freePort();
    CHECK(r.config(catConfig(c1)).status == FOXAPI_OK);
    CHECK(!refuses(c1));
    CHECK(r.config("{\"cat\":{\"enabled\":false}}").status == FOXAPI_OK);
    CHECK(refuses(c1));
    std::printf("H5: a port change restarts (:%d -> :%d), the same configuration restarts nothing, a disable closes the socket; CAT likewise\n", p1, p2);
}

void testRefusalAndRetry() {
    Rig r("testHooks=1;servers=1");
    const int port = freePort();
    {
        PortHolder other(port);
        CHECK(other.held);
        const FoxCommandResult a = r.config(webConfig(port));
        CHECK(a.status == FOXAPI_OK);  // the configuration is stored; the START was refused, as a report, not as a result
        const FoxListItem w = r.server(0);
        CHECK(fault(w) && !active(w));
        CHECK(std::string(w.detail).find("could not bind") != std::string::npos);
        CHECK_EQ(w.value[0], static_cast<double>(port));  // not listening: the configured port
        CHECK(!r.e.servers().webListening());
        std::printf("H6: port %d held by someone else -> FAULT '%s'\n", port, w.detail);
        // Nothing hammers a refused port: the pass goes on, the host stays idle.
        r.pump(3);
        CHECK(r.e.servers().waitIdle(std::chrono::milliseconds(200)));
    }  // the other holder is gone
    // THE RETRY IS THE OPERATOR'S: the same configuration said again (NO_CHANGE) tries again.
    const FoxCommandResult again = r.config(webConfig(port));
    CHECK(again.status == FOXAPI_NO_CHANGE);
    const FoxListItem w2 = r.server(0);
    CHECK(active(w2) && !fault(w2));
    CHECK(!refuses(port));
    // An off-machine bind with no password: the policy refuses, no socket opens, and the sentence is published whole.
    const int open = freePort();
    CHECK(r.config(webConfig(open, "0.0.0.0")).status == FOXAPI_OK);
    const FoxListItem w3 = r.server(0);
    CHECK(fault(w3) && !active(w3));
    const std::string policy = r.fact("fact.web.policy");
    CHECK(policy.size() > 10u);
    CHECK(std::string(w3.detail).compare(0, std::min<std::size_t>(std::strlen(w3.detail), policy.size()), policy, 0,
                                         std::min<std::size_t>(std::strlen(w3.detail), policy.size())) == 0);
    CHECK(refuses(open));
    CHECK(!r.e.servers().webListening());
    std::printf("H6: freed and said again -> ACTIVE; 0.0.0.0 with no password -> FAULT, no socket ('%.60s...')\n", policy.c_str());
}

void testPasswordRestartsAndGatesTheLogin() {
    Rig r("testHooks=1;servers=1");
    const int port = freePort();
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    const auto open = cli.Get("/api/status");
    CHECK(open && open->status == 200);  // loopback with no password serves openly
    // The hash runs on a worker; its record restarts the server (the host holds the restart back until the record exists).
    const std::string secret = "correct horse battery";
    CHECK(r.config("{\"web\":{\"password\":\"" + secret + "\"}}", /*settle=*/false).status == FOXAPI_OK);
    CHECK(waitUntil(60000, [&] {
        r.pump(1);
        return r.fact("fact.web.hasPassword") == "1" && r.fact("fact.web.passwordPending") == "0";
    }));
    CHECK(r.e.servers().waitIdle(std::chrono::seconds(30)));
    r.pump(2);
    CHECK(active(r.server(0)));
    httplib::Client cli2("127.0.0.1", port);
    cli2.set_connection_timeout(2, 0);
    cli2.set_read_timeout(5, 0);
    const auto closed = cli2.Get("/api/status");
    CHECK(closed && closed->status == 401);
    const auto wrong = cli2.Post("/api/login", "{\"username\":\"admin\",\"password\":\"not the password\"}", "application/json");
    CHECK(wrong && wrong->status == 401);
    const auto right = cli2.Post("/api/login", "{\"username\":\"admin\",\"password\":\"" + secret + "\"}", "application/json");
    CHECK(right && right->status == 200);
    if (right) {
        const std::string cookie = right->get_header_value("Set-Cookie");
        CHECK(!cookie.empty());
        httplib::Headers withCookie;
        withCookie.emplace("Cookie", cookie.substr(0, cookie.find(';')));
        const auto signedIn = cli2.Get("/api/status", withCookie);
        CHECK(signedIn && signedIn->status == 200);
    }
    // THE RESTART WAITS FOR THE RECORD (H7b): a configuration change that lands while a password is being hashed is not acted on until the
    // record exists - the server comes up once, with the password just set, instead of once without it and once with. (The hash is ~0.3 s;
    // a check made after it had finished says so and is skipped, never passed.)
    {
        int p2 = freePort();
        while (p2 == port) { p2 = freePort(); }
        CHECK(r.config("{\"web\":{\"password\":\"another correct horse\"}}", /*settle=*/false).status == FOXAPI_OK);
        CHECK(r.config("{\"web\":{\"port\":" + std::to_string(p2) + "}}", /*settle=*/false).status == FOXAPI_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        if (r.fact("fact.web.passwordPending") == "1") {
            CHECK(!refuses(port));  // still the old binding, with the old record
            CHECK(refuses(p2));     // the new one is not opened ahead of the password
            std::printf("H7b: a port change made while the password was being hashed waited for the record\n");
        } else {
            std::printf("H7b: SKIPPED - the hash had already finished (nothing was held back to check)\n");
        }
        CHECK(waitUntil(60000, [&] {
            r.pump(1);
            return r.fact("fact.web.passwordPending") == "0";
        }));
        CHECK(r.e.servers().waitIdle(std::chrono::seconds(30)));
        CHECK(!refuses(p2));
        CHECK(refuses(port));
    }
    // The config the engine writes carries the record, and the plaintext is nowhere in it.
    cascade::core::AppConfig cfg;
    r.e.servers().store(cfg);
    CHECK(!cfg.webPasswordRecord.empty() && cfg.webPasswordRecord.find(secret) == std::string::npos);
    std::printf("H7: the password hashed on a worker, the server restarted with its record - open -> 401, wrong login 401, right login 200\n");
}

void testConfigRestoreStartsWhatTheFileSaid() {
    Rig r("testHooks=1;servers=1");
    cascade::core::AppConfig cfg;
    cfg.webEnabled = true;
    cfg.webBindAddress = "127.0.0.1";
    cfg.webPort = freePort();
    cfg.catEnabled = true;
    cfg.catPort = freePort();
    while (cfg.catPort == cfg.webPort) { cfg.catPort = freePort(); }
    const auto t0 = std::chrono::steady_clock::now();
    r.e.applyConfig(cfg);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.e.servers().waitIdle(std::chrono::seconds(30)));
    CHECK(r.e.servers().webServer().running() && r.e.servers().catServer().running());
    CHECK(!refuses(cfg.webPort) == true);
    r.pump(2);
    CHECK(active(r.server(0)) && active(r.server(1)));
    cascade::core::AppConfig back = r.e.configSnapshot();
    r.e.servers().store(back);
    CHECK(back.webEnabled && back.webPort == cfg.webPort && back.catEnabled && back.catPort == cfg.catPort);
    std::printf("H8: applyConfig (%.1f ms) started the web server on :%d and CAT on :%d from the file's fields\n", ms, cfg.webPort, cfg.catPort);
}

void testTeardownStopsFirst() {
    Rig r("testHooks=1;servers=1", /*thread=*/true);
    const int wp = freePort();
    int cp = freePort();
    while (cp == wp) { cp = freePort(); }
    CHECK(r.config(webConfig(wp)).status == FOXAPI_OK);
    CHECK(r.config(catConfig(cp)).status == FOXAPI_OK);
    CHECK(!refuses(wp) && !refuses(cp));
    r.e.shutdownQuiesce();
    // After the FIRST step of the teardown both listeners are gone (not at the engine's destruction): no thread of either remains.
    CHECK(!r.e.servers().webServer().running());
    CHECK(!r.e.servers().catServer().running());
    CHECK(refuses(wp));
    CHECK(refuses(cp));
    std::printf("H9: shutdownQuiesce stopped the web server (:%d) and CAT (:%d) - nothing listens\n", wp, cp);
}

// H10. A browser holds a request open, so the web server's stop must WAIT for its handler (up to the 10 s read timeout). The stop is
// the worker's: the free-running pass goes on at its rate meanwhile.
void testStopIsNeverInThePass() {
    Rig r("testHooks=1;servers=1", /*thread=*/true);
    const int port = freePort();
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    SockT held = connectTo(port, 200);
    CHECK(held != INVALID_SOCKET);
    if (held == INVALID_SOCKET) { return; }
    // A request line and a header, and no blank line: the handler blocks reading the rest.
    CHECK(sendRaw(held, "GET /api/status HTTP/1.1\r\nHost: x\r\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // The disabling SERVER_CONFIG is SUBMITTED here and the pass is watched from this instant (not after its answer: a pass that applied it
    // by stopping the server itself would not answer for as long as the stop waits, and a test that waited for the answer first would
    // see the passes resume afterwards and call that fine).
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = FOXAPI_OP_SERVER_CONFIG;
    const std::string json = "{\"web\":{\"enabled\":false}}";
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(api()->submit_text(r.op, &c, json.data(), static_cast<uint32_t>(json.size()), &sr) >= 0);
    const std::uint64_t p0 = Access::passes(r.e);
    std::uint64_t last = p0;
    auto lastAt = t0;
    double maxGapMs = 0.0;
    double answeredAfterMs = -1.0;
    const auto until = t0 + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until) {
        const std::uint64_t now = Access::passes(r.e);
        const auto t = std::chrono::steady_clock::now();
        if (now != last) {
            maxGapMs = std::max(maxGapMs, std::chrono::duration<double, std::milli>(t - lastAt).count());
            last = now;
            lastAt = t;
        }
        if (answeredAfterMs < 0.0) {
            FoxCommandResult res[4];
            for (auto& x : res) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(r.op, res, 4);
            for (int32_t i = 0; i < got; ++i) {
                if (res[i].ticket == sr.ticket && res[i].status == FOXAPI_OK) {
                    answeredAfterMs = std::chrono::duration<double, std::milli>(t - t0).count();
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    maxGapMs = std::max(maxGapMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastAt).count());
    const std::uint64_t passes = last - p0;
    // 2 s in, the stop is still waiting for the browser: the worker is inside it, the server has stopped accepting (running() is down at
    // the top of stop()), and the pass long since answered.
    const bool workerBusy = !r.e.servers().waitIdle(std::chrono::milliseconds(1));
    std::printf("H10: the worker inside WebServer::stop() for 2+ s (a browser holding a request): the command answered after %.0f ms, %llu passes in 2 s, longest gap between two %.1f ms\n",
                answeredAfterMs, static_cast<unsigned long long>(passes), maxGapMs);
    CHECK(workerBusy);
    CHECK(!r.e.servers().webServer().running());
    CHECK(answeredAfterMs >= 0.0 && answeredAfterMs < 500.0);  // answered by the next pass or two, not after the stop
    CHECK(passes >= 40);          // 16 ms period: ~125 expected
    CHECK(maxGapMs < 400.0);      // a pass stalled past 1 s unkeys the transmitter's thread; the stop alone is up to 10 s
    closeSock(held);              // the browser goes away; the stop completes
    CHECK(r.e.servers().waitIdle(std::chrono::seconds(20)));
    CHECK(!r.e.servers().webServer().running());
    CHECK(refuses(port));
}


// ---- the page's streams (stage 5e) -------------------------------------------------------------------------------------------------

// One browser listening to /api/audio on its own thread, counting bytes until told to stop. `status` is the response's (0 until it arrives).
struct AudioListener {
    std::atomic<std::size_t> bytes{0};
    std::atomic<int> status{0};
    std::atomic<bool> stop{false};
    std::string contentType;
    std::thread t;
    void start(int port) {
        t = std::thread([this, port] {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(5, 0);
            cli.set_read_timeout(30, 0);
            (void)cli.Get(
                "/api/audio", httplib::Headers{},
                [this](const httplib::Response& r) {
                    contentType = r.get_header_value("Content-Type");
                    status = r.status;
                    return true;
                },
                [this](const char*, std::size_t len) {
                    bytes += len;
                    return !stop.load();
                });
            if (status.load() == 0) { status = -1; }
        });
    }
    void finish() {
        stop = true;
        if (t.joinable()) { t.join(); }
    }
    ~AudioListener() { finish(); }
};

// H12. A free-running engine with its web server and NO window: the page's spectrum long-poll is answered from the table.
void testSpectrumWithNoWindow() {
    Rig r("testHooks=1;servers=1", /*thread=*/true);
    const int port = freePort();
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    std::uint64_t cursor = 0;
    int fresh = 0;
    bool monotonic = true;
    std::size_t binChars = 0;
    double centre = 0.0, span = 0.0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (fresh < 10 && std::chrono::steady_clock::now() < until) {
        const auto res = cli.Get(("/api/spectrum?since=" + std::to_string(cursor)).c_str());
        if (res && res->status == 200) {
            const nlohmann::json j = nlohmann::json::parse(res->body, nullptr, false);
            if (!j.is_discarded() && j.value("fresh", false)) {
                const std::uint64_t seq = j.value("seq", std::uint64_t{0});
                if (seq <= cursor) { monotonic = false; }
                cursor = seq;
                ++fresh;
                centre = j.value("centerHz", 0.0);
                span = j.value("spanHz", 0.0);
                if (j.contains("bins") && j["bins"].is_string()) { binChars = j["bins"].get<std::string>().size(); }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(fresh >= 10);
    CHECK(monotonic);
    CHECK(binChars > 0);
    // The label is the frame's own: the source's centre and the rate it samples at (the generator's, here).
    const FoxReceiverState st = r.state();
    CHECK_EQ(centre, st.centreHz);
    CHECK_EQ(span, st.sampleRateHz);
    std::printf("H12: no window - /api/spectrum gave %d frames, each newer than its cursor (seq %llu, %zu base64 chars of bins, centre %.0f Hz, span %.0f Hz)\n",
                fresh, static_cast<unsigned long long>(cursor), binChars, centre, span);
}

// H13. The page's audio over the real chain.
void testAudioThroughTheWholeChain() {
    Rig r("testHooks=1;servers=1", /*thread=*/true);
    const int port = freePort();
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    AudioListener a;
    a.start(port);
    CHECK(waitUntil(15000, [&] { return a.bytes.load() >= 48000; }));  // half a second of 16-bit mono at 48 kHz
    CHECK_EQ(a.status.load(), 200);
    CHECK_EQ(a.contentType, std::string("audio/L16;rate=48000;channels=1"));
    // The panel's count, read off the table's list, and the cap: four listeners, a fifth is refused 503 and takes no slot.
    CHECK(waitUntil(5000, [&] { return r.server(0).value[2] == 1.0; }));
    AudioListener more[3];
    for (AudioListener& m : more) { m.start(port); }
    CHECK(waitUntil(15000, [&] { return r.e.servers().webServer().audioListeners() == 4; }));
    {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        const auto fifth = cli.Get("/api/audio");
        CHECK(fifth && fifth->status == 503);
    }
    CHECK_EQ(r.e.servers().webServer().audioListeners(), std::size_t{4});
    // Every listener is live (their cursors are their own): each has bytes.
    for (AudioListener& m : more) { CHECK(waitUntil(15000, [&] { return m.bytes.load() >= 9600; })); }
    const std::size_t total = a.bytes.load();
    a.finish();
    for (AudioListener& m : more) { m.finish(); }
    CHECK(waitUntil(10000, [&] { return r.e.servers().webServer().audioListeners() == 0; }));
    std::printf("H13: /api/audio = audio/L16;rate=48000;channels=1, %zu bytes to the first of four listeners, a fifth 503, every slot given back\n", total);
}

// H15. Headless: no basemap.
void testNoWindowNoTiles() {
    Rig r("testHooks=1;servers=1", /*thread=*/true);
    const int port = freePort();
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    const auto st = cli.Get("/api/status");
    CHECK(st && st->status == 200);
    if (st) {
        const nlohmann::json j = nlohmann::json::parse(st->body, nullptr, false);
        CHECK(!j.is_discarded() && j.contains("basemap") && !j["basemap"].value("active", true));
    }
    const auto tile = cli.Get("/api/tile/3/1/2");
    CHECK(tile && tile->status == 202);
    if (tile) { CHECK(tile->body.find("\"pending\":true") != std::string::npos); }
    std::printf("H15: headless - the status says the basemap is not active and a tile asked for anyway is 202 pending\n");
}

// H14. The control pass with a listener attached, and with the audio mutex held. The pass is watched for the longest gap between two passes (a
// stalled pass is what unkeys the transmitter's thread at 1 s) and by the engine's own pass histogram.
struct PassFigures {
    double maxGapMs = 0.0;
    std::uint64_t passes = 0;
    std::string stats;
};
PassFigures watchThePass(Rig& r, int ms, const std::function<void()>& during = {}) {
    Access::markPassStats(r.e);
    PassFigures f;
    const std::uint64_t p0 = Access::passes(r.e);
    std::uint64_t last = p0;
    const auto t0 = std::chrono::steady_clock::now();
    auto lastAt = t0;
    const auto until = t0 + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (during) { during(); }
        const std::uint64_t now = Access::passes(r.e);
        const auto t = std::chrono::steady_clock::now();
        if (now != last) {
            f.maxGapMs = std::max(f.maxGapMs, std::chrono::duration<double, std::milli>(t - lastAt).count());
            last = now;
            lastAt = t;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    f.maxGapMs = std::max(f.maxGapMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lastAt).count());
    f.passes = last - p0;
    f.stats = Access::passStats(r.e);
    return f;
}

void testThePassWithAListenerAndTheAudioLockHeld() {
    Rig r("testHooks=1;servers=1", /*thread=*/true);
    const int port = freePort();
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    // Settle: the pipeline's first blocks and the first publishes are not the steady state.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const PassFigures none = watchThePass(r, 3000);
    AudioListener a;
    a.start(port);
    CHECK(waitUntil(15000, [&] { return a.bytes.load() >= 9600; }));
    const std::size_t b0 = a.bytes.load();
    const PassFigures with = watchThePass(r, 3000);
    const std::size_t streamed = a.bytes.load() - b0;
    CHECK(streamed >= 96000);  // the listener was served about two seconds of audio of the three: the stream runs while the pass is watched

    // THE HOLD: the pipeline's audio mutex taken by a thread of the test for 1.5 s, the listener attached throughout. The DSP thread cannot
    // finish its block; the audio feed (every read_audio, the pass's own top-up) must not wait for it, and neither must the pass.
    std::atomic<bool> acquired{false};
    std::thread holder([&] { Access::holdAudioLock(r.e, 1500, &acquired); });
    CHECK(waitUntil(5000, [&] { return acquired.load(); }));
    const std::size_t h0 = a.bytes.load();
    const PassFigures held = watchThePass(r, 1300);   // inside the hold
    const std::size_t heldBytes = a.bytes.load() - h0;
    holder.join();
    // After the hold the DSP thread catches up and the stream goes on.
    const std::size_t after0 = a.bytes.load();
    CHECK(waitUntil(10000, [&] { return a.bytes.load() >= after0 + 9600; }));
    a.finish();

    std::printf("H14: pass, no listener   : %llu passes in 3 s, longest gap %.1f ms; engine %s\n", static_cast<unsigned long long>(none.passes),
                none.maxGapMs, none.stats.c_str());
    std::printf("H14: pass, 1 listener    : %llu passes in 3 s, longest gap %.1f ms, %zu bytes streamed; engine %s\n",
                static_cast<unsigned long long>(with.passes), with.maxGapMs, streamed, with.stats.c_str());
    std::printf("H14: pass, audio lock HELD (1.5 s, listener attached): %llu passes in 1.3 s, longest gap %.1f ms, %zu bytes streamed meanwhile; engine %s\n",
                static_cast<unsigned long long>(held.passes), held.maxGapMs, heldBytes, held.stats.c_str());
    CHECK(none.passes >= 30);
    CHECK(with.passes >= 30);
    CHECK(held.passes >= 20);     // 16 ms period: ~80 expected in 1.3 s
    CHECK(none.maxGapMs < 400.0);
    CHECK(with.maxGapMs < 400.0);
    CHECK(held.maxGapMs < 400.0); // a pass that queued behind the DSP block would show the hold's 1 500 ms here
}

}  // namespace

// H11: THE WEB SESSION, THROUGH THE WHOLE REAL CHAIN (stage 5d): a real httplib client, the real web server the engine hosts, the strict
// parse, the hand-over on the HTTP thread, the engine's ONE web session, the pass - and the effect READ BACK through a table session. What the
// route does is test_web_server.cpp's (against a stub session); what the engine's key rules do is test_remote_control_pull.cpp's; this holds
// that the two are joined: a field of a ControlRequest, an application op the page's sliders send, a bookmark named by its ROW, and the
// session's life.
void testTheWebSessionThroughTheRealChain() {
    Rig r("testHooks=1;servers=1");
    const int port = freePort();
    const auto webSessions = [&] { return r.e.api().sessionCountForTest(cascade::engine::InternalKind::Web); };
    CHECK_EQ(webSessions(), std::size_t{0});  // no server, no session
    CHECK(r.config(webConfig(port)).status == FOXAPI_OK);
    CHECK(r.e.servers().webServer().running());
    CHECK_EQ(webSessions(), std::size_t{1});  // ONE session for the running server

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(5, 0);
    const auto post = [&](const char* body) {
        const auto res = cli.Post("/api/control", body, "application/json");
        r.pump(3);
        return res ? res->status : 0;
    };

    // ---- the receiver's fields, through the session's profile -------------------------------------------------------------------
    CHECK_EQ(post("{\"centerHz\":100000000,\"mode\":\"NFM\",\"bandwidthHz\":12500,\"squelchDb\":-60,\"volume\":0.4}"), 202);
    FoxReceiverState st = r.state();
    CHECK_EQ(st.centreHz, 100.0e6);
    CHECK_EQ(st.demodMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_NFM));
    CHECK_EQ(st.bandwidthHz, 12500.0);
    CHECK_EQ(st.squelchDb, -60.0);
    CHECK(st.volume > 0.39 && st.volume < 0.41);
    CHECK_EQ(post("{\"vfoOffsetHz\":25000}"), 202);
    CHECK_EQ(r.state().vfoOffsetHz, 25000.0);

    // ---- the application's own ops, which no other REMOTE session may send and the web profile holds (D2): the display's ends and the
    //      noise-reduction strength. Refused to any session the table opens for a client; accepted here. ---------------------------------
    CHECK_EQ(post("{\"dbMin\":-95}"), 202);
    CHECK_EQ(r.state().dbMin, -95.0);
    CHECK_EQ(post("{\"dbMax\":-5}"), 202);
    CHECK_EQ(r.state().dbMax, -5.0);
    CHECK_EQ(post("{\"nrStrength\":0.6}"), 202);
    CHECK(r.state().nrStrength > 0.59 && r.state().nrStrength < 0.61);

    // ---- a bookmark named by its ROW, resolved when the request is POSTED from the block /api/status serves --------------------------
    CHECK_EQ(post("{\"centerHz\":101000000,\"bookmarkAdd\":\"H11 first\"}"), 202);
    CHECK_EQ(post("{\"centerHz\":102000000,\"bookmarkAdd\":\"H11 second\"}"), 202);
    r.pump(3);
    const auto rowOf = [&](const char* name) {
        const auto s = cli.Get("/api/status");
        if (!s || s->status != 200) { return -1; }
        const nlohmann::json j = nlohmann::json::parse(s->body, nullptr, false);
        if (j.is_discarded() || !j.contains("bookmarks")) { return -1; }
        int row = 0;
        for (const auto& b : j["bookmarks"]) {
            if (b.value("name", std::string()) == name) { return row; }
            ++row;
        }
        return -1;
    };
    const int rowFirst = rowOf("H11 first");
    const int rowSecond = rowOf("H11 second");
    CHECK(rowFirst >= 0 && rowSecond >= 0 && rowFirst != rowSecond);
    // tune to the first: the centre goes back to 101 MHz (the bookmark's frequency; the row named the ID, not a position)
    CHECK_EQ(post(("{\"bookmarkTune\":" + std::to_string(rowFirst) + "}").c_str()), 202);
    CHECK(std::fabs(r.state().centreHz + r.state().vfoOffsetHz - 101.0e6) < 1.0e5);
    // remove the second by its row: it goes, and the first stays
    CHECK_EQ(post(("{\"bookmarkRemove\":" + std::to_string(rowSecond) + "}").c_str()), 202);
    r.pump(3);
    CHECK_EQ(rowOf("H11 second"), -1);
    CHECK(rowOf("H11 first") >= 0);

    // ---- a PTT with no transmitter is the gate's 409 and nothing reaches the engine ---------------------------------------------------
    CHECK_EQ(post("{\"transmitPtt\":true}"), 409);

    // ---- the session's life: 17 browsers' cookies are not 17 engine sessions; the stop closes it; a restart opens a new one --------------
    for (int i = 0; i < 17; ++i) {
        httplib::Client other("127.0.0.1", port);
        other.set_connection_timeout(2, 0);
        const auto s = other.Get("/api/status");
        CHECK(s && s->status == 200);
    }
    CHECK_EQ(webSessions(), std::size_t{1});
    CHECK(r.config("{\"web\":{\"enabled\":false}}").status == FOXAPI_OK);
    CHECK(!r.e.servers().webServer().running());
    CHECK_EQ(webSessions(), std::size_t{0});
    const int port2 = freePort();
    CHECK(r.config(webConfig(port2)).status == FOXAPI_OK);
    CHECK_EQ(webSessions(), std::size_t{1});
    std::printf("H11: a browser's fields, the page's application ops (display ends, NR strength), bookmark rows, the PTT gate - through the real "
                "server, the one web session and the pass; 17 cookies = 1 engine session; stop = 0; restart = 1\n");
}

int main(int argc, char** argv) {
    // (An argument of the form H<n> runs that case alone - the break-it checks use it; the CTest entry passes none.)
    std::string only;
    if (argc > 1 && argv[1][0] == 'H' && std::isdigit(static_cast<unsigned char>(argv[1][1])) != 0) { only = argv[1]; }
    const auto want = [&](const char* id) { return only.empty() || only == id; };
    engine_api_test::hermetic("serverhost");
    std::printf("test_server_host\n");
    if (want("H1")) { testHermeticEngineHostsNothing(); }
    if (want("H2")) { testWebServerStartedBySessionConfig(); }
    if (want("H4")) { testCatServer(); }
    if (want("H5")) { testChangeRestartsStopStops(); }
    if (want("H6")) { testRefusalAndRetry(); }
    if (want("H7")) { testPasswordRestartsAndGatesTheLogin(); }
    if (want("H8")) { testConfigRestoreStartsWhatTheFileSaid(); }
    if (want("H9")) { testTeardownStopsFirst(); }
    if (want("H10")) { testStopIsNeverInThePass(); }
    if (want("H11")) { testTheWebSessionThroughTheRealChain(); }
    if (want("H12")) { testSpectrumWithNoWindow(); }
    if (want("H13")) { testAudioThroughTheWholeChain(); }
    if (want("H14")) { testThePassWithAListenerAndTheAudioLockHeld(); }
    if (want("H15")) { testNoWindowNoTiles(); }
    return testSummary("test_server_host");
}
