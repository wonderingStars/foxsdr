/*
 * THE RECORDER SECTION'S START LABELS, IN THE REAL WINDOW (0.99.63, stub campaign B18, item U2).
 *
 * A recording's file is opened on a worker (gui/record_start.hpp, engine/record_start.hpp), so between the press of Record and the file being there the
 * Recorder section says so instead of the frame waiting for the disk: the button reads "Starting IQ - click to cancel" (or "Starting audio - ..."), a press
 * on it withdraws the start, a withdrawn start reads "Cancelling IQ start..." (disabled) until the worker has answered, and once the open has been out a
 * second the line under it says how long. The ledger carried the item as re-expressed (C5) and the four labels had no test at all: tests/test_record_start.cpp
 * says in its header that "the panel's drawing is ImGui output no test here reads", and the window the merge produced drew a bare "Record IQ" for the whole
 * time a start was out - a slow disk read as a button that did nothing.
 *
 * This DRAWS the real window's Recorder section (AppWindow::drawRecorderSection, in a bare ImGui window of this test's own) over the real engine, with the
 * recorders' opener held by the test - a start is pending for exactly as long as the test says, which is a slow disk as far as the section can tell - and
 * reads what the section drew from the UI census (gui/ui_census.hpp: every state of the button notes the label it carries). A press is a real ImGui mouse
 * event at the rectangle the census reported, so what is proved is the whole path: the press, the command the window sends, the engine's pass, the state the
 * engine publishes for the window to read, the next draw.
 *
 *   idle          Record IQ and Record audio, and nothing about a start
 *   opening       both opens held: both buttons read "Starting ... - click to cancel", the other labels are gone, and no waiting line yet
 *   waiting       a second on: "Waiting for the disk to open the file: <n> s" under each
 *   cancel        a press on the IQ button withdraws THAT start at the engine (and not the audio one), and its label becomes "Cancelling IQ start..." and is not
 *                 pressable (a second press changes nothing)
 *   settled       the held opens answered: the withdrawn IQ start left no take and the file it opened is closed, the audio take is armed and reads "Stop audio"
 *   stop          a press on Stop audio ends the take and the button is Record audio again
 *
 * Hermetic: scratch per-user directories with the process id in the name, no radio (the engine's own hooks), no network, no real recordings folder.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/recorder.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "gui/fonts.hpp"
#include "gui/ui_census.hpp"
#include "imgui.h"
#include "pass_seam.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::Recorder;

namespace {

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
#endif
}

// THE DISK THAT WILL NOT ANSWER, until the test lets it: the recorders' opener blocks on this (bounded, so a failed test cannot hang the suite) and then
// calls the REAL Recorder::openFile, so the file, its header and everything after it are the real thing. `opens` counts the opens that reached it.
std::atomic<bool> g_release{false};
std::atomic<int> g_opens{0};
Recorder::Opener g_realOpener;

bool heldOpener(const Recorder::OpenRequest& req, Recorder::OpenedFile& out, std::string& error) {
    ++g_opens;
    for (int i = 0; i < 3000 && !g_release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return g_realOpener(req, out, error);
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

// What the last draw noted, read back from the census file.
struct Seen {
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    bool has(const std::string& s) const { return items.count(s) != 0; }
    bool anyWith(const std::string& prefix) const {
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { return true; }
        }
        return false;
    }
    std::string firstWith(const std::string& prefix) const {
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { return s; }
        }
        return std::string();
    }
    const Rect* rect(const std::string& n) const {
        const auto it = rects.find(n);
        return it == rects.end() ? nullptr : &it->second;
    }
};

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;

    static void installHooks() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    // One frame's engine part, in drawUi's order: the session's queue taken, the publish, the status copy, the window's read of the state.
    static void look(AppWindow& a) {
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            rawEngine(a.engine_).pumpInputs();
            rawEngine(a.engine_).pumpPlugins();
            (void)rawEngine(a.engine_).pumpPublish();
        });
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    // The Recorder section alone, in a window of this test's own.
    static void draw(AppWindow& a) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(420.0f, 900.0f));
        ImGui::Begin("##recorder_test", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        a.drawRecorderSection();
        ImGui::End();
    }

    static void bindOpener(AppWindow& a, Recorder::Opener o) {
        passseam::on(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).iqRecorder_.bindOpener(o);
            rawEngine(a.engine_).audioRecorder_.bindOpener(std::move(o));
        });
    }
    static Recorder::Opener opener(AppWindow& a) {
        return passseam::read<Recorder::Opener>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).audioRecorder_.opener(); });
    }
    static void setRecordDir(AppWindow& a, std::string d) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).recordDir_ = std::move(d); });
    }
    static void poll(AppWindow& a, double nowS) { passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pollRecordStarts(nowS); }); }
    static void settleFinishes(AppWindow& a) {
        cascade::core::RecordFinisher::drain(std::chrono::steady_clock::now() + std::chrono::seconds(10));
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pollRecordFinishes(); });
    }
    static bool iqPending(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).iqStart_.pending(); }); }
    static bool audioPending(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).audioStart_.pending(); }); }
    static bool iqCancelled(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).iqStart_.cancelled(); }); }
    static bool audioCancelled(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).audioStart_.cancelled(); }); }
    static bool iqRecording(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).iqRecorder_.recording(); }); }
    static bool audioRecording(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).audioRecorder_.recording(); }); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

fs::path g_census;

// One frame of the application as far as this section is concerned: the engine's part, then the section drawn. The census holds ONLY what this draw noted.
Seen frame(AppWindow& app, float deltaSeconds = 1.0f / 60.0f) {
    Access::look(app);
    cascade::gui::census::clear();
    ImGui::GetIO().DeltaTime = deltaSeconds;
    ImGui::NewFrame();
    Access::draw(app);
    ImGui::Render();
    Seen s;
    if (!cascade::gui::census::write()) { return s; }
    std::ifstream in(g_census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { s.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string name;
        Rect rc;
        ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
        s.rects[name] = rc;
    }
    return s;
}

// A press and release on the middle of `at`, as ImGui's own mouse events: the pointer arrives, goes down, comes up - a frame each, the window drawn in each.
void click(AppWindow& app, const Rect& at) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.cx(), at.cy());
    (void)frame(app);
    io.AddMouseButtonEvent(0, true);
    (void)frame(app);
    io.AddMouseButtonEvent(0, false);
    (void)frame(app);
}

// The frames it takes for a command the window sent to be applied by the engine and the state it published to be drawn: the click's own frames, then these.
Seen settle(AppWindow& app, int frames = 3) {
    Seen s;
    for (int i = 0; i < frames; ++i) { s = frame(app); }
    return s;
}

bool waitFor(const std::function<bool()>& what, int ms = 15000) {
    for (int i = 0; i < ms / 5; ++i) {
        if (what()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return what();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_recorder_start_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_recorder_start_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { setEnv(url, "http://127.0.0.1:9"); }
    // THE CENSUS IS ON FOR THIS PROCESS (read once, on the first note) and the Recorder section is open (benchSection reads the list once too).
    g_census = g_scratch / "census.txt";
    setEnv("FOXSDR_UI_CENSUS", g_census.string());
    setEnv("FOXSDR_OPEN_SECTIONS", "Recorder");
    setEnv("FOXSDR_TEST_NO_CREATE", "");
    const fs::path recDir = g_scratch / "recordings";
    fs::create_directories(recDir, ec);

    Access::installHooks();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    {
        AppWindow app;
        Access::setRecordDir(app, recDir.string());
        g_realOpener = Access::opener(app);
        Access::bindOpener(app, &heldOpener);

        // --- idle -----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  idle\n");
        Seen s = settle(app, 4);
        CHECK(s.has("recorder:iq:button:Record IQ"));
        CHECK(s.has("recorder:audio:button:Record audio"));
        CHECK(!s.anyWith("recorder:iq:button:Starting"));
        CHECK(!s.anyWith("recorder:audio:button:Starting"));
        CHECK(!s.anyWith("recorder:iq:waiting"));
        const Rect* iqKey = s.rect("recorder:iq:button");
        const Rect* audioKey = s.rect("recorder:audio:button");
        CHECK(iqKey != nullptr && audioKey != nullptr);
        if (iqKey == nullptr || audioKey == nullptr) { return testSummary("test_recorder_start_app"); }
        const Rect iqRect = *iqKey;
        const Rect audioRect = *audioKey;

        // --- opening --------------------------------------------------------------------------------------------------------------------------------
        std::printf("  opening: both opens held\n");
        click(app, iqRect);
        click(app, audioRect);
        s = settle(app);
        CHECK(waitFor([&] { return g_opens.load() >= 2; }));  // both workers are out, and held
        CHECK(Access::iqPending(app));
        CHECK(Access::audioPending(app));
        s = settle(app);
        CHECK(s.has("recorder:iq:button:Starting IQ - click to cancel"));
        CHECK(s.has("recorder:audio:button:Starting audio - click to cancel"));
        // THE OTHER LABELS ARE GONE: a start in flight is not a button that records.
        CHECK(!s.has("recorder:iq:button:Record IQ"));
        CHECK(!s.has("recorder:audio:button:Record audio"));
        CHECK(!s.anyWith("recorder:iq:waiting"));  // not yet a second

        // --- waiting --------------------------------------------------------------------------------------------------------------------------------
        std::printf("  waiting: a second on\n");
        s = frame(app, 1.3f);
        s = frame(app, 1.0f / 60.0f);
        CHECK(s.has("recorder:iq:waiting"));
        CHECK(s.has("recorder:audio:waiting"));
        // The sentence is the one the file jobs say (the same key), with whole seconds.
        CHECK(s.has("recorder:iq:waiting:Waiting for the disk to open the file: 1 s"));

        // --- cancel ---------------------------------------------------------------------------------------------------------------------------------
        std::printf("  cancel: the IQ start withdrawn, the audio one not\n");
        const Rect* nowKey = s.rect("recorder:iq:button");
        CHECK(nowKey != nullptr);
        click(app, nowKey != nullptr ? *nowKey : iqRect);
        s = settle(app);
        CHECK(Access::iqCancelled(app));       // the press reached the ENGINE's start, and
        CHECK(!Access::audioCancelled(app));   // ...the other one's, no
        CHECK(Access::iqPending(app));         // the file is still being opened: nothing has answered yet
        CHECK(s.has("recorder:iq:button:Cancelling IQ start..."));
        CHECK(!s.has("recorder:iq:button:Starting IQ - click to cancel"));
        CHECK(s.has("recorder:audio:button:Starting audio - click to cancel"));
        // THE WITHDRAWN LABEL IS NOT A KEY: a second press on it withdraws nothing more and changes nothing the user can see.
        CHECK(s.has("recorder:iq:button:disabled"));
        click(app, *s.rect("recorder:iq:button"));
        s = settle(app);
        CHECK(s.has("recorder:iq:button:Cancelling IQ start..."));
        CHECK(!s.anyWith("recorder:iq:button:Record"));

        // --- settled --------------------------------------------------------------------------------------------------------------------------------
        std::printf("  settled: the held opens answer\n");
        g_release = true;
        CHECK(waitFor([&] {
            Access::poll(app, 1.0);
            return !Access::iqPending(app) && !Access::audioPending(app);
        }));
        s = settle(app);
        // The withdrawn start left no take; its file was closed unused. The audio take is armed.
        CHECK(!Access::iqRecording(app));
        CHECK(Access::audioRecording(app));
        CHECK(s.has("recorder:iq:button:Record IQ"));
        CHECK(!s.anyWith("recorder:iq:button:Cancelling"));
        CHECK(!s.anyWith("recorder:iq:button:Starting"));
        CHECK(s.has("recorder:audio:button:Stop audio"));
        CHECK(!s.anyWith("recorder:audio:waiting"));

        // --- stop -----------------------------------------------------------------------------------------------------------------------------------
        std::printf("  stop\n");
        click(app, *s.rect("recorder:audio:button"));
        (void)settle(app);
        Access::settleFinishes(app);
        s = settle(app);
        CHECK(!Access::audioRecording(app));
        CHECK(s.has("recorder:audio:button:Record audio"));
        CHECK(!s.anyWith("recorder:audio:button:Stop"));
    }

    ImGui::DestroyContext();
    fs::remove_all(g_scratch, ec);
    return testSummary("test_recorder_start_app");
}
