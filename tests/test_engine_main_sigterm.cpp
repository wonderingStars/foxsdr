// A stop signal ends the headless engine in order (engine extraction stage 7d): `foxsdr-engine` run with no `--seconds`
// goes on until it is ended from outside, and what ends it from outside is, in a service's life, SIGTERM (`systemctl
// stop`), Ctrl+C (SIGINT), the terminal closing (SIGHUP) - and on Windows Ctrl+C, Ctrl+Break and a closing console.
//
// WHAT IS ASSERTED, on the real foxsdr-engine executable:
//   A. A PERSISTENT RUN, ASKED TO STOP (SIGTERM on POSIX; the console's Ctrl+Break on Windows - the handler answers
//      Ctrl+C, Ctrl+Break and a closing console alike): the settings file's marker is FALSE while it runs; after the
//      request the run says what it was asked and stops in the window's order, exits 0 within seconds (not at the
//      stop's 58 s bound), and leaves the marker TRUE - one launch, NO crash counted. The next start counts none either.
//   B. THE CONTRAST, in the same file so the pair reads together: a run that is KILLED leaves the marker FALSE, and
//      the next start counts the crash (tests/test_engine_main_clean_exit_marker.cpp holds the kill; this is the
//      pair of A).
//   C. A HERMETIC RUN (no settings file) stops the same way, exit 0, and leaves nothing on the disk.
//   D. POSIX ONLY: SIGINT and SIGHUP each stop a persistent run the same way as SIGTERM.
//   E. POSIX ONLY: A SIGNAL ASKED TWICE ENDS THE PROCESS: with the first one held by a stop that has been wedged in
//      the plugin unload (the bounded-run seam), the second is handed back to the previous disposition (the default
//      here: no sentinel is armed) and the process dies of that signal, at once - the operator who asked twice is
//      never held by a stop that will not finish. (The composition with the exit recorder's own handler is held by
//      tests/test_stop_signal.cpp.)
//
// Break-it (docs/engine-stage4.md 6.18): the wait loops in engine_main.cpp not reading the request (stopRequested()
// never true) -> A, C and D are RED (the run never ends: the test ends it after its wait, exit code not 0, no stop line).
//
// HONESTY ABOUT WHAT RAN. On Windows (the machine this was written on) A, B and C run with Ctrl+Break through a console
// the child shares with this process (tests/engine_main_proc.hpp). D and E, and the POSIX half of A-C, have never run
// on Linux or arm64 from this file; the stand-alone check of the signal module under WSL2 is tests/test_stop_signal.cpp.
//
// HERMETIC: every per-user folder is a scratch tree, the settings file a scratch path named on the command line, the
// volume seeded 0, the telemetry endpoint a closed local port. The real %APPDATA% config is never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

#if defined(_WIN32)
constexpr const char* kStopName = "a console event";
#else
constexpr const char* kStopName = "SIGTERM";
#endif

json readConfig(const fs::path& p) {
    const std::string text = emt::readFile(p);
    if (text.empty()) { return json(); }
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json();
}

std::optional<bool> marker(const fs::path& p) {
    const json j = readConfig(p);
    if (!j.is_object() || !j.contains("telemetryCleanExit")) { return std::nullopt; }
    return j["telemetryCleanExit"].get<bool>();
}

std::uint64_t counter(const fs::path& p, const char* key) {
    const json j = readConfig(p);
    return j.is_object() && j.contains(key) ? j[key].get<std::uint64_t>() : ~std::uint64_t{0};
}

bool waitMarker(const fs::path& p, bool want, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        const std::optional<bool> m = marker(p);
        if (m.has_value() && *m == want) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool waitErr(const fs::path& errFile, const std::string& needle, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (emt::contains(emt::readText(errFile), needle)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void seed(const fs::path& cfg) {
    fs::create_directories(cfg.parent_path());
    cascade::core::AppConfig s;  // a predecessor that ended cleanly (the default), silent
    s.volume = 0.0f;
    std::ofstream out(cfg, std::ios::binary);
    out << cascade::core::ConfigStore::serialize(s);
}

double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

// A persistent run with no `--seconds`, started so it can be asked to stop, and waited until it is up.
bool startUp(emt::Child& run, const fs::path& exe, const fs::path& cfg, const fs::path& cwd, const fs::path& outDir,
             const std::string& tag) {
    run.setConsoleGroup(true);
    if (!run.start(exe, {"--config", cfg.string()}, cwd, outDir / (tag + ".out"), outDir / (tag + ".err"))) { return false; }
    return waitMarker(cfg, false, 30000) &&
           waitErr(outDir / (tag + ".err"), "settings file in use (restored) (until ended from outside)", 30000);
}

}  // namespace

int main() {
    std::printf("test_engine_main_sigterm\n");
    const fs::path root = emt::scratch("engine-sigterm");
    const fs::path home = root / "home";
    const fs::path cwd = home / "cwd";
    fs::create_directories(home / "tmp");
    fs::create_directories(cwd);
    emt::isolate(home);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(home.string(), 0) == 0);

    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const fs::path cfg = home / "cx" / "config.json";
    seed(cfg);
    CHECK(marker(cfg).value_or(false) == true);
    CHECK(counter(cfg, "telemetryLaunches") == 0 && counter(cfg, "telemetryCrashes") == 0);
    const fs::path outDir = root / "out";
    fs::create_directories(outDir);

    // --- A. a persistent run is asked to stop: exit 0 in order, the marker TRUE, no crash --------------------------
    {
        emt::Child run;
        CHECK(startUp(run, engineExe, cfg, cwd, outDir, "a"));
        CHECK(!run.waitExit(0));
        CHECK(marker(cfg).value_or(true) == false);  // alive: FALSE on disk
        const auto asked = std::chrono::steady_clock::now();
        CHECK(run.requestStop());
        const bool ended = run.waitExit(20000);
        const double took = since(asked);
        CHECK(ended);
        std::printf("  A: asked, the run ended %s in %.2f s with exit code %d\n", ended ? "by itself" : "NOT", took, run.code());
        CHECK(run.code() == 0);
        // An orderly stop is a second or three; the stop's own bound is 58 s. Twenty is the test's patience, the
        // ordinary figure is far under it.
        CHECK(took < 15.0);
        const std::string err = emt::readText(outDir / "a.err");
        std::printf("  A: stderr:\n%s", err.c_str());
        CHECK(emt::contains(err, std::string("stop requested (") + kStopName + "), stopping"));
        CHECK(emt::contains(err, "foxsdr-engine: stopped after "));
        CHECK(!emt::contains(err, "did not finish within its budget"));
        CHECK(emt::contains(err, "settings saved "));
        CHECK(marker(cfg).value_or(false) == true);
        CHECK(counter(cfg, "telemetryLaunches") == 1);
        CHECK(counter(cfg, "telemetryCrashes") == 0);
        // The next start counts nothing: a signal-driven stop is a clean exit, not a death.
        const emt::RunResult next = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "a2", 120000);
        CHECK(next.started && !next.timedOut && next.exitCode == 0);
        CHECK(counter(cfg, "telemetryCrashes") == 0);
        CHECK(counter(cfg, "telemetryLaunches") == 2);
        CHECK(marker(cfg).value_or(false) == true);
    }

    // --- B. the contrast: a KILLED run leaves FALSE, and the next start counts it ---------------------------------
    {
        emt::Child run;
        CHECK(startUp(run, engineExe, cfg, cwd, outDir, "b"));
        run.kill();
        CHECK(marker(cfg).value_or(true) == false);
        CHECK(counter(cfg, "telemetryCrashes") == 0);  // the NEXT start counts a death
        const emt::RunResult next = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "b2", 120000);
        CHECK(next.started && !next.timedOut && next.exitCode == 0);
        CHECK(counter(cfg, "telemetryCrashes") == 1);
        CHECK(counter(cfg, "telemetryLaunches") == 4);  // A's two, B's, the next
        CHECK(marker(cfg).value_or(false) == true);
    }

    // --- C. a hermetic run (no settings file) stops the same way and leaves nothing --------------------------------
    {
        const std::vector<std::string> before = emt::tree(home);
        emt::Child run;
        run.setConsoleGroup(true);
        CHECK(run.start(engineExe, {}, cwd, outDir / "c.out", outDir / "c.err"));
        CHECK(waitErr(outDir / "c.err", "running, signal generator, muted, no config (until ended from outside)", 30000));
        CHECK(!run.waitExit(0));
        const auto asked = std::chrono::steady_clock::now();
        CHECK(run.requestStop());
        const bool ended = run.waitExit(20000);
        CHECK(ended);
        CHECK(since(asked) < 15.0);
        CHECK(run.code() == 0);
        const std::string err = emt::readText(outDir / "c.err");
        CHECK(emt::contains(err, std::string("stop requested (") + kStopName + "), stopping"));
        CHECK(emt::contains(err, "foxsdr-engine: stopped after "));
        CHECK(emt::tree(home) == before);  // nothing written anywhere under the profile (out/ is beside it, not in it)
        CHECK(emt::readText(outDir / "c.out").empty());
    }

#if !defined(_WIN32)
    // --- D. SIGINT and SIGHUP stop a persistent run as SIGTERM does (POSIX) -----------------------------------------
    for (const auto& [sig, name] : {std::pair<int, const char*>{SIGINT, "SIGINT"}, std::pair<int, const char*>{SIGHUP, "SIGHUP"}}) {
        const fs::path cfgD = home / (std::string("cd-") + name) / "config.json";
        seed(cfgD);
        const std::string tag = std::string("d-") + name;
        emt::Child run;
        CHECK(startUp(run, engineExe, cfgD, cwd, outDir, tag));
        CHECK(run.requestStop(sig));
        CHECK(run.waitExit(20000));
        CHECK(run.code() == 0);
        const std::string err = emt::readText(outDir / (tag + ".err"));
        CHECK(emt::contains(err, std::string("stop requested (") + name + "), stopping"));
        CHECK(marker(cfgD).value_or(false) == true);
        CHECK(counter(cfgD, "telemetryCrashes") == 0);
    }

    // --- E. a signal asked twice ends the process, wedged stop or not (POSIX) --------------------------------------
    {
        const fs::path cfgE = home / "ce" / "config.json";
        seed(cfgE);
        emt::setEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS", "120000");
        emt::Child run;
        const bool started = run.start(engineExe, {"--config", cfgE.string(), "--seconds", "300"}, cwd, outDir / "e.out", outDir / "e.err");
        emt::unsetEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS");
        CHECK(started);
        CHECK(waitErr(outDir / "e.err", "settings file in use (restored)", 30000));
        CHECK(run.requestStop(SIGTERM));
        CHECK(waitErr(outDir / "e.err", "--diag-unload-stall wedging the plugin unload", 30000));
        CHECK(!run.waitExit(0));  // held in the stop
        const auto second = std::chrono::steady_clock::now();
        CHECK(run.requestStop(SIGTERM));
        CHECK(run.waitExit(5000));
        CHECK(since(second) < 5.0);
        CHECK(run.code() == 128 + SIGTERM);  // died of the signal, as the default does
        CHECK(marker(cfgE).value_or(true) == false);
    }
#else
    std::printf("  D, E: POSIX only (SIGINT, SIGHUP, a second SIGTERM) - not run on Windows\n");
#endif

    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_engine_main_sigterm");
}
