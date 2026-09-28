// test_remote_key_release.cpp - the standing conditions on the WEB REMOTE's
// transmit key (engine/stage3b-pre remote-key coverage, 2026-09-28).
//
// THE RULE (engine.cpp FOXAPI_OP_TX_PTT, ~5629; app_window.cpp
// applyWebControls, ~17628): a remote key exists only while (a) the operator
// has the Transmit page open (engine_.transmitOpen_), so FOXAPI_OP_TX_PTT
// itself refuses to key with it closed, and (b) the web server that carries
// the browser's requests is actually running - closing the Transmit page or
// stopping/disabling the server each release a key already held, in
// applyWebControls's two "STANDING CONDITIONS" checks, because the hold's own
// kRemotePttHoldMs expiry would otherwise leave the transmitter keyed for up
// to two seconds with nobody able to ask again.
//
// test_apply_command.cpp's FOXAPI_OP_TX_PTT coverage already proves the
// REFUSAL half (keying while transmitOpen_ is false is refused, never
// remembered). THIS FILE proves the RELEASE half, which lived in the window
// and had no test at all: a key already held is dropped the moment the page
// closes, and separately the moment the web server is not running - each
// checked in isolation, so a mutant deleting either "if" is caught by THAT
// check specifically, not accidentally by the other one also being true.
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
    static void setTransmitOpen(AppWindow& a, bool on) { a.engine_.transmitOpen_ = on; }
    static std::int64_t remoteHoldMs(AppWindow& a) {
        return a.engine_.transmitter_.remoteHoldRemainingMs();
    }
    static bool haveTx(AppWindow& a) { return a.engine_.transmitter_.haveSink(); }
    static void applyWebControls(AppWindow& a) { a.applyWebControls(); }
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

    // --- open a transmitter, so there is a real sink to key ----------------
    cascade::test::FakeTxIiod board;
    std::string err;
    CHECK(board.start(err));
    cascade::test::stockTxBoard(board);
    const std::string args = "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(board.port()));
    CHECK(ok(Access::apply(app, cmd::makeText(FOXAPI_OP_TX_OPEN, args))));
    CHECK(Access::haveTx(app));

    WebServer& webServer = Access::webServer(app);

    // === B: THE WEB SERVER NOT RUNNING releases a key already held =========
    // Run FIRST, before the server is ever started: WebServer::stop() itself
    // QUEUES a synthetic "transmitPtt=false" ControlRequest (web_server.cpp,
    // "A SERVER THAT HAS STOPPED RELEASES THE TRANSMIT KEY... by queueing the
    // release"), which applyWebControls's FIRST loop (webServer_.
    // takePendingControls()) would apply regardless of the standing-condition
    // "if" this test means to isolate - stopping an already-started server
    // would exercise THAT mechanism, not this one. A server that was NEVER
    // started queues nothing (stop()'s early return, guarded by
    // running_.exchange, never fires), so running()==false here is exactly
    // the case the check's own comment names: "a server disabled in the
    // settings panel, where there is no stop() to queue anything."
    {
        CHECK(!webServer.running());
        Access::setTransmitOpen(app, true);   // the page stays open throughout
        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPI_OP_TX_PTT, 1))));
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::applyWebControls(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        std::printf("  B: server never started -> remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // --- NOW start the server, for A and the control below: both need
    //     running()==true throughout, with only transmitOpen_ moving. ------
    const int port = startOnFreePort(webServer, loopbackConfig(), err);
    if (port < 0) {
        std::printf("SKIP: could not bind a loopback port for the web server (%s)\n", err.c_str());
        return testSummary("test_remote_key_release");
    }
    std::printf("  app's web server bound to 127.0.0.1:%d\n", port);
    CHECK(webServer.running());

    // === A: THE PAGE CLOSING releases a key already held ====================
    {
        Access::setTransmitOpen(app, true);
        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPI_OP_TX_PTT, 1))));
        CHECK(Access::remoteHoldMs(app) > 0);
        // The web server keeps running throughout - only transmitOpen_ moves.
        Access::setTransmitOpen(app, false);
        Access::applyWebControls(app);
        CHECK(Access::remoteHoldMs(app) == 0);
        std::printf("  A: page closed -> remote hold %lld ms\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    // === control: with the page open AND the server running, a fresh key
    //     survives one applyWebControls pass untouched (neither condition
    //     should be firing when neither is true - a mutant that always
    //     releases would still pass A and B above but fail here). ==========
    {
        CHECK(webServer.running());
        Access::setTransmitOpen(app, true);
        CHECK(ok(Access::apply(app, cmd::makeInt(FOXAPI_OP_TX_PTT, 1))));
        CHECK(Access::remoteHoldMs(app) > 0);
        Access::applyWebControls(app);
        CHECK(Access::remoteHoldMs(app) > 0);   // NEITHER condition applies - still held
        std::printf("  control: page open + server running -> remote hold %lld ms (still held)\n",
                    static_cast<long long>(Access::remoteHoldMs(app)));
    }

    const int rc = testSummary("test_remote_key_release");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(g_scratch, ec);
    }
    return rc;
}
