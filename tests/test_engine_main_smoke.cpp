// foxsdr-engine runs, stops cleanly, and writes NO file (engine extraction stage 7b).
//
// THE HERMETIC RULE. Until stage 7c gives the engine a life-cycle and a config, the
// engine binary is a hermetic program: it reads no config, writes no config, and puts
// no log, crash directory or recording anywhere - its only output is stderr. The rule is
// held where it could most easily be broken: a bare `foxsdr-engine` with NO arguments.
// For the window application that is "a real session" (src/engine/process_boot.cpp,
// diagnosticsOnDisk): a log file, a crashes directory, the sentinel's directory and the
// user's config read ahead of the window. The Engine kind has none of that, so the test
// runs the bare command - which runs until it is ended from outside - and ENDS it, then
// looks at the machine; the `--seconds` run is the clean one.
//
// What is asserted, on the real executable, in a profile of its own:
//   1. `--seconds 1`: exit 0 after about a second (not at once - it ran the engine - and
//      not for ever), a first stderr line saying it is running a muted generator with no
//      config, a last one saying it stopped with a count of engine passes that shows the
//      engine's own thread really ran (a free-running pass is 16 ms: dozens in a second),
//      nothing on stdout;
//   2. the bare command stays up until the test ends it (it is a service: it does not
//      exit by itself), says so on stderr, and has no window module mapped (the process
//      this test looks into, not the library);
//   3. after BOTH runs the whole isolated profile - every per-user folder the program
//      consults, its temp directory and its working directory are one tree - is exactly
//      what the test made before them: no new file, no new directory, no file that grew
//      (the profile includes a config the engine must not read or write, which stays
//      byte for byte; and the real profile is not the one in use, which is checked).
//
// RUN_SERIAL is not needed: nothing here depends on another process's timing beyond
// generous bounds.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

namespace {

// "(N passes)" from the stopped line, or -1.
long passesIn(const std::string& err) {
    const std::size_t at = err.find("s (");
    if (at == std::string::npos || err.find("stopped after") == std::string::npos) { return -1; }
    return std::strtol(err.c_str() + at + 3, nullptr, 10);
}

}  // namespace

int main() {
    std::printf("test_engine_main_smoke\n");
    const fs::path root = emt::scratch("engine-smoke");
    const fs::path home = root / "home";
    const fs::path cwd = home / "cwd";
    fs::create_directories(home / "tmp");
    fs::create_directories(cwd);
    emt::isolate(home);

    // The profile in use is the scratch one: where the config would be is under it, so the real
    // %APPDATA%\foxsdr\config.json is out of reach of anything that asks the environment.
    const std::string configPath = cascade::core::ConfigStore::defaultPath();
    CHECK(configPath.rfind(home.string(), 0) == 0);
    // A config the engine must not read or rewrite (nothing in it is a reason to: it is simply there).
    fs::create_directories(fs::path(configPath).parent_path());
    const std::string seeded = "{\n  \"schemaVersion\": 1,\n  \"diagnosticsEnabled\": true\n}\n";
    {
        std::ofstream out(configPath, std::ios::binary);
        out << seeded;
    }
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const std::vector<std::string> before = emt::tree(home);
    CHECK(before.size() >= 4);  // tmp, cwd, the config's folder and the config

    // --- 1. the bounded run -------------------------------------------------------------------------------
    const emt::RunResult bounded = emt::run(engineExe, {"--seconds", "1"}, cwd, root / "out", "bounded", 120000);
    std::printf("  --seconds 1: exit %d after %.2f s\n%s", bounded.exitCode, bounded.seconds, bounded.err.c_str());
    CHECK(bounded.started && !bounded.timedOut);
    CHECK(bounded.exitCode == 0);
    CHECK(bounded.seconds >= 1.0);   // it ran the engine for the second it was asked for
    CHECK(bounded.seconds < 60.0);   // and ended, however slow the machine
    CHECK(emt::contains(bounded.err, ": running, signal generator, muted, no config\n"));
    CHECK(!emt::contains(bounded.err, "until ended from outside"));
    CHECK(emt::contains(bounded.err, "foxsdr-engine: stopped after "));
    CHECK(passesIn(bounded.err) >= 5);  // the engine's own thread passed (about 60 at a 16 ms period)
    CHECK(bounded.out.empty());          // stdout is for the modes that print
    CHECK(!emt::contains(bounded.err, "failed"));

    // --- 2. the bare command: a service that is up and stays up --------------------------------------------
    {
        emt::Child bare;
        CHECK(bare.start(engineExe, {}, cwd, root / "out" / "bare.out", root / "out" / "bare.err"));
        // Up: it has not exited on its own after a few seconds (startup alone is over a second), and it is
        // saying what it is on stderr.
        const bool exitedByItself = bare.waitExit(3500);
        CHECK(!exitedByItself);
        const std::string err = emt::readText(root / "out" / "bare.err");
        std::printf("  bare run, still up after 3.5 s: stderr '%s'\n", err.c_str());
        CHECK(emt::contains(err, ": running, signal generator, muted, no config (until ended from outside)\n"));
        // A headless process: no window library is mapped into it (the module list is the process's own).
        const std::vector<std::string> mods = bare.modules();
        CHECK(!mods.empty());
        bool sawSelf = false;
        for (const std::string& m : mods) {
            if (m.find("foxsdr-engine") != std::string::npos) { sawSelf = true; }
            CHECK(m.find("opengl32") == std::string::npos);
            CHECK(m.find("glfw") == std::string::npos);
            CHECK(m.find("imgui") == std::string::npos);
        }
        CHECK(sawSelf);  // the list was really the engine's (the check above could not pass on an empty list)
        // ENDED FROM OUTSIDE: no signal path until stage 7d, so it is a kill, and the test's exit code is not the engine's.
        bare.kill();
        CHECK(bare.killed());
    }

    // --- 3. the machine, as the test left it ---------------------------------------------------------------
    const std::vector<std::string> after = emt::tree(home);
    if (after != before) {
        std::printf("  the tree changed:\n");
        for (const std::string& f : after) { std::printf("    after:  %s\n", f.c_str()); }
        for (const std::string& f : before) { std::printf("    before: %s\n", f.c_str()); }
    }
    CHECK(after == before);
    CHECK(emt::readFile(configPath) == seeded);  // the config the engine must not touch, byte for byte

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_smoke");
}
