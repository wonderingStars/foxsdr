// One program at a time may use a settings file (engine extraction stage 7c): the lock beside it, held to the
// letter, on the real executables and on the lock itself.
//
// WHY. The headless engine shares the window application's ONE settings file by design (the survey's decision
// O3), and two programs saving one file lose each other's changes - the last writer wins, and an engine beside an
// open window would put its snapshot of the window's own fields back over what the window just changed. So the
// engine takes a lock beside the file (core/config_lock.hpp) before it reads it, and a second program that finds
// it taken is refused with a sentence that says which program is in the way.
//
// WHAT IS ASSERTED:
//   A. THE LOCK ITSELF (in this process): acquire holds; a second acquire on the same file is Contended and names
//      the holder (role, pid); two spellings of one file (a relative path, a `..` detour) name ONE lock; another file
//      is another lock; release lets the next take it; a folder that cannot take the file is Unavailable (and says
//      why);
//   B. ENGINE AGAINST ENGINE: a foxsdr-engine running on a file; a second on the same file exits 1 with the sentence
//      ("not started", "another foxsdr-engine", the first's process id, the file's path), says nothing of running,
//      starts no engine (it is over in a moment and its stdout is empty), and leaves the first run alone (it ends
//      0 with its marker written) - the refusal wrote nothing to the file; a relative spelling of the same file is
//      refused the same way; and a DIFFERENT file is not: both run;
//   C. ENGINE AGAINST THE WINDOW APPLICATION: this process takes the lock as the host does (role "cascade", the call
//      src/main.cpp makes); the engine on the file exits 1 naming "the FoxSDR window application" and THIS process's
//      id, without a line of "running"; when the "window" lets go, the engine runs;
//   D. NO STALE LOCK: an engine that is KILLED while it holds the lock (End task, a crash) leaves a lock file behind and
//      no lock: the next engine on the file starts and runs (the lock is the operating system's, not the file's
//      existence);
//   E. THE HOST'S HALF, from the text of src/main.cpp (a real window cannot be opened by a test): the window application
//      takes the lock through its engine's service (best effort, role "cascade" - shell retirement R7: the service is
//      main's, the lock is the service's), only in a real session (frames < 0; the service holds nothing without a
//      config path), BEFORE the window object exists, and its failure to get it is a log line and never an exit; and the real `cascade --frames 3` on a
//      CASCADE_CONFIG_TEST file - a bounded run - takes none and leaves no ".lock" beside it (a test stays hermetic).
//
// Break-it (docs/engine-stage4.md 6.17): EngineService::acquireConfig returns true without taking the lock -> B and C
// are RED (the second engine runs); ConfigLock::acquire on Windows opens with FILE_SHARE_WRITE -> A and B RED.
//
// HERMETIC: scratch profile, scratch settings files named on the command line, volumes seeded 0 (silent), the real
// %APPDATA% config never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/config_lock.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using cascade::core::ConfigLock;

namespace {

// A settings file the engine restores silently (volume 0), as a predecessor that ended cleanly left it.
void seed(const fs::path& cfg) {
    fs::create_directories(cfg.parent_path());
    cascade::core::AppConfig c;
    c.volume = 0.0f;
    std::ofstream out(cfg, std::ios::binary);
    out << cascade::core::ConfigStore::serialize(c);
}

bool waitErr(const fs::path& errFile, const std::string& needle, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (emt::contains(emt::readText(errFile), needle)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

unsigned long thisPid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

}  // namespace

int main() {
    std::printf("test_engine_main_second_instance_refused\n");
    const fs::path root = emt::scratch("engine-lock");
    const fs::path home = root / "home";
    const fs::path cwd = home / "cwd";
    fs::create_directories(home / "tmp");
    fs::create_directories(cwd);
    emt::isolate(home);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(home.string(), 0) == 0);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const fs::path outDir = root / "out";
    fs::create_directories(outDir);

    // --- A. the lock itself -----------------------------------------------------------------------------------------
    {
        const fs::path dir = home / "la";
        fs::create_directories(dir);
        const std::string path = (dir / "config.json").string();
        ConfigLock a;
        CHECK(a.acquire(path, "foxsdr-engine") == ConfigLock::State::Held);
        CHECK(a.state() == ConfigLock::State::Held);
        CHECK(a.lockPath() == ConfigLock::lockPathFor(path));
        CHECK(a.lockPath().size() > 5 && a.lockPath().compare(a.lockPath().size() - 5, 5, ".lock") == 0);
        CHECK(fs::exists(a.lockPath()));
        CHECK(a.acquire(path, "foxsdr-engine") == ConfigLock::State::Held);  // idempotent while held

        ConfigLock b;
        CHECK(b.acquire(path, "cascade") == ConfigLock::State::Contended);
        CHECK(b.holder().role == "foxsdr-engine");
        CHECK(b.holder().pid == thisPid());
        // Two spellings of one file are one lock: a `..` detour, and (when the file is in the working
        // directory) the bare name.
        ConfigLock c;
        const std::string detour = (dir / ".." / "la" / "config.json").string();
        CHECK(c.acquire(detour, "cascade") == ConfigLock::State::Contended);
        CHECK(ConfigLock::lockPathFor(detour) == ConfigLock::lockPathFor(path));
        // Another file is another lock.
        ConfigLock other;
        CHECK(other.acquire((dir / "other.json").string(), "cascade") == ConfigLock::State::Held);
        // Let go: the next one takes it.
        a.release();
        CHECK(a.state() == ConfigLock::State::None);
        CHECK(b.acquire(path, "cascade") == ConfigLock::State::Held);
        ConfigLock d;
        CHECK(d.acquire(path, "foxsdr-engine") == ConfigLock::State::Contended);
        CHECK(d.holder().role == "cascade");
        // A folder that cannot take the file: a FILE stands where the folder would be.
        const fs::path squatter = home / "squatter";
        { std::ofstream out(squatter, std::ios::binary); out << "x"; }
        ConfigLock u;
        CHECK(u.acquire((squatter / "config.json").string(), "foxsdr-engine") == ConfigLock::State::Unavailable);
        CHECK(!u.error().empty());
    }

    // --- B. engine against engine ----------------------------------------------------------------------------------
    {
        const fs::path cfg = home / "lb" / "config.json";
        seed(cfg);
        emt::Child first;
        CHECK(first.start(engineExe, {"--config", cfg.string(), "--seconds", "20"}, cwd, outDir / "b1.out", outDir / "b1.err"));
        CHECK(waitErr(outDir / "b1.err", "settings file in use (restored)", 60000));
        CHECK(!first.waitExit(0));

        const emt::RunResult second = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "b2", 120000);
        std::printf("  B: second engine: exit %d after %.2f s\n%s", second.exitCode, second.seconds, second.err.c_str());
        CHECK(second.started && !second.timedOut);
        CHECK(second.exitCode == 1);
        CHECK(emt::contains(second.err, "foxsdr-engine: not started - another foxsdr-engine (process " + std::to_string(first.pid()) + ")"));
        CHECK(emt::contains(second.err, "is already using the settings file"));
        CHECK(emt::contains(second.err, cfg.string()));
        CHECK(emt::contains(second.err, "--config PATH"));
        CHECK(!emt::contains(second.err, "running"));
        CHECK(!emt::contains(second.err, "stopped after"));
        CHECK(second.out.empty());
        CHECK(second.seconds < 30.0);  // refused at once: no engine was started to wait for

        // The same file spelled relative to the working directory: the same lock.
        const emt::RunResult relative =
            emt::run(engineExe, {"--config", "../lb/config.json", "--seconds", "1"}, home / "cwd", outDir, "b3", 120000);
        CHECK(relative.started && !relative.timedOut && relative.exitCode == 1);
        CHECK(emt::contains(relative.err, "another foxsdr-engine"));

        // ...and a different file is not in anyone's way: both run at once.
        const fs::path other = home / "lb2" / "config.json";
        seed(other);
        const emt::RunResult parallel = emt::run(engineExe, {"--config", other.string(), "--seconds", "1"}, cwd, outDir, "b4", 120000);
        CHECK(parallel.started && !parallel.timedOut && parallel.exitCode == 0);
        CHECK(emt::contains(parallel.err, "settings file in use (restored)"));

        // The first run was not disturbed by any of it: it ends 0 and its marker is on disk.
        CHECK(first.waitExit(120000));
        CHECK(first.code() == 0);
        const std::string after = emt::readFile(cfg);
        CHECK(emt::contains(after, "\"telemetryCleanExit\": true"));
        CHECK(emt::contains(after, "\"telemetryLaunches\": 1"));  // one launch: the refused ones never started
        CHECK(!emt::contains(emt::readText(outDir / "b1.err"), "not started"));
    }

    // --- C. engine against the window application -----------------------------------------------------------------
    {
        const fs::path cfg = home / "lc" / "config.json";
        seed(cfg);
        ConfigLock window;  // what src/main.cpp does for a real session
        CHECK(window.acquire(cfg.string(), "cascade") == ConfigLock::State::Held);
        const emt::RunResult r = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "c1", 120000);
        std::printf("  C: engine beside the window: exit %d\n%s", r.exitCode, r.err.c_str());
        CHECK(r.started && !r.timedOut && r.exitCode == 1);
        CHECK(emt::contains(r.err, "not started - the FoxSDR window application (process " + std::to_string(thisPid()) + ")"));
        CHECK(!emt::contains(r.err, "running"));
        // The refused engine did not touch the file: still the seed, no clean-exit rewrite, no launch counted.
        CHECK(emt::contains(emt::readFile(cfg), "\"telemetryLaunches\": 0"));
        window.release();
        const emt::RunResult ok = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "c2", 120000);
        CHECK(ok.started && !ok.timedOut && ok.exitCode == 0);
    }

    // --- D. a killed holder leaves no lock ----------------------------------------------------------------------------
    {
        const fs::path cfg = home / "ld" / "config.json";
        seed(cfg);
        {
            emt::Child holder;
            CHECK(holder.start(engineExe, {"--config", cfg.string()}, cwd, outDir / "d1.out", outDir / "d1.err"));
            CHECK(waitErr(outDir / "d1.err", "settings file in use (restored)", 60000));
            const emt::RunResult refused = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "d2", 120000);
            CHECK(refused.exitCode == 1);  // while it lives, the lock is held
            holder.kill();
        }
        CHECK(fs::exists(ConfigLock::lockPathFor(cfg.string())));  // the file is left behind...
        const emt::RunResult next = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "d3", 120000);
        std::printf("  D: after the holder was killed: exit %d\n", next.exitCode);
        CHECK(next.started && !next.timedOut && next.exitCode == 0);  // ...and holds nothing
        CHECK(emt::contains(next.err, "settings file in use (restored)"));
    }

    // --- E. the host's half --------------------------------------------------------------------------------------------
    {
#ifdef CASCADE_SOURCE_DIR
        const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
#else
        const fs::path src = fs::current_path() / "src";
#endif
        std::string mainText = emt::readFile(src / "main.cpp");
        CHECK(!mainText.empty());
        // The text with comments blanked would be better; the lines below are ones a comment cannot be mistaken for.
        // SINCE SHELL RETIREMENT R7 the lock is the engine's SERVICE's (EngineService::acquireConfig, role = the service's label, "cascade" for the window
        // application, policy BestEffort: carries on and logs the line main.cpp has always logged), and main builds the service before the window exists, so
        // it is released after the window is gone (the service is declared first) and, being a member of the service, before the software-GL relaunch.
        const std::size_t policyAt = mainText.find("options.lock = cascade::engine::ServiceLock::BestEffort;");
        const std::size_t labelAt = mainText.find("options.label = \"cascade\";");
        const std::size_t declAt = mainText.find("cascade::engine::EngineService service(");
        const std::size_t lockAt = mainText.find("service.acquireConfig(");
        const std::size_t windowAt = mainText.find("cascade::gui::AppWindow app(");
        CHECK(policyAt != std::string::npos && labelAt != std::string::npos && declAt != std::string::npos && lockAt != std::string::npos &&
              windowAt != std::string::npos);
        if (lockAt == std::string::npos || windowAt == std::string::npos || declAt == std::string::npos) { return testSummary("test_engine_main_second_instance_refused"); }
        CHECK(policyAt < declAt && labelAt < declAt);   // the policy and the role are chosen before the service is built
        CHECK(declAt < lockAt && lockAt < windowAt);    // before the window exists, the service declared before it so released after it
        const std::size_t gate = mainText.rfind("if (boot.frames < 0)", lockAt);
        CHECK(gate != std::string::npos && lockAt - gate < 100);  // only a real session (the service itself holds nothing when it has no settings file)
        // Never a refusal: the answer is not read, and nothing between the attempt and the window object returns or exits.
        CHECK(lockAt >= 6 && mainText.substr(lockAt - 6, 6) == "(void)");
        const std::string between = mainText.substr(lockAt, windowAt - lockAt);
        CHECK(between.find("return") == std::string::npos);
        CHECK(between.find("exit(") == std::string::npos);

        // A bounded run of the REAL window application on a CASCADE_CONFIG_TEST file takes no lock and leaves no file.
        const fs::path cfg = home / "le" / "config.json";
        seed(cfg);
        emt::setEnv("CASCADE_CONFIG_TEST", cfg.string());
        const emt::RunResult host = emt::run(emt::appExe("cascade"), {"--frames", "3"}, cwd, outDir, "e1", 120000);
        emt::unsetEnv("CASCADE_CONFIG_TEST");
        std::printf("  E: cascade --frames 3: exit %d\n", host.exitCode);
        CHECK(host.started && !host.timedOut && host.exitCode == 0);
        CHECK(!fs::exists(ConfigLock::lockPathFor(cfg.string())));
    }

    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_engine_main_second_instance_refused");
}
