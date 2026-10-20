// test_remote_key_release.cpp - the standing conditions on the WEB REMOTE's
// transmit key (engine/stage3b-pre remote-key coverage, 2026-09-28; the
// consent rule since engine stage 4, API 0.2's THE KEY).
//
// THE RULE: a remote key exists only while (a) a LOCAL session - the
// window's, through the Transmit page's "Allow remote transmit" - has
// consented (TX_REMOTE_ARM 1), so the browser's PTT itself is refused
// without it, and withdrawing it releases a key held; and (b) the web server
// that carries the browser's requests is actually running - a server that
// never started, or that is switched off in the settings panel, releases a
// key already held in the same step (FOXAPP_OP_WEB_CONTROL_STOPPED), because
// the 2 s hold's own expiry would otherwise leave the transmitter keyed with
// nobody able to ask again. (Since the pump move's step P1 the ENGINE judges
// both, in its remote-controls phase, and the "same step" is "the same
// phase": the window no longer applies the command itself.)
//
// test_transmit_dead_man.cpp holds (a)'s timing end to end on a bare Engine
// (withdrawn, closed, silent); THIS FILE holds it through the window - the
// window's own page-close path (gui::txKeyCommands withdraws the consent) -
// and (b), each checked in isolation, so a mutant deleting either release is
// caught by THAT check specifically.
//
// A real FakeTxIiod (the same fake TX board test_apply_command.cpp opens
// FOXAPI_OP_TX_OPEN against) and a real WebServer bound to a free loopback
// port (test_web_server.cpp's port-scan pattern, shifted to its own range so
// the two suites never fight over the same ports run in parallel).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "gui/app_window.hpp"
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

// A DIFFERENT range from test_web_server.cpp's 18073-18172, so two suites
// bound for parallel worktrees never contend for the same free port.
int startOnFreePort(WebServer& server, WebServerConfig cfg, std::string& error) {
    for (int port = 18273; port < 18373; ++port) {
        cfg.port = port;
        if (server.start(cfg, error)) { return port; }
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
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c, const std::string& lt = {}) {
        return a.engine_.applyCommand(c, lt);
    }
    static FoxCommandResult apply(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) {
        return a.engine_.applyCommand(q.c, q.longText);
    }
    static cascade::engine::Engine& engine(AppWindow& a) { return a.engine_; }
    // The engine's pass as far as the key needs it - the queue applied, the
    // state published - without the audio half, which reads the window's
    // ImGui clock (no ImGui context in this test).
    static void pass(AppWindow& a) {
        a.engine_.pumpFrameBegin();
        (void)a.engine_.pumpPublish();
    }
    static std::int64_t remoteHoldMs(AppWindow& a) {
        return a.engine_.transmitter_.remoteHoldRemainingMs();
    }
    static bool haveTx(AppWindow& a) { return a.engine_.transmitter_.haveSink(); }
    // The engine's remote-controls phase: the web and CAT queues pulled through the source the window installed, and the
    // remote key's standing condition judged (the pump move's step P1 - this was the window's applyWebControls).
    static void pumpRemote(AppWindow& a) { a.engine_.pumpRemoteControls(); }
    // The settings panel switching the web server off.
    static void disableWeb(AppWindow& a) {
        a.webCfg_.enabled = false;
        a.applyWebSettings();
    }
    static WebServer& webServer(AppWindow& a) { return a.webServer_; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

namespace cmd = cascade::core::cmd;

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

}  // namespace

int main() {
    std::printf("test_remote_key_release\n");
    isolate();

    cascade::gui::AppWindow app;
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
    // The browser polls its status (the web remote's keep-alive) as it presses.
    auto webKey = [&] {
        engine.api().webRemoteBeat();
        return Access::apply(app, cmd::makeInt(FOXAPI_OP_TX_PTT, 1)); };

    WebServer& webServer = Access::webServer(app);

    // === 0: NO CONSENT, NO KEY - the browser's PTT is refused, never
    //     remembered: consenting afterwards keys nothing by itself. ==========
    {
        const FoxCommandResult r = webKey();
        CHECK(r.status == FOXAPI_DENIED);
        CHECK(Access::remoteHoldMs(app) == 0);
        consent(1);
        CHECK(Access::remoteHoldMs(app) == 0);
        consent(0);
        std::printf("  0: no consent -> status %d, remote hold %lld ms\n", r.status,
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === B: THE WEB SERVER NOT RUNNING releases a key already held =========
    // Run FIRST, before the server is ever started: WebServer::stop() itself
    // QUEUES a synthetic "transmitPtt=false" ControlRequest (web_server.cpp,
    // "A SERVER THAT HAS STOPPED RELEASES THE TRANSMIT KEY... by queueing the
    // release"), which the remote-controls phase's FIRST loop would apply regardless of
    // the standing condition this test means to isolate. A server that was
    // NEVER started queues nothing, so running()==false here is exactly the
    // case the condition names: "a server that never started or was disabled
    // in the settings panel".
    {
        CHECK(!webServer.running());
        consent(1);
        CHECK(ok(webKey()));
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::pumpRemote(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        // The rules let go IN THE SAME STEP, not only the radio - not left
        // for the next pass to notice.
        CHECK((engine.api().txFacts().owner) == 0u);
        std::printf("  B: server never started -> remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // --- NOW start the server, for A, D and the control below: all need
    //     running()==true throughout, with only the consent moving. --------
    const int port = startOnFreePort(webServer, loopbackConfig(), err);
    if (port < 0) {
        std::printf("SKIP: could not bind a loopback port for the web server (%s)\n", err.c_str());
        return testSummary("test_remote_key_release");
    }
    std::printf("  app's web server bound to 127.0.0.1:%d\n", port);
    CHECK(webServer.running());

    // === A: THE CONSENT WITHDRAWN releases a key already held, in the pass
    //     that applies it. ===================================================
    {
        CHECK(ok(webKey()));
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
        CHECK(ok(webKey()));
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::pumpRemote(app);
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
        CHECK((engine.api().txFacts().remoteArmed) == false);
        std::printf("  D: page closed -> remote hold %lld ms\n", static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === E: switching the web server OFF in the settings panel releases a
    //     key held, in the engine's NEXT PASS - not inside the click. Until the
    //     pump move (step P1) the window applied FOXAPP_OP_WEB_CONTROL_STOPPED
    //     itself in applyWebSettings, "in the same step"; that call ran beside
    //     the pass once the pass has a thread of its own (the review's F3), so
    //     it is gone: WebServer::stop() queues a release and the engine's
    //     remote-controls phase pulls it (and finds the server not running).
    //     The one timing difference of the change, said here: a pass (<= 16 ms
    //     in the product) instead of the click's own frame. ==================
    {
        desk.frame(true, false, false);
        consent(1);
        CHECK(ok(webKey()));
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::disableWeb(app);
        CHECK(!webServer.running());
        // The window did NOT reach into the engine: the key is still held until the pass runs.
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::pumpRemote(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        CHECK((engine.api().txFacts().owner) == 0u);
        std::printf("  E: web server switched off -> the key held until the engine's pass, then remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    desk.close();
    const int rc = testSummary("test_remote_key_release");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(g_scratch, ec);
    }
    return rc;
}
