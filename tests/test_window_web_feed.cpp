// test_window_web_feed.cpp - WHAT THE WINDOW STILL FEEDS THE ENGINE'S WEB SERVER, and the panels' side of owning none (engine extraction
// stage 5b; docs/engine-stage4.md 6.10). A real AppWindow over a real engine; the web server is the ENGINE'S, started the way the settings
// panel starts it (the staged edit and its Apply: a SERVER_CONFIG on the window's session), and the window's frame feeds it the audio the
// pipeline produces (publishWebAudio -> EngineShell::pushWebAudio -> the server's ring) exactly as it fed the server it used to own.
//
//   F1  THE PANEL STARTS THE ENGINE'S SERVER: Apply submits a SERVER_CONFIG; nothing listens until the pass has applied it and the host's
//       worker has started the server; the state the window then reads carries WEB_LISTENING (which is what gates every feed below), and
//       the SERVERS entry the panels draw from is ACTIVE on the port with no browser on it.
//   F2  THE AUDIO FEED: a browser's /api/audio stream gets the pipeline's audio - 16-bit mono at 48 kHz, live from the moment it joins -
//       from the window's publishWebAudio; the panel's count of browsers listening to the audio (the list's value[2]) is one while it
//       listens and the feed pushes nothing while the server is not listening (the mark is kept current, so switching it on later starts
//       from live).
//   F3  THE PANELS' MIRRORS FOLLOW THE TABLE and wait for the answers the window is owed: after Apply the staged edits are not overwritten
//       by the old configuration while the SERVER_CONFIG is on its way (serverAsksPending_), and are taken from the facts once it landed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <httplib.h>

#include "core/app_commands.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "test_check.hpp"

namespace {

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
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_window_web_feed_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// A free loopback port (bound, read back, closed).
int freePort() {
    httplib::Server s;
    const int p = s.bind_to_any_port("127.0.0.1");
    s.stop();
    return p;
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static cascade::engine::Engine& engine(AppWindow& a) { return rawEngine(a.engine_); }
    // The engine's pass as far as the look needs it (the audio half reads the window's ImGui clock: no context in this test).
    static void pass(AppWindow& a) {
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            (void)rawEngine(a.engine_).pumpPublish();
        });
    }
    // The look: the state (its flags carry WEB_LISTENING) and the answers the window is owed.
    static void look(AppWindow& a) {
        a.pollUiAnswers();
        a.refreshUiState();
        a.actOnUiAnswers();
    }
    static void run(AppWindow& a) { a.uiSubmit(cascade::core::cmd::makeInt(FOXAPI_OP_RUN, 1)); }
    // The settings panel: the staged edits, then Apply.
    static void stageAndApply(AppWindow& a, int port) {
        a.webStageEnabled_ = true;
        a.webStageBind_ = "127.0.0.1";
        a.webPortMirror_ = port;
        a.webDirty_ = true;
        a.applyWebSettings();
    }
    static void stageOff(AppWindow& a) {
        a.webStageEnabled_ = false;
        a.webDirty_ = true;
        a.applyWebSettings();
    }
    static bool listening(AppWindow& a) { return a.webListening(); }
    static void feedAudio(AppWindow& a) { a.publishWebAudio(); }
    static const AppWindow::ServerPanel& panel(AppWindow& a, bool facts) { return a.serverPanel(facts); }
    static AppWindow::ServerLive live(AppWindow& a) { return a.readServerLive(false); }
    static int asksPending(AppWindow& a) { return a.serverAsksPending_; }
    static void syncMirrors(AppWindow& a) { a.syncServerMirrors(a.serverPanel(true)); }
    static int portMirror(AppWindow& a) { return a.webPortMirror_; }
    static bool stagedEnabled(AppWindow& a) { return a.webStageEnabled_; }
    static bool dirty(AppWindow& a) { return a.webDirty_; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_window_web_feed\n");
    isolate();
    cascade::gui::AppWindow app;
    cascade::engine::Engine& engine = Access::engine(app);

    // The receiver runs on the signal generator, so the pipeline makes audio.
    Access::run(app);
    for (int i = 0; i < 3; ++i) { Access::pass(app); }
    Access::look(app);
    CHECK(!Access::listening(app));

    // === F1: the panel starts the engine's server ====================================================================================
    const int port = freePort();
    CHECK(port > 0);
    httplib::Client probe("127.0.0.1", port);
    probe.set_connection_timeout(0, 200000);
    CHECK(!probe.Get("/api/status"));  // nothing listens yet
    Access::stageAndApply(app, port);
    CHECK(Access::asksPending(app) == 1);
    CHECK(!engine.servers().webServer().running());  // submitted, not applied: the click started nothing
    // F3 (first half): with the answer still owed, the mirrors are NOT re-read from a table that has not heard of the click.
    Access::look(app);
    Access::syncMirrors(app);
    CHECK(Access::portMirror(app) == port);
    CHECK(Access::stagedEnabled(app));
    for (int i = 0; i < 3; ++i) { Access::pass(app); }
    CHECK(engine.servers().waitIdle(std::chrono::seconds(30)));
    for (int i = 0; i < 3; ++i) { Access::pass(app); }
    CHECK(engine.servers().webServer().running());
    Access::look(app);
    CHECK(Access::asksPending(app) == 0);  // the answer came and was counted
    CHECK(Access::listening(app));
    CHECK(Access::panel(app, true).webRunning);
    CHECK(Access::panel(app, true).webPort == port);
    CHECK(Access::panel(app, true).webConfiguredPort == port);
    CHECK(!Access::panel(app, true).webFault);
    CHECK(Access::live(app).clients == 0 && Access::live(app).audioListeners == 0);
    std::printf("F1: Apply submitted a SERVER_CONFIG, the engine's worker started the server on :%d, the window reads WEB_LISTENING and an ACTIVE entry\n",
                port);

    // F3 (second half): the staged edits were taken from / agree with the facts once the answer landed.
    Access::syncMirrors(app);
    CHECK(Access::portMirror(app) == port);
    CHECK(Access::stagedEnabled(app));

    // === F2: the audio feed ==============================================================================================================
    std::atomic<std::size_t> bytes{0};
    std::atomic<bool> done{false};
    std::thread listener([&] {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(10, 0);
        (void)cli.Get("/api/audio", [&](const char* /*data*/, std::size_t len) {
            bytes += len;
            return !done.load() && bytes.load() < 24000;  // half a second of 16-bit mono at 48 kHz, then hang up
        });
    });
    CHECK(waitUntil(5000, [&] { return engine.servers().webServer().audioListeners() == 1; }));
    CHECK(Access::live(app).audioListeners == 1);  // the panel's "N listening to audio", read off the table
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (bytes.load() < 24000 && std::chrono::steady_clock::now() < until) {
        Access::pass(app);
        Access::look(app);
        Access::feedAudio(app);  // the window's frame, as drawUi does it
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    done = true;
    listener.join();
    std::printf("F2: /api/audio streamed %zu bytes of the pipeline's audio fed by the window's publishWebAudio\n", bytes.load());
    CHECK(bytes.load() >= 24000);
    CHECK(waitUntil(5000, [&] { return engine.servers().webServer().audioListeners() == 0; }));

    // THE PANEL SWITCHES IT OFF: the staged edit and Apply again. The window reads the bit go down, and the feed then pushes nothing (the
    // mark is kept current, so switching it on later starts from live) - the call is made, and the server has no listener to give anything to.
    Access::stageOff(app);
    for (int i = 0; i < 3; ++i) { Access::pass(app); }
    CHECK(engine.servers().waitIdle(std::chrono::seconds(30)));
    for (int i = 0; i < 3; ++i) { Access::pass(app); }
    Access::look(app);
    CHECK(!engine.servers().webServer().running());
    CHECK(!Access::listening(app));
    CHECK(!Access::panel(app, true).webRunning);
    Access::feedAudio(app);
    CHECK(engine.servers().webServer().audioListeners() == 0);
    std::printf("F2: switched off from the panel - the window reads WEB_LISTENING go down and the feed idles\n");
    return testSummary("test_window_web_feed");
}
