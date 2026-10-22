// test_window_web_feed.cpp - THE WEB PAGE'S READS DO NOT NEED A WINDOW THAT IS FRAMING, and the panels' side of owning no server (engine
// extraction stage 5b, rewritten for stage 5e; docs/engine-stage4.md 6.10 and 6.13). A real AppWindow over a real engine; the web server is the
// ENGINE'S, started the way the settings panel starts it (the staged edit and its Apply: a SERVER_CONFIG on the window's session). Until 5e the
// window's frame FED that server the audio the pipeline produced and the spectrum it drew (publishWebAudio, publishWebSpectrum), so a window
// that stopped framing - hidden, minimised, a menu held open - froze the page. The page reads the table now (read_audio and read_spectrum on the
// web server's own session) and nothing here ever frames the window after the server is up: only the engine's pass runs.
//
//   F1  THE PANEL STARTS THE ENGINE'S SERVER: Apply submits a SERVER_CONFIG; nothing listens until the pass has applied it and the host's
//       worker has started the server; the state the window then reads carries WEB_LISTENING and the SERVERS entry the panels draw from is
//       ACTIVE on the port with no browser on it.
//   F2  THE AUDIO: a browser's /api/audio stream gets the pipeline's audio - 16-bit mono at 48 kHz, live from the moment it joins - with the
//       window doing NOTHING (no look, no draw: the engine's pass runs, the window is not asked); the panel's count of browsers listening to
//       the audio (the list's value[2]) is one while it listens; switched off from the panel, the page's audio route is gone.
//   F3  THE PANELS' MIRRORS FOLLOW THE TABLE and wait for the answers the window is owed: after Apply the staged edits are not overwritten
//       by the old configuration while the SERVER_CONFIG is on its way (serverAsksPending_), and are taken from the facts once it landed.
//   F5  THE BASEMAP'S TILES ARE THE ONE FEED THE WINDOW STILL GIVES THE PAGE (EngineShell::serveWebTiles): a tile asked for with no basemap is
//       answered "pending" and the want is recorded; the window's pump clears on the basemap's change, drains the wants and publishes what its
//       plugin client (here the bounded-run stand-in) has - a BMP the page then gets - and no more than sixteen a frame, the rest left for the
//       page's retry.
//   F4  THE SPECTRUM KEEPS MOVING WHILE THE WINDOW DOES NOT FRAME: the page's /api/spectrum long-poll gets a new frame, again and again, each
//       newer than the cursor it gave, while the window is never looked through at all - the point of 5e. (Red against 5d's window-copy path:
//       the page's frame was the window's last look, so a window that does not look has no new frame to serve.)
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
#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "gui/app_window.hpp"
#include "gui/basemap_stand_in.hpp"
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
    static void attachStandInBasemap(AppWindow& a) { a.basemap_.attach(cascade::gui::standInBasemap()); }
    static void pumpTiles(AppWindow& a) { a.pumpWebTiles(); }
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
    // THE ENGINE'S PASS RUNS; THE WINDOW IS NEVER ASKED (no look, no draw, no feed): the audio is the table's read_audio on the web session.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (bytes.load() < 24000 && std::chrono::steady_clock::now() < until) {
        Access::pass(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    done = true;
    listener.join();
    std::printf("F2: /api/audio streamed %zu bytes of the pipeline's audio with the window not framing at all\n", bytes.load());
    CHECK(bytes.load() >= 24000);
    CHECK(waitUntil(5000, [&] { return engine.servers().webServer().audioListeners() == 0; }));

    // === F4: the spectrum keeps moving while the window does not frame ================================================================
    {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        std::uint64_t cursor = 0;
        int fresh = 0;
        bool monotonic = true;
        std::size_t bins = 0;
        const auto spectrumUntil = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (fresh < 8 && std::chrono::steady_clock::now() < spectrumUntil) {
            Access::pass(app);  // the engine publishes the pipeline's newest frame into the ring once a pass; nothing else runs
            const auto r = cli.Get(("/api/spectrum?since=" + std::to_string(cursor)).c_str());
            if (r && r->status == 200) {
                const nlohmann::json j = nlohmann::json::parse(r->body, nullptr, false);
                if (!j.is_discarded() && j.value("fresh", false)) {
                    const std::uint64_t seq = j.value("seq", std::uint64_t{0});
                    if (seq <= cursor) { monotonic = false; }
                    cursor = seq;
                    ++fresh;
                    if (j.contains("bins")) { bins = j["bins"].get<std::string>().size(); }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        CHECK(fresh >= 8);
        CHECK(monotonic);
        CHECK(bins > 0);
        std::printf("F4: /api/spectrum gave %d frames, each newer than its cursor (last seq %llu, %zu base64 chars of bins) with the window never looked through\n",
                    fresh, static_cast<unsigned long long>(cursor), bins);
    }

    // === F5: the basemap's tiles, the one feed the window still gives the page ===========================================================
    {
        Access::look(app);   // the state the pump reads WEB_LISTENING from
        CHECK(Access::listening(app));
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        // No basemap yet: the want is recorded and answered "pending" (202).
        const auto none = cli.Get("/api/tile/3/1/2");
        CHECK(none && none->status == 202);
        Access::attachStandInBasemap(app);
        Access::pumpTiles(app);   // the basemap changed: the stores are cleared (the want above is dropped with them), nothing to serve yet
        const auto again = cli.Get("/api/tile/3/1/2");
        CHECK(again && again->status == 202);   // asked again, and recorded again
        Access::pumpTiles(app);   // the pump drains the want, asks the plugin client, publishes the BMP
        const auto served = cli.Get("/api/tile/3/1/2");
        CHECK(served && served->status == 200);
        if (served) {
            CHECK(served->get_header_value("Content-Type") == "image/bmp");
            CHECK(served->body.size() >= 54u + 256u * 256u * 3u);   // a 256 x 256 24-bit BMP
        }
        // The bound: twenty distinct tiles asked for, the pump serves no more than sixteen of them in a frame.
        for (int x = 0; x < 20; ++x) {
            const auto r = cli.Get(("/api/tile/5/" + std::to_string(x) + "/3").c_str());
            CHECK(r && r->status == 202);
        }
        Access::pumpTiles(app);
        int ready = 0;
        int pending = 0;
        for (int x = 0; x < 20; ++x) {
            const auto r = cli.Get(("/api/tile/5/" + std::to_string(x) + "/3").c_str());
            if (r && r->status == 200) { ++ready; }
            if (r && r->status == 202) { ++pending; }
        }
        CHECK(ready == 16);
        CHECK(pending == 4);
        std::printf("F5: a tile asked for with no basemap is pending; with the stand-in attached the pump published it (a 256x256 BMP); of 20 asked for, one frame served %d and left %d for the page's retry\n",
                    ready, pending);
    }
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
    CHECK(engine.servers().webServer().audioListeners() == 0);
    {
        httplib::Client gone("127.0.0.1", port);
        gone.set_connection_timeout(0, 300000);
        CHECK(!gone.Get("/api/audio"));  // nothing answers any more
    }
    std::printf("F2: switched off from the panel - the window reads WEB_LISTENING go down and the page's streams are gone\n");
    return testSummary("test_window_web_feed");
}
