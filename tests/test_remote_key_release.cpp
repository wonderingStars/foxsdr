// test_remote_key_release.cpp - the standing conditions on the WEB REMOTE's
// transmit key (engine/stage3b-pre remote-key coverage, 2026-09-28; the
// consent rule since engine stage 4, API 0.2's THE KEY; the web server's
// engine SESSION since stage 5d, docs/engine-stage4.md 6.12).
//
// THE RULE: a remote key exists only while (a) a LOCAL session - the
// window's, through the Transmit page's "Allow remote transmit" - has
// consented (TX_REMOTE_ARM 1), so the browser's PTT itself is refused
// without it, and withdrawing it releases a key held; and (b) the web server
// that carries the browser's requests is actually running - a server that
// never started, or that is switched off in the settings panel, holds no key,
// and one that stops while the browser holds it lets the key go, because the
// 2 s hold's own expiry would otherwise leave the transmitter keyed with
// nobody able to ask again. (Until stage 5d the engine judged (b) by pulling
// the server's queue and asking "is it running" in its remote-controls phase,
// and FOXAPP_OP_WEB_CONTROL_STOPPED released the key; now the web server holds
// ONE ENGINE SESSION while it runs, the browser's PTT is that session's, and
// the server stopping is that session CLOSING - the key rules open the key a
// closed session held at the start of the next pass.)
//
// THE BROWSER HERE IS A REAL HTTP CLIENT against the engine's own web server
// (a WebServer on a free loopback port): its PTT is a POST /api/control, its
// keep-alive is a GET /api/status, and the whole chain - route, strict parse,
// the 409 gate, the hand-over, the session, the pass - is the product's.
//
// test_transmit_dead_man.cpp holds (a)'s timing end to end on a bare Engine
// (withdrawn, closed, silent); THIS FILE holds it through the window - the
// window's own page-close path (gui::txKeyCommands withdraws the consent) -
// and (b), each checked in isolation, so a mutant deleting either release is
// caught by THAT check specifically. F holds the one thing only a real server
// can: a control POST is not a heartbeat.
//
// A real FakeTxIiod (the same fake TX board test_apply_command.cpp opens
// FOXAPI_OP_TX_OPEN against) and a real WebServer bound to a free loopback
// port (test_web_server.cpp's port-scan pattern, shifted to its own range so
// the two suites never fight over the same ports run in parallel).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// (before windows.h: httplib brings winsock2 in, and winsock.h from windows.h would clash with it)
#include <httplib.h>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "iiod_fake_server.hpp"
#include "net/web_policy.hpp"
#include "net/web_server.hpp"
#include "test_check.hpp"
#include "tx_front_end.hpp"

namespace {

using cascade::net::WebServer;
using cascade::net::WebServerConfig;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() /
                ("foxsdr_remote_key_release_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// A port the OS says nobody has: bound on loopback with port 0, read back, closed. THE BROWSER HERE IS A REAL HTTP CLIENT (stage 5d), so a
// fixed range shared by this suite's three CTest variants (the default, _free and _stepped, run in parallel) would let one variant talk to
// the other's server - and on Windows a second bind to a port someone holds succeeds, so the collision would not even announce itself. The
// OS hands each process its own ephemeral ports.
int osFreePort() {
#if defined(_WIN32)
    static bool started = false;
    if (!started) {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
        started = true;
    }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const bool bad = s == INVALID_SOCKET;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    const bool bad = s < 0;
#endif
    if (bad) { return -1; }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = -1;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
#if defined(_WIN32)
        int len = sizeof(a);
#else
        socklen_t len = sizeof(a);
#endif
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
    return port;
}

int startOnFreePort(WebServer& server, WebServerConfig cfg, std::string& error) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        const int port = osFreePort();
        if (port < 1024) { continue; }
        cfg.port = port;
        if (server.start(cfg, error)) { return port; }
        // A policy refusal will fail identically on every port, so stop rather than grinding through fifty of them.
        if (!server.decision().allowed()) { return -1; }
    }
    return -1;
}

WebServerConfig loopbackConfig() {
    WebServerConfig cfg;
    cfg.enabled = true;
    cfg.bindAddress = "127.0.0.1";
    cfg.username = "admin";
    return cfg;
}

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for its tests.
struct AppWindowTestAccess {
    // A command applied as the engine applies one: on the pass's thread, with the pass out of the way when it free-runs
    // (passseam::read - the key rules are the pass's alone, and an apply from this thread would run beside it).
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c, const std::string& lt = {}) {
        return passseam::read<FoxCommandResult>(th::engine(a), [&] { return th::engine(a).applyCommand(c, lt); });
    }
    static FoxCommandResult apply(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) {
        return passseam::read<FoxCommandResult>(th::engine(a), [&] { return th::engine(a).applyCommand(q.c, q.longText); });
    }
    // The engine itself, for what is any thread's: the API handle a desk session opens on.
    static cascade::engine::Engine& engine(AppWindow& a) { return th::engine(a); }
    // What the publish carries of the key: the key rules' own fields, read on the pass's thread (passseam::read).
    static cascade::engine::ApiHost::TxFacts txFacts(AppWindow& a) {
        return passseam::read<cascade::engine::ApiHost::TxFacts>(th::engine(a), [&] { return th::engine(a).api().txFacts(); });
    }
    // The engine's pass as far as the key needs it - the queues applied (the local and the API's, then the remote controls' phase: the web
    // server's session, drained in the internal tier), the state published - without the audio half, which reads the window's ImGui clock
    // (no ImGui context in this test).
    static void pass(AppWindow& a) {
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            th::engine(a).pumpRemoteControls();
            (void)th::engine(a).pumpPublish();
        });
    }
    // Any thread: Transmitter::remoteHoldRemainingMs reads only atomics, haveSink takes the transmitter's own mutex, and
    // transmitter_ is a member the engine never replaces.
    static std::int64_t remoteHoldMs(AppWindow& a) {
        return th::engine(a).transmitter_.remoteHoldRemainingMs();
    }
    static bool haveTx(AppWindow& a) { return th::engine(a).transmitter_.haveSink(); }
    // The settings panel switching the web server off: the staged edit and its Apply, which SUBMITS a SERVER_CONFIG on the window's session
    // (stage 5b: the server is the engine's; a pass applies the command and the host's worker stops the server - nothing happens inside the
    // click itself).
    static void disableWeb(AppWindow& a) {
        a.webStageEnabled_ = false;
        a.applyWebSettings();
    }
    // The engine's own web server (ServerHost owns it since stage 5b).
    static WebServer& webServer(AppWindow& a) { return th::engine(a).servers().webServer(); }
    // The host's worker has made the servers match the configuration (a stop or a start has returned).
    static bool serversSettled(AppWindow& a) { return th::engine(a).servers().waitIdle(std::chrono::seconds(15)); }

    // THE PASS HELD, free-running only (the idiom of test_tuning_surface.cpp): a piece of work posted to the pass
    // (Engine::postToPass) that waits until the hold is let go. A pass runs posted work at the head of its front part, after it
    // has taken its queues, so while it is held no pass runs: what the window does meanwhile (the settings panel's click) is
    // judged by a pass that starts after the release - which is what E's "not inside the click" is about. Inline and stepped no
    // pass runs unless the test asks for one, so there is nothing to hold. Nothing may ask for a pass (apply, pass,
    // txFacts) while it is held.
    struct Hold {
        std::mutex m;
        std::condition_variable cv;
        bool running = false;
        bool release = false;
    };
    static std::shared_ptr<Hold>& held() {
        static std::shared_ptr<Hold> h;
        return h;
    }
    static void holdPass(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free || held() != nullptr) { return; }
        passseam::attach(th::engine(a));
        auto h = std::make_shared<Hold>();
        th::engine(a).postToPass([h] {
            std::unique_lock<std::mutex> lk(h->m);
            h->running = true;
            h->cv.notify_all();
            h->cv.wait(lk, [&] { return h->release; });
        });
        std::unique_lock<std::mutex> lk(h->m);
        const bool took = h->cv.wait_for(lk, std::chrono::seconds(10), [&] { return h->running; });
        CHECK(took);
        held() = h;
    }
    static void releasePass(AppWindow&) {
        const std::shared_ptr<Hold> h = held();
        if (h == nullptr) { return; }
        held().reset();
        {
            std::lock_guard<std::mutex> lk(h->m);
            h->release = true;
        }
        h->cv.notify_all();
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

namespace cmd = cascade::core::cmd;

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

// THE BROWSER: a real HTTP client on the web server's loopback port. Its status poll is the page's keep-alive (the web session's heartbeat),
// its PTT is a POST /api/control. A fresh one for each time the server starts (a stopped server drops its connections).
struct Browser {
    explicit Browser(int port) : cli("127.0.0.1", port) {
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
    }
    // The HTTP status the answer carried, or 0 when nothing answered (no server listening).
    int poll() {
        const auto r = cli.Get("/api/status");
        return r ? r->status : 0;
    }
    int ptt(bool on) {
        const auto r = cli.Post("/api/control", on ? "{\"transmitPtt\":true}" : "{\"transmitPtt\":false}", "application/json");
        return r ? r->status : 0;
    }
    httplib::Client cli;
};

std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

int main() {
    std::printf("test_remote_key_release\n");
    isolate();

    th::TestWindow app;
    cascade::engine::Engine& engine = Access::engine(app);

    // --- open a transmitter, so there is a real sink to key ----------------
    cascade::test::FakeTxIiod board;
    std::string err;
    CHECK(board.start(err));
    cascade::test::stockTxBoard(board);
    const std::string args = "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(board.port()));
    CHECK(ok(Access::apply(app, cmd::makeText(FOXAPI_OP_TX_OPEN, args))));
    CHECK(Access::haveTx(app));

    // The desk: a LOCAL session doing what the window's Transmit page does.
    tx_test::FrontEnd desk(engine, "the desk");
    desk.frame(true, false, false);
    Access::pass(app);  // a transmitter is now PUBLISHED open
    auto consent = [&](int on) {
        desk.frame(true, false, false, true, on);
        Access::pass(app);
    };
    // The browser polls its status (the web session's heartbeat) and then presses: a POST /api/control, then the engine's pass that applies
    // it. Answers the HTTP status of the press (202 taken, 409 the gate's refusal, 0 nobody answered).
    auto webKey = [&](Browser& b) {
        (void)b.poll();
        const int st = b.ptt(true);
        Access::pass(app);
        return st;
    };
    auto webSessions = [&] { return engine.api().sessionCountForTest(cascade::engine::InternalKind::Web); };

    WebServer& webServer = Access::webServer(app);

    // === B (first half): THE WEB SERVER NEVER STARTED holds no key - there is no session to hand a press to, and no listener to ask. =====
    // Run FIRST, before the server is ever started: the case the condition names, "a server that never started".
    {
        CHECK(!webServer.running());
        CHECK(webSessions() == 0u);
        consent(1);
        Browser nobody(osFreePort());  // (a port nothing is listening on)
        CHECK(nobody.ptt(true) == 0);
        Access::pass(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        CHECK(Access::txFacts(app).owner == 0u);
        consent(0);
        std::printf("  B: server never started -> no session (%zu), nothing to ask, remote hold %lld ms\n", webSessions(),
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // --- NOW start the server, for 0, A, D and the control below: all need
    //     running()==true throughout, with only the consent moving. --------
    int port = startOnFreePort(webServer, loopbackConfig(), err);
    if (port < 0) {
        std::printf("SKIP: could not bind a loopback port for the web server (%s)\n", err.c_str());
        return testSummary("test_remote_key_release");
    }
    std::printf("  app's web server bound to 127.0.0.1:%d\n", port);
    CHECK(webServer.running());
    CHECK(webSessions() == 1u);  // ONE session for the running server
    auto page = std::make_unique<Browser>(port);

    // === 0: NO CONSENT, NO KEY - the browser's PTT is refused, never
    //     remembered: consenting afterwards keys nothing by itself. ==========
    {
        (void)page->poll();
        const int st = page->ptt(true);
        Access::pass(app);
        CHECK(st == 409);  // the gate: the status says no remote transmit is allowed at the desk (test_web_server pins the sentence)
        CHECK(Access::remoteHoldMs(app) == 0);
        consent(1);
        CHECK(Access::remoteHoldMs(app) == 0);
        consent(0);
        std::printf("  0: no consent -> HTTP %d, remote hold %lld ms\n", st, static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === B (second half): THE SERVER STOPPING releases a key already held - its session closes, and the key rules open the key at the
    //     next pass. The stop is the WebServer's own (what the host's worker calls), not a command to the engine. =======================
    {
        consent(1);
        CHECK(webKey(*page) == 202);
        CHECK(Access::remoteHoldMs(app) > 0);
        CHECK(Access::txFacts(app).owner != 0u);
        webServer.stop();
        CHECK(!webServer.running());
        CHECK(webSessions() == 0u);  // the session closed with the server
        Access::pass(app);           // the NEXT pass: sessionGone opens the key the closed session held
        CHECK(Access::remoteHoldMs(app) == 0);
        // The rules let go IN THAT PASS, not only the radio.
        CHECK((Access::txFacts(app).owner) == 0u);
        std::printf("  B: server stopped while the browser held the key -> remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
        consent(0);
        // ...and the server starting again (a new session) lets the browser key as before: the stop was not sticky.
        port = startOnFreePort(webServer, loopbackConfig(), err);
        CHECK(port > 0);
        CHECK(webSessions() == 1u);
        page = std::make_unique<Browser>(port);
    }

    // === A: THE CONSENT WITHDRAWN releases a key already held, in the pass
    //     that applies it. ===================================================
    {
        consent(1);
        CHECK(webKey(*page) == 202);
        CHECK(Access::remoteHoldMs(app) > 0);
        consent(0);
        CHECK(Access::remoteHoldMs(app) == 0);
        std::printf("  A: consent withdrawn -> remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === control: consent standing AND the server running - a fresh key
    //     survives the remote-controls phase and a pass untouched (a mutant that always
    //     releases would pass A, B, D and E but fail here). ==================
    {
        CHECK(webServer.running());
        consent(1);
        CHECK(webKey(*page) == 202);
        CHECK(Access::remoteHoldMs(app) > 0);
        desk.frame(true, false, false);
        Access::pass(app);
        CHECK(Access::remoteHoldMs(app) > 0);  // neither condition applies - still held
        std::printf("  control: consent + server running -> remote hold %lld ms (still held)\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === D: THE PAGE CLOSING withdraws the consent (the window's own path,
    //     gui::txKeyCommands) - and the key held goes in that pass. ==========
    {
        CHECK(Access::remoteHoldMs(app) > 0);
        desk.frame(false, false, false);  // the page closes
        Access::pass(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        CHECK((Access::txFacts(app).remoteArmed) == false);
        std::printf("  D: page closed -> remote hold %lld ms\n", static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === F: A CONTROL POST IS NOT A HEARTBEAT (API 0.2: only the interface's own heartbeat counts). The browser's last status poll is
    //     the web session's last heartbeat; from then on it keeps saying PTT every 300 ms - each one buys a fresh 2 s hold - and never
    //     polls again, while the local operator is present and consenting. The key goes at the remote keep-alive counted from the
    //     LAST POLL, hold renewed or not. (Red if the server's /api/control route beats the session: the key would be held for as long as
    //     the browser kept talking.) ============================================================================================================
    {
        desk.frame(true, false, false);
        consent(1);
        const std::int64_t t0 = steadyMs();  // before the poll: its stamp is taken inside
        (void)page->poll();
        CHECK(page->ptt(true) == 202);
        Access::pass(app);
        CHECK(Access::remoteHoldMs(app) > 0);
        std::int64_t releasedAt = -1;
        std::int64_t lastPost = steadyMs();
        while (steadyMs() - t0 < 4500) {
            desk.beat();  // the desk is present
            if (steadyMs() - lastPost >= 300) {
                (void)page->ptt(true);  // ...the browser re-asserts, and does not poll
                lastPost = steadyMs();
            }
            Access::pass(app);
            if (Access::remoteHoldMs(app) == 0 && Access::txFacts(app).owner == 0u) {
                releasedAt = steadyMs() - t0;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::printf("  F: browser re-asserting PTT every 300 ms, not polling: released %lld ms after its last poll\n",
                    static_cast<long long>(releasedAt));
        CHECK(releasedAt >= 0);
        CHECK(releasedAt >= FOXAPI_REMOTE_KEEPALIVE_MS - 50);
        CHECK(releasedAt <= FOXAPI_REMOTE_KEEPALIVE_MS + 400);
        consent(0);
    }

    // === E: switching the web server OFF in the settings panel releases a
    //     key held, in the engine's NEXT PASS after the server has stopped -
    //     not inside the click. The click only SUBMITS the configuration; the
    //     host's worker (never the pass) stops the server, which closes its
    //     session; the next pass opens the key that session held. ==================
    {
        desk.frame(true, false, false);
        consent(1);
        CHECK(webKey(*page) == 202);
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::holdPass(app);  // (free-running: no pass between the click and the read below)
        Access::disableWeb(app);
        // The window did NOT reach into the engine: the click only SUBMITTED the configuration, so no pass has applied it - the server
        // still runs and the key is still held until the pass runs.
        CHECK(webServer.running());
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::releasePass(app);
        // The pass applies the SERVER_CONFIG; the host's WORKER (never the pass) stops the server, and with it the web session; the next
        // pass opens the key that session held.
        Access::pass(app);
        CHECK(Access::serversSettled(app));
        CHECK(!webServer.running());
        CHECK(webSessions() == 0u);
        Access::pass(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        CHECK((Access::txFacts(app).owner) == 0u);
        std::printf("  E: web server switched off -> the key held until the engine's pass, then remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    page.reset();
    desk.close();
    const int rc = testSummary("test_remote_key_release");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(g_scratch, ec);
    }
    return rc;
}
