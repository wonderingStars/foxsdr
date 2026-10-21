// test_plugin_store_window.cpp - THE PLUGIN STORE READS ITS ENGINE API SESSION (window port, group 10: the rail's store
// key, the plugins key, the retired rows, the store window and storeLists; engine-stage4.md 6.5, API 0.3 section 8.9).
//
// What the store draws is the table's CATALOGUE (with its ABI, size and BUILD_HERE), twelve facts an entry, PLUGINS and
// the facts of the host's record, STORE_UPDATES, STORE_BLOCKED and the two facts of each retired row, STORE_ACTIVITY
// (the transfer, read live, and the ADD ALL run) and the status lines the engine hands over; what it changes it
// changes by nine commands on its session. This holds the window's side of that against the real engine, over a REAL
// fetch of a local index.json (an address with no "://" is read from disk) laid over a plugin folder of this test's own:
// a retired module with an update plan, an updatable fitted one, and five entries for the columns (the world of
// test_engine_api_03's group I).
//   A  the model the window draws from the table, held equal field for field to the one it built from the engine's own
//      members before the port (the code it replaced, restated here), in every state: before a fetch, after one, with
//      a row selected and the notice ticked or not, a module stopped, a grant, a transfer in flight, a failure
//   B  what the window is told, entry by entry, from the index: ABI, size, builds, the gate, the plan, the host's
//      record, the retired row's sentence and id, the rail's counts
//   C  the cache: one rebuild when listSeq moves, the full tier only while the window is drawn, the live progress,
//      and the allocations of a settled frame
//   D  the real window drawn and clicked: every command it can send goes out with the right operands, and the retired
//      rows' Update, Remove and Confirm on the rail
//   E  a click lands at the engine's NEXT pass and the window reads the engine's value the frame after
//   F  what the engine used to call into the window for: the consents cleared on the window's own thread, from the
//      table - a fetch begun, a run ended, a different catalogue - and the ADD ALL run's line through a real run
//
// THE TEST-ACCESSOR TRAP: a ported member reads the SESSION, so every frame here looks first, as drawUi does (the
// engine's pass takes the session's queue, publishes, the window copies the status lines and reads its state), draws,
// and lets the NEXT look take what the draw sent; the workers are polled after the draw, as drawUi polls them.
// pumpTransmitKey is not run, so the session's answers are still there to be read.
//
// AGAINST A FREE-RUNNING PASS (FOXSDR_TEST_PASS=free, tests/pass_seam.hpp): every read or write of an engine field from
// this thread goes through passseam::on/read (run on the pass, with the pass out of the way). Where a statement is about
// ONE publish - "the click only queued it", the survey's "nothing is applied: no pass runs", "a fetch began, seen" and the
// ADD ALL run lettered publish by publish - the pass is PARKED at its publish (Engine::TestHooks::betweenPublishes, set
// before any thread runs): the state and the lists of that publish are the newest, the engine's fields are as they were
// published (the pass's workers have not run), and while it is parked they are read here directly.
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
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <set>
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
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/plugin_store_view.hpp"
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

const char* const kWindowName = "Plugin store###pluginstorewindow";
const char* const kRailName = "storerail";

// --- the index documents (the world of test_engine_api_03's group I) ---------------------------------------------
struct IdxPlatform {
    std::string os, arch, file, sha;
    unsigned long long size = 0;
};
struct IdxEntry {
    std::string id, name, version, author, licence, summary, description, homepage, legal, floor;
    unsigned abi = CASCADE_PLUGIN_ABI_VERSION;
    std::vector<IdxPlatform> platforms;
};

std::string hex64(char c) { return std::string(64, c); }

// The texts are plain ASCII with no quotes, so no escaping is needed.
std::string indexJson(const std::vector<IdxEntry>& v) {
    std::string j = "{\"schemaVersion\":1,\"plugins\":[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        const IdxEntry& e = v[i];
        j += i == 0 ? "{" : ",{";
        j += "\"id\":\"" + e.id + "\",\"name\":\"" + e.name + "\",\"version\":\"" + e.version + "\"";
        j += ",\"author\":\"" + e.author + "\",\"licence\":\"" + e.licence + "\",\"summary\":\"" + e.summary + "\"";
        j += ",\"description\":\"" + e.description + "\",\"homepage\":\"" + e.homepage + "\"";
        j += ",\"legalNotice\":\"" + e.legal + "\",\"minSupportedVersion\":\"" + e.floor + "\"";
        j += ",\"abiVersion\":" + std::to_string(e.abi) + ",\"platforms\":[";
        for (std::size_t k = 0; k < e.platforms.size(); ++k) {
            const IdxPlatform& p = e.platforms[k];
            j += k == 0 ? "{" : ",{";
            j += "\"os\":\"" + p.os + "\",\"arch\":\"" + p.arch + "\",\"file\":\"" + p.file + "\"";
            j += ",\"url\":\"https://example.invalid/" + p.file + "\",\"sha256\":\"" + p.sha + "\"";
            if (p.size != 0) { j += ",\"sizeBytes\":" + std::to_string(p.size); }
            j += "}";
        }
        j += "]}";
    }
    return j + "]}";
}

IdxPlatform hostBuild(const std::string& file, char shaChar, unsigned long long size) {
    return IdxPlatform{cascade::core::PluginRepo::hostOs(), cascade::core::PluginRepo::hostArch(), file, hex64(shaChar), size};
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// THE PASS PARKED AT ITS PUBLISH (free-running mode only: inline and stepped no pass runs beside the test). The hook runs
// on the pass's thread between the state's publish (the lists are handed over just before it) and the status lines';
// armed, it parks there - when `when` holds, evaluated on the pass - until it is let go. Asked for again while one is
// parked, the next publish is armed BEFORE the parked one goes, so a walk publish by publish misses none.
struct PublishGate {
    std::mutex m;
    std::condition_variable cv;
    bool armed = false;
    bool parked = false;
    std::function<bool()> when;
};
PublishGate g_gate;

void betweenPublishes() {
    std::unique_lock<std::mutex> lk(g_gate.m);
    if (!g_gate.armed || (g_gate.when && !g_gate.when())) { return; }
    g_gate.armed = false;
    g_gate.parked = true;
    g_gate.cv.notify_all();
    g_gate.cv.wait(lk, [] { return !g_gate.parked; });
}
bool parked() {
    std::lock_guard<std::mutex> lk(g_gate.m);
    return g_gate.parked;
}
void parkAtPublish(cascade::engine::Engine& e, std::function<bool()> when = {}) {
    if (passseam::mode() != passseam::Mode::Free) { return; }
    passseam::attach(e);
    std::unique_lock<std::mutex> lk(g_gate.m);
    g_gate.when = std::move(when);
    g_gate.armed = true;
    if (g_gate.parked) {
        g_gate.parked = false;
        g_gate.cv.notify_all();
    }
    g_gate.cv.wait(lk, [] { return g_gate.parked; });
    g_gate.when = nullptr;
}
void unpark() {
    std::lock_guard<std::mutex> lk(g_gate.m);
    g_gate.armed = false;
    g_gate.when = nullptr;
    if (g_gate.parked) {
        g_gate.parked = false;
        g_gate.cv.notify_all();
    }
}

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
        Engine::testHooks_.betweenPublishes = &betweenPublishes;  // (idle unless a free-running test parks a publish)
    }

    // An engine read or write from this thread: on the pass (passseam), or right here while the pass is parked.
    template <class R>
    static R onPass(AppWindow& a, const std::function<R()>& f) {
        return parked() ? f() : passseam::read<R>(rawEngine(a.engine_), f);
    }
    static void doOnPass(AppWindow& a, const std::function<void()>& f) {
        if (parked()) {
            f();
        } else {
            passseam::on(rawEngine(a.engine_), f);
        }
    }
    // The pass parked at its next publish (free-running; nothing otherwise) - for the click, the survey's clicks, the
    // ADD ALL walk - and let go by the next look, the next workers' poll or dropQueued.
    static void park(AppWindow& a) { parkAtPublish(rawEngine(a.engine_)); }
    // ...at the first publish of a fetch in flight.
    static void parkAtFetch(AppWindow& a) {
        parkAtPublish(rawEngine(a.engine_), [&a] { return rawEngine(a.engine_).catalogPending_; });
    }
    // THE LOOK OF ONE PUBLISH: free-running, the pass parked at its next publish (the one parked now let go) and that
    // publish read; otherwise the look. With its `after`: free-running nothing (the parked pass polls its workers when it
    // goes on), otherwise the workers.
    static void lookAtPublish(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free) {
            look(a);
            return;
        }
        parkAtPublish(rawEngine(a.engine_));
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    static void afterPublish(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free) { after(a); }
    }
    // The look at the publish of the fetch just submitted (free-running: parked there; otherwise the look).
    static void lookAtFetch(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free) {
            look(a);
            return;
        }
        parkAtFetch(a);
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }

    // One frame's engine part, in drawUi's order: the session's queue taken, the publish, the status copy, the
    // window's read of the state.
    static void look(AppWindow& a) {
        unpark();
        passseam::pass(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            rawEngine(a.engine_).pumpInputs();
            rawEngine(a.engine_).pumpPlugins();
            (void)rawEngine(a.engine_).pumpPublish();
        });
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    // ...and the end of the frame: the workers polled (a fetch or a download that finished is applied, the ADD ALL
    // queue steps) and the lines handed over, as drawUi does after the draw.
    static void after(AppWindow& a) {
        unpark();
        passseam::workers(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpWorkers();
            rawEngine(a.engine_).pumpAudio();
        });
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
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static void dropQueued(AppWindow& a) {
        a.closeUiSession();
        a.openUiSession();
        a.refreshUiState();
        unpark();  // (what was queued is gone: the pass may go on)
    }
    static void command(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) {
        doOnPass(a, [&] { (void)rawEngine(a.engine_).applyCommand(q.c, q.longText); });
    }
    static void command(AppWindow& a, const FoxCommand& c) {
        doOnPass(a, [&] { (void)rawEngine(a.engine_).applyCommand(c); });
    }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static std::size_t loadedDecoders(AppWindow& a) {
        return onPass<std::size_t>(a, [&] { return rawEngine(a.engine_).loadedDecoderCount(); });
    }
    static std::size_t fedDecoders(AppWindow& a) {
        return onPass<std::size_t>(a, [&] { return rawEngine(a.engine_).fedDecoderCount(); });
    }

    // The window's own: its cache, its deck, its open flag, the flag the fitted window reads.
    static StoreListCache& lists(AppWindow& a, bool full = true) { return a.storeLists(full); }
    static StoreListCache& cache(AppWindow& a) { return *a.storeCache_; }
    static PluginStoreDeck& deck(AppWindow& a) { return *a.pluginStoreDeck_; }
    static void setOpen(AppWindow& a, bool open) { a.pluginBrowseOpen_ = open; }
    static bool isOpen(AppWindow& a) { return a.pluginBrowseOpen_; }
    static bool storeDrew(AppWindow& a) { return a.pluginBrowserDrawnThisFrame_; }
    static const std::string& blockedConfirm(AppWindow& a) { return a.blockedRemoveConfirmFile_; }
    static void setBlockedConfirm(AppWindow& a, const std::string& f) { a.blockedRemoveConfirmFile_ = f; }
    static void drawStore(AppWindow& a) { a.drawPluginStoreWindow(); }
    static void drawRail(AppWindow& a) {
        a.drawPluginStoreSection();
        a.drawPluginsSection();
    }
    static void setStatusLines(AppWindow& a, const cascade::engine::EngineStatusText& t) { a.statusLines_ = t; }
    static const cascade::engine::EngineStatusText& statusLines(AppWindow& a) { return a.statusLines_; }
    static std::string& urlBuf(AppWindow& a, std::size_t& cap) {
        static std::string s;
        s = a.pluginUrlBuf_;
        cap = sizeof(a.pluginUrlBuf_);
        return s;
    }
    static void setUrlBuf(AppWindow& a, const std::string& v) { std::snprintf(a.pluginUrlBuf_, sizeof(a.pluginUrlBuf_), "%s", v.c_str()); }

    // The engine, for comparison and for what a hermetic window cannot make (by value: never a reference into a member
    // the pass rewrites).
    static std::size_t catalogSize(AppWindow& a) { return onPass<std::size_t>(a, [&] { return rawEngine(a.engine_).catalog_.size(); }); }
    static bool catalogPending(AppWindow& a) { return onPass<bool>(a, [&] { return rawEngine(a.engine_).catalogPending_; }); }
    static bool installPending(AppWindow& a) { return onPass<bool>(a, [&] { return rawEngine(a.engine_).installPending_; }); }
    static bool addAllActive(AppWindow& a) { return onPass<bool>(a, [&] { return rawEngine(a.engine_).addAllRun_.active; }); }
    static std::size_t addAllNext(AppWindow& a) { return onPass<std::size_t>(a, [&] { return rawEngine(a.engine_).addAllRun_.next; }); }
    static std::size_t addAllTotal(AppWindow& a) { return onPass<std::size_t>(a, [&] { return rawEngine(a.engine_).addAllRun_.total; }); }
    static std::string engineUrl(AppWindow& a) { return onPass<std::string>(a, [&] { return rawEngine(a.engine_).pluginCatalogueUrl_; }); }
    static std::string engineSummary(AppWindow& a) { return onPass<std::string>(a, [&] { return rawEngine(a.engine_).addAllSummary_; }); }
    // What writes the engine's lines (the table's own hook is not in an engine the window holds): set where the engine
    // keeps them, handed over at the end of the next frame.
    static void setAddAllFailed(AppWindow& a, bool v) { doOnPass(a, [&] { rawEngine(a.engine_).addAllFailed_ = v; }); }
    static void setCatalogStatus(AppWindow& a, const std::string& v) { doOnPass(a, [&] { rawEngine(a.engine_).catalogStatus_ = v; }); }
    static void setAddAllSummary(AppWindow& a, const std::string& v) { doOnPass(a, [&] { rawEngine(a.engine_).addAllSummary_ = v; }); }
    static std::string blockedReason(AppWindow& a, int idx, bool acked) {
        return onPass<std::string>(a, [&] { return rawEngine(a.engine_).pluginInstallBlockedReason(idx, acked); });
    }
    static bool stopped(AppWindow& a, const std::string& key) { return onPass<bool>(a, [&] { return rawEngine(a.engine_).pluginIsStopped(key); }); }
    static std::vector<cascade::core::LoadedPlugin> host(AppWindow& a) {
        return onPass<std::vector<cascade::core::LoadedPlugin>>(a, [&] { return rawEngine(a.engine_).pluginHost_.plugins(); });
    }
    static std::vector<cascade::core::BlockedPlugin> engineBlocked(AppWindow& a) {
        return onPass<std::vector<cascade::core::BlockedPlugin>>(a, [&] { return rawEngine(a.engine_).pluginBlocked_; });
    }
    static std::vector<cascade::core::PluginUpdate> plans(AppWindow& a) {
        return onPass<std::vector<cascade::core::PluginUpdate>>(a, [&] { return rawEngine(a.engine_).plannedPluginUpdates(); });
    }

    // A transfer as its worker would have left it (test_engine_api_03, group I): the engine's install slot holds a
    // promise the test keeps, so a download is in flight (installPending_, the busy name) until the test says it is
    // over, with a result the pump applies exactly as it applies a real worker's. Nothing touches the network.
    static std::promise<Engine::PluginInstallResult>& installPromise() {
        static std::promise<Engine::PluginInstallResult> p;
        return p;
    }
    static void beginTransfer(AppWindow& a, const std::string& name) {
        doOnPass(a, [&] {
            installPromise() = std::promise<Engine::PluginInstallResult>();
            rawEngine(a.engine_).installFuture_ = installPromise().get_future();
            rawEngine(a.engine_).installBusyName_ = name;
            rawEngine(a.engine_).installPending_ = true;
        });
    }
    static void endTransfer(bool ok, const std::string& name, const std::string& error) {
        Engine::PluginInstallResult r;
        r.ok = ok;
        r.name = name;
        r.error = error;
        installPromise().set_value(std::move(r));
    }
    // (the repo's progress is an atomic, stored by a worker in the product: any thread)
    static void setProgress(AppWindow& a, float p) { rawEngine(a.engine_).pluginRepo_.setProgressForTest(p); }

    // THE MODEL AS IT WAS BEFORE THE PORT: built from the engine's own members, every frame the window was open (the
    // code AppWindow::buildPluginStoreModel held from the day it was written to the day it was ported, restated). The
    // ADD ALL line is the one place it is NOT restated: it lettered min(next + 1, total), which reads one too many
    // (API 0.3 8.9, "the position is the module in flight"), and the test says what the true number is instead. Built
    // on the pass; `allocs`, when given, gets what the build allocated, counted on the thread that built it.
    static PluginStoreModel oldModel(AppWindow& a, long* allocs = nullptr) {
        return onPass<PluginStoreModel>(a, [&] {
            const long mark = t_allocs;
            PluginStoreModel m = oldModelOnPass(a);
            if (allocs != nullptr) { *allocs += t_allocs - mark; }
            return m;
        });
    }
    static PluginStoreModel oldModelOnPass(AppWindow& a) {
        Engine& e = rawEngine(a.engine_);
        PluginStoreModel model;
        model.sourceUrl = e.pluginCatalogueUrl_;
        model.haveCatalogue = !e.catalog_.empty();
        model.sourceStatus = e.catalogStatus_;
        model.sourceError = a.statusLines_.catalogError;
        model.busy = e.catalogPending_ || e.installPending_;
        model.progress = e.pluginRepo_.progress();
        if (e.installPending_) {
            model.busyLabel = cascade::core::formatText(cascade::i18n::tr("downloading %s"), e.installBusyName_.c_str());
        } else {
            model.busyLabel = e.catalogPending_ ? std::string(cascade::i18n::tr("fetching the catalogue")) : std::string();
        }
        model.resultReport = e.installReport_;
        model.resultError = e.installError_;
        model.addAllRunning = e.addAllRun_.active;
        model.addAllProgress = trueAddAllLine(a);
        model.addAllSummary = e.addAllSummary_;
        model.addAllFailed = e.addAllFailed_;
        const std::vector<cascade::core::PluginUpdate> updates = e.plannedPluginUpdates();
        for (int i = 0; i < static_cast<int>(e.catalog_.size()); ++i) {
            const cascade::core::PluginCatalogEntry& ce = e.catalog_[static_cast<std::size_t>(i)];
            StoreModule sm;
            sm.id = ce.id;
            sm.plate.name = ce.name;
            sm.plate.version = ce.version;
            sm.plate.maker = ce.author;
            sm.plate.licence = ce.licence;
            sm.plate.blurb = ce.description.empty() ? ce.summary : ce.description;
            sm.plate.summary = ce.summary;
            sm.plate.homepage = ce.homepage;
            sm.plate.legalNotice = ce.legalNotice;
            sm.plate.haveAbi = true;
            sm.plate.abiVersion = ce.abiVersion;
            sm.plate.hostAbiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
            sm.plate.retirementFloor = ce.minSupportedVersion;
            for (const cascade::core::PluginPlatform& pf : ce.platforms) {
                if (!sm.plate.platforms.empty()) { sm.plate.platforms += ", "; }
                sm.plate.platforms += pf.os + "/" + pf.arch;
            }
            const cascade::core::PluginPlatform* plat = ce.thisPlatform();
            if (plat != nullptr && plat->sizeBytes > 0u) {
                sm.plate.haveSizeBytes = true;
                sm.plate.sizeBytes = plat->sizeBytes;
            }
            sm.installableHere = ce.compatible && plat != nullptr;
            sm.plate.fitted = e.catalogEntryInstalled(ce);
            if (const cascade::core::LoadedPlugin* lp = e.installedPluginRecord(ce)) {
                sm.plate.fileName = std::filesystem::path(lp->path).filename().string();
                sm.plate.loaded = lp->loaded;
                sm.plate.refusalReason = lp->error;
                sm.plate.haveCapabilities = lp->loaded;
                sm.plate.capabilities = lp->capabilities;
                const std::string key = cascade::core::pluginKey(*lp);
                sm.plate.running = lp->loaded && !e.pluginIsStopped(key);
                if (lp->hostClient != nullptr) {
                    sm.plate.haveTuneGrant = true;
                    sm.plate.tuneGranted = rawPluginUi(*a.pluginFeed_).tuneAllowed(cascade::core::PluginUi::tuneKey(*lp));
                }
            }
            const bool acked = (i == a.pluginStoreDeck_->selected) && a.pluginStoreDeck_->legalAck;
            sm.blockedReason = e.pluginInstallBlockedReason(i, acked);
            sm.blockedReasonIfAcknowledged = e.pluginInstallBlockedReason(i, true);
            for (const cascade::core::PluginUpdate& u : updates) {
                if (u.id != ce.id) { continue; }
                sm.updateToVersion = u.toVersion;
                sm.updateReason = u.reason;
                break;
            }
            model.modules.push_back(std::move(sm));
        }
        return model;
    }

    // "installing P of N: name" with P the module in flight (1-based), from the engine's own run: what the table
    // serves and the window letters. The OLD window lettered min(next + 1, total) here. (Both read the engine's run:
    // called on the pass, or with the pass parked at the publish being compared.)
    static std::string trueAddAllLine(AppWindow& a) {
        const auto& run = rawEngine(a.engine_).addAllRun_;
        if (!run.active) { return {}; }
        const std::size_t pos = std::clamp<std::size_t>(run.next, 1u, run.total);
        return storeAddAllLine(pos, run.total, run.currentName);
    }
    static std::string oldAddAllLine(AppWindow& a) {
        const auto& run = rawEngine(a.engine_).addAllRun_;
        if (!run.active) { return {}; }
        return cascade::core::formatText(cascade::i18n::tr("installing %zu of %zu: %s"), std::min(run.next + 1u, run.total),
                                         run.total, run.currentName.empty() ? cascade::i18n::tr("starting") : run.currentName.c_str());
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::PluginStoreModel;
using cascade::gui::StoreModule;
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

const std::string kStageId = "stage-one-fixture";
const std::string kNineId = "group-nine-fixture";
const std::string kHostPair = std::string(cc::PluginRepo::hostOs()) + "/" + cc::PluginRepo::hostArch();

// The scratch world: the plugin folder (three real modules and a file that is not one), the install records of two of
// them, and the index documents. Built BEFORE the window, whose engine reads the manifest when it first scans.
struct World {
    std::filesystem::path index, indexRun, indexB, indexC;
    std::string digest(const std::string& file) const {
        std::string hex, err;
        CHECK(cc::PluginRepo::sha256File((g_plugins / file).string(), hex, err));
        return hex;
    }
    World() {
        std::error_code ec;
        std::filesystem::remove_all(g_plugins, ec);
        std::filesystem::create_directories(g_plugins);
        for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP8_PLUGIN, FIXTURE_GROUP9_PLUGIN}) {
            const std::filesystem::path p(src);
            std::filesystem::copy_file(p, g_plugins / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        writeText(g_plugins / kRefused, "this is not a portable executable");
        // The install records: the stage-1 module at 1.0.0 and the group-9 one at 9.0.0, each with the digest of the
        // file that is really there (an honest record: no integrity finding to confuse the facts).
        for (int which = 0; which < 2; ++which) {
            cc::PluginCatalogEntry rec;
            rec.id = which == 0 ? kStageId : kNineId;
            rec.name = which == 0 ? "Stage One Fixture Plugin With A Deliberately Long Display Name So The Retirement Sentence Passes The Cut"
                                  : "Group Nine Fixture";
            rec.version = which == 0 ? "1.0.0" : "9.0.0";
            rec.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
            cc::PluginPlatform p;
            p.os = cc::PluginRepo::hostOs();
            p.arch = cc::PluginRepo::hostArch();
            p.file = which == 0 ? kStage1 : kGroup9;
            p.url = "https://example.invalid/x";
            p.sha256 = digest(p.file);
            rec.platforms.push_back(p);
            std::string err;
            CHECK(cc::PluginRepo::recordInstall(g_plugins.string(), rec, err));
        }
        std::vector<IdxEntry> v;
        {
            IdxEntry e;  // a plain entry: two builds, the one for this machine with a size
            e.id = "alpha-good";
            e.name = "Alpha Decoder";
            e.version = "1.0.0";
            e.author = "Alpha Labs Ltd";
            e.licence = "MIT";
            e.summary = "Decodes alpha.";
            e.description = std::string(400, 'd');
            e.homepage = "https://example.org/alpha";
            e.platforms = {hostBuild(moduleFile("alpha_good"), 'a', 12345), IdxPlatform{"plan9", "mips", "alpha_plan9.so", hex64('b'), 99}};
            v.push_back(e);
        }
        {
            IdxEntry e;  // a legal notice longer than a list item, a floor, a dotted id, no stated size, no author
            e.id = "beta.legal";
            e.name = "Beta Decoder";
            e.version = "0.9.1";
            e.licence = "GPL-3.0-or-later";
            e.legal = std::string(300, 'L');
            e.floor = "0.9.0";
            e.platforms = {hostBuild(moduleFile("beta_legal"), 'c', 0)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // announced before its first build: no platforms at all
            e.id = "gamma-nobuild";
            e.name = "Gamma Decoder";
            e.version = "0.1.0";
            e.licence = "MIT";
            v.push_back(e);
        }
        {
            IdxEntry e;  // another ABI
            e.id = "delta-abi";
            e.name = "Delta Decoder";
            e.version = "3.0.0";
            e.licence = "MIT";
            e.abi = CASCADE_PLUGIN_ABI_VERSION + 1u;
            e.platforms = {hostBuild(moduleFile("delta_abi"), 'd', 777)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // no licence declared
            e.id = "epsilon-nolicence";
            e.name = "Epsilon Decoder";
            e.version = "1.1.0";
            e.platforms = {hostBuild(moduleFile("epsilon"), 'e', 4096)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // installed at 1.0.0 below its own floor: RETIRED, and an update is planned
            e.id = kStageId;
            e.name = "Stage One Fixture Plugin With A Deliberately Long Display Name So The Retirement Sentence Passes The Cut";
            e.version = "2.0.0";
            e.author = "FoxSDR tests";
            e.licence = "MIT";
            e.floor = "2.0.0";
            e.platforms = {hostBuild(kStage1, 'f', 0)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // installed at 9.0.0, loaded: a host record to join, and an update is planned
            e.id = kNineId;
            e.name = "Group Nine Fixture";
            e.version = "9.2.0";
            e.author = "FoxSDR tests (group nine)";
            e.licence = "MIT";
            e.platforms = {hostBuild(kGroup9, '7', 2048)};
            v.push_back(e);
        }
        index = g_scratch / "store_index_group10.json";
        writeText(index, indexJson(v));
        // A DIFFERENT catalogue with the same shape (one entry reworded), for "the catalogue was replaced".
        v[1].legal = std::string(300, 'M');
        indexB = g_scratch / "store_index_group10_b.json";
        writeText(indexB, indexJson(v));
        // TWO RETIRED MODULES: the same shape with the group-9 module's floor above its installed 9.0.0 too.
        v[1].legal = std::string(300, 'L');
        v[6].floor = "9.2.0";
        indexC = g_scratch / "store_index_group10_c.json";
        writeText(indexC, indexJson(v));
        // The ADD ALL run: three entries whose file names the loader's rule refuses (a separator), so every install is
        // refused by install() before it reaches the network, in a handful of passes.
        std::vector<IdxEntry> run;
        for (const char* word : {"One", "Two", "Three"}) {
            IdxEntry e;
            e.id = std::string("bad-") + word;
            e.name = std::string("Bad ") + word;
            e.version = "1.0.0";
            e.licence = "MIT";
            e.platforms = {hostBuild(std::string("sub/") + moduleFile(word), '8', 100)};
            run.push_back(e);
        }
        indexRun = g_scratch / "store_index_group10_run.json";
        writeText(indexRun, indexJson(run));
    }
};

// A window with the world laid out and its state read.
struct Env {
    World w;
    AppWindow app;
    Env() {
        Access::look(app);
        Access::look(app);
    }
};

// --- frames --------------------------------------------------------------------------------------------------
void imguiFrame(AppWindow& a, bool store, bool rail) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    if (store) { Access::drawStore(a); }
    if (rail) {
        // The rail's own window: top left, narrow, as the rail is.
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(320.0f, 900.0f), ImGuiCond_Always);
        ImGui::Begin(kRailName, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
        Access::drawRail(a);
        ImGui::End();
    }
    ImGui::Render();
}
// The engine's pass only (the lists are published; the window reads nothing).
void pass(AppWindow& a) {
    Access::look(a);
    Access::after(a);
}
// A frame of the application as far as this window goes: the engine's part, the window drawn, the workers polled.
void frame(AppWindow& a, bool store = true, bool rail = false) {
    Access::look(a);
    imguiFrame(a, store, rail);
    Access::after(a);
}
// The same, but only the ImGui half (no engine pass between: a click's frames).
void drawOnly(AppWindow& a, bool store, bool rail) { imguiFrame(a, store, rail); }

template <class Pred>
bool passUntil(AppWindow& a, Pred pred, int max = 3000) {
    for (int i = 0; i < max; ++i) {
        if (pred()) { return true; }
        pass(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// Points the store at `path`, fetches, and passes until it has landed with `entries` entries.
bool fetchCatalogue(AppWindow& a, const std::filesystem::path& path, std::size_t entries) {
    Access::submit(a, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, path.string()));
    Access::submit(a, cmd::make(FOXAPI_OP_STORE_FETCH));
    const bool landed = passUntil(a, [&] { return Access::catalogSize(a) == entries && !Access::catalogPending(a); });
    pass(a);
    pass(a);
    return landed;
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

const StoreModule* find(const PluginStoreModel& m, const std::string& id) {
    for (const StoreModule& x : m.modules) {
        if (x.id == id) { return &x; }
    }
    return nullptr;
}
const StoreModule* mod(AppWindow& a, const std::string& id) { return find(Access::lists(a).model, id); }
int indexOf(AppWindow& a, const std::string& id) {
    const auto& ms = Access::lists(a).model.modules;
    for (std::size_t i = 0; i < ms.size(); ++i) {
        if (ms[i].id == id) { return static_cast<int>(i); }
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

// The same module, as the engine's own members give it and as the table gave it.
bool sameModule(const StoreModule& got, const StoreModule& want, const char* when) {
    bool ok = true;
    const auto check = [&](bool same, const char* field) {
        if (!same) {
            std::printf("      %s: %s of %s differs\n", when, field, want.id.c_str());
            ok = false;
        }
        CHECK(same);
    };
    const cascade::gui::ModulePlate& g = got.plate;
    const cascade::gui::ModulePlate& w = want.plate;
    check(got.id == want.id, "id");
    check(g.haveDescriptor == w.haveDescriptor, "haveDescriptor");
    // THE NAME IS CUT AT THE LIST ITEM'S 95 BYTES (API 0.3 8.9): the old window drew a display name whole. No entry of
    // the live catalogue comes near it (the longest, "406 MHz Distress Beacon Decoder (EPIRB / ELT / PLB)", is 51
    // characters); this world has one that does, on purpose, to hold the cut where it is.
    check(g.name == w.name.substr(0, 95), "name (cut at 95 bytes)");
    check(g.version == w.version, "version");
    check(g.maker == w.maker, "maker");
    check(g.licence == w.licence, "licence");
    check(g.blurb == w.blurb, "blurb");
    check(g.summary == w.summary, "summary");
    check(g.homepage == w.homepage, "homepage");
    check(g.legalNotice == w.legalNotice, "legalNotice");
    check(g.fileName == w.fileName, "fileName");
    check(g.fitted == w.fitted, "fitted");
    check(g.loaded == w.loaded, "loaded");
    check(g.running == w.running, "running");
    check(g.refusalReason == w.refusalReason, "refusalReason");
    check(g.haveCapabilities == w.haveCapabilities, "haveCapabilities");
    check(g.capabilities == w.capabilities, "capabilities");
    check(g.haveTuneGrant == w.haveTuneGrant, "haveTuneGrant");
    check(g.tuneGranted == w.tuneGranted, "tuneGranted");
    check(g.haveSizeBytes == w.haveSizeBytes, "haveSizeBytes");
    check(g.sizeBytes == w.sizeBytes, "sizeBytes");
    check(g.haveAbi == w.haveAbi, "haveAbi");
    check(g.abiVersion == w.abiVersion, "abiVersion");
    check(g.hostAbiVersion == w.hostAbiVersion, "hostAbiVersion");
    check(g.platforms == w.platforms, "platforms");
    check(g.retirementFloor == w.retirementFloor, "retirementFloor");
    check(got.installableHere == want.installableHere, "installableHere");
    check(got.blockedReason == want.blockedReason, "blockedReason");
    check(got.blockedReasonIfAcknowledged == want.blockedReasonIfAcknowledged, "blockedReasonIfAcknowledged");
    check(got.updateToVersion == want.updateToVersion, "updateToVersion");
    check(got.updateReason == want.updateReason, "updateReason");
    return ok;
}

// The whole model against the old window's, module by module and the fields beside the modules. Returns the modules
// compared. (The ADD ALL line is compared with the TRUE position: the one difference, recorded.)
int sameModel(AppWindow& a, const char* when) {
    Access::look(a);
    const PluginStoreModel old = Access::oldModel(a);
    const PluginStoreModel& got = Access::lists(a).model;
    CHECK_EQ(got.modules.size(), old.modules.size());
    const auto field = [&](bool same, const char* what) {
        if (!same) { std::printf("      %s: model.%s differs\n", when, what); }
        CHECK(same);
    };
    field(got.sourceUrl == old.sourceUrl, "sourceUrl");
    field(got.haveCatalogue == old.haveCatalogue, "haveCatalogue");
    field(got.sourceStatus == old.sourceStatus, "sourceStatus");
    field(got.sourceError == old.sourceError, "sourceError");
    field(got.busy == old.busy, "busy");
    // (the bar is drawn only while something is busy; the table reads the figure only then and 0 otherwise, where the
    // old window read the repo's last figure whatever it was doing)
    field(!old.busy || got.progress == old.progress, "progress");
    field(got.busyLabel == old.busyLabel, "busyLabel");
    field(got.resultReport == old.resultReport, "resultReport");
    field(got.resultError == old.resultError, "resultError");
    field(got.addAllRunning == old.addAllRunning, "addAllRunning");
    field(got.addAllProgress == old.addAllProgress, "addAllProgress");
    field(got.addAllSummary == old.addAllSummary, "addAllSummary");
    field(got.addAllFailed == old.addAllFailed, "addAllFailed");
    int compared = 0;
    for (std::size_t i = 0; i < got.modules.size() && i < old.modules.size(); ++i) {
        if (sameModule(got.modules[i], old.modules[i], when)) { ++compared; }
    }
    return compared;
}

// --- A -----------------------------------------------------------------------

void testModelEqualsTheOldWindow() {
    std::printf("  A: the model the window draws from the table, equal to the one it built from the engine's members\n");
    Env f;
    AppWindow& app = f.app;
    int compared = 0;
    int states = 0;
    const auto state = [&](const char* when) {
        compared += sameModel(app, when);
        ++states;
    };
    // NOTHING FETCHED: no rows, the default index, no lines.
    state("nothing fetched");
    CHECK(!Access::lists(app).model.haveCatalogue);
    CHECK(Access::lists(app).model.modules.empty());
    CHECK(Access::lists(app).model.sourceStatus.empty());

    CHECK(fetchCatalogue(app, f.w.index, 7));
    state("fetched, nothing selected");
    CHECK_EQ(Access::lists(app).model.modules.size(), 7u);
    CHECK_EQ(Access::lists(app).model.sourceUrl, f.w.index.string());
    CHECK_EQ(Access::lists(app).model.sourceStatus, std::string("7 plugins in the catalogue"));

    // The tick belongs to ONE row, the selected one: asked of every other row as unacknowledged.
    const int beta = indexOf(app, "beta.legal");
    const int alpha = indexOf(app, "alpha-good");
    CHECK(beta >= 0 && alpha >= 0);
    Access::deck(app).selected = beta;
    Access::deck(app).legalAck = false;
    state("the notice's row selected, not ticked");
    Access::deck(app).legalAck = true;
    state("the notice's row selected and ticked");
    CHECK(mod(app, "beta.legal")->blockedReason.empty());
    Access::deck(app).selected = alpha;
    state("another row selected with the tick still given (it is for the first row only)");
    CHECK_EQ(mod(app, "beta.legal")->blockedReason, std::string(cascade::gui::kLegalNoticeFirstReason));
    Access::deck(app).legalAck = false;
    Access::deck(app).selected = -1;

    // A module stopped, then a tune grant.
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_STOP, kGroup9));
    pass(app);
    pass(app);
    state("a fitted module stopped");
    CHECK(!mod(app, kNineId)->plate.running && mod(app, kNineId)->plate.loaded);
    Access::command(app, cmd::makeText(FOXAPI_OP_DECODER_START, kGroup9));
    Access::command(app, cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, kGroup9, 1, 1));
    pass(app);
    pass(app);
    state("running again, with the tune grant");
    CHECK(mod(app, kNineId)->plate.running && mod(app, kNineId)->plate.haveTuneGrant && mod(app, kNineId)->plate.tuneGranted);

    // A DOWNLOAD in flight (the worker's slot is a promise the test keeps), a quarter done, and what it leaves.
    Access::beginTransfer(app, "Alpha Decoder");
    Access::setProgress(app, 0.25f);
    pass(app);
    state("a download in flight");
    CHECK(Access::lists(app).model.busy);
    CHECK_EQ(Access::lists(app).model.busyLabel, std::string("downloading Alpha Decoder"));
    CHECK_EQ(Access::lists(app).model.progress, 0.25f);
    CHECK_EQ(mod(app, "alpha-good")->blockedReason, std::string("a transfer is already in progress"));
    Access::setProgress(app, 0.75f);
    state("the same download, three quarters");
    CHECK_EQ(Access::lists(app).model.progress, 0.75f);
    Access::endTransfer(false, "Alpha Decoder", "the sha256 did not match");
    pass(app);
    pass(app);
    state("it failed");
    CHECK_EQ(Access::lists(app).model.resultError, std::string("the sha256 did not match"));
    CHECK(!Access::lists(app).model.busy);

    // A refetch that fails: the catalogue is emptied, and the red line is the engine's.
    Access::submit(app, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, f.w.index.string() + ".missing"));
    Access::submit(app, cmd::make(FOXAPI_OP_STORE_FETCH));
    CHECK(passUntil(app, [&] { return !Access::catalogPending(app) && Access::catalogSize(app) == 0u && !Access::statusLines(app).catalogError.empty(); }));
    pass(app);
    state("a refetch that failed");
    CHECK(!Access::lists(app).model.haveCatalogue && Access::lists(app).model.modules.empty());
    CHECK(!Access::lists(app).model.sourceError.empty());
    CHECK(Access::lists(app).model.sourceStatus.empty());
    std::printf("      %d modules held equal to the old window's, field for field, in %d states\n", compared, states);
    CHECK(compared >= 7 * 9);
}

// --- B -----------------------------------------------------------------------

void testWhatTheWindowIsTold() {
    std::printf("  B: the entries from the index - ABI, size, builds, the gate, the plan, the host's record, the retired row\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    pass(app);
    const PluginStoreModel& model = Access::lists(app).model;
    const unsigned abi = CASCADE_PLUGIN_ABI_VERSION;
    CHECK_EQ(model.modules.size(), 7u);

    const StoreModule* alpha = mod(app, "alpha-good");
    const StoreModule* beta = mod(app, "beta.legal");
    const StoreModule* gamma = mod(app, "gamma-nobuild");
    const StoreModule* delta = mod(app, "delta-abi");
    const StoreModule* eps = mod(app, "epsilon-nolicence");
    const StoreModule* stage = mod(app, kStageId);
    const StoreModule* nine = mod(app, kNineId);
    CHECK(alpha && beta && gamma && delta && eps && stage && nine);
    if (!(alpha && beta && gamma && delta && eps && stage && nine)) { return; }

    // The plain entry: every text whole, the size of THIS machine's build, both builds in the index's order.
    CHECK_EQ(alpha->plate.name, std::string("Alpha Decoder"));
    CHECK_EQ(alpha->plate.maker, std::string("Alpha Labs Ltd"));
    CHECK_EQ(alpha->plate.licence, std::string("MIT"));
    CHECK_EQ(alpha->plate.blurb, std::string(400, 'd'));  // the description, whole (not cut at a list item's 159)
    CHECK_EQ(alpha->plate.summary, std::string("Decodes alpha."));
    CHECK_EQ(alpha->plate.homepage, std::string("https://example.org/alpha"));
    CHECK(alpha->plate.haveAbi && alpha->plate.abiVersion == abi && alpha->plate.hostAbiVersion == abi);
    CHECK_EQ(alpha->plate.platforms, kHostPair + ", plan9/mips");
    CHECK(alpha->plate.haveSizeBytes && alpha->plate.sizeBytes == 12345u);
    CHECK(alpha->installableHere && !alpha->plate.fitted);
    CHECK(alpha->blockedReason.empty() && alpha->blockedReasonIfAcknowledged.empty());
    CHECK(alpha->updateToVersion.empty() && alpha->updateReason.empty());
    CHECK(!alpha->plate.loaded && alpha->plate.fileName.empty() && !alpha->plate.haveCapabilities);

    // The notice: its text whole, a floor, no maker, no size told - and the gate with and without the tick.
    CHECK_EQ(beta->plate.legalNotice, std::string(300, 'L'));
    CHECK_EQ(beta->plate.retirementFloor, std::string("0.9.0"));
    CHECK(beta->plate.maker.empty());
    CHECK(!beta->plate.haveSizeBytes && beta->plate.sizeBytes == 0u);  // "not told", never an empty file
    CHECK(beta->installableHere);
    CHECK_EQ(beta->blockedReason, std::string(cascade::gui::kLegalNoticeFirstReason));  // not ticked: the engine's own sentence
    CHECK(beta->blockedReasonIfAcknowledged.empty());
    CHECK_EQ(Access::blockedReason(app, indexOf(app, "beta.legal"), false), beta->blockedReason);  // ...which IS the engine's
    Access::deck(app).selected = indexOf(app, "beta.legal");
    Access::deck(app).legalAck = true;
    CHECK(mod(app, "beta.legal")->blockedReason.empty());
    Access::deck(app).legalAck = false;
    Access::deck(app).selected = -1;

    // No build at all: no platforms, not installable, and the gate says so.
    CHECK(gamma->plate.platforms.empty() && !gamma->installableHere && !gamma->plate.haveSizeBytes);
    CHECK_EQ(gamma->blockedReason, cascade::gui::pluginNoBuildReason(kHostPair));
    // Another ABI: both numbers are stated, not installable here though a build is, and the reason carries both.
    CHECK(delta->plate.haveAbi && delta->plate.abiVersion == abi + 1u && delta->plate.hostAbiVersion == abi);
    CHECK(!delta->installableHere && delta->plate.haveSizeBytes && delta->plate.sizeBytes == 777u);
    CHECK_EQ(delta->blockedReason, cascade::gui::pluginAbiMismatchReason(abi + 1u, abi));
    // No licence: installable by the columns, and the gate refuses it.
    CHECK(eps->installableHere && eps->plate.licence.empty());
    CHECK_EQ(eps->blockedReason, std::string("the catalogue entry declares no licence"));

    // The retired module: INSTALLED (by the manifest) and the host holds no record; its update is planned.
    CHECK(stage->plate.fitted && !stage->plate.loaded && stage->plate.fileName.empty() && !stage->plate.haveCapabilities);
    CHECK(!stage->plate.running && !stage->plate.haveTuneGrant);
    CHECK_EQ(stage->updateToVersion, std::string("2.0.0"));
    CHECK_EQ(stage->updateReason, std::string("version 2.0.0 replaces the installed 1.0.0"));
    CHECK_EQ(stage->plate.retirementFloor, std::string("2.0.0"));
    CHECK_EQ(stage->blockedReason, std::string("already installed"));
    // The fitted one: the host's record, joined on the file, with its caps, its run state and a tune key.
    CHECK(nine->plate.fitted && nine->plate.loaded && nine->plate.running);
    CHECK_EQ(nine->plate.fileName, kGroup9);
    CHECK(nine->plate.haveCapabilities && (nine->plate.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u);
    CHECK(nine->plate.haveTuneGrant && !nine->plate.tuneGranted);
    CHECK(nine->plate.refusalReason.empty());
    CHECK_EQ(nine->updateToVersion, std::string("9.2.0"));
    CHECK_EQ(nine->updateReason, std::string("version 9.2.0 replaces the installed 9.0.0"));

    // The rail tier: counts, the retired row with its whole sentence, id and plan.
    const cascade::gui::StoreListCache& rail = Access::lists(app, false);
    CHECK(rail.haveCatalogue && rail.updates == 2u && !rail.transfer && !rail.addAll);
    CHECK_EQ(rail.refused, 1u);  // the file the host did not load
    CHECK_EQ(rail.blocked.size(), 1u);
    if (rail.blocked.size() == 1u) {
        const auto& b = rail.blocked[0];
        CHECK_EQ(b.file, kStage1);
        CHECK_EQ(b.id, kStageId);
        CHECK(b.hasPlan);
        // The sentence the old window composed at draw, now the engine's fact: the same function on the same record
        // in the language in force, and more than a list item holds.
        const auto& eb = Access::engineBlocked(app);
        CHECK_EQ(eb.size(), 1u);
        if (eb.size() == 1u) { CHECK_EQ(b.message, cc::PluginRepo::pluginBlockMessage(eb[0].installed, eb[0].policy)); }
        CHECK(b.message.size() > 159u);
        CHECK(b.message.find("out of date and has been disabled") != std::string::npos);
    }
    std::printf("      seven entries from the index, the retired row (%zu bytes of sentence) and the rail's counts, as the index and the engine say\n",
                rail.blocked.empty() ? 0u : rail.blocked[0].message.size());
}

// --- C -----------------------------------------------------------------------

void testCacheFollowsListSeq() {
    std::printf("  C: one rebuild when listSeq moves, the full tier only while drawn, the live progress, a settled frame allocates nothing\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    pass(app);
    Access::setOpen(app, true);
    frame(app);
    frame(app);
    auto& c = Access::cache(app);
    const std::uint32_t full0 = c.refreshes;
    const std::uint32_t rail0 = c.railRefreshes;
    CHECK(full0 >= 1u && rail0 >= 1u);

    // Nothing moved: no rebuild, however many frames - a decoder's line moves nothing.
    const std::uint64_t seq0 = Access::listSeq(app);
    for (int i = 0; i < 200; ++i) { frame(app, true, true); }
    CHECK_EQ(Access::listSeq(app), seq0);
    CHECK_EQ(c.refreshes, full0);
    CHECK_EQ(c.railRefreshes, rail0);

    // A settled frame: 1000 reads of an unchanged state allocate nothing - both tiers, the status lines, the rows'
    // reasons (the model is set on every call; a string given what its room holds allocates nothing).
    long before = t_allocs;
    std::size_t sink = 0;
    for (int i = 0; i < 1000; ++i) {
        const cascade::gui::StoreListCache& l = Access::lists(app, true);
        sink += l.model.modules.size() + l.model.sourceUrl.size() + l.model.sourceStatus.size() + l.blocked.size();
        for (const StoreModule& x : l.model.modules) { sink += x.blockedReason.size(); }
    }
    long spent = t_allocs - before;
    std::printf("      1000 full reads of an unchanged state: %ld allocation(s) (%zu)\n", spent, sink);
    CHECK_EQ(spent, 0L);
    before = t_allocs;
    for (int i = 0; i < 1000; ++i) { sink += Access::lists(app, false).blocked.size(); }
    spent = t_allocs - before;
    std::printf("      1000 rail-tier reads (the window shut): %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);
    // ...with the notice's row selected and a deck that moves (the reason it depends on is set every call).
    Access::deck(app).selected = indexOf(app, "beta.legal");
    (void)Access::lists(app);
    before = t_allocs;
    for (int i = 0; i < 1000; ++i) {
        Access::deck(app).legalAck = (i % 2) == 0;
        sink += Access::lists(app).model.modules[static_cast<std::size_t>(Access::deck(app).selected)].blockedReason.size();
    }
    spent = t_allocs - before;
    std::printf("      1000 reads with the tick flipping on the selected row: %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);
    Access::deck(app).legalAck = false;
    Access::deck(app).selected = -1;

    // What the window cost before the port, every frame it was open, for the record: the same seven entries.
    long oldAllocs = 0;
    const int frames = 100;
    for (int i = 0; i < frames; ++i) { sink += Access::oldModel(app, &oldAllocs).modules.size(); }
    const long oldPerFrame = oldAllocs / frames;
    std::printf("      the old window's model, built every frame from the engine's members: %ld allocations a frame\n", oldPerFrame);
    CHECK(oldPerFrame > 0);

    // THE LIVE PROGRESS: a transfer in flight is read once a frame, moves no list and allocates nothing.
    Access::beginTransfer(app, "Alpha Decoder");
    Access::setProgress(app, 0.1f);
    pass(app);
    (void)Access::lists(app);
    const std::uint64_t seqT = Access::listSeq(app);
    const std::uint32_t fullT = c.refreshes;
    before = t_allocs;
    float last = -1.0f;
    int moves = 0;
    for (int i = 0; i < 1000; ++i) {
        Access::setProgress(app, static_cast<float>(i) / 1000.0f);
        const float p = Access::lists(app).model.progress;
        if (p != last) { ++moves; }
        last = p;
        CHECK_EQ(p, static_cast<float>(i) / 1000.0f);  // the figure the worker stored, read the same frame
    }
    spent = t_allocs - before;
    std::printf("      the progress moved 1000 times: %d reads saw a new figure, %ld allocation(s), listSeq and the cache unmoved\n", moves, spent);
    CHECK_EQ(spent, 0L);
    CHECK_EQ(moves, 1000);
    CHECK_EQ(Access::listSeq(app), seqT);
    CHECK_EQ(c.refreshes, fullT);
    // ...and through passes of the engine: the list does not move while only the figure does.
    for (int i = 0; i < 20; ++i) {
        Access::setProgress(app, 0.5f + 0.01f * static_cast<float>(i));
        frame(app);
        CHECK_EQ(Access::listSeq(app), seqT);
    }
    Access::endTransfer(true, "Alpha Decoder", "");
    pass(app);
    pass(app);
    CHECK(!Access::lists(app).transfer);
    CHECK_EQ(Access::lists(app).model.progress, 0.0f);

    // A change of the lists: one rebuild of each tier, bounded.
    const auto aChange = [&](int n) {
        // Any list-moving command: the stopped set.
        Access::command(app, cmd::makeText(n % 2 == 0 ? FOXAPI_OP_DECODER_STOP : FOXAPI_OP_DECODER_START, kGroup9));
    };
    (void)Access::lists(app);
    aChange(1);
    Access::look(app);
    const std::uint32_t r1 = c.refreshes;
    const std::uint32_t q1 = c.railRefreshes;
    before = t_allocs;
    (void)Access::lists(app);
    spent = t_allocs - before;
    CHECK_EQ(c.refreshes, r1 + 1u);
    CHECK_EQ(c.railRefreshes, q1 + 1u);
    (void)Access::lists(app);
    CHECK_EQ(c.refreshes, r1 + 1u);  // read once however often it is asked
    long inRefresh = 0;
    for (int i = 0; i < 100; ++i) {
        aChange(2 + i);
        Access::look(app);
        const long mark = t_allocs;
        (void)Access::lists(app);
        inRefresh += t_allocs - mark;
    }
    const std::size_t rows = Access::lists(app).model.modules.size();
    std::printf("      a rebuild (%zu entries, 12 facts each, the host's records, the retired row): %ld allocation(s) the first time, %ld on average over 100 (all but a few inside the table's get_setting)\n",
                rows, spent, inRefresh / 100);
    CHECK(spent <= 400L);
    CHECK(inRefresh / 100 <= 400L);

    // The window SHUT builds no model: the rail tier is read, the full tier is not, however the lists move.
    Access::setOpen(app, false);
    const std::uint32_t rShut = c.refreshes;
    const std::uint32_t qShut = c.railRefreshes;
    for (int i = 0; i < 50; ++i) {
        aChange(5000 + i);
        frame(app, true, true);
    }
    CHECK_EQ(c.refreshes, rShut);
    CHECK(c.railRefreshes >= qShut + 50u);  // (the rail draws every frame; each move is read once)
    Access::setOpen(app, true);
    frame(app);
    CHECK_EQ(c.refreshes, rShut + 1u);  // ...and one read when it opens, however much moved
    (void)sink;
}

// --- D -----------------------------------------------------------------------

struct Hit {
    ImGuiID id;
    float x, y;
};

// The distinct widgets of a window, each at the first point of a grid the pointer finds it at (the store's title bar
// is skipped: its collapse arrow swallows a click and folds the window).
std::vector<Hit> sweep(AppWindow& app, bool store, bool rail, float stepX, float stepY) {
    std::vector<Hit> out;
    ImGuiIO& io = ImGui::GetIO();
    drawOnly(app, store, rail);
    ImGuiWindow* w = ImGui::FindWindowByName(store ? kWindowName : kRailName);
    CHECK(w != nullptr);
    if (w == nullptr) { return out; }
    const ImVec2 p = w->Pos;
    const ImVec2 sz = w->Size;
    const float top = store ? 30.0f : 4.0f;
    for (float y = p.y + top; y < p.y + sz.y - 8.0f; y += stepY) {
        for (float x = p.x + 8.0f; x < p.x + sz.x - 8.0f; x += stepX) {
            io.AddMousePosEvent(x, y);
            drawOnly(app, store, rail);
            drawOnly(app, store, rail);  // (ImGui finds what is under the pointer a frame after it moves)
            const ImGuiID hov = GImGui->HoveredId;
            if (hov == 0) { continue; }
            bool seen = false;
            for (const Hit& s : out) { seen = seen || s.id == hov; }
            if (!seen) { out.push_back({hov, x, y}); }
        }
    }
    io.AddMousePosEvent(2000.0f, 2000.0f);
    drawOnly(app, store, rail);
    return out;
}

// A click in frames of the window with no engine pass between them: the press, the release (which is where a key
// fires and sends its command), and the pointer put away.
void clickNoPass(AppWindow& app, bool store, bool rail, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    drawOnly(app, store, rail);
    drawOnly(app, store, rail);
    io.AddMouseButtonEvent(0, true);
    drawOnly(app, store, rail);
    io.AddMouseButtonEvent(0, false);
    drawOnly(app, store, rail);
    io.AddMousePosEvent(2000.0f, 2000.0f);
    drawOnly(app, store, rail);
}

struct Found {
    std::string command;  // what the click sent ("" nothing)
    float x, y;
};

// Every widget of a window in the state `prepare` sets, found and clicked once, and the command each click sent
// (nothing is applied: no pass runs). The caller's tap is put back by the caller; `prepare` runs before each click.
template <class Prepare>
std::vector<Found> survey(AppWindow& app, bool store, bool rail, float stepX, float stepY, Prepare prepare) {
    Access::Sent sent;
    Access::tap(app, &sent);
    prepare();
    drawOnly(app, store, rail);
    drawOnly(app, store, rail);
    ImGuiWindow* w0 = ImGui::FindWindowByName(store ? kWindowName : kRailName);
    CHECK(w0 != nullptr);
    if (w0 == nullptr) { return {}; }
    const ImVec2 winPos = w0->Pos;
    const ImVec2 winSize = w0->Size;
    const auto putBack = [&] {
        ImGui::SetWindowPos(store ? kWindowName : kRailName, winPos, ImGuiCond_Always);
        ImGui::SetWindowSize(store ? kWindowName : kRailName, winSize, ImGuiCond_Always);
    };
    const std::vector<Hit> spots = sweep(app, store, rail, stepX, stepY);
    std::vector<Found> found;
    for (const Hit& s : spots) {
        putBack();
        prepare();
        drawOnly(app, store, rail);
        Access::park(app);  // (free-running: no pass until the click's queue is dropped)
        sent.clear();
        clickNoPass(app, store, rail, s.x, s.y);
        Found fd{"", s.x, s.y};
        for (const auto& q : sent) {
            if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
            fd.command = describe(q);
        }
        found.push_back(fd);
        Access::dropQueued(app);
    }
    putBack();
    prepare();
    drawOnly(app, store, rail);
    Access::tap(app, nullptr);  // (the survey's own log goes out of scope: the caller puts its own back)
    return found;
}

// A window tall enough for the list AND the plate beside it, whose keys sit below the plate's text (the display is
// made to fit it).
void bigStoreWindow() {
    ImGui::SetWindowPos(kWindowName, ImVec2(10.0f, 10.0f), ImGuiCond_Always);
    ImGui::SetWindowSize(kWindowName, ImVec2(1580.0f, 1980.0f), ImGuiCond_Always);
}

std::vector<std::string> commandsOf(const std::vector<Found>& v) {
    std::vector<std::string> out;
    for (const Found& f : v) {
        if (!f.command.empty()) { out.push_back(f.command); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Exactly these, in any order; a difference is printed.
bool sameCommands(const std::vector<std::string>& got, std::vector<std::string> want, const char* when) {
    std::sort(want.begin(), want.end());
    const bool same = got == want;
    if (!same) {
        std::printf("      %s: the clicks sent something else\n", when);
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : want) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(same);
    return same;
}

const Found* spotFor(const std::vector<Found>& spots, const std::string& command) {
    for (const Found& s : spots) {
        if (s.command == command) { return &s; }
    }
    return nullptr;
}

// A click that lands: the press with the engine's pass before it, and the release - where a key fires - with NO pass
// after it, so the caller can show nothing has happened yet. The next frame is the engine's next pass. (Free-running,
// the pass is parked at a publish from before the release to that frame.)
void clickThenHold(AppWindow& app, bool store, bool rail, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame(app, store, rail);
    frame(app, store, rail);
    io.AddMouseButtonEvent(0, true);
    frame(app, store, rail);
    Access::park(app);
    io.AddMouseButtonEvent(0, false);
    drawOnly(app, store, rail);
    io.AddMousePosEvent(2000.0f, 2000.0f);
}

std::size_t countOf(const Access::Sent& sent, const char* op) {
    std::size_t n = 0;
    for (const auto& q : sent) { n += std::string(cmd::opName(q.c.op)) == op ? 1u : 0u; }
    return n;
}

// The window opened, drawn tall enough for its plate's keys, the pointer away.
void openBig(AppWindow& app) {
    Access::setOpen(app, true);
    frame(app);
    frame(app);
    bigStoreWindow();
    frame(app);
    ImGui::GetIO().AddMousePosEvent(2000.0f, 2000.0f);
    frame(app);
}

// --- D -----------------------------------------------------------------------

void testFirstOpen() {
    std::printf("  D1: the first open reads the catalogue once - and never over one that is already loaded\n");
    // A catalogue another path loaded (the web remote's, a bounded run's hook: here the engine's own fetch) before the
    // window was ever opened: opening it asks the table before it asks whether to fetch, so it sends nothing - a FETCH
    // would CLEAR the catalogue it holds.
    {
        Env f;
        AppWindow& app = f.app;
        CHECK(fetchCatalogue(app, f.w.index, 7));
        Access::Sent sent;
        Access::tap(app, &sent);
        Access::setOpen(app, true);
        for (int i = 0; i < 6; ++i) { frame(app); }
        CHECK_EQ(countOf(sent, "STORE_FETCH"), 0u);
        CHECK_EQ(Access::catalogSize(app), 7u);
        CHECK(Access::cache(app).model.haveCatalogue && Access::cache(app).model.modules.size() == 7u);
        Access::tap(app, nullptr);
    }
    // Nothing asked yet (the index is a local file: no network): the first frame it is open sends one FETCH, and the
    // table then shows it in flight, so no more follow; it lands and the rows are there.
    {
        Env f;
        AppWindow& app = f.app;
        Access::submit(app, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, f.w.index.string()));
        pass(app);
        pass(app);
        CHECK_EQ(Access::engineUrl(app), f.w.index.string());
        Access::Sent sent;
        Access::tap(app, &sent);
        Access::setOpen(app, true);
        for (int i = 0; i < 4000 && Access::catalogSize(app) != 7u; ++i) {
            frame(app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int i = 0; i < 6; ++i) { frame(app); }
        CHECK_EQ(countOf(sent, "STORE_FETCH"), 1u);
        CHECK_EQ(Access::catalogSize(app), 7u);
        CHECK_EQ(Access::cache(app).model.modules.size(), 7u);
        CHECK_EQ(Access::cache(app).model.sourceStatus, std::string("7 plugins in the catalogue"));
        Access::tap(app, nullptr);
        std::printf("      already loaded: no FETCH on opening; asked for the first time: one FETCH, then the 7 rows\n");
    }
}

void testDrawnAndClicked() {
    std::printf("  D2: the real window drawn; every widget found and clicked sends exactly the commands it should\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    openBig(app);
    CHECK(Access::storeDrew(app));
    // Idle: a window drawn for ten frames sends nothing (the usage report's page note excepted, once).
    Access::Sent sent;
    Access::tap(app, &sent);
    for (int i = 0; i < 10; ++i) { frame(app); }
    std::set<std::string> names;
    for (const auto& q : sent) {
        CHECK_EQ(q.c.op, static_cast<std::uint32_t>(FOXAPP_OP_NOTE_PANEL));
        names.insert(q.c.text);
    }
    CHECK(names.size() <= 1u);
    sent.clear();
    for (int i = 0; i < 5; ++i) { frame(app); }
    CHECK(sent.empty());
    Access::tap(app, nullptr);

    // The drawing follows the state, and only the state: the same frame twice is the same picture, a selection moves it.
    frame(app);
    frame(app);
    const std::uint64_t h0 = drawHash();
    frame(app);
    const std::uint64_t h1 = drawHash();
    CHECK(h0 != 0u);
    CHECK_EQ(h0, h1);
    Access::deck(app).selected = indexOf(app, "beta.legal");
    frame(app);
    frame(app);
    CHECK(drawHash() != h0);
    Access::deck(app) = cascade::gui::PluginStoreDeck{};
    frame(app);
    frame(app);
    CHECK_EQ(drawHash(), h0);

    const int alpha = indexOf(app, "alpha-good");
    const int beta = indexOf(app, "beta.legal");
    const int nine = indexOf(app, kNineId);
    const auto inState = [&](int sel, bool legal, bool all) {
        return [&app, sel, legal, all] {
            Access::deck(app) = cascade::gui::PluginStoreDeck{};
            Access::deck(app).selected = sel;
            Access::deck(app).legalAck = legal;
            Access::deck(app).addAllAck = all;
            Access::look(app);
        };
    };
    const std::string upStage = expect("APP_STORE_UPDATE", 0, 0, kStageId);
    const std::string upNine = expect("APP_STORE_UPDATE", 0, 0, kNineId);
    const std::string fetch = expect("STORE_FETCH", 0, 0, "");
    const std::string fitAlpha = expect("STORE_INSTALL", 0, 0, "alpha-good");

    // THE ALPHA ROW SELECTED, nothing ticked: ADD ALL (no notice covered), the two updates in the banner and on their
    // rows, CHECK NOW, and FIT on alpha's row and on the plate.
    {
        const auto found = survey(app, true, false, 36.0f, 14.0f, inState(alpha, false, false));
        CHECK(found.size() >= 20u);
        sameCommands(commandsOf(found),
                     {expect("APP_STORE_ADD_ALL", 0, 0, ""), upStage, upStage, upNine, upNine, fetch, fitAlpha, fitAlpha},
                     "alpha selected");
        std::printf("      alpha selected: %zu widgets found, 8 commands sent\n", found.size());
    }
    // THE NOTICE'S ROW SELECTED AND TICKED, and ADD ALL's own tick: ADD ALL carries the row's id and its tick; FIT on
    // that row and on the plate carries the acknowledgement (ival[0] 1); FIT on another row does not.
    {
        const auto found = survey(app, true, false, 36.0f, 14.0f, inState(beta, true, true));
        sameCommands(commandsOf(found),
                     {expect("APP_STORE_ADD_ALL", 1, 0, "beta.legal"), upStage, upStage, upNine, upNine, fetch, fitAlpha,
                      expect("STORE_INSTALL", 1, 0, "beta.legal"), expect("STORE_INSTALL", 1, 0, "beta.legal")},
                     "the notice's row selected and ticked");
        std::printf("      the notice's row selected and ticked: ADD ALL names it, FIT on it carries the tick\n");
    }
    // THE NOTICE'S ROW SELECTED, NOT TICKED: FIT on it is dead (the table's reason says why) - nothing is sent for it.
    {
        const auto found = survey(app, true, false, 36.0f, 14.0f, inState(beta, false, false));
        sameCommands(commandsOf(found), {expect("APP_STORE_ADD_ALL", 0, 0, ""), upStage, upStage, upNine, upNine, fetch, fitAlpha},
                     "the notice's row selected, not ticked");
    }
    // THE FITTED, UPDATABLE ROW SELECTED: its plate offers UPDATE, not FIT.
    {
        const auto found = survey(app, true, false, 36.0f, 14.0f, inState(nine, false, false));
        sameCommands(commandsOf(found),
                     {expect("APP_STORE_ADD_ALL", 0, 0, ""), upStage, upStage, upNine, upNine, upNine, fetch, fitAlpha},
                     "the fitted row selected");
    }
    // A TRANSFER IN FLIGHT: everything that would start another is dead; the one key left is CANCEL.
    {
        Access::beginTransfer(app, "Alpha Decoder");
        Access::setProgress(app, 0.3f);
        const auto found = survey(app, true, false, 36.0f, 14.0f, inState(alpha, false, false));
        sameCommands(commandsOf(found), {expect("STORE_CANCEL", 0, 0, "")}, "a transfer in flight");
        Access::endTransfer(false, "Alpha Decoder", "x");
        pass(app);
        pass(app);
        CHECK(!Access::installPending(app));
    }
    std::printf("      the three kinds of row, the notice with and without its tick, and a transfer in flight: the commands are the right ones\n");
}

void testNothingFetched() {
    std::printf("  D3: no catalogue: CHECK NOW is the one key, and it is dead without an index to read\n");
    Env f;
    AppWindow& app = f.app;
    Access::submit(app, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, f.w.index.string() + ".missing"));
    Access::submit(app, cmd::make(FOXAPI_OP_STORE_FETCH));
    CHECK(passUntil(app, [&] { return !Access::catalogPending(app) && !Access::statusLines(app).catalogError.empty(); }));
    pass(app);
    openBig(app);
    // (opening it read the index once more - the first open of a session with nothing loaded - and that fetch is over
    // before the keys are looked at: while it is in flight CANCEL is the one live key)
    for (int i = 0; i < 4000 && (Access::catalogPending(app) || Access::cache(app).model.busy || Access::cache(app).model.sourceError.empty()); ++i) {
        frame(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    frame(app);
    const auto found = survey(app, true, false, 36.0f, 14.0f, [&] {
        Access::deck(app) = cascade::gui::PluginStoreDeck{};
        Access::look(app);
    });
    sameCommands(commandsOf(found), {expect("STORE_FETCH", 0, 0, "")}, "no catalogue");
    CHECK(!Access::cache(app).model.haveCatalogue);
    CHECK(!Access::cache(app).model.sourceError.empty());
}

void testRailRows() {
    std::printf("  D4: the rail: the store key, the URL field, the retired row's Update, Remove and Confirm - and the plugins key\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    frame(app, false, true);
    frame(app, false, true);
    ImGui::GetIO().AddMousePosEvent(2000.0f, 2000.0f);
    const std::string update = expect("APP_STORE_UPDATE", 0, 0, kStageId);
    const std::string remove = expect("APP_STORE_REMOVE_BLOCKED", 0, 0, kStage1);
    Access::setBlockedConfirm(app, "");
    // Not armed: the Update key sends its command, Remove only arms, the keys open windows and send nothing.
    {
        const auto found = survey(app, false, true, 20.0f, 8.0f, [&] {
            Access::setBlockedConfirm(app, "");
            Access::look(app);
        });
        sameCommands(commandsOf(found), {update}, "the rail, nothing armed");
        std::printf("      the rail: %zu widgets; the retired row's Update sends APP_STORE_UPDATE; Remove arms and sends nothing\n", found.size());
        CHECK_EQ(Access::blockedConfirm(app), std::string());
    }
    // Armed: Confirm sends the removal of THAT file, Cancel disarms.
    {
        const auto found = survey(app, false, true, 20.0f, 8.0f, [&] {
            Access::setBlockedConfirm(app, kStage1);
            Access::look(app);
        });
        sameCommands(commandsOf(found), {update, remove}, "the rail, the retired row armed");
    }
    Access::setBlockedConfirm(app, "");
    // The two keys' own effect, as a click makes it: Remove arms the row for its FILE.
    {
        const auto found = survey(app, false, true, 20.0f, 8.0f, [&] {
            Access::setBlockedConfirm(app, "");
            Access::look(app);
        });
        int arms = 0;
        for (const Found& fd : found) {
            if (!fd.command.empty()) { continue; }
            Access::setBlockedConfirm(app, "");
            Access::look(app);
            clickNoPass(app, false, true, fd.x, fd.y);
            if (Access::blockedConfirm(app) == kStage1) { ++arms; }
        }
        CHECK_EQ(arms, 1);  // exactly one key arms the row (the store key and the plugins key open windows)
        Access::setBlockedConfirm(app, "");
    }
}

// --- E -----------------------------------------------------------------------

void testControlsLandAtTheNextPass() {
    std::printf("  E: a click lands at the engine's next pass and the window reads the engine's value the frame after\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    openBig(app);
    const int beta = indexOf(app, "beta.legal");
    const int alpha = indexOf(app, "alpha-good");
    const auto prepare = [&](int sel, bool legal, bool all) {
        Access::deck(app) = cascade::gui::PluginStoreDeck{};
        Access::deck(app).selected = sel;
        Access::deck(app).legalAck = legal;
        Access::deck(app).addAllAck = all;
        Access::look(app);
    };
    // The spots, found once in this state.
    const std::vector<Found> spots = survey(app, true, false, 36.0f, 14.0f, [&] { prepare(alpha, false, false); });
    Access::Sent sent;
    Access::tap(app, &sent);

    // CHECK NOW, with a selection and both ticks given: nothing in the click's frame; at the next pass the fetch is in
    // flight (the table says so, the catalogue is empty, the lamp and its label are up) and the consents are gone;
    // when it lands the rows are back.
    {
        const Found* now = spotFor(spots, expect("STORE_FETCH", 0, 0, ""));
        CHECK(now != nullptr);
        if (now == nullptr) { return; }
        prepare(beta, true, true);
        frame(app);
        sent.clear();
        clickThenHold(app, true, false, now->x, now->y);
        CHECK_EQ(sent.size(), 1u);
        CHECK(!Access::catalogPending(app));              // the click only queued it
        CHECK(!Access::cache(app).model.busy);
        CHECK(Access::deck(app).legalAck && Access::deck(app).addAllAck);  // the ticks are still given
        frame(app);                                      // the engine's next pass...
        CHECK(Access::cache(app).model.busy);            // ...and the window reads it that same frame
        CHECK_EQ(Access::cache(app).model.busyLabel, std::string("fetching the catalogue"));
        CHECK(!Access::cache(app).model.haveCatalogue && Access::cache(app).model.modules.empty());
        CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck);  // consent for a catalogue that has gone
        CHECK_EQ(Access::deck(app).selected, -1);
        CHECK(passUntil(app, [&] { return !Access::catalogPending(app) && Access::catalogSize(app) == 7u; }));
        frame(app);
        frame(app);
        CHECK(Access::cache(app).model.haveCatalogue && !Access::cache(app).model.busy);
        CHECK_EQ(Access::cache(app).model.modules.size(), 7u);
        CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck);
        std::printf("      CHECK NOW: queued by its click, in flight at the next pass (\"%s\"), the ticks and the selection gone, the rows back\n",
                    "fetching the catalogue");
    }

    // FIT on alpha: the key was alive when pressed; the engine re-tests the gate when the command applies (here a
    // transfer began in between) and answers DENIED - the reason is the red line, the frame after the pass.
    {
        const Found* fit = spotFor(spots, expect("STORE_INSTALL", 0, 0, "alpha-good"));
        CHECK(fit != nullptr);
        if (fit == nullptr) { return; }
        prepare(alpha, false, false);
        frame(app);
        (void)Access::answers(app);
        sent.clear();
        clickThenHold(app, true, false, fit->x, fit->y);
        CHECK_EQ(sent.size(), 1u);
        if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("STORE_INSTALL", 0, 0, "alpha-good")); }
        Access::beginTransfer(app, "Alpha Decoder");  // a transfer begins before the engine's pass
        frame(app);
        const auto answers = Access::answers(app);
        CHECK_EQ(answers.size(), 1u);
        if (answers.size() == 1u) {
            CHECK_EQ(answers[0].first, static_cast<std::uint32_t>(FOXAPI_OP_STORE_INSTALL));
            CHECK_EQ(answers[0].second, FOXAPI_DENIED);
        }
        frame(app);
        CHECK_EQ(Access::cache(app).model.resultError, std::string("a transfer is already in progress"));
        CHECK(Access::cache(app).model.busy);
        CHECK_EQ(Access::cache(app).model.busyLabel, std::string("downloading Alpha Decoder"));
        std::printf("      FIT: queued, applied at the next pass against the gate as it then was (DENIED), the reason on the red line\n");
        // CANCEL: its own key appears with the transfer, sends its command, and the engine takes it at the next pass.
        const auto busySpots = survey(app, true, false, 36.0f, 14.0f, [&] { prepare(alpha, false, false); });
        const Found* cancel = spotFor(busySpots, expect("STORE_CANCEL", 0, 0, ""));
        CHECK(cancel != nullptr);
        if (cancel != nullptr) {
            Access::tap(app, &sent);
            (void)Access::answers(app);
            sent.clear();
            clickThenHold(app, true, false, cancel->x, cancel->y);
            CHECK_EQ(sent.size(), 1u);
            frame(app);
            const auto a2 = Access::answers(app);
            CHECK_EQ(a2.size(), 1u);
            if (a2.size() == 1u) {
                CHECK_EQ(a2[0].first, static_cast<std::uint32_t>(FOXAPI_OP_STORE_CANCEL));
                CHECK_EQ(a2[0].second, FOXAPI_OK);
            }
            std::printf("      CANCEL: shown only with a transfer, queued by its click, accepted at the next pass\n");
        }
        Access::endTransfer(false, "Alpha Decoder", "cancelled");
        frame(app);
        frame(app);
        CHECK(!Access::cache(app).model.busy);
        CHECK_EQ(Access::cache(app).model.resultError, std::string("cancelled"));
    }
    Access::tap(app, nullptr);
}

// THE RAIL'S EFFECTS, as clicks make them: Update (the engine takes it at the next pass - here against a transfer
// that is in flight, which a download is not allowed to follow, so nothing leaves the machine), Remove and Confirm
// (the quarantined file goes at the next pass, its row with it and the sentence in the same frame), the URL field.
void testRailControlsLand() {
    std::printf("  E2: the rail's keys land at the next pass: Update, Remove then Confirm, and the catalogue URL\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    frame(app, false, true);
    frame(app, false, true);
    ImGui::GetIO().AddMousePosEvent(2000.0f, 2000.0f);
    const std::string update = expect("APP_STORE_UPDATE", 0, 0, kStageId);
    Access::Sent sent;
    const auto spotsOf = [&](const std::string& armed) {
        return survey(app, false, true, 20.0f, 8.0f, [&] {
            Access::setBlockedConfirm(app, armed);
            Access::look(app);
        });
    };
    const std::vector<Found> plain = spotsOf("");
    const Found* upd = spotFor(plain, update);
    CHECK(upd != nullptr);
    Access::tap(app, &sent);
    if (upd != nullptr) {
        Access::setBlockedConfirm(app, "");
        (void)Access::answers(app);
        sent.clear();
        clickThenHold(app, false, true, upd->x, upd->y);
        CHECK_EQ(sent.size(), 1u);
        Access::beginTransfer(app, "Other");  // a download may not follow another: the engine's gate, at its pass
        frame(app, false, true);
        const auto answers = Access::answers(app);
        CHECK_EQ(answers.size(), 1u);
        if (answers.size() == 1u) {
            CHECK_EQ(answers[0].first, static_cast<std::uint32_t>(FOXAPP_OP_STORE_UPDATE));
            CHECK_EQ(answers[0].second, FOXAPI_OK);
        }
        Access::endTransfer(false, "Other", "x");
        frame(app, false, true);
        frame(app, false, true);
        CHECK(!Access::installPending(app));
    }
    // REMOVE, then CONFIRM: the first press only arms; the second sends the removal of the file; the file, the row
    // and the sentence go at the next pass.
    {
        const std::vector<Found> armedSpots = spotsOf(kStage1);
        Access::tap(app, &sent);
        const Found* confirm = spotFor(armedSpots, expect("APP_STORE_REMOVE_BLOCKED", 0, 0, kStage1));
        CHECK(confirm != nullptr);
        // the arming key: the spot of `plain` that is silent and arms the row
        const Found* arm = nullptr;
        for (const Found& fd : plain) {
            if (!fd.command.empty()) { continue; }
            Access::setBlockedConfirm(app, "");
            Access::look(app);
            clickNoPass(app, false, true, fd.x, fd.y);
            if (Access::blockedConfirm(app) == kStage1) {
                arm = &fd;
                break;
            }
        }
        CHECK(arm != nullptr);
        if (arm != nullptr && confirm != nullptr) {
            const std::filesystem::path quarantined = g_plugins / (kStage1 + ".disabled");
            Access::setBlockedConfirm(app, "");
            Access::look(app);
            sent.clear();
            clickThenHold(app, false, true, arm->x, arm->y);
            CHECK(sent.empty());  // the first press sends nothing
            frame(app, false, true);
            CHECK_EQ(Access::blockedConfirm(app), kStage1);
            const std::size_t before = Access::cache(app).blocked.size();
            CHECK_EQ(before, 1u);
            sent.clear();
            (void)Access::answers(app);
            clickThenHold(app, false, true, confirm->x, confirm->y);
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) { CHECK_EQ(describe(sent[0]), expect("APP_STORE_REMOVE_BLOCKED", 0, 0, kStage1)); }
            CHECK(Access::blockedConfirm(app).empty());                       // the click disarmed it
            CHECK_EQ(Access::cache(app).blocked.size(), 1u);                  // not in the click's frame: the row is still up
            frame(app, false, true);                                          // the engine's next pass
            frame(app, false, true);
            CHECK_EQ(Access::cache(app).blocked.size(), 0u);                  // the row is gone...
            CHECK_EQ(Access::statusLines(app).installReport, "Removed " + kStage1);  // ...and the sentence is up
            const auto answers = Access::answers(app);
            bool ok = false;
            for (const auto& a : answers) { ok = ok || (a.first == static_cast<std::uint32_t>(FOXAPP_OP_STORE_REMOVE_BLOCKED) && a.second == FOXAPI_OK); }
            CHECK(ok);
            std::printf("      REMOVE then CONFIRM: the first press arms, the second sends REMOVE_BLOCKED for the file; the row and \"%s\" arrive at the next pass\n",
                        Access::statusLines(app).installReport.c_str());
            (void)quarantined;
        }
    }
    Access::tap(app, nullptr);
}

void testUrlField() {
    std::printf("  E3: the catalogue URL field commits when it is left, as a command, and the table shows it a pass after\n");
    Env f;
    AppWindow& app = f.app;
    frame(app, false, true);
    frame(app, false, true);
    ImGui::GetIO().AddMousePosEvent(2000.0f, 2000.0f);
    ImGuiIO& io = ImGui::GetIO();
    Access::Sent sent;
    Access::tap(app, &sent);
    Access::setUrlBuf(app, "");
    frame(app, false, true);
    // Find the field: the widget under whose pointer a click makes the window want text.
    const std::vector<Hit> spots = sweep(app, false, true, 20.0f, 8.0f);
    const Hit* field = nullptr;
    for (const Hit& h : spots) {
        io.AddMousePosEvent(h.x, h.y);
        drawOnly(app, false, true);
        drawOnly(app, false, true);
        io.AddMouseButtonEvent(0, true);
        drawOnly(app, false, true);
        io.AddMouseButtonEvent(0, false);
        drawOnly(app, false, true);
        if (io.WantTextInput) {
            field = &h;
            break;
        }
    }
    CHECK(field != nullptr);
    if (field == nullptr) { return; }
    const std::string typed = f.w.index.string();
    io.AddInputCharactersUTF8(typed.c_str());
    drawOnly(app, false, true);
    drawOnly(app, false, true);
    sent.clear();
    CHECK(sent.empty());  // typing commits nothing: only leaving the field does
    // Leave it: a click in the rail's empty lower part. (Free-running, the pass parked from before it to the next frame.)
    Access::park(app);
    clickNoPass(app, false, true, 160.0f, 880.0f);
    CHECK(!io.WantTextInput);
    std::size_t url = 0;
    std::string seen;
    for (const auto& q : sent) {
        if (q.c.op == FOXAPP_OP_SET_CATALOGUE_URL) {
            ++url;
            seen = q.longText.empty() ? q.c.text : q.longText;
        }
    }
    CHECK_EQ(url, 1u);
    CHECK_EQ(seen, typed);
    CHECK(Access::engineUrl(app) != typed);  // queued, not applied in the click's frame
    frame(app, false, true);
    CHECK_EQ(Access::engineUrl(app), typed);  // applied at the pass
    frame(app, true, true);
    Access::setOpen(app, true);
    frame(app, true, false);
    CHECK_EQ(Access::cache(app).model.sourceUrl, typed);  // and the table's fact, a pass after the command
    Access::tap(app, nullptr);
    std::printf("      typing sends nothing; leaving the field sends SET_CATALOGUE_URL once; the engine has it at the next pass\n");
}

// --- F -----------------------------------------------------------------------

std::string otherLanguage() {
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (l.code != "en") { return l.code; }
    }
    return std::string();
}

void testConsentsAndRun() {
    std::printf("  F: the consents are cleared on the window's own thread from the table, and the ADD ALL run is lettered true\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    frame(app, true, true);  // (no window open: only the rail tier is read)
    const auto tick = [&] {
        Access::deck(app).selected = indexOf(app, "beta.legal");
        Access::deck(app).legalAck = true;
        Access::deck(app).addAllAck = true;
    };

    // (a) A FETCH BEGAN, SEEN: entry 1 with value[1] 1 in a block the window reads - the window shut, the rail drawn.
    tick();
    Access::submit(app, cmd::make(FOXAPI_OP_STORE_FETCH));
    Access::lookAtFetch(app);               // the engine's pass: the fetch is in flight
    CHECK(Access::deck(app).legalAck);      // (not read yet)
    (void)Access::lists(app, false);        // the rail tier reads the block
    CHECK(Access::cache(app).transfer && Access::cache(app).transferKind == 1);
    CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck && Access::deck(app).selected == -1);
    Access::after(app);
    CHECK(passUntil(app, [&] { return !Access::catalogPending(app) && Access::catalogSize(app) == 7u; }));
    pass(app);
    pass(app);

    // (b) A FETCH THAT NOBODY SAW: it starts and lands between two reads of the window (a local index.json can), and
    // brings a catalogue whose notice is another text. Nothing read the block in between; the full tier reads the
    // rows, finds them different from the ones it held, and clears the consents.
    Access::setOpen(app, true);
    frame(app);                                       // the rows are held
    tick();
    const std::uint32_t railBefore = Access::cache(app).railRefreshes;
    Access::submit(app, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, f.w.indexB.string()));
    Access::submit(app, cmd::make(FOXAPI_OP_STORE_FETCH));
    bool sawFetch = false;
    for (int i = 0; i < 3000; ++i) {
        Access::look(app);
        Access::after(app);
        if (Access::catalogSize(app) == 7u && !Access::catalogPending(app) && Access::engineUrl(app) == f.w.indexB.string()) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pass(app);
    CHECK(Access::deck(app).legalAck && Access::deck(app).addAllAck);  // nobody has read anything: still given
    CHECK_EQ(Access::cache(app).railRefreshes, railBefore);
    (void)sawFetch;
    (void)Access::lists(app, true);
    CHECK(!Access::cache(app).transfer);                               // no transfer entry was ever read...
    CHECK(Access::cache(app).railRefreshes > railBefore);
    CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck && Access::deck(app).selected == -1);  // ...and the consents are gone
    CHECK_EQ(mod(app, "beta.legal")->plate.legalNotice, std::string(300, 'M'));

    // (c) THE SAME CATALOGUE AGAIN, unseen: nothing about it changed, so nothing is cleared (the old window cleared at
    // every fetch; recorded - a tick for a notice that is the same text stays valid, and a fetch the window SAW
    // clears it as in (a)).
    tick();
    Access::submit(app, cmd::make(FOXAPI_OP_STORE_FETCH));
    for (int i = 0; i < 3000; ++i) {
        Access::look(app);
        Access::after(app);
        if (Access::catalogSize(app) == 7u && !Access::catalogPending(app) && i > 2) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    pass(app);
    (void)Access::lists(app, true);
    CHECK(Access::deck(app).legalAck && Access::deck(app).addAllAck);
    Access::deck(app) = cascade::gui::PluginStoreDeck{};

    // (d) THE ADD ALL RUN, for real over three entries the loader's name rule refuses (every install is refused before
    // any network): its position is the module in flight - 1-based, each number once - where the old window lettered
    // one more for the first two; the tick given for the run stays while it runs and goes when it ends; and the
    // summary is the red line of a run with failures.
    CHECK(fetchCatalogue(app, f.w.indexRun, 3));
    frame(app);
    frame(app);
    Access::deck(app).addAllAck = true;
    Access::Sent sent;
    Access::tap(app, &sent);
    Access::park(app);  // (free-running: from here the run is walked publish by publish, none missed)
    Access::submit(app, cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, "", 1));
    std::vector<std::string> trace;
    std::vector<std::string> oldTrace;
    bool everRan = false;
    bool tickHeldWhileRunning = true;
    std::set<std::string> seen;
    for (int i = 0; i < 6000; ++i) {
        Access::lookAtPublish(app);
        imguiFrame(app, true, true);
        // Here the engine's state is what was just published and read: the window's line is the true one.
        const bool running = Access::cache(app).model.addAllRunning;
        CHECK_EQ(running, Access::addAllActive(app));
        CHECK_EQ(Access::cache(app).model.addAllProgress, Access::trueAddAllLine(app));
        if (running) {
            everRan = true;
            tickHeldWhileRunning = tickHeldWhileRunning && Access::deck(app).addAllAck;
            const std::string line = Access::cache(app).model.addAllProgress;
            if (trace.empty() || trace.back() != line) {
                trace.push_back(line);
                oldTrace.push_back(Access::oldAddAllLine(app));
            }
            seen.insert(line);
        }
        Access::afterPublish(app);
        if (everRan && !Access::addAllActive(app) && !Access::installPending(app)) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    frame(app);
    frame(app);
    CHECK(everRan);
    CHECK(tickHeldWhileRunning);
    CHECK(!Access::deck(app).addAllAck);  // the run ended: the tick was for that run
    const std::set<std::string> allowed = {"installing 1 of 3: starting", "installing 1 of 3: Bad One", "installing 2 of 3: Bad Two",
                                           "installing 3 of 3: Bad Three"};
    std::string joined;
    for (const std::string& t : trace) { joined += (joined.empty() ? "" : " | ") + t; }
    std::string oldJoined;
    for (const std::string& t : oldTrace) { oldJoined += (oldJoined.empty() ? "" : " | ") + t; }
    std::printf("      the run as lettered: %s\n      (the old window lettered: %s)\n", joined.c_str(), oldJoined.c_str());
    for (const std::string& t : trace) { CHECK(allowed.count(t) == 1u); }
    CHECK(seen.count("installing 1 of 3: Bad One") == 1u && seen.count("installing 2 of 3: Bad Two") == 1u &&
          seen.count("installing 3 of 3: Bad Three") == 1u);
    CHECK(!Access::cache(app).model.addAllRunning);
    CHECK(Access::cache(app).model.addAllProgress.empty());
    CHECK(Access::cache(app).model.addAllSummary.rfind("0 installed, 3 failed.", 0) == 0);
    CHECK(Access::cache(app).model.addAllFailed);                       // the red line
    CHECK_EQ(Access::cache(app).model.addAllSummary, Access::engineSummary(app));
    CHECK_EQ(countOf(sent, "APP_STORE_ADD_ALL"), 1u);
    Access::tap(app, nullptr);
    // The old window lettered one more for the first two modules - and the same for the last.
    CHECK(oldTrace.size() >= 3u);
    if (oldTrace.size() >= 3u) {
        CHECK(std::find(oldTrace.begin(), oldTrace.end(), "installing 2 of 3: Bad One") != oldTrace.end());
        CHECK(std::find(oldTrace.begin(), oldTrace.end(), "installing 3 of 3: Bad Two") != oldTrace.end());
        CHECK(std::find(oldTrace.begin(), oldTrace.end(), "installing 3 of 3: Bad Three") != oldTrace.end());
    }

    // (e) THE TWO LINES AND THEIR INK: the catalogue's line is information; the ADD ALL summary is red (a failed run)
    // or plain, as the engine says, and clears with its text.
    CHECK(!Access::cache(app).model.sourceStatus.empty());
    Access::setAddAllFailed(app, false);
    frame(app);
    frame(app);
    CHECK(!Access::cache(app).model.addAllFailed && !Access::cache(app).model.addAllSummary.empty());
    Access::setAddAllFailed(app, true);
    frame(app);
    frame(app);
    CHECK(Access::cache(app).model.addAllFailed);
    Access::setCatalogStatus(app, "0 plugins in the catalogue");
    frame(app);
    frame(app);
    CHECK_EQ(Access::cache(app).model.sourceStatus, std::string("0 plugins in the catalogue"));
    Access::setAddAllSummary(app, "");
    frame(app);
    frame(app);
    CHECK(Access::cache(app).model.addAllSummary.empty());
    std::printf("      the consents: a fetch seen, a fetch unseen that brings another catalogue, a run's end; the summary red or plain, its text cleared, the catalogue's line\n");
}

// THE RETIRED ROW'S CONFIRMATION IS KEYED ON ITS FILE: with two retired modules, the first press arms the SECOND, then
// the first is removed behind the window's back - the second row is now the first one, and it is the one still armed.
void testRetiredConfirmFollowsTheFile() {
    std::printf("  F2: the retired row's Remove is armed for a FILE, and a rescan that moves the rows does not move it\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.indexC, 7));
    frame(app, false, true);
    frame(app, false, true);
    const cascade::gui::StoreListCache& rail = Access::cache(app);
    CHECK_EQ(rail.blocked.size(), 2u);
    if (rail.blocked.size() != 2u) { return; }
    const std::string first = rail.blocked[0].file;
    const std::string second = rail.blocked[1].file;
    CHECK(first != second);
    // Armed on the second, as a click on its Remove leaves it.
    Access::setBlockedConfirm(app, second);
    frame(app, false, true);
    CHECK_EQ(Access::blockedConfirm(app), second);
    // The first goes (any path that rescans: here the command itself, applied at once).
    Access::command(app, cmd::makeText(FOXAPP_OP_STORE_REMOVE_BLOCKED, first));
    frame(app, false, true);
    frame(app, false, true);
    CHECK_EQ(Access::cache(app).blocked.size(), 1u);
    CHECK_EQ(Access::cache(app).blocked[0].file, second);
    CHECK_EQ(Access::blockedConfirm(app), second);  // still armed for ITS file, now in the first place
    // ...and its Confirm is on the screen and removes THAT file (the sweep finds it; an index would have lost it).
    Access::Sent sent;
    const auto found = survey(app, false, true, 20.0f, 8.0f, [&] {
        Access::setBlockedConfirm(app, second);
        Access::look(app);
    });
    bool confirmSends = false;
    for (const Found& fd : found) { confirmSends = confirmSends || fd.command == expect("APP_STORE_REMOVE_BLOCKED", 0, 0, second); }
    CHECK(confirmSends);
    // A confirmation for a file that is not listed any more is not carried to a later module of the same name.
    Access::setBlockedConfirm(app, first);
    frame(app, false, true);
    CHECK(Access::blockedConfirm(app).empty());
    Access::command(app, cmd::makeText(FOXAPP_OP_STORE_REMOVE_BLOCKED, second));
    Access::setBlockedConfirm(app, second);
    frame(app, false, true);
    frame(app, false, true);
    frame(app, false, true);
    CHECK_EQ(Access::cache(app).blocked.size(), 0u);
    CHECK(Access::blockedConfirm(app).empty());
    std::printf("      armed for %s; after %s was removed the row moved up and stayed armed; a file that left disarms\n", second.c_str(), first.c_str());
}

void testLanguageAndRail() {
    std::printf("  F3: the retired row's sentence follows the language (rebuilt by the engine); the plugins key's counts are the engine's\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    frame(app, false, true);
    frame(app, false, true);
    const std::string english = Access::cache(app).blocked.empty() ? std::string() : Access::cache(app).blocked[0].message;
    CHECK(!english.empty());
    const std::string other = otherLanguage();
    CHECK(!other.empty());
    if (!other.empty() && !english.empty()) {
        const std::uint32_t r0 = Access::cache(app).railRefreshes;
        (void)cascade::i18n::setLanguage(other);
        frame(app, false, true);
        frame(app, false, true);
        const std::string translated = Access::cache(app).blocked[0].message;
        CHECK(Access::cache(app).railRefreshes > r0);
        CHECK(translated != english);
        (void)cascade::i18n::setLanguage("en");
        frame(app, false, true);
        frame(app, false, true);
        CHECK_EQ(Access::cache(app).blocked[0].message, english);
        std::printf("      in %s: \"%.50s...\" (rebuilt when the language moved; back in English it is as it was)\n", other.c_str(), translated.c_str());
    }
    // The plugins key: refused records, fitted decoders and the decoders fed - the state's and the table's, equal to the
    // engine's own counts, with the receiver stopped and running.
    for (int running = 0; running < 2; ++running) {
        if (running == 1) {
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
            Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 1));
            pass(app);
            pass(app);
        }
        frame(app, false, true);
        const auto& st = Access::state(app);
        std::size_t refused = 0;
        for (const cc::LoadedPlugin& p : Access::host(app)) { refused += p.loaded ? 0u : 1u; }
        CHECK_EQ(Access::cache(app).refused, refused);
        CHECK_EQ(static_cast<std::size_t>(st.decodersFitted), Access::loadedDecoders(app));
        CHECK_EQ(static_cast<std::size_t>(running == 1 ? st.decodersRunning : 0u), running == 1 ? Access::fedDecoders(app) : 0u);
        CHECK_EQ(Access::cache(app).blocked.size(), Access::engineBlocked(app).size());
    }
    Access::command(app, cmd::makeInt(FOXAPI_OP_RUN, 0));
}

// --- main ----------------------------------------------------------------------

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_store_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_psw_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    g_plugins = g_scratch / "plugins";
    Access::installHooks();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 2000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testModelEqualsTheOldWindow();
    testWhatTheWindowIsTold();
    testCacheFollowsListSeq();
    testFirstOpen();
    testDrawnAndClicked();
    testNothingFetched();
    testRailRows();
    testControlsLandAtTheNextPass();
    testRailControlsLand();
    testUrlField();
    testConsentsAndRun();
    testRetiredConfirmFollowsTheFile();
    testLanguageAndRail();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_plugin_store_window");
}
