// test_fitted_modules_window.cpp - THE FITTED MODULES WINDOW READS ITS ENGINE API SESSION (window port, group 9: drawFittedModulesWindow and
// fittedLists, app_window_fitted.cpp; engine-stage4.md 6.5 and 6.22 B5, API 0.3 sections 8.8, 9.3 - 9.5).
//
// REWRITTEN IN STUB CAMPAIGN B5 against master's 0.99.72 - 0.99.77 window. This was the port's own test of the 0.99.42 window (not master's: master
// has no such test), whose section A held the model built from the table equal to "the model the window built from the engine's own members before
// the port". That model has since grown eleven fields (the module's page, the IDLE state and KEEP RUNNING, where a file came from, its install
// record, the catalogue's category and record of it, the clean-up of old copies, the window a double-click opens), so section A now restates MASTER'S
// 0.99.72 builder - AppWindowTestAccess::masterModel, written from drawFittedModulesWindow@20647 of master (origin/master 04cba53a) and from nothing
// the new code does - and holds the table's model equal to it, field for field, in every state a module can be in. The five deliberate differences
// are stated where compared (the TRANSMIT plate, which is stub batch B13's and which no fixture here declares; the install record's catalogue
// key, which master worked out from the record and the table gives whole; the file's size, which is the engine's own measure when the table's block
// is made where master carried the scan's; the pictures, which are the store's cache's; and the catalogue's modules, held by id and count).
//
// What the window draws is the table's PLUGINS list and per-file facts, PLUGIN_COMMANDS, the store's old-copy facts, two status lines and the
// state's RX_RUNNING flag; what it changes it changes by commands on its session. This holds the window's side of that against the real engine with
// three REAL fixture modules fitted (tests/fixture_stage1_plugin.cpp, fixture_group8_plugin.cpp and fixture_group9_plugin.cpp: the last an audio
// processor with two command keys and a descriptor of its own) and a file the host refuses:
//   A  the model the window builds from the table, held equal field for field to master's 0.99.72 model, in every state a module can be in: fed,
//      stopped, ASLEEP, kept running, a grant of each kind, a notice, an integrity finding, a refused file, an orphan, an install record, a plugin
//      site and a site since removed, a page open
//   B  what the window is told: the modules, size, descriptor, notice, grants, MATCHED with the receiver stopped
//   C  the cache follows listSeq: one rebuild when it moves, none when a decoder prints a line, none while the window is closed - and a settled
//      frame allocates nothing (1000 reads of an unchanged state: 0)
//   D  the real window drawn: every widget of the list and of a module's page found, clicked, sends exactly the commands it should; the draw
//      changes when the state does and not otherwise
//   E  each control, clicked, lands at the engine's NEXT pass (not in the click's own frame) and the window reads the engine's value the frame
//      after: Stop, Start, the two grants, a module's command key, Rescan, Remove (with its two presses) and the sentence a remove leaves
//   F  THE RUN STATES (0.99.73): a module nothing uses is IDLE - listed, not failing, its key says START - and START and the page's KEEP RUNNING
//      tick pin it (PLUGIN_RUN_MODE / DECODER_START), unticking sends AUTO; a double-click on a row opens that plugin's own window
//
// THE TEST-ACCESSOR TRAP: a ported member reads the SESSION, so every frame here looks first, as drawUi does (the
// engine's pass takes the session's queue, publishes, the window reads its state), draws, and lets the NEXT look
// take what the draw sent. pumpTransmitKey is not run, so the session's answers are still there to be read.
//
// AGAINST A FREE-RUNNING PASS (FOXSDR_TEST_PASS=free, tests/pass_seam.hpp): every read or write of an engine field from
// this thread goes through passseam::on/read (run on the pass, with the pass out of the way); the survey's clicks are
// made with the window's session CLOSED, so the tap records what each click sends and nothing reaches the engine (a
// free pass would otherwise apply them - a RESCAN, a STOP - mid-survey); and where E says "the click only queued it"
// the pass is HELD from just before the click's release to the look after it (holdPass): that statement is about the
// pass not having run, which a free-running pass can only be made to keep by being held.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
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
#include "core/plugin_abi.h"
#include "core/plugin_cleanup.hpp"
#include "core/plugin_repo.hpp"
#include "core/plugin_run.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/plugin_window_ids.hpp"
#include "gui/plugins_view.hpp"
#include "gui/transmit_page.hpp"
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

// THE PASS HELD (free-running mode only; inline and stepped there is no pass of its own to hold): work posted to the pass
// that waits until it is let go, so from the moment holdPass returns until releasePass no pass runs - what E's "the click
// only queued it" needs. While it is held the engine's fields may be read from this thread (the pass thread is parked in
// the hold, ordered by its mutex: the same "pass out of the way" runOnPass gives). Released by the next look and by Fresh.
struct PassHold {
    std::mutex m;
    std::condition_variable cv;
    bool parked = false;
    bool released = false;
};
std::shared_ptr<PassHold> g_hold;

void holdPass(cascade::engine::Engine& e) {
    if (passseam::mode() != passseam::Mode::Free || g_hold != nullptr) { return; }
    passseam::attach(e);
    auto h = std::make_shared<PassHold>();
    g_hold = h;
    e.postToPass([h] {
        std::unique_lock<std::mutex> lk(h->m);
        h->parked = true;
        h->cv.notify_all();
        h->cv.wait(lk, [&] { return h->released; });
    });
    std::unique_lock<std::mutex> lk(h->m);
    h->cv.wait(lk, [&] { return h->parked; });
}
void releasePass() {
    if (g_hold == nullptr) { return; }
    {
        std::lock_guard<std::mutex> lk(g_hold->m);
        g_hold->released = true;
    }
    g_hold->cv.notify_all();
    g_hold.reset();
}
bool passHeld() { return g_hold != nullptr; }

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
        releasePass();
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            rawEngine(a.engine_).pumpInputs();
            rawEngine(a.engine_).pumpPlugins();
            (void)rawEngine(a.engine_).pumpPublish();
        });
        a.statusLines_ = rawEngine(a.engine_).statusText();
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
    static Engine& engine(AppWindow& a) { return rawEngine(a.engine_); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    // The survey's clicks are made with the session CLOSED (the tap records each submit before the session is asked), so
    // what a click sends is seen and never applied, whenever a pass runs; reopened after the click, with the state read.
    static void unplug(AppWindow& a) { a.closeUiSession(); }
    static void plugBack(AppWindow& a) {
        a.openUiSession();
        a.refreshUiState();
    }
    static void command(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) {
        passseam::on(rawEngine(a.engine_), [&] { (void)rawEngine(a.engine_).applyCommand(q.c, q.longText); });
    }
    static void command(AppWindow& a, const FoxCommand& c) {
        passseam::on(rawEngine(a.engine_), [&] { (void)rawEngine(a.engine_).applyCommand(c); });
    }
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

    // The engine, for comparison and for what a hermetic window cannot make. (Copies made on the pass, never a reference
    // into a member the pass rewrites.)
    static std::vector<cascade::core::LoadedPlugin> host(AppWindow& a) {
        return passseam::read<std::vector<cascade::core::LoadedPlugin>>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).pluginHost_.plugins(); });
    }
    static bool running(AppWindow& a) {
        return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).pipeline_.running(); });
    }
    static bool stopped(AppWindow& a, const std::string& key) {
        if (passHeld()) { return rawEngine(a.engine_).pluginIsStopped(key); }  // (the pass is parked in the hold)
        return passseam::read<bool>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).pluginIsStopped(key); });
    }
    // (both are the API core's grants, under its lock: any thread)
    static bool tuneAllowed(AppWindow& a, const std::string& key) { return rawPluginUi(*a.pluginFeed_).tuneAllowed(key); }
    static bool settingsAllowed(AppWindow& a, const std::string& key) { return rawPluginUi(*a.pluginFeed_).settingsAllowed(key); }
    // (the core is built with the engine and never replaced; client() and the plugin-side calls take its lock)
    static cascade::core::PluginApiCore& core(AppWindow& a) { return *rawEngine(a.engine_).pluginApi_; }
    static void addInventory(AppWindow& a, const cascade::core::InstalledPlugin& rec) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pluginInventory_.plugins.push_back(rec); });
    }
    static void clearInventory(AppWindow& a) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pluginInventory_.plugins.clear(); });
    }
    static void aLine(AppWindow& a, const std::string& plugin, const std::string& text) {
        passseam::on(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).decoderLog_.push_back({plugin, text});
            while (rawEngine(a.engine_).decoderLog_.size() > Engine::kDecoderLogMax) { rawEngine(a.engine_).decoderLog_.pop_front(); }
            ++rawEngine(a.engine_).decoderLinesTotal_;
        });
    }
    // An environment variable the fixture reads when the pass creates its instance, set on the pass.
    static void envOnPass(AppWindow& a, const char* name, const std::string& value) {
        passseam::on(rawEngine(a.engine_), [&] { setEnv(name, value); });
    }

    // STUB CAMPAIGN B5 (0.99.73, docs/engine-stage4.md 6.22): a fitted plugin runs only while it is USED, and in a hermetic frame nothing uses a decoder
    // - so a module nothing reads is ASLEEP (IDLE), which is a state this test holds on purpose (F, and A's first state). Where the test needs the
    // instances to run - FED, a decoder's command keys - the plugins are PINNED to ALWAYS ("keep running"), the state the product has for a plugin the
    // user keeps running (test_decoders_mute_grants.cpp does the same, for the same reason).
    static void pinPlugins(AppWindow& a) {
        Engine& e = rawEngine(a.engine_);
        passseam::on(e, [&] {
            std::vector<std::string> keys;
            for (const cascade::core::LoadedPlugin& p : e.pluginHost_.plugins()) {
                if (p.loaded) { keys.push_back(cascade::core::pluginKey(p)); }
            }
            for (const std::string& k : keys) { e.setPluginRun(k, cascade::core::PluginRun::Always); }
        });
    }
    static void resetDeck(AppWindow& a) { a.fittedDeck_ = std::make_unique<FittedModulesDeck>(); }
    static void addRecord(AppWindow& a, const cascade::core::InstalledPlugin& rec) { addInventory(a, rec); }
    static void setSites(AppWindow& a, const std::vector<std::string>& sites) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pluginSites_ = sites; });
    }
    // A catalogue entry, as the engine holds one after a read (catalogSeq_ is what moves the table's fingerprint for it).
    static void addCatalogue(AppWindow& a, const std::string& id, const std::string& name, const std::string& category, const std::string& source = {}) {
        passseam::on(rawEngine(a.engine_), [&] {
            cascade::core::PluginCatalogEntry ce;
            ce.id = id;
            ce.name = name;
            ce.version = "1.0.0";
            ce.author = "Catalogue Test Maker";
            ce.licence = "MIT";
            ce.category = category;
            ce.source = source;
            ce.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
            rawEngine(a.engine_).catalog_.push_back(ce);
            ++rawEngine(a.engine_).catalogSeq_;
        });
    }
    static void clearCatalogue(AppWindow& a) {
        passseam::on(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).catalog_.clear();
            ++rawEngine(a.engine_).catalogSeq_;
        });
    }
    static void setOldCopies(AppWindow& a, const std::vector<cascade::core::SupersededPlugin>& copies, const std::string& report) {
        passseam::on(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pluginOldCopies_ = copies;
            rawEngine(a.engine_).pluginCleanupReport_ = report;
        });
    }
    static FittedModulesDeck& openPage(AppWindow& a, const std::string& file) {
        resetDeck(a);
        a.fittedDeck_->pageFile = file;
        return *a.fittedDeck_;
    }

    // THE MODEL MASTER'S 0.99.72 WINDOW BUILT, every frame it was open, from the application's own members - restated from drawFittedModulesWindow@20647
    // of master (origin/master 04cba53a, src/gui/app_window.cpp) and from nothing in the code under test: the Engine's members carry master's names
    // (pluginIsIdle, pluginRunMode, catalog_, pluginSites_, pluginOldCopies_, pluginCleanupReport_, pluginInventory_), so each line below is master's
    // line with `this` read as the engine. `pageFile` is the module whose page is open ("" = the list): master built the store's whole model, and joined
    // every module to it by the catalogue key, only then. Built on the pass; `allocs`, when given, gets what the build allocated, counted on the thread
    // that built it.
    static FittedModulesModel masterModel(AppWindow& a, const std::string& pageFile = std::string(), long* allocs = nullptr) {
        return passseam::read<FittedModulesModel>(rawEngine(a.engine_), [&] {
            const long mark = t_allocs;
            FittedModulesModel m = masterModelOnPass(a, pageFile);
            if (allocs != nullptr) { *allocs += t_allocs - mark; }
            return m;
        });
    }
    // pluginCategoryOf, master's (app_window.cpp@23523): the install record's catalogue key, then the catalogue's entry's category.
    static std::string masterCategoryOf(Engine& e, const std::string& file) {
        for (const cascade::core::InstalledPlugin& ip : e.pluginInventory_.plugins) {
            if (!cascade::gui::equalsFileNameAscii(ip.file, file)) { continue; }
            if (ip.id.empty()) { return std::string(); }
            const std::string key = cascade::core::PluginRepo::catalogueKey(ip.source, ip.id);
            for (const cascade::core::PluginCatalogEntry& ce : e.catalog_) {
                if (cascade::core::PluginRepo::catalogueKey(ce.source, ce.id) == key) { return ce.category; }
            }
            return std::string();
        }
        return std::string();
    }
    static FittedModulesModel masterModelOnPass(AppWindow& a, const std::string& pageFile) {
        Engine& e = rawEngine(a.engine_);
        FittedModulesModel model;
        model.directory = e.pluginDir_;
        model.receiverRunning = e.pipeline_.running();
        const std::vector<cascade::core::DecoderStatus> status = e.pluginRunner_.status();
        const std::vector<cascade::core::LoadedPlugin>& list = e.pluginHost_.plugins();
        const std::vector<std::string> settingsAskers = e.pluginApi_->settingsRequesters();
        const std::vector<cascade::core::PluginFileVerdict> verdicts =
            cascade::core::classifyPluginFiles(list, e.pluginInventory_.plugins, e.catalog_);
        // THE CATALOGUE'S RECORDS of the page that is open: one a catalogue entry, in the catalogue's order, named by its catalogue key.
        std::vector<std::string> catalogueIds;
        if (!pageFile.empty()) {
            for (const cascade::core::PluginCatalogEntry& ce : e.catalog_) {
                catalogueIds.push_back(cascade::core::PluginRepo::catalogueKey(ce.source, ce.id));
            }
        }
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
            FittedModule m = makeFittedModule(p, e.pluginIsStopped(file), e.pluginRunner_.isFeeding(file), std::move(idleDetail),
                                              rawPluginUi(*a.pluginFeed_).tuneAllowed(cascade::core::PluginUi::tuneKey(p)), e.pluginIsIdle(file),
                                              e.pluginRunMode(file) == cascade::core::PluginRun::Always);
            m.settingsCapable = std::find(settingsAskers.begin(), settingsAskers.end(), file) != settingsAskers.end();
            m.settingsAllowed = rawPluginUi(*a.pluginFeed_).settingsAllowed(file);
            // MASTER'S TRANSMIT BLOCK (0.99.74; the table's side came back in stub batch B13): the grant offered to a module whose active() has been non-zero at
            // least once - or that already holds it - and what the plate says. No fixture here declares CASCADE_CAP_TX_AUDIO, so the three stay as makeFittedModule
            // left them and the table's model must hold the same; the key, the plate and the grant's round trip are tests/test_tx_audio_plate_app.cpp's (the real
            // binary, the transmitting fixture) and the bits the plate is made of are tests/test_engine_api_txplugin.cpp's.
            if ((p.capabilities & CASCADE_CAP_TX_AUDIO) != 0u) {
                const cascade::core::PluginRunner::TxPluginState ts = e.pluginRunner_.txState(file);
                m.txAllowed = std::find(e.pluginTransmitAllowed_.begin(), e.pluginTransmitAllowed_.end(), file) != e.pluginTransmitAllowed_.end();
                m.txCapable = ts.asked || m.txAllowed;
                const bool onAir = ts.holder && e.transmitter_.pluginKeyed() && e.txKeyedKey_ == file;
                m.txPlate = static_cast<int>(cascade::gui::txPlateState(ts.asked, m.txAllowed, e.txArming_.armed(), onAir, ts.waiting, ts.stopped));
            }
            for (const cascade::core::HostCommand& hc : e.pluginApi_->commands(file)) {
                m.commands.push_back(FittedModule::Command{hc.id, hc.label});
            }
            if (const auto nit = e.pluginNotices_.find(file); nit != e.pluginNotices_.end()) {
                m.noticeLevel = nit->second.level;
                m.notice = nit->second.text;
            }
            m.integrityNote = cascade::core::PluginRepo::changedSinceInstallNote(e.pluginInventory_.plugins, file);
            for (const cascade::core::PluginFileVerdict& v : verdicts) {
                if (v.file == file) {
                    m.orphaned = v.orphaned();
                    break;
                }
            }
            for (const cascade::core::InstalledPlugin& ip : e.pluginInventory_.plugins) {
                if (!cascade::gui::equalsFileNameAscii(ip.file, file)) { continue; }
                m.abiVersion = ip.abiVersion;
                m.fittedAtUnix = ip.installedAtUnix;
                if (!ip.id.empty()) {
                    m.catalogueId = cascade::core::PluginRepo::catalogueKey(ip.source, ip.id);
                    m.id = m.catalogueId;
                }
                if (!ip.source.empty()) {
                    const std::string label = cascade::core::PluginRepo::siteLabel(ip.source);
                    const bool stillListed = std::find(e.pluginSites_.begin(), e.pluginSites_.end(), ip.source) != e.pluginSites_.end();
                    m.originNote = cascade::core::formatText(
                        stillListed ? "Installed from %s, not from the FoxSDR catalogue." : "Installed from %s, a site you removed. It gets no updates.",
                        label.c_str());
                }
                break;
            }
            m.category = masterCategoryOf(e, file);
            if (!m.catalogueId.empty() && !catalogueIds.empty()) {
                for (int ci = 0; ci < static_cast<int>(catalogueIds.size()); ++ci) {
                    if (catalogueIds[static_cast<std::size_t>(ci)] == m.catalogueId) {
                        m.catalogueIndex = ci;
                        break;
                    }
                }
            }
            // THE SIZE IS THE RECORD'S, and makeFittedModule has already carried it (LoadedPlugin::fileBytes).
            model.modules.push_back(std::move(m));
        }
        for (const cascade::core::SupersededPlugin& o : e.pluginOldCopies_) {
            model.oldCopies.push_back({o.name, o.version, o.file, o.keptVersion});
        }
        model.cleanupReport = e.pluginCleanupReport_;
        return model;
    }
    static bool openOwn(AppWindow& a, const std::string& file) { return a.openPluginOwnWindow(file); }
    static bool shown(AppWindow& a, const std::string& id) { return a.pluginWindows_.shown(id); }
    static auto& pluginWindows(AppWindow& a) { return a.pluginWindows_; }
    static std::vector<std::string> trackers(AppWindow& a) { return a.declaredTrackPluginNames(); }
    static const AppWindow::PluginListCache& rows(AppWindow& a) { return a.pluginLists(); }
    static std::string fact(AppWindow& a, const std::string& key) {
        std::string s;
        a.readFactText(key.c_str(), s);
        return s;
    }
    static std::vector<std::string> masterCatalogueIds(AppWindow& a) {
        return passseam::read<std::vector<std::string>>(rawEngine(a.engine_), [&] {
            std::vector<std::string> ids;
            for (const cascade::core::PluginCatalogEntry& ce : rawEngine(a.engine_).catalog_) {
                ids.push_back(cascade::core::PluginRepo::catalogueKey(ce.source, ce.id));
            }
            return ids;
        });
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::FittedModule;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

// A plugin FILE name on this platform: what PluginHost::hasPluginExtension scans for and PluginRepo's kModuleExtension
// admits (".dll" on Windows, ".so" on Linux, ".dylib" on macOS), behind the prefix the build gives a module (CMake's
// shared-library prefix: none on Windows, "lib" elsewhere, as FIXTURE_*_PLUGIN's own names have it). The prefix keeps the
// host's by-file-name order the same on every platform: "zz_..." sorts after the fixtures as "fixture_..." and as
// "libfixture_..." alike. On Windows every name comes out as it always was. (Names the engine never scans or judges as a
// file - a display name, another platform's build, a module that is not there - stay as they are.)
#if defined(_WIN32)
constexpr const char* kModulePrefix = "";
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModulePrefix = "lib";
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModulePrefix = "lib";
constexpr const char* kModuleExt = ".so";
#endif
std::string moduleFile(const std::string& stem) { return kModulePrefix + stem + kModuleExt; }

const std::string kStage1 = fileOf(FIXTURE_STAGE1_PLUGIN);
const std::string kGroup8 = fileOf(FIXTURE_GROUP8_PLUGIN);
const std::string kGroup9 = fileOf(FIXTURE_GROUP9_PLUGIN);
const std::string kRefused = moduleFile("zz_refused");
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
    // `pin`: the fitted modules are kept running (ALWAYS), the state the product has for a plugin the user keeps - so their instances exist and
    // are FED, which B to E read; F and the first state of A are the ones that leave them to the engine's own decision (AUTO: asleep).
    explicit Fresh(bool pin = true) {
        Access::look(app);
        Access::look(app);
        if (pin) {
            Access::pinPlugins(app);
            Access::look(app);
            Access::look(app);
        }
        // A window tall enough for the list AND the plate beside it (the display is made to fit it).
        Access::setRect(app, 20, 20, 1560, 1900);
        Access::setOpen(app, true);
    }
    ~Fresh() { releasePass(); }  // (a held pass must not outlive the window's teardown, which stops the thread)
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
    // ...and the eleven fields master's 0.99.72 - 0.99.77 window added to the model (stub campaign B5). The run state is the table's for a module the host
    // LOADED (PLUGINS value[3]; a refused file has none, and the panel never reads it for one).
    check(got.idle == (want.loaded && want.idle), "idle");
    check(got.keepRunning == (want.loaded && want.keepRunning), "keepRunning");
    check(got.ownWindow == want.ownWindow, "ownWindow");
    check(got.originNote == want.originNote, "originNote");
    check(got.orphaned == want.orphaned, "orphaned");
    check(got.id == want.id, "id");
    check(got.catalogueId == want.catalogueId, "catalogueId");
    check(got.category == want.category, "category");
    check(got.abiVersion == want.abiVersion, "abiVersion");
    check(got.fittedAtUnix == want.fittedAtUnix, "fittedAtUnix");
    check(got.catalogueIndex == want.catalogueIndex, "catalogueIndex");
    // (The TRANSMIT grant and plate: the table carries them since stub batch B13 - the wanted side restates master's block above. No fixture here declares the
    // bit, so all three stay at their defaults on both sides.)
    check(got.txCapable == want.txCapable && got.txAllowed == want.txAllowed && got.txPlate == want.txPlate, "tx*");
    bool sameKeys = got.commands.size() == want.commands.size();
    for (std::size_t i = 0; sameKeys && i < got.commands.size(); ++i) {
        sameKeys = got.commands[i].id == want.commands[i].id && got.commands[i].label == want.commands[i].label;
    }
    check(sameKeys, "commands");
    return ok;
}

// The whole model against master's 0.99.72 one, module by module, and the things beside the modules: the clean-up's old copies and report, and - while
// a page is open (`pageFile`) - the catalogue's records its modules are joined to.
int sameModel(AppWindow& a, const char* when, const std::string& pageFile = std::string()) {
    Access::look(a);
    // The window's own frame, so the catalogue copy a page needs is the one it made (a window draws before its model is read in the shipped
    // loop; here the page is opened on the deck and the model is asked for the way the draw asks).
    if (!pageFile.empty()) {
        Access::openPage(a, pageFile);
        Access::setOpen(a, true);
        lookDraw(a);
        lookDraw(a);
    }
    const cascade::gui::FittedModulesModel want = Access::masterModel(a, pageFile);
    const cascade::gui::FittedModulesModel& got = Access::lists(a).model;
    CHECK_EQ(got.modules.size(), want.modules.size());
    CHECK_EQ(got.directory, want.directory);
    CHECK_EQ(got.receiverRunning, want.receiverRunning);
    int compared = 0;
    for (std::size_t i = 0; i < got.modules.size() && i < want.modules.size(); ++i) {
        if (sameModule(got.modules[i], want.modules[i], when)) { ++compared; }
        // ...and the same strip: the census the window prints is a function of the model
        CHECK_EQ(static_cast<int>(cascade::gui::fittedState(got.modules[i], got.receiverRunning)),
                 static_cast<int>(cascade::gui::fittedState(want.modules[i], want.receiverRunning)));
    }
    const cascade::gui::FittedCounts a1 = cascade::gui::countStates(got.modules, got.receiverRunning);
    const cascade::gui::FittedCounts b1 = cascade::gui::countStates(want.modules, want.receiverRunning);
    CHECK(a1.fed == b1.fed && a1.notFed == b1.notFed && a1.noSignal == b1.noSignal && a1.stopped == b1.stopped &&
          a1.refused == b1.refused && a1.total == b1.total && a1.idle == b1.idle);
    // The old copies and the report of the last clean-up.
    bool sameCopies = got.oldCopies.size() == want.oldCopies.size();
    for (std::size_t i = 0; sameCopies && i < got.oldCopies.size(); ++i) {
        sameCopies = got.oldCopies[i].name == want.oldCopies[i].name && got.oldCopies[i].version == want.oldCopies[i].version &&
                     got.oldCopies[i].file == want.oldCopies[i].file && got.oldCopies[i].keptVersion == want.oldCopies[i].keptVersion;
    }
    CHECK(sameCopies);
    CHECK_EQ(got.cleanupReport, want.cleanupReport);
    // THE CATALOGUE'S RECORDS (DELIBERATE DIFFERENCE: held by count and id. Master built the store's whole model for the page, which the store window's
    // own test, test_plugin_store_window.cpp A, holds equal to master's field for field; this window copies the store's cache's.) Only while a page is
    // open, and then one a catalogue entry in the catalogue's order.
    const std::vector<std::string> ids = pageFile.empty() ? std::vector<std::string>() : Access::masterCatalogueIds(a);
    CHECK_EQ(got.catalogue.size(), ids.size());
    for (std::size_t i = 0; i < got.catalogue.size() && i < ids.size(); ++i) { CHECK_EQ(got.catalogue[i].id, ids[i]); }
    return compared;
}

// --- A -----------------------------------------------------------------------

void testModelEqualsMastersModel() {
    std::printf("  A: the model the window draws from the table, equal to the one master's 0.99.72 window built from the engine's members\n");
    Fresh f(/*pin=*/false);
    AppWindow& app = f.app;
    int compared = 0;
    int states = 0;
    const auto same = [&](const char* when, const std::string& page = std::string()) {
        compared += sameModel(app, when, page);
        ++states;
    };
    same("four files fitted, nothing using a decoder, the receiver stopped");
    CHECK_EQ(Access::lists(app).model.modules.size(), 4u);  // three modules and the file the host refuses
    // ASLEEP (0.99.73): the two decoders are AUTO and nothing uses them, so they have no instance and the window says IDLE; the audio processor is in
    // use for as long as it is fitted (a standing duty), and a refused file has no run state. The refused file is an ORPHAN: no index knows it and
    // nothing runs it.
    CHECK(mod(app, kStage1)->idle && mod(app, kGroup8)->idle);
    CHECK(!mod(app, kGroup9)->idle && !mod(app, kRefused)->idle);
    CHECK(!mod(app, kStage1)->keepRunning && !mod(app, kGroup8)->keepRunning);
    CHECK(mod(app, kRefused)->orphaned && !mod(app, kStage1)->orphaned && !mod(app, kGroup9)->orphaned);
    CHECK(cascade::gui::fittedState(*mod(app, kStage1), false) == cascade::gui::FittedState::Idle);
    CHECK(cascade::gui::countStates(Access::lists(app).model.modules, false).idle == 2);

    // KEPT RUNNING: pinned to ALWAYS, which is what START and the page's tick do. Nothing is IDLE then, and the window says the module is kept.
    Access::pinPlugins(app);
    Access::look(app);
    Access::look(app);
    same("the three modules kept running");
    CHECK(mod(app, kStage1)->keepRunning && mod(app, kGroup8)->keepRunning && !mod(app, kStage1)->idle && !mod(app, kGroup8)->idle);

    // The receiver running: the decoders and the processor are fed.
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
    Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 1));
    Access::look(app);
    Access::look(app);
    CHECK(Access::running(app));
    same("the receiver running");
    CHECK(mod(app, kGroup9)->fed && mod(app, kStage1)->fed && mod(app, kGroup8)->fed);
    CHECK(!mod(app, kRefused)->fed);

    // One stopped.
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kGroup9));
    Access::look(app);
    Access::look(app);
    same("one stopped");
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
    same("a tune grant, a request, a warning");
    CHECK(mod(app, kGroup9)->tuneAllowed && mod(app, kGroup9)->settingsCapable && !mod(app, kGroup9)->settingsAllowed);
    CHECK_EQ(mod(app, kGroup9)->noticeLevel, static_cast<std::uint32_t>(CASCADE_LOG_WARN));
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup9, 2, 1));
    CHECK_EQ(Access::core(app).log(c9, CASCADE_LOG_ERROR, std::string(300, 'e').c_str()), CASCADE_API_OK);
    Access::look(app);
    Access::look(app);
    same("a radio-settings grant, an error of 300 characters");
    CHECK(mod(app, kGroup9)->settingsAllowed);
    CHECK_EQ(mod(app, kGroup9)->noticeLevel, static_cast<std::uint32_t>(CASCADE_LOG_ERROR));
    CHECK_EQ(mod(app, kGroup9)->notice.size(), 300u);  // whole: a list item's detail would cut it at 159

    // A finding of the install record's re-hash, and the record itself: the catalogue key the record gives the module, the plugin ABI the fitted
    // build was made for, and when it was fitted.
    cc::InstalledPlugin rec;
    rec.id = "group-nine-fixture";
    rec.name = kNineName;
    rec.file = kGroup9;
    rec.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    rec.installedAtUnix = 1790000000;
    rec.digestMismatch = true;
    Access::addRecord(app, rec);
    Access::look(app);
    Access::look(app);
    same("an integrity finding and an install record");
    CHECK(mod(app, kGroup9)->integrityNote.find("is not the file that was installed") != std::string::npos);
    CHECK(mod(app, kGroup8)->integrityNote.empty());
    CHECK_EQ(mod(app, kGroup9)->catalogueId, std::string("group-nine-fixture"));
    CHECK_EQ(mod(app, kGroup9)->id, std::string("group-nine-fixture"));
    CHECK_EQ(mod(app, kGroup9)->abiVersion, static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
    CHECK_EQ(mod(app, kGroup9)->fittedAtUnix, static_cast<std::int64_t>(1790000000));
    CHECK(mod(app, kGroup8)->catalogueId.empty() && mod(app, kGroup8)->id == kGroup8);  // a hand-fitted module keeps its file name for an id
    CHECK(mod(app, kGroup9)->category.empty());                                         // the catalogue was never read
    CHECK(!mod(app, kGroup9)->orphaned);                                                 // an install record makes a file the index's

    // THE CATALOGUE'S CATEGORY, and its record of the module when the module's page is open (master built the store's whole model then).
    Access::addCatalogue(app, "group-nine-fixture", kNineName, "meters");
    Access::addCatalogue(app, "another-plugin", "Another Plugin", "marine");
    Access::look(app);
    Access::look(app);
    same("a catalogue that knows the module, no page open");
    CHECK_EQ(mod(app, kGroup9)->category, std::string("meters"));
    CHECK(Access::lists(app).model.catalogue.empty() && mod(app, kGroup9)->catalogueIndex == -1);
    same("the same, the module's page open", kGroup9);
    CHECK_EQ(Access::lists(app).model.catalogue.size(), 2u);
    CHECK_EQ(mod(app, kGroup9)->catalogueIndex, 0);
    CHECK(mod(app, kGroup8)->catalogueIndex == -1);
    Access::resetDeck(app);
    lookDraw(app);
    CHECK(Access::lists(app).model.catalogue.empty());  // the page shut: the copy goes, and every index with it
    CHECK(mod(app, kGroup9)->catalogueIndex == -1);

    // WHERE A MODULE CAME FROM (0.99.74): installed from a plugin site that is still listed, and from one the user has since removed. The refused
    // file is given a record too, so it is no longer an orphan (and the verdict word is the record's).
    cc::InstalledPlugin site;
    site.id = "stage-one";
    site.source = "https://plugins.example.org/index.json";
    site.name = "Stage One";
    site.file = kStage1;
    Access::addRecord(app, site);
    cc::InstalledPlugin known;
    known.id = "refused-one";
    known.name = "Refused One";
    known.file = kRefused;
    Access::addRecord(app, known);
    Access::setSites(app, {site.source});
    Access::look(app);
    Access::look(app);
    same("a module installed from a plugin site that is listed; the refused file recorded");
    CHECK(mod(app, kStage1)->originNote.find("Installed from ") == 0u && mod(app, kStage1)->originNote.find("plugins.example.org") != std::string::npos);
    CHECK(mod(app, kStage1)->originNote.find("not from the FoxSDR catalogue") != std::string::npos);
    CHECK_EQ(mod(app, kStage1)->catalogueId, std::string("stage-one@plugins.example.org"));  // the key of a site's plugin: "<id>@<site label>"
    CHECK(!mod(app, kRefused)->orphaned);
    Access::setSites(app, {});
    Access::look(app);
    Access::look(app);
    same("the same site removed");
    CHECK(mod(app, kStage1)->originNote.find("a site you removed. It gets no updates.") != std::string::npos);
    CHECK(mod(app, kGroup9)->originNote.empty());

    // THE OLD COPIES an update left behind, and what the last clean-up did.
    cc::SupersededPlugin old;
    old.file = kRefused;
    old.name = kNineName;
    old.version = "9.0.0";
    old.keptFile = kGroup9;
    old.keptVersion = "9.1.0";
    Access::setOldCopies(app, {old}, "Removed 1 old copy");
    Access::look(app);
    Access::look(app);
    same("an old copy and the clean-up's report");
    CHECK_EQ(Access::lists(app).model.oldCopies.size(), 1u);
    CHECK_EQ(Access::lists(app).model.cleanupReport, std::string("Removed 1 old copy"));
    Access::setOldCopies(app, {}, "");
    Access::clearInventory(app);
    Access::clearCatalogue(app);

    // Idle: the processor cannot start. The runner's sentence, whole.
    Access::envOnPass(app, kNoCreate, "1");
    Access::command(app, cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
    Access::look(app);
    Access::look(app);
    Access::look(app);
    same("an audio processor that cannot start");
    CHECK(!mod(app, kGroup9)->fed);
    CHECK(mod(app, kGroup9)->idleDetail.find("failed to start its audio processor") != std::string::npos);
    CHECK(mod(app, kGroup8)->idleDetail.empty());
    Access::envOnPass(app, kNoCreate, "");
    Access::command(app, cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
    Access::look(app);
    Access::look(app);
    Access::look(app);
    same("started again after a rescan");
    CHECK(mod(app, kGroup9)->fed && mod(app, kGroup9)->idleDetail.empty());
    // (a rescan keeps what the plugin asked for: the flag is sticky)
    CHECK(mod(app, kGroup9)->settingsCapable);
    std::printf("      %d modules held equal to master's, field for field, in %d states\n", compared, states);
    CHECK_EQ(compared, 4 * states);

    // The one conversion beside the table's, held on a real host's records: the builder over a row against the builder over a LoadedPlugin
    // (what makeFittedModule is still used for), for every record - and the window a double-click opens, which the row works out from the capability
    // word and master worked out from the table pointers the host kept.
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
        CHECK(newM.ownWindow == oldM.ownWindow);
        CHECK_EQ(newM.id, oldM.id);
    }
    // ...and the rules of the row builder on their own: the host-client key only for a LOADED module that declares
    // the bit; the name only when a descriptor was read (the table fills `name` with the file's own for a file that
    // was refused first); the module's keys are not the row's; and the run state is the row's.
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
        r.runMode = 1;
        r.asleep = true;
        FittedModule m = cascade::gui::makeFittedModuleFromRow(r);
        CHECK(m.tuneCapable && m.loaded && m.name == "X" && m.fed && m.stopped && m.sizeBytes == 12345u);
        CHECK(m.noticeLevel == 3u && m.notice == "n" && m.integrityNote == "i" && m.idleDetail == "w");
        CHECK(m.settingsCapable && m.settingsAllowed && m.tuneAllowed && m.commands.empty());
        CHECK(m.idle && m.keepRunning && m.id == "x.dll");
        CHECK(m.ownWindow == cascade::gui::FittedOwnWindow::None);  // a text decoder has no window of its own: the Decoder output window
        // (the window a double-click opens: the panel, else the picture, else the instrument, else the map, and none for a module that is not loaded)
        const auto windowOf = [&](std::uint32_t caps, bool loaded) {
            r.caps = caps;
            r.loaded = loaded;
            return cascade::gui::makeFittedModuleFromRow(r).ownWindow;
        };
        using W = cascade::gui::FittedOwnWindow;
        CHECK(windowOf(CASCADE_CAP_PANEL | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_TRACK_SOURCE, true) == W::Panel);
        CHECK(windowOf(CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_TRACK_SOURCE, true) == W::Picture);
        CHECK(windowOf(CASCADE_CAP_INSTRUMENT | CASCADE_CAP_TRACK_SOURCE, true) == W::Instrument);
        CHECK(windowOf(CASCADE_CAP_TRACK_SOURCE, true) == W::Map);
        CHECK(windowOf(CASCADE_CAP_PANEL, false) == W::None);
        // A refused module is neither asleep nor kept running, whatever the row's run word said.
        r.loaded = false;
        r.error = "refused";
        CHECK(!cascade::gui::makeFittedModuleFromRow(r).tuneCapable);  // (a duplicate the host turned off)
        CHECK(!cascade::gui::makeFittedModuleFromRow(r).idle && !cascade::gui::makeFittedModuleFromRow(r).keepRunning);
        r.loaded = true;
        r.caps = CASCADE_CAP_DECODER;
        CHECK(!cascade::gui::makeFittedModuleFromRow(r).tuneCapable);
        r.caps = 0u;
        r.name = "x.dll";
        CHECK(cascade::gui::makeFittedModuleFromRow(r).name.empty());
        // written into a record that has a name, keys and an origin: nothing of them is left
        FittedModule used;
        used.name = "old";
        used.commands.push_back(FittedModule::Command{7u, "old"});
        used.originNote = "old";
        used.orphaned = true;
        used.catalogueId = "old";
        used.catalogueIndex = 3;
        cascade::gui::fillFittedModule(used, r);
        CHECK(used.name.empty() && used.commands.empty() && used.originNote.empty() && !used.orphaned && used.catalogueId.empty() &&
              used.catalogueIndex == -1);
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
    Access::command(app, cmd::makeText(FOXAPI_OP_STORE_REMOVE, moduleFile("no-such-file")));
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
    Access::command(app, cmd::makeText(FOXAPI_OP_STORE_REMOVE, moduleFile("no-such-file")));
    Access::look(app);
    Access::look(app);
    (void)Access::lists(app);
    before = t_allocs;
    for (int i = 0; i < 1000; ++i) { sink += Access::lists(app).model.error.size(); }
    spent = t_allocs - before;
    std::printf("      1000 reads with the install sentence up: %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);

    // What master's window DID cost, every frame it was open, for the record: the same four files, its model built from the engine's members.
    long masterAllocs = 0;
    const int frames = 100;
    for (int i = 0; i < frames; ++i) { sink += Access::masterModel(app, std::string(), &masterAllocs).modules.size(); }
    const long masterPerFrame = masterAllocs / frames;
    std::printf("      master's model, built every frame from the engine's members: %ld allocations a frame\n", masterPerFrame);
    CHECK(masterPerFrame > 0);

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
    std::string page;        // the module whose page the click opened ("" the list)
    float x, y;
};

// THE LIST (`file` empty) or the page of the module `file` (0.99.72: the window shows one or the other), in a fresh deck; `armed` is the file whose
// REMOVE has been pressed once, so the list draws CONFIRM in its row.
void selectModule(AppWindow& app, const std::string& file, const std::string& armed = std::string()) {
    cascade::gui::FittedModulesDeck& deck = Access::openPage(app, file);
    if (!armed.empty()) {
        deck.confirmRemove = armed;
        deck.confirmUntil = ImGui::GetTime() + 1.0e9;
    }
    lookDraw(app);
}

// Every widget of the window - the list, or the page of `file` - found and clicked once in a fresh deck each time, and what
// the click did: the command it sent (nothing is applied: no pass runs), the removal it armed and the page it opened. The
// caller's tap is dropped; the caller puts its own back.
std::vector<Found> survey(AppWindow& app, const std::string& file, std::vector<std::string>* commandsSent = nullptr,
                          const std::string& armed = std::string()) {
    Access::Sent sent;
    Access::tap(app, &sent);
    selectModule(app, file, armed);
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
        selectModule(app, file, armed);
        sent.clear();
        Access::unplug(app);
        clickNoPass(app, s.x, s.y);
        Found fd{"", Access::deck(app).confirmRemove == armed ? std::string() : Access::deck(app).confirmRemove, Access::deck(app).pageFile, s.x, s.y};
        for (const auto& q : sent) {
            if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
            fd.command = describe(q);
            if (commandsSent != nullptr) { commandsSent->push_back(fd.command); }
        }
        found.push_back(fd);
        Access::plugBack(app);
    }
    Access::tap(app, nullptr);
    putBack();
    selectModule(app, file, armed);
    selectModule(app, file, armed);
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

    // THE LIST (0.99.72: the window shows the list, or one module's page - not both): every widget found, clicked once in a fresh deck, and what each did.
    // The top bar's SCAN AGAIN (a rescan) and RESET WINDOW SIZES (the window's own: no command), the six chips and the search field (filters: no command);
    // in each row the glyph and the name (they open the module's page), REMOVE (it arms, and sends nothing) and the module's key - STOP for a module that
    // runs, which is every loaded one here (the fixtures are kept running), none for the file the host refused.
    std::vector<std::string> got;
    const std::vector<Found> list = survey(app, std::string(), &got);
    std::printf("      the list: %zu widgets found\n", list.size());
    CHECK(list.size() >= 20u);
    std::sort(got.begin(), got.end());
    std::vector<std::string> want = {
        expect("PLUGIN_RESCAN", 0, 0, ""),
        expect("DECODER_STOP", 0, 0, kGroup8),
        expect("DECODER_STOP", 0, 0, kGroup9),
        expect("DECODER_STOP", 0, 0, kStage1),
    };
    std::sort(want.begin(), want.end());
    if (got != want) {
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : want) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(got == want);
    // One REMOVE a file (four rows), each arming its own file; the glyph and the name of each row open that module's page.
    std::set<std::string> armedFiles, openedPages;
    for (const Found& fd : list) {
        if (!fd.armed.empty()) { armedFiles.insert(fd.armed); }
        if (!fd.page.empty()) { openedPages.insert(fd.page); }
    }
    CHECK(armedFiles == (std::set<std::string>{kStage1, kGroup8, kGroup9, kRefused}));
    CHECK(openedPages == (std::set<std::string>{kStage1, kGroup8, kGroup9, kRefused}));
    std::printf("      %zu command(s) sent by the clicks, one arming of Remove and one page for each of the four rows, as expected\n", got.size());

    // A REMOVE ALREADY PRESSED ONCE shows CONFIRM in the row, and that key sends the removal of that file (and no other).
    got.clear();
    (void)survey(app, std::string(), &got, kGroup9);
    CHECK(std::count(got.begin(), got.end(), expect("STORE_REMOVE", 0, 0, kGroup9)) == 1);
    CHECK(std::count_if(got.begin(), got.end(), [](const std::string& s) { return s.rfind("STORE_REMOVE", 0) == 0; }) == 1);

    // THE PAGE of a module: the key in its header (STOP), the receiver-control grant, the module's two keys and the top bar's SCAN AGAIN. (The
    // settings grant is not offered to a module that has not asked, and the audio processor has no KEEP RUNNING tick: it is in use for as long as it
    // is fitted. REMOVE is the list's.)
    got.clear();
    const std::vector<Found> page = survey(app, kGroup9, &got);
    std::printf("      the page of %s: %zu widgets found\n", kGroup9.c_str(), page.size());
    CHECK(page.size() >= 10u);
    std::sort(got.begin(), got.end());
    std::vector<std::string> wantPage = {
        expect("PLUGIN_RESCAN", 0, 0, ""),
        expect("DECODER_STOP", 0, 0, kGroup9),
        expect("PLUGIN_GRANT", 1, 1, kGroup9),
        expect("PLUGIN_COMMAND", 1, 0, kGroup9),
        expect("PLUGIN_COMMAND", 2, 0, kGroup9),
    };
    std::sort(wantPage.begin(), wantPage.end());
    if (got != wantPage) {
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : wantPage) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(got == wantPage);
    for (const Found& fd : page) { CHECK(fd.armed.empty()); }

    // A refused file's page: no key to start it (a refused module has nothing to start) and nothing but the top bar's.
    got.clear();
    const std::vector<Found> bad = survey(app, kRefused, &got);
    for (const Found& fd : bad) { CHECK(fd.armed.empty()); }
    std::sort(got.begin(), got.end());
    CHECK(got == std::vector<std::string>{expect("PLUGIN_RESCAN", 0, 0, "")});
    std::printf("      the refused file's page: %zu command(s), no key to start it\n", got.size());
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
// (Free-running, the pass is held from before the release to that look: holdPass.)
void clickThenHold(AppWindow& app, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    lookDraw(app);
    lookDraw(app);
    io.AddMouseButtonEvent(0, true);
    lookDraw(app);
    holdPass(Access::engine(app));
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

    // RESCAN: the whole list is replaced at the next pass; an armed Remove stays armed on its FILE. (REMOVE is the list's key: the surveys of the list
    // find where each row's REMOVE and the top bar's SCAN AGAIN are.)
    {
        const std::vector<Found> listSpots = survey(app, std::string());
        Access::tap(app, &sent);
        ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
        lookDraw(app);
        junk(moduleFile("zz_second_refused"));
        const Found* arm = armSpot(listSpots, kGroup9);
        CHECK(arm != nullptr);
        if (arm != nullptr) {
            selectModule(app, std::string());
            clickThenHold(app, arm->x, arm->y);
            lookDraw(app);
            CHECK_EQ(Access::deck(app).confirmRemove, kGroup9);
        }
        const Found* rescan = spotFor(listSpots, expect("PLUGIN_RESCAN", 0, 0, ""));
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
            CHECK(mod(app, moduleFile("zz_second_refused")) != nullptr && !mod(app, moduleFile("zz_second_refused"))->loaded);
            CHECK_EQ(Access::deck(app).confirmRemove, kGroup9);      // keyed on the file, not on a position
            CHECK(mod(app, kGroup9)->loaded && mod(app, kGroup9)->fed);
            std::printf("      RESCAN: the list replaced at the next pass (5 files now); the armed Remove still names its file\n");
        }
        Access::deck(app).confirmRemove.clear();
    }

    // REMOVE: two presses (arm, confirm); the file goes at the next pass, and its row and the sentence leave and
    // arrive in the SAME frame. (The list's REMOVE arms; CONFIRM takes the place of the row's STOP key beside it, so its place is found with the row
    // armed.) TWO KINDS OF FILE: one the plugin index does not know and nothing runs is an ORPHAN (0.99.69) - its key says REMOVE FILE and the engine
    // judges it again against its own scan (FOXAPP_OP_STORE_CLEAN_OLD, ival[0] 1) - and every other module is removed by FOXAPI_OP_STORE_REMOVE.
    const auto removeFlow = [&](const std::string& victim, const std::string& wantedCommand, const char* what) {
        const std::vector<Found> plain = survey(app, std::string());
        const std::vector<Found> armedList = survey(app, std::string(), nullptr, victim);
        Access::tap(app, &sent);
        ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
        lookDraw(app);
        const Found* arm = armSpot(plain, victim);
        const Found* confirm = spotFor(armedList, wantedCommand);
        if (arm == nullptr || confirm == nullptr) {
            std::printf("        no %s: the list's widgets, armed files and commands:\n", arm == nullptr ? "arm" : "confirm");
            for (const Found& fd : armedList) {
                std::printf("          armed  armed=[%s] page=[%s] cmd=[%s] at %.0f,%.0f\n", fd.armed.c_str(), fd.page.c_str(), fd.command.c_str(), fd.x, fd.y);
            }
        }
        CHECK(arm != nullptr && confirm != nullptr);
        if (arm != nullptr && confirm != nullptr) {
            selectModule(app, std::string());
            sent.clear();
            clickThenHold(app, arm->x, arm->y);
            CHECK(sent.empty());  // the first press sends nothing
            lookDraw(app);
            CHECK_EQ(Access::deck(app).confirmRemove, victim);
            sent.clear();
            clickThenHold(app, confirm->x, confirm->y);  // CONFIRM
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), wantedCommand); }
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
            std::printf("      REMOVE (%s): two presses; the file, its row and the sentence \"%s\" go at the next pass, in one frame\n", what,
                        Access::lists(app).model.report.c_str());
        }
    };
    {
        // AN ORPHAN: the file the last rescan added, which no record names.
        const std::string victim = moduleFile("zz_second_refused");
        CHECK(mod(app, victim) != nullptr && mod(app, victim)->orphaned);
        removeFlow(victim, expect("APP_STORE_CLEAN_OLD", 1, 0, victim), "an orphan: REMOVE FILE");
        CHECK_EQ(Access::lists(app).model.modules.size(), 4u);
    }
    {
        // A MODULE THE INDEX KNOWS (an install record names the file): the ordinary remove.
        const std::string victim = moduleFile("zz_third_refused");
        junk(victim);
        Access::command(app, cmd::make(FOXAPI_OP_PLUGIN_RESCAN));
        Access::look(app);
        Access::look(app);
        cc::InstalledPlugin rec;
        rec.id = "third-refused";
        rec.name = "Third Refused";
        rec.file = victim;
        Access::addRecord(app, rec);
        Access::look(app);
        Access::look(app);
        CHECK(mod(app, victim) != nullptr && !mod(app, victim)->orphaned && !mod(app, victim)->loaded);
        removeFlow(victim, expect("STORE_REMOVE", 0, 0, victim), "a recorded module");
        CHECK_EQ(Access::lists(app).model.modules.size(), 4u);
    }
    Access::tap(app, nullptr);
}

// --- F -----------------------------------------------------------------------

int countOf(const std::vector<std::string>& v, const std::string& s) { return static_cast<int>(std::count(v.begin(), v.end(), s)); }

void testRunStatesAndOwnWindows() {
    std::printf("  F: a module nothing uses is IDLE (START and the tick pin it); a module's own windows are named from what it declares\n");
    {
        Fresh f(/*pin=*/false);
        AppWindow& app = f.app;
        lookDraw(app);
        lookDraw(app);
        const FittedModule* s1 = mod(app, kStage1);
        CHECK(s1 != nullptr);
        if (s1 == nullptr) { return; }
        // IDLE is not STOPPED and not failing: the module is listed, its sentence says it starts by itself, and its key offers START.
        CHECK(s1->idle && !s1->keepRunning && !s1->stopped);
        CHECK(cascade::gui::fittedState(*s1, false) == cascade::gui::FittedState::Idle);
        CHECK(cascade::gui::fittedOffersStart(*s1));
        CHECK_EQ(std::string(cascade::gui::fittedKeyWord(*s1)), std::string("START"));

        // THE LIST: a START for each of the two sleeping decoders, a STOP for the processor that is in use whatever the screen shows.
        std::vector<std::string> got;
        const std::vector<Found> list = survey(app, std::string(), &got);
        CHECK_EQ(countOf(got, expect("DECODER_START", 0, 0, kStage1)), 1);
        CHECK_EQ(countOf(got, expect("DECODER_START", 0, 0, kGroup8)), 1);
        CHECK_EQ(countOf(got, expect("DECODER_STOP", 0, 0, kGroup9)), 1);
        CHECK_EQ(countOf(got, expect("DECODER_STOP", 0, 0, kStage1)), 0);

        // THE PAGE: the header's key is START too, and the KEEP RUNNING tick is there, unticked - for a decoder, which can be idle; the audio
        // processor has none (it is in use for as long as it is fitted).
        got.clear();
        std::vector<Found> spots = survey(app, kStage1, &got);
        CHECK_EQ(countOf(got, expect("DECODER_START", 0, 0, kStage1)), 1);
        if (countOf(got, expect("APP_PLUGIN_RUN_MODE", 1, 0, kStage1)) != 1) {
            for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        }
        CHECK_EQ(countOf(got, expect("APP_PLUGIN_RUN_MODE", 1, 0, kStage1)), 1);
        got.clear();
        (void)survey(app, kGroup9, &got);
        CHECK(std::none_of(got.begin(), got.end(), [](const std::string& s) { return s.rfind("APP_PLUGIN_RUN_MODE", 0) == 0; }));

        // THE TICK PRESSED: queued by the click, applied at the next pass - the module is kept running, no longer IDLE (its decoder is made, nothing
        // using it) - and the tick is ticked, so the next press asks for AUTO.
        const Found* tick = spotFor(spots, expect("APP_PLUGIN_RUN_MODE", 1, 0, kStage1));
        CHECK(tick != nullptr);
        if (tick == nullptr) { return; }
        Access::Sent sent;
        Access::tap(app, &sent);
        ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
        selectModule(app, kStage1);
        lookDraw(app);
        sent.clear();
        clickThenHold(app, tick->x, tick->y);
        CHECK_EQ(sent.size(), 1u);
        if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("APP_PLUGIN_RUN_MODE", 1, 0, kStage1)); }
        CHECK(mod(app, kStage1)->idle && !mod(app, kStage1)->keepRunning);  // the click only queued it
        (void)Access::answers(app);
        lookDraw(app);
        CHECK(mod(app, kStage1)->keepRunning && !mod(app, kStage1)->idle);
        {
            const auto answers = Access::answers(app);
            CHECK_EQ(answers.size(), 1u);
            if (answers.size() == 1u) {
                CHECK_EQ(answers[0].first, static_cast<std::uint32_t>(FOXAPP_OP_PLUGIN_RUN_MODE));
                CHECK_EQ(answers[0].second, FOXAPI_OK);
            }
        }
        CHECK_EQ(Access::fact(app, "fact.plugin.dir").empty() ? 1 : 0, 0);  // (the facts are read: the table is answering)
        Access::tap(app, nullptr);
        got.clear();
        spots = survey(app, kStage1, &got);
        CHECK_EQ(countOf(got, expect("APP_PLUGIN_RUN_MODE", 0, 0, kStage1)), 1);  // ticked: the next press UNticks
        CHECK_EQ(countOf(got, expect("DECODER_STOP", 0, 0, kStage1)), 1);     // and the header's key is STOP
        // UNTICKED: back to AUTO. The plugin was running and nothing has wanted it for no time at all, so it is not put out at once - the clock
        // (30 s) does that, which the engine's tests hold (tests/test_engine_api_03.cpp group N) - and the window says so by not saying IDLE yet.
        const Found* untick = spotFor(spots, expect("APP_PLUGIN_RUN_MODE", 0, 0, kStage1));
        CHECK(untick != nullptr);
        if (untick != nullptr) {
            Access::tap(app, &sent);
            ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
            selectModule(app, kStage1);
            lookDraw(app);
            sent.clear();
            clickThenHold(app, untick->x, untick->y);
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("APP_PLUGIN_RUN_MODE", 0, 0, kStage1)); }
            lookDraw(app);
            CHECK(!mod(app, kStage1)->keepRunning && !mod(app, kStage1)->stopped);
            Access::tap(app, nullptr);
        }

        // START on the other sleeping decoder's ROW pins it: DECODER_START is the user's own press, which the engine takes as ALWAYS.
        const Found* start = spotFor(list, expect("DECODER_START", 0, 0, kGroup8));
        CHECK(start != nullptr);
        if (start != nullptr) {
            Access::tap(app, &sent);
            ImGui::GetIO().AddMousePosEvent(2.0f, 2.0f);
            selectModule(app, std::string());
            lookDraw(app);
            sent.clear();
            clickThenHold(app, start->x, start->y);
            CHECK_EQ(sent.size(), 1u);
            CHECK(mod(app, kGroup8)->idle);  // not in the click's frame
            lookDraw(app);
            CHECK(mod(app, kGroup8)->keepRunning && !mod(app, kGroup8)->idle && !mod(app, kGroup8)->stopped);
            Access::tap(app, nullptr);
        }
        std::printf("      IDLE: START on a row and the page's tick pin the module (PLUGIN_RUN_MODE ALWAYS), unticking asks for AUTO; each lands at the next pass\n");
    }

    // A MODULE'S OWN WINDOWS are named from what it declares: the group-14 fixture is a track source that also declares a panel and an instrument.
    {
        const std::filesystem::path src(FIXTURE_GROUP14_PLUGIN);
        const std::string file = src.filename().string();
        std::error_code ec;
        std::filesystem::copy_file(src, g_plugins / file, std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        Fresh f(/*pin=*/false);
        AppWindow& app = f.app;
        lookDraw(app);
        lookDraw(app);
        const FittedModule* g14 = mod(app, file);
        CHECK(g14 != nullptr);
        if (g14 != nullptr) {
            // Asleep (a track source nothing uses), and still a module that opens its own window: the panel, before the instrument and the map.
            CHECK(g14->loaded && g14->idle);
            CHECK(g14->ownWindow == cascade::gui::FittedOwnWindow::Panel);
            CHECK(cascade::gui::fittedOpensOwnWindow(*g14));
            // The table's own statements of it.
            CHECK_EQ(Access::fact(app, "fact.plugin." + file + ".windows"), std::string("panel\tGroup Fourteen Panel\ninstrument\tGroup Fourteen Instrument"));
            CHECK(Access::fact(app, "fact.plugin." + file + ".record").empty());  // hand-fitted: no install record
            CHECK(Access::fact(app, "fact.plugin." + kStage1 + ".windows").empty() && Access::fact(app, "fact.plugin." + kStage1 + ".record").empty());
            CHECK_EQ(Access::fact(app, "fact.plugin." + kRefused + ".windows"), std::string());  // a refused file declares nothing
            const auto& rows = Access::rows(app);
            const cascade::gui::PluginRow* row = nullptr;
            for (const cascade::gui::PluginRow& r : rows.plugins) {
                if (r.file == file) { row = &r; }
            }
            CHECK(row != nullptr);
            if (row != nullptr) {
                CHECK_EQ(row->panelTitle, std::string("Group Fourteen Panel"));
                CHECK_EQ(row->instrumentTitle, std::string("Group Fourteen Instrument"));
                CHECK(row->asleep && row->category.empty());
                CHECK_EQ(row->runId, cc::pluginRunId(file));
            }
            // THE TRACK SOURCES THE TABLE DECLARES, asleep or not: the map page of a tracker nothing uses must exist for its row to wake it.
            CHECK(Access::trackers(app) == std::vector<std::string>{"Group Fourteen Tracks"});

            // THE DOUBLE-CLICK: the panel's window is marked shown by its declared title, on a plugin with no instance yet - which is what the
            // report of what the window shows (and so the engine's wake) is made from.
            const std::string panelId = cascade::gui::panelWindowId("Group Fourteen Tracks", "Group Fourteen Panel");
            CHECK(!Access::shown(app, panelId));
            CHECK(Access::openOwn(app, file));
            CHECK(Access::shown(app, panelId));
            CHECK(!Access::shown(app, cascade::gui::instrumentWindowIdFor("Group Fourteen Tracks", "Group Fourteen Instrument")));  // one window, not all
            // A module with no window of its own opens none (its page is what the double-click gives).
            CHECK(!Access::openOwn(app, kStage1));
            CHECK(!Access::openOwn(app, moduleFile("no-such-module")));
            // A STOPPED plugin has no instances, so nothing is shown for it and it declares no tracker.
            Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, file));
            Access::look(app);
            Access::look(app);
            CHECK(mod(app, file)->stopped);
            Access::pluginWindows(app).hide(panelId);
            CHECK(!Access::openOwn(app, file));
            CHECK(!Access::shown(app, panelId));
            CHECK(Access::trackers(app).empty());
            std::printf("      a track source with a panel and an instrument, asleep: declared as a map, its panel named \"Group Fourteen Panel\", opened by the double-click's call and wakeable; stopped: none\n");
        }
        std::filesystem::remove(g_plugins / file, ec);
    }
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

    testModelEqualsMastersModel();
    junk(kRefused);
    testWhatTheWindowIsTold();
    junk(kRefused);
    testCacheFollowsListSeq();
    junk(kRefused);
    testDrawnAndClicked();
    junk(kRefused);
    testControlsLandAtTheNextPass();
    junk(kRefused);
    testRunStatesAndOwnWindows();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_fitted_modules_window");
}
