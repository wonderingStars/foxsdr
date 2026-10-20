// test_map_window.cpp - THE RECEIVER'S POSITION AND THE MAP READ THE WINDOW'S ENGINE API SESSION (window port, group 14,
// part a: ensureMapPage, drawReceiverPositionOffers, drawRxPositionEntry, drawSatelliteMapBody, drawMapPageSections,
// drawPluginWindowRows, drawTrackList, findVisibleTrack, drawTargetDetailsSection, drawTargetDetailsWindow and the follow
// that replaced the engine's onReceiverPositionApplied hook; API 0.3 section 8.11.1 - no new API).
//
// WHAT THEY DRAW FROM. The position is the state's (FOXAPI_RX_POSITION_SET, rxLatDeg, rxLonDeg), read once a frame; the
// targets are the HOST PROCESS's - the plugin windows' own poll of the loaded track sources, reached through hostTracks()
// and its siblings - and this file is the first to put real ones in a hermetic frame: tests/fixture_group14_plugin.cpp is a
// REAL track source (a loaded module, polled through the plugin ABI) reporting a fixed set. The commands the members send
// are FOXAPI_OP_SET_POSITION and FOXAPP_OP_PLUGIN_AUTO_PRESET through the session.
//
//   A  with no track source: the rail's "no track source" block (the census from the table's PLUGINS rows)
//   B  the fixture: the poll, the page the plugin gets, its count of visible targets (the stale one is not), its path,
//      the position at the page's birth, the altitude lookup
//   C  THE POSITION FOLLOW (it replaced a hook the engine called from its own pass): the typed fields, every map page, every
//      Patch Map part and the coverage follow a position that MOVED; a first position seen at the first look; and
//      THE WEDGE CLEAR - pressing "Set RX here" (or either offer, or a click on the map) with the pair already in force
//      still starts the coverage again, because that is the window's own effect, while the same re-apply from anywhere
//      else (a session of another kind) cannot be seen by the window and does not
//   D  the offers (the middle of the traffic heard, from the real aircraft) and the typed entry, and what each sends
//   E  the target register and the details of one target (the position the distances are measured from; the stale target)
//   F  the rail's rows: the page's row with its count, and the click that sends the auto-preset for the module FILE
//
// THE ACCESSOR TRAP: a ported member reads the SESSION, so each frame here looks first (the engine's pass takes the
// session's queue, publishes, the window copies the status lines and reads its state and follows the position), draws, and
// lets the NEXT look take what the draw sent.
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
    using Sent = std::vector<cmd::QueuedCommand>;
    using Engine = cascade::engine::Engine;

    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
    }

    // ---- one frame's engine part, in drawUi's order ----------------------------------------------------------------------
    static void look(AppWindow& a) {
        passseam::pass(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            a.engine_.pumpInputs();
            a.engine_.pumpPlugins();
            (void)a.engine_.pumpPublish();
        });
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
    }
    static void after(AppWindow& a) {
        passseam::workers(a.engine_, [&] {
            a.engine_.pumpWorkers();
            a.engine_.pumpAudio();
        });
    }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }

    // ---- the members ---------------------------------------------------------------------------------------------------------
    static void drawPluginWindows(AppWindow& a) { a.drawPluginWindows(); }
    static void drawOffers(AppWindow& a) { a.drawReceiverPositionOffers(); }
    static void drawEntry(AppWindow& a) { a.drawRxPositionEntry(); }
    static void drawMapRows(AppWindow& a) { a.drawMapPageSections(); }
    static void drawWindowRows(AppWindow& a) { a.drawPluginWindowRows(); }
    static void drawTargetList(AppWindow& a, AppWindow::MapPage& page) { a.drawTrackList(page, a.hostTracks()); }
    static void drawDetails(AppWindow& a) { a.drawTargetDetailsSection(); }
    static std::string& detailsId(AppWindow& a) { return a.detailsTrackId_; }
    static const cascade::core::HostTrack* findVisible(AppWindow& a, const std::string& id) { return a.findVisibleTrack(id); }
    static AppWindow::MapPage& ensurePage(AppWindow& a, const std::string& name) { return a.ensureMapPage(name); }
    static std::vector<AppWindow::MapPage>& pages(AppWindow& a) { return a.mapPages_; }
    static const std::vector<cascade::core::HostTrack>& tracks(AppWindow& a) { return a.hostTracks(); }
    static const std::vector<cascade::core::HostPath>& paths(AppWindow& a) { return a.hostPaths(); }
    static const std::vector<std::string>& trackNames(AppWindow& a) { return a.pluginUi_.trackPluginNames(); }
    static const std::vector<cascade::core::HostTrack>& pageTracks(AppWindow& a) { return a.pageTracks_; }
    static const std::vector<cascade::core::HostPath>& pagePaths(AppWindow& a) { return a.pagePaths_; }
    static bool altitudeNear(AppWindow& a, const std::string& plugin, const std::string& id, double lat, double lon,
                             double& alt) {
        return a.hostAltitudeNear(plugin, id, lat, lon, alt);
    }
    static CoverageMap& coverage(AppWindow& a) { return a.coverage_; }
    static double& typedLat(AppWindow& a) { return a.rxLatInput_; }
    static double& typedLon(AppWindow& a) { return a.rxLonInput_; }
    static std::map<cascade::core::patch::NodeId, std::unique_ptr<MapView>>& patchViews(AppWindow& a) { return a.patchMapViews_; }
    static const ScopeView& scope(AppWindow& a) { return a.scope_; }
    static bool coverageResetPending(AppWindow& a) { return a.coverageResetPending_; }
    static void press(AppWindow& a, double lat, double lon) { a.submitReceiverPosition(lat, lon); }
    static const AppWindow::PluginListCache& pluginRows(AppWindow& a) { return a.pluginLists(); }
    static std::string moduleFile(AppWindow& a, const std::string& name) {
        return cascade::gui::moduleFileForName(a.pluginLists().plugins, name);
    }
    // The engine's own fields, for what the engine says and the window must agree with.
    static bool engineHasPosition(AppWindow& a) { return a.engine_.rxSet_; }
    static double engineLat(AppWindow& a) { return a.engine_.rxLat_; }
    static double engineLon(AppWindow& a) { return a.engine_.rxLon_; }
    static void engineSetPosition(AppWindow& a, double lat, double lon) { (void)a.engine_.applyReceiverPosition(lat, lon); }
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
const char* const kWin = "mapwin";
const char* const kPlugin = "Group Fourteen Tracks";
std::string g_text;

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
        if (std::getenv("MAPWIN_DUMP") != nullptr) { std::printf("---- frame text ----\n%s\n", g_text.c_str()); }
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

// Every command the window sends, in order, while this object lives.
struct Tap {
    AppWindow& a;
    Access::Sent log;
    explicit Tap(AppWindow& w) : a(w) { Access::tap(a, &log); }
    ~Tap() { Access::tap(a, nullptr); }
    int count(std::uint32_t op) const {
        int n = 0;
        for (const cmd::QueuedCommand& q : log) { n += q.c.op == op ? 1 : 0; }
        return n;
    }
    const cmd::QueuedCommand* last(std::uint32_t op) const {
        for (auto it = log.rbegin(); it != log.rend(); ++it) {
            if (it->c.op == op) { return &*it; }
        }
        return nullptr;
    }
};

// A click on the item `label` names, found by sweeping the pointer over the window while `body` is drawn (ImGui finds what
// is under the pointer a frame after it moves). No engine pass is run: the command goes where uiSubmit sends it.
template <class F>
bool clickItem(AppWindow& a, F body, const char* label, bool switchRow = false) {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiID target = 0;
    imguiFrame([&] {
        body();
        if (switchRow) {
            // benchSwitchRow: PushID(label), then an InvisibleButton("##switchrow") the whole width of the rail.
            ImGui::PushID(label);
            target = ImGui::GetID("##switchrow");
            ImGui::PopID();
        } else {
            target = ImGui::GetID(label);
        }
    });
    bool found = false;
    float hx = 0.0f, hy = 0.0f;
    for (float y = 2.0f; y < 260.0f && !found; y += 4.0f) {
        for (float x = 8.0f; x < 700.0f && !found; x += 20.0f) {
            io.AddMousePosEvent(x, y);
            imguiFrame(body);
            imguiFrame(body);
            if (GImGui->HoveredId == target) {
                found = true;
                hx = x;
                hy = y;
            }
        }
    }
    if (!found) {
        io.AddMousePosEvent(2.0f, 2.0f);
        imguiFrame(body);
        return false;
    }
    io.AddMousePosEvent(hx, hy);
    imguiFrame(body);
    io.AddMouseButtonEvent(0, true);
    imguiFrame(body);
    io.AddMouseButtonEvent(0, false);
    imguiFrame(body);
    io.AddMousePosEvent(2.0f, 2.0f);
    imguiFrame(body);
    imguiFrame(body);
    (void)a;
    return true;
}

// The window, built over the plugin directory as it is now.
struct Env {
    std::unique_ptr<AppWindow> app;
    Env() {
        app = std::make_unique<AppWindow>();
        passes(*app, 3);
    }
    AppWindow& a() { return *app; }
};

// The plugin windows drawn once (the poll, the page filter, the coverage feed).
void drawPlugins(AppWindow& a) {
    frame(a, [&] { Access::drawPluginWindows(a); });
}

// ======================================================================================================================
// A. NO TRACK SOURCE
// ======================================================================================================================
void testNoTrackSource() {
    std::printf("  A: with no track source the rail says so, from the table's PLUGINS rows\n");
    Env env;
    AppWindow& a = env.a();
    frame(a, [&] { Access::drawMapRows(a); }, true);
    CHECK(has(g_text, "No fitted module publishes tracks of any kind"));  // the census: nothing fitted that is a track source
    CHECK(Access::pages(a).empty());
    CHECK(Access::tracks(a).empty());
    CHECK(Access::trackNames(a).empty());
    CHECK(Access::moduleFile(a, kPlugin).empty());
    CHECK(Access::moduleFile(a, "").empty());
    std::printf("      no page, no track, no module file for a name nobody has\n");
}

// ======================================================================================================================
// B. THE FIXTURE
// ======================================================================================================================
void testFixture(AppWindow& a) {
    std::printf("  B: the fixture's targets, its page, its count, its path, the page's birth\n");
    CHECK(Access::tracks(a).empty());  // nothing is polled before the plugin windows draw
    drawPlugins(a);
    CHECK_EQ(Access::tracks(a).size(), std::size_t(4));
    CHECK_EQ(Access::paths(a).size(), std::size_t(1));
    CHECK_EQ(Access::trackNames(a).size(), std::size_t(1));
    CHECK(Access::trackNames(a)[0] == kPlugin);
    CHECK_EQ(Access::pages(a).size(), std::size_t(1));
    auto& page = Access::pages(a)[0];
    CHECK(page.plugin == kPlugin);
    CHECK_EQ(page.visibleCount, std::size_t(3));  // the stale one (120 s) is not shown
    CHECK(!page.open);                            // a page never opens itself
    CHECK_EQ(Access::pageTracks(a).size(), std::size_t(4));
    CHECK_EQ(Access::pagePaths(a).size(), std::size_t(1));
    CHECK(Access::pagePaths(a)[0].plugin == kPlugin);
    CHECK_EQ(Access::pagePaths(a)[0].points.size(), std::size_t(3));
    // No position was set when the page was born, so the view has no home.
    CHECK(!page.view->hasHome());
    std::printf("      4 tracks polled, 1 page, 3 visible, 1 path, no home before a position\n");

    // The altitude lookup the page's view was given at birth goes to the plugin UI's observations.
    double alt = 0.0;
    CHECK(!Access::altitudeNear(a, kPlugin, "NOPE", 51.5, -0.1, alt));
    // (the poll records an observation for a track that reports an altitude: G14A3 is at 9000 m)
    CHECK(Access::altitudeNear(a, kPlugin, "G14A3", 51.50, -0.10, alt));
    CHECK_NEAR(alt, 9000.0, 1.0);
    std::printf("      the altitude near a track's own position: %.0f m\n", alt);

    // The module file for the page's name is what the table's PLUGINS row names, and it is the one file the fixture is.
    const std::string file = Access::moduleFile(a, kPlugin);
    CHECK(!file.empty());
    CHECK(file == std::filesystem::path(FIXTURE_GROUP14_PLUGIN).filename().string());
    std::printf("      the display name's module file: %s\n", file.c_str());
}

// A second page, born AFTER a position is set, has the position at birth.
void testBirthWithPosition(AppWindow& a) {
    std::printf("  B2: a page made after the position is set has it at birth (no read of the engine)\n");
    CHECK(positionSet(a));
    auto& other = Access::ensurePage(a, "Another Source");
    CHECK(other.view->hasHome());
    CHECK_NEAR(other.view->homeLatDeg(), Access::state(a).rxLatDeg, 1e-12);
    CHECK_NEAR(other.view->homeLonDeg(), Access::state(a).rxLonDeg, 1e-12);
    // (the same name finds the same page)
    CHECK(&Access::ensurePage(a, "Another Source") == &Access::pages(a).back());
    Access::pages(a).pop_back();
}

// ======================================================================================================================
// C. THE POSITION FOLLOW
// ======================================================================================================================
void testFollow(AppWindow& a) {
    std::printf("  C: the position follow: typed fields, every page, every Patch Map part, the scope, the coverage\n");
    auto& page = Access::pages(a)[0];
    // A Patch Map part's own view (made by the page as the node appears).
    auto& pv = Access::patchViews(a);
    pv.clear();
    pv[1] = std::make_unique<cascade::gui::MapView>();
    CHECK(!pv[1]->hasHome());

    // No position yet: nothing to follow.
    CHECK(!positionSet(a));
    CHECK(!page.view->hasHome());

    // THE FIRST POSITION: from "unset" it is a change, so the pages, the Patch Map part and the fields get it at the look.
    setPosition(a, 51.47, -0.45);
    CHECK(positionSet(a));
    CHECK_NEAR(Access::state(a).rxLatDeg, 51.47, 1e-12);
    CHECK(page.view->hasHome());
    CHECK_NEAR(page.view->homeLatDeg(), 51.47, 1e-12);
    CHECK_NEAR(page.view->homeLonDeg(), -0.45, 1e-12);
    CHECK(pv[1]->hasHome());
    CHECK_NEAR(pv[1]->homeLatDeg(), 51.47, 1e-12);
    CHECK_NEAR(Access::typedLat(a), 51.47, 1e-12);
    CHECK_NEAR(Access::typedLon(a), -0.45, 1e-12);
    CHECK_NEAR(Access::scope(a).receiverLatDeg(), 51.47, 1e-12);
    std::printf("      first position: page, Patch Map part, typed fields and scope all at 51.47 -0.45\n");

    // THE COVERAGE: fed by the plugin windows' frame from the real targets, and gone when the position MOVES.
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    const int wedges = Access::coverage(a).filledBuckets();
    CHECK(wedges >= 1);
    setPosition(a, 51.60, -0.20);
    CHECK(Access::coverage(a).empty());
    CHECK_NEAR(page.view->homeLatDeg(), 51.60, 1e-12);
    CHECK_NEAR(pv[1]->homeLonDeg(), -0.20, 1e-12);
    CHECK_NEAR(Access::typedLon(a), -0.20, 1e-12);
    std::printf("      a moved position: %d wedge(s) cleared, every view at 51.60 -0.20\n", wedges);

    // THE ENGINE AND THE WINDOW AGREE (the window reads the state, which is the engine's field a pass old).
    CHECK(Access::engineHasPosition(a));
    CHECK_NEAR(Access::engineLat(a), Access::state(a).rxLatDeg, 1e-12);

    // A position the engine REFUSES moves nothing (out of range, and 0,0).
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    setPosition(a, 91.0, 0.0);
    setPosition(a, 0.0, 0.0);
    CHECK_NEAR(Access::state(a).rxLatDeg, 51.60, 1e-12);
    CHECK(!Access::coverage(a).empty());
    CHECK_NEAR(page.view->homeLatDeg(), 51.60, 1e-12);
    std::printf("      a refused position (91, and 0 0) leaves the views and the coverage alone\n");

    // A MOVE OF ONE COORDINATE ALONE is a move: the longitude, then the latitude (each followed, each clearing the coverage).
    setPosition(a, 51.60, -0.25);
    CHECK_NEAR(page.view->homeLonDeg(), -0.25, 1e-12);
    CHECK_NEAR(Access::typedLon(a), -0.25, 1e-12);
    CHECK(Access::coverage(a).empty());
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    setPosition(a, 51.65, -0.25);
    CHECK_NEAR(page.view->homeLatDeg(), 51.65, 1e-12);
    CHECK_NEAR(Access::typedLat(a), 51.65, 1e-12);
    CHECK(Access::coverage(a).empty());
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    setPosition(a, 51.60, -0.20);  // (back to where the next checks stand)
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    std::printf("      a move of the longitude alone, and of the latitude alone, is followed and clears the coverage\n");

    // THE SAME PAIR FROM ANOTHER SESSION'S KIND (a direct submit, as a remote client's): the state does not change, so the
    // window sees nothing and the coverage stays. It is the one difference from the hook, which cleared on every apply.
    send(a, cmd::makeNum(FOXAPI_OP_SET_POSITION, 51.60, -0.20));
    CHECK(!Access::coverage(a).empty());
    std::printf("      the same pair from a session that is not the window's own press: the coverage stays (stated difference)\n");
}

void testWedgeClear(AppWindow& a) {
    std::printf("  C2: THE WEDGE CLEAR - a press with the pair already in force still starts the coverage again\n");
    setPosition(a, 51.50, -0.30);
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    // The typed fields hold the pair in force (the follow put them there).
    CHECK_NEAR(Access::typedLat(a), 51.50, 1e-12);
    CHECK_NEAR(Access::typedLon(a), -0.30, 1e-12);

    {
        // (1) "Set RX here", the typed entry, pressed for real: a click on the button.
        Tap tap(a);
        CHECK(clickItem(a, [&] { Access::drawEntry(a); }, "Set RX here"));
        CHECK_EQ(tap.count(FOXAPI_OP_SET_POSITION), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPI_OP_SET_POSITION);
        CHECK(q != nullptr);
        if (q != nullptr) {
            CHECK_NEAR(q->c.num[0], 51.50, 1e-12);
            CHECK_NEAR(q->c.num[1], -0.30, 1e-12);
        }
        CHECK(Access::coverageResetPending(a));
        CHECK(!Access::coverage(a).empty());  // nothing is cleared until the window's next look
        pass(a);
        CHECK(Access::coverage(a).empty());
        CHECK(!Access::coverageResetPending(a));
        std::printf("      the typed entry: SET_POSITION 51.5 -0.3 sent, the coverage clear at the next look (same numbers)\n");
    }
    // (2) the same press with DIFFERENT numbers: cleared, and the views move.
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    Access::typedLat(a) = 51.55;
    Access::typedLon(a) = -0.35;
    {
        Tap tap(a);
        CHECK(clickItem(a, [&] { Access::drawEntry(a); }, "Set RX here"));
        CHECK_EQ(tap.count(FOXAPI_OP_SET_POSITION), 1);
    }
    passes(a, 2);
    CHECK(Access::coverage(a).empty());
    CHECK_NEAR(Access::pages(a)[0].view->homeLatDeg(), 51.55, 1e-12);
    std::printf("      different numbers: cleared, the page's home moved\n");

    // (3) a pair the window's rule refuses: the button is disabled (no command), and the helper sends it but notes nothing.
    drawPlugins(a);
    CHECK(!Access::coverage(a).empty());
    Access::typedLat(a) = 0.0;
    Access::typedLon(a) = 0.0;
    {
        Tap tap(a);
        (void)clickItem(a, [&] { Access::drawEntry(a); }, "Set RX here");  // disabled: a press does nothing
        CHECK_EQ(tap.count(FOXAPI_OP_SET_POSITION), 0);
        Access::press(a, 95.0, 10.0);  // what a caller that skipped the check would send
        CHECK_EQ(tap.count(FOXAPI_OP_SET_POSITION), 1);
        CHECK(!Access::coverageResetPending(a));
    }
    passes(a, 2);
    CHECK(!Access::coverage(a).empty());
    CHECK_NEAR(Access::state(a).rxLatDeg, 51.55, 1e-12);
    std::printf("      an unacceptable pair: no button, and a refused send starts nothing again\n");
}

// ======================================================================================================================
// D. THE OFFERS
// ======================================================================================================================
void testOffers(AppWindow& a) {
    std::printf("  D: the offers: the middle of the traffic heard (the real aircraft), and the map's centre\n");
    drawPlugins(a);
    // THE OFFER COUNTS EVERY AIRCRAFT THE SOURCE REPORTS, the stale one too (scopeTrafficCentre takes the tracks as they
    // are; it never asked the staleness rule - that is how it was, and this part does not change it): four heard, and
    // their middle is 51.125 N and 0 E (the three live ones' alone would be 51.5 N and -1/3 W).
    frame(a, [&] { Access::drawOffers(a); }, true);
    CHECK(has(g_text, "Use the middle of the 4 aircraft heard"));
    CHECK(has(g_text, "51.12N"));
    CHECK(has(g_text, "0.00E"));
    {
        Tap tap(a);
        CHECK(clickItem(a, [&] { Access::drawOffers(a); }, "Use the middle of the 4 aircraft heard  (51.12N 0.00E)"));
        CHECK_EQ(tap.count(FOXAPI_OP_SET_POSITION), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPI_OP_SET_POSITION);
        CHECK(q != nullptr);
        if (q != nullptr) {
            CHECK_NEAR(q->c.num[0], 51.125, 1e-9);
            CHECK_NEAR(q->c.num[1], 0.0, 1e-9);
        }
    }
    passes(a, 2);
    CHECK_NEAR(Access::state(a).rxLatDeg, 51.125, 1e-9);
    CHECK_NEAR(Access::state(a).rxLonDeg, 0.0, 1e-9);
    CHECK_NEAR(Access::typedLat(a), 51.125, 1e-9);
    std::printf("      the offer sends the mean of the aircraft heard (51.125, 0), the state and the typed fields follow\n");

    // The map's centre: the page's view, which has not been panned, looks at its default.
    frame(a, [&] { Access::drawOffers(a); }, true);
    CHECK(has(g_text, "Use the Group Fourteen Tracks map's centre"));
    {
        Tap tap(a);
        CHECK(clickItem(a, [&] { Access::drawOffers(a); }, "Use the Group Fourteen Tracks map's centre  (54.00N 2.00W)"));
        const cmd::QueuedCommand* q = tap.last(FOXAPI_OP_SET_POSITION);
        CHECK(q != nullptr);
        if (q != nullptr) {
            CHECK_NEAR(q->c.num[0], 54.0, 1e-9);
            CHECK_NEAR(q->c.num[1], -2.0, 1e-9);
        }
    }
    passes(a, 2);
    CHECK_NEAR(Access::state(a).rxLatDeg, 54.0, 1e-9);
    CHECK_NEAR(Access::state(a).rxLonDeg, -2.0, 1e-9);
    std::printf("      the map's centre (54, -2) is offered and sent\n");
    setPosition(a, 51.50, -0.30);  // back to where the rest of the file measures from
}

// ======================================================================================================================
// E. THE REGISTER AND THE DETAILS
// ======================================================================================================================
void testRegisterAndDetails(AppWindow& a) {
    std::printf("  E: the target register and the details of one target\n");
    auto& page = Access::pages(a)[0];
    drawPlugins(a);

    // The position is known: no "need the RX position" note.
    CHECK(positionSet(a));
    frame(a, [&] { Access::drawTargetList(a, page); }, true);
    CHECK(has(g_text, "F-G14A1"));
    CHECK(has(g_text, "F-G14A2"));
    CHECK(has(g_text, "F-G14A3"));
    CHECK(!has(g_text, "F-G14OLD"));  // stale: never listed
    CHECK(!has(g_text, "need the RX position"));
    std::printf("      three targets listed, the stale one not, no note about the position\n");

    // The details: the id is found among the shown targets, by the staleness rule.
    CHECK(Access::findVisible(a, "G14A2") != nullptr);
    CHECK(Access::findVisible(a, "G14OLD") == nullptr);
    CHECK(Access::findVisible(a, "NOPE") == nullptr);
    Access::detailsId(a) = "G14A2";
    frame(a, [&] { Access::drawDetails(a); }, true);
    const bool listed = has(g_text, "G14A2");
    CHECK(listed);
    CHECK(!has(g_text, "No longer being reported"));
    // What the details letter, from the track itself and the position the distance is measured from (51.5 N, -0.3 W):
    // the aircraft is at 51.40 N, -0.30 W, so 11.1 km south of the receiver.
    CHECK(has(g_text, "51.4"));
    Access::detailsId(a) = "G14OLD";
    frame(a, [&] { Access::drawDetails(a); }, true);
    CHECK(has(g_text, "No longer being reported"));
    Access::detailsId(a) = "NOPE";
    frame(a, [&] { Access::drawDetails(a); }, true);
    CHECK(has(g_text, "No longer being reported"));
    Access::detailsId(a).clear();
    frame(a, [&] { Access::drawDetails(a); }, true);
    CHECK(has(g_text, "No target chosen"));
    std::printf("      the details of G14A2 drawn; a stale or unknown id reads \"No longer being reported\"; none chosen says so\n");
}

// WITH NO POSITION: the register says why its distance and bearing are empty.
void testRegisterWithoutPosition() {
    std::printf("  E2: with no position the register says its distance and bearing need one\n");
    Env env;
    AppWindow& a = env.a();
    drawPlugins(a);
    auto& page = Access::pages(a)[0];
    CHECK(!positionSet(a));
    frame(a, [&] { Access::drawTargetList(a, page); }, true);
    CHECK(has(g_text, "F-G14A1"));
    CHECK(has(g_text, "need the RX position"));
    setPosition(a, 51.47, -0.45);
    frame(a, [&] { Access::drawTargetList(a, page); }, true);
    CHECK(!has(g_text, "need the RX position"));
}

// A POSITION RESTORED FROM THE CONFIG (window port group 14, part e). applyConfig used to read the engine's restored position
// (five reads) and push it into the typed fields and every map page's home; the follow does it at the first look, which sees a
// restored position as a change from "unset" - so the restore reads nothing of the engine. This holds the follow to that: a window
// built from a config with a position, with a page and a Patch Map part already there, has the position nowhere in the window
// until the first look, and everywhere the restore used to put it right after.
void testRestoredPosition() {
    std::printf("  E3: a position restored from the config reaches the fields, the pages, the Patch Map part and the scope at the first look\n");
    const std::filesystem::path cfg = g_scratch / "restored.json";
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"rxPositionSet\": true, "
             "\"rxLatDeg\": 51.47, \"rxLonDeg\": -0.45 }\n";
    }
    AppWindow a(cfg.string(), false);
    CHECK(Access::engineHasPosition(a));  // the engine restored it
    CHECK_NEAR(Access::engineLat(a), 51.47, 1e-12);
    // A page and a Patch Map part, as a restored layout holds them, made before anything has looked at the engine.
    auto& page = Access::ensurePage(a, "Group Fourteen Tracks");
    auto& pv = Access::patchViews(a);
    pv.clear();
    pv[1] = std::make_unique<cascade::gui::MapView>();
    CHECK(!positionSet(a));  // nothing published yet
    CHECK(!page.view->hasHome());
    CHECK(!pv[1]->hasHome());
    pass(a);  // the first look
    CHECK(positionSet(a));
    CHECK_NEAR(Access::typedLat(a), 51.47, 1e-12);
    CHECK_NEAR(Access::typedLon(a), -0.45, 1e-12);
    CHECK(page.view->hasHome());
    CHECK_NEAR(page.view->homeLatDeg(), 51.47, 1e-12);
    CHECK_NEAR(page.view->homeLonDeg(), -0.45, 1e-12);
    CHECK(pv[1]->hasHome());
    CHECK_NEAR(pv[1]->homeLatDeg(), 51.47, 1e-12);
    CHECK_NEAR(Access::scope(a).receiverLatDeg(), 51.47, 1e-12);
    // (a page made after it has the position at birth, from the state: B2)
    Access::pages(a).clear();
    pv.clear();
}

// ======================================================================================================================
// F. THE RAIL'S ROWS
// ======================================================================================================================
void testRows(AppWindow& a) {
    std::printf("  F: the page's rail row, and the auto-preset it sends for the module FILE\n");
    drawPlugins(a);
    const char* const row = "Group Fourteen Tracks map###mappage:Group Fourteen Tracks";
    auto& page = Access::pages(a)[0];
    CHECK(!page.open);
    {
        Tap tap(a);
        CHECK(clickItem(a, [&] { Access::drawMapRows(a); }, row, true));
        CHECK(page.open);
        CHECK_EQ(tap.count(FOXAPP_OP_PLUGIN_AUTO_PRESET), 1);
        const cmd::QueuedCommand* q = tap.last(FOXAPP_OP_PLUGIN_AUTO_PRESET);
        CHECK(q != nullptr);
        if (q != nullptr) {
            CHECK(cmd::textOf(q->c, q->longText) == std::filesystem::path(FIXTURE_GROUP14_PLUGIN).filename().string());
            CHECK_EQ(q->c.ival[0], 1);
        }
        std::printf("      the click opened the page and sent PLUGIN_AUTO_PRESET with the module file\n");
        // Closing it sends nothing.
        CHECK(clickItem(a, [&] { Access::drawMapRows(a); }, row, true));
        CHECK(!page.open);
        CHECK_EQ(tap.count(FOXAPP_OP_PLUGIN_AUTO_PRESET), 1);
    }
    // THE RAIL HAS A PAGE, so it has no "no track source" block (that note is the NONE row's).
    frame(a, [&] { Access::drawMapRows(a); }, true);
    CHECK(!has(g_text, "target positions"));
    // The plugin windows' rows: this module has no PICTURE, so no picture row (since group 14 part d it also declares a panel and
    // an instrument, whose windows test_plugin_window draws and opens).
    CHECK(Access::pageTracks(a).size() == 4);
    CHECK(!clickItem(a, [&] { Access::drawWindowRows(a); }, "Group Fourteen Tracks image###imgrow:Group Fourteen Tracks", true));

    // STOPPED BY THE USER: the census is the table's (PLUGINS capability word and STOPPED flag), so the rail names the module
    // and says the operator stopped it - and the page is gone with the module's instance.
    send(a, cmd::makeText(FOXAPP_OP_DECODER_STOP_LIST, std::filesystem::path(FIXTURE_GROUP14_PLUGIN).filename().string(), 0));
    passes(a, 3);
    drawPlugins(a);
    frame(a, [&] { Access::drawMapRows(a); }, true);
    CHECK(has(g_text, "Group Fourteen Tracks"));
    CHECK(has(g_text, "you stopped it"));
    CHECK(Access::pages(a).empty());
    std::printf("      stopped by the operator: no page, and the rail says whose module it was\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_map_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_mapwin_" + std::to_string(pid));
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
    // The rail's sections are closed until pressed; the details section is opened for the frames that read it.
    setEnv("FOXSDR_OPEN_SECTIONS", "Target details");

    // A: the plugin directory is empty.
    testNoTrackSource();

    // The fixture goes into the directory BEFORE the next window is made (the host scans at construction).
    const std::filesystem::path fixture(FIXTURE_GROUP14_PLUGIN);
    std::error_code ec;
    std::filesystem::copy_file(fixture, std::filesystem::path(g_pluginDir) / fixture.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    {
        Env env;
        AppWindow& a = env.a();
        testFixture(a);
        testFollow(a);
        testBirthWithPosition(a);
        testWedgeClear(a);
        testOffers(a);
        testRegisterAndDetails(a);
        testRows(a);
    }
    testRegisterWithoutPosition();
    testRestoredPosition();

    ImGui::DestroyContext();
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_map_window");
}
