// test_plugin_crashloop.cpp - the crash-loop breaker against REAL deaths of REAL processes (plugin containment P1, item 1; docs/engine-stage4.md
// 6.28, core/plugin_marker.hpp).
//
//   A  WHO IS GUILTY, in dead processes. This executable re-runs itself as a child that loads the fault probe and dies five ways, and the
//      parent reads what the child's marker held and judges it with the product's rules:
//        hot-av       process() - the HOT path, unguarded - faults: the crash handler walks the stack, finds the plugin's module on it, writes
//                     `blamed-plugin:` lines into the report and BLAMES the plugin in the marker: suspect, though the plugin was `running`;
//        create-die   the process ends inside create(): no handler runs; the marker says `create`: suspect;
//        destroy-die  ...inside destroy(): `unload`: suspect;
//        app-bug      the APPLICATION's own bug (a fault in this program's code) with the plugin running healthily: NOBODY is suspect;
//        clean        an orderly stop: nothing is left in the marker.
//   B  THE WHOLE LOOP, with the real `foxsdr-engine` binary, one settings file, one plugins folder: a plugin that kills the engine inside
//      create() kills it once; the next start is without the plugin (its probe log shows it was never mapped) and says so in the record; the one
//      after gives it a second chance and it kills the engine again; the one after that is the second strike - and from then on the engine starts
//      without it, run after run, until a person re-enables it (tests/test_engine_api_plugin_fault.cpp holds the re-enable). A healthy plugin is
//      never touched. --safe-plugins starts without any plugin and changes no record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_marker.hpp"
#include "core/plugin_runner.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"
#include "fault_probe_names.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace marker = cascade::core::marker;
using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;
using cascade::core::PluginRunner;
using engine_main_test::Child;

namespace {

std::string leafOf(const char* id) { return std::string(id) + CASCADE_PROBE_SUFFIX; }

std::string readFile(const fs::path& p) { return engine_main_test::readFile(p); }

std::size_t countLines(const std::string& text, const std::string& line) {
    std::size_t n = 0;
    std::size_t at = 0;
    while ((at = text.find(line + "\n", at)) != std::string::npos) {
        if (at == 0 || text[at - 1] == '\n') { ++n; }
        at += line.size();
    }
    return n;
}

// =============================================================================================================================
// THE CHILD
// =============================================================================================================================
int childMain(const std::string& scenario, const fs::path& dir, const std::string& probe) {
    cascade::core::CrashHandlerConfig cfg;
    cfg.crashDir = (dir / "crash").string();
    cfg.enabled = true;
    cfg.exitAfterReport = true;   // die at once and quietly, with the exception's own code (no error-reporting dialog)
    cascade::core::installCrashHandlers(cfg);
    marker::open((dir / marker::kFileName).string());

    PluginHost host;
    host.scan((dir / "plugins").string());
    const LoadedPlugin* lp = nullptr;
    for (const LoadedPlugin& p : host.plugins()) {
        if (p.loaded) { lp = &p; }
    }
    if (lp == nullptr) { return 20; }
    PluginRunner runner;
    float buf[480] = {};

    if (scenario == "hot-av") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        engine_main_test::setEnv("FAULT_PROBE", "process-av");
        runner.processAudio(buf, 480);   // the plugin's process() faults here, on the unguarded hot path
        return 21;                        // (not reached)
    }
    if (scenario == "create-die") {
        engine_main_test::setEnv("FAULT_PROBE", "create-die");
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        return 22;
    }
    if (scenario == "destroy-die") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        runner.processAudio(buf, 480);
        engine_main_test::setEnv("FAULT_PROBE", "destroy-die");
        runner.clear();
        return 23;
    }
    if (scenario == "app-bug") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        runner.processAudio(buf, 480);
        marker::promoteFirstPass(0xFFFFFFFFu, 0);   // it has been alive long enough: running
        cascade::core::raiseTestFault(cascade::core::TestFaultKind::AccessViolation);   // this program's own code faults
        return 24;
    }
    if (scenario == "clean") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        runner.processAudio(buf, 480);
        marker::promoteFirstPass(0xFFFFFFFFu, 0);
        runner.clear();
        host.unloadAll();
        marker::cleanShutdown();
        return 0;
    }
    (void)probe;
    return 25;
}

// =============================================================================================================================
// THE PARENT
// =============================================================================================================================
struct Run {
    int code = -1;
    bool ended = false;
};

Run runChild(const fs::path& self, const std::string& scenario, const fs::path& dir, const std::string& probe, int timeoutMs = 60000) {
    Child c;
    Run r;
    if (!c.start(self, {"--child", scenario, dir.string(), probe}, dir, dir / (scenario + ".out"), dir / (scenario + ".err"))) { return r; }
    r.ended = c.waitExit(timeoutMs);
    r.code = c.code();
    return r;
}

void freshDir(const fs::path& dir, const std::string& probe, const char* id) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "plugins");
    fs::copy_file(probe, dir / "plugins" / leafOf(id), fs::copy_options::overwrite_existing, ec);
    fs::create_directories(dir / "crash");
}

// What the dead child's marker held, read by this process: through the product's own reader (open() returns the previous run's content).
std::vector<marker::InPlay> previousOf(const fs::path& dir) {
    marker::resetForTest();
    std::vector<marker::InPlay> prev = marker::open((dir / marker::kFileName).string());
    marker::resetForTest();
    return prev;
}

const marker::InPlay* named(const std::vector<marker::InPlay>& v, const char* id) {
    for (const marker::InPlay& p : v) {
        if (p.id == id) { return &p; }
    }
    return nullptr;
}

void testDeaths(const fs::path& self, const fs::path& root, const std::string& probe) {
#if defined(_WIN32)
    // ---- hot-av: the unguarded signal path faults; the handler blames the plugin by its module on the stack
    {
        const fs::path dir = root / "hot";
        freshDir(dir, probe, "victim");
        const Run r = runChild(self, "hot-av", dir, probe);
        CHECK(r.ended && r.code == static_cast<int>(0xC0000005));
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr);
        if (v != nullptr) {
            CHECK(v->blamed);                                   // the crash handler put its name in the marker
            CHECK(v->version == "1.2.3");
            CHECK(v->phase == marker::Phase::FirstPass || v->phase == marker::Phase::Running);
        }
        const std::vector<marker::InPlay> suspects = cascade::core::suspectsOf(prev);
        CHECK(suspects.size() == 1 && suspects[0].id == "victim");
        // ...and the report says whose module it was (the PUBLIC id: this plugin is unrecorded, so a hash, never its name)
        std::string report;
        for (const auto& e : fs::directory_iterator(dir / "crash")) {
            if (e.path().filename().string().rfind("crash-", 0) == 0) { report = readFile(e.path()); }
        }
        CHECK(!report.empty());
        CHECK(report.find("blamed-plugin-how: stack") != std::string::npos);
        CHECK(report.find("blamed-plugin-version: 1.2.3") != std::string::npos);
        CHECK(report.find("blamed-plugin-origin: unrecorded") != std::string::npos);
        CHECK(report.find("blamed-plugin: site-") != std::string::npos);
        CHECK(report.find("blamed-plugin: victim") == std::string::npos);
        CHECK(report.find("victim") == std::string::npos || report.find("victim") != report.find("blamed-plugin"));   // (the module list may name the file; the header does not)
        // the strike story from here: strike one, skipped next start
        const cascade::core::StartPlan plan = cascade::core::planStart({}, prev, 1);
        CHECK(plan.skip == std::vector<std::string>{"victim"} && plan.notices.size() == 1 && !plan.notices[0].permanent);
    }

    // ---- create-die: no handler ran at all; the marker says `create`
    {
        const fs::path dir = root / "create";
        freshDir(dir, probe, "victim");
        const Run r = runChild(self, "create-die", dir, probe);
        CHECK(r.ended && r.code == static_cast<int>(0xC0000005));
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::Create && !v->blamed);
        CHECK(cascade::core::suspectsOf(prev).size() == 1);
        CHECK(fs::is_empty(dir / "crash"));   // (a death nothing could report: the marker is the only evidence there is)
    }

    // ---- destroy-die: `unload`
    {
        const fs::path dir = root / "destroy";
        freshDir(dir, probe, "victim");
        const Run r = runChild(self, "destroy-die", dir, probe);
        CHECK(r.ended && r.code == static_cast<int>(0xC0000005));
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::Unload);
        CHECK(cascade::core::suspectsOf(prev).size() == 1);
    }

    // ---- app-bug: the application's OWN fault, with the plugin running healthily: not a strike
    {
        const fs::path dir = root / "appbug";
        freshDir(dir, probe, "victim");
        const Run r = runChild(self, "app-bug", dir, probe);
        CHECK(r.ended && r.code == static_cast<int>(0xC0000005));
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::Running && !v->blamed);
        CHECK(cascade::core::suspectsOf(prev).empty());
        const cascade::core::StartPlan plan = cascade::core::planStart({}, prev, 1);
        CHECK(plan.skip.empty() && plan.notices.empty() && plan.records.empty());
        // the report names no plugin
        std::string report;
        for (const auto& e : fs::directory_iterator(dir / "crash")) {
            if (e.path().filename().string().rfind("crash-", 0) == 0) { report = readFile(e.path()); }
        }
        CHECK(report.find("blamed-plugin: none") != std::string::npos);
    }

    // ---- clean: an orderly stop leaves nothing
    {
        const fs::path dir = root / "clean";
        freshDir(dir, probe, "victim");
        const Run r = runChild(self, "clean", dir, probe);
        CHECK(r.ended && r.code == 0);
        CHECK(previousOf(dir).empty());
    }
#else
    (void)self; (void)root; (void)probe;
    SKIP_LINUX("the real-death scenarios use TerminateProcess and structured exceptions");
#endif
}

// =============================================================================================================================
// B. the whole loop, with the real foxsdr-engine
// =============================================================================================================================
Run runEngine(const fs::path& home, const std::vector<std::string>& extra, int timeoutMs = 90000) {
    Child c;
    Run r;
    std::vector<std::string> args = {"--config", (home / "cfg" / "config.json").string(), "--seconds", "3"};
    for (const std::string& a : extra) { args.push_back(a); }
    static int n = 0;
    ++n;
    if (!c.start(engine_main_test::appExe("foxsdr-engine"), args, home, home / ("engine-" + std::to_string(n) + ".out"),
                 home / ("engine-" + std::to_string(n) + ".err"))) {
        return r;
    }
    r.ended = c.waitExit(timeoutMs);
    r.code = c.code();
    return r;
}

std::string storeText(const fs::path& home) { return readFile(home / "cfg" / cascade::core::kStoreFileName); }

void testLoop(const fs::path& root, const std::string& probe) {
#if defined(_WIN32)
    const fs::path home = root / "engine-home";
    std::error_code ec;
    fs::remove_all(home, ec);
    fs::create_directories(home / "tmp");
    fs::create_directories(home / "cfg");
    engine_main_test::isolate(home);
    engine_main_test::setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    const fs::path plugins = home / "foxsdr" / "plugins";   // the per-user plugins folder, under the scratch LOCALAPPDATA
    fs::create_directories(plugins);
    fs::copy_file(probe, plugins / leafOf("crashy"), fs::copy_options::overwrite_existing, ec);
    fs::copy_file(probe, plugins / leafOf("calm"), fs::copy_options::overwrite_existing, ec);
    const fs::path log = home / "probe.log";
    engine_main_test::setEnv("FAULT_PROBE_LOG", log.string());

    // The faults are chosen by the environment of the CHILD, and both copies of the probe read the same variable: the loop is staged with ONE
    // plugin in the folder at a time (the other copy is moved aside), so "crashy" is the only one that can die.
    const fs::path aside = home / "aside";
    fs::create_directories(aside);
    fs::rename(plugins / leafOf("calm"), aside / leafOf("calm"), ec);

    engine_main_test::setEnv("FAULT_PROBE", "create-die");
    // RUN 1: the engine starts, maps the plugin, and the plugin ends the process inside create()
    Run r1 = runEngine(home, {});
    CHECK(r1.ended);
    CHECK(r1.code == static_cast<int>(0xC0000005));
    CHECK(countLines(readFile(log), "create") == 1);
    CHECK(fs::exists(home / "cfg" / marker::kFileName));
    CHECK(!fs::exists(home / "cfg" / cascade::core::kStoreFileName));   // nothing is learned until the NEXT start reads the marker

    // RUN 2: strike one - the engine starts WITHOUT the plugin: it is not even mapped (create is not called; the log gains no line)
    Run r2 = runEngine(home, {});
    CHECK(r2.ended && r2.code == 0);
    CHECK(countLines(readFile(log), "create") == 1);
    {
        const std::string store = storeText(home);
        CHECK(store.find("\"crashy\"") != std::string::npos);
        CHECK(store.find("\"state\": \"suspect\"") != std::string::npos);
        CHECK(store.find("\"strikes\": 1") != std::string::npos);
        CHECK(store.find("\"phase\": \"create\"") != std::string::npos);
    }

    // RUN 3: its second chance - on probation it loads and ends the process again
    Run r3 = runEngine(home, {});
    CHECK(r3.ended && r3.code == static_cast<int>(0xC0000005));
    CHECK(countLines(readFile(log), "create") == 2);

    // RUN 4: strike two - DISABLED; the engine starts without it
    Run r4 = runEngine(home, {});
    CHECK(r4.ended && r4.code == 0);
    CHECK(countLines(readFile(log), "create") == 2);
    {
        const std::string store = storeText(home);
        CHECK(store.find("\"state\": \"disabled\"") != std::string::npos);
        CHECK(store.find("\"strikes\": 2") != std::string::npos);
    }
    // RUN 5 AND 6: it stays disabled, run after run - and with the fault gone from the plugin too: only a person's re-enable brings it back
    engine_main_test::unsetEnv("FAULT_PROBE");
    Run r5 = runEngine(home, {});
    CHECK(r5.ended && r5.code == 0);
    Run r6 = runEngine(home, {});
    CHECK(r6.ended && r6.code == 0);
    CHECK(countLines(readFile(log), "create") == 2);
    CHECK(storeText(home).find("\"state\": \"disabled\"") != std::string::npos);

    // SAFE MODE: starts without any plugin, says so, and changes no record
    const std::string before = storeText(home);
    fs::rename(aside / leafOf("calm"), plugins / leafOf("calm"), ec);
    Run r7 = runEngine(home, {"--safe-plugins"});
    CHECK(r7.ended && r7.code == 0);
    CHECK(storeText(home) == before);
    CHECK(engine_main_test::readText(home / "engine-7.err").find("safe mode") != std::string::npos);
    CHECK(countLines(readFile(log), "create") == 2);   // the healthy plugin was not created either

    // A HEALTHY PLUGIN IS NEVER TOUCHED: with "calm" in the folder and no fault, a normal start creates it (the log gains its line) and the
    // engine stops in order; the disabled one stays out, and the calm one earned no record
    Run r8 = runEngine(home, {});
    CHECK(r8.ended && r8.code == 0);
    CHECK(countLines(readFile(log), "create") >= 3);
    CHECK(storeText(home).find("\"calm\"") == std::string::npos);
    // an orderly stop left the marker empty
    {
        std::error_code copyEc;
        fs::copy_file(home / "cfg" / marker::kFileName, home / "marker-copy", fs::copy_options::overwrite_existing, copyEc);
        marker::resetForTest();
        CHECK(marker::open((home / "marker-copy").string()).empty());
        marker::resetForTest();
    }
#else
    (void)root; (void)probe;
    SKIP_LINUX("the loop is staged with a plugin that ends its own process (TerminateProcess)");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--child") == 0) { return childMain(argv[2], argv[3], argv[4]); }
    if (argc < 2) {
        std::printf("usage: test_plugin_crashloop <fault probe module>\n");
        return 2;
    }
    const std::string probe = argv[1];
    const fs::path self = fs::absolute(argv[0]);
    const fs::path root = fs::temp_directory_path() / ("foxsdr-test-plugin-crashloop-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                                 ::GetCurrentProcessId()
#else
                                                                                                 1
#endif
                                                                                                     )));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    testDeaths(self, root, probe);
    testLoop(root, probe);

    fs::remove_all(root, ec);
    return testSummary("test_plugin_crashloop");
}
