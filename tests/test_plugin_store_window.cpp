// test_plugin_store_window.cpp - THE PLUGIN STORE READS ITS ENGINE API SESSION (window port, group 10: the rail's store key, the plugins
// key, the retired rows, the store window and storeLists; engine-stage4.md 6.5, API 0.3 sections 8.9 and 9.4) - REWRITTEN IN STUB CAMPAIGN
// BATCH B4 AGAINST THE 0.99.72 - 0.99.77 STORE.
//
// This was never master's test: it is this branch's own port test, written when the store window moved onto the table, and it restated "the
// model the window built from the engine's own members before the port" - the 0.99.42 store: one row selected on a plate beside a list, no
// categories, no sites, no pictures, no kept copy. The 0.99.72 store (tabs, cards, a page for each plugin, an UPDATES tab, the catalogue kept
// offline) and the 0.99.74 plugin sites replaced that model, and what the window is held equal to here is master's: the model
// AppWindow::buildPluginStoreModel / fillStoreModule built from the catalogue's members (restated from MASTER's source in
// AppWindowTestAccess::masterModel, not from this tree's storeLists).
//
// What the store draws is the table's CATALOGUE (with its ABI, size and BUILD_HERE), the facts of each entry (the texts, the shop-window fields,
// where it comes from, how it stands against what is installed, its pictures), PLUGINS and the facts of the host's record, STORE_UPDATES,
// STORE_BLOCKED and the two facts of each retired row, STORE_ACTIVITY (the transfer, read live, and the ADD ALL run), STORE_PICTURES, the
// store's own facts (the kept copy, the sites, the old copies, the clean-up's report) and the status lines the engine handed over; what it
// changes it changes by commands on its session. This holds the window's side of that against the real engine, over a REAL fetch of a local
// index.json (an address with no "://" is read from disk) laid over a plugin folder of this test's own: a retired module with an update plan,
// an updatable fitted one whose catalogue file is NOT the file on disk, and five entries for the columns (the world of test_engine_api_03's
// group I).
//   A  the model the window draws from the table, held equal field for field to the one master's 0.99.72 window built (restated), in every
//      state: before a fetch, after one, with a page open and the notice ticked or not, a module stopped, a grant, pictures queued / ready /
//      failed, the regional list's say-so, an old copy and the clean-up's report, a kept copy and a failed refresh beside it, a plugin site
//      added and removed, a transfer in flight, a failure, a refetch that failed
//   A2 how an entry stands against what is installed, by id and version: an update, installed, a newer copy installed
//   B  what the window is told, entry by entry, from the index, stated by hand
//   C  the cache: one rebuild when listSeq moves, the full tier only while the window is drawn, the live progress, and the allocations of a
//      settled frame - with the richer model
//   D  the real window drawn and clicked: the first open reads once (and not over a catalogue read this session), and every command a key of
//      the 0.99.72 page can send goes out with the right operands - GET, UPDATE, CHECK AGAIN, CANCEL, GET EVERYTHING, UPDATE ALL, CLEAN UP,
//      the SITES panel's ADD (only with the consent ticked) and REMOVE, the page's pictures - and a card's click opens its page; the rail's
//      retired rows' Update, Remove and Confirm
//   E  a click lands at the engine's NEXT pass and the window reads the engine's value the frame after
//   F  what the engine used to call into the window for: the consents cleared on the window's own thread, from the table - a fetch begun, a run
//      ended, a different catalogue - and the ADD ALL run's line through a real run
//
// THE TEST-ACCESSOR TRAP: a ported member reads the SESSION, so every frame here looks first, as drawUi does (the engine's pass takes the session's
// queue, publishes, the window copies the status lines and reads its state), draws, and lets the NEXT look take what the draw sent; the workers
// are polled after the draw, as drawUi polls them. pumpTransmitKey is not run, so the session's answers are still there to be read.
//
// AGAINST A FREE-RUNNING PASS (FOXSDR_TEST_PASS=free, tests/pass_seam.hpp): every read or write of an engine field from this thread goes through
// passseam::on/read (run on the pass, with the pass out of the way). Where a statement is about ONE publish - "the click only queued it", the
// survey's "nothing is applied: no pass runs", "a fetch began, seen" and the ADD ALL run lettered publish by publish - the pass is PARKED at its
// publish (Engine::TestHooks::betweenPublishes, set before any thread runs): the state and the lists of that publish are the newest, the engine's
// fields are as they were published (the pass's workers have not run), and while it is parked they are read here directly.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
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
#include "test_host.hpp"
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
struct IdxShot {
    std::string sha, caption;
};
struct IdxEntry {
    std::string id, name, version, author, licence, summary, description, homepage, legal, floor;
    // the shop-window fields (0.99.72): a category's id, the experimental mark, the paragraph about this version, a YYYY-MM-DD date, the names of
    // the capabilities the binary declares, the pictures
    std::string category, whatsNew, published;
    bool experimental = false;
    std::vector<std::string> capabilities;
    std::vector<IdxShot> shots;
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
        if (!e.category.empty()) { j += ",\"category\":\"" + e.category + "\""; }
        if (e.experimental) { j += ",\"experimental\":true"; }
        if (!e.whatsNew.empty()) { j += ",\"whatsNew\":\"" + e.whatsNew + "\""; }
        if (!e.published.empty()) { j += ",\"published\":\"" + e.published + "\""; }
        if (!e.capabilities.empty()) {
            j += ",\"capabilities\":[";
            for (std::size_t k = 0; k < e.capabilities.size(); ++k) { j += (k == 0 ? "\"" : ",\"") + e.capabilities[k] + "\""; }
            j += "]";
        }
        if (!e.shots.empty()) {
            j += ",\"screenshots\":[";
            for (std::size_t k = 0; k < e.shots.size(); ++k) {
                j += k == 0 ? "{" : ",{";
                j += "\"url\":\"https://example.invalid/shots/" + e.id + "-" + std::to_string(k) + ".png\",\"sha256\":\"" + e.shots[k].sha +
                     "\",\"caption\":\"" + e.shots[k].caption + "\",\"width\":1280,\"height\":800}";
            }
            j += "]";
        }
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

// --- THE TABLE'S READS, COUNTED (the settled-frame property is "0 reads", and the door is where to count them) -------------------------
// The window holds a pointer to the engine's function table; a test may point it at a copy whose read_list and get_setting count and forward.
const FoxEngineApi* g_realApi = nullptr;
FoxEngineApi g_countingApi;
long g_listReads = 0;
long g_factReads = 0;
long g_readsOfList[64] = {};  // by list id (for the report of a read that should not be there)
int32_t FOXAPI_CALL countedReadList(FoxSession* s, uint32_t list, uint32_t index, FoxListItem* out) {
    ++g_listReads;
    ++g_readsOfList[list & 63u];
    return g_realApi->read_list(s, list, index, out);
}
int32_t FOXAPI_CALL countedGetSetting(FoxSession* s, const char* key, char* buf, size_t cap) {
    ++g_factReads;
    return g_realApi->get_setting(s, key, buf, cap);
}

// A REBUILD OF THE WINDOW'S MODEL, in allocations, for the seven entries of this world: measured (205 for these seven entries on the machine this
// was written on - about 24 facts an entry through the table's get_setting, each a string given room, plus the lists' items and the model's
// own vectors), and bounded with headroom so a fact added to every card moves this on purpose. Master built the same model EVERY FRAME (C
// prints its cost: about 520 a frame); this is paid once for each move of listSeq.
constexpr long kRebuildAllocBound = 400;

// THE PICTURE TRANSPORT STAND-IN (PluginRepo::setTransportForTest): nothing in this test reaches a network. Every request is counted, waits while
// g_netHold is set, and is answered HTTP 503 (a catalogue is read from disk - an address with no "://" - and a download is refused by the
// loader's name rule before any transport, so pictures are all that ever asks).
std::atomic<bool> g_netHold{false};
std::atomic<int> g_netHits{0};

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
        return parked() ? f() : passseam::read<R>(th::engine(a), f);
    }
    static void doOnPass(AppWindow& a, const std::function<void()>& f) {
        if (parked()) {
            f();
        } else {
            passseam::on(th::engine(a), f);
        }
    }
    // The pass parked at its next publish (free-running; nothing otherwise) - for the click, the survey's clicks, the
    // ADD ALL walk - and let go by the next look, the next workers' poll or dropQueued.
    static void park(AppWindow& a) { parkAtPublish(th::engine(a)); }
    // ...at the first publish of a fetch in flight.
    static void parkAtFetch(AppWindow& a) {
        parkAtPublish(th::engine(a), [&a] { return th::engine(a).catalogPending_; });
    }
    // THE LOOK OF ONE PUBLISH: free-running, the pass parked at its next publish (the one parked now let go) and that
    // publish read; otherwise the look. With its `after`: free-running nothing (the parked pass polls its workers when it
    // goes on), otherwise the workers.
    static void lookAtPublish(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free) {
            look(a);
            return;
        }
        parkAtPublish(th::engine(a));
        a.statusLines_ = th::engine(a).statusText();
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
        a.statusLines_ = th::engine(a).statusText();
        a.refreshUiState();
    }

    // One frame's engine part, in drawUi's order: the session's queue taken, the publish, the status copy, the
    // window's read of the state.
    static void look(AppWindow& a) {
        unpark();
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            th::engine(a).pumpInputs();
            th::engine(a).pumpPlugins();
            (void)th::engine(a).pumpPublish();
        });
        a.statusLines_ = th::engine(a).statusText();
        a.refreshUiState();
    }
    // ...and the end of the frame: the workers polled (a fetch or a download that finished is applied, the ADD ALL
    // queue steps) and the lines handed over, as drawUi does after the draw.
    static void after(AppWindow& a) {
        unpark();
        passseam::workers(th::engine(a), [&] {
            th::engine(a).pumpWorkers();
            th::engine(a).pumpAudio();
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
        doOnPass(a, [&] { (void)th::engine(a).applyCommand(q.c, q.longText); });
    }
    static void command(AppWindow& a, const FoxCommand& c) {
        doOnPass(a, [&] { (void)th::engine(a).applyCommand(c); });
    }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static std::size_t loadedDecoders(AppWindow& a) {
        return onPass<std::size_t>(a, [&] { return th::engine(a).loadedDecoderCount(); });
    }
    static std::size_t fedDecoders(AppWindow& a) {
        return onPass<std::size_t>(a, [&] { return th::engine(a).fedDecoderCount(); });
    }

    // The table's reads, counted at the door (see countedReadList): on/off for this window, and the counts since they were last reset.
    static void countReads(AppWindow& a, bool on) {
        if (on) {
            if (a.engineApi_ != &g_countingApi) {
                g_realApi = a.engineApi_;
                g_countingApi = *g_realApi;
                g_countingApi.read_list = &countedReadList;
                g_countingApi.get_setting = &countedGetSetting;
                a.engineApi_ = &g_countingApi;
            }
            resetReads();
        } else if (a.engineApi_ == &g_countingApi) {
            a.engineApi_ = g_realApi;
        }
    }
    static void resetReads() {
        g_listReads = 0;
        g_factReads = 0;
        for (long& n : g_readsOfList) { n = 0; }
    }
    static void printReadsByList() {
        for (unsigned i = 0; i < 64; ++i) {
            if (g_readsOfList[i] != 0) { std::printf("        list %u: %ld read(s)\n", i, g_readsOfList[i]); }
        }
    }
    // The reads of the STORE'S lists and facts: not the lists whose entries are "read at the call - never moves listSeq, read it each time you draw"
    // by design (API 0.3: PATCH_LIVE 47, GPS 48, SERVERS 49), which the window's state refresh and the patch and source panels read every frame.
    static void reads(long& lists, long& facts) {
        lists = g_listReads - g_readsOfList[47] - g_readsOfList[48] - g_readsOfList[49];
        facts = g_factReads;
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
    static std::size_t catalogSize(AppWindow& a) { return onPass<std::size_t>(a, [&] { return th::engine(a).catalog_.size(); }); }
    static bool catalogPending(AppWindow& a) { return onPass<bool>(a, [&] { return th::engine(a).catalogPending_; }); }
    static bool installPending(AppWindow& a) { return onPass<bool>(a, [&] { return th::engine(a).installPending_; }); }
    static bool addAllActive(AppWindow& a) { return onPass<bool>(a, [&] { return th::engine(a).addAllRun_.active; }); }
    static std::size_t addAllNext(AppWindow& a) { return onPass<std::size_t>(a, [&] { return th::engine(a).addAllRun_.next; }); }
    static std::size_t addAllTotal(AppWindow& a) { return onPass<std::size_t>(a, [&] { return th::engine(a).addAllRun_.total; }); }
    static std::string engineUrl(AppWindow& a) { return onPass<std::string>(a, [&] { return th::engine(a).pluginCatalogueUrl_; }); }
    static std::string engineSummary(AppWindow& a) { return onPass<std::string>(a, [&] { return th::engine(a).addAllSummary_; }); }
    // What writes the engine's lines (the table's own hook is not in an engine the window holds): set where the engine
    // keeps them, handed over at the end of the next frame.
    static void setAddAllFailed(AppWindow& a, bool v) { doOnPass(a, [&] { th::engine(a).addAllFailed_ = v; }); }
    static void setCatalogStatus(AppWindow& a, const std::string& v) { doOnPass(a, [&] { th::engine(a).catalogStatus_ = v; }); }
    static void setAddAllSummary(AppWindow& a, const std::string& v) { doOnPass(a, [&] { th::engine(a).addAllSummary_ = v; }); }
    static std::string blockedReason(AppWindow& a, int idx, bool acked) {
        return onPass<std::string>(a, [&] { return th::engine(a).pluginInstallBlockedReason(idx, acked); });
    }
    static bool stopped(AppWindow& a, const std::string& key) { return onPass<bool>(a, [&] { return th::engine(a).pluginIsStopped(key); }); }
    static std::vector<cascade::core::LoadedPlugin> host(AppWindow& a) {
        return onPass<std::vector<cascade::core::LoadedPlugin>>(a, [&] { return th::engine(a).pluginHost_.plugins(); });
    }
    static std::vector<cascade::core::BlockedPlugin> engineBlocked(AppWindow& a) {
        return onPass<std::vector<cascade::core::BlockedPlugin>>(a, [&] { return th::engine(a).pluginBlocked_; });
    }
    static std::vector<cascade::core::PluginUpdate> plans(AppWindow& a) {
        return onPass<std::vector<cascade::core::PluginUpdate>>(a, [&] { return th::engine(a).plannedPluginUpdates(); });
    }

    // A transfer as its worker would have left it (test_engine_api_03, group I): the engine's install slot holds a
    // promise the test keeps, so a download is in flight (installPending_, the busy name) until the test says it is
    // over, with a result the pump applies exactly as it applies a real worker's. Nothing touches the network.
    static std::promise<Engine::PluginInstallResult>& installPromise() {
        static std::promise<Engine::PluginInstallResult> p;
        return p;
    }
    static void beginTransfer(AppWindow& a, const std::string& name, const std::string& key = std::string()) {
        doOnPass(a, [&] {
            installPromise() = std::promise<Engine::PluginInstallResult>();
            th::engine(a).installFuture_ = installPromise().get_future();
            th::engine(a).installBusyName_ = name;
            th::engine(a).installBusyId_ = key;
            th::engine(a).installPending_ = true;
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
    static void setProgress(AppWindow& a, float p) { th::engine(a).pluginRepo_.setProgressForTest(p); }

    // THE MODEL AS MASTER'S 0.99.72 - 0.99.77 WINDOW BUILT IT, restated here from MASTER's own source (origin/master 04cba53a,
    // src/gui/app_window.cpp: AppWindow::buildPluginStoreModel@20314 and AppWindow::fillStoreModule@20114) and NOT from this tree's
    // storeLists / fillStoreModule: master built the whole model every frame the store was open from the members of the window that
    // owned the catalogue; the engine owns them now (the same names, Engine's members), and this is that code read against them. The
    // statement of the 0.99.42 version this test used to hold - one row selected, no sites, no pictures, no kept copy - is gone with the
    // window it described: deck.selected is the OPEN PAGE's id (pageId) and the tick belongs to that page.
    //
    // THE FOUR PLACES THE TABLE DIFFERS FROM MASTER, each deliberate and asserted where it is compared (sameModule / sameModel):
    //   1. a name is cut at the list item's 95 bytes (API 0.3 8.9; no live catalogue entry comes near it);
    //   2. the ADD ALL line says the true position - "the module in flight" - where master lettered min(next + 1, total) (a one-too-many
    //      the 0.99.84 merge corrected, API 0.3 8.9);
    //   3. the transfer's progress is read only while one is moving (0 otherwise), where master read the repo's last figure always;
    //   4. a picture's advisory width and height are not in the table (the frame is 16:10 whatever the index says; nothing draws them).
    // Built on the pass; `allocs`, when given, gets what the build allocated, counted on the thread that built it.
    static PluginStoreModel masterModel(AppWindow& a, long* allocs = nullptr) {
        return onPass<PluginStoreModel>(a, [&] {
            const long mark = t_allocs;
            PluginStoreModel m = masterModelOnPass(a);
            if (allocs != nullptr) { *allocs += t_allocs - mark; }
            return m;
        });
    }
    static void masterFillModule(AppWindow& a, int i, const std::vector<cascade::core::PluginUpdate>& updates, StoreModule& sm) {
        Engine& eng = th::engine(a);
        const cascade::core::PluginCatalogEntry& e = eng.catalog_[static_cast<std::size_t>(i)];
        // THE CATALOGUE KEY (0.99.74): the id for an official plugin, "<id>@<site>" for a site's.
        sm.id = cascade::core::PluginRepo::catalogueKey(e.source, e.id);
        sm.thirdParty = !e.source.empty();
        sm.siteHost = sm.thirdParty ? cascade::core::PluginRepo::siteLabel(e.source) : std::string();
        ModulePlate& p = sm.plate;
        p.name = e.name;
        p.version = e.version;
        p.maker = e.author;
        p.licence = e.licence;
        p.blurb = e.description.empty() ? e.summary : e.description;
        p.summary = e.summary;
        p.homepage = e.homepage;
        // A plugin from a site the user added ALWAYS carries a notice (0.99.74).
        p.legalNotice = sm.thirdParty ? cascade::gui::storeComposeNotice(sm.siteHost, e.legalNotice) : e.legalNotice;
        if (e.regional) {
            p.originNote = cascade::i18n::tr("Offered by foxsdr.com in some countries only.");
        } else if (sm.thirdParty) {
            p.originNote = cascade::core::formatText(cascade::i18n::tr("From a site you added: %s"),
                                                     cascade::core::PluginRepo::siteDisplay(e.source, 90).c_str());
        }
        p.category = e.category;
        p.experimental = e.experimental;
        p.whatsNew = e.whatsNew;
        p.published = e.published;
        p.haveAbi = true;
        p.abiVersion = e.abiVersion;
        p.hostAbiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
        p.retirementFloor = e.minSupportedVersion;
        sm.buildsWindows = false;
        sm.buildsLinux = false;
        for (const cascade::core::PluginPlatform& pf : e.platforms) {
            if (!p.platforms.empty()) { p.platforms += ", "; }
            p.platforms += pf.os + "/" + pf.arch;
            std::string os = pf.os;
            for (char& ch : os) {
                if (ch >= 'A' && ch <= 'Z') { ch = static_cast<char>(ch - 'A' + 'a'); }
            }
            if (os == "windows") { sm.buildsWindows = true; }
            if (os == "linux") { sm.buildsLinux = true; }
        }
        const cascade::core::PluginPlatform* plat = e.thisPlatform();
        sm.haveBuildHere = plat != nullptr;
        if (plat != nullptr) {
            p.sha256 = plat->sha256;
            if (plat->sizeBytes > 0u) {
                p.haveSizeBytes = true;
                p.sizeBytes = plat->sizeBytes;
            }
        }
        sm.installableHere = e.compatible && plat != nullptr;
        if (e.capabilities != 0u || !e.capabilityNames.empty()) {
            p.haveCapabilities = true;
            p.capabilities = e.capabilities != 0u ? e.capabilities : 0x80000000u;
        }
        const cascade::core::InstallState st = eng.pluginInstallState(e);
        switch (st.kind) {
            case cascade::core::InstallStateKind::NotInstalled: sm.install = cascade::gui::StoreInstallKind::NotInstalled; break;
            case cascade::core::InstallStateKind::Installed: sm.install = cascade::gui::StoreInstallKind::Installed; break;
            case cascade::core::InstallStateKind::UpdateAvailable: sm.install = cascade::gui::StoreInstallKind::UpdateAvailable; break;
            case cascade::core::InstallStateKind::NewerInstalled: sm.install = cascade::gui::StoreInstallKind::NewerInstalled; break;
        }
        sm.installedVersion = st.installedVersion;
        p.fitted = eng.catalogEntryInstalled(e);
        // ...and if it is fitted, what the host made of it - by plugin id: the NEWEST scanned copy of this id speaks, a refused file nothing
        // names by id is found by file name as before.
        const cascade::core::LoadedPlugin* lp = nullptr;
        {
            std::string bestFile;
            std::string bestVersion;
            for (const cascade::core::ScannedModule& m : eng.scannedModules()) {
                if (m.id != e.id || m.source != e.source) { continue; }
                if (bestFile.empty() || cascade::core::PluginRepo::compareVersions(m.version, bestVersion) > 0) {
                    bestFile = m.file;
                    bestVersion = m.version;
                }
            }
            if (!bestFile.empty()) {
                for (const cascade::core::LoadedPlugin& cand : eng.pluginHost_.plugins()) {
                    if (std::filesystem::path(cand.path).filename().string() == bestFile) {
                        lp = &cand;
                        break;
                    }
                }
            }
            if (lp == nullptr) { lp = eng.installedPluginRecord(e); }
        }
        if (lp != nullptr) {
            p.fileName = std::filesystem::path(lp->path).filename().string();
            p.loaded = lp->loaded;
            p.refusalReason = lp->error;
            p.haveCapabilities = lp->loaded;
            p.capabilities = lp->capabilities;
            const std::string key = cascade::core::pluginKey(*lp);
            p.running = lp->loaded && !eng.pluginIsStopped(key);
            if (lp->hostClient != nullptr) {
                p.haveTuneGrant = true;
                p.tuneGranted = th::pluginUi(a).tuneAllowed(cascade::core::PluginUi::tuneKey(*lp));
            }
        }
        // THE TICK BELONGS TO ONE PLUGIN, the one whose page is open.
        const bool acked = sm.id == a.pluginStoreDeck_->pageId && a.pluginStoreDeck_->legalAck;
        sm.blockedReason = eng.pluginInstallBlockedReason(i, acked);
        sm.blockedReasonIfAcknowledged = eng.pluginInstallBlockedReason(i, true);
        for (const cascade::core::PluginUpdate& u : updates) {
            if (u.id != e.id || u.source != e.source) { continue; }
            sm.updateToVersion = u.toVersion;
            sm.updateReason = u.reason;
            break;
        }
        if (sm.install == cascade::gui::StoreInstallKind::UpdateAvailable && sm.updateToVersion.empty()) {
            if (!e.compatible) {
                sm.updateBlockedReason = cascade::gui::pluginAbiMismatchReason(static_cast<unsigned>(e.abiVersion),
                                                                               static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
            } else if (plat == nullptr) {
                sm.updateBlockedReason = cascade::gui::pluginNoBuildReason(std::string(cascade::core::PluginRepo::hostOs()) + "/" +
                                                                           cascade::core::PluginRepo::hostArch());
            }
        }
        for (const cascade::core::CatalogScreenshot& s : e.screenshots) {
            // (a site's picture that is not on the site's own host is not listed - the table's rule, master's mergeSite held it at the merge)
            if (!e.source.empty() && !cascade::core::PluginRepo::siteFilesAllowed(s.url, e.source)) { continue; }
            cascade::gui::StorePicture pic;
            pic.sha256 = s.sha256;
            pic.caption = s.caption;
            const Engine::PictureStatus ps = eng.pictureStatus(s);
            switch (ps.state) {
                case Engine::PictureState::None: pic.state = cascade::gui::PictureState::None; break;
                case Engine::PictureState::Pending: pic.state = cascade::gui::PictureState::Pending; break;
                case Engine::PictureState::Ready: pic.state = cascade::gui::PictureState::Ready; break;
                case Engine::PictureState::Failed: pic.state = cascade::gui::PictureState::Failed; break;
            }
            pic.path = ps.path;
            pic.reason = ps.reason;
            sm.pictures.push_back(std::move(pic));
        }
    }
    static PluginStoreModel masterModelOnPass(AppWindow& a) {
        Engine& e = th::engine(a);
        PluginStoreModel model;
        model.sourceUrl = e.pluginCatalogueUrl_;
        model.hostPlatform = std::string(cascade::core::PluginRepo::hostOs()) + "/" + cascade::core::PluginRepo::hostArch();
        // THE PLUGIN SITES, in the order they are read, each with how many of its plugins the list holds NOW (counted from the catalogue)
        // and what the last read or the kept copy said of it.
        model.maxSites = cascade::core::AppConfig::kMaxPluginSites;
        for (const std::string& siteUrl : e.pluginSites_) {
            cascade::gui::StoreSite s;
            s.url = siteUrl;
            s.label = cascade::core::PluginRepo::siteLabel(siteUrl);
            s.display = cascade::core::PluginRepo::siteDisplay(siteUrl, 90);
            for (const cascade::core::PluginCatalogEntry& ce : e.catalog_) {
                if (ce.source == siteUrl) { ++s.plugins; }
            }
            for (const Engine::SiteStatus& st : e.siteStatus_) {
                if (st.url != siteUrl) { continue; }
                switch (st.state) {
                    case Engine::SiteStatus::State::NotRead: s.state = cascade::gui::StoreSite::State::NotRead; break;
                    case Engine::SiteStatus::State::Kept: s.state = cascade::gui::StoreSite::State::Kept; break;
                    case Engine::SiteStatus::State::Read: s.state = cascade::gui::StoreSite::State::Read; break;
                    case Engine::SiteStatus::State::Failed: s.state = cascade::gui::StoreSite::State::Failed; break;
                }
                s.hiddenSameId = st.hiddenSameId;
                s.hiddenOffHost = st.hiddenOffHost;
                s.error = st.error;
                break;
            }
            model.sites.push_back(std::move(s));
        }
        model.cleanupReport = e.pluginCleanupReport_;
        for (const cascade::core::SupersededPlugin& o : e.pluginOldCopies_) {
            model.oldCopies.push_back({o.name, o.version, o.file, o.keptVersion});
        }
        // NOT "the catalogue is empty": the rows are there, kept or read.
        model.haveCatalogue = !e.catalog_.empty();
        model.sourceStatus = e.catalogStatus_;
        model.sourceError = a.statusLines_.catalogError;
        model.refreshFailed = e.catalogueRefreshFailed_;
        model.catalogueReadTime = e.catalogueReadTime_;
        model.catalogueFromCache = e.catalogueFromCache_;
        model.busy = e.catalogPending_ || e.installPending_;
        // (A picture is Pending only while it is queued or in flight - requestScreenshots and pumpScreenshots keep that - so master's "queue not
        // empty or one on the worker" is also "some picture Pending"; a test that SETS a picture's state, as A does, sets the second only.)
        model.pictureBusy = e.screenshotPending_ || !e.screenshotQueue_.empty();
        for (const auto& [sha, ps] : e.pictures_) {
            if (ps.state == Engine::PictureState::Pending) { model.pictureBusy = true; }
        }
        model.progress = e.pluginRepo_.progress();
        model.busyId = e.installPending_ ? e.installBusyId_ : std::string();
        if (e.installPending_) {
            model.busyLabel = cascade::core::formatText(cascade::i18n::tr("downloading %s"), e.installBusyName_.c_str());
        } else {
            model.busyLabel = e.catalogPending_ ? std::string(cascade::i18n::tr("fetching the catalogue")) : std::string();
        }
        model.resultReport = e.installReport_;
        model.resultError = e.installError_;
        model.resultId = e.installResultId_;
        model.addAllRunning = e.addAllRun_.active;
        model.addAllProgress = trueAddAllLine(a);
        model.addAllSummary = e.addAllSummary_;
        model.addAllFailed = e.addAllFailed_;
        const std::vector<cascade::core::PluginUpdate> updates = e.plannedPluginUpdates();
        model.modules.reserve(e.catalog_.size());
        for (int i = 0; i < static_cast<int>(e.catalog_.size()); ++i) {
            StoreModule sm;
            masterFillModule(a, i, updates, sm);
            model.modules.push_back(std::move(sm));
        }
        return model;
    }

    // WHAT A TEST MAY SET ON THE ENGINE that no hermetic world makes (a picture's state, the regional list's say-so, an old copy a scan found):
    // set where the engine keeps it, with the counter the lists' fingerprint hashes moved, on the pass.
    static void setPicture(AppWindow& a, const std::string& sha, int state, const std::string& path, const std::string& reason) {
        doOnPass(a, [&] {
            Engine& e = th::engine(a);
            Engine::PictureStatus ps;
            ps.state = state == 1 ? Engine::PictureState::Pending : state == 2 ? Engine::PictureState::Ready : Engine::PictureState::Failed;
            ps.path = path;
            ps.reason = reason;
            e.pictures_[sha] = ps;
            ++e.picturesSeq_;
        });
    }
    static void setRegional(AppWindow& a, const std::string& id, bool regional) {
        doOnPass(a, [&] {
            Engine& e = th::engine(a);
            for (cascade::core::PluginCatalogEntry& ce : e.catalog_) {
                if (ce.id == id) { ce.regional = regional; }
            }
            ++e.catalogSeq_;
        });
    }
    static void setOldCopies(AppWindow& a, const std::vector<cascade::core::SupersededPlugin>& v, const std::string& report) {
        doOnPass(a, [&] {
            Engine& e = th::engine(a);
            e.pluginOldCopies_ = v;
            e.pluginCleanupReport_ = report;
        });
    }
    static void setCatalogueFlags(AppWindow& a, bool fromCache, bool refreshFailed, std::int64_t readTime) {
        doOnPass(a, [&] {
            Engine& e = th::engine(a);
            e.catalogueFromCache_ = fromCache;
            e.catalogueRefreshFailed_ = refreshFailed;
            e.catalogueReadTime_ = readTime;
        });
    }
    static void setResultKey(AppWindow& a, const std::string& key) {
        doOnPass(a, [&] { th::engine(a).installResultId_ = key; });
    }
    static bool screenshotPending(AppWindow& a) { return onPass<bool>(a, [&] { return th::engine(a).screenshotPending_; }); }
    static bool readThisSession(AppWindow& a) { return onPass<bool>(a, [&] { return th::engine(a).catalogueReadThisSession(); }); }
    static std::size_t siteCount(AppWindow& a) { return onPass<std::size_t>(a, [&] { return th::engine(a).pluginSites_.size(); }); }

    // "installing P of N: name" with P the module in flight (1-based), from the engine's own run: what the table
    // serves and the window letters. MASTER lettered min(next + 1, total) here. (Both read the engine's run:
    // called on the pass, or with the pass parked at the publish being compared.)
    static std::string trueAddAllLine(AppWindow& a) {
        const auto& run = th::engine(a).addAllRun_;
        if (!run.active) { return {}; }
        const std::size_t pos = std::clamp<std::size_t>(run.next, 1u, run.total);
        return storeAddAllLine(pos, run.total, run.currentName);
    }
    static std::string oldAddAllLine(AppWindow& a) {
        const auto& run = th::engine(a).addAllRun_;
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
    std::filesystem::path index, indexRun, indexB, indexC, indexInstalled, indexNewer, site;
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
        // The install records: the stage-1 module at 1.0.0 and the group-9 one at 9.1.0 (the version the module itself declares: the scan wins where it and the record both know a module), each with the digest of the
        // file that is really there (an honest record: no integrity finding to confuse the facts).
        for (int which = 0; which < 2; ++which) {
            cc::PluginCatalogEntry rec;
            rec.id = which == 0 ? kStageId : kNineId;
            rec.name = which == 0 ? "Stage One Fixture Plugin With A Deliberately Long Display Name So The Retirement Sentence Passes The Cut"
                                  : "Group Nine Fixture";
            rec.version = which == 0 ? "1.0.0" : "9.1.0";
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
            e.category = "aircraft";
            e.experimental = true;
            e.whatsNew = "1.0.0: the first release.";
            e.published = "2026-10-07";
            e.capabilities = {"CASCADE_CAP_DECODER"};
            e.shots = {IdxShot{hex64('1'), "Picture 1"}, IdxShot{hex64('2'), "Picture 2"}, IdxShot{hex64('3'), "Picture 3"}};
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
            IdxEntry e;  // installed at 9.1.0, loaded: a host record to join, and an update is planned
            e.id = kNineId;
            e.name = "Group Nine Fixture";
            e.version = "9.2.0";
            e.author = "FoxSDR tests (group nine)";
            e.licence = "MIT";
            // ...whose build for this machine is a file of ANOTHER NAME than the module on disk: the update's file has never been there
            e.platforms = {hostBuild(moduleFile("group_nine_next"), '7', 2048)};
            v.push_back(e);
        }
        index = g_scratch / "store_index_group10.json";
        writeText(index, indexJson(v));
        // A DIFFERENT catalogue with the same shape (one entry reworded), for "the catalogue was replaced".
        v[1].legal = std::string(300, 'M');
        indexB = g_scratch / "store_index_group10_b.json";
        writeText(indexB, indexJson(v));
        // TWO RETIRED MODULES: the same shape with the group-9 module's floor above its installed 9.1.0 too.
        v[1].legal = std::string(300, 'L');
        v[6].floor = "9.2.0";
        indexC = g_scratch / "store_index_group10_c.json";
        writeText(indexC, indexJson(v));
        // THE GROUP-NINE ENTRY AT THE VERSION INSTALLED, and BELOW it (a newer copy is installed): by id and version, not by the file's name.
        v[6].floor.clear();
        v[6].version = "9.1.0";
        indexInstalled = g_scratch / "store_index_group10_installed.json";
        writeText(indexInstalled, indexJson(v));
        v[6].version = "8.0.0";
        indexNewer = g_scratch / "store_index_group10_newer.json";
        writeText(indexNewer, indexJson(v));
        // A PLUGIN SITE'S INDEX: two plugins of its own and one whose id an official plugin already holds (hidden, and counted).
        {
            std::vector<IdxEntry> s;
            for (const char* word : {"radio", "extra"}) {
                IdxEntry e;
                e.id = std::string("site-") + word;
                e.name = std::string("Site ") + word;
                e.version = "0.1.0";
                e.licence = "MIT";
                e.summary = std::string("A third party's ") + word + ".";
                e.platforms = {hostBuild(moduleFile(std::string("site_") + word), '9', 2000)};
                s.push_back(e);
            }
            IdxEntry clash;
            clash.id = "alpha-good";
            clash.name = "Impostor Alpha";
            clash.version = "9.9.9";
            clash.licence = "MIT";
            clash.platforms = {hostBuild(moduleFile("impostor"), '9', 2000)};
            s.push_back(clash);
            site = g_scratch / "store_site_group10.json";
            writeText(site, indexJson(s));
        }
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
    th::TestWindow app;
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
    // THE LIST STAYS WHILE A FETCH IS IN FLIGHT (0.99.72), so "the entries are there" says nothing about a second read: the commands are
    // applied first (the next pass), and then the fetch is waited for.
    pass(a);
    pass(a);
    const bool landed =
        passUntil(a, [&] { return Access::catalogSize(a) == entries && !Access::catalogPending(a) && Access::engineUrl(a) == path.string(); });
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

// The same module, as master's window built it from the catalogue's own members and as the table gave it.
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
    check(got.id == want.id, "id (the catalogue key)");
    check(g.haveDescriptor == w.haveDescriptor, "haveDescriptor");
    // THE NAME IS CUT AT THE LIST ITEM'S 95 BYTES (API 0.3 8.9): master drew a display name whole. No entry of the live catalogue comes
    // near it (the longest, "406 MHz Distress Beacon Decoder (EPIRB / ELT / PLB)", is 51 characters); this world has one that does, on
    // purpose, to hold the cut where it is.
    check(g.name == w.name.substr(0, 95), "name (cut at 95 bytes)");
    check(g.version == w.version, "version");
    check(g.maker == w.maker, "maker");
    check(g.licence == w.licence, "licence");
    check(g.blurb == w.blurb, "blurb");
    check(g.summary == w.summary, "summary");
    check(g.homepage == w.homepage, "homepage");
    check(g.legalNotice == w.legalNotice, "legalNotice (a site's plugin always carries one)");
    check(g.originNote == w.originNote, "originNote");
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
    check(g.category == w.category, "category");
    check(g.experimental == w.experimental, "experimental");
    check(g.whatsNew == w.whatsNew, "whatsNew");
    check(g.published == w.published, "published");
    check(g.sha256 == w.sha256, "sha256");
    check(got.thirdParty == want.thirdParty, "thirdParty");
    check(got.siteHost == want.siteHost, "siteHost");
    check(got.installableHere == want.installableHere, "installableHere");
    check(got.haveBuildHere == want.haveBuildHere, "haveBuildHere");
    check(got.buildsWindows == want.buildsWindows, "buildsWindows");
    check(got.buildsLinux == want.buildsLinux, "buildsLinux");
    check(got.install == want.install, "install (by id and version)");
    check(got.installedVersion == want.installedVersion, "installedVersion");
    check(got.blockedReason == want.blockedReason, "blockedReason");
    check(got.blockedReasonIfAcknowledged == want.blockedReasonIfAcknowledged, "blockedReasonIfAcknowledged");
    check(got.updateToVersion == want.updateToVersion, "updateToVersion");
    check(got.updateReason == want.updateReason, "updateReason");
    check(got.updateBlockedReason == want.updateBlockedReason, "updateBlockedReason");
    // THE PICTURES: the digest, the caption and how far each has got (the advisory width and height are not in the table).
    check(got.pictures.size() == want.pictures.size(), "pictures (how many)");
    for (std::size_t i = 0; i < got.pictures.size() && i < want.pictures.size(); ++i) {
        check(got.pictures[i].sha256 == want.pictures[i].sha256, "picture sha256");
        check(got.pictures[i].caption == want.pictures[i].caption, "picture caption");
        check(got.pictures[i].state == want.pictures[i].state, "picture state");
        check(got.pictures[i].path == want.pictures[i].path, "picture path");
        check(got.pictures[i].reason == want.pictures[i].reason, "picture reason");
    }
    return ok;
}

// The whole model against master's, module by module and the fields beside the modules. Returns the modules compared. (The ADD ALL line
// is compared with the TRUE position: the one difference of that line, recorded.)
int sameModel(AppWindow& a, const char* when) {
    Access::look(a);
    const PluginStoreModel want = Access::masterModel(a);
    const PluginStoreModel& got = Access::lists(a).model;
    CHECK_EQ(got.modules.size(), want.modules.size());
    const auto field = [&](bool same, const char* what) {
        if (!same) { std::printf("      %s: model.%s differs\n", when, what); }
        CHECK(same);
    };
    field(got.sourceUrl == want.sourceUrl, "sourceUrl");
    field(got.hostPlatform == want.hostPlatform, "hostPlatform");
    field(got.maxSites == want.maxSites, "maxSites");
    field(got.haveCatalogue == want.haveCatalogue, "haveCatalogue");
    field(got.sourceStatus == want.sourceStatus, "sourceStatus");
    field(got.sourceError == want.sourceError, "sourceError");
    field(got.refreshFailed == want.refreshFailed, "refreshFailed");
    field(got.catalogueReadTime == want.catalogueReadTime, "catalogueReadTime");
    field(got.catalogueFromCache == want.catalogueFromCache, "catalogueFromCache");
    field(got.pictureBusy == want.pictureBusy, "pictureBusy");
    field(got.busy == want.busy, "busy");
    // (the bar is drawn only while something is busy; the table reads the figure only then and 0 otherwise, where master read the
    // repo's last figure whatever it was doing)
    field(!want.busy || got.progress == want.progress, "progress");
    field(got.busyLabel == want.busyLabel, "busyLabel");
    field(got.busyId == want.busyId, "busyId");
    field(got.resultReport == want.resultReport, "resultReport");
    field(got.resultError == want.resultError, "resultError");
    field(got.resultId == want.resultId, "resultId");
    field(got.addAllRunning == want.addAllRunning, "addAllRunning");
    field(got.addAllProgress == want.addAllProgress, "addAllProgress");
    field(got.addAllSummary == want.addAllSummary, "addAllSummary");
    field(got.addAllFailed == want.addAllFailed, "addAllFailed");
    field(got.cleanupReport == want.cleanupReport, "cleanupReport");
    field(got.sites.size() == want.sites.size(), "sites (how many)");
    for (std::size_t i = 0; i < got.sites.size() && i < want.sites.size(); ++i) {
        const cascade::gui::StoreSite& g = got.sites[i];
        const cascade::gui::StoreSite& w = want.sites[i];
        field(g.url == w.url && g.label == w.label && g.display == w.display, "a site's address, label and display line");
        field(g.state == w.state, "a site's state");
        field(g.plugins == w.plugins, "a site's plugin count");
        field(g.hiddenSameId == w.hiddenSameId && g.hiddenOffHost == w.hiddenOffHost, "a site's hidden counts");
        field(g.error == w.error, "a site's error");
    }
    field(got.oldCopies.size() == want.oldCopies.size(), "oldCopies (how many)");
    for (std::size_t i = 0; i < got.oldCopies.size() && i < want.oldCopies.size(); ++i) {
        const cascade::gui::StoreOldCopy& g = got.oldCopies[i];
        const cascade::gui::StoreOldCopy& w = want.oldCopies[i];
        field(g.name == w.name && g.version == w.version && g.file == w.file && g.keptVersion == w.keptVersion, "an old copy");
    }
    int compared = 0;
    for (std::size_t i = 0; i < got.modules.size() && i < want.modules.size(); ++i) {
        if (sameModule(got.modules[i], want.modules[i], when)) { ++compared; }
    }
    return compared;
}

// --- A -----------------------------------------------------------------------

void testModelEqualsMasters() {
    std::printf("  A: the model the window draws from the table, equal to the one master's 0.99.72 window built from the catalogue's members\n");
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
    CHECK(!Access::readThisSession(app));

    CHECK(fetchCatalogue(app, f.w.index, 7));
    state("fetched, no page open");
    CHECK_EQ(Access::lists(app).model.modules.size(), 7u);
    CHECK_EQ(Access::lists(app).model.sourceUrl, f.w.index.string());
    CHECK_EQ(Access::lists(app).model.sourceStatus, std::string("7 plugins in the catalogue"));
    CHECK(Access::readThisSession(app) && Access::lists(app, false).readThisSession);
    CHECK(!Access::lists(app).model.catalogueFromCache && Access::lists(app).model.catalogueReadTime > 0);

    // The tick belongs to ONE page, the one that is open: asked of every other card as unacknowledged.
    Access::deck(app).pageId = "beta.legal";
    Access::deck(app).legalAck = false;
    state("the notice's page open, not ticked");
    Access::deck(app).legalAck = true;
    state("the notice's page open and ticked");
    CHECK(mod(app, "beta.legal")->blockedReason.empty());
    Access::deck(app).pageId = "alpha-good";
    state("another page open with the tick still given (it is for the first page only)");
    CHECK_EQ(mod(app, "beta.legal")->blockedReason, std::string(cascade::gui::kLegalNoticeFirstReason));
    Access::deck(app).legalAck = false;
    Access::deck(app).pageId.clear();

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

    // THE SHOP-WINDOW FIELDS and the pictures of the first entry (alpha: category, experimental, what's new, published, two declared
    // capabilities, three pictures), each in a state: queued, on disk, failed.
    {
        const StoreModule* alpha = mod(app, "alpha-good");
        CHECK(alpha != nullptr && alpha->pictures.size() == 3u);
        if (alpha != nullptr && alpha->pictures.size() == 3u) {
            Access::setPicture(app, alpha->pictures[0].sha256, 1, "", "");
            Access::setPicture(app, alpha->pictures[1].sha256, 2, (g_scratch / "a1.png").string(), "");
            Access::setPicture(app, alpha->pictures[2].sha256, 3, "", "the picture did not match its sha256");
            pass(app);
            pass(app);
            state("three pictures: queued, ready, failed");
            CHECK(Access::lists(app).pictureBusy && Access::lists(app).model.pictureBusy);
            const StoreModule* a2 = mod(app, "alpha-good");
            CHECK(a2 != nullptr && a2->pictures.size() == 3u);
            if (a2 != nullptr && a2->pictures.size() == 3u) {
                CHECK(a2->pictures[0].state == cascade::gui::PictureState::Pending);
                CHECK(a2->pictures[1].state == cascade::gui::PictureState::Ready && a2->pictures[1].path == (g_scratch / "a1.png").string());
                CHECK(a2->pictures[2].state == cascade::gui::PictureState::Failed &&
                      a2->pictures[2].reason == "the picture did not match its sha256");
            }
        }
    }
    // THE REGIONAL LIST'S SAY-SO: "Offered by foxsdr.com in some countries only" is the card's origin line.
    Access::setRegional(app, "epsilon-nolicence", true);
    pass(app);
    pass(app);
    state("a plugin the regional list offered");
    CHECK_EQ(mod(app, "epsilon-nolicence")->plate.originNote, std::string("Offered by foxsdr.com in some countries only."));
    // THE OLD COPIES an update left, and what the last clean-up did.
    {
        cascade::core::SupersededPlugin o;
        o.file = "stage_old.dll";
        o.name = "Stage One Old";
        o.version = "0.9.0";
        o.keptFile = kStage1;
        o.keptVersion = "1.0.0";
        Access::setOldCopies(app, {o}, "Removed the old version stage_old.dll.");
        pass(app);
        pass(app);
        state("an old copy and the clean-up's report");
        CHECK_EQ(Access::lists(app).model.oldCopies.size(), 1u);
        CHECK_EQ(Access::lists(app).model.cleanupReport, std::string("Removed the old version stage_old.dll."));
        Access::setOldCopies(app, {}, "");
        pass(app);
        pass(app);
    }
    // The kept copy's freshness, and a refresh that failed beside it.
    Access::setCatalogueFlags(app, true, false, 1790000000);
    pass(app);
    pass(app);
    state("a kept copy");
    CHECK(Access::lists(app).model.catalogueFromCache && Access::lists(app).model.catalogueReadTime == 1790000000);
    CHECK(!Access::lists(app, false).readThisSession);  // a kept copy is not a read: the rail says IDLE, the first open reads
    Access::setCatalogueFlags(app, true, true, 1790000000);
    pass(app);
    pass(app);
    state("a kept copy beside a refresh that failed");
    CHECK(Access::lists(app).model.refreshFailed);
    Access::setCatalogueFlags(app, false, false, Access::lists(app).model.catalogueReadTime);
    pass(app);
    pass(app);

    // A SITE THE USER ADDED (a local index: the form of an address that lets a test use files): its plugin is a card with THIRD PARTY, its
    // notice is composed, the id it shares with an official plugin is hidden and counted.
    Access::submit(app, cmd::makeText(FOXAPP_OP_STORE_SITE, f.w.site.string(), 0, 1));
    CHECK(passUntil(app, [&] { return Access::siteCount(app) == 1u && !Access::catalogPending(app) && Access::catalogSize(app) == 9u; }));
    pass(app);
    pass(app);
    state("a site added, read");
    {
        const StoreModule* sp = mod(app, "site-radio@" + f.w.site.filename().string());
        CHECK(sp != nullptr);
        if (sp != nullptr) {
            CHECK(sp->thirdParty && sp->siteHost == f.w.site.filename().string());
            CHECK(sp->plate.legalNotice.rfind("This plugin comes from ", 0) == 0);
            CHECK(sp->plate.originNote.rfind("From a site you added: ", 0) == 0);
            CHECK_EQ(sp->blockedReason, std::string(cascade::gui::kLegalNoticeFirstReason));  // always asks for the tick
        }
        const auto& sites = Access::lists(app).model.sites;
        CHECK_EQ(sites.size(), 1u);
        if (sites.size() == 1u) {
            CHECK(sites[0].state == cascade::gui::StoreSite::State::Read && sites[0].plugins == 2 && sites[0].hiddenSameId == 1);
        }
    }
    Access::deck(app).pageId = "site-radio@" + f.w.site.filename().string();
    state("the site plugin's page open, not ticked");
    Access::deck(app).legalAck = true;
    state("the site plugin's page open and ticked");
    CHECK(mod(app, "site-radio@" + f.w.site.filename().string())->blockedReason.empty());
    Access::deck(app) = cascade::gui::PluginStoreDeck{};
    Access::submit(app, cmd::makeText(FOXAPP_OP_STORE_SITE, f.w.site.string(), 1, 0));
    CHECK(passUntil(app, [&] { return Access::siteCount(app) == 0u && Access::catalogSize(app) == 7u; }));
    pass(app);
    pass(app);
    state("the site removed");
    CHECK(Access::lists(app).model.sites.empty());

    // A DOWNLOAD in flight (the worker's slot is a promise the test keeps), a quarter done, and what it leaves.
    Access::beginTransfer(app, "Alpha Decoder", "alpha-good");
    Access::setProgress(app, 0.25f);
    pass(app);
    state("a download in flight");
    CHECK(Access::lists(app).model.busy);
    CHECK_EQ(Access::lists(app).model.busyLabel, std::string("downloading Alpha Decoder"));
    CHECK_EQ(Access::lists(app).model.busyId, std::string("alpha-good"));
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
    Access::setResultKey(app, "alpha-good");
    pass(app);
    pass(app);
    state("the outcome belongs to a plugin");
    CHECK_EQ(Access::lists(app).model.resultId, std::string("alpha-good"));

    // A refetch that fails: THE LIST STAYS (0.99.72: a user offline for a moment was shown an empty store - master replaced the 0.99.42
    // rule on purpose, and so did the engine, stub campaign B3), the red line is the engine's, and the status line is cleared.
    Access::submit(app, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, f.w.index.string() + ".missing"));
    Access::submit(app, cmd::make(FOXAPI_OP_STORE_FETCH));
    CHECK(passUntil(app, [&] { return !Access::catalogPending(app) && !Access::statusLines(app).catalogError.empty(); }));
    pass(app);
    state("a refetch that failed");
    CHECK(Access::lists(app).model.haveCatalogue && Access::lists(app).model.modules.size() == 7u);
    CHECK(!Access::lists(app).model.sourceError.empty());
    CHECK(Access::lists(app).model.refreshFailed);
    CHECK(Access::lists(app).model.sourceStatus.empty());
    std::printf("      %d modules held equal to master's, field for field, in %d states\n", compared, states);
    CHECK(compared >= 7 * 14);
}

// THE FOUR WAYS AN ENTRY STANDS AGAINST WHAT IS INSTALLED, by id and version (0.99.72), each with a catalogue of its own: the module the host
// loaded is the group-nine fixture at 9.1.0, and the entry for it says 9.2.0 (an update: the file the entry would write is not the one on disk),
// 9.1.0 (installed), 8.0.0 (a newer copy is installed) or a plugin nobody installed.
void testInstallStates() {
    std::printf("  A2: how an entry stands against what is installed - by id and version, never by the file's name\n");
    Env f;
    AppWindow& app = f.app;
    const struct {
        const std::filesystem::path* index;
        cascade::gui::StoreInstallKind kind;
        const char* word;
        const char* version;
    } cases[] = {
        {&f.w.index, cascade::gui::StoreInstallKind::UpdateAvailable, "an update", "9.1.0"},
        {&f.w.indexInstalled, cascade::gui::StoreInstallKind::Installed, "installed", "9.1.0"},
        {&f.w.indexNewer, cascade::gui::StoreInstallKind::NewerInstalled, "a newer copy installed", "9.1.0"},
    };
    for (const auto& c : cases) {
        CHECK(fetchCatalogue(app, *c.index, 7));
        pass(app);
        const StoreModule* nine = mod(app, kNineId);
        CHECK(nine != nullptr);
        if (nine == nullptr) { continue; }
        std::printf("      %-24s -> install kind %d, installed %s, fitted %d, host file %s, update to \"%s\"\n", c.word,
                    static_cast<int>(nine->install), nine->installedVersion.c_str(), nine->plate.fitted ? 1 : 0, nine->plate.fileName.c_str(),
                    nine->updateToVersion.c_str());
        CHECK(nine->install == c.kind);
        CHECK_EQ(nine->installedVersion, std::string(c.version));
        // FITTED whatever the file's name: this is what the 0.99.42 rule got wrong for an update (the new file had never been on disk).
        CHECK(nine->plate.fitted);
        // ...and the host's record is the module that IS on disk, found by the newest installed copy, not by the name the entry would write.
        CHECK_EQ(nine->plate.fileName, kGroup9);
        CHECK(nine->plate.loaded);
        CHECK_EQ(nine->updateToVersion.empty(), c.kind != cascade::gui::StoreInstallKind::UpdateAvailable);
        CHECK_EQ(nine->blockedReason, std::string("already installed"));
        sameModel(app, c.word);
    }
    // A plugin nobody installed: none, and the host has no record.
    const StoreModule* alpha = mod(app, "alpha-good");
    CHECK(alpha != nullptr && alpha->install == cascade::gui::StoreInstallKind::NotInstalled && alpha->installedVersion.empty());
}

// --- B -----------------------------------------------------------------------

void testWhatTheWindowIsTold() {
    std::printf("  B: the entries from the index - ABI, size, builds, the shop-window fields, the install state, the gate, the plan, the host's record, the retired row\n");
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
    CHECK(alpha->updateToVersion.empty() && alpha->updateReason.empty() && alpha->updateBlockedReason.empty());
    CHECK(!alpha->plate.loaded && alpha->plate.fileName.empty());
    // THE SHOP-WINDOW FIELDS, as the index said them: the category's id, the experimental mark, the paragraph about THIS version, the date,
    // this host's digest, the declared capabilities (the bit of the one name this build knows).
    CHECK_EQ(alpha->plate.category, std::string("aircraft"));
    CHECK(alpha->plate.experimental);
    CHECK_EQ(alpha->plate.whatsNew, std::string("1.0.0: the first release."));
    CHECK_EQ(alpha->plate.published, std::string("2026-10-07"));
    CHECK_EQ(alpha->plate.sha256, hex64('a'));
    CHECK(alpha->plate.haveCapabilities && alpha->plate.capabilities == static_cast<std::uint32_t>(CASCADE_CAP_DECODER));
    CHECK(alpha->haveBuildHere);
    CHECK_EQ(alpha->buildsWindows, std::string(cc::PluginRepo::hostOs()) == "windows");  // the builds it publishes, as operating systems
    CHECK_EQ(alpha->buildsLinux, std::string(cc::PluginRepo::hostOs()) == "linux");
    CHECK_EQ(alpha->install, cascade::gui::StoreInstallKind::NotInstalled);
    CHECK(!alpha->thirdParty && alpha->siteHost.empty() && alpha->plate.originNote.empty());
    // THREE PICTURES, named by digest and caption, none asked for yet (the grid never asks): the table lists no picture until one is wanted.
    CHECK_EQ(alpha->pictures.size(), 3u);
    for (std::size_t i = 0; i < alpha->pictures.size(); ++i) {
        CHECK_EQ(alpha->pictures[i].sha256, hex64(static_cast<char>('1' + i)));
        CHECK_EQ(alpha->pictures[i].caption, "Picture " + std::to_string(i + 1));
        CHECK(alpha->pictures[i].state == cascade::gui::PictureState::None);
    }

    // The notice: its text whole, a floor, no maker, no size told - and the gate with and without the tick.
    CHECK_EQ(beta->plate.legalNotice, std::string(300, 'L'));
    CHECK_EQ(beta->plate.retirementFloor, std::string("0.9.0"));
    CHECK(beta->plate.maker.empty());
    CHECK(!beta->plate.haveSizeBytes && beta->plate.sizeBytes == 0u);  // "not told", never an empty file
    CHECK(beta->installableHere);
    CHECK(!beta->plate.haveCapabilities);  // the index said nothing: "not declared", not "declares none"
    CHECK(beta->plate.category.empty() && !beta->plate.experimental && beta->plate.whatsNew.empty() && beta->plate.published.empty());
    CHECK_EQ(beta->blockedReason, std::string(cascade::gui::kLegalNoticeFirstReason));  // not ticked: the engine's own sentence
    CHECK(beta->blockedReasonIfAcknowledged.empty());
    CHECK_EQ(Access::blockedReason(app, indexOf(app, "beta.legal"), false), beta->blockedReason);  // ...which IS the engine's
    Access::deck(app).pageId = "beta.legal";
    Access::deck(app).legalAck = true;
    CHECK(mod(app, "beta.legal")->blockedReason.empty());
    Access::deck(app).legalAck = false;
    Access::deck(app).pageId.clear();

    // No build at all: no platforms, not installable, and the gate says so.
    CHECK(gamma->plate.platforms.empty() && !gamma->installableHere && !gamma->plate.haveSizeBytes && !gamma->haveBuildHere);
    CHECK(!gamma->buildsWindows && !gamma->buildsLinux);
    CHECK_EQ(gamma->blockedReason, cascade::gui::pluginNoBuildReason(kHostPair));
    // Another ABI: both numbers are stated, not installable here though a build is, and the reason carries both.
    CHECK(delta->plate.haveAbi && delta->plate.abiVersion == abi + 1u && delta->plate.hostAbiVersion == abi);
    CHECK(!delta->installableHere && delta->plate.haveSizeBytes && delta->plate.sizeBytes == 777u && delta->haveBuildHere);
    CHECK_EQ(delta->blockedReason, cascade::gui::pluginAbiMismatchReason(abi + 1u, abi));
    // No licence: installable by the columns, and the gate refuses it.
    CHECK(eps->installableHere && eps->plate.licence.empty());
    CHECK_EQ(eps->blockedReason, std::string("the catalogue entry declares no licence"));

    // The retired module: INSTALLED (by the records) and the host holds no record; its update is planned.
    CHECK(stage->plate.fitted && !stage->plate.loaded && stage->plate.fileName.empty() && !stage->plate.haveCapabilities);
    CHECK(!stage->plate.running && !stage->plate.haveTuneGrant);
    CHECK_EQ(stage->install, cascade::gui::StoreInstallKind::UpdateAvailable);
    CHECK_EQ(stage->installedVersion, std::string("1.0.0"));
    CHECK_EQ(stage->updateToVersion, std::string("2.0.0"));
    CHECK_EQ(stage->updateReason, std::string("version 2.0.0 replaces the installed 1.0.0"));
    CHECK_EQ(stage->plate.retirementFloor, std::string("2.0.0"));
    CHECK_EQ(stage->blockedReason, std::string("already installed"));
    // The fitted one: BY ID AND VERSION (an update: the entry's own file is a different name from the module on disk), the host's record found
    // by the newest installed copy, with its caps, its run state and a tune key.
    CHECK(nine->plate.fitted && nine->plate.loaded && nine->plate.running);
    CHECK_EQ(nine->install, cascade::gui::StoreInstallKind::UpdateAvailable);
    CHECK_EQ(nine->installedVersion, std::string("9.1.0"));
    CHECK_EQ(nine->plate.fileName, kGroup9);
    CHECK(nine->plate.haveCapabilities && (nine->plate.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u);
    CHECK(nine->plate.haveTuneGrant && !nine->plate.tuneGranted);
    CHECK(nine->plate.refusalReason.empty());
    CHECK_EQ(nine->updateToVersion, std::string("9.2.0"));
    CHECK_EQ(nine->updateReason, std::string("version 9.2.0 replaces the installed 9.1.0"));

    // The rail tier: counts, the retired row with its whole sentence, id and plan.
    const cascade::gui::StoreListCache& rail = Access::lists(app, false);
    CHECK(rail.haveCatalogue && rail.readThisSession && rail.updates == 2u && !rail.transfer && !rail.addAll);
    CHECK_EQ(rail.refused, 1u);  // the file the host did not load
    CHECK_EQ(rail.blocked.size(), 1u);
    if (rail.blocked.size() == 1u) {
        const auto& b = rail.blocked[0];
        CHECK_EQ(b.file, kStage1);
        CHECK_EQ(b.id, kStageId);
        CHECK(b.hasPlan);
        // The sentence master composed at draw, now the engine's fact: the same function on the same record in the language in force, and
        // more than a list item holds.
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
    std::printf("  C: one rebuild when listSeq moves, the full tier only while drawn, the live progress, a settled frame reads and allocates nothing\n");
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

    // THE READS OF A SETTLED FRAME, counted at the table's own door (the window's engine API pointer is a copy of the table whose read_list and
    // get_setting count and forward): 200 frames of the window drawn with the rail beside it, nothing in flight, read NOTHING - not a list item,
    // not a fact. (The state is read once a frame, by the engine's own call; that is not a list or a fact.)
    Access::countReads(app, true);
    for (int i = 0; i < 200; ++i) { frame(app, true, true); }
    long listReads = 0;
    long factReads = 0;
    Access::reads(listReads, factReads);
    std::printf("      200 settled frames, store and rail drawn: %ld list read(s), %ld fact read(s)\n", listReads, factReads);
    if (listReads != 0 || factReads != 0) { Access::printReadsByList(); }
    CHECK_EQ(listReads, 0L);
    CHECK_EQ(factReads, 0L);

    // A settled frame: 1000 reads of an unchanged state allocate nothing - both tiers, the status lines, the rows' reasons (the model is set
    // on every call; a string given what its room holds allocates nothing) - and read nothing.
    Access::resetReads();
    long before = t_allocs;
    std::size_t sink = 0;
    for (int i = 0; i < 1000; ++i) {
        const cascade::gui::StoreListCache& l = Access::lists(app, true);
        sink += l.model.modules.size() + l.model.sourceUrl.size() + l.model.sourceStatus.size() + l.blocked.size();
        for (const StoreModule& x : l.model.modules) { sink += x.blockedReason.size() + x.pictures.size(); }
    }
    long spent = t_allocs - before;
    Access::reads(listReads, factReads);
    std::printf("      1000 full reads of an unchanged state: %ld allocation(s), %ld list read(s), %ld fact read(s) (%zu)\n", spent, listReads,
                factReads, sink);
    CHECK_EQ(spent, 0L);
    CHECK_EQ(listReads + factReads, 0L);
    before = t_allocs;
    for (int i = 0; i < 1000; ++i) { sink += Access::lists(app, false).blocked.size(); }
    spent = t_allocs - before;
    std::printf("      1000 rail-tier reads (the window shut): %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);
    // ...with a page open and a deck that moves (the reason it depends on is set every call).
    Access::deck(app).pageId = "beta.legal";
    (void)Access::lists(app);
    before = t_allocs;
    Access::resetReads();
    for (int i = 0; i < 1000; ++i) {
        Access::deck(app).legalAck = (i % 2) == 0;
        sink += Access::lists(app).model.modules[static_cast<std::size_t>(indexOf(app, "beta.legal"))].blockedReason.size();
    }
    spent = t_allocs - before;
    Access::reads(listReads, factReads);
    std::printf("      1000 reads with the tick flipping on the open page: %ld allocation(s), %ld read(s)\n", spent, listReads + factReads);
    CHECK_EQ(spent, 0L);
    CHECK_EQ(listReads + factReads, 0L);
    Access::deck(app).legalAck = false;
    Access::deck(app).pageId.clear();

    // What master's window cost, every frame it was open, for the record: the same seven entries built from the catalogue's members.
    long masterAllocs = 0;
    const int frames = 100;
    for (int i = 0; i < frames; ++i) { sink += Access::masterModel(app, &masterAllocs).modules.size(); }
    const long masterPerFrame = masterAllocs / frames;
    std::printf("      master's model, built every frame from the catalogue's members: %ld allocations a frame\n", masterPerFrame);
    CHECK(masterPerFrame > 0);

    // THE LIVE PROGRESS: a transfer in flight is read once a frame, moves no list and allocates nothing.
    Access::beginTransfer(app, "Alpha Decoder", "alpha-good");
    Access::setProgress(app, 0.1f);
    pass(app);
    (void)Access::lists(app);
    const std::uint64_t seqT = Access::listSeq(app);
    const std::uint32_t fullT = c.refreshes;
    before = t_allocs;
    Access::resetReads();
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
    Access::reads(listReads, factReads);
    std::printf("      the progress moved 1000 times: %d reads saw a new figure, %ld allocation(s), %ld list read(s) (the live figure's own), %ld fact read(s), listSeq and the cache unmoved\n",
                moves, spent, listReads, factReads);
    CHECK_EQ(spent, 0L);
    CHECK_EQ(moves, 1000);
    CHECK_EQ(listReads, 1000L);  // ONE read a frame - STORE_ACTIVITY's first entry - and nothing else
    CHECK_EQ(factReads, 0L);
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
    Access::resetReads();
    (void)Access::lists(app);
    spent = t_allocs - before;
    Access::reads(listReads, factReads);
    CHECK_EQ(c.refreshes, r1 + 1u);
    CHECK_EQ(c.railRefreshes, q1 + 1u);
    const long rebuildListReads = listReads;
    const long rebuildFactReads = factReads;
    Access::resetReads();
    (void)Access::lists(app);
    Access::reads(listReads, factReads);
    CHECK_EQ(c.refreshes, r1 + 1u);  // read once however often it is asked
    CHECK_EQ(listReads + factReads, 0L);
    long inRefresh = 0;
    for (int i = 0; i < 100; ++i) {
        aChange(2 + i);
        Access::look(app);
        const long mark = t_allocs;
        (void)Access::lists(app);
        inRefresh += t_allocs - mark;
    }
    const std::size_t rows = Access::lists(app).model.modules.size();
    std::printf("      a rebuild (%zu entries, ~26 facts each, three pictures on one, the host's records, the retired row): %ld list read(s), %ld fact read(s), %ld allocation(s) the first time, %ld on average over 100 (all but a few inside the table's get_setting)\n",
                rows, rebuildListReads, rebuildFactReads, spent, inRefresh / 100);
    // ~26 facts an entry and the status of the store (about twenty more): bounded by the entries, never by the frames.
    CHECK(rebuildFactReads <= static_cast<long>(rows) * 30L + 40L);
    CHECK(spent <= kRebuildAllocBound);
    CHECK(inRefresh / 100 <= kRebuildAllocBound);

    // A PICTURE CHANGING STATE moves listSeq ONCE and rebuilds once: the window sees the new state at the next look, then settles again.
    {
        const StoreModule* alpha = mod(app, "alpha-good");
        CHECK(alpha != nullptr && alpha->pictures.size() == 3u);
        if (alpha != nullptr && alpha->pictures.size() == 3u) {
            const std::string sha = alpha->pictures[0].sha256;
            const std::uint64_t seqP = Access::listSeq(app);
            const std::uint32_t fullP = c.refreshes;
            Access::setPicture(app, sha, 1, "", "");
            Access::look(app);
            CHECK(Access::listSeq(app) > seqP);
            CHECK(Access::lists(app).pictureBusy);
            CHECK_EQ(c.refreshes, fullP + 1u);
            CHECK(mod(app, "alpha-good")->pictures[0].state == cascade::gui::PictureState::Pending);
            Access::resetReads();
            for (int i = 0; i < 50; ++i) { frame(app, true, true); }
            Access::reads(listReads, factReads);
            CHECK_EQ(c.refreshes, fullP + 1u);
            CHECK_EQ(listReads + factReads, 0L);
        }
    }

    // The window SHUT builds no model: the rail tier is read, the full tier is not, however the lists move.
    Access::countReads(app, false);
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

// THE COMMANDS A SURVEY'S CLICKS SENT, except the asks for a page's pictures (see wantsOf): opening a page is an ask, and a card has several
// places to click that open its page.
std::vector<std::string> commandsOf(const std::vector<Found>& v) {
    std::vector<std::string> out;
    for (const Found& f : v) {
        if (!f.command.empty() && f.command.rfind("APP_STORE_PICTURES_WANT", 0) != 0) { out.push_back(f.command); }
    }
    std::sort(out.begin(), out.end());
    return out;
}
// ...the pages the clicks opened: the keys each ask named, every one with the retry flag (ival[0] 1) that the edge of a page sends.
std::set<std::string> wantsOf(const std::vector<Found>& v) {
    std::set<std::string> out;
    const std::string head = "APP_STORE_PICTURES_WANT ival=1,0 text=[";
    for (const Found& f : v) {
        if (f.command.rfind("APP_STORE_PICTURES_WANT", 0) != 0) { continue; }
        const bool formed = f.command.rfind(head, 0) == 0 && f.command.back() == ']';
        CHECK(formed);
        if (formed) { out.insert(f.command.substr(head.size(), f.command.size() - head.size() - 1)); }
    }
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
    std::printf("  D1: the first open reads the catalogue once - and never over one that was read this session; a kept copy is not a read\n");
    // A catalogue another path read THIS SESSION (the web remote's, a bounded run's hook: here the engine's own fetch) before the window was
    // ever opened: opening it asks the table before it asks whether to fetch, so it sends nothing.
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
    // Nothing asked yet (the index is a local file: no network): the first frame it is open sends one FETCH, and the table then shows it in
    // flight, so no more follow; it lands and the rows are there.
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
        std::printf("      read this session: no FETCH on opening; asked for the first time: one FETCH, then the 7 rows\n");
    }
    // THE KEPT COPY (0.99.72): the catalogue the last session read is on disk and fills the store at start-up - the rows are there, kept - but
    // it is NOT a read: the first open still asks once, and the kept copy is what shows until it answers. (Master asked "is the list empty" until
    // 0.99.72; asking that here would never read the catalogue for anyone who had.)
    {
        World w;
        std::filesystem::copy_file(w.index, g_plugins / "catalogue.json", std::filesystem::copy_options::overwrite_existing);
        writeText(g_plugins / "catalogue.json.time", "1790000000\n");
        th::TestWindow app;
        Access::look(app);
        Access::look(app);
        Access::submit(app, cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, w.index.string()));  // (a local index to read: no network)
        pass(app);
        pass(app);
        const cascade::gui::StoreListCache& rail = Access::lists(app, false);
        CHECK(rail.haveCatalogue);                // the rows are there...
        CHECK(!rail.readThisSession);             // ...and nobody has looked: the rail says IDLE, not OK
        Access::Sent sent;
        Access::tap(app, &sent);
        Access::setOpen(app, true);
        frame(app);
        CHECK(Access::lists(app).model.catalogueFromCache);   // what shows first is the kept copy, and the table says so
        CHECK_EQ(Access::lists(app).model.catalogueReadTime, 1790000000);
        CHECK_EQ(Access::lists(app).model.modules.size(), 7u);
        CHECK_EQ(countOf(sent, "STORE_FETCH"), 1u);           // one read, once
        for (int i = 0; i < 4000 && Access::cache(app).model.catalogueFromCache; ++i) {
            frame(app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int i = 0; i < 6; ++i) { frame(app); }
        CHECK_EQ(countOf(sent, "STORE_FETCH"), 1u);
        CHECK(!Access::lists(app).model.catalogueFromCache && Access::lists(app, false).readThisSession);
        Access::tap(app, nullptr);
        std::printf("      a kept copy at start-up: 7 rows from disk, the rail not looking, the first open sends one FETCH, the read replaces it\n");
    }
    // A KEPT COPY AND A READ THAT FAILS (the default address, the transport stand-in answering HTTP 503 - a user offline for a moment): the kept
    // copy stays on screen, the table says the refresh failed and the rows are still the kept copy's; the red line is the reason. (The 0.99.42
    // window emptied the store here, and so did the engine until stub campaign B3.)
    {
        World w;
        std::filesystem::copy_file(w.index, g_plugins / "catalogue.json", std::filesystem::copy_options::overwrite_existing);
        writeText(g_plugins / "catalogue.json.time", "1790000000\n");
        th::TestWindow app;
        Access::look(app);
        Access::look(app);
        Access::setOpen(app, true);
        for (int i = 0; i < 4000 && !Access::cache(app).model.refreshFailed; ++i) {
            frame(app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (int i = 0; i < 6; ++i) { frame(app); }
        const cascade::gui::PluginStoreModel& m = Access::cache(app).model;
        CHECK(m.refreshFailed && m.catalogueFromCache && m.haveCatalogue && m.modules.size() == 7u);
        CHECK(!m.sourceError.empty() && m.sourceStatus.empty());
        CHECK_EQ(m.catalogueReadTime, 1790000000);
        CHECK(!Access::lists(app, false).readThisSession);  // still nothing read this session
        std::printf("      a kept copy and a failed refresh: the %zu rows stay (kept, %lld), the table says the refresh failed (\"%.40s\")\n",
                    m.modules.size(), static_cast<long long>(m.catalogueReadTime), m.sourceError.c_str());
    }
}

// THE STATES THE PAGE CAN BE IN, as the survey sets them: which page is open (by catalogue key), the tick on that page, the GET EVERYTHING tick, the tab.
struct Scene {
    std::string page;
    bool legal = false;
    bool all = false;
    int tab = 0;
};

void testDrawnAndClicked() {
    std::printf("  D2: the real window drawn; every widget found and clicked sends exactly the commands it should, in every state of the page\n");
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

    // The drawing follows the state, and only the state: the same frame twice is the same picture, an open page moves it.
    frame(app);
    frame(app);
    const std::uint64_t h0 = drawHash();
    frame(app);
    const std::uint64_t h1 = drawHash();
    CHECK(h0 != 0u);
    CHECK_EQ(h0, h1);
    Access::deck(app).pageId = "beta.legal";
    frame(app);
    frame(app);
    CHECK(drawHash() != h0);
    Access::deck(app) = cascade::gui::PluginStoreDeck{};
    frame(app);
    frame(app);
    CHECK_EQ(drawHash(), h0);

    // A PAGE OPENED CLEARS THE TICK (the view does it on the edge of a new page: the tick belongs to the page that had it) and asks for its
    // pictures, so the page is opened and drawn once, what that asked is dropped (the survey wants no engine state of its own making), and
    // the ticks are given after.
    const auto inScene = [&](const Scene& s) {
        return [&app, s] {
            Access::deck(app) = cascade::gui::PluginStoreDeck{};
            Access::deck(app).pageId = s.page;
            Access::deck(app).tab = s.tab;
            Access::look(app);
            drawOnly(app, true, false);
            Access::dropQueued(app);
            Access::deck(app).legalAck = s.legal;
            Access::deck(app).addAllAck = s.all;
            Access::look(app);
        };
    };
    std::set<std::string> allKeys;
    for (const StoreModule& sm : Access::lists(app).model.modules) { allKeys.insert(sm.id); }
    CHECK_EQ(allKeys.size(), 7u);
    const std::string upStage = expect("APP_STORE_UPDATE", 0, 0, kStageId);
    const std::string upNine = expect("APP_STORE_UPDATE", 0, 0, kNineId);
    const std::string fetch = expect("STORE_FETCH", 0, 0, "");
    const std::string fitAlpha = expect("STORE_INSTALL", 0, 0, "alpha-good");
    const std::string addAll = expect("APP_STORE_ADD_ALL", 0, 0, "");
    const std::string updateAll = expect("APP_STORE_ADD_ALL", 0, 1, "");

    // THE GRID, nothing ticked: GET on the one plugin that needs no notice and has a build (alpha), UPDATE on the two the planner has a plan for,
    // CHECK AGAIN, and GET EVERYTHING (the engine plans the run). GET on the notice's card is NOT here: it opens the page and sends nothing; GET
    // on the entries that cannot be fitted is greyed. The tabs, the search field, the SITES key and every card's body send nothing either.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({}));
        CHECK(found.size() >= 20u);
        sameCommands(commandsOf(found), {fetch, fitAlpha, upStage, upNine, addAll}, "the grid");
        // ...and EVERY CARD'S PAGE WAS OPENED BY A CLICK on it (its body, its name, its strip: the whole card is the page's key, 0.99.77) and
        // asked for its pictures: seven pages, the retry flag on each.
        CHECK(wantsOf(found) == allKeys);
        std::printf("      the grid: %zu widgets found, 5 commands sent, and the seven cards each opened their page\n", found.size());
    }
    // THE GRID WITH GET EVERYTHING'S OWN TICK: the same key carries it (ival[0] 1) and no module (that tick covers every notice of the run).
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({"", false, true, 0}));
        sameCommands(commandsOf(found), {fetch, fitAlpha, upStage, upNine, expect("APP_STORE_ADD_ALL", 1, 0, "")}, "the grid, GET EVERYTHING's tick given");
    }
    // A PAGE WITH NO NOTICE (alpha): its header key is GET and sends the same fit; nothing else on the page does.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({"alpha-good", false, false, 0}));
        sameCommands(commandsOf(found), {fetch, fitAlpha}, "alpha's page");
        CHECK(wantsOf(found).empty());  // (the page is open already: nothing on it opens another)
    }
    // THE NOTICE'S PAGE, NOT TICKED: the key is greyed - the one thing the page can send is CHECK AGAIN.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({"beta.legal", false, false, 0}));
        sameCommands(commandsOf(found), {fetch}, "the notice's page, not ticked");
    }
    // ...AND TICKED: the key is GET and carries the acknowledgement (ival[0] 1).
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({"beta.legal", true, false, 0}));
        sameCommands(commandsOf(found), {fetch, expect("STORE_INSTALL", 1, 0, "beta.legal")}, "the notice's page, ticked");
        std::printf("      the notice's page: greyed until the tick; ticked, GET carries the acknowledgement\n");
    }
    // A FITTED PLUGIN WITH A NEWER BUILD (nine): its page offers UPDATE, not GET.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({kNineId, false, false, 0}));
        sameCommands(commandsOf(found), {fetch, upNine}, "the fitted plugin's page");
    }
    // THE UPDATES TAB: a row and a key for each planned update, UPDATE ALL, CHECK AGAIN.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({"", false, false, 1}));
        sameCommands(commandsOf(found), {fetch, upStage, upNine, updateAll}, "the UPDATES tab");
        std::printf("      the UPDATES tab: UPDATE on each row, UPDATE ALL (ADD ALL with ival[1] 1)\n");
    }
    // A TRANSFER IN FLIGHT: everything that would start another is dead; the one key left is CANCEL.
    {
        Access::beginTransfer(app, "Alpha Decoder", "alpha-good");
        Access::setProgress(app, 0.3f);
        const auto found = survey(app, true, false, 30.0f, 12.0f, inScene({}));
        sameCommands(commandsOf(found), {expect("STORE_CANCEL", 0, 0, "")}, "a transfer in flight");
        CHECK(wantsOf(found) == allKeys);  // (looking at a page is not a transfer: the cards still open)
        Access::endTransfer(false, "Alpha Decoder", "x");
        pass(app);
        pass(app);
        CHECK(!Access::installPending(app));
    }
    std::printf("      the grid, the three kinds of page, the notice with and without its tick, the UPDATES tab and a transfer in flight: the commands are the right ones\n");
}

// THE SITES PANEL (0.99.74) and what it may send: the engine holds the sites, and the panel's two keys are commands on the session. ADD does
// nothing until the consent box is ticked AND the address passes the panel's own check; REMOVE is on each listed site.
void testSitesPanel() {
    std::printf("  D2b: the SITES panel - ADD only with the consent ticked, REMOVE on each site, and what each sends\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    openBig(app);
    const std::string address = f.w.site.generic_string();
    const auto scene = [&](bool consent, const std::string& typed) {
        return [&app, consent, typed] {
            Access::deck(app) = cascade::gui::PluginStoreDeck{};
            Access::deck(app).sitesOpen = true;
            Access::deck(app).siteConsent = consent;
            std::snprintf(Access::deck(app).siteUrl, sizeof(Access::deck(app).siteUrl), "%s", typed.c_str());
            Access::look(app);
        };
    };
    const std::string fetch = expect("STORE_FETCH", 0, 0, "");
    const std::string fitAlpha = expect("STORE_INSTALL", 0, 0, "alpha-good");
    const std::string upStage = expect("APP_STORE_UPDATE", 0, 0, kStageId);
    const std::string upNine = expect("APP_STORE_UPDATE", 0, 0, kNineId);
    const std::string addAll = expect("APP_STORE_ADD_ALL", 0, 0, "");
    const std::vector<std::string> grid = {fetch, fitAlpha, upStage, upNine, addAll};
    // THE ADDRESS TYPED, THE CONSENT NOT TICKED: ADD is greyed - nothing is sent for it, however it is pressed.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, scene(false, address));
        sameCommands(commandsOf(found), grid, "an address, no consent");
    }
    // THE CONSENT TICKED, NO ADDRESS: greyed again.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, scene(true, ""));
        sameCommands(commandsOf(found), grid, "consent, no address");
    }
    // BOTH: ADD sends the address (as typed), the consent (ival[1] 1 - the op asks for it again, so a client that is not this window cannot add
    // a site without saying so) and ADD (ival[0] 0).
    {
        std::vector<std::string> want = grid;
        want.push_back(expect("APP_STORE_SITE", 0, 1, address));
        const auto found = survey(app, true, false, 30.0f, 12.0f, scene(true, address));
        sameCommands(commandsOf(found), want, "an address and the consent");
    }
    // THE ENGINE HAS IT, and the panel lists it with a REMOVE (ival[0] 1, no consent) and the status line of what the read said.
    Access::submit(app, cmd::makeText(FOXAPP_OP_STORE_SITE, address, 0, 1));
    CHECK(passUntil(app, [&] { return Access::siteCount(app) == 1u && !Access::catalogPending(app) && Access::catalogSize(app) == 9u; }));
    pass(app);
    pass(app);
    {
        std::vector<std::string> want = grid;
        want.push_back(expect("APP_STORE_SITE", 1, 0, address));
        const auto found = survey(app, true, false, 30.0f, 12.0f, scene(false, ""));
        sameCommands(commandsOf(found), want, "a site listed: REMOVE");
        std::printf("      ADD: dead without the consent or without an address, one APP_STORE_SITE with both; REMOVE on a listed site\n");
    }
    // A SECOND ADD of the same address is refused by the panel itself (already listed) - nothing goes to the engine.
    {
        std::vector<std::string> want = grid;
        want.push_back(expect("APP_STORE_SITE", 1, 0, address));
        const auto found = survey(app, true, false, 30.0f, 12.0f, scene(true, address));
        sameCommands(commandsOf(found), want, "the same address again");
    }
}

// CLEAN UP OLD VERSIONS (0.99.49): the key at the foot of the UPDATES tab is drawn only when the engine's scan found an old copy, and ONE
// confirmation stands between it and the removal; the removal is the engine's (FOXAPP_OP_STORE_CLEAN_OLD 0: every old copy its own scan found).
void testCleanUp() {
    std::printf("  D2c: CLEAN UP OLD VERSIONS - the key only with an old copy, one confirmation, then the engine's op\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    openBig(app);
    const auto updatesTab = [&] {
        Access::deck(app) = cascade::gui::PluginStoreDeck{};
        Access::deck(app).tab = 1;
        Access::look(app);
    };
    const std::string cleanOld = expect("APP_STORE_CLEAN_OLD", 0, 0, "");
    // NO OLD COPY: no key, no command.
    {
        const auto found = survey(app, true, false, 30.0f, 12.0f, updatesTab);
        for (const Found& fd : found) { CHECK(fd.command != cleanOld); }
    }
    cascade::core::SupersededPlugin o;
    o.file = "stage_old.dll";
    o.name = "Stage One Old";
    o.version = "0.9.0";
    o.keptFile = kStage1;
    o.keptVersion = "1.0.0";
    Access::setOldCopies(app, {o}, "");
    pass(app);
    pass(app);
    // ONE OLD COPY: the key is there. Its own click opens the confirmation and sends nothing; the confirmation's key sends the one command.
    const auto found = survey(app, true, false, 30.0f, 12.0f, updatesTab);
    int silent = 0;
    for (const Found& fd : found) {
        if (fd.command.empty()) { ++silent; }
    }
    CHECK(silent > 0);
    // Find the two clicks that matter: the key (silent, and after it a second widget appears that sends CLEAN_OLD) by clicking each silent spot
    // and then looking for a widget that sends the command.
    Access::Sent log;
    Access::tap(app, &log);
    bool sentOnce = false;
    std::size_t cleanOps = 0;
    for (const Found& key : found) {
        if (!key.command.empty() || sentOnce) { continue; }
        updatesTab();
        frame(app);
        log.clear();
        clickNoPass(app, true, false, key.x, key.y);  // (the key, if this is it: the confirmation is now open)
        drawOnly(app, true, false);
        drawOnly(app, true, false);
        const auto confirm = sweep(app, true, false, 30.0f, 12.0f);
        for (const Hit& h : confirm) {
            log.clear();
            clickNoPass(app, true, false, h.x, h.y);
            for (const auto& q : log) { cleanOps += describe(q) == cleanOld ? 1u : 0u; }
            if (cleanOps > 0) {
                sentOnce = true;
                break;
            }
            // not it: put the confirmation back for the next candidate
            updatesTab();
            frame(app);
            clickNoPass(app, true, false, key.x, key.y);
            drawOnly(app, true, false);
            drawOnly(app, true, false);
        }
    }
    Access::tap(app, nullptr);
    std::printf("      the key and its one confirmation: %zu CLEAN_OLD command(s) sent in all\n", cleanOps);
    CHECK_EQ(cleanOps, 1u);
    Access::dropQueued(app);
}

// THE PAGE'S PICTURES (0.99.72): opening a page asks the engine for its pictures ONCE - on the edge, retrying a failed one - and later frames
// ask again only for a picture the table lists that nobody has asked for, and only once for a move of listSeq; the grid never asks.
void testPagePictures() {
    std::printf("  D2d: a page asks for its pictures on the edge of opening - once - and the grid never asks; one transfer at a time\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    openBig(app);
    // THE PICTURE TRANSPORT HOLDS: a request waits where it is (the engine's one transfer slot is taken, the rest of the page's pictures are
    // queued behind it) until the test lets it answer - HTTP 503 - so the states can be looked at; nothing reaches a network.
    g_netHold = true;
    g_netHits = 0;
    Access::Sent sent;
    Access::tap(app, &sent);
    // THE GRID: ten frames, nothing asked, nothing fetched.
    for (int i = 0; i < 10; ++i) { frame(app); }
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 0u);
    CHECK_EQ(g_netHits.load(), 0);
    // OPEN alpha's page (three pictures): ONE ask, on the edge, with the retry flag (ival[0] 1) and the catalogue key; the engine takes it at
    // the next pass, the first picture goes to the worker, the other two wait behind it - the table says all three are pending - and no
    // further ask follows however long the page stays open.
    Access::deck(app).pageId = "alpha-good";
    sent.clear();
    frame(app);
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 1u);
    std::string asked;
    long retry = -1;
    for (const auto& q : sent) {
        if (q.c.op == FOXAPP_OP_STORE_PICTURES_WANT) {
            asked = q.longText.empty() ? q.c.text : q.longText;
            retry = static_cast<long>(q.c.ival[0]);
        }
    }
    CHECK_EQ(asked, std::string("alpha-good"));
    CHECK_EQ(retry, 1L);
    for (int i = 0; i < 20; ++i) {
        frame(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 1u);
    CHECK(Access::lists(app).pictureBusy);
    for (const auto& pic : mod(app, "alpha-good")->pictures) { CHECK(pic.state == cascade::gui::PictureState::Pending); }
    CHECK_EQ(g_netHits.load(), 1);  // ONE transfer at a time: the other two are queued behind it
    // A PAGE WITH NO PICTURES asks only on the edge (the engine finds nothing to queue): one ask, none after.
    sent.clear();
    Access::deck(app).pageId = "beta.legal";
    frame(app);
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 1u);
    for (int i = 0; i < 20; ++i) { frame(app); }
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 1u);
    // LET THEM ANSWER: the three pictures fail (HTTP 503), one after another, and the table says each failed with the reason - the card's
    // page draws "could not be fetched" for them; the transfer slot is free again.
    g_netHold = false;
    CHECK(passUntil(app, [&] { return !Access::lists(app).pictureBusy && !Access::screenshotPending(app); }));
    pass(app);
    pass(app);
    CHECK_EQ(g_netHits.load(), 3);
    {
        const StoreModule* alpha = mod(app, "alpha-good");
        CHECK(alpha != nullptr && alpha->pictures.size() == 3u);
        if (alpha != nullptr && alpha->pictures.size() == 3u) {
            for (const auto& pic : alpha->pictures) {
                CHECK(pic.state == cascade::gui::PictureState::Failed);
                CHECK(pic.reason.find("503") != std::string::npos);
            }
        }
    }
    // THE PAGE OPENED AGAIN retries the failed ones (the edge asks with the retry flag) - and ONLY then: the sixty frames after the edge ask
    // for nothing a failure left behind.
    Access::deck(app).pageId.clear();
    frame(app);
    sent.clear();
    Access::deck(app).pageId = "alpha-good";
    frame(app);
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 1u);
    CHECK(passUntil(app, [&] { return !Access::lists(app).pictureBusy && !Access::screenshotPending(app) && g_netHits.load() >= 6; }));
    pass(app);
    for (int i = 0; i < 20; ++i) { frame(app); }
    CHECK_EQ(countOf(sent, "APP_STORE_PICTURES_WANT"), 1u);
    CHECK_EQ(g_netHits.load(), 6);
    // CLOSING THE WINDOW drops the page (the pictures are kept for the window's life and no longer): opening it again starts on the grid.
    Access::setOpen(app, false);
    frame(app);
    CHECK(Access::deck(app).pageId.empty());
    Access::tap(app, nullptr);
    std::printf("      the grid asks for nothing; a page asks once on opening, one transfer at a time; a failed picture is asked again only when the page is opened again\n");
}

// THE WELL BETWEEN CARDS BELONGS TO NO CARD (0.99.77: "the whole card should be clickable"): the card is the page's key from its edge to its edge - and
// not a pixel beyond, so a click on the well below one card or beside it opens nothing. Found with the pointer, not asked of the layout: down a
// column of the grid, every other pixel, the widget under the pointer is a card, then NOTHING for the 12 px well between two rows of a section, then
// the next card; across a row, the same for the 14 px well between two columns. A click in the middle of each gap sends nothing and opens no page.
// (test_plugin_store_app holds the same for the well at a card's left edge, in the real binary; this holds the other three sides, where an
// over-wide hit area would swallow the gap.)
void testCardGap() {
    std::printf("  D2e: the well between cards belongs to no card - a click in it opens no page\n");
    Env f;
    AppWindow& app = f.app;
    CHECK(fetchCatalogue(app, f.w.index, 7));
    openBig(app);
    Access::deck(app) = cascade::gui::PluginStoreDeck{};
    Access::look(app);
    frame(app);
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindow* w = ImGui::FindWindowByName(kWindowName);
    CHECK(w != nullptr);
    if (w == nullptr) { return; }
    const auto widgetAt = [&](float x, float y) {
        io.AddMousePosEvent(x, y);
        drawOnly(app, true, false);
        drawOnly(app, true, false);  // (ImGui finds what is under the pointer a frame after it moves)
        return GImGui->HoveredId;
    };
    // Scans along one axis from `from` to `to` at the fixed other coordinate and returns every stretch of NOTHING between two widgets as {its
    // middle, its length in pixels}. The wells between cards are the layout's own: 12 px between two rows of one section, 14 between two
    // columns (the sections' headings are further apart than that, so a longer stretch is not what is looked for). Seen from the pointer's side,
    // a hit area a few pixels too big on any side of a card closes its well - to the pixel.
    const auto wellsAlong = [&](bool vertical, float fixed, float from, float to, float step) {
        std::vector<std::pair<float, float>> out;
        ImGuiID before = 0;
        float emptyFrom = -1.0f;
        for (float p = from; p < to; p += step) {
            const ImGuiID hov = vertical ? widgetAt(fixed, p) : widgetAt(p, fixed);
            if (hov != 0) {
                if (before != 0 && emptyFrom >= 0.0f && p - emptyFrom >= 2.0f * step) { out.push_back({0.5f * (emptyFrom + p), p - emptyFrom}); }
                before = hov;
                emptyFrom = -1.0f;
            } else if (before != 0 && emptyFrom < 0.0f) {
                emptyFrom = p;
            }
        }
        return out;
    };
    const auto wellBetween = [](const std::vector<std::pair<float, float>>& runs, float lo, float hi) {
        for (const auto& r : runs) {
            if (r.second >= lo && r.second <= hi) { return r.first; }
        }
        return -1.0f;
    };
    // DOWN A COLUMN: try columns until one runs through two rows of one section.
    float colX = -1.0f, gapY = -1.0f;
    for (float x = w->Pos.x + 60.0f; x < w->Pos.x + w->Size.x - 60.0f && gapY < 0.0f; x += 45.0f) {
        gapY = wellBetween(wellsAlong(true, x, w->Pos.y + 90.0f, w->Pos.y + w->Size.y - 40.0f, 2.0f), 8.0f, 16.0f);
        if (gapY >= 0.0f) { colX = x; }
    }
    std::printf("      down the column at x=%.0f: a 12 px well of nothing between two rows, around y=%.0f\n", colX, gapY);
    CHECK(gapY >= 0.0f);
    // ACROSS A ROW: below the first section (alpha's, of one card: nothing beside it) find the 14 px well between two columns.
    float rowY = -1.0f, gapX = -1.0f;
    if (gapY >= 0.0f) {
        for (float y = gapY - 60.0f; y < gapY + 520.0f && gapX < 0.0f && y < w->Pos.y + w->Size.y - 40.0f; y += 14.0f) {
            gapX = wellBetween(wellsAlong(false, y, w->Pos.x + 20.0f, w->Pos.x + w->Size.x - 20.0f, 2.0f), 10.0f, 18.0f);
            if (gapX >= 0.0f) { rowY = y; }
        }
    }
    std::printf("      across the row at y=%.0f: a 14 px well of nothing between two columns, around x=%.0f\n", rowY, gapX);
    CHECK(gapX >= 0.0f);
    // A CLICK IN EACH GAP: nothing is sent, no page opens.
    Access::Sent sent;
    Access::tap(app, &sent);
    for (const auto& at : {std::make_pair(colX, gapY), std::make_pair(gapX, rowY)}) {
        if (at.first < 0.0f || at.second < 0.0f) { continue; }
        sent.clear();
        Access::park(app);
        clickNoPass(app, true, false, at.first, at.second);
        CHECK(Access::deck(app).pageId.empty());
        for (const auto& q : sent) { CHECK_EQ(q.c.op, static_cast<std::uint32_t>(FOXAPP_OP_NOTE_PANEL)); }
        Access::dropQueued(app);
    }
    Access::tap(app, nullptr);
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
    // A PAGE OPENED CLEARS THE TICK (the view does it on the edge of a new page: the tick belongs to the page that had it), so the page is
    // opened and drawn once, and the ticks are given after.
    const auto prepare = [&](const std::string& page, bool legal, bool all) {
        Access::deck(app) = cascade::gui::PluginStoreDeck{};
        Access::deck(app).pageId = page;
        Access::look(app);
        drawOnly(app, true, false);
        Access::deck(app).legalAck = legal;
        Access::deck(app).addAllAck = all;
        Access::look(app);
    };
    // The spots, found once in the grid.
    const std::vector<Found> spots = survey(app, true, false, 30.0f, 12.0f, [&] { prepare("", false, false); });
    Access::Sent sent;
    Access::tap(app, &sent);

    // CHECK NOW, with a page open and both ticks given: nothing in the click's frame; at the next pass the fetch is in flight (the table says
    // so, the lamp and its label are up) and the consents are gone - and THE LIST STAYS (0.99.72: a fetch does not empty what is on screen; a
    // user offline for a moment was shown an empty store), as does the open page; when it lands the rows are the new read's.
    {
        const Found* now = spotFor(spots, expect("STORE_FETCH", 0, 0, ""));
        CHECK(now != nullptr);
        if (now == nullptr) { return; }
        prepare("beta.legal", true, true);
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
        CHECK(Access::cache(app).model.haveCatalogue && Access::cache(app).model.modules.size() == 7u);  // the list stays while it is read again
        CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck);  // consent for a catalogue that has gone
        CHECK_EQ(Access::deck(app).pageId, std::string("beta.legal"));      // ...but the page the user is reading is theirs to close
        CHECK(passUntil(app, [&] { return !Access::catalogPending(app) && Access::catalogSize(app) == 7u; }));
        frame(app);
        frame(app);
        CHECK(Access::cache(app).model.haveCatalogue && !Access::cache(app).model.busy);
        CHECK_EQ(Access::cache(app).model.modules.size(), 7u);
        CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck);
        std::printf("      CHECK NOW: queued by its click, in flight at the next pass (\"%s\"), the ticks gone, the list and the page kept, the new read's rows back\n",
                    "fetching the catalogue");
    }

    // FIT on alpha: the key was alive when pressed; the engine re-tests the gate when the command applies (here a
    // transfer began in between) and answers DENIED - the reason is the red line, the frame after the pass.
    {
        const Found* fit = spotFor(spots, expect("STORE_INSTALL", 0, 0, "alpha-good"));
        CHECK(fit != nullptr);
        if (fit == nullptr) { return; }
        prepare("", false, false);
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
        const auto busySpots = survey(app, true, false, 30.0f, 12.0f, [&] { prepare("", false, false); });
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
        Access::deck(app).pageId = "beta.legal";
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
    CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck && Access::deck(app).pageId == "beta.legal");
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
    CHECK(!Access::deck(app).legalAck && !Access::deck(app).addAllAck && Access::deck(app).pageId == "beta.legal");  // ...and the consents are gone
    CHECK_EQ(mod(app, "beta.legal")->plate.legalNotice, std::string(300, 'M'));

    // (c) THE SAME CATALOGUE AGAIN, unseen: nothing about it changed, so nothing is cleared (master cleared at
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
    // any network): its position is the module in flight - 1-based, each number once - where master's window lettered
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
    std::printf("      the run as lettered: %s\n      (master's window lettered: %s)\n", joined.c_str(), oldJoined.c_str());
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
    // Master's window lettered one more for the first two modules - and the same for the last.
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
    cascade::core::PluginRepo::setTransportForTest([](const std::string&, std::uint64_t, const std::function<bool(const void*, std::size_t)>&,
                                                      std::string& error) {
        ++g_netHits;
        while (g_netHold.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        error = "the server returned HTTP 503";
        return false;
    });

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 2000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testModelEqualsMasters();
    testInstallStates();
    testWhatTheWindowIsTold();
    testCacheFollowsListSeq();
    testFirstOpen();
    testDrawnAndClicked();
    testSitesPanel();
    testCleanUp();
    testPagePictures();
    testCardGap();
    testNothingFetched();
    testRailRows();
    testControlsLandAtTheNextPass();
    testRailControlsLand();
    testUrlField();
    testConsentsAndRun();
    testRetiredConfirmFollowsTheFile();
    testLanguageAndRail();

    ImGui::DestroyContext();
    cascade::core::PluginRepo::setTransportForTest(nullptr);
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_plugin_store_window");
}
