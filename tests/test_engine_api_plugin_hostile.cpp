// test_engine_api_plugin_hostile.cpp - THE ENGINE'S HALF OF THE HOSTILE-PLUGIN SUITE (plugin containment P2, docs/engine-stage4.md 6.29): the real
// Engine, its real passes, a "window" (a PluginUi on the directive path) and the hostile fixture, one abuse per rig.
//
// test_plugin_hostile holds the guard (every entry x a fault, a throw, a loop) with the ledger fed the way the engine feeds it; this holds the
// ENGINE: that it is the engine's own pass which feeds the anonymous ledger, puts a switched-off plugin down in both halves (the runner's instances
// and the window's, through one directive), says why on the plugin's row and in the diagnostic log, and leaves a neighbour alone - for each kind
// of abuse and through each of the drivers that can reach one (the engine's wake of a plugin, the window's per-frame poll, the picture poll).
//
//   A  FAULT   in create() (the engine wakes the plugin), in poll_tracks() (the window's frame), a wild pixel pointer (the picture poll)
//   B  THROW   a C++ exception out of create()
//   C  LOOP    a create() that does not return within four of its budgets (stood down to 100 ms)
//   D  MISUSE  floods and wrong-thread calls: refused and counted, said ONCE in the log, no ledger word, the plugin NOT put down
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "check.hpp"
#include "hostile_support.hpp"
#include "core/app_commands.hpp"
#include "core/health_events.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_marker.hpp"
#include "core/plugin_ui.hpp"
#include "core/telemetry.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace cc = cascade::core;
namespace fault = cascade::core::fault;
namespace marker = cascade::core::marker;
using cascade::engine::Engine;

namespace {

fs::path g_scratch;
fs::path g_plugins;
std::string g_fixture;

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string pluginDirText() { return g_plugins.string(); }


}  // namespace

namespace cascade::gui {
// The friend the Engine names for its tests.
struct AppWindowTestAccess {
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirText;
    }
    static void installWindow(Engine& e, cc::PluginUi& ui) { e.setPluginUiApplier([&ui](const cc::PluginDirective& d) { ui.apply(d); }); }
    static void removeWindow(Engine& e) { e.setPluginUiApplier({}); }
    static cc::PluginRunner& runner(Engine& e) { return e.pluginRunner_; }
    static const std::vector<cc::LoadedPlugin>& plugins(Engine& e) { return e.pluginHost_.plugins(); }
    static std::vector<std::string> stopped(Engine& e) { return e.stoppedPluginIds(); }
    static void setRemote(Engine& e, bool on) { e.commandFromRemote_ = on; }
    static FoxCommandResult applyDirect(Engine& e, const FoxCommand& c) { return e.applyCommand(c, std::string()); }
    static bool isQuarantinedId(Engine& e, const std::string& id) { return e.pluginIsQuarantinedId(id); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

void installFixture(const char* id) { fs::copy_file(g_fixture, g_plugins / hostile::leafOf(id), fs::copy_options::overwrite_existing); }

void clearPlugins() {
    std::error_code ec;
    std::vector<fs::path> all;
    for (const auto& e : fs::directory_iterator(g_plugins, ec)) { all.push_back(e.path()); }
    for (const fs::path& p : all) { fs::remove(p, ec); }
}

struct Rig {
    Engine eng;
    std::unique_ptr<cc::PluginUi> ui;
    std::unique_ptr<testing::Session> local;

    // `settingsDir` empty is a hermetic run (no records); otherwise the crash-loop records live there.
    explicit Rig(const std::string& settingsDir = std::string(), bool safeMode = false) {
        if (safeMode) { eng.setSafePlugins(true); }
        if (!settingsDir.empty()) { eng.beginPluginContainment(settingsDir); }
        ui = std::make_unique<cc::PluginUi>(eng.pluginApiShared());
        eng.configureApi(kHooks);
        Access::installWindow(eng, *ui);
        eng.initialise();
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL,
                                                   FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE);
        CHECK(local->ok());
        pump(3);
    }
    ~Rig() { Access::removeWindow(eng); }

    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    // One command, and passes until its result lands.
    testing::Outcome send(const FoxCommand& c, testing::Session* as = nullptr) {
        testing::Session& s = as != nullptr ? *as : *local;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 30 && !o.landed; ++pass) {
            pump(1);
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s.raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string();
    }
    // The PLUGINS row of a module file, if the host holds a record for it.
    bool item(const std::string& file, FoxListItem& out) {
        const int32_t count = api()->read_list(local->raw(), FOXAPI_LIST_PLUGINS, 0, nullptr);
        for (int32_t i = 0; i < count; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            api()->read_list(local->raw(), FOXAPI_LIST_PLUGINS, static_cast<std::uint32_t>(i), &it);
            if (file == it.detail) {
                out = it;
                return true;
            }
        }
        return false;
    }
    bool runnerHas(const std::string& file) { return Access::runner(eng).hasInstances(file); }
    bool uiHas(const std::string& file) { return ui->hasInstances(file); }
    // Pinned to run: a window-attached engine lets AUTO plugins sleep until something uses them.
    void pin(const std::string& file) {
        CHECK_EQ(send(testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, 1, file.c_str())).status(), FOXAPI_OK);
        pump(3);
    }
    bool loaded(const std::string& file) {
        FoxListItem it{};
        return item(file, it) && (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
    }
};

std::map<std::string, unsigned> ledgerCounts() { return cc::health::globalLedger()->counts(); }
unsigned counted(const char* word) {
    const auto c = ledgerCounts();
    const auto it = c.find(std::string("recovered.") + word);
    return it == c.end() ? 0u : it->second;
}
void armLedger() {
    auto ledger = cc::health::globalLedger();
    ledger->reset();
    ledger->arm("", cc::newInstallId(), false);
}

// ---- the scenarios -----------------------------------------------------------------------------------------------------------------

// A scenario's two plugins have ids of their own (he-a1/he-b1, he-a2/he-b2...): a module the guard switched off stays mapped for the life of the
// process, and a mapped module's file cannot be replaced on Windows.
std::string g_aId = "he-a";
std::string g_bId = "he-b";
int g_scenario = 0;
std::string aMode(const char* mode) { return g_aId + ":" + mode; }

const cc::LoadedPlugin* pluginOf(Rig& r, const std::string& file) {
    for (const cc::LoadedPlugin& p : Access::plugins(r.eng)) {
        if (cc::pluginKey(p) == file) { return &p; }
    }
    return nullptr;
}

bool logHas(const std::string& needle) {
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

std::size_t logCount(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : cc::DiagLog::instance().ringSnapshot()) { n += line.find(needle) != std::string::npos ? 1u : 0u; }
    return n;
}

// Both plugins built and running in both halves (the abuser first, then its neighbour).
void bringUp(Rig& r, const std::string& a, const std::string& b) {
    r.pin(a);
    r.pin(b);
    CHECK(r.runnerHas(a) && r.uiHas(a) && r.runnerHas(b) && r.uiHas(b));
}

// What the engine must have done about a plugin the guard switched off, for ANY abuse that does it: both halves put down through the one directive,
// the neighbour untouched, the row's sentence naming the entry and the kind of failure, the notice level an error, the diagnostic log saying so.
void expectPutDown(Rig& r, const std::string& a, const std::string& b, const char* kindPhrase, const char* entryWord) {
    CHECK(fault::isQuarantinedKey(a));
    CHECK(Access::isQuarantinedId(r.eng, g_aId) && !Access::isQuarantinedId(r.eng, g_bId));
    CHECK(!r.runnerHas(a) && !r.uiHas(a));
    CHECK(r.runnerHas(b) && r.uiHas(b));
    const std::string sentence = r.fact("plugin." + a + ".fault");
    std::printf("      the row says: %s\n", sentence.c_str());
    CHECK(sentence.find(kindPhrase) != std::string::npos);
    CHECK(sentence.find(std::string(" in ") + entryWord + " ") != std::string::npos);
    CHECK(sentence.find("RE-ENABLE") != std::string::npos);
    CHECK(r.fact("plugin." + b + ".fault").empty());
    FoxListItem it{};
    CHECK(r.item(a, it));
    CHECK((static_cast<int>(it.value[3]) & 8) != 0);
    CHECK(it.value[2] >= 2.0);
    CHECK(logHas("plugin: " + g_aId + " was switched off for this session"));
    // the module stays mapped: its instances were leaked on purpose
    const cc::LoadedPlugin* lp = pluginOf(r, a);
    CHECK(lp != nullptr && lp->loaded && lp->nativeHandle != nullptr);
    // the plugin's row in the health table
    bool row = false;
    for (const fault::HealthRow& h : fault::health()) { row = row || (h.id == g_aId && h.version == "1.2.3" && h.quarantines == 1); }
    CHECK(row);
}

void freshRig() {
    ++g_scenario;
    g_aId = "he-a" + std::to_string(g_scenario);
    g_bId = "he-b" + std::to_string(g_scenario);
    clearPlugins();
    installFixture(g_aId.c_str());
    installFixture(g_bId.c_str());
    fault::resetForTest();
    fault::dropBudgetOverridesForTest();
    marker::resetForTest();
    hostile::setMode(nullptr);
    armLedger();
}

void testFaults() {
    std::printf("  A: a FAULT, through each driver\n");
#if !defined(_WIN32)
    std::printf("    SKIPPED: the absorbing needs Windows' structured exceptions\n");
    return;
#else
    // ---- the engine's own wake of the plugin: create()
    {
        std::printf("    create() when the engine wakes the plugin\n");
        freshRig();
        const std::string a = hostile::leafOf(g_aId);
        const std::string b = hostile::leafOf(g_bId);
        Rig r;
        r.pin(b);
        hostile::setMode(aMode("create-av").c_str());
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, 1, a.c_str())).status(), FOXAPI_OK);
        r.pump(4);
        hostile::setMode(nullptr);
        expectPutDown(r, a, b, "faulted", "create");
        CHECK(counted("pluginfault") == 1 && counted("pluginquarantine") == 1 && counted("pluginwedge") == 0 && counted("pluginoveruse") == 0);
        CHECK(logHas("plugin: " + g_aId + " faulted in create"));
        std::string viol;
        CHECK(hostile::violations(*pluginOf(r, a), &viol) == 0);
        CHECK(hostile::violations(*pluginOf(r, b)) == 0);
        // ---- and only a person brings it back (6.28 rule 10): a remote session cannot, a local one does, and what it did stays counted
        const FoxCommand reenable = testing::cmd(FOXAPP_OP_PLUGIN_REENABLE, 0.0, 0, a.c_str());
        {
            testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE,
                                    FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_ADMIN, "s3cret");
            CHECK(remote.ok());
            CHECK_EQ(r.send(reenable, &remote).status(), FOXAPI_DENIED);
        }
        r.pump(2);
        CHECK(fault::isQuarantinedKey(a) && !r.runnerHas(a));
        CHECK_EQ(r.send(reenable).status(), FOXAPI_OK);
        r.pump(4);
        CHECK(!fault::isQuarantinedKey(a));
        r.pin(a);
        CHECK(r.runnerHas(a) && r.uiHas(a));   // built afresh in both halves, the plugin behaving
        bool kept = false;
        for (const fault::HealthRow& h : fault::health()) { kept = kept || (h.id == g_aId && h.quarantines == 1 && h.faults == 1); }
        CHECK(kept);
    }
    // ---- the window's per-frame poll: poll_tracks()
    {
        std::printf("    poll_tracks() in the window's frame\n");
        freshRig();
        const std::string a = hostile::leafOf(g_aId);
        const std::string b = hostile::leafOf(g_bId);
        Rig r;
        bringUp(r, a, b);
        hostile::setMode(aMode("poll-tracks-av").c_str());
        r.ui->poll();   // the frame: absorbed, poll() returned
        hostile::setMode(nullptr);
        r.pump(4);
        expectPutDown(r, a, b, "faulted", "poll-tracks");
        CHECK(counted("pluginfault") == 1 && counted("pluginquarantine") == 1);
        r.ui->poll();   // and the frame goes on with the neighbour alone
        CHECK(r.ui->trackCount() >= 1);
    }
    // ---- the picture poll: a pixel pointer that is not memory
    {
        std::printf("    a wild pixel pointer in the picture poll\n");
        freshRig();
        const std::string a = hostile::leafOf(g_aId);
        const std::string b = hostile::leafOf(g_bId);
        Rig r;
        bringUp(r, a, b);
        hostile::setMode(aMode("poll-image-wild").c_str());
        std::vector<cc::HostImage> images;
        Access::runner(r.eng).pollImages(images);
        hostile::setMode(nullptr);
        r.pump(4);
        expectPutDown(r, a, b, "faulted", "poll-image");
        CHECK(counted("pluginfault") == 1 && counted("pluginquarantine") == 1);
    }
#endif
}

void testThrow() {
    std::printf("  B: a C++ exception across the C boundary\n");
    freshRig();
    const std::string a = hostile::leafOf(g_aId);
    const std::string b = hostile::leafOf(g_bId);
    Rig r;
    r.pin(b);
    hostile::setMode(aMode("create-throw").c_str());
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, 1, a.c_str())).status(), FOXAPI_OK);
    r.pump(4);
    hostile::setMode(nullptr);
    expectPutDown(r, a, b, "threw an error", "create");
    CHECK(counted("pluginfault") == 1 && counted("pluginquarantine") == 1 && counted("pluginwedge") == 0);
    CHECK(fault::quarantineWhy(fault::slotFor(a)) == fault::Why::Exception);
}

void testLoop() {
    std::printf("  C: a call that does not return\n");
    freshRig();
    const std::string a = hostile::leafOf(g_aId);
    const std::string b = hostile::leafOf(g_bId);
    Rig r;
    r.pin(b);
    fault::overrideBudgetForTest(fault::Entry::Create, 100);   // wedged at 400 ms
    hostile::setEnv("HOSTILE_PLUGIN_SPIN_MS", "1300");
    hostile::setMode(aMode("create-spin").c_str());
    // (the pass is inline here: it is inside create() for the 1.3 s, and nothing watches it - the call's own end counts the late return)
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, 1, a.c_str())).status(), FOXAPI_OK);
    r.pump(4);
    hostile::setMode(nullptr);
    hostile::setEnv("HOSTILE_PLUGIN_SPIN_MS", nullptr);
    fault::dropBudgetOverridesForTest();
    expectPutDown(r, a, b, "stopped answering", "create");
    CHECK(counted("pluginwedge") == 1 && counted("pluginoveruse") == 1 && counted("pluginquarantine") == 1 && counted("pluginfault") == 0);
    CHECK(fault::quarantineWhy(fault::slotFor(a)) == fault::Why::Wedge);
}

void testMisuse() {
    std::printf("  D: misuse is refused, counted, said once, and costs the plugin nothing\n");
    freshRig();
    const std::string a = hostile::leafOf(g_aId);
    const std::string b = hostile::leafOf(g_bId);
    (void)cc::DiagLog::instance().ringSnapshot();
    Rig r;
    bringUp(r, a, b);
    const cc::LoadedPlugin* lp = pluginOf(r, a);
    CHECK(lp != nullptr && hostile::doOp(*lp, "stat-host") == 1);
    if (lp == nullptr) { return; }
    // a flood of log lines, a wrong-thread call, a malformed call
    CHECK(hostile::doOp(*lp, "log-flood", 400) == 400 - static_cast<int>(cc::PluginApiCore::kLogQueue));
    {
        cc::RealtimeThreadScope rt;
        CHECK(hostile::doOp(*lp, "settings-get") == CASCADE_API_WRONG_THREAD);
    }
    CHECK(hostile::doOp(*lp, "bad-call") == 5);
    r.pump(4);
    // counted, per id and version...
    std::uint64_t flood = 0, wrong = 0, bad = 0;
    for (const fault::HealthRow& h : fault::health()) {
        if (h.id != g_aId) { continue; }
        CHECK(h.version == "1.2.3");
        flood = h.misuse[static_cast<std::size_t>(fault::Misuse::Flood)];
        wrong = h.misuse[static_cast<std::size_t>(fault::Misuse::WrongThread)];
        bad = h.misuse[static_cast<std::size_t>(fault::Misuse::BadCall)];
    }
    CHECK(flood == 400 - cc::PluginApiCore::kLogQueue && wrong == 1 && bad == 5);
    // ...said ONCE each, in the log, by the engine's pass...
    CHECK(logCount("plugin: " + g_aId + " broke a rule of the plugin API (flood)") == 1);
    CHECK(logCount("plugin: " + g_aId + " broke a rule of the plugin API (wrong-thread)") == 1);
    CHECK(logCount("plugin: " + g_aId + " broke a rule of the plugin API (bad-call)") == 1);
    (void)hostile::doOp(*lp, "bad-call");
    r.pump(3);
    CHECK(logCount("plugin: " + g_aId + " broke a rule of the plugin API (bad-call)") == 1);   // not again
    // ...but it is not the anonymous ledger's business, and the plugin is not put down: the ABI has an answer for each and it was given
    CHECK(counted("pluginfault") == 0 && counted("pluginquarantine") == 0 && counted("pluginwedge") == 0 && counted("pluginoveruse") == 0);
    CHECK(!Access::isQuarantinedId(r.eng, g_aId));
    CHECK(r.runnerHas(a) && r.uiHas(a));
    CHECK(r.fact("plugin." + a + ".fault").empty());
    // and the neighbour has no record of anything
    for (const fault::HealthRow& h : fault::health()) {
        if (h.id != g_bId) { continue; }
        for (std::size_t k = 0; k < fault::kMisuseCount; ++k) { CHECK(h.misuse[k] == 0); }
    }
    // nothing the real engine did counted a host re-entry, and neither plugin saw a rule broken
    for (const fault::HealthRow& h : fault::health()) { CHECK(h.misuse[static_cast<std::size_t>(fault::Misuse::HostReentry)] == 0); }
    CHECK(hostile::violations(*lp) == 0);
    CHECK(hostile::violations(*pluginOf(r, b)) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_plugin_hostile\n");
    if (argc < 2) {
        std::printf("usage: test_engine_api_plugin_hostile <hostile fixture module>\n");
        return 2;
    }
    g_fixture = argv[1];
    engine_api_test::hermetic("plugin_hostile");
    engine_api_test::setEnv("FOXSDR_DORMANT_AFTER_MS", "400");
    g_scratch = fs::path(std::getenv("LOCALAPPDATA") != nullptr ? std::getenv("LOCALAPPDATA") : ".") / "plugin_hostile_test";
    g_plugins = g_scratch / "plugins";
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_plugins, ec);
    Access::installHooks();

    testFaults();
    testThrow();
    testLoop();
    testMisuse();

    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_plugin_hostile");
}
