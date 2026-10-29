// test_plugin_hostile_loop.cpp - THE CRASH-LOOP BREAKER AND THE UNGUARDED HOT PATH, against the hostile fixture, in REAL dead processes
// (plugin containment P2, docs/engine-stage4.md 6.29; the rules are P1's, 6.28 rules 4 and 18-22, core/plugin_marker.hpp).
//
// The in-process suites (test_plugin_hostile, test_plugin_misuse) cover everything a plugin can do that the application survives. This is the part
// it does not survive, by design: the signal path (process(), pull(), active(), retune()) is not guarded, and a plugin that ends the process there,
// or inside any call, is turned from a loop of crashes into one crash and a start without it by the marker.
//
//   A  WHO IS GUILTY. This executable re-runs itself as a child that loads the hostile fixture and dies seven ways:
//        hot-av          process() - the unguarded hot path - faults: the report names the plugin (its module is on the stack) and the marker
//                        BLAMES it, though it was `running`;
//        hot-die         the process ends inside process(): no handler, no report; the plugin was `running` and unblamed - INNOCENT (the one case
//                        the breaker cannot attribute, said here as a fact and not hidden);
//        first-pass-die  the process ends inside poll_text() of a plugin that was created a moment ago and is the only one in `first-pass`: suspect;
//        stack-overflow  a plugin recurses in poll_text() until the stack is gone: a fault the guard must NOT absorb - the process dies of it;
//        create-die      the process ends inside create(): suspect;  destroy-die ...inside destroy(): suspect;
//        clean           an orderly stop leaves nothing.
//   B  THE WHOLE LOOP with the real `foxsdr-engine`: a hostile plugin that ends the engine inside create() ends it once; the start after is without
//      it; the one after gives it its second chance and it ends the engine again; then it is disabled, run after run, and the fault being gone from
//      the plugin does not bring it back; a neighbour fixture is never touched; --safe-plugins starts with none and changes no record.
//
// Windows only (TerminateProcess and structured exceptions); elsewhere SKIP with the reason.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_marker.hpp"
#include "core/plugin_runner.hpp"
#include "engine_main_proc.hpp"
#include "hostile_support.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace marker = cascade::core::marker;
using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;
using cascade::core::PluginRunner;
using engine_main_test::Child;
using hostile::countLines;
using hostile::leafOf;
using hostile::readFile;

namespace {

// =============================================================================================================================
// THE CHILD
// =============================================================================================================================
int childMain(const std::string& scenario, const fs::path& dir) {
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
        marker::promoteFirstPass(0xFFFFFFFFu, 0);   // it has been alive long enough: running
        hostile::setMode("process-av");
        runner.processAudio(buf, 480);   // the plugin's process() faults here, on the unguarded hot path
        return 21;                        // (not reached)
    }
    if (scenario == "hot-die") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        marker::promoteFirstPass(0xFFFFFFFFu, 0);
        hostile::setMode("process-die");
        runner.processAudio(buf, 480);
        return 22;
    }
    if (scenario == "first-pass-die") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);   // created just now: `first-pass`
        hostile::setMode("poll-text-die");
        runner.processAudio(buf, 480);   // process(), then the poll - which ends the process
        return 23;
    }
    if (scenario == "stack-overflow") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        hostile::setMode("poll-text-stack");
        runner.processAudio(buf, 480);   // process(), then the poll - in which the plugin recurses until the stack is gone
        return 27;
    }
    if (scenario == "create-die") {
        hostile::setMode("create-die");
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        return 24;
    }
    if (scenario == "destroy-die") {
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        runner.processAudio(buf, 480);
        marker::promoteFirstPass(0xFFFFFFFFu, 0);
        hostile::setMode("destroy-die");
        runner.clear();
        return 25;
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
    return 26;
}

// =============================================================================================================================
// THE PARENT
// =============================================================================================================================
struct Run {
    int code = -1;
    bool ended = false;
};

Run runChild(const fs::path& self, const std::string& scenario, const fs::path& dir, int timeoutMs = 60000) {
    Child c;
    Run r;
    if (!c.start(self, {"--child", scenario, dir.string()}, dir, dir / (scenario + ".out"), dir / (scenario + ".err"))) { return r; }
    r.ended = c.waitExit(timeoutMs);
    r.code = c.code();
    return r;
}

void freshDir(const fs::path& dir, const std::string& fixture, const char* id) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "plugins");
    fs::copy_file(fixture, dir / "plugins" / leafOf(id), fs::copy_options::overwrite_existing, ec);
    fs::create_directories(dir / "crash");
}

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

std::string crashReport(const fs::path& dir) {
    std::string report;
    for (const auto& e : fs::directory_iterator(dir / "crash")) {
        if (e.path().filename().string().rfind("crash-", 0) == 0) { report = readFile(e.path()); }
    }
    return report;
}

void testDeaths(const fs::path& self, const fs::path& root, const std::string& fixture) {
#if defined(_WIN32)
    std::printf("  A: who is guilty, in dead processes\n");
    const int kAv = static_cast<int>(0xC0000005);
    {   // hot-av
        const fs::path dir = root / "hot";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "hot-av", dir);
        CHECK(r.ended && r.code == kAv);
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->blamed && v->version == "1.2.3");   // the crash handler put its name in the marker, though it was running
        CHECK(cascade::core::suspectsOf(prev).size() == 1);
        const std::string report = crashReport(dir);
        CHECK(!report.empty());
        CHECK(report.find("blamed-plugin-how: stack") != std::string::npos);
        CHECK(report.find("blamed-plugin: site-") != std::string::npos);        // named by its hash: nothing records where this file came from
        CHECK(report.find("blamed-plugin: victim") == std::string::npos);
        CHECK(report.find("blamed-plugin-entry: none") != std::string::npos);   // the hot path is not a guarded entry: no call was in flight
        const cascade::core::StartPlan plan = cascade::core::planStart({}, prev, 1);
        CHECK(plan.skip == std::vector<std::string>{"victim"} && plan.notices.size() == 1);
    }
    {   // hot-die: the one death the breaker cannot attribute
        const fs::path dir = root / "hotdie";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "hot-die", dir);
        CHECK(r.ended && r.code == kAv);
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && !v->blamed && v->phase == marker::Phase::Running);
        CHECK(cascade::core::suspectsOf(prev).empty());   // (documented in 6.29: a plugin that ends the process by TerminateProcess from the hot path while running)
        CHECK(fs::is_empty(dir / "crash"));
    }
    {   // first-pass-die
        const fs::path dir = root / "firstpass";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "first-pass-die", dir);
        CHECK(r.ended && r.code == kAv);
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::FirstPass);
        const std::vector<marker::InPlay> suspects = cascade::core::suspectsOf(prev);
        CHECK(suspects.size() == 1 && suspects[0].id == "victim");   // the lone plugin in first-pass
    }
    {   // stack-overflow: a guarded call, but a fault the guard is not allowed to absorb (6.28 rule 5)
        const fs::path dir = root / "stack";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "stack-overflow", dir);
        std::printf("    stack overflow: exit 0x%08X\n", static_cast<unsigned>(r.code));
        CHECK(r.ended && r.code == static_cast<int>(0xC00000FD));   // the process died of it - the guard did not hide it
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::FirstPass);
        CHECK(cascade::core::suspectsOf(prev).size() == 1);   // and the next start is without it: a death inside a plugin that was created a moment ago
        const std::string report = crashReport(dir);
        std::printf("    stack overflow: the crash handler %s\n", report.empty() ? "wrote no report (nothing can be said of a stack with none left)" : "wrote a report");
        if (!report.empty()) {
            CHECK(report.find("absorbed") == std::string::npos);   // a report of a DEATH, not an absorbed fault
            CHECK(report.find("blamed-plugin: site-") != std::string::npos);
            CHECK(report.find("blamed-plugin-entry: poll-text") != std::string::npos);
        }
    }
    {   // create-die
        const fs::path dir = root / "create";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "create-die", dir);
        CHECK(r.ended && r.code == kAv);
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::Create && !v->blamed);
        CHECK(cascade::core::suspectsOf(prev).size() == 1);
    }
    {   // destroy-die
        const fs::path dir = root / "destroy";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "destroy-die", dir);
        CHECK(r.ended && r.code == kAv);
        const std::vector<marker::InPlay> prev = previousOf(dir);
        const marker::InPlay* v = named(prev, "victim");
        CHECK(v != nullptr && v->phase == marker::Phase::Unload);
        CHECK(cascade::core::suspectsOf(prev).size() == 1);
    }
    {   // clean
        const fs::path dir = root / "clean";
        freshDir(dir, fixture, "victim");
        const Run r = runChild(self, "clean", dir);
        CHECK(r.ended && r.code == 0);
        CHECK(previousOf(dir).empty());
    }
#else
    (void)self; (void)root; (void)fixture;
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

void testLoop(const fs::path& root, const std::string& fixture) {
#if defined(_WIN32)
    std::printf("  B: the whole loop with the real engine\n");
    const fs::path home = root / "engine-home";
    std::error_code ec;
    fs::remove_all(home, ec);
    fs::create_directories(home / "tmp");
    fs::create_directories(home / "cfg");
    engine_main_test::isolate(home);
    engine_main_test::setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    const fs::path plugins = home / "foxsdr" / "plugins";
    fs::create_directories(plugins);
    fs::copy_file(fixture, plugins / leafOf("hostile-a"), fs::copy_options::overwrite_existing, ec);
    fs::copy_file(fixture, plugins / leafOf("hostile-b"), fs::copy_options::overwrite_existing, ec);
    const fs::path log = home / "hostile.log";
    engine_main_test::setEnv("HOSTILE_PLUGIN_LOG", log.string());
    // The mode is scoped to "hostile-a": its neighbour reads the same variable and ignores it.
    engine_main_test::setEnv("HOSTILE_PLUGIN", "hostile-a:create-die");

    // RUN 1: the engine starts, maps both, and the hostile one ends the process inside create()
    const Run r1 = runEngine(home, {});
    CHECK(r1.ended && r1.code == static_cast<int>(0xC0000005));
    CHECK(countLines(readFile(log), "create") >= 1);
    CHECK(!fs::exists(home / "cfg" / cascade::core::kStoreFileName));   // nothing is learned until the NEXT start reads the marker

    // RUN 2: strike one - the engine starts WITHOUT it (not even mapped) and the neighbour is created as ever
    const std::size_t createdAfter1 = countLines(readFile(log), "create");
    const Run r2 = runEngine(home, {});
    CHECK(r2.ended && r2.code == 0);
    {
        const std::string store = storeText(home);
        CHECK(store.find("\"hostile-a\"") != std::string::npos);
        CHECK(store.find("\"state\": \"suspect\"") != std::string::npos);
        CHECK(store.find("\"strikes\": 1") != std::string::npos);
        CHECK(store.find("\"hostile-b\"") == std::string::npos);   // the neighbour earned no record
    }
    CHECK(countLines(readFile(log), "create") > createdAfter1);   // the neighbour lived and was created

    // RUN 3: its second chance, on probation: it ends the process again
    const Run r3 = runEngine(home, {});
    CHECK(r3.ended && r3.code == static_cast<int>(0xC0000005));

    // RUN 4: strike two - DISABLED
    const Run r4 = runEngine(home, {});
    CHECK(r4.ended && r4.code == 0);
    CHECK(storeText(home).find("\"state\": \"disabled\"") != std::string::npos);
    CHECK(storeText(home).find("\"strikes\": 2") != std::string::npos);

    // RUNS 5 AND 6: it stays disabled, with the fault gone from the plugin too
    engine_main_test::unsetEnv("HOSTILE_PLUGIN");
    const std::string lastLog = readFile(log);
    const Run r5 = runEngine(home, {});
    const Run r6 = runEngine(home, {});
    CHECK(r5.ended && r5.code == 0 && r6.ended && r6.code == 0);
    CHECK(storeText(home).find("\"state\": \"disabled\"") != std::string::npos);

    // SAFE MODE: no plugin at all, no change to any record
    const std::string before = storeText(home);
    const std::size_t createdBeforeSafe = countLines(readFile(log), "create");
    const Run r7 = runEngine(home, {"--safe-plugins"});
    CHECK(r7.ended && r7.code == 0);
    CHECK(storeText(home) == before);
    CHECK(engine_main_test::readText(home / "engine-7.err").find("safe mode") != std::string::npos);
    CHECK(countLines(readFile(log), "create") == createdBeforeSafe);
    (void)lastLog;
#else
    (void)root; (void)fixture;
    SKIP_LINUX("the loop is staged with a plugin that ends its own process (TerminateProcess)");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--child") == 0) { return childMain(argv[2], argv[3]); }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: test_plugin_hostile_loop <hostile fixture module>\n");
        return 2;
    }
    const std::string fixture = argv[1];
    const fs::path self = fs::absolute(argv[0]);
    const fs::path root = fs::temp_directory_path() / ("foxsdr-test-plugin-hostile-loop-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                                 ::GetCurrentProcessId()
#else
                                                                                                 1
#endif
                                                                                                     )));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    testDeaths(self, root, fixture);
    testLoop(root, fixture);

    fs::remove_all(root, ec);
    return testSummary("test_plugin_hostile_loop");
}
