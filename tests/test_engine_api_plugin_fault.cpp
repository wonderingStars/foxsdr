// test_engine_api_plugin_fault.cpp - the engine's half of the plugin containment, against the real Engine, its real passes, a "window"
// (a PluginUi on the directive path) and the real fault probe module (plugin containment P1; docs/engine-stage4.md 6.28;
// engine/engine_plugin_fault.cpp).
//
//   A  A PLUGIN THE GUARD SWITCHED OFF IS PUT DOWN BY THE PASS: its runner instances and the window's go through ONE life directive as a
//      stopped plugin; its row says so (PLUGINS value[3] bit 3, the `fault` fact, the notice); a START cannot bring it back; a rescan does
//      not map it again; its neighbour is untouched; the anonymous ledger counts `pluginquarantine`; and the plugin's module stays mapped.
//   B  RE-ENABLING IS A LOCAL DECISION: a REMOTE session, the web server's own session and a session without the grant are DENIED at submit,
//      and a remote flag is refused again where the op is applied; a LOCAL session re-enables, the plugin is built afresh, the counters are kept.
//   C  A REAL FAULT IN THE ENGINE'S OWN PATH: a create() that faults when the pass wakes a plugin is absorbed, and the plugin is switched off.
//   D  THE START: a plugin that was in play when the last run ended is refused at this start (its verdict is a fact, the notice is a fact), a
//      person's re-enable loads it, and the records follow the strike rules; SAFE MODE refuses every plugin and changes no record; a plugin
//      site's file that is not the bytes that were installed is refused, an official one is not.
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
#include "fault_probe_names.hpp"
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
std::string g_probe;

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string pluginDirText() { return g_plugins.string(); }

// The probe reads FAULT_PROBE through the process environment at the time of each call.
void probeMode(const char* mode) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA("FAULT_PROBE", mode);
#else
    if (mode == nullptr) { ::unsetenv("FAULT_PROBE"); } else { ::setenv("FAULT_PROBE", mode, 1); }
#endif
}

std::string leafOf(const char* id) { return std::string(id) + CASCADE_PROBE_SUFFIX; }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

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

void installProbe(const char* id) { fs::copy_file(g_probe, g_plugins / leafOf(id), fs::copy_options::overwrite_existing); }

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

// ---- A and B ----------------------------------------------------------------------------------------------------------------------
void testLiveQuarantineAndReenable() {
    std::printf("  A: a plugin the guard switched off is put down by the pass\n");
    clearPlugins();
    installProbe("eq-a");
    installProbe("eq-b");
    fault::resetForTest();
    marker::resetForTest();
    armLedger();
    Rig r;
    const std::string a = leafOf("eq-a");
    const std::string b = leafOf("eq-b");
    r.pin(a);
    r.pin(b);
    CHECK(r.runnerHas(a) && r.uiHas(a) && r.runnerHas(b) && r.uiHas(b));
    CHECK(Access::plugins(r.eng).size() == 2);

    // the guard switches eq-a off (as it does for a fault it caught in a poll)
    CHECK(fault::quarantine(fault::slotFor(a), fault::Why::Fault, fault::Entry::PollText));
    r.pump(3);
    CHECK(!r.runnerHas(a) && !r.uiHas(a));   // both halves put down, through the one directive
    CHECK(r.runnerHas(b) && r.uiHas(b));     // the neighbour is untouched
    const std::vector<std::string> stopped = Access::stopped(r.eng);
    CHECK(std::find(stopped.begin(), stopped.end(), "eq-a") != stopped.end());
    CHECK(std::find(stopped.begin(), stopped.end(), "eq-b") == stopped.end());
    CHECK(Access::isQuarantinedId(r.eng, "eq-a") && !Access::isQuarantinedId(r.eng, "eq-b"));
    // its row: bit 3 of PLUGINS value[3], the fact, the notice level
    FoxListItem it{};
    CHECK(r.item(a, it));
    CHECK((static_cast<int>(it.value[3]) & 8) != 0);
    CHECK(it.value[2] >= 2.0);   // the notice level: an error
    FoxListItem itb{};
    CHECK(r.item(b, itb));
    CHECK((static_cast<int>(itb.value[3]) & 8) == 0);
    const std::string sentence = r.fact("plugin." + a + ".fault");
    std::printf("    the row's sentence: %s\n", sentence.c_str());
    CHECK(!sentence.empty() && sentence.find("RE-ENABLE") != std::string::npos);
    CHECK(r.fact("plugin." + b + ".fault").empty());
    CHECK(r.fact("plugin." + a + ".notice") == sentence);
    // the module is still mapped: a switched-off plugin's module is never unmapped
    bool mapped = false;
    for (const cc::LoadedPlugin& p : Access::plugins(r.eng)) { mapped = mapped || (cc::pluginKey(p) == a && p.loaded && p.nativeHandle != nullptr); }
    CHECK(mapped);
    // counted, anonymously
    CHECK(counted("pluginquarantine") == 1u);
    // a START cannot bring it back
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, 1, a.c_str())).status(), FOXAPI_OK);
    r.pump(3);
    CHECK(!r.runnerHas(a) && !r.uiHas(a));
    // ...nor does a rescan map it again as a working plugin
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    r.pump(4);
    CHECK(!r.runnerHas(a) && !r.uiHas(a));
    CHECK(!r.loaded(a));
    CHECK(r.fact("plugin." + a + ".verdict") == "switched-off");
    CHECK(!r.fact("plugin." + a + ".fault").empty());
    CHECK(r.loaded(b));

    std::printf("  B: re-enabling is a local decision\n");
    const FoxCommand reenable = testing::cmd(FOXAPP_OP_PLUGIN_REENABLE, 0.0, 0, a.c_str());
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE,
                                FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_ADMIN, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(r.send(reenable, &remote).status(), FOXAPI_DENIED);
        // the web server's own session: its profile has no such op
        std::int32_t st = 0;
        FoxSession* web = r.eng.api().openInternalSession(cascade::engine::InternalKind::Web, "the web server", &st);
        CHECK(web != nullptr);
        if (web != nullptr) {
            FoxSubmitResult sr{};
            sr.structSize = sizeof(sr);
            const int32_t n = api()->submit(web, &reenable, 1, &sr);
            const int32_t submitted = n < 0 ? n : sr.status;
            bool refused = submitted == FOXAPI_DENIED;
            for (int pass = 0; pass < 20 && !refused && submitted == FOXAPI_OK; ++pass) {
                r.pump(1);
                FoxCommandResult rr[8];
                for (auto& x : rr) { x.structSize = sizeof(x); }
                const int32_t got = api()->poll_results(web, rr, 8);
                for (int32_t i = 0; i < got; ++i) {
                    if (rr[i].ticket == sr.ticket) { refused = rr[i].status == FOXAPI_DENIED; }
                }
            }
            CHECK(refused);
            api()->close_session(web);
        }
        // a LOCAL session without the SETTINGS grant
        testing::Session viewer(api(), r.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        CHECK(viewer.ok());
        CHECK_EQ(r.send(reenable, &viewer).status(), FOXAPI_DENIED);
    }
    // the fourth lock, where the op is applied: with every submit-time lock out of the way a remote flag still refuses it
    Access::setRemote(r.eng, true);
    const FoxCommandResult direct = Access::applyDirect(r.eng, reenable);
    Access::setRemote(r.eng, false);
    CHECK(direct.status == FOXAPI_DENIED);
    r.pump(2);
    CHECK(fault::isQuarantinedKey(a));   // nothing changed
    CHECK(!r.runnerHas(a));
    // an empty name, a plugin nobody has, a plugin that is not switched off
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_REENABLE, 0.0, 0, "")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_REENABLE, 0.0, 0, "no-such-plugin.dll")).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_REENABLE, 0.0, 0, b.c_str())).status(), FOXAPI_NO_CHANGE);
    // and the local session with the SETTINGS grant does it
    const std::uint64_t quarantinesBefore = fault::totals().quarantines;
    CHECK_EQ(r.send(reenable).status(), FOXAPI_OK);
    r.pump(4);
    CHECK(!fault::isQuarantinedKey(a));
    CHECK(!Access::isQuarantinedId(r.eng, "eq-a"));
    CHECK(r.loaded(a));
    CHECK(r.fact("plugin." + a + ".fault").empty());
    FoxListItem back{};
    CHECK(r.item(a, back));
    CHECK((static_cast<int>(back.value[3]) & 8) == 0);
    r.pin(a);
    CHECK(r.runnerHas(a) && r.uiHas(a));   // built afresh, in both halves
    // the counters are kept: it was switched off once, and that is still a fact
    CHECK(fault::totals().quarantines == quarantinesBefore);
    bool kept = false;
    for (const fault::HealthRow& row : fault::health()) { kept = kept || (row.id == "eq-a" && row.quarantines == 1 && row.version == "1.2.3"); }
    CHECK(kept);
    armLedger();
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------
void testRealFaultInThePass() {
    std::printf("  C: a real fault in the engine's own path\n");
#if !defined(_WIN32)
    // The guard absorbs only where the operating system has structured exceptions (core/plugin_fault.hpp, the platform caveat): elsewhere the
    // plugin's fault IS the process's, and this scenario would end the test.
    std::printf("    SKIPPED: the absorbing needs Windows' structured exceptions\n");
    return;
#endif
    clearPlugins();
    installProbe("eq-c");
    fault::resetForTest();
    marker::resetForTest();
    armLedger();
    Rig r;
    const std::string c = leafOf("eq-c");
    CHECK(r.loaded(c));
    // the pass wakes it and its create() faults: absorbed on the control path, nothing is built, the plugin is switched off
    probeMode("create-av");
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, 1, c.c_str())).status(), FOXAPI_OK);
    r.pump(4);
    probeMode(nullptr);
#if defined(_WIN32)
    CHECK(!r.runnerHas(c));
    CHECK(fault::isQuarantinedKey(c));
    CHECK(fault::quarantineEntry(fault::slotFor(c)) == fault::Entry::Create);
    CHECK(Access::isQuarantinedId(r.eng, "eq-c"));
    FoxListItem it{};
    CHECK(r.item(c, it));
    CHECK((static_cast<int>(it.value[3]) & 8) != 0);
    CHECK(counted("pluginfault") >= 1u);
    CHECK(counted("pluginquarantine") == 1u);
#else
    std::printf("    SKIPPED: the absorbing needs Windows' structured exceptions\n");
#endif
    armLedger();
}

// ---- D ----------------------------------------------------------------------------------------------------------------------------
void testStart() {
    std::printf("  D: the start - the crash-loop records, safe mode, a tampered site file\n");
    clearPlugins();
    installProbe("eq-d");
    installProbe("eq-e");
    fault::resetForTest();
    const fs::path settings = g_scratch / "settings";
    std::error_code ec;
    fs::remove_all(settings, ec);
    fs::create_directories(settings);
    const std::string d = leafOf("eq-d");
    const std::string e = leafOf("eq-e");

    // THE DEAD RUN: eq-d was inside create() when it ended
    marker::resetForTest();
    marker::open((settings / marker::kFileName).string());
    marker::set("eq-d", marker::Phase::Create, "1.2.3");
    marker::resetForTest();   // (the process is gone: the file stays as it was)

    {
        armLedger();
        Rig r(settings.string());
        CHECK(r.eng.pluginContainment().started() && !r.eng.pluginContainment().safeMode());
        CHECK(r.eng.pluginContainment().notices().size() == 1);
        FoxListItem it{};
        CHECK(r.item(d, it));
        CHECK((it.flags & FOXAPI_ITEM_ACTIVE) == 0u);   // refused: never mapped
        CHECK(r.fact("plugin." + d + ".verdict") == "faulted-last-session");
        CHECK(r.fact("plugin." + d + ".fault").find("RE-ENABLE") != std::string::npos);
        const std::string notices = r.fact("plugins.fault.notices");
        std::printf("    the start notices: %s", notices.c_str());
        CHECK(notices.rfind("eq-d\t1.2.3\t1\t0\tcreate\n", 0) == 0);
        CHECK(r.fact("plugins.safe") == "0");
        CHECK(r.loaded(e));   // its neighbour loaded as ever
        // a rescan says it again, with no loader to say it
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK(r.fact("plugin." + d + ".verdict") == "faulted-last-session");
        CHECK(!r.loaded(d));
        // the person's click
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_PLUGIN_REENABLE, 0.0, 0, d.c_str())).status(), FOXAPI_OK);
        r.pump(4);
        CHECK(r.loaded(d));
        CHECK(r.fact("plugin." + d + ".verdict").empty());
        const cc::StrikeRecord* rec = r.eng.pluginContainment().recordOf("eq-d");
        CHECK(rec != nullptr && rec->state == cc::StrikeState::Probation && rec->strikes == 1);
        // the run ends in order, with it having run: the strike comes off
        r.pin(d);
        marker::promoteFirstPass(0xFFFFFFFFu, 0);
        r.eng.markCleanExit();
        CHECK(r.eng.pluginContainment().recordOf("eq-d") == nullptr);
    }
    marker::resetForTest();

    // SAFE MODE: nothing loads, the facts say so, and nothing about the records changes
    {
        marker::open((settings / marker::kFileName).string());
        marker::set("eq-e", marker::Phase::Create, "1.2.3");
        marker::resetForTest();
        const std::string before = readFile(settings / cc::kStoreFileName);
        Rig r(settings.string(), true);
        CHECK(r.eng.safePlugins() && r.eng.pluginContainment().safeMode());
        CHECK(r.fact("plugins.safe") == "1");
        std::printf("    safe mode: %zu records\n", Access::plugins(r.eng).size());
        // (the two modules of this scenario, and the two the guard switched off in A and C: a switched-off module stays mapped for the life of
        // the process, so Windows will not let the scenario's clean-up delete its file. Safe mode refuses all of them.)
        CHECK(Access::plugins(r.eng).size() >= 2);
        for (const cc::LoadedPlugin& p : Access::plugins(r.eng)) {
            CHECK(!p.loaded);
            CHECK(p.verdict == "safe-mode");
        }
        CHECK(r.eng.pluginContainment().notices().empty());
        r.eng.markCleanExit();
        CHECK(readFile(settings / cc::kStoreFileName) == before);
    }
    marker::resetForTest();

    // A SITE'S FILE THAT IS NOT THE BYTES THAT WERE INSTALLED is refused; the same mismatch on an OFFICIAL plugin is a note, not a refusal
    {
        clearPlugins();
        installProbe("eq-s");
        installProbe("eq-o");
        const char* kBadSha = "0000000000000000000000000000000000000000000000000000000000000000";
        {
            std::ofstream man(g_plugins / "installed.json", std::ios::binary | std::ios::trunc);
            man << "{\"plugins\":["
                << "{\"id\":\"eq-s\",\"version\":\"1.2.3\",\"file\":\"" << leafOf("eq-s")
                << "\",\"source\":\"https://site.example/plugins\",\"sha256\":\"" << kBadSha << "\"},"
                << "{\"id\":\"eq-o\",\"version\":\"1.2.3\",\"file\":\"" << leafOf("eq-o") << "\",\"sha256\":\"" << kBadSha << "\"}]}";
        }
        Rig r;
        const std::string s = leafOf("eq-s");
        const std::string o = leafOf("eq-o");
        FoxListItem is{};
        FoxListItem io{};
        CHECK(r.item(s, is) && r.item(o, io));
        std::printf("    the tampered site file: verdict=%s error=%s\n", r.fact("plugin." + s + ".verdict").c_str(),
                    r.fact("plugin." + s + ".error").c_str());
        CHECK((is.flags & FOXAPI_ITEM_ACTIVE) == 0u);
        CHECK(r.fact("plugin." + s + ".verdict") == "changed-since-install");
        CHECK(!r.fact("plugin." + s + ".error").empty());
        CHECK((io.flags & FOXAPI_ITEM_ACTIVE) != 0u);
        CHECK(r.fact("plugin." + o + ".verdict").empty());
        std::error_code rm;
        fs::remove(g_plugins / "installed.json", rm);
    }
    marker::resetForTest();
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_plugin_fault\n");
    if (argc < 2) {
        std::printf("usage: test_engine_api_plugin_fault <fault probe module>\n");
        return 2;
    }
    g_probe = argv[1];
    engine_api_test::hermetic("plugin_fault");
    engine_api_test::setEnv("FOXSDR_DORMANT_AFTER_MS", "400");
    g_scratch = fs::path(std::getenv("LOCALAPPDATA") != nullptr ? std::getenv("LOCALAPPDATA") : ".") / "plugin_fault_test";
    g_plugins = g_scratch / "plugins";
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_plugins, ec);
    Access::installHooks();

    testLiveQuarantineAndReenable();
    testRealFaultInThePass();
    testStart();

    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_plugin_fault");
}
