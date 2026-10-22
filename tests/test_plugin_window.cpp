// test_plugin_window.cpp - THE PLUGIN WINDOWS TAKE THE PLUGIN UI THROUGH THE HOST ACCESSORS AND THE POSITION FROM THE STATE
// (window port, group 14, part d: drawPluginWindows and openPluginWindowsFor; no API - the plugin UI is the HOST PROCESS's,
// polled from the frame as the plugin ABI says; docs/engine-stage4.md section 3 item 3).
//
// WHAT CHANGED UNDER THEM. They named the engine 22 times (the plugin UI's poll, tracks, paths, track-source names, panels and
// instruments; the runner's picture poll; the receiver's position three ways; the recorder's folder) and name it nowhere now:
// the plugin UI comes through eight host accessors (hostPollPlugins, hostTracks, hostPaths, hostTrackPluginNames, hostPanels,
// hostInstruments, hostAltitudeNear, hostPollImages), the receiver's position from the session's STATE, the recorder's folder
// from fact.recorder.dir at the click. The data the views draw moved to core/plugin_ui_data.hpp (test_plugin_ui_data_split).
//
// THE MODULES. Three real ones, copied into a plugin directory of this test's own: the group-14 fixture (a track source - four
// aircraft, one stale, and a trail - AND, since this part, a PANEL and an INSTRUMENT: fixed content), the stage-1 fixture (a
// text decoder: the one whose preset opens the shared Decoder output window) and the group-13 fixture (a picture decoder).
//
//   A  the feed: what one poll puts in the accessors, from the real modules (and nothing before it)
//   B  the frame: it polls once; a plugin window is on screen only when asked for; the panel and the instrument draw
//   C  the coverage feed and the receiver's position: from the STATE (not from the engine's own field), the stale aircraft
//      left out, and the page's hint
//   D  the map pages: one per track source, and a page whose module is gone is pruned
//   E  the preset press (openPluginWindowsFor): the page, the panel, the instrument, the picture window, the Decoder output window
//   F  the receiver's own pictures (hostPollImages) and the "Save as BMP" key, whose folder is the recorder's fact
//   G  an empty plugin directory
//
// THE TRAPS OF GROUP 14 (docs/engine-stage4.md): a test frame is LOOK then DRAW; the pass that applies a draw's command comes
// after it; and no GL is touched - the picture window is drawn with the texture's revision already current.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include "core/i18n.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/map_view.hpp"
#include "gui/spectrum_view.hpp"
#include "gui/track_metrics.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

namespace cmd = cascade::core::cmd;
using cascade::gui::AppWindow;

namespace {
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }
}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;

    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
    }

    // ---- one frame's engine part, in drawUi's order ----------------------------------------------------------------------
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
    static void after(AppWindow& a) {
        passseam::workers(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpWorkers();
            rawEngine(a.engine_).pumpAudio();
        });
    }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }

    // ---- the members ---------------------------------------------------------------------------------------------------------
    static void drawPluginWindows(AppWindow& a) { a.drawPluginWindows(); }
    // The window's half of a preset press, as the event handler and run()'s seam call it: the module's name and what it declares.
    static void openFor(AppWindow& a, const cascade::core::LoadedPlugin& p) { a.openPluginWindowsFor(p.name, p.capabilities); }
    // The session's events, once (the frame's pumpTransmitKey does it at its end; a look here does not).
    static void pumpEvents(AppWindow& a) { a.pumpUiEvents(); }
    // What the window hands the engine once a frame (the track-info answers, the pictures' headers), and what a browser reads.
    static void hand(AppWindow& a) { a.handFrontEndFacts(); }
    static cascade::net::RadioStatus webStatus(AppWindow& a) { return rawEngine(a.engine_).webStatusNow(); }
    static void attachTrackInfo(AppWindow& a, const CascadeTrackInfoApi* api) { a.trackInfo_.attach(api); }
    static void sendTrackInfoText(AppWindow& a) { a.sendTrackInfoText(); }
    static const AppWindow::DecoderLineCache& decoderLines(AppWindow& a) { return a.decoderLines(); }
    static const SpectrumView& spectrum(AppWindow& a) { return *a.spectrum_; }
    // the eight accessors
    static void pollPlugins(AppWindow& a) { a.hostPollPlugins(); }
    static const std::vector<cascade::core::HostTrack>& tracks(AppWindow& a) { return a.hostTracks(); }
    static const std::vector<cascade::core::HostPath>& paths(AppWindow& a) { return a.hostPaths(); }
    static const std::vector<std::string>& trackNames(AppWindow& a) { return a.hostTrackPluginNames(); }
    static const std::vector<cascade::core::HostPanel>& panels(AppWindow& a) { return a.hostPanels(); }
    static const std::vector<cascade::core::HostInstrument>& instruments(AppWindow& a) { return a.hostInstruments(); }
    static bool altitudeNear(AppWindow& a, const std::string& plugin, const std::string& id, double lat, double lon,
                             double& alt) {
        return a.hostAltitudeNear(plugin, id, lat, lon, alt);
    }
    static void pollImages(AppWindow& a, std::vector<cascade::core::HostImage>& out) { a.hostPollImages(out); }

    static std::vector<AppWindow::MapPage>& pages(AppWindow& a) { return a.mapPages_; }
    static const std::vector<cascade::core::HostTrack>& pageTracks(AppWindow& a) { return a.pageTracks_; }
    static const std::vector<cascade::core::HostPath>& pagePaths(AppWindow& a) { return a.pagePaths_; }
    static AppWindow::MapPage& ensurePage(AppWindow& a, const std::string& name) { return a.ensureMapPage(name); }
    static cascade::core::PluginWindows& windows(AppWindow& a) { return a.pluginWindows_; }
    static bool& decoderWindowOpen(AppWindow& a) { return a.decoderWindowOpen_; }
    static CoverageMap& coverage(AppWindow& a) { return a.coverage_; }
    static const std::vector<cascade::core::HostImage>& images(AppWindow& a) { return a.pluginImages_; }
    static std::vector<std::uint64_t>& imageTexRev(AppWindow& a) { return a.imageTexRev_; }
    static const std::string& imageSaveNote(AppWindow& a) { return a.imageSaveNote_; }
    static std::map<std::string, AppWindow::InstrumentSeen>& instrumentSeen(AppWindow& a) { return a.instrumentSeen_; }
    // The engine's plugin host's records: a COPY made on the pass's thread (the pass owns the host; a reference into it
    // would be read from this thread beside a free-running pass).
    static std::vector<cascade::core::LoadedPlugin> loaded(AppWindow& a) {
        return passseam::read<std::vector<cascade::core::LoadedPlugin>>(rawEngine(a.engine_),
                                                                          [&] { return rawEngine(a.engine_).pluginHost_.plugins(); });
    }
    static const AppWindow::PluginListCache& pluginRows(AppWindow& a) { return a.pluginLists(); }
    static std::string fact(AppWindow& a, const char* key) {
        std::string s;
        a.readFactText(key, s);
        return s;
    }
    // The engine's own, for what the window must NOT be reading and for the runner's feed. The position is the pass's
    // (passseam::on); the runner is fed and counted directly: PluginRunner guards every call with its own mutex (the product
    // feeds it from the audio thread beside the pass), and pluginRunner_ is a member the engine never replaces.
    static void engineSetPosition(AppWindow& a, double lat, double lon) {
        passseam::on(rawEngine(a.engine_), [&] { (void)rawEngine(a.engine_).applyReceiverPosition(lat, lon); });
    }
    static void feedAudio(AppWindow& a, std::size_t frames) {
        std::vector<float> mono(frames, 0.0f);
        rawEngine(a.engine_).pluginRunner_.processAudio(mono.data(), frames);
    }
    static std::size_t activeDecoders(AppWindow& a) { return rawEngine(a.engine_).pluginRunner_.activeCount(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

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
const char* const kWin = "pluginwin";
const std::string kTracks = "Group Fourteen Tracks";
const std::string kTracksB = "Group Fourteen Tracks B";
const std::string kPanelTitle = "Group Fourteen Panel";
const std::string kInstrumentTitle = "Group Fourteen Instrument";
const std::string kText = "Stage One Fixture";
const std::string kImage = "Group Thirteen Fixture";
std::string g_text;

const std::string kPanelId = kPanelTitle + "###panel_" + kTracks;
const std::string kInstrumentId = kInstrumentTitle + "###instrument_" + kTracks + "|" + kInstrumentTitle;
const std::string kPanelIdB = kPanelTitle + "###panel_" + kTracksB;
const std::string kImageId = kImage + " image###image_" + kImage;

// One ImGui frame with `body` drawn in a window of the given size (no engine pass), the text it lettered kept.
template <class F>
void imguiFrame(F body, bool capture = false, float w = 900.0f, float h = 700.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin(kWin, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (capture) { ImGui::LogToBuffer(); }
    body();
    if (capture) {
        g_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        if (std::getenv("PLUGINWIN_DUMP") != nullptr) { std::printf("---- frame text ----\n%s\n", g_text.c_str()); }
    }
    ImGui::End();
    ImGui::Render();
}

// LOOK, then DRAW `body`, then the engine's own work after the frame.
template <class F>
void frame(AppWindow& a, F body, bool capture = false) {
    Access::look(a);
    imguiFrame(body, capture);
    Access::after(a);
}
void pass(AppWindow& a) {
    Access::look(a);
    Access::after(a);
}
void passes(AppWindow& a, int n) {
    for (int i = 0; i < n; ++i) { pass(a); }
}
bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
bool positionSet(AppWindow& a) { return (Access::state(a).flags & FOXAPI_RX_POSITION_SET) != 0u; }

void send(AppWindow& a, const cmd::QueuedCommand& q) {
    Access::submit(a, q);
    passes(a, 2);
}
void send(AppWindow& a, const FoxCommand& c) { send(a, cmd::QueuedCommand{c, {}}); }
void setPosition(AppWindow& a, double lat, double lon) { send(a, cmd::makeNum(FOXAPI_OP_SET_POSITION, lat, lon)); }

// The plugin windows drawn once, with the text they letter kept.
void drawPlugins(AppWindow& a, bool capture = false) {
    frame(a, [&] { Access::drawPluginWindows(a); }, capture);
}

struct Env {
    std::unique_ptr<AppWindow> app;
    Env() {
        app = std::make_unique<AppWindow>();
        passes(*app, 3);
    }
    AppWindow& a() { return *app; }
};

// The engine's record for `name`, COPIED out of the host (null: none by that name).
std::unique_ptr<const cascade::core::LoadedPlugin> loadedNamed(AppWindow& a, const std::string& name) {
    for (const cascade::core::LoadedPlugin& p : Access::loaded(a)) {
        if (p.name == name) { return std::make_unique<const cascade::core::LoadedPlugin>(p); }
    }
    return nullptr;
}

// A click on the item `label` names inside the plugin window `windowId`: found by sweeping the pointer over that window's
// rectangle while `body` is drawn. A page draws its body in child windows, so the item's id is the label hashed with the id of
// the window - or of one of its children - that holds it: every such id is a candidate, and the pointer finds the right one.
template <class F>
bool clickInWindow(F body, const std::string& windowId, const char* label) {
    ImGuiIO& io = ImGui::GetIO();
    std::vector<ImGuiID> targets;
    const auto drawAll = [&] { body(); };
    const auto collect = [&] {
        targets.clear();
        ImGuiWindow* root = ImGui::FindWindowByName(windowId.c_str());
        if (root == nullptr) { return; }
        for (ImGuiWindow* w : GImGui->Windows) {
            for (ImGuiWindow* up = w; up != nullptr; up = up->ParentWindow) {
                if (up == root) {
                    targets.push_back(ImGui::GetIDWithSeed(label, nullptr, w->ID));
                    break;
                }
            }
        }
    };
    imguiFrame(drawAll);
    imguiFrame(drawAll);
    collect();
    ImGuiWindow* w = ImGui::FindWindowByName(windowId.c_str());
    if (w == nullptr || targets.empty()) {
        std::printf("      (clickInWindow: window %s, %zu candidate ids)\n", w == nullptr ? "missing" : "found", targets.size());
        return false;
    }
    const ImVec2 pos = w->Pos;
    const ImVec2 size = w->Size;
    bool found = false;
    float hx = 0.0f, hy = 0.0f;
    for (float y = pos.y + 4.0f; y < pos.y + size.y && !found; y += 3.0f) {
        for (float x = pos.x + 4.0f; x < pos.x + size.x && !found; x += 6.0f) {
            io.AddMousePosEvent(x, y);
            imguiFrame(drawAll);
            imguiFrame(drawAll);
            if (std::find(targets.begin(), targets.end(), GImGui->HoveredId) != targets.end()) {
                found = true;
                hx = x;
                hy = y;
            }
        }
    }
    if (!found) {
        std::printf("      (clickInWindow: no point of %.0fx%.0f at %.0f,%.0f hovered the item)\n", size.x, size.y, pos.x, pos.y);
        io.AddMousePosEvent(2.0f, 2.0f);
        imguiFrame(drawAll);
        return false;
    }
    io.AddMousePosEvent(hx, hy);
    imguiFrame(drawAll);
    io.AddMouseButtonEvent(0, true);
    imguiFrame(drawAll);
    io.AddMouseButtonEvent(0, false);
    imguiFrame(drawAll);
    io.AddMousePosEvent(2.0f, 2.0f);
    imguiFrame(drawAll);
    return true;
}

// ======================================================================================================================
// A. THE FEED
// ======================================================================================================================
void testFeed(AppWindow& a) {
    std::printf("  A: what one poll puts in the accessors, from the real modules\n");
    // NOTHING IS POLLED UNTIL THE FRAME ASKS. The engine's rebuild has made the panel and the instrument (their shape is
    // resolved at create time: title, headings) but the rows, the reading and every track and path come only with a poll.
    CHECK(Access::tracks(a).empty());
    CHECK(Access::paths(a).empty());
    CHECK_EQ(Access::panels(a).size(), std::size_t(1));
    if (Access::panels(a).empty() || Access::instruments(a).empty()) { CHECK(!"no panel or instrument"); return; }
    CHECK(Access::panels(a)[0].rows.empty());
    CHECK_EQ(Access::instruments(a).size(), std::size_t(1));
    CHECK(!Access::instruments(a)[0].have);
    CHECK(Access::instruments(a)[0].rows.empty());
    // THE NAMES are the modules that hold a track-source instance - the other fixtures do not.
    CHECK_EQ(Access::trackNames(a).size(), std::size_t(1));
    if (Access::trackNames(a).empty()) { CHECK(!"no track source name"); return; }
    CHECK(Access::trackNames(a)[0] == kTracks);

    Access::pollPlugins(a);
    CHECK_EQ(Access::tracks(a).size(), std::size_t(4));
    CHECK_EQ(Access::paths(a).size(), std::size_t(1));
    CHECK_EQ(Access::panels(a).size(), std::size_t(1));
    CHECK_EQ(Access::instruments(a).size(), std::size_t(1));
    CHECK_EQ(Access::trackNames(a).size(), std::size_t(1));

    if (Access::panels(a).empty() || Access::instruments(a).empty()) { CHECK(!"no panel or instrument"); return; }
    const auto& pn = Access::panels(a)[0];
    CHECK(pn.plugin == kTracks);
    CHECK(pn.title == kPanelTitle);
    CHECK_EQ(pn.headings.size(), std::size_t(2));
    CHECK(pn.headings[0] == "Item" && pn.headings[1] == "Reading");
    CHECK_EQ(pn.rows.size(), std::size_t(3));
    CHECK(pn.rows[0].kind == CASCADE_ROW_HEADING && std::string(pn.rows[0].cells[0]) == "G14 summary");
    CHECK(pn.rows[1].kind == CASCADE_ROW_CELLS && std::string(pn.rows[1].cells[0]) == "aircraft" &&
          std::string(pn.rows[1].cells[1]) == "3");
    CHECK(pn.rows[2].flags == CASCADE_ROW_FLAG_MUTED && std::string(pn.rows[2].cells[0]) == "stale");

    const auto& in = Access::instruments(a)[0];
    CHECK(in.plugin == kTracks);
    CHECK(in.title == kInstrumentTitle);
    CHECK(in.kind == CASCADE_INSTRUMENT_GENERIC);
    CHECK(in.have);
    CHECK_EQ(in.state.seq, 7u);
    CHECK(in.state.flags == CASCADE_INSTRUMENT_FLAG_LOCK);
    CHECK_NEAR(in.state.values[0], 42.0, 1e-12);
    CHECK(std::string(in.state.text[0]) == "G14 READY");
    CHECK_EQ(in.headings.size(), std::size_t(2));
    CHECK(in.headings[0] == "Event" && in.headings[1] == "Detail");
    CHECK_EQ(in.rows.size(), std::size_t(1));
    CHECK(std::string(in.rows[0].cells[0]) == "G14 memory" && std::string(in.rows[0].cells[1]) == "m1");

    // A second poll is a poll, not an append: the same four, the same one.
    Access::pollPlugins(a);
    CHECK_EQ(Access::tracks(a).size(), std::size_t(4));
    CHECK_EQ(Access::panels(a)[0].rows.size(), std::size_t(3));
    CHECK_EQ(Access::instruments(a)[0].rows.size(), std::size_t(1));

    double alt = 0.0;
    CHECK(Access::altitudeNear(a, kTracks, "G14A3", 51.50, -0.10, alt));
    CHECK_NEAR(alt, 9000.0, 1.0);
    std::printf("      4 tracks, 1 path, 1 panel (3 rows), 1 instrument (seq 7, 1 memory row), 1 source name\n");
}

// ======================================================================================================================
// B. THE FRAME
// ======================================================================================================================
void testFrame(AppWindow& a) {
    std::printf("  B: the frame polls, opens nothing by itself, and draws the panel and the instrument when asked\n");
    // THE FRAME IS WHAT POLLS: a window made and looked at (passes, no draw) holds no track, no path and no row; one draw of
    // the plugin windows fills them all (hostPollPlugins, once, at the head of the frame).
    if (Access::panels(a).empty() || Access::instruments(a).empty()) { CHECK(!"no panel or instrument"); return; }
    CHECK(Access::tracks(a).empty());
    CHECK(Access::paths(a).empty());
    CHECK(Access::panels(a)[0].rows.empty());
    CHECK(!Access::instruments(a)[0].have);
    drawPlugins(a);
    CHECK_EQ(Access::paths(a).size(), std::size_t(1));
    CHECK_EQ(Access::panels(a)[0].rows.size(), std::size_t(3));
    CHECK(Access::instruments(a)[0].have);
    CHECK_EQ(Access::tracks(a).size(), std::size_t(4));
    CHECK_EQ(Access::pages(a).size(), std::size_t(1));
    if (Access::pages(a).empty()) { CHECK(!"no map page"); return; }
    CHECK(Access::pages(a)[0].plugin == kTracks);
    // NOTHING OPENS BY ITSELF (0.79.1: the application starts on the main screen alone).
    CHECK_EQ(Access::windows(a).count(), std::size_t(0));
    CHECK(!Access::pages(a)[0].open);
    CHECK(ImGui::FindWindowByName(kPanelId.c_str()) == nullptr);
    CHECK(ImGui::FindWindowByName(kInstrumentId.c_str()) == nullptr);
    drawPlugins(a, true);
    CHECK(!has(g_text, "G14 summary"));
    CHECK(!has(g_text, "G14 memory"));

    // THE PANEL: asked for by its window id, it letters the plugin's table.
    Access::windows(a).show(kPanelId);
    drawPlugins(a, true);
    CHECK(ImGui::FindWindowByName(kPanelId.c_str()) != nullptr);
    CHECK(has(g_text, "G14 summary"));
    CHECK(has(g_text, "aircraft"));
    CHECK(has(g_text, "stale"));
    CHECK(has(g_text, "Reading"));
    std::printf("      the panel window lettered its heading, its two rows and its column\n");

    // THE INSTRUMENT: its face is a drawing, its memory a table: the memory is lettered, and the window remembers the sequence
    // it has drawn (the NEW lamp's cue).
    CHECK(Access::instrumentSeen(a).count(kInstrumentId) == 0u);
    Access::windows(a).show(kInstrumentId);
    drawPlugins(a, true);
    CHECK(ImGui::FindWindowByName(kInstrumentId.c_str()) != nullptr);
    CHECK(has(g_text, "G14 memory"));
    CHECK(has(g_text, "m1"));
    CHECK_EQ(Access::instrumentSeen(a).count(kInstrumentId), std::size_t(1));
    CHECK_EQ(Access::instrumentSeen(a)[kInstrumentId].seq, 7u);
    std::printf("      the instrument window lettered its memory row and kept seq 7\n");

    // Hidden again, both are gone from the text.
    Access::windows(a).hide(kPanelId);
    Access::windows(a).hide(kInstrumentId);
    drawPlugins(a, true);
    CHECK(!has(g_text, "G14 summary"));
    CHECK(!has(g_text, "G14 memory"));
}

// ======================================================================================================================
// C. THE COVERAGE FEED AND THE POSITION
// ======================================================================================================================
void testCoverage(AppWindow& a) {
    std::printf("  C: the coverage is fed from the STATE's position, the stale aircraft left out\n");
    if (Access::pages(a).empty()) { CHECK(!"no map page"); return; }
    auto& page = Access::pages(a)[0];
    page.open = true;

    // NO POSITION: nothing to measure a bearing from. The accumulator is never fed and the page says so.
    CHECK(!positionSet(a));
    drawPlugins(a, true);
    CHECK(Access::coverage(a).empty());
    CHECK(has(g_text, "set the RX position to measure coverage"));
    std::printf("      no position: no coverage, and the hint says to set one\n");

    // A POSITION: the three aircraft shown are measured, the fourth (120 s old) is not.
    const double lat = 51.47, lon = -0.45;
    setPosition(a, lat, lon);
    CHECK(positionSet(a));
    drawPlugins(a, true);
    CHECK(!Access::coverage(a).empty());
    CHECK(!has(g_text, "set the RX position to measure coverage"));
    CHECK(has(g_text, "bearings, best"));
    double farKm = 0.0;
    for (const cascade::core::HostTrack& t : Access::tracks(a)) {
        if (!cascade::core::trackPresentation(t.t.ageMs, t.t.kind).visible) { continue; }
        farKm = std::max(farKm, cascade::gui::greatCircleKm(lat, lon, t.t.latDeg, t.t.lonDeg));
    }
    const double stale = cascade::gui::greatCircleKm(lat, lon, 50.00, 1.00);
    CHECK_NEAR(Access::coverage(a).peakKm(), farKm, 1e-9);
    CHECK(stale > farKm);  // so a stale aircraft that WAS recorded would have raised the peak
    CHECK_EQ(Access::coverage(a).filledBuckets(), 3);
    std::printf("      from %.2f %.2f: %d bearings, best %.1f km (the stale aircraft, %.1f km, is not recorded)\n", lat, lon,
                Access::coverage(a).filledBuckets(), Access::coverage(a).peakKm(), stale);

    // THE STATE, NOT THE ENGINE'S FIELD: the engine is moved far away with no look, so the window's state is still the old
    // position, and a draw measures from that - a read of the engine's own field would record bearings from the new one.
    const int before = Access::coverage(a).filledBuckets();
    const double peakBefore = Access::coverage(a).peakKm();
    Access::engineSetPosition(a, 40.0, 10.0);
    CHECK_NEAR(Access::state(a).rxLatDeg, lat, 1e-12);
    imguiFrame([&] { Access::drawPluginWindows(a); });  // a draw with NO look: the state is a pass old
    CHECK_EQ(Access::coverage(a).filledBuckets(), before);
    CHECK_NEAR(Access::coverage(a).peakKm(), peakBefore, 1e-12);
    std::printf("      the engine moved with no look: the draw still measured from the state's position\n");

    // Put it back through the session, so the later parts start from a position and an empty accumulator.
    setPosition(a, lat, lon);
    page.open = false;
}

// ======================================================================================================================
// D. THE MAP PAGES
// ======================================================================================================================
void testPages(AppWindow& a) {
    std::printf("  D: one map page per track source, and a page whose module is gone is pruned\n");
    drawPlugins(a);
    CHECK_EQ(Access::pages(a).size(), std::size_t(1));
    if (Access::pages(a).empty()) { CHECK(!"no map page"); return; }
    auto& live = Access::pages(a)[0];
    live.open = true;
    live.visibleCount = 99;  // a mark: a live page survives a frame with its state intact

    Access::ensurePage(a, "Dead Source").open = true;
    CHECK_EQ(Access::pages(a).size(), std::size_t(2));
    drawPlugins(a);
    CHECK_EQ(Access::pages(a).size(), std::size_t(1));
    CHECK(Access::pages(a)[0].plugin == kTracks);
    CHECK(Access::pages(a)[0].open);
    CHECK_EQ(Access::pages(a)[0].visibleCount, std::size_t(3));  // refreshed by the frame, same page
    std::printf("      the dead page went, the live one stayed open and was refreshed\n");
    Access::pages(a)[0].open = false;
}

// ======================================================================================================================
// E. THE PRESET PRESS
// ======================================================================================================================
void testOpenFor(AppWindow& a) {
    std::printf("  E: openPluginWindowsFor: the page, the panel, the instrument, the picture and the Decoder output window\n");
    drawPlugins(a);
    const auto tracks = loadedNamed(a, kTracks);
    const auto text = loadedNamed(a, kText);
    const auto image = loadedNamed(a, kImage);
    CHECK(tracks != nullptr);
    CHECK(text != nullptr);
    CHECK(image != nullptr);
    if (tracks == nullptr || text == nullptr || image == nullptr) { return; }

    // THE TRACK SOURCE WITH A PANEL AND AN INSTRUMENT: its map page opens, both windows open, and the shared Decoder output
    // window does NOT (the module has windows of its own).
    if (Access::pages(a).empty()) { CHECK(!"no map page"); return; }
    CHECK(!Access::pages(a)[0].open);
    CHECK_EQ(Access::windows(a).count(), std::size_t(0));
    Access::decoderWindowOpen(a) = false;
    Access::openFor(a, *tracks);
    CHECK(Access::pages(a)[0].open);
    CHECK(Access::windows(a).shown(kPanelId));
    CHECK(Access::windows(a).shown(kInstrumentId));
    CHECK_EQ(Access::windows(a).count(), std::size_t(2));
    CHECK(!Access::decoderWindowOpen(a));
    std::printf("      the track source: map page open, panel and instrument windows shown, no Decoder output window\n");

    // THE PICTURE DECODER: its picture window, no page, nothing else.
    const std::size_t pagesBefore = Access::pages(a).size();
    Access::openFor(a, *image);
    CHECK(Access::windows(a).shown(kImageId));
    CHECK_EQ(Access::windows(a).count(), std::size_t(3));
    CHECK_EQ(Access::pages(a).size(), pagesBefore);  // not a track source: no page is made for it
    CHECK(!Access::decoderWindowOpen(a));

    // THE TEXT DECODER with no window of its own: the press opens the shared Decoder output window instead.
    Access::openFor(a, *text);
    CHECK(Access::decoderWindowOpen(a));
    CHECK_EQ(Access::windows(a).count(), std::size_t(3));
    CHECK_EQ(Access::pages(a).size(), pagesBefore);
    std::printf("      the picture decoder: its picture window; the text decoder: the Decoder output window\n");

    // A second press of the same changes nothing.
    Access::openFor(a, *tracks);
    CHECK_EQ(Access::windows(a).count(), std::size_t(3));
    CHECK_EQ(Access::pages(a).size(), pagesBefore);

    // Left tidy for the next part.
    Access::pages(a)[0].open = false;
    Access::windows(a).hide(kPanelId);
    Access::windows(a).hide(kInstrumentId);
    Access::windows(a).hide(kImageId);
    Access::decoderWindowOpen(a) = false;
}

// ======================================================================================================================
// E2. THE PRESET PRESS IS AN EVENT (the pump move, step P2; API 0.3 8.14)
// ======================================================================================================================
// The engine used to call the window's openPluginWindowsFor from inside applyPluginPreset, on whichever thread the pass ran on.
// It raises FOXAPI_EVENT_PRESET_APPLIED now, once for each application (the module's file name), and the window opens what the
// module contributes when it looks. Held here against the real engine and the real fixture plugin: the engine's pass opens NOTHING
// (the hook is gone), the event does, from the PLUGINS row, once.
void testPresetEvent(AppWindow& a) {
    std::printf("  E2: a preset applied is an event: the pass opens nothing, the window's look opens the Decoder output window\n");
    drawPlugins(a);
    Access::pumpEvents(a);  // whatever earlier parts left in the session's ring goes first
    Access::decoderWindowOpen(a) = false;
    std::string file;
    for (const cascade::gui::PluginRow& row : Access::pluginRows(a).plugins) {
        if (row.name == kText) { file = row.file; }
    }
    CHECK(!file.empty());
    if (file.empty()) { return; }
    // The press: PLUGIN_PRESET through the window's own session, applied by the engine's next pass.
    send(a, cmd::makeText(FOXAPI_OP_PLUGIN_PRESET, file, 0));
    CHECK(!Access::decoderWindowOpen(a));   // the engine opened nothing itself
    Access::pumpEvents(a);
    CHECK(Access::decoderWindowOpen(a));    // ...the event did: the text decoder has no window of its own
    // ONE event for ONE application: a second look finds none, and closing the window by hand is not undone by it.
    Access::decoderWindowOpen(a) = false;
    Access::pumpEvents(a);
    CHECK(!Access::decoderWindowOpen(a));
    // The same press again is a new application: a new event.
    send(a, cmd::makeText(FOXAPI_OP_PLUGIN_PRESET, file, 0));
    Access::pumpEvents(a);
    CHECK(Access::decoderWindowOpen(a));
    Access::decoderWindowOpen(a) = false;
    std::printf("      one application, one event, one window\n");
}

// ======================================================================================================================
// F2. WHAT THE WEB STATUS TAKES OF THE PLUGIN POLL, AND THE TRACK-INFO PLUGIN'S LINES (the pump move, step P2)
// ======================================================================================================================
// The engine's pass used to ask the window, through two hooks, for a track's enrichment and the pictures' headers while it
// published (reading the window's TrackInfoCache and picture buffer from the pass), and for the track-info plugin's status lines
// every pass. The window hands the first two over in FrontEndFacts::webExtras and SENDS the lines as commands. A fake track-info
// plugin (a scripted C table, test_track_info_cache.cpp's shape) and the fixture track source and picture decoder drive both.
std::string g_infoKnownId;
bool g_infoLineSent = false;
void* infoCreate() { return reinterpret_cast<void*>(1); }
std::int32_t infoGet(void*, const char* id, std::uint32_t, CascadeTrackInfo* out) {
    if (id == nullptr || g_infoKnownId != id) { return CASCADE_INFO_MISSING; }
    out->registration = "G-TEST";
    out->typeCode = "A319";
    out->typeName = "Airbus A319-111";
    out->manufacturer = "Airbus";
    out->operatorName = "Test Air";
    out->country = "Testland";
    return CASCADE_INFO_READY;
}
void infoRelease(void*, const CascadeTrackInfo*) {}
std::int32_t infoPollText(void*, char* buf, size_t cap) {
    if (g_infoLineSent || cap < 40) { return 0; }
    g_infoLineSent = true;
    const char line[] = "cannot reach the registry\n";
    std::memcpy(buf, line, sizeof(line) - 1);
    return static_cast<std::int32_t>(sizeof(line) - 1);
}
void infoDestroy(void*) {}

void testWebExtras(AppWindow& a) {
    std::printf("  F2: the web status' enrichment and picture headers are HANDED OVER; the track-info lines are SENT\n");
    drawPlugins(a);
    const std::vector<cascade::core::HostTrack> tracks = Access::tracks(a);
    CHECK(tracks.size() >= 2u);
    if (tracks.size() < 2u) { return; }
    g_infoKnownId = tracks[0].t.id;
    CascadeTrackInfoApi api{};
    api.structSize = static_cast<std::uint32_t>(sizeof(api));
    api.create = &infoCreate;
    api.get_info = &infoGet;
    api.release_info = &infoRelease;
    api.poll_text = &infoPollText;
    api.destroy = &infoDestroy;
    Access::attachTrackInfo(a, &api);

    // NOTHING HANDED OVER YET: the web status carries no enrichment, no pictures and - since the pump move's step P3 - no tracks either
    // (a headless pass has none): the pass reads NOTHING of the plugin UI itself, which is the window's and rewritten by its poll
    // every frame; the tracks reach the browser in the same hand-over as the enrichment and the pictures' headers.
    {
        Access::look(a);
        const cascade::net::RadioStatus st = Access::webStatus(a);
        CHECK(st.tracks.empty());
        for (const auto& t : st.tracks) { CHECK_EQ(t.infoState, 0u); }
        CHECK(st.images.empty());
    }
    // HANDED OVER: the known track is enriched, the other is "the source does not have it" (2), and the pictures' headers are listed.
    Access::hand(a);
    Access::look(a);
    {
        const cascade::net::RadioStatus st = Access::webStatus(a);
        int known = 0;
        int missing = 0;
        for (const auto& t : st.tracks) {
            if (t.id == g_infoKnownId) {
                ++known;
                CHECK_EQ(t.infoState, 1u);
                CHECK(t.registration == "G-TEST");
                CHECK(t.acType == "Airbus A319-111");
                CHECK(t.acOperator == "Test Air");
                CHECK(t.acCountry == "Testland");
            } else {
                ++missing;
                CHECK_EQ(t.infoState, 2u);
                CHECK(t.registration.empty());
            }
        }
        CHECK_EQ(known, 1);
        CHECK(missing >= 1);
        CHECK_EQ(st.images.size(), Access::images(a).size());
        if (!st.images.empty() && !Access::images(a).empty()) {
            CHECK(st.images[0].plugin == Access::images(a)[0].plugin);
            CHECK_EQ(st.images[0].width, Access::images(a)[0].width);
            CHECK_EQ(st.images[0].revision, Access::images(a)[0].revision);
        }
        std::printf("      %d track enriched, %d answered missing, %zu picture header(s) listed\n", known, missing,
                    st.images.size());
    }
    // THE TRACK-INFO PLUGIN'S STATUS LINE: polled on this thread and sent; it lands in the Decoder output backlog, counted.
    const std::uint64_t total0 = Access::state(a).decoderLinesTotal;
    Access::sendTrackInfoText(a);
    passes(a, 2);
    CHECK_EQ(Access::state(a).decoderLinesTotal, total0 + 1u);
    const auto& lines = Access::decoderLines(a);
    CHECK(!lines.lines.empty());
    if (!lines.lines.empty()) {
        CHECK(lines.lines.back().plugin == "Aircraft info");
        CHECK(lines.lines.back().text == "cannot reach the registry");
    }
    // ...once: the plugin said it once, and polling again sends nothing.
    Access::sendTrackInfoText(a);
    passes(a, 2);
    CHECK_EQ(Access::state(a).decoderLinesTotal, total0 + 1u);
    // DETACHED: no enrichment is handed over any more.
    Access::attachTrackInfo(a, nullptr);
    Access::hand(a);
    Access::look(a);
    for (const auto& t : Access::webStatus(a).tracks) { CHECK_EQ(t.infoState, 0u); }
}

// ======================================================================================================================
// F3. THE SPECTRUM'S RANGE FOLLOWS THE STATE (the pump move, step P2: the engine's onDisplayRange hook is gone)
// ======================================================================================================================
void testDisplayRange(AppWindow& a) {
    std::printf("  F3: the spectrum's axis follows the display range the state carries\n");
    send(a, cmd::makeNum(FOXAPI_OP_SET_DISPLAY_RANGE, -83.0, -17.0));
    CHECK_EQ(Access::spectrum(a).dbMin(), -83.0f);
    CHECK_EQ(Access::spectrum(a).dbMax(), -17.0f);
    send(a, cmd::makeNum(FOXAPP_OP_SET_DISPLAY_MIN, -90.0));
    CHECK_EQ(Access::spectrum(a).dbMin(), -90.0f);
    CHECK_EQ(Access::spectrum(a).dbMax(), -17.0f);
    // a span below the minimum is pushed back by the engine (kMinDbSpan), and the axis shows what the ENGINE took
    send(a, cmd::makeNum(FOXAPP_OP_SET_DISPLAY_MAX, -89.999));
    CHECK(Access::spectrum(a).dbMax() > Access::spectrum(a).dbMin());
    CHECK_EQ(static_cast<double>(Access::spectrum(a).dbMax()), Access::state(a).dbMax);
    send(a, cmd::makeNum(FOXAPI_OP_SET_DISPLAY_RANGE, -100.0, 0.0));
    CHECK_EQ(Access::spectrum(a).dbMin(), -100.0f);
    CHECK_EQ(Access::spectrum(a).dbMax(), 0.0f);
}

// ======================================================================================================================
// F. THE RECEIVER'S OWN PICTURES, AND "SAVE AS BMP"
// ======================================================================================================================
void testImages(AppWindow& a) {
    std::printf("  F: the receiver plugins' pictures through hostPollImages, and the recorder's folder at the click\n");
    drawPlugins(a);
    // The decoder instances the runner built for the loaded modules (the picture decoder is one; so is the text decoder).
    CHECK(Access::activeDecoders(a) >= 1u);
    CHECK(Access::images(a).empty() || Access::images(a)[0].width == 0u);

    // THE PICTURES THE FIXTURE OFFERS: one for the audio the runner has been handed since the last poll (RGB24 8x6 first,
    // GRAY8 5x3 next), so feeding audio and drawing a frame is what brings one into the window's buffer.
    Access::feedAudio(a, 2400);
    drawPlugins(a);
    CHECK_EQ(Access::images(a).size(), std::size_t(1));
    if (Access::images(a).empty()) { return; }
    const cascade::core::HostImage first = Access::images(a)[0];
    CHECK(first.plugin == kImage);
    CHECK_EQ(first.sequence, std::uint64_t(1));
    CHECK_EQ(first.width, 8u);
    CHECK_EQ(first.height, 6u);
    CHECK(first.format == CASCADE_IMAGE_RGB24);
    CHECK(!first.complete);
    CHECK_EQ(first.pixels.size(), std::size_t(144));
    CHECK_EQ(first.pixels[0], std::uint8_t((1 * 13) & 0xFF));
    // No new audio, no new picture: the buffer is not rewritten (the same revision).
    drawPlugins(a);
    CHECK_EQ(Access::images(a)[0].revision, first.revision);
    Access::feedAudio(a, 2400);
    drawPlugins(a);
    CHECK_EQ(Access::images(a)[0].sequence, std::uint64_t(2));
    CHECK_EQ(Access::images(a)[0].width, 5u);
    CHECK(Access::images(a)[0].format == CASCADE_IMAGE_GRAY8);
    CHECK(Access::images(a)[0].revision != first.revision);
    std::printf("      two feeds, two pictures: 8x6 RGB24 then 5x3 GRAY8, the buffer rewritten only on a new one\n");

    // THE PICTURE WINDOW AND "SAVE AS BMP". The window is drawn with the texture's revision already current (no GL context
    // exists here, and the upload is the only GL the window makes), and the key is found in the window and pressed: the
    // BMP lands in the folder fact.recorder.dir names, and the note says where.
    const std::string dir = Access::fact(a, "fact.recorder.dir");
    CHECK(!dir.empty());
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("Group_Thirteen_Fixture-", 0) == 0) { std::filesystem::remove(e.path(), ec); }
    }
    drawPlugins(a);  // sizes the texture slots
    CHECK(!Access::imageTexRev(a).empty());
    if (Access::imageTexRev(a).empty()) { return; }
    Access::imageTexRev(a)[0] = Access::images(a)[0].revision;
    Access::windows(a).show(kImageId);
    drawPlugins(a);
    // The picture window opens in a slot to the right of the main window, off this display: bring it in, where the pointer can
    // reach its keys.
    ImGui::SetWindowPos(kImageId.c_str(), ImVec2(100.0f, 100.0f), ImGuiCond_Always);
    CHECK(Access::imageSaveNote(a).empty());
    const bool clicked = clickInWindow([&] { Access::drawPluginWindows(a); }, kImageId, cascade::i18n::trId("Save as BMP"));
    CHECK(clicked);
    int bmps = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("Group_Thirteen_Fixture-", 0) == 0 && e.path().extension() == ".bmp") { ++bmps; }
    }
    CHECK(bmps >= 1);
    CHECK(has(Access::imageSaveNote(a), "Saved"));
    CHECK(has(Access::imageSaveNote(a), dir));
    std::printf("      Save as BMP wrote %d file(s) in %s\n", bmps, dir.c_str());
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("Group_Thirteen_Fixture-", 0) == 0) { std::filesystem::remove(e.path(), ec); }
    }
    Access::windows(a).hide(kImageId);
}

// ======================================================================================================================
// H. TWO TRACK SOURCES
// ======================================================================================================================
void testTwoSources() {
    std::printf("  H: two track sources: one page each, each page its own module's targets, a page pruned beside a live one\n");
    // A directory of its own with the two track sources only (the host scans at construction).
    const std::filesystem::path dir2 = g_scratch / "plugins2";
    std::error_code ec;
    std::filesystem::create_directories(dir2, ec);
    for (const char* path : {FIXTURE_GROUP14_PLUGIN, FIXTURE_GROUP14B_PLUGIN}) {
        const std::filesystem::path fixture(path);
        std::filesystem::copy_file(fixture, dir2 / fixture.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    const std::string saved = g_pluginDir;
    g_pluginDir = dir2.string();
    Env env;
    AppWindow& a = env.a();
    g_pluginDir = saved;

    CHECK_EQ(Access::trackNames(a).size(), std::size_t(2));
    drawPlugins(a);
    CHECK_EQ(Access::tracks(a).size(), std::size_t(8));  // four from each
    CHECK_EQ(Access::paths(a).size(), std::size_t(2));
    CHECK_EQ(Access::panels(a).size(), std::size_t(2));
    CHECK_EQ(Access::instruments(a).size(), std::size_t(2));
    CHECK_EQ(Access::pages(a).size(), std::size_t(2));
    std::size_t withA = 0, withB = 0;
    for (const cascade::core::HostTrack& t : Access::tracks(a)) {
        if (t.plugin == kTracks && std::string(t.t.id).rfind("G14A", 0) == 0) { ++withA; }
        if (t.plugin == kTracksB && std::string(t.t.id).rfind("G14BA", 0) == 0) { ++withB; }
    }
    CHECK_EQ(withA, std::size_t(3));  // G14A1..3 (the stale G14OLD does not start "G14A")
    CHECK_EQ(withB, std::size_t(3));
    // THE PER-PAGE FILTER: each page counts its OWN module's three shown targets (not six), and the scratch the frame filled
    // last holds only the last page's own four tracks and one path.
    for (const auto& pg : Access::pages(a)) {
        CHECK_EQ(pg.visibleCount, std::size_t(3));
        CHECK(pg.plugin == kTracks || pg.plugin == kTracksB);
    }
    if (Access::pages(a).size() < 2) { CHECK(!"two pages"); return; }
    CHECK(Access::pages(a)[0].plugin != Access::pages(a)[1].plugin);
    const std::string last = Access::pages(a).back().plugin;
    CHECK_EQ(Access::pageTracks(a).size(), std::size_t(4));
    for (const cascade::core::HostTrack& t : Access::pageTracks(a)) { CHECK(t.plugin == last); }
    CHECK_EQ(Access::pagePaths(a).size(), std::size_t(1));
    CHECK(Access::pagePaths(a)[0].plugin == last);
    std::printf("      2 pages, 8 tracks, each page 3 visible targets of its own module\n");

    // THE PRESET PRESS opens ITS module's page only.
    const auto b = loadedNamed(a, kTracksB);
    CHECK(b != nullptr);
    if (b == nullptr) { return; }
    for (auto& pg : Access::pages(a)) { pg.open = false; }
    Access::openFor(a, *b);
    int open = 0;
    for (const auto& pg : Access::pages(a)) {
        if (pg.open) {
            ++open;
            CHECK(pg.plugin == kTracksB);
        }
    }
    CHECK_EQ(open, 1);
    CHECK(Access::windows(a).shown(kPanelIdB));
    CHECK(!Access::windows(a).shown(kPanelId));

    // THE SECOND MODULE STOPPED: its page is pruned (its saved geometry folded first) and the first module's page stays.
    send(a, cmd::makeText(FOXAPP_OP_DECODER_STOP_LIST, std::filesystem::path(FIXTURE_GROUP14B_PLUGIN).filename().string(), 0));
    passes(a, 3);
    drawPlugins(a);
    CHECK_EQ(Access::trackNames(a).size(), std::size_t(1));
    CHECK_EQ(Access::pages(a).size(), std::size_t(1));
    if (Access::pages(a).empty()) { CHECK(!"no map page"); return; }
    CHECK(Access::pages(a)[0].plugin == kTracks);
    CHECK_EQ(Access::tracks(a).size(), std::size_t(4));
    std::printf("      the press opened only the second module's page; stopping it pruned that page and left the first\n");
}

// ======================================================================================================================
// G. AN EMPTY PLUGIN DIRECTORY
// ======================================================================================================================
void testEmpty() {
    std::printf("  G: with no module fitted the frame draws nothing and the accessors are empty\n");
    Env env;
    AppWindow& a = env.a();
    drawPlugins(a, true);
    CHECK(Access::tracks(a).empty());
    CHECK(Access::paths(a).empty());
    CHECK(Access::panels(a).empty());
    CHECK(Access::instruments(a).empty());
    CHECK(Access::trackNames(a).empty());
    CHECK(Access::pages(a).empty());
    CHECK(Access::images(a).empty());
    CHECK(Access::coverage(a).empty());
    std::vector<cascade::core::HostImage> out;
    Access::pollImages(a, out);
    CHECK(out.empty());
    CHECK_EQ(Access::windows(a).count(), std::size_t(0));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_pluginwin_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch / "plugins");
    g_pluginDir = (g_scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());
    Access::install();

    // G first: the plugin directory is empty.
    testEmpty();

    // The three modules go into the directory BEFORE the next window is made (the host scans at construction).
    std::error_code ec;
    for (const char* path : {FIXTURE_GROUP14_PLUGIN, FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP13_PLUGIN}) {
        const std::filesystem::path fixture(path);
        std::filesystem::copy_file(fixture, std::filesystem::path(g_pluginDir) / fixture.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    {
        Env env;
        testFeed(env.a());
    }
    {
        Env env;
        AppWindow& a = env.a();
        testFrame(a);
        testCoverage(a);
        testPages(a);
        testOpenFor(a);
        testPresetEvent(a);
        testImages(a);
        testWebExtras(a);
        testDisplayRange(a);
    }
    testTwoSources();

    ImGui::DestroyContext();
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_plugin_window");
}
