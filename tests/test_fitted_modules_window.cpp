// test_fitted_modules_window.cpp - THE FITTED MODULES WINDOW READS ITS ENGINE API SESSION (window port, group 9:
// drawFittedModulesWindow and fittedLists; engine-stage4.md 6.5, API 0.3 section 8.8).
//
// What the window draws is the table's PLUGINS list and per-file facts, PLUGIN_COMMANDS, two status lines and the
// state's RX_RUNNING flag; what it changes it changes by seven commands on its session. This holds the window's
// side of that against the real engine with three REAL fixture modules fitted (tests/fixture_stage1_plugin.cpp,
// fixture_group8_plugin.cpp and fixture_group9_plugin.cpp: the last an audio processor with two command keys and a
// descriptor of its own) and a file the host refuses:
//   A  the model the window is built from the table, held equal field for field to the one the window built from
//      the engine's own records before the port (the code it replaced, restated here), in every state a module can
//      be in: fed, stopped, idle, a grant of each kind, a notice, an integrity finding, a refused file
//   B  what the window is told: the modules, size, descriptor, notice, grants, MATCHED with the receiver stopped
//   C  the cache follows listSeq: one rebuild when it moves, none when a decoder prints a line, none while the
//      window is closed - and a settled frame allocates nothing (1000 reads of an unchanged state: 0)
//   D  the real window drawn: every widget found, clicked, sends exactly the commands it should; the draw changes
//      when the state does and not otherwise
//   E  each control, clicked, lands at the engine's NEXT pass (not in the click's own frame) and the window reads
//      the engine's value the frame after: Stop, Start, the two grants, a module's command key, Rescan, Remove
//      (with its two presses) and the sentence a remove leaves
//
// THE TEST-ACCESSOR TRAP: a ported member reads the SESSION, so every frame here looks first, as drawUi does (the
// engine's pass takes the session's queue, publishes, the window reads its state), draws, and lets the NEXT look
// take what the draw sent. pumpTransmitKey is not run, so the session's answers are still there to be read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "gui/fonts.hpp"
#include "gui/plugins_view.hpp"
#include "imgui.h"
#include "imgui_internal.h"
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
    if (value.empty()) { unsetenv(name); } else { setenv(name, value.c_str(), 1); }
#endif
}

std::filesystem::path g_scratch;
std::filesystem::path g_plugins;
std::string pluginDirHook() { return g_plugins.string(); }

std::string fileOf(const char* fixture) { return std::filesystem::path(fixture).filename().string(); }

const char* const kWindowName = "Fitted modules###fittedmoduleswindow";

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cascade::core::cmd::QueuedCommand>;
    using Answer = std::pair<std::uint32_t, std::int32_t>;  // {op, status}
    using Engine = cascade::engine::Engine;

    static void installHooks() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirHook;
    }

    // One frame's engine part, in drawUi's order: the session's queue taken, the publish, the status copy, the
    // window's read of the state.
    static void look(AppWindow& a) {
        a.engine_.pumpFrameBegin();
        a.engine_.pumpInputs();
        a.engine_.pumpPlugins();
        (void)a.engine_.pumpPublish();
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
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
    static void command(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) { (void)a.engine_.applyCommand(q.c, q.longText); }
    static void command(AppWindow& a, const FoxCommand& c) { (void)a.engine_.applyCommand(c); }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }

    // The window's own: its cache, its deck, its open flag, the store's flag.
    static FittedListCache& lists(AppWindow& a) { return a.fittedLists(); }
    static FittedListCache& cache(AppWindow& a) { return *a.fittedCache_; }
    static FittedModulesDeck& deck(AppWindow& a) { return *a.fittedDeck_; }
    static void setOpen(AppWindow& a, bool open) { a.fittedWindowOpen_ = open; }
    static void setRect(AppWindow& a, int x, int y, int w, int h) {
        a.fittedWinX_ = x;
        a.fittedWinY_ = y;
        a.fittedWinW_ = w;
        a.fittedWinH_ = h;
    }
    static bool isOpen(AppWindow& a) { return a.fittedWindowOpen_; }
    static void storeDrew(AppWindow& a, bool drew) { a.pluginBrowserDrawnThisFrame_ = drew; }
    static void draw(AppWindow& a) { a.drawFittedModulesWindow(); }

    // The engine, for comparison and for what a hermetic window cannot make.
    static const std::vector<cascade::core::LoadedPlugin>& host(AppWindow& a) { return a.engine_.pluginHost_.plugins(); }
    static bool running(AppWindow& a) { return a.engine_.pipeline_.running(); }
    static bool stopped(AppWindow& a, const std::string& key) { return a.engine_.pluginIsStopped(key); }
    static bool tuneAllowed(AppWindow& a, const std::string& key) { return a.engine_.pluginUi_.tuneAllowed(key); }
    static bool settingsAllowed(AppWindow& a, const std::string& key) { return a.engine_.pluginUi_.settingsAllowed(key); }
    static cascade::core::PluginApiCore& core(AppWindow& a) { return a.engine_.pluginUi_.api(); }
    static cascade::core::PluginInventory& inventory(AppWindow& a) { return a.engine_.pluginInventory_; }
    static const std::string& engineDir(AppWindow& a) { return a.engine_.pluginDir_; }
    static void aLine(AppWindow& a, const std::string& plugin, const std::string& text) {
        a.engine_.decoderLog_.push_back({plugin, text});
        while (a.engine_.decoderLog_.size() > Engine::kDecoderLogMax) { a.engine_.decoderLog_.pop_front(); }
        ++a.engine_.decoderLinesTotal_;
    }

    // THE WINDOW AS IT WAS BEFORE THE PORT: the model built from the engine's own records, every frame (the code
    // drawFittedModulesWindow held from the day it was written to the day it was ported, restated).
    static FittedModulesModel oldModel(AppWindow& a) {
        Engine& e = a.engine_;
        FittedModulesModel model;
        model.directory = e.pluginDir_;
        model.receiverRunning = e.pipeline_.running();
        const std::vector<cascade::core::DecoderStatus> status = e.pluginRunner_.status();
        const std::vector<cascade::core::LoadedPlugin>& list = e.pluginHost_.plugins();
        const std::vector<std::string> settingsAskers = e.pluginUi_.api().settingsRequesters();
        model.modules.reserve(list.size());
        for (const cascade::core::LoadedPlugin& p : list) {
            const std::string file = cascade::core::pluginKey(p);
            std::string idleDetail;
            for (const cascade::core::DecoderStatus& s : status) {
                if (s.key != file) { continue; }
                if (s.reason == cascade::core::DecoderIdleReason::Running) { continue; }
                idleDetail = s.detail;
                break;
            }
            FittedModule m = makeFittedModule(p, e.pluginIsStopped(file), e.pluginRunner_.isFeeding(file),
                                              std::move(idleDetail),
                                              e.pluginUi_.tuneAllowed(cascade::core::PluginUi::tuneKey(p)));
            m.settingsCapable = std::find(settingsAskers.begin(), settingsAskers.end(), file) != settingsAskers.end();
            m.settingsAllowed = e.pluginUi_.settingsAllowed(file);
            for (const cascade::core::HostCommand& hc : e.pluginUi_.api().commands(file)) {
                m.commands.push_back(FittedModule::Command{hc.id, hc.label});
            }
            if (const auto nit = e.pluginNotices_.find(file); nit != e.pluginNotices_.end()) {
                m.noticeLevel = nit->second.level;
                m.notice = nit->second.text;
            }
            m.integrityNote = cascade::core::PluginRepo::changedSinceInstallNote(e.pluginInventory_.plugins, file);
            std::error_code sizeEc;
            const std::uintmax_t bytes = std::filesystem::file_size(p.path, sizeEc);
            if (!sizeEc) { m.sizeBytes = static_cast<std::uint64_t>(bytes); }
            model.modules.push_back(std::move(m));
        }
        return model;
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::FittedModule;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

const std::string kStage1 = fileOf(FIXTURE_STAGE1_PLUGIN);
const std::string kGroup8 = fileOf(FIXTURE_GROUP8_PLUGIN);
const std::string kGroup9 = fileOf(FIXTURE_GROUP9_PLUGIN);
const std::string kRefused = "zz_refused.dll";
const std::string kNineName = "Group Nine Fixture";

const char* const kNoCreate = "FOXSDR_FIXTURE_G9_NO_CREATE";

void junk(const std::string& file) {
    std::ofstream f(g_plugins / file, std::ios::binary);
    f << "this is not a portable executable";
}

// A window with the three fixtures loaded and the refused file in the folder (put back: a scenario may remove it), its
// state read and its window open.
struct Fresh {
    AppWindow app;
    Fresh() {
        Access::look(app);
        Access::look(app);
        // A window tall enough for the list AND the plate beside it (the display is made to fit it).
        Access::setRect(app, 20, 20, 1560, 1900);
        Access::setOpen(app, true);
    }
};

// A frame of the application, as far as this window is concerned: the engine's part, then the window drawn.
void drawFrame(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    Access::draw(a);
    ImGui::Render();
}
void lookDraw(AppWindow& a) {
    Access::look(a);
    drawFrame(a);
}

std::uint64_t drawHash() {
    std::uint64_t h = 1469598103934665603ull;
    const ImDrawData* dd = ImGui::GetDrawData();
    if (dd == nullptr) { return 0; }
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* l = dd->CmdLists[n];
        const unsigned char* v = reinterpret_cast<const unsigned char*>(l->VtxBuffer.Data);
        const std::size_t vb = static_cast<std::size_t>(l->VtxBuffer.Size) * sizeof(ImDrawVert);
        for (std::size_t i = 0; i < vb; ++i) { h = (h ^ v[i]) * 1099511628211ull; }
        h = (h ^ static_cast<std::uint64_t>(l->IdxBuffer.Size)) * 1099511628211ull;
    }
    return h;
}

const FittedModule* find(const cascade::gui::FittedModulesModel& m, const std::string& file) {
    for (const FittedModule& x : m.modules) {
        if (x.file == file) { return &x; }
    }
    return nullptr;
}
const FittedModule* mod(AppWindow& a, const std::string& file) { return find(Access::lists(a).model, file); }

int indexOf(AppWindow& a, const std::string& file) {
    const auto& ms = Access::lists(a).model.modules;
    for (std::size_t i = 0; i < ms.size(); ++i) {
        if (ms[i].file == file) { return static_cast<int>(i); }
    }
    return -1;
}

std::string describe(const cmd::QueuedCommand& q) {
    char buf[480];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld text=[%s]", cmd::opName(q.c.op), static_cast<long long>(q.c.ival[0]),
                  static_cast<long long>(q.c.ival[1]), q.longText.empty() ? q.c.text : q.longText.c_str());
    return buf;
}
std::string expect(const char* op, long long i0, long long i1, const std::string& text) {
    char buf[480];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld text=[%s]", op, i0, i1, text.c_str());
    return buf;
}

// The same module, as the engine's own records give it and as the table gave it.
bool sameModule(const FittedModule& got, const FittedModule& want, const char* when) {
    bool ok = true;
    const auto check = [&](bool same, const char* field) {
        if (!same) {
            std::printf("      %s: %s of %s differs\n", when, field, want.file.c_str());
            ok = false;
        }
        CHECK(same);
    };
    check(got.file == want.file, "file");
    check(got.path == want.path, "path");
    check(got.name == want.name, "name");
    check(got.version == want.version, "version");
    check(got.author == want.author, "author");
    check(got.licence == want.licence, "licence");
    check(got.capabilities == want.capabilities, "capabilities");
    check(got.loaded == want.loaded, "loaded");
    check(got.error == want.error, "error");
    // (a stop is held for a file that did not load too; the plate reads it only after `loaded`)
    check(got.stopped == (want.loaded && want.stopped), "stopped");
    check(got.fed == want.fed, "fed");
    check(got.idleDetail == want.idleDetail, "idleDetail");
    check(got.tuneCapable == want.tuneCapable, "tuneCapable");
    check(got.tuneAllowed == want.tuneAllowed, "tuneAllowed");
    check(got.settingsCapable == want.settingsCapable, "settingsCapable");
    check(got.settingsAllowed == want.settingsAllowed, "settingsAllowed");
    check(got.noticeLevel == want.noticeLevel, "noticeLevel");
    check(got.notice == want.notice, "notice");
    check(got.integrityNote == want.integrityNote, "integrityNote");
    check(got.sizeBytes == want.sizeBytes, "sizeBytes");
    bool sameKeys = got.commands.size() == want.commands.size();
    for (std::size_t i = 0; sameKeys && i < got.commands.size(); ++i) {
        sameKeys = got.commands[i].id == want.commands[i].id && got.commands[i].label == want.commands[i].label;
    }
    check(sameKeys, "commands");
    return ok;
}

// The whole model against the old window's, module by module, and the two things beside the modules.
int sameModel(AppWindow& a, const char* when) {
    Access::look(a);
    const cascade::gui::FittedModulesModel old = Access::oldModel(a);
    const cascade::gui::FittedModulesModel& got = Access::lists(a).model;
    CHECK_EQ(got.modules.size(), old.modules.size());
    CHECK_EQ(got.directory, old.directory);
    CHECK_EQ(got.receiverRunning, old.receiverRunning);
    int compared = 0;
    for (std::size_t i = 0; i < got.modules.size() && i < old.modules.size(); ++i) {
        if (sameModule(got.modules[i], old.modules[i], when)) { ++compared; }
        // ...and the same strip: the census the window prints is a function of the model
        CHECK_EQ(static_cast<int>(cascade::gui::fittedState(got.modules[i], got.receiverRunning)),
                 static_cast<int>(cascade::gui::fittedState(old.modules[i], old.receiverRunning)));
    }
    const cascade::gui::FittedCounts a1 = cascade::gui::countStates(got.modules, got.receiverRunning);
    const cascade::gui::FittedCounts b1 = cascade::gui::countStates(old.modules, old.receiverRunning);
    CHECK(a1.fed == b1.fed && a1.notFed == b1.notFed && a1.noSignal == b1.noSignal && a1.stopped == b1.stopped &&
          a1.refused == b1.refused && a1.total == b1.total);
    return compared;
}

// --- A -----------------------------------------------------------------------

void testModelEqualsTheOldWindow() {
    std::printf("  A: the model the window draws from the table, equal to the one it built from the engine's records\n");
    Fresh f;
    AppWindow& app = f.app;
    int compared = 0;
    compared += sameModel(app, "four files fitted, the receiver stopped");
    CHECK_EQ(Access::lists(app).model.modules.size(), 4u);  // three modules and the file the host refused

    // The receiver running: the decoders and the processor are fed.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
    Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 1));
    Access::look(app);
    Access::look(app);
    CHECK(Access::running(app));
    compared += sameModel(app, "the receiver running");
    CHECK(mod(app, kGroup9)->fed && mod(app, kStage1)->fed && mod(app, kGroup8)->fed);
    CHECK(!mod(app, kRefused)->fed);

    // One stopped.
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kGroup9));
    Access::look(app);
    Access::look(app);
    compared += sameModel(app, "one stopped");
    CHECK(mod(app, kGroup9)->stopped && !mod(app, kGroup9)->fed);
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_START, kGroup9));
    Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 0));
    Access::look(app);
    Access::look(app);

    // The grants of each kind, a request for a setting (the module has now asked), a warning, an error.
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup9, 1, 1));
    cc::PluginApiClient& c9 = Access::core(app).client(kGroup9, kNineName);
    cc::PluginControl ask;
    ask.kind = cc::PluginControl::Kind::Mode;
    ask.mode = CASCADE_DEMOD_NFM;
    CHECK_EQ(Access::core(app).requestControl(c9, ask), CASCADE_API_DENIED);
    CHECK_EQ(Access::core(app).log(c9, CASCADE_LOG_WARN, "the antenna is not what I was built for"), CASCADE_API_OK);
    Access::look(app);
    Access::look(app);
    compared += sameModel(app, "a tune grant, a request, a warning");
    CHECK(mod(app, kGroup9)->tuneAllowed && mod(app, kGroup9)->settingsCapable && !mod(app, kGroup9)->settingsAllowed);
    CHECK_EQ(mod(app, kGroup9)->noticeLevel, static_cast<std::uint32_t>(CASCADE_LOG_WARN));
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup9, 2, 1));
    CHECK_EQ(Access::core(app).log(c9, CASCADE_LOG_ERROR, std::string(300, 'e').c_str()), CASCADE_API_OK);
    Access::look(app);
    Access::look(app);
    compared += sameModel(app, "a radio-settings grant, an error of 300 characters");
    CHECK(mod(app, kGroup9)->settingsAllowed);
    CHECK_EQ(mod(app, kGroup9)->noticeLevel, static_cast<std::uint32_t>(CASCADE_LOG_ERROR));
    CHECK_EQ(mod(app, kGroup9)->notice.size(), 300u);  // whole: a list item's detail would cut it at 159

    // A finding of the install record's re-hash.
    cc::InstalledPlugin rec;
    rec.id = "group-nine-fixture";
    rec.name = kNineName;
    rec.file = kGroup9;
    rec.digestMismatch = true;
    Access::inventory(app).plugins.push_back(rec);
    Access::look(app);
    Access::look(app);
    compared += sameModel(app, "an integrity finding");
    CHECK(mod(app, kGroup9)->integrityNote.find("is not the file that was installed") != std::string::npos);
    CHECK(mod(app, kGroup8)->integrityNote.empty());
    Access::inventory(app).plugins.clear();

    // Idle: the processor cannot start. The runner's sentence, whole.
    setEnv(kNoCreate, "1");
    Access::command(app, cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
    Access::look(app);
    Access::look(app);
    Access::look(app);
    compared += sameModel(app, "an audio processor that cannot start");
    CHECK(!mod(app, kGroup9)->fed);
    CHECK(mod(app, kGroup9)->idleDetail.find("failed to start its audio processor") != std::string::npos);
    CHECK(mod(app, kGroup8)->idleDetail.empty());
    setEnv(kNoCreate, "");
    Access::command(app, cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
    Access::look(app);
    Access::look(app);
    Access::look(app);
    compared += sameModel(app, "started again after a rescan");
    CHECK(mod(app, kGroup9)->fed && mod(app, kGroup9)->idleDetail.empty());
    // (a rescan keeps what the plugin asked for: the flag is sticky)
    CHECK(mod(app, kGroup9)->settingsCapable);
    std::printf("      %d modules held equal to the old window's, field for field, in eight states\n", compared);
    CHECK_EQ(compared, 4 * 8);

    // The one conversion beside the table's, held on a real host's records: the builder over a row against the
    // builder over a LoadedPlugin (what makeFittedModule is still used for), for every record.
    for (const cc::LoadedPlugin& p : Access::host(app)) {
        const std::string file = cc::pluginKey(p);
        const FittedModule oldM = cascade::gui::makeFittedModule(p, false, false, "", false);
        cascade::gui::FittedRow r;
        r.file = file;
        r.name = p.name.empty() ? file : p.name;
        r.caps = p.capabilities;
        r.loaded = p.loaded;
        r.path = p.path;
        r.version = p.version;
        r.author = p.author;
        r.licence = p.licence;
        r.error = p.error;
        const FittedModule newM = cascade::gui::makeFittedModuleFromRow(r);
        CHECK(newM.file == oldM.file && newM.path == oldM.path && newM.name == oldM.name && newM.version == oldM.version &&
              newM.author == oldM.author && newM.licence == oldM.licence && newM.capabilities == oldM.capabilities &&
              newM.loaded == oldM.loaded && newM.error == oldM.error && newM.tuneCapable == oldM.tuneCapable);
    }
    // ...and the rules of the row builder on their own: the host-client key only for a LOADED module that declares
    // the bit; the name only when a descriptor was read (the table fills `name` with the file's own for a file that
    // was refused first); the module's keys are not the row's.
    {
        cascade::gui::FittedRow r;
        r.file = "x.dll";
        r.name = "X";
        r.caps = CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_DECODER;
        r.loaded = true;
        r.matched = true;
        r.stopped = true;
        r.sizeBytes = 12345u;
        r.noticeLevel = 3u;
        r.notice = "n";
        r.integrity = "i";
        r.idle = "w";
        r.askedSettings = true;
        r.settingsAllowed = true;
        r.tuneAllowed = true;
        FittedModule m = cascade::gui::makeFittedModuleFromRow(r);
        CHECK(m.tuneCapable && m.loaded && m.name == "X" && m.fed && m.stopped && m.sizeBytes == 12345u);
        CHECK(m.noticeLevel == 3u && m.notice == "n" && m.integrityNote == "i" && m.idleDetail == "w");
        CHECK(m.settingsCapable && m.settingsAllowed && m.tuneAllowed && m.commands.empty());
        r.loaded = false;
        r.error = "refused";
        CHECK(!cascade::gui::makeFittedModuleFromRow(r).tuneCapable);  // (a duplicate the host turned off)
        r.loaded = true;
        r.caps = CASCADE_CAP_DECODER;
        CHECK(!cascade::gui::makeFittedModuleFromRow(r).tuneCapable);
        r.caps = 0u;
        r.name = "x.dll";
        CHECK(cascade::gui::makeFittedModuleFromRow(r).name.empty());
        // written into a record that has a name and keys: nothing of them is left
        FittedModule used;
        used.name = "old";
        used.commands.push_back(FittedModule::Command{7u, "old"});
        cascade::gui::fillFittedModule(used, r);
        CHECK(used.name.empty() && used.commands.empty());
    }
}

// --- B -----------------------------------------------------------------------

void testWhatTheWindowIsTold() {
    std::printf("  B: the modules, their size, descriptor and keys, MATCHED with the receiver stopped, a refused file\n");
    Fresh f;
    AppWindow& app = f.app;
    lookDraw(app);
    lookDraw(app);
    const cascade::gui::FittedModulesModel& model = Access::lists(app).model;
    CHECK_EQ(model.directory, g_plugins.string());
    CHECK(!model.receiverRunning);
    const FittedModule* g9 = mod(app, kGroup9);
    CHECK(g9 != nullptr);
    if (g9 == nullptr) { return; }
    CHECK_EQ(g9->name, kNineName);
    CHECK_EQ(g9->version, std::string("9.1.0"));
    CHECK_EQ(g9->author, std::string("FoxSDR tests (group nine)"));
    CHECK_EQ(g9->licence, std::string("MIT"));
    CHECK_EQ(std::filesystem::path(g9->path), g_plugins / kGroup9);
    CHECK_EQ(g9->sizeBytes, static_cast<std::uint64_t>(std::filesystem::file_size(g_plugins / kGroup9)));
    CHECK(g9->sizeBytes > 0u);
    CHECK(g9->loaded && g9->error.empty());
    CHECK((g9->capabilities & CASCADE_CAP_AUDIO_PROCESSOR) != 0u && (g9->capabilities & CASCADE_CAP_HOST_CLIENT) != 0u);
    CHECK(g9->tuneCapable && !g9->tuneAllowed && !g9->settingsCapable && !g9->settingsAllowed);
    CHECK(!g9->stopped);
    // MATCHED with the receiver stopped - for a module that is not a decoder - so the window says what it needs.
    CHECK(g9->fed);
    CHECK(cascade::gui::fittedState(*g9, model.receiverRunning) == cascade::gui::FittedState::NotFed);
    const std::string sentence = cascade::gui::fittedStateSentence(*g9, model.receiverRunning);
    CHECK(sentence.find("starting the receiver is all this needs") != std::string::npos);
    // Its two command keys, in the order it added them.
    CHECK_EQ(g9->commands.size(), 2u);
    if (g9->commands.size() == 2u) {
        CHECK(g9->commands[0].id == 1u && g9->commands[0].label == "RESET");
        CHECK(g9->commands[1].id == 2u && g9->commands[1].label == "CALIBRATE");
    }
    CHECK(mod(app, kGroup8)->commands.empty());
    CHECK_EQ(mod(app, kStage1)->commands.size(), 1u);  // the stage-1 fixture's one key
    // The refused file: no descriptor read, the host's reason whole, no keys, not matched.
    const FittedModule* bad = mod(app, kRefused);
    CHECK(bad != nullptr);
    if (bad != nullptr) {
        CHECK(!bad->loaded && bad->name.empty() && bad->version.empty() && bad->author.empty() && bad->licence.empty());
        CHECK_EQ(bad->capabilities, 0u);
        CHECK(!bad->error.empty());
        CHECK(!bad->fed && !bad->tuneCapable && bad->commands.empty());
        CHECK(cascade::gui::fittedState(*bad, false) == cascade::gui::FittedState::Refused);
        CHECK(!cascade::gui::makeModulePlate(*bad).haveDescriptor);
        CHECK(cascade::gui::makeModulePlate(*g9).haveDescriptor);
        bool engineSays = false;
        for (const cc::LoadedPlugin& p : Access::host(app)) {
            if (cc::pluginKey(p) == kRefused) { engineSays = p.error == bad->error; }
        }
        CHECK(engineSays);
    }
    // The plugin's file is not measured when it cannot be (a file that vanished behind the engine's back is
    // measured as 0 at the next rebuild, which the plate draws as "not measured").
    std::printf("      %s: %llu bytes, %s %s by %s (%s), %zu keys, MATCHED with the receiver stopped; %s refused: %s\n",
                kGroup9.c_str(), static_cast<unsigned long long>(g9->sizeBytes), g9->name.c_str(), g9->version.c_str(),
                g9->author.c_str(), g9->licence.c_str(), g9->commands.size(), kRefused.c_str(),
                bad != nullptr ? bad->error.substr(0, 60).c_str() : "?");

    // The install sentence comes from the status lines, and only when the store did not print it.
    CHECK(model.report.empty() && model.error.empty());
    Access::command(app, cmd::makeText(FOXAPI_OP_STORE_REMOVE, "no-such-file.dll"));
    Access::look(app);
    Access::look(app);
    CHECK(Access::lists(app).model.report.empty());
    CHECK(Access::lists(app).model.error.find("is not an installed plugin file") != std::string::npos);
    Access::storeDrew(app, true);  // the store window drew it this frame: the sentence is not said twice
    CHECK(Access::lists(app).model.report.empty() && Access::lists(app).model.error.empty());
    Access::storeDrew(app, false);
    CHECK(Access::lists(app).model.error.find("is not an installed plugin file") != std::string::npos);
}

// --- C -----------------------------------------------------------------------

void testCacheFollowsListSeq() {
    std::printf("  C: one rebuild when listSeq moves, none for a decoder line or while the window is shut; a settled frame allocates nothing\n");
    Fresh f;
    AppWindow& app = f.app;
    lookDraw(app);
    lookDraw(app);
    auto& c = Access::cache(app);
    const std::uint32_t refreshes0 = c.refreshes;
    CHECK(refreshes0 >= 1u);

    // Nothing moved: no rebuild. A decoder's line moves listSeq nowhere (D3), so a busy decoder re-reads nothing.
    const std::uint64_t seq0 = Access::listSeq(app);
    for (int i = 0; i < 1000; ++i) {
        Access::aLine(app, "ADS-B", "line " + std::to_string(i));
        Access::look(app);
        (void)Access::lists(app);
    }
    CHECK_EQ(Access::listSeq(app), seq0);
    CHECK_EQ(c.refreshes, refreshes0);

    // A settled frame: 1000 reads of an unchanged state allocate nothing (the receiver's run state and the install
    // sentence are set on the model each time; a string given what its room holds allocates nothing).
    long before = t_allocs;
    std::size_t sink = 0;
    for (int i = 0; i < 1000; ++i) {
        const cascade::gui::FittedModulesModel& m = Access::lists(app).model;
        sink += m.modules.size() + m.directory.size() + m.report.size() + (m.receiverRunning ? 1u : 0u);
        for (const FittedModule& x : m.modules) { sink += x.commands.size() + x.notice.size(); }
    }
    long spent = t_allocs - before;
    std::printf("      1000 reads of an unchanged state: %ld allocation(s) (%zu)\n", spent, sink);
    CHECK_EQ(spent, 0L);
    // ...also while a sentence is up (it is copied into the model each frame: the room is there after the first).
    Access::command(app, cmd::makeText(FOXAPI_OP_STORE_REMOVE, "no-such-file.dll"));
    Access::look(app);
    Access::look(app);
    (void)Access::lists(app);
    before = t_allocs;
    for (int i = 0; i < 1000; ++i) { sink += Access::lists(app).model.error.size(); }
    spent = t_allocs - before;
    std::printf("      1000 reads with the install sentence up: %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);

    // What the window DID cost before the port, every frame it was open, for the record: the same four files.
    before = t_allocs;
    const int frames = 100;
    for (int i = 0; i < frames; ++i) { sink += Access::oldModel(app).modules.size(); }
    const long oldPerFrame = (t_allocs - before) / frames;
    std::printf("      the old window's model, built every frame from the engine's records: %ld allocations a frame (and a stat of each file)\n",
                oldPerFrame);
    CHECK(oldPerFrame > 0);

    // A change of the lists: one rebuild, bounded.
    const auto aChange = [&](int n) {
        cc::PluginApiClient& c9 = Access::core(app).client(kGroup9, kNineName);
        (void)Access::core(app).log(c9, CASCADE_LOG_WARN, ("warning number " + std::to_string(n)).c_str());
    };
    aChange(1);
    Access::look(app);
    CHECK(Access::listSeq(app) != seq0);
    const std::uint32_t r1 = c.refreshes;
    before = t_allocs;
    (void)Access::lists(app);
    spent = t_allocs - before;
    CHECK_EQ(c.refreshes, r1 + 1u);
    (void)Access::lists(app);
    CHECK_EQ(c.refreshes, r1 + 1u);  // read once however often it is asked
    CHECK_EQ(mod(app, kGroup9)->notice, std::string("warning number 1"));
    // 100 more, each with a text of a different length so that nothing keeps its room by accident.
    long inRefresh = 0;
    for (int i = 0; i < 100; ++i) {
        aChange(2 + i);
        Access::look(app);
        const long mark = t_allocs;
        (void)Access::lists(app);
        inRefresh += t_allocs - mark;
    }
    const std::size_t modules = Access::lists(app).model.modules.size();
    std::printf("      a rebuild (%zu files, 8 facts each, one list of keys): %ld allocation(s) the first time, %ld on average over 100 (all but a few inside the table's get_setting)\n",
                modules, spent, inRefresh / 100);
    CHECK(spent <= 120L);
    CHECK(inRefresh / 100 <= 120L);

    // The same warning said again every pass moves nothing, whatever the plugin; a different one every pass moves
    // listSeq every pass - the cost of that, measured, because a chatty plugin must not make this window slow.
    const std::uint64_t seqSame = Access::listSeq(app);
    const std::uint32_t rSame = c.refreshes;
    for (int i = 0; i < 1000; ++i) {
        aChange(100);
        Access::look(app);
        (void)Access::lists(app);
    }
    std::printf("      the same warning 1000 passes running: listSeq %llu -> %llu, %u rebuilds\n",
                static_cast<unsigned long long>(seqSame), static_cast<unsigned long long>(Access::listSeq(app)),
                c.refreshes - rSame);
    CHECK_EQ(Access::listSeq(app), seqSame + 1u);  // (the first of them is a different text from the last: one move)
    CHECK(c.refreshes - rSame <= 1u);
    const std::uint32_t rBusy = c.refreshes;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        aChange(1000 + i);
        Access::look(app);
        (void)Access::lists(app);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("      a different warning every pass, 1000 passes: %u rebuilds, %.1f ms in all with the engine's pass and the publish (%.3f ms a pass)\n",
                c.refreshes - rBusy, ms, ms / 1000.0);
    CHECK_EQ(c.refreshes - rBusy, 1000u);

    // The window SHUT reads nothing: the cache is not touched however the lists move.
    Access::setOpen(app, false);
    const std::uint32_t rShut = c.refreshes;
    for (int i = 0; i < 50; ++i) {
        aChange(5000 + i);
        lookDraw(app);
    }
    CHECK_EQ(c.refreshes, rShut);
    Access::setOpen(app, true);
    lookDraw(app);
    CHECK_EQ(c.refreshes, rShut + 1u);  // ...and one read when it opens, however much moved
    CHECK_EQ(mod(app, kGroup9)->notice, std::string("warning number 5049"));
}

// --- D -----------------------------------------------------------------------

struct Hit {
    ImGuiID id;
    float x, y;
};

// The distinct widgets of the window, each at the first point of a grid the pointer finds it at (the title bar is
// skipped: its collapse arrow swallows a click and folds the window).
std::vector<Hit> sweep(AppWindow& app) {
    std::vector<Hit> out;
    ImGuiIO& io = ImGui::GetIO();
    drawFrame(app);
    ImGuiWindow* w = ImGui::FindWindowByName(kWindowName);
    CHECK(w != nullptr);
    if (w == nullptr) { return out; }
    const ImVec2 p = w->Pos;
    const ImVec2 sz = w->Size;
    for (float y = p.y + 30.0f; y < p.y + sz.y - 8.0f; y += 10.0f) {
        for (float x = p.x + 8.0f; x < p.x + sz.x - 8.0f; x += 24.0f) {
            io.AddMousePosEvent(x, y);
            drawFrame(app);
            drawFrame(app);
            const ImGuiID hov = GImGui->HoveredId;
            if (hov == 0) { continue; }
            bool seen = false;
            for (const Hit& s : out) { seen = seen || s.id == hov; }
            if (!seen) { out.push_back({hov, x, y}); }
        }
    }
    io.AddMousePosEvent(2.0f, 2.0f);
    drawFrame(app);
    return out;
}

// A click in three frames of the window with no engine pass between them: the press, the release (which is where
// the key fires and sends its command), and the pointer put away.
void clickNoPass(AppWindow& app, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    drawFrame(app);
    drawFrame(app);  // (ImGui finds what is under the pointer a frame after it moves)
    io.AddMouseButtonEvent(0, true);
    drawFrame(app);
    io.AddMouseButtonEvent(0, false);
    drawFrame(app);
    io.AddMousePosEvent(2.0f, 2.0f);
    drawFrame(app);
}

struct Found {
    std::string command;     // what the click sent ("" nothing)
    std::string armed;       // the file the click armed for removal ("" none)
    float x, y;
};

void selectModule(AppWindow& app, const std::string& file) {
    Access::deck(app) = cascade::gui::FittedModulesDeck{};
    Access::deck(app).selected = indexOf(app, file);
    lookDraw(app);
}

// Every widget of the window with `file` on the plate, found and clicked once in a fresh deck each time, and what
// the click did: the command it sent (nothing is applied: no pass runs) and the removal it armed. The caller's
// tap is dropped; the caller puts its own back.
std::vector<Found> survey(AppWindow& app, const std::string& file, std::vector<std::string>* commandsSent = nullptr) {
    Access::Sent sent;
    Access::tap(app, &sent);
    selectModule(app, file);
    // The page's RESET WINDOWS key puts the window back at its opening place, so where the window sits is put
    // back before each click and after the last (a click that moved it would move every widget under the pointer).
    ImGuiWindow* w0 = ImGui::FindWindowByName(kWindowName);
    CHECK(w0 != nullptr);
    if (w0 == nullptr) { return {}; }
    const ImVec2 winPos = w0->Pos;
    const ImVec2 winSize = w0->Size;
    const auto putBack = [&] {
        ImGui::SetWindowPos(kWindowName, winPos, ImGuiCond_Always);
        ImGui::SetWindowSize(kWindowName, winSize, ImGuiCond_Always);
    };
    const std::vector<Hit> spots = sweep(app);
    std::vector<Found> found;
    for (const Hit& s : spots) {
        putBack();
        selectModule(app, file);
        sent.clear();
        clickNoPass(app, s.x, s.y);
        Found fd{"", Access::deck(app).confirmRemove, s.x, s.y};
        for (const auto& q : sent) {
            if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
            fd.command = describe(q);
            if (commandsSent != nullptr) { commandsSent->push_back(fd.command); }
        }
        found.push_back(fd);
        Access::dropQueued(app);
    }
    Access::tap(app, nullptr);
    putBack();
    selectModule(app, file);
    selectModule(app, file);
    return found;
}

void testDrawnAndClicked() {
    std::printf("  D: the real window drawn; every widget found, clicked, sends exactly the commands it should\n");
    Fresh f;
    AppWindow& app = f.app;
    lookDraw(app);
    lookDraw(app);
    // Idle: a window drawn for ten frames sends nothing (the usage report's page note excepted, once).
    Access::Sent sent;
    Access::tap(app, &sent);
    for (int i = 0; i < 10; ++i) { lookDraw(app); }
    std::set<std::string> names;
    for (const auto& q : sent) {
        CHECK_EQ(q.c.op, static_cast<std::uint32_t>(FOXAPP_OP_NOTE_PANEL));
        names.insert(q.c.text);
    }
    CHECK(names.size() <= 1u);
    sent.clear();
    for (int i = 0; i < 5; ++i) { lookDraw(app); }
    CHECK(sent.empty());
    Access::tap(app, nullptr);
    CHECK(Access::deck(app).confirmRemove.empty());

    // The drawing follows the state, and only the state: the same frame twice is the same picture.
    ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
    lookDraw(app);
    lookDraw(app);
    const std::uint64_t h0 = drawHash();
    lookDraw(app);
    const std::uint64_t h1 = drawHash();
    CHECK(h0 != 0u);
    CHECK_EQ(h0, h1);
    CHECK(Access::isOpen(app));

    // Every widget with the module on the plate.
    std::vector<std::string> got;
    const std::vector<Found> found = survey(app, kGroup9, &got);
    std::printf("      %zu widgets found\n", found.size());
    CHECK(found.size() >= 10u);
    std::sort(got.begin(), got.end());
    // Rescan; Stop on each of the three loaded rows and on the plate (for the module the plate shows); the tune
    // grant; the module's two keys. (The refused row has no key; Remove arms and sends nothing; the settings
    // grant is not offered to a module that has not asked.)
    std::vector<std::string> want = {
        expect("PLUGIN_RESCAN", 0, 0, ""),
        expect("DECODER_STOP", 0, 0, kGroup8),
        expect("DECODER_STOP", 0, 0, kGroup9),
        expect("DECODER_STOP", 0, 0, kGroup9),
        expect("DECODER_STOP", 0, 0, kStage1),
        expect("PLUGIN_GRANT", 1, 1, kGroup9),
        expect("PLUGIN_COMMAND", 1, 0, kGroup9),
        expect("PLUGIN_COMMAND", 2, 0, kGroup9),
    };
    std::sort(want.begin(), want.end());
    if (got != want) {
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : want) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(got == want);
    int arms = 0;
    for (const Found& fd : found) { arms += fd.armed == kGroup9 ? 1 : 0; }
    CHECK_EQ(arms, 1);  // one key arms the removal of the module the plate shows, and sends nothing
    std::printf("      %zu command(s) sent by the clicks and one arming of Remove, as expected\n", got.size());

    // A refused file's plate: no key to start it, Remove arms, nothing else but the rows'.
    got.clear();
    const std::vector<Found> bad = survey(app, kRefused, &got);
    int armsBad = 0;
    for (const Found& fd : bad) { armsBad += fd.armed == kRefused ? 1 : 0; }
    CHECK_EQ(armsBad, 1);
    std::sort(got.begin(), got.end());
    std::vector<std::string> wantBad = {
        expect("PLUGIN_RESCAN", 0, 0, ""),
        expect("DECODER_STOP", 0, 0, kGroup8),
        expect("DECODER_STOP", 0, 0, kGroup9),
        expect("DECODER_STOP", 0, 0, kStage1),
    };
    std::sort(wantBad.begin(), wantBad.end());
    CHECK(got == wantBad);
    std::printf("      the refused file's plate: %zu command(s), no key to start it, one arming of Remove\n", got.size());
}

// --- E -----------------------------------------------------------------------

const Found* spotFor(const std::vector<Found>& spots, const std::string& command) {
    for (const Found& s : spots) {
        if (s.command == command) { return &s; }
    }
    return nullptr;
}
const Found* armSpot(const std::vector<Found>& spots, const std::string& file) {
    for (const Found& s : spots) {
        if (s.armed == file) { return &s; }
    }
    return nullptr;
}

// A click that lands: the press with the engine's pass before it, and the release - where the key fires - with NO
// pass after it, so the caller can show nothing has happened yet. The next lookDraw is the engine's next pass.
void clickThenHold(AppWindow& app, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    lookDraw(app);
    lookDraw(app);
    io.AddMouseButtonEvent(0, true);
    lookDraw(app);
    io.AddMouseButtonEvent(0, false);
    drawFrame(app);
    io.AddMousePosEvent(2.0f, 2.0f);
}

void testControlsLandAtTheNextPass() {
    std::printf("  E: each control lands at the engine's next pass and the window reads the engine's value the frame after\n");
    Fresh f;
    AppWindow& app = f.app;
    lookDraw(app);
    lookDraw(app);
    std::vector<Found> spots = survey(app, kGroup9);
    Access::Sent sent;
    Access::tap(app, &sent);
    ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
    lookDraw(app);
    lookDraw(app);

    // STOP on the plate: nothing in the click's frame, the engine stopped at the next pass, the window shows it.
    {
        const Found* stop = spotFor(spots, expect("DECODER_STOP", 0, 0, kGroup9));
        CHECK(stop != nullptr);
        if (stop == nullptr) { return; }
        (void)Access::answers(app);
        lookDraw(app);
        sent.clear();
        clickThenHold(app, stop->x, stop->y);
        CHECK_EQ(sent.size(), 1u);
        CHECK(!Access::stopped(app, kGroup9));  // the click only queued it
        CHECK(!mod(app, kGroup9)->stopped);
        lookDraw(app);                          // the engine's next pass...
        CHECK(Access::stopped(app, kGroup9));
        CHECK(mod(app, kGroup9)->stopped);      // ...and the window reads it that same frame
        CHECK(!mod(app, kGroup9)->fed);         // a stopped module has no instance
        CHECK(mod(app, kGroup9)->commands.empty());  // its keys go with it
        const auto answers = Access::answers(app);
        CHECK_EQ(answers.size(), 1u);
        if (answers.size() == 1u) {
            CHECK_EQ(answers[0].first, static_cast<std::uint32_t>(FOXAPI_OP_DECODER_STOP));
            CHECK_EQ(answers[0].second, FOXAPI_OK);
        }
        // The same key is START now: pressed in the same place it starts it again.
        sent.clear();
        clickThenHold(app, stop->x, stop->y);
        CHECK_EQ(sent.size(), 1u);
        if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("DECODER_START", 0, 0, kGroup9)); }
        CHECK(Access::stopped(app, kGroup9));
        lookDraw(app);
        CHECK(!Access::stopped(app, kGroup9));
        CHECK(!mod(app, kGroup9)->stopped && mod(app, kGroup9)->fed);
        CHECK_EQ(mod(app, kGroup9)->commands.size(), 2u);  // the module's keys are back, as it attaches
        // THE PICTURE FOLLOWS THE ENGINE'S STATE. (A key that has been pressed keeps the focus, so the picture is
        // compared only with pictures drawn after the same clicks: the state is moved by commands that no click
        // sends, and the frame after each is drawn.)
        lookDraw(app);
        lookDraw(app);
        const std::uint64_t running = drawHash();
        Access::submit(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kGroup9));
        lookDraw(app);
        lookDraw(app);
        const std::uint64_t stoppedPicture = drawHash();
        Access::submit(app, cmd::makeText(FOXAPI_OP_DECODER_START, kGroup9));
        lookDraw(app);
        lookDraw(app);
        const std::uint64_t runningAgain = drawHash();
        CHECK(running != stoppedPicture);
        CHECK_EQ(running, runningAgain);
        (void)Access::answers(app);
        std::printf("      STOP then START: each queued by its click, applied at the next pass and read that frame; the picture follows (stopped differs from running: %s; running again is the same picture: %s)\n",
                    running != stoppedPicture ? "yes" : "no", running == runningAgain ? "yes" : "no");
    }

    // THE TUNE GRANT: the label flips with the engine's value, so a second press asks for the opposite.
    {
        const Found* grant = spotFor(spots, expect("PLUGIN_GRANT", 1, 1, kGroup9));
        CHECK(grant != nullptr);
        if (grant != nullptr) {
            sent.clear();
            clickThenHold(app, grant->x, grant->y);
            CHECK_EQ(sent.size(), 1u);
            CHECK(!Access::tuneAllowed(app, kGroup9) && !mod(app, kGroup9)->tuneAllowed);
            lookDraw(app);
            CHECK(Access::tuneAllowed(app, kGroup9) && mod(app, kGroup9)->tuneAllowed);
            sent.clear();
            clickThenHold(app, grant->x, grant->y);
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("PLUGIN_GRANT", 1, 0, kGroup9)); }
            lookDraw(app);
            CHECK(!Access::tuneAllowed(app, kGroup9) && !mod(app, kGroup9)->tuneAllowed);
            std::printf("      GRANT / REVOKE RECEIVER CONTROL: each press lands at the next pass, the key reads the engine's value\n");
        }
    }

    // THE RADIO-SETTINGS GRANT: offered only once the module has asked (the table's ASKED_SETTINGS), then as above.
    {
        CHECK(!mod(app, kGroup9)->settingsCapable);
        cc::PluginApiClient& c9 = Access::core(app).client(kGroup9, kNineName);
        cc::PluginControl ask;
        ask.kind = cc::PluginControl::Kind::Mode;
        ask.mode = CASCADE_DEMOD_NFM;
        CHECK_EQ(Access::core(app).requestControl(c9, ask), CASCADE_API_DENIED);
        lookDraw(app);
        CHECK(mod(app, kGroup9)->settingsCapable);
        // The key is new (and the module's own keys are further down the plate): found afresh.
        spots = survey(app, kGroup9);
        Access::tap(app, &sent);
        ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
        lookDraw(app);
        const Found* key = spotFor(spots, expect("PLUGIN_GRANT", 2, 1, kGroup9));
        CHECK(key != nullptr);
        if (key != nullptr) {
            sent.clear();
            clickThenHold(app, key->x, key->y);
            CHECK_EQ(sent.size(), 1u);
            CHECK(!Access::settingsAllowed(app, kGroup9));
            lookDraw(app);
            CHECK(Access::settingsAllowed(app, kGroup9) && mod(app, kGroup9)->settingsAllowed);
            sent.clear();
            clickThenHold(app, key->x, key->y);
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("PLUGIN_GRANT", 2, 0, kGroup9)); }
            lookDraw(app);
            CHECK(!Access::settingsAllowed(app, kGroup9) && !mod(app, kGroup9)->settingsAllowed);
            std::printf("      GRANT / REVOKE RADIO SETTINGS: offered after the module asked, then as the other grant\n");
        }
    }

    // A MODULE'S OWN KEY: accepted at the next pass (a press has no visible effect of its own); an id the module has
    // not listed is the engine's NOT_FOUND.
    {
        const Found* key = spotFor(spots, expect("PLUGIN_COMMAND", 2, 0, kGroup9));
        CHECK(key != nullptr);
        if (key != nullptr) {
            (void)Access::answers(app);
            sent.clear();
            clickThenHold(app, key->x, key->y);
            CHECK_EQ(sent.size(), 1u);
            lookDraw(app);
            const auto answers = Access::answers(app);
            CHECK_EQ(answers.size(), 1u);
            if (answers.size() == 1u) {
                CHECK_EQ(answers[0].first, static_cast<std::uint32_t>(FOXAPI_OP_PLUGIN_COMMAND));
                CHECK_EQ(answers[0].second, FOXAPI_OK);
            }
            Access::submit(app, cmd::makeText(FOXAPI_OP_PLUGIN_COMMAND, kGroup9, 99));
            Access::look(app);
            const auto none = Access::answers(app);
            CHECK_EQ(none.size(), 1u);
            if (none.size() == 1u) { CHECK_EQ(none[0].second, FOXAPI_NOT_FOUND); }
            std::printf("      CALIBRATE: accepted at the next pass; a key the module does not list: NOT_FOUND\n");
        }
    }

    // RESCAN: the whole list is replaced at the next pass; an armed Remove stays armed on its FILE.
    {
        junk("zz_second_refused.dll");
        const Found* arm = armSpot(spots, kGroup9);
        CHECK(arm != nullptr);
        if (arm != nullptr) {
            selectModule(app, kGroup9);
            clickThenHold(app, arm->x, arm->y);
            lookDraw(app);
            CHECK_EQ(Access::deck(app).confirmRemove, kGroup9);
        }
        const Found* rescan = spotFor(spots, expect("PLUGIN_RESCAN", 0, 0, ""));
        CHECK(rescan != nullptr);
        if (rescan != nullptr) {
            CHECK_EQ(Access::lists(app).model.modules.size(), 4u);
            sent.clear();
            clickThenHold(app, rescan->x, rescan->y);
            CHECK_EQ(sent.size(), 1u);
            CHECK_EQ(Access::lists(app).model.modules.size(), 4u);  // not in the click's frame
            lookDraw(app);
            lookDraw(app);
            CHECK_EQ(Access::lists(app).model.modules.size(), 5u);  // the file that was added is in the list
            CHECK(mod(app, "zz_second_refused.dll") != nullptr && !mod(app, "zz_second_refused.dll")->loaded);
            CHECK_EQ(Access::deck(app).confirmRemove, kGroup9);      // keyed on the file, not on a position
            CHECK(mod(app, kGroup9)->loaded && mod(app, kGroup9)->fed);
            std::printf("      RESCAN: the list replaced at the next pass (5 files now); the armed Remove still names its file\n");
        }
        Access::deck(app).confirmRemove.clear();
    }

    // REMOVE: two presses (arm, confirm); the file goes at the next pass, and its row and the sentence leave and
    // arrive in the SAME frame.
    {
        const std::string victim = "zz_second_refused.dll";
        spots = survey(app, victim);
        Access::tap(app, &sent);
        ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
        lookDraw(app);
        const Found* arm = armSpot(spots, victim);
        CHECK(arm != nullptr);
        if (arm != nullptr) {
            selectModule(app, victim);
            sent.clear();
            clickThenHold(app, arm->x, arm->y);
            CHECK(sent.empty());  // the first press sends nothing
            lookDraw(app);
            CHECK_EQ(Access::deck(app).confirmRemove, victim);
            sent.clear();
            clickThenHold(app, arm->x, arm->y);  // CONFIRM DELETE: the same key
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("STORE_REMOVE", 0, 0, victim)); }
            CHECK(std::filesystem::exists(g_plugins / victim));          // not in the click's frame
            CHECK(mod(app, victim) != nullptr);
            (void)Access::answers(app);
            lookDraw(app);                                               // the engine's next pass
            CHECK(!std::filesystem::exists(g_plugins / victim));
            CHECK(mod(app, victim) == nullptr);                          // the row is gone...
            CHECK_EQ(Access::lists(app).model.report, "Removed " + victim);  // ...and the sentence is up, in that frame
            CHECK(Access::lists(app).model.error.empty());
            CHECK(Access::deck(app).confirmRemove.empty());
            const auto answers = Access::answers(app);
            CHECK_EQ(answers.size(), 1u);
            if (answers.size() == 1u) { CHECK_EQ(answers[0].second, FOXAPI_OK); }
            CHECK_EQ(Access::lists(app).model.modules.size(), 4u);
            std::printf("      REMOVE: two presses; the file, its row and the sentence \"%s\" go at the next pass, in one frame\n",
                        Access::lists(app).model.report.c_str());
        }
    }
    Access::tap(app, nullptr);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_fitted_modules_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_fmw_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv(kNoCreate, "");
    // The three fixture modules, and a file that is not one, in a plugin folder of this test's own.
    g_plugins = g_scratch / "plugins";
    std::filesystem::create_directories(g_plugins);
    for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP8_PLUGIN, FIXTURE_GROUP9_PLUGIN}) {
        const std::filesystem::path p(src);
        std::error_code ec;
        std::filesystem::copy_file(p, g_plugins / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    junk(kRefused);
    Access::installHooks();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 2000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testModelEqualsTheOldWindow();
    junk(kRefused);
    testWhatTheWindowIsTold();
    junk(kRefused);
    testCacheFollowsListSeq();
    junk(kRefused);
    testDrawnAndClicked();
    junk(kRefused);
    testControlsLandAtTheNextPass();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_fitted_modules_window");
}
