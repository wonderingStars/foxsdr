// test_decoders_mute_grants.cpp - THE DECODERS, THE PRESETS, THE MARKS, THE PLUGIN-MUTE CLUSTER AND THE TUNE
// GRANTS READ THEIR ENGINE API SESSION (window port, group 8: drawDecodersSection, drawDecoderStatusRows,
// drawDecoderWindow, drawPluginPresets, drawPresetKeys, drawDecoderPresetBars, drawPluginTuneControls,
// drawPluginMarkers, drawMutePopup, muteBannerSubject, drawMuteBanner and muteStateForDisplayName;
// engine-stage4.md 6.5, API 0.3 section 8.7).
//
// What they draw is the table's lists, facts and one event. This holds the window's side of that against the real
// engine with two REAL fixture modules loaded (tests/fixture_stage1_plugin.cpp, tests/fixture_group8_plugin.cpp), a
// file the host refuses, the signal generator, and fake muting plugins by the engine's own stand-in (muteStates_,
// as the test.muteBy hook sets it):
//   A  the rules the window restates (gui/decoder_reads.hpp), held against the engine's own answer for the same
//      plugins: the presets, the runnable decoders, the runner's status, the census, the marks
//   B  the cache follows the engine: PLUGINS merged with DECODERS, PRESETS and USER_PRESETS, MARKERS, the grants
//   C  D3: a decoder line moves listSeq nowhere - 1,000 of them leave every list cache of the window unread
//   D  the decoder output: only the NEW lines are copied, in order, bounded; Clear
//   E  the plugin-mute cluster: the causes, the event once per tune-away, the dialog that opens on it and is
//      withdrawn, the banner
//   F  what a frame costs once nothing has changed: 1000 reads allocate nothing
//   G  the members DRAWN in real frames send nothing idle, and EVERY widget of each, clicked, sends exactly the
//      commands it should
//   H  each command so sent, replayed in a fresh window through the session: accepted and applied
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "gui/decoder_reads.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // LogToBuffer: what a frame drew, as text
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

// --- allocations on THIS thread (the window and the test share it) ------------
namespace {
thread_local long t_allocs = 0;
}
void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
std::filesystem::path g_plugins;
std::string pluginDirHook() { return g_plugins.string(); }

std::string fileOf(const char* fixture) { return std::filesystem::path(fixture).filename().string(); }

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cascade::core::cmd::QueuedCommand>;
    using Answer = std::pair<std::uint32_t, std::int32_t>;  // {op, status}
    using Engine = cascade::engine::Engine;
    using Plugins = AppWindow::PluginListCache;
    using Lines = AppWindow::DecoderLineCache;

    // The hermetic world: the plugin folder, and no radio, USB walk or SoapySDR probe of the desk's.
    static void installHooks() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirHook;
    }

    // One frame's engine part, in drawUi's order: the session's queue taken, the publish, the status copy, the
    // window's read of the state, the audio mute decided (AFTER the publish: what it decides is published by the
    // NEXT pass), and the key's drain (answers and, since group 8, the session's events).
    static void look(AppWindow& a) {
        a.engine_.pumpFrameBegin();
        a.engine_.pumpInputs();
        a.engine_.pumpPlugins();
        (void)a.engine_.pumpPublish();
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
    }
    static void frameOf(AppWindow& a) {
        look(a);
        a.engine_.pumpAudioMute();
        a.pumpTransmitKey();
    }
    static void frames(AppWindow& a, int n) {
        for (int i = 0; i < n; ++i) { frameOf(a); }
    }
    static std::vector<Answer> answers(AppWindow& a) {
        std::vector<Answer> out;
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            for (int32_t i = 0; i < got; ++i) { out.push_back({r[i].op, r[i].status}); }
            if (got < 16) { break; }
        }
        return out;
    }
    static void submit(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static void dropQueued(AppWindow& a) {
        a.closeUiSession();
        a.openUiSession();
        a.refreshUiState();
    }
    // A command applied at once (what a test needs before it looks).
    static void command(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) { (void)a.engine_.applyCommand(q.c, q.longText); }
    static void command(AppWindow& a, const FoxCommand& c) { (void)a.engine_.applyCommand(c); }

    // The window's state and what it keeps.
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static const Plugins& plugins(AppWindow& a) { return a.pluginLists(); }
    static const Plugins& pluginCache(AppWindow& a) { return a.pluginCache_; }
    static const Lines& lines(AppWindow& a) { return a.decoderLines(); }
    static const Lines& lineCache(AppWindow& a) { return a.decoderLineCache_; }
    static void clearLines(AppWindow& a) { a.clearDecoderLines(); }
    static const AppWindow::RailListCache& rail(AppWindow& a) { return a.railLists(); }
    static const AppWindow::TuningListCache& tuning(AppWindow& a) { return a.tuningLists(); }
    static const AppWindow::RailListCache& railCache(AppWindow& a) { return a.railCache_; }
    static const AppWindow::TuningListCache& tuningCache(AppWindow& a) { return a.tuningCache_; }
    static std::string banner(AppWindow& a) { return a.muteBannerSubject(); }
    static bool popupOpen(AppWindow& a) { return a.mutePopupOpen_; }
    static bool popupRequested(AppWindow& a) { return a.mutePopupOpenRequest_; }
    static const std::vector<std::string>& popupNames(AppWindow& a) { return a.mutePopupNames_; }
    static const std::vector<std::string>& popupKeys(AppWindow& a) { return a.mutePopupKeys_; }
    static const cascade::gui::PluginRow* rowNamed(AppWindow& a, const std::string& n) { return a.muteStateForDisplayName(n); }
    static std::int32_t count(AppWindow& a, uint32_t list) { return a.engineApi_->read_list(a.uiSession_, list, 0, nullptr); }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
    static void openDecoderWindow(AppWindow& a) { a.decoderWindowOpen_ = true; }
    static long factAllocations(AppWindow& a, const char* key) {
        char buf[512];
        const long mark = t_allocs;
        (void)a.engineApi_->get_setting(a.uiSession_, key, buf, sizeof(buf));
        return t_allocs - mark;
    }

    // The members, drawn.
    static void drawDecoders(AppWindow& a) { a.drawDecodersSection(); }
    static void drawStatusRows(AppWindow& a) { a.drawDecoderStatusRows(); }
    static void drawOutput(AppWindow& a) { a.drawDecoderWindow(); }
    static void drawPresetBars(AppWindow& a) { a.drawDecoderPresetBars(); }
    static void drawPresetBar(AppWindow& a, const std::string& n) { a.drawPluginPresetBar(n); }
    static void drawTune(AppWindow& a) { a.drawPluginTuneControls(); }
    static void drawMarkers(AppWindow& a, bool waterfall) { a.drawPluginMarkers(0.0f, 0.0f, 800.0f, 200.0f, waterfall); }
    static void drawPopup(AppWindow& a) { a.drawMutePopup(); }
    static void drawBanner(AppWindow& a, const std::string& words) {
        cascade::gui::MuteBannerLayout mb;
        mb.px = ImGui::GetFontSize();
        mb.wordsPx = mb.px;
        mb.x0 = 0.0f;
        mb.y0 = 0.0f;
        mb.x1 = 600.0f;
        mb.y1 = 40.0f;
        mb.wordsX = 8.0f;
        mb.wordsY = 8.0f;
        mb.wordsDrawnW = 380.0f;
        mb.wordsWhole = true;
        mb.keyX = 400.0f;
        mb.keyY = 8.0f;
        mb.keyW = 100.0f;
        mb.keyH = 20.0f;
        a.drawMuteBanner(mb, ImVec2(0.0f, 40.0f), words);
    }

    // The engine, for comparison and for what a hermetic window cannot make.
    static const std::vector<cascade::core::LoadedPlugin>& host(AppWindow& a) { return a.engine_.pluginHost_.plugins(); }
    static const std::vector<cascade::core::MutePlugin>& muteStates(AppWindow& a) { return a.engine_.muteStates_; }
    static std::vector<cascade::gui::RunnableDecoder> runnable(AppWindow& a) { return a.engine_.runnableDecoders(); }
    static bool statusEmpty(AppWindow& a) { return a.engine_.pluginRunner_.status().empty(); }
    static std::size_t activeCount(AppWindow& a) { return a.engine_.pluginRunner_.activeCount(); }
    static bool running(AppWindow& a) { return a.engine_.pipeline_.running(); }
    static cascade::gui::ModuleCensus census(AppWindow& a) {
        return cascade::gui::censusModules(a.engine_.pluginHost_.plugins(), cascade::gui::kDecoderCaps,
                                           [&a](const std::string& key) { return a.engine_.pluginIsStopped(key); });
    }
    static const std::vector<cascade::core::HostMarker>& engineMarkers(AppWindow& a) { return a.engine_.pluginMarkers_; }
    static bool stopped(AppWindow& a, const std::string& key) { return a.engine_.pluginIsStopped(key); }
    static bool pluginMutes(AppWindow& a, const cascade::core::LoadedPlugin& p) { return a.engine_.pluginMutes(p); }
    // (the plugin UI is the window's since the pump move's step P3, the API core the engine's; the bridge a plugin calls
    // through reaches the window's PluginUi)
    static bool tuneAllowed(AppWindow& a, const std::string& key) { return a.pluginUi_.tuneAllowed(key); }
    static bool settingsAllowed(AppWindow& a, const std::string& key) { return a.pluginUi_.settingsAllowed(key); }
    static std::int32_t askToTune(AppWindow& a, const std::string& key) {
        return a.pluginUi_.tuneRequestFromPlugin(key, 100.0e6);
    }
    static std::size_t engineLog(AppWindow& a) { return a.engine_.decoderLog_.size(); }
    static bool enginePopupOpen(AppWindow& a) { return a.engine_.mutePopup_.open; }
    static bool engineKept(AppWindow& a) { return a.engine_.muteKeptRunning_; }
    static double tuned(AppWindow& a) { return a.engine_.currentAbsoluteHz(); }
    // Stand-ins for the engine's: a decoder line (test.decoderLine) and plugins that mute the audio on a preset
    // (test.muteBy: "name@hz;name@hz", each a running, muting plugin with one preset at that frequency).
    static void aLine(AppWindow& a, const std::string& plugin, const std::string& text) {
        a.engine_.decoderLog_.push_back({plugin, text});
        while (a.engine_.decoderLog_.size() > Engine::kDecoderLogMax) { a.engine_.decoderLog_.pop_front(); }
        ++a.engine_.decoderLinesTotal_;
    }
    static void muteBy(AppWindow& a, const std::string& spec) {
        a.engine_.muteStates_.clear();
        std::size_t at = 0;
        while (at < spec.size()) {
            const std::size_t semi = spec.find(';', at);
            const std::string one = spec.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
            const std::size_t amp = one.find('@');
            if (amp != std::string::npos) {
                cascade::core::MutePlugin m;
                m.name = one.substr(0, amp);
                m.key = m.name + ".dll";
                m.running = true;
                m.mutes = true;
                cascade::core::MutePreset ps;
                ps.frequencyHz = std::atof(one.c_str() + amp + 1);
                ps.label = m.name;
                m.presets.push_back(std::move(ps));
                a.engine_.muteStates_.push_back(std::move(m));
            }
            if (semi == std::string::npos) { break; }
            at = semi + 1;
        }
    }
    // A change of the lists that touches nothing the caches under test read (the preset note).
    static void aListChange(AppWindow& a) {
        static unsigned n = 0;
        a.engine_.presetNote_ = "note " + std::to_string(++n);
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

// One frame with `body` drawn inside a window; no engine pass.
template <class F>
void frame(F body, float w = 520.0f, float h = 900.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    body();
    ImGui::End();
    ImGui::Render();
}

// What a frame drew, as text: ImGui's own log of the text its widgets rendered.
template <class F>
std::string drawnText(F body) {
    std::string out;
    frame([&] {
        ImGui::LogToBuffer();
        body();
        out = ImGui::GetCurrentContext()->LogBuffer.c_str();  // (read before LogFinish, which empties it)
        ImGui::LogFinish();
    });
    return out;
}

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

std::string describe(const cmd::QueuedCommand& q) {
    char buf[480];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld num=%.17g,%.17g,%.17g,%.17g text=[%s]", cmd::opName(q.c.op),
                  static_cast<long long>(q.c.ival[0]), static_cast<long long>(q.c.ival[1]), q.c.num[0], q.c.num[1],
                  q.c.num[2], q.c.num[3], q.longText.empty() ? q.c.text : q.longText.c_str());
    return buf;
}

std::string expect(const char* op, long long i0, long long i1, const std::string& text) {
    char buf[480];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld num=%.17g,%.17g,%.17g,%.17g text=[%s]", op, i0, i1, 0.0, 0.0, 0.0,
                  0.0, text.c_str());
    return buf;
}

void checkSet(std::vector<std::string> got, std::vector<std::string> want, const char* what) {
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    const bool ok = got == want;
    if (!ok) {
        std::printf("      %s: the commands the clicks sent differ\n", what);
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : want) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(ok);
    std::printf("      %s: %zu command(s), as expected\n", what, got.size());
}

const std::string kStage1 = fileOf(FIXTURE_STAGE1_PLUGIN);
const std::string kGroup8 = fileOf(FIXTURE_GROUP8_PLUGIN);

// A window with the two fixtures loaded, its state read.
struct Fresh {
    AppWindow app;
    Fresh() { Access::frames(app, 2); }
};

const cascade::gui::PluginRow* row(AppWindow& a, const char* name) { return Access::rowNamed(a, name); }

// --- A -----------------------------------------------------------------------

void testRulesAgainstTheEngine() {
    std::printf("  A: the rules the window restates, against the engine's own answer for the same plugins\n");
    Fresh f;
    AppWindow& app = f.app;
    // The operator keeps two presets of her own for one plugin and one for the other, so the saved ones are in play.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 146.0e6));
    Access::command(app, cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, kStage1));
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 433.5e6));
    Access::command(app, cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, kStage1));
    Access::command(app, cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, kGroup8));
    Access::frames(app, 2);

    const auto& rows = Access::plugins(app).plugins;
    const auto& host = Access::host(app);
    CHECK_EQ(rows.size(), host.size());
    CHECK_EQ(rows.size(), 3u);  // two modules and the file the host refused
    for (std::size_t i = 0; i < rows.size() && i < host.size(); ++i) {
        const cc::LoadedPlugin& p = host[i];
        CHECK_EQ(rows[i].file, cc::pluginKey(p));
        CHECK_EQ(rows[i].name, p.name.empty() ? cc::pluginKey(p) : p.name);
        CHECK_EQ(rows[i].caps, p.capabilities);
        CHECK_EQ(rows[i].loaded, p.loaded);
        CHECK_EQ(rows[i].decoder, p.loaded && (p.decoder != nullptr || p.iqDecoder != nullptr || p.imageDecoder != nullptr));
        CHECK_EQ(rows[i].mutes, rows[i].decoder && Access::pluginMutes(app, p));
        CHECK_EQ(rows[i].stopped, p.loaded && Access::stopped(app, cc::pluginKey(p)));
        // A loaded plugin's tables are its declared bits (the host proves it), so the word says what the tables do.
        CHECK_EQ(cascade::gui::opensWindowOfItsOwn(rows[i]), p.trackSource != nullptr || p.imageDecoder != nullptr || p.panel != nullptr);
        CHECK_EQ(cascade::gui::isTextDecoder(rows[i]), p.decoder != nullptr);
    }
    // The presets: row for row what the engine's mute snapshot holds, the plugin's own then the operator's.
    int compared = 0;
    for (const cc::MutePlugin& m : Access::muteStates(app)) {
        const cascade::gui::PluginRow* r = row(app, m.name.c_str());
        CHECK(r != nullptr);
        if (r == nullptr) { continue; }
        CHECK_EQ(r->file, m.key);
        CHECK_EQ(r->presets.size(), m.presets.size());
        for (std::size_t k = 0; k < m.presets.size() && k < r->presets.size(); ++k) {
            const cc::MutePreset& want = m.presets[k];
            const cc::MutePreset& got = r->presets[k];
            const bool same = got.frequencyHz == want.frequencyHz && got.bandwidthHz == want.bandwidthHz &&
                              got.deviceCentre == want.deviceCentre && got.index == want.index && got.label == want.label;
            if (!same) { std::printf("      %s preset %zu differs\n", m.name.c_str(), k); }
            CHECK(same);
            ++compared;
        }
    }
    // (stage 1: two of its own + two saved; group 8: the two the host keeps (index 1 is dropped) + one saved)
    std::printf("      %d presets held equal to the engine's snapshot (raw indexes %u, %u; saved at %u, %u)\n", compared,
                row(app, "Group Eight Fixture")->presets[0].index, row(app, "Group Eight Fixture")->presets[1].index,
                row(app, "Stage One Fixture")->presets[2].index, row(app, "Stage One Fixture")->presets[3].index);
    CHECK_EQ(compared, 7);
    CHECK_EQ(row(app, "Group Eight Fixture")->ownPresets, 2u);
    CHECK_EQ(row(app, "Group Eight Fixture")->presets[0].index, 0u);
    CHECK_EQ(row(app, "Group Eight Fixture")->presets[1].index, 2u);
    CHECK(row(app, "Group Eight Fixture")->presets[0].deviceCentre);
    CHECK_EQ(row(app, "Group Eight Fixture")->presets[1].label, std::string("Group Eight Fixture"));  // no label: the plugin's name
    CHECK_EQ(row(app, "Group Eight Fixture")->presets[1].bandwidthHz, 25000.0);
    CHECK_EQ(row(app, "Stage One Fixture")->presets[2].index, 1000u);
    CHECK_EQ(row(app, "Stage One Fixture")->presets[3].index, 1001u);

    // The decoders, the runner's status and the census, in each state the Decoders section can find them in.
    const auto same = [&](const char* when) {
        const auto& rs = Access::plugins(app).plugins;
        const bool receiverRunning = Access::running(app);
        CHECK_EQ(cascade::gui::runnerStatusEmpty(rs), Access::statusEmpty(app));
        CHECK_EQ(cascade::gui::runningDecoderCount(rs, receiverRunning),
                 cascade::gui::runningCount(Access::runnable(app), receiverRunning));
        CHECK(cascade::gui::runningDecoderKeys(rs, receiverRunning) ==
              cascade::gui::stopAllKeys(Access::runnable(app), receiverRunning));
        const auto want = Access::runnable(app);
        const auto got = cascade::gui::runnableOf(rs);
        CHECK_EQ(got.size(), want.size());
        for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
            CHECK_EQ(got[i].name, want[i].name);
            CHECK_EQ(got[i].key, want[i].key);
            CHECK_EQ(got[i].stopped, want[i].stopped);
        }
        const cascade::gui::ModuleCensus a = cascade::gui::censusOf(rs, cascade::gui::kDecoderCaps);
        const cascade::gui::ModuleCensus b = Access::census(app);
        const bool eq = a.live == b.live && a.stopped == b.stopped && a.refused == b.refused && a.unread == b.unread &&
                        a.stoppedName == b.stoppedName && a.refusedName == b.refusedName;
        if (!eq) {
            std::printf("      %s: census %d/%d/%d/%d against %d/%d/%d/%d\n", when, a.live, a.stopped, a.refused, a.unread,
                        b.live, b.stopped, b.refused, b.unread);
        }
        CHECK(eq);
        CHECK_EQ(Access::state(app).decodersMatched > 0u, Access::activeCount(app) > 0u);
    };
    same("both loaded, the receiver stopped");
    CHECK_EQ(Access::census(app).live, 2);
    CHECK_EQ(Access::census(app).unread, 1);  // the file the host refused before it read what it was
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kStage1));
    Access::frames(app, 2);
    same("one stopped");
    CHECK_EQ(Access::census(app).stopped, 1);
    CHECK(!cascade::gui::runnerStatusEmpty(Access::plugins(app).plugins));
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kGroup8));
    Access::frames(app, 2);
    same("both stopped");
    CHECK(cascade::gui::runnerStatusEmpty(Access::plugins(app).plugins));
    CHECK(Access::statusEmpty(app));
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_START, kStage1));
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_START, kGroup8));
    Access::frames(app, 2);
    same("both started again");
    // The receiver running (the signal generator, the user's volume at zero): both are fed.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
    Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 1));
    Access::frames(app, 3);
    CHECK(Access::running(app));
    same("the receiver running");
    CHECK_EQ(cascade::gui::runningDecoderCount(Access::plugins(app).plugins, true), 2);
    CHECK(row(app, "Stage One Fixture")->fed && row(app, "Group Eight Fixture")->fed);
    CHECK_EQ(Access::state(app).decodersRunning, 2u);
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kGroup8));
    Access::frames(app, 2);
    same("running, one stopped");
    CHECK_EQ(cascade::gui::runningDecoderCount(Access::plugins(app).plugins, true), 1);
    Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 0));
    Access::frames(app, 2);

    // The marks the plugin set, field for field (a stopped plugin has none: it is started again, and sets them anew).
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_START, kGroup8));
    Access::frames(app, 3);
    const auto& marks = Access::plugins(app).markers;
    const auto& theirs = Access::engineMarkers(app);
    CHECK_EQ(marks.size(), theirs.size());
    CHECK_EQ(marks.size(), 2u);
    for (const cc::HostMarker& hm : theirs) {
        bool found = false;
        for (const CascadeMarker& m : marks) {
            if (m.id != hm.m.id) { continue; }
            found = true;
            CHECK_EQ(m.kind, hm.m.kind);
            CHECK_EQ(m.flags, hm.m.flags);
            CHECK_EQ(m.freqHz, hm.m.freqHz);
            CHECK_EQ(m.widthHz, hm.m.widthHz);
            CHECK_EQ(m.colourRgba, hm.m.colourRgba);
            CHECK_EQ(std::string(m.label), std::string(hm.m.label));
        }
        CHECK(found);
    }
    // ...and the pure conversions on their own: a label of 31 characters is whole, a longer one is cut to fit.
    const CascadeMarker long1 = cascade::gui::markerOf(7u, std::string(60, 'z').c_str(), 1.0, 2.0, 3u, true, false, true);
    CHECK_EQ(std::strlen(long1.label), static_cast<std::size_t>(CASCADE_MARKER_LABEL_CHARS - 1));
    CHECK_EQ(long1.kind, static_cast<std::uint32_t>(CASCADE_MARKER_SPAN));
    CHECK_EQ(long1.flags, static_cast<std::uint32_t>(CASCADE_MARKER_FLAG_SPECTRUM_ONLY));
    // A preset's bandwidth is kept only when it is one; a plugin's label stands in for an empty one.
    const cc::MutePreset p0 = cascade::gui::ownPresetOf(100.0e6, 0.0, false, 3u, "", "Plug");
    const cc::MutePreset p1 = cascade::gui::ownPresetOf(100.0e6, 2.0e12, false, 3u, "L", "Plug");
    const cc::MutePreset p2 = cascade::gui::ownPresetOf(100.0e6, -5.0, true, 3u, "L", "Plug");
    const cc::MutePreset p3 = cascade::gui::ownPresetOf(100.0e6, 12500.0, true, 3u, "L", "Plug");
    CHECK_EQ(p0.label, std::string("Plug"));
    CHECK_EQ(p1.bandwidthHz, 0.0);
    CHECK_EQ(p2.bandwidthHz, 0.0);
    CHECK_EQ(p3.bandwidthHz, 12500.0);
    CHECK(p2.deviceCentre && !p1.deviceCentre);
    const cc::MutePreset s0 = cascade::gui::savedPresetOf(145.0e6, 1.0e13, 2u, "mine");
    CHECK_EQ(s0.index, 1002u);
    CHECK_EQ(s0.bandwidthHz, 1.0e13);  // (a saved preset's is as it was saved: the snapshot kept it so)
    CHECK(!s0.deviceCentre);
}

// A2: the rules over every combination of one row's states, against the model they restate. (The two fixtures are both
// decoders and a stopped decoder is never fed, so against them alone the status rule could not tell "a loaded plugin" from
// "a loaded decoder" and the running count could not tell a stopped row from an unfed one.)
void testRulesOverEveryCombination() {
    std::printf("  A2: the rules over every combination of a row's states\n");
    using cascade::gui::PluginRow;
    const std::uint32_t capsOf[] = {0u, CASCADE_CAP_DECODER, CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET,
                                    CASCADE_CAP_TRACK_SOURCE, CASCADE_CAP_BASEMAP};
    std::vector<PluginRow> all;  // every combination, one row each
    for (const std::uint32_t caps : capsOf) {
        for (int bits = 0; bits < 8; ++bits) {
            PluginRow r;
            r.name = "P" + std::to_string(all.size());
            r.file = r.name + ".dll";
            r.caps = caps;
            r.loaded = (bits & 1) != 0;
            r.stopped = (bits & 2) != 0;
            r.fed = (bits & 4) != 0;
            r.decoder = r.loaded && (caps & cascade::gui::kDecoderCaps) != 0u;  // (the table lists a loaded decoder)
            all.push_back(r);
        }
    }
    int checked = 0;
    for (const bool receiverRunning : {false, true}) {
        // The count, the keys and the status list over EVERY subset of the first dozen rows, in order.
        for (unsigned mask = 0; mask < (1u << 12); ++mask) {
            std::vector<PluginRow> rows;
            for (unsigned i = 0; i < 12; ++i) {
                if ((mask & (1u << i)) != 0u) { rows.push_back(all[(i * 3u) % all.size()]); }
            }
            int running = 0;
            std::vector<std::string> keys;
            bool anyRunningPlugin = false;
            for (const PluginRow& r : rows) {
                if (r.loaded && r.decoder && receiverRunning && !r.stopped && r.fed) {
                    ++running;
                    keys.push_back(r.file);
                }
                anyRunningPlugin = anyRunningPlugin || (r.loaded && !r.stopped);
            }
            CHECK_EQ(cascade::gui::runningDecoderCount(rows, receiverRunning), running);
            CHECK(cascade::gui::runningDecoderKeys(rows, receiverRunning) == keys);
            CHECK_EQ(cascade::gui::runnerStatusEmpty(rows), !anyRunningPlugin);
            ++checked;
        }
    }
    // The census over every combination, against the rule written out: not loaded -> refused (a bit of the mask declared)
    // or unread (no word at all); loaded and providing -> stopped or live; loaded and not providing -> nothing.
    for (const PluginRow& r : all) {
        const std::vector<PluginRow> one{r};
        const cascade::gui::ModuleCensus c = cascade::gui::censusOf(one, cascade::gui::kDecoderCaps);
        const bool declares = (r.caps & cascade::gui::kDecoderCaps) != 0u;
        CHECK_EQ(c.refused, (!r.loaded && declares) ? 1 : 0);
        CHECK_EQ(c.unread, (!r.loaded && r.caps == 0u) ? 1 : 0);
        CHECK_EQ(c.stopped, (r.loaded && declares && r.stopped) ? 1 : 0);
        CHECK_EQ(c.live, (r.loaded && declares && !r.stopped) ? 1 : 0);
        CHECK_EQ(c.stoppedName, c.stopped != 0 ? r.name : std::string());
        CHECK_EQ(c.refusedName, c.refused != 0 ? r.name : std::string());
        ++checked;
    }
    // The runnable list is the loaded decoders, in order, with their stop state and file.
    {
        const auto got = cascade::gui::runnableOf(all);
        std::size_t k = 0;
        for (const PluginRow& r : all) {
            if (!r.loaded || !r.decoder) { continue; }
            CHECK(k < got.size());
            if (k < got.size()) {
                CHECK_EQ(got[k].key, r.file);
                CHECK_EQ(got[k].name, r.name);
                CHECK_EQ(got[k].stopped, r.stopped);
                CHECK_EQ(got[k].feeding, r.fed);
            }
            ++k;
        }
        CHECK_EQ(got.size(), k);
    }
    std::printf("      %d combinations held against the model (%zu row states x 2 receiver states over every subset of 12)\n",
                checked, all.size());
}

// --- B -----------------------------------------------------------------------

void testCacheFollowsTheEngine() {
    std::printf("  B: the plugin cache follows the engine: grants, the last refusal, the stale grants\n");
    Fresh f;
    AppWindow& app = f.app;
    const auto& c = Access::plugins(app);
    CHECK_EQ(c.staleGrants.size(), 0u);
    CHECK_EQ(c.lastDenied, std::string());
    CHECK(!row(app, "Group Eight Fixture")->tuneAllowed && !row(app, "Group Eight Fixture")->settingsAllowed);
    // A grant of each kind, the flags beside the engine's own.
    Access::command(app, [&] {
        FoxCommand g = cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup8, 1, 1).c;
        return g;
    }());
    {
        FoxCommand g = cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup8, 2, 1).c;
        Access::command(app, g);
    }
    Access::frames(app, 2);
    CHECK(row(app, "Group Eight Fixture")->tuneAllowed && row(app, "Group Eight Fixture")->settingsAllowed);
    CHECK(Access::tuneAllowed(app, kGroup8) && Access::settingsAllowed(app, kGroup8));
    CHECK(!row(app, "Stage One Fixture")->tuneAllowed);
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup8, 1, 0));
    Access::frames(app, 2);
    CHECK(!row(app, "Group Eight Fixture")->tuneAllowed && row(app, "Group Eight Fixture")->settingsAllowed);
    CHECK_EQ(Access::plugins(app).staleGrants.size(), 0u);
    // Stale: a module the host has no record of, and a file it refused.
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, "removed-module.dll", 1, 1));
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, "zz_refused.dll", 1, 1));
    Access::frames(app, 2);
    const auto& stale = Access::plugins(app).staleGrants;
    CHECK_EQ(stale.size(), 2u);
    if (stale.size() == 2u) {
        CHECK_EQ(stale[0].file, std::string("removed-module.dll"));
        CHECK(!stale[0].refused);
        CHECK_EQ(stale[1].file, std::string("zz_refused.dll"));
        CHECK(stale[1].refused);
    }
    // The last refusal: the module file name; the dialog's row for it reads the grant flag of that module.
    CHECK_EQ(Access::askToTune(app, kStage1), static_cast<std::int32_t>(CASCADE_TUNE_DENIED));
    Access::frames(app, 2);
    CHECK_EQ(Access::plugins(app).lastDenied, kStage1);
    // A rescan forgets it.
    FoxCommand rescan{};
    rescan.structSize = sizeof(rescan);
    rescan.op = FOXAPI_OP_PLUGIN_RESCAN;
    Access::command(app, rescan);
    Access::frames(app, 3);
    CHECK_EQ(Access::plugins(app).lastDenied, std::string());
}

// --- C -----------------------------------------------------------------------

void testLinesLeaveTheCachesAlone() {
    std::printf("  C (D3): a decoder line moves no list: 1,000 of them re-read none of the window's caches\n");
    Fresh f;
    AppWindow& app = f.app;
    // Every cache has read once.
    (void)Access::plugins(app);
    (void)Access::rail(app);
    (void)Access::tuning(app);
    const std::uint64_t seq = Access::listSeq(app);
    const std::uint32_t railRefreshes = Access::railCache(app).refreshes;
    const std::uint32_t railRows = Access::railCache(app).bandReads;
    const std::uint32_t tuningRefreshes = Access::tuningCache(app).refreshes;
    const std::uint32_t pluginRefreshes = Access::pluginCache(app).refreshes;
    const std::uint64_t total = Access::state(app).decoderLinesTotal;
    // A busy decoder: one line in every pass, a thousand passes.
    for (int pass = 0; pass < 1000; ++pass) {
        Access::aLine(app, "ADS-B", "line " + std::to_string(pass));
        Access::look(app);
        (void)Access::plugins(app);
        (void)Access::rail(app);
        (void)Access::tuning(app);
    }
    std::printf("      1000 lines, one a pass: listSeq %llu -> %llu; rail refreshes %u (band plan rows copied %u times), tuning refreshes %u, plugin refreshes %u\n",
                static_cast<unsigned long long>(seq), static_cast<unsigned long long>(Access::listSeq(app)),
                Access::railCache(app).refreshes - railRefreshes, Access::railCache(app).bandReads - railRows,
                Access::tuningCache(app).refreshes - tuningRefreshes, Access::pluginCache(app).refreshes - pluginRefreshes);
    CHECK_EQ(Access::state(app).decoderLinesTotal, total + 1000u);  // the measurement moved
    CHECK_EQ(Access::listSeq(app), seq);                             // ...and no list did
    CHECK_EQ(Access::railCache(app).refreshes, railRefreshes);
    CHECK_EQ(Access::railCache(app).bandReads, railRows);  // (the band plan's rows were never copied again)
    CHECK_EQ(Access::tuningCache(app).refreshes, tuningRefreshes);
    CHECK_EQ(Access::pluginCache(app).refreshes, pluginRefreshes);
    // Any other change of the lists still moves it, and each cache re-reads once.
    Access::aListChange(app);
    Access::look(app);
    (void)Access::plugins(app);
    (void)Access::rail(app);
    CHECK(Access::listSeq(app) != seq);
    CHECK_EQ(Access::railCache(app).refreshes, railRefreshes + 1u);
    CHECK_EQ(Access::pluginCache(app).refreshes, pluginRefreshes + 1u);
}

// --- D -----------------------------------------------------------------------

void testDecoderOutputCopiesOnlyWhatIsNew() {
    std::printf("  D: the decoder output: only the new lines are copied, in order, bounded; Clear\n");
    Fresh f;
    AppWindow& app = f.app;
    const auto& c = Access::lineCache(app);
    CHECK(c.lines.empty());
    const std::uint64_t reads0 = c.reads;
    // A frame with nothing new reads nothing.
    for (int i = 0; i < 5; ++i) { Access::look(app); }
    CHECK_EQ(c.reads, reads0);
    CHECK_EQ(c.copied, 0u);
    // 1,000 lines, ten a frame: each copied once, none again.
    for (int frameNo = 0; frameNo < 100; ++frameNo) {
        for (int i = 0; i < 10; ++i) {
            Access::aLine(app, (i % 2) ? "POCSAG" : "ACARS", "message " + std::to_string(frameNo * 10 + i));
        }
        Access::look(app);
    }
    std::printf("      1000 lines in 100 frames: %llu copied in %u ring reads; the window holds %zu (newest id %llu)\n",
                static_cast<unsigned long long>(c.copied), c.reads - static_cast<std::uint32_t>(reads0), c.lines.size(),
                static_cast<unsigned long long>(c.lines.empty() ? 0 : c.lines.back().id));
    CHECK_EQ(c.copied, 1000u);
    CHECK_EQ(c.reads - reads0, 100u);
    CHECK_EQ(c.lines.size(), 500u);  // the newest 500, as the engine's tail was
    CHECK_EQ(c.lines.back().id, Access::state(app).decoderLinesTotal);
    CHECK_EQ(c.lines.back().text, std::string("message 999"));
    CHECK_EQ(c.lines.back().plugin, std::string("ACARS") == c.lines.back().plugin ? std::string("ACARS") : std::string("POCSAG"));
    CHECK_EQ(c.lines.front().text, std::string("message 500"));
    for (std::size_t i = 1; i < c.lines.size(); ++i) { CHECK_EQ(c.lines[i].id, c.lines[i - 1].id + 1u); }
    // A burst bigger than the ring: what the ring holds (500), once.
    const std::uint64_t before = c.copied;
    for (int i = 0; i < 700; ++i) { Access::aLine(app, "ADS-B", "burst " + std::to_string(i)); }
    Access::look(app);
    CHECK_EQ(c.copied - before, 500u);
    CHECK_EQ(c.lines.back().text, std::string("burst 699"));
    CHECK_EQ(c.lines.front().text, std::string("burst 200"));
    // Clear: the copy goes at once, and nothing that was in it comes back.
    Access::clearLines(app);
    CHECK(c.lines.empty());
    Access::aLine(app, "ADS-B", "after the clear");
    Access::look(app);
    CHECK_EQ(c.lines.size(), 1u);
    CHECK_EQ(c.lines.front().text, std::string("after the clear"));
    // A line of the longest the table carries is cut where the table cuts it (159 bytes).
    Access::aLine(app, "ADS-B", std::string(300, 'q'));
    Access::look(app);
    CHECK_EQ(c.lines.back().text.size(), 159u);
}

// --- E -----------------------------------------------------------------------

void testMuteClusterAndTheQuestion() {
    std::printf("  E: the plugin-mute cluster: the causes, the event once a tune-away, the dialog, the banner\n");
    Fresh f;
    AppWindow& app = f.app;
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
    Access::muteBy(app, "Alpha decoder@145500000;Beta decoder@145500000");
    Access::frames(app, 3);
    CHECK(Access::plugins(app).muteCauses.empty());
    CHECK(!Access::plugins(app).muteKeptRunning);
    CHECK_EQ(Access::banner(app), std::string());
    CHECK(!Access::popupOpen(app));
    // On their preset: named, not held, no banner, no question.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
    Access::frames(app, 3);
    const auto& causes = Access::plugins(app).muteCauses;
    CHECK_EQ(causes.size(), 2u);
    if (causes.size() == 2u) {
        CHECK_EQ(causes[0].name, std::string("Alpha decoder"));
        CHECK_EQ(causes[0].key, std::string("Alpha decoder.dll"));
        CHECK_EQ(causes[1].name, std::string("Beta decoder"));
    }
    CHECK_EQ(Access::plugins(app).muteSubject, std::string("Alpha decoder and Beta decoder"));
    CHECK(!Access::plugins(app).muteKeptRunning);
    CHECK_EQ(Access::banner(app), std::string());  // on a preset the Sinks panel explains it; the banner is for a held mute
    CHECK(!Access::popupOpen(app));
    // Tuned away: the question opens on the event, ONCE, in the frame the event is taken.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
    int opened = 0;
    bool wasOpen = false;
    for (int i = 0; i < 10; ++i) {
        Access::frameOf(app);
        if (Access::popupOpen(app) && !wasOpen) { ++opened; }
        wasOpen = Access::popupOpen(app);
    }
    CHECK_EQ(opened, 1);
    CHECK(Access::popupOpen(app));
    CHECK(Access::plugins(app).muteKeptRunning);
    CHECK(Access::popupNames(app) == (std::vector<std::string>{"Alpha decoder", "Beta decoder"}));
    CHECK(Access::popupKeys(app) == (std::vector<std::string>{"Alpha decoder.dll", "Beta decoder.dll"}));
    CHECK_EQ(Access::banner(app), std::string("Alpha decoder and Beta decoder"));
    // The dialog, drawn: it names the plugins it captured and offers its two answers.
    {
        std::string text = drawnText([&] { Access::drawPopup(app); });
        text += drawnText([&] { Access::drawPopup(app); });
        CHECK(has(text, "Alpha decoder and Beta decoder"));
        CHECK(has(text, "Stop Alpha decoder and Beta decoder and resume sound"));
        CHECK(has(text, "Keep it running"));
        CHECK(GImGui->OpenPopupStack.Size > 0);
        std::printf("      the dialog drawn: names them, offers Stop and Keep it running\n");
    }
    // No more events, however many frames go by: no second question, and the one up stays the same.
    Access::frames(app, 30);
    CHECK(Access::popupOpen(app));
    // "Keep it running" (the engine's answer to the key, G presses it): the hold stays, the banner stays, and no
    // frame afterwards asks again.
    Access::command(app, cmd::make(FOXAPP_OP_MUTE_KEEP_RUNNING));
    Access::frames(app, 3);
    CHECK(Access::plugins(app).muteKeptRunning);
    CHECK_EQ(Access::banner(app), std::string("Alpha decoder and Beta decoder"));
    Access::frames(app, 30);
    // The hold lets go when the receiver is back on a preset: the question is withdrawn.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
    Access::frames(app, 4);
    CHECK(!Access::plugins(app).muteKeptRunning);
    CHECK_EQ(Access::banner(app), std::string());
    // A SECOND tune-away is a second question; the plugins named are the ones now muting.
    Access::muteBy(app, "Alpha decoder@145500000");
    Access::frames(app, 3);
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
    Access::frames(app, 4);
    CHECK(Access::popupOpen(app));
    CHECK(Access::popupNames(app) == (std::vector<std::string>{"Alpha decoder"}));
    // A hold that went before the question could be shown is not asked: the plugin stopped in between.
    Access::muteBy(app, "");
    Access::frames(app, 5);
    CHECK(!Access::plugins(app).muteKeptRunning);
    {
        // The dialog open but the hold gone: drawing it closes it (the engine withdrew its own).
        (void)drawnText([&] { Access::drawPopup(app); });
        CHECK(!Access::popupOpen(app));
    }
    // The cause list and the facts are quiet again.
    CHECK(Access::plugins(app).muteCauses.empty());
    CHECK_EQ(Access::plugins(app).muteSubject, std::string());
    // The banner names the plugins WHOLE: two names of 70 bytes make a sentence the state's field (127) cuts.
    {
        const std::string n1(70, 'a');
        const std::string n2(70, 'b');
        Access::muteBy(app, n1 + "@145500000;" + n2 + "@145500000");
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
        Access::frames(app, 4);
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
        Access::frames(app, 5);
        CHECK_EQ(Access::banner(app), n1 + " and " + n2);
        CHECK_EQ(std::strlen(Access::state(app).audioMutedBy), 127u);
    }
}

// --- F -----------------------------------------------------------------------

void testNothingAllocatesOnceSettled() {
    std::printf("  F: a settled frame's reads allocate nothing\n");
    Fresh f;
    AppWindow& app = f.app;
    (void)Access::plugins(app);
    (void)Access::lines(app);
    const std::string stageOneName = "Stage One Fixture";
    const long before = t_allocs;
    std::size_t sink = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto& c = Access::plugins(app);
        sink += c.plugins.size() + c.markers.size() + c.staleGrants.size() + c.muteCauses.size();
        sink += Access::lines(app).lines.size();
        sink += Access::banner(app).size();
        sink += Access::rowNamed(app, stageOneName) != nullptr ? 1u : 0u;
        sink += cascade::gui::runningDecoderCount(c.plugins, false) > 0 ? 1u : 0u;
        sink += cascade::gui::runnerStatusEmpty(c.plugins) ? 1u : 0u;
    }
    const long spent = t_allocs - before;
    std::printf("      1000 reads of an unchanged state: %ld allocation(s) (%zu)\n", spent, sink);
    CHECK_EQ(spent, 0L);
    // A refresh with nothing different to say re-reads the lists into strings that already have the room; what is
    // left is the engine's, not the window's (one get_setting of a fact allocates once inside the table).
    Access::aListChange(app);
    Access::look(app);
    (void)Access::plugins(app);
    long inRefresh = 0;
    for (int i = 0; i < 100; ++i) {
        Access::aListChange(app);
        Access::look(app);
        const long mark = t_allocs;
        (void)Access::plugins(app);
        inRefresh += t_allocs - mark;
    }
    const long perFact = Access::factAllocations(app, "fact.mute.subject");
    std::printf("      100 refreshes of the plugin cache: %ld allocation(s); one get_setting costs %ld, three a refresh\n",
                inRefresh, perFact);
    // (a refresh rebuilds the rows' preset labels and the census names, which are strings longer than a small
    // one; the bound is a regression net, measured: six a refresh with three plugins)
    CHECK(inRefresh <= 100L * (3L * perFact + 16L));
}

// --- G -----------------------------------------------------------------------

struct Spot {
    ImGuiID id;
    float x, y;
};

// The distinct widgets of what `body` draws, each at the first point of a grid the pointer finds it at.
template <class F>
std::vector<Spot> sweep(F body, float x0 = 6.0f, float y0 = 4.0f, float x1 = 510.0f, float y1 = 880.0f,
                        float dx = 12.0f, float dy = 4.0f, float w = 520.0f, float h = 900.0f) {
    std::vector<Spot> out;
    ImGuiIO& io = ImGui::GetIO();
    for (float y = y0; y < y1; y += dy) {
        for (float x = x0; x < x1; x += dx) {
            io.AddMousePosEvent(x, y);
            frame(body, w, h);
            frame(body, w, h);
            const ImGuiID hov = GImGui->HoveredId;
            if (hov == 0) { continue; }
            bool seen = false;
            for (const Spot& s : out) { seen = seen || s.id == hov; }
            if (!seen) { out.push_back({hov, x, y}); }
        }
    }
    io.AddMousePosEvent(w - 10.0f, h - 10.0f);
    frame(body, w, h);
    return out;
}

template <class F>
void clickAt(F body, float x, float y, float w = 520.0f, float h = 900.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame(body, w, h);
    io.AddMouseButtonEvent(0, true);
    frame(body, w, h);
    io.AddMouseButtonEvent(0, false);
    frame(body, w, h);
    io.AddMousePosEvent(w - 10.0f, h - 10.0f);
    frame(body, w, h);
    io.AddMouseButtonEvent(0, true);
    frame(body, w, h);
    io.AddMouseButtonEvent(0, false);
    frame(body, w, h);
}

// Every widget found, clicked once, and the commands each sent (except the usage report's).
template <class F>
std::vector<std::string> clickEverything(AppWindow& app, F body, std::vector<cmd::QueuedCommand>* captured, float topSkip,
                                         const std::function<void()>& after = {}) {
    Access::Sent sent;
    Access::tap(app, &sent);
    for (int i = 0; i < 3; ++i) { frame(body); }  // the first sightings
    const std::vector<Spot> spots = sweep(body);
    std::vector<std::string> out;
    std::printf("      %zu widgets found\n", spots.size());
    for (const Spot& sp : spots) {
        if (sp.y < topSkip) { continue; }
        sent.clear();
        clickAt(body, sp.x, sp.y);
        if (after) { after(); }
        for (const auto& q : sent) {
            if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
            out.push_back(describe(q));
            if (captured != nullptr) { captured->push_back(q); }
        }
    }
    Access::tap(app, nullptr);
    std::sort(out.begin(), out.end());
    return out;
}

struct Captured {
    std::vector<cmd::QueuedCommand> all;
};
Captured g_cap;

void testDrawnAndClicked() {
    std::printf("  G: the members drawn in real frames; every widget clicked\n");
    // --- idle: three frames of everything send only the first sightings, then nothing ---
    {
        Fresh f;
        AppWindow& app = f.app;
        Access::openDecoderWindow(app);
        Access::tap(app, nullptr);
        Access::Sent sent;
        Access::tap(app, &sent);
        const auto all = [&] {
            Access::drawDecoders(app);
            Access::drawOutput(app);
            Access::drawPresetBars(app);
            Access::drawPresetBar(app, "Stage One Fixture");
            Access::drawTune(app);
            Access::drawMarkers(app, false);
            Access::drawMarkers(app, true);
            Access::drawPopup(app);
        };
        // Lines in the output, a muting plugin on its preset (so there is a banner subject too), a refusal: every
        // member has something to say. Before the first read, then after.
        for (int looked = 0; looked < 2; ++looked) {
            if (looked == 1) {
                for (int i = 0; i < 12; ++i) { Access::aLine(app, "ACARS", "line " + std::to_string(i)); }
                Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, "removed-module.dll", 1, 1));
                (void)Access::askToTune(app, kStage1);
                Access::frames(app, 3);
            }
            for (int i = 0; i < 3; ++i) { frame(all); }
        }
        std::set<std::string> names;
        for (const auto& q : sent) {
            CHECK_EQ(q.c.op, static_cast<std::uint32_t>(FOXAPP_OP_NOTE_PANEL));
            names.insert(q.c.text);
        }
        std::printf("      eight members, six frames: first sightings %s\n", [&] {
            std::string s;
            for (const auto& n : names) { s += (s.empty() ? "" : ", ") + n; }
            return s;
        }().c_str());
        CHECK(names == (std::set<std::string>{"decoded", "decoders"}));
        sent.clear();
        for (int i = 0; i < 5; ++i) { frame(all); }
        CHECK(sent.empty());
        // The text they drew.
        const std::string tune = drawnText([&] { Access::drawTune(app); });
        CHECK(has(tune, "asked to tune the receiver and was refused"));
        CHECK(has(tune, "removed-module.dll"));
        CHECK(has(tune, "no module of this name is fitted here - grant kept"));
        const std::string out = drawnText([&] { Access::drawOutput(app); });
        CHECK(has(out, "ACARS"));
        CHECK(has(out, "line 11"));
        CHECK(has(out, "12 lines"));
        const std::string rows = drawnText([&] { Access::drawStatusRows(app); });
        CHECK(has(rows, "12 lines decoded"));
        Access::tap(app, nullptr);
    }
    // --- the Decoders section: both fitted, the receiver stopped; then running, both fed ---
    for (int running = 0; running < 2; ++running) {
        Fresh f;
        AppWindow& app = f.app;
        // One saved preset, so the saved keys (and their forget keys) are drawn too.
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 146.0e6));
        Access::command(app, cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, kStage1));
        if (running != 0) {
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
            Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 1));
        }
        Access::frames(app, 3);
        CHECK_EQ(Access::state(app).decodersRunning, running != 0 ? 2u : 0u);
        const auto body = [&] { Access::drawDecoders(app); };
        std::printf("    the Decoders section, %s:\n", running != 0 ? "the receiver running, both fed" : "the receiver stopped");
        const auto got = clickEverything(app, body, &g_cap.all, 56.0f);
        // What the clicks should have sent: the plugins' presets by raw index, the saved key, its forget key,
        // each plugin's STOP, its mute box (flipped), its Save key - and STOP ALL when two are running.
        std::vector<std::string> want = {
            expect("PLUGIN_PRESET", 0, 0, kStage1),
            expect("PLUGIN_PRESET", 1, 0, kStage1),
            expect("PLUGIN_PRESET", 0, 0, kGroup8),
            expect("PLUGIN_PRESET", 2, 0, kGroup8),
            expect("APP_USER_PRESET_APPLY", 0, 0, kStage1),
            expect("APP_USER_PRESET_FORGET_AT", 0, 0, kStage1),
            expect("DECODER_STOP", 0, 0, kStage1),
            expect("DECODER_STOP", 0, 0, kGroup8),
            expect("PLUGIN_MUTE", 1, 0, kStage1),  // an audio decoder does not mute by default: ticking it turns it on
            expect("PLUGIN_MUTE", 0, 0, kGroup8),  // an I/Q decoder does: unticking turns it off
            expect("USER_PRESET_SAVE", 0, 0, kStage1),
            expect("USER_PRESET_SAVE", 0, 0, kGroup8),
        };
        if (running != 0) {
            want.push_back(expect("APP_DECODER_STOP_LIST", 0, 0, kGroup8 + "\n" + kStage1));
        }
        checkSet(got, want, running != 0 ? "running" : "stopped");
        Access::dropQueued(app);
        // What the section says is running. (The header's chip - "2 FED" - is drawn straight onto the draw list, which
        // no text log reads: it is the state's decodersRunning, held by A, and compared in the bounded runs.)
        const std::string drawn = drawnText(body);
        CHECK(has(drawn, "Mute audio while running"));
        CHECK_EQ(has(drawn, "STOP ALL 2 RUNNING"), running != 0);
        // The status rows: the receiver stopped with two decoders built and matched is not "no decoder is running".
        const std::string rows = drawnText([&] { Access::drawStatusRows(app); });
        CHECK(!has(rows, "No decoder is running."));
        CHECK(!has(rows, "No decoder plugin is installed"));
    }
    // --- the preset bars of the Decoder output window, and a plugin's own ---
    {
        Fresh f;
        AppWindow& app = f.app;
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 146.0e6));
        Access::command(app, cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, kStage1));
        Access::frames(app, 3);
        const auto body = [&] { Access::drawPresetBars(app); };
        std::printf("    the Decoder output window's preset bars (the text decoders):\n");
        auto got = clickEverything(app, body, &g_cap.all, 26.0f);
        checkSet(got,
                 {expect("PLUGIN_PRESET", 0, 0, kStage1), expect("PLUGIN_PRESET", 1, 0, kStage1),
                  expect("APP_USER_PRESET_APPLY", 0, 0, kStage1), expect("USER_PRESET_SAVE", 0, 0, kStage1)},
                 "bars");
        Access::dropQueued(app);
        std::printf("    one plugin's own bar (an I/Q decoder with a device-centre key):\n");
        const auto own = [&] { Access::drawPresetBar(app, "Group Eight Fixture"); };
        got = clickEverything(app, own, &g_cap.all, 26.0f);
        checkSet(got,
                 {expect("PLUGIN_PRESET", 0, 0, kGroup8), expect("PLUGIN_PRESET", 2, 0, kGroup8),
                  expect("USER_PRESET_SAVE", 0, 0, kGroup8)},
                 "own bar");
        Access::dropQueued(app);
        // A plugin with no loaded record draws nothing, not even a separator.
        const std::string none = drawnText([&] { Access::drawPresetBar(app, "No Such Plugin"); });
        CHECK(none.empty());
    }
    // --- the receiver-control rows: a stale grant of each kind, and a refusal ---
    {
        Fresh f;
        AppWindow& app = f.app;
        Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, "removed-module.dll", 1, 1));
        Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, "zz_refused.dll", 1, 1));
        (void)Access::askToTune(app, kStage1);
        Access::frames(app, 3);
        const auto body = [&] { Access::drawTune(app); };
        std::printf("    the Receiver control rows (two stale grants and a refusal):\n");
        const std::string text = drawnText(body);
        CHECK(has(text, "\"" + kStage1 + "\" asked to tune the receiver and was refused."));
        CHECK(has(text, "this file is in the plugin folder and the host refused it"));
        CHECK(has(text, "no module of this name is fitted here - grant kept"));
        const auto got = clickEverything(app, body, &g_cap.all, 26.0f);
        checkSet(got,
                 {expect("PLUGIN_GRANT", 1, 0, "removed-module.dll"), expect("PLUGIN_GRANT", 1, 0, "zz_refused.dll")},
                 "grants");
        Access::dropQueued(app);
        // A refusal of a module that holds the grant is not shown; one the host never heard of, and that no
        // grant names, is.
        Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kStage1, 1, 1));
        Access::frames(app, 3);
        CHECK(!has(drawnText(body), "asked to tune the receiver"));
    }
    // --- the dialog and the banner: their keys ---
    {
        Fresh f;
        AppWindow& app = f.app;
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
        Access::muteBy(app, "Alpha decoder@145500000;Beta decoder@145500000");
        Access::frames(app, 3);
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
        Access::frames(app, 3);
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
        Access::frames(app, 5);
        CHECK(Access::popupOpen(app));
        const std::string words = "Sound muted by Alpha decoder and Beta decoder";
        const auto banner = [&] { Access::drawBanner(app, words); };
        std::printf("    the banner (the held mute):\n");
        Access::Sent sent;
        Access::tap(app, &sent);
        const std::vector<Spot> spots = sweep(banner, 6.0f, 30.0f, 590.0f, 90.0f, 8.0f, 4.0f, 700.0f, 100.0f);
        CHECK(!spots.empty());
        std::vector<std::string> got;
        for (const Spot& sp : spots) {
            if (sp.y < 30.0f) { continue; }  // (the title bar's collapse arrow would fold the panel)
            sent.clear();
            clickAt(banner, sp.x, sp.y, 700.0f, 100.0f);
            for (const auto& q : sent) {
                got.push_back(describe(q));
                g_cap.all.push_back(q);
            }
        }
        checkSet(got, {expect("APP_DECODER_STOP_LIST", 1, 0, "Alpha decoder.dll\nBeta decoder.dll")}, "banner");
        // The dialog's Stop key stops exactly the plugins it captured; its other key keeps them running.
        std::printf("    the dialog:\n");
        const auto dialog = [&] { Access::drawPopup(app); };
        (void)drawnText(dialog);
        (void)drawnText(dialog);
        const std::vector<Spot> keys = sweep(dialog, 120.0f, 420.0f, 880.0f, 660.0f, 8.0f, 4.0f, 1000.0f, 1000.0f);
        std::printf("      %zu widgets found\n", keys.size());
        std::vector<std::string> pressed;
        for (const Spot& sp : keys) {
            // A fresh dialog for each key: the window opens it again from the event's capture.
            Access::tap(app, &sent);
            sent.clear();
            AppWindow& a = app;
            (void)a;
            std::printf("      the dialog is %s at (%.0f, %.0f)\n", Access::popupOpen(app) ? "up" : "NOT up", sp.x, sp.y);
            clickAt(dialog, sp.x, sp.y, 1000.0f, 1000.0f);
            std::printf("      ...%zu command(s) sent\n", sent.size());
            for (const auto& q : sent) {
                pressed.push_back(describe(q));
                g_cap.all.push_back(q);
            }
            // The key answered it: the dialog is down, and - what the click queued not being applied (the session is
            // dropped) - stays down while the hold stays: no frame asks again.
            CHECK(!Access::popupOpen(app));
            Access::dropQueued(app);
            Access::frames(app, 5);
            CHECK(!Access::popupOpen(app));
            CHECK(Access::plugins(app).muteKeptRunning);
            // (The question is opened again for the next key.)
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
            Access::frames(app, 3);
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
            Access::frames(app, 5);
            (void)drawnText(dialog);
            (void)drawnText(dialog);
        }
        Access::tap(app, nullptr);
        checkSet(pressed,
                 {expect("APP_DECODER_STOP_LIST", 1, 0, "Alpha decoder.dll\nBeta decoder.dll"),
                  expect("APP_MUTE_KEEP_RUNNING", 0, 0, "")},
                 "dialog");
    }
    // --- the Decoder output window's Clear ---
    {
        Fresh f;
        AppWindow& app = f.app;
        Access::openDecoderWindow(app);
        for (int i = 0; i < 5; ++i) { Access::aLine(app, "ACARS", "line " + std::to_string(i)); }
        Access::frames(app, 3);
        const auto body = [&] { Access::drawOutput(app); };
        std::printf("    the Decoder output window:\n");
        Access::Sent sent;
        Access::tap(app, &sent);
        const std::string before = drawnText(body);
        CHECK(has(before, "line 4"));
        (void)drawnText(body);
        (void)drawnText(body);
        ImGuiWindow* page = ImGui::FindWindowByName("###decoderout");
        CHECK(page != nullptr);
        if (page == nullptr) { return; }
        std::printf("      the page is at (%.0f, %.0f), %.0f x %.0f\n", page->Pos.x, page->Pos.y, page->Size.x, page->Size.y);
        const std::vector<Spot> spots = sweep(body, page->Pos.x + 4.0f, page->Pos.y + 4.0f,
                                              std::min(990.0f, page->Pos.x + page->Size.x), page->Pos.y + 70.0f, 8.0f, 4.0f,
                                              300.0f, 100.0f);
        std::printf("      %zu widgets found\n", spots.size());
        std::vector<std::string> got;
        for (const Spot& sp : spots) {
            if (sp.y < page->Pos.y + 26.0f) { continue; }  // (the page's own title bar and its close key)
            sent.clear();
            clickAt(body, sp.x, sp.y, 300.0f, 100.0f);
            for (const auto& q : sent) {
                if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
                got.push_back(describe(q));
                g_cap.all.push_back(q);
            }
        }
        Access::tap(app, nullptr);
        // (the page also carries the text decoders' preset bars above its log: their two keys and the Save key)
        checkSet(got,
                 {expect("APP_CLEAR_STATUS", static_cast<long long>(cmd::FOXAPP_STATUS_DECODER_LOG), 0, ""),
                  expect("PLUGIN_PRESET", 0, 0, kStage1), expect("PLUGIN_PRESET", 1, 0, kStage1),
                  expect("USER_PRESET_SAVE", 0, 0, kStage1)},
                 "clear");
        CHECK(Access::lineCache(app).lines.empty());  // the window's copy went at once
        CHECK(!has(drawnText(body), "line 4"));
    }
}

// --- H -----------------------------------------------------------------------

// Each command the members sent when clicked (G's), as they built it, submitted on the window's session in a fresh
// window and applied by the engine's pass: accepted (OK), and the state the members read back is where it put it.
void testTheCommandsReachTheEngine() {
    std::printf("  H: each command the members sent, as built, is accepted through the session and applied\n");
    std::set<std::string> done;
    int distinct = 0;
    for (const cmd::QueuedCommand& q : g_cap.all) {
        if (!done.insert(describe(q)).second) { continue; }
        ++distinct;
        Fresh f;
        AppWindow& app = f.app;
        // The saved preset the apply and forget keys name, and a held mute for the answers to it.
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 146.0e6));
        Access::command(app, cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, kStage1));
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 433.0e6));
        const std::uint64_t op = q.c.op;
        std::size_t savedBefore = 0;
        if (op == FOXAPI_OP_DECODER_START) { Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, q.c.text)); }
        if (op == FOXAPP_OP_MUTE_KEEP_RUNNING || (op == FOXAPP_OP_DECODER_STOP_LIST && q.c.ival[0] == 1)) {
            Access::muteBy(app, "Alpha decoder@145500000;Beta decoder@145500000");
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
            Access::frames(app, 3);
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.0e6));
            Access::frames(app, 4);
            CHECK(Access::engineKept(app));
        }
        if (op == FOXAPI_OP_PLUGIN_GRANT) { Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, q.c.text, 1, 1)); }
        if (op == FOXAPP_OP_CLEAR_STATUS) {
            for (int i = 0; i < 3; ++i) { Access::aLine(app, "ACARS", "line"); }
        }
        Access::frames(app, 3);
        savedBefore = Access::plugins(app).plugins.empty() ? 0 : row(app, "Stage One Fixture")->presets.size();
        (void)Access::answers(app);
        Access::submit(app, q);
        Access::look(app);
        Access::look(app);
        const auto answers = Access::answers(app);
        if (answers.size() != 1u || answers[0].second != FOXAPI_OK) {
            std::printf("      %s: %zu answers, first %d\n", describe(q).c_str(), answers.size(),
                        answers.empty() ? 0 : answers[0].second);
        }
        CHECK_EQ(answers.size(), 1u);
        if (answers.size() == 1u) { CHECK_EQ(answers[0].second, FOXAPI_OK); }
        const std::string key = q.c.text;
        const auto isRow = [&](const char* name) { return row(app, name); };
        switch (op) {
            case FOXAPI_OP_PLUGIN_PRESET: {
                // Tuned to the preset's frequency; the plugin's own index, by file.
                const cascade::gui::PluginRow* r = key == kStage1 ? isRow("Stage One Fixture") : isRow("Group Eight Fixture");
                CHECK(r != nullptr);
                if (r != nullptr) {
                    for (std::size_t k = 0; k < r->ownPresets; ++k) {
                        if (r->presets[k].index == static_cast<std::uint32_t>(q.c.ival[0])) {
                            const double hz = r->presets[k].frequencyHz;
                            // (a device-centre preset moves the radio's centre, any other the tuned frequency)
                            const double at = r->presets[k].deviceCentre ? Access::state(app).centreHz : Access::state(app).tunedHz;
                            CHECK(std::fabs(at - hz) < 1.0);
                        }
                    }
                }
                break;
            }
            case FOXAPP_OP_USER_PRESET_APPLY: CHECK(std::fabs(Access::state(app).tunedHz - 146.0e6) < 1.0); break;
            case FOXAPP_OP_USER_PRESET_FORGET_AT:
                CHECK_EQ(isRow("Stage One Fixture")->presets.size(), savedBefore - 1u);
                break;
            case FOXAPI_OP_USER_PRESET_SAVE: {
                const cascade::gui::PluginRow* r = key == kStage1 ? isRow("Stage One Fixture") : isRow("Group Eight Fixture");
                CHECK(r != nullptr);
                if (r != nullptr && key == kStage1) { CHECK_EQ(r->presets.size(), savedBefore + 1u); }
                if (r != nullptr && key == kGroup8) { CHECK_EQ(r->presets.size(), r->ownPresets + 1u); }
                break;
            }
            case FOXAPI_OP_DECODER_STOP: CHECK(isRow(key == kStage1 ? "Stage One Fixture" : "Group Eight Fixture")->stopped); break;
            case FOXAPI_OP_DECODER_START: CHECK(!isRow(key == kStage1 ? "Stage One Fixture" : "Group Eight Fixture")->stopped); break;
            case FOXAPP_OP_DECODER_STOP_LIST:
                if (q.c.ival[0] == 1) {
                    // The mute dialog's and the banner's form: the plugins stopped, the hold ended, the question closed.
                    CHECK(Access::stopped(app, "Alpha decoder.dll") && Access::stopped(app, "Beta decoder.dll"));
                    CHECK(!Access::engineKept(app));
                    CHECK(Access::plugins(app).muteCauses.empty());
                } else {
                    CHECK(isRow("Stage One Fixture")->stopped && isRow("Group Eight Fixture")->stopped);
                }
                break;
            case FOXAPI_OP_PLUGIN_MUTE:
                CHECK_EQ(isRow(key == kStage1 ? "Stage One Fixture" : "Group Eight Fixture")->mutes, q.c.ival[0] != 0);
                break;
            case FOXAPI_OP_PLUGIN_GRANT: {
                bool gone = true;
                for (const auto& g : Access::plugins(app).staleGrants) { gone = gone && g.file != key; }
                CHECK(gone);  // unticked: the grant, and with it its row, is gone
                break;
            }
            case FOXAPP_OP_CLEAR_STATUS: CHECK_EQ(Access::engineLog(app), 0u); break;
            case FOXAPP_OP_MUTE_KEEP_RUNNING:
                CHECK(Access::engineKept(app));           // the mute stays
                CHECK(!Access::enginePopupOpen(app));      // ...and the engine's question is closed
                CHECK(Access::plugins(app).muteKeptRunning);
                break;
            default:
                std::printf("      an op the members should not have sent: %s\n", describe(q).c_str());
                CHECK(false);
                break;
        }
        Access::dropQueued(app);
    }
    std::printf("      %d distinct commands replayed\n", distinct);
    CHECK(distinct >= 14);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_decoders_mute_grants\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_dmg_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // The Decoders section starts closed; the capture hook opens it (read once).
    setEnv("FOXSDR_OPEN_DECODERS", "1");
    // ...and the Decoder output page opens inside the display, not beside the main window (read once).
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", "1");
    // The two fixture modules, and a file that is not one, in a plugin folder of this test's own.
    g_plugins = g_scratch / "plugins";
    std::filesystem::create_directories(g_plugins);
    for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP8_PLUGIN}) {
        const std::filesystem::path p(src);
        std::error_code ec;
        std::filesystem::copy_file(p, g_plugins / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    {
        std::ofstream junk(g_plugins / "zz_refused.dll", std::ios::binary);
        junk << "this is not a portable executable";
    }
    Access::installHooks();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testRulesAgainstTheEngine();
    testRulesOverEveryCombination();
    testCacheFollowsTheEngine();
    testLinesLeaveTheCachesAlone();
    testDecoderOutputCopiesOnlyWhatIsNew();
    testMuteClusterAndTheQuestion();
    testNothingAllocatesOnceSettled();
    testDrawnAndClicked();
    testTheCommandsReachTheEngine();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_decoders_mute_grants");
}
