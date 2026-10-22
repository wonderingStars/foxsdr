// A WEDGED STOP IS ENDED BY ITS BUDGET (engine extraction stage 7d, finding F5 of stage 7c): the headless engine's
// stop - however it was asked for - runs under the window teardown's own budget (HangWatchdog::kShutdownThresholdMs,
// 53 s) and, if it has not returned a short grace later, the process is ended with exit code 3, after one line on
// stderr. The window's watchdog never ends anything; a service is the other way round (engine/shutdown_guard.hpp).
//
// THE WEDGE is the 7c seam: FOXSDR_ENGINE_UNLOAD_STALL_MS, honoured only with `--seconds`, sleeps right after the
// plugins are unloaded and before the clean-exit marker - where a plugin's destroy() runs - and says so on stderr, so
// the test knows the process is inside it. A bounded run's second seam, FOXSDR_ENGINE_SHUTDOWN_BUDGET_MS (also only with
// `--seconds`), shrinks the budget so most of the cases below take eight seconds instead of fifty-eight.
//
// WHAT IS ASSERTED, on the real foxsdr-engine executable:
//   W1. A STOP ASKED BY A SIGNAL (Ctrl+Break on Windows, SIGTERM on POSIX), wedged: the final-state save is on disk
//       and the marker is FALSE while it is wedged; the process is ended by the budget (3 s + the 5 s grace, not the
//       120 s the wedge asks for), exit code 3, the line on stderr; the marker is still FALSE; the NEXT START COUNTS
//       THE CRASH (crashes 0 -> 1) and ends clean.
//   W2. THE `--seconds` STOP, wedged: the same, exit code 3 (armed for the bounded stop as for the signal-driven one).
//   W3. A HERMETIC RUN's stop, wedged: the same (a guard on every stop, not only a persistent one's).
//   W4. THE POSITIVE CONTROL: a stall SHORTER than the budget (1.5 s against 3 s) is waited out - exit 0, the marker
//       TRUE, no budget line - so a guard that simply ended every stop is RED too.
//   W5. A HANG REPORT, when a report may be written (FOXSDR_DIAG_DIR names a directory): the watchdog's capture, with the
//       shutdown budget, is on disk when the process is ended.
//   W6. THE PRODUCTION BUDGET, no seam: the stop wedged for 120 s is ended at HangWatchdog::kShutdownThresholdMs +
//       the grace (58 s), measured from the stall's start (between 55 and 70 s) - the number the window's teardown
//       is held to is the one in force.
//
// Break-it (docs/engine-stage4.md 6.18): the guard not armed (ShutdownGuard::arm() returning at once) -> W1, W2, W3, W5, W6
// are RED (the run is wedged for its whole 120 s; the test ends it after its wait: no exit code 3, no budget line).
//
// HONESTY ABOUT WHAT RAN: Windows only so far (Ctrl+Break through a shared console, TerminateProcess to end); the
// POSIX ending (_exit after writing the exit record) has never run here.
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
#include "core/hang_watchdog.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

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

constexpr const char* kStallMsg = "--diag-unload-stall wedging the plugin unload";
constexpr const char* kBudgetMsg = "the shutdown did not finish within its budget of ";

// The seams for one child, set before the spawn and cleared at once after (the child inherits the environment).
struct Seams {
    Seams(int stallMs, int budgetMs, const std::string& diagDir = std::string()) {
        emt::setEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS", std::to_string(stallMs));
        if (budgetMs > 0) { emt::setEnv("FOXSDR_ENGINE_SHUTDOWN_BUDGET_MS", std::to_string(budgetMs)); }
        if (!diagDir.empty()) { emt::setEnv("FOXSDR_DIAG_DIR", diagDir); }
    }
    ~Seams() {
        emt::unsetEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS");
        emt::unsetEnv("FOXSDR_ENGINE_SHUTDOWN_BUDGET_MS");
        emt::unsetEnv("FOXSDR_DIAG_DIR");
    }
};

}  // namespace

int main() {
    std::printf("test_engine_main_sigterm_wedged\n");
    const fs::path root = emt::scratch("engine-wedged");
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
    const fs::path outDir = root / "out";
    fs::create_directories(outDir);
    constexpr int kBudgetMs = 3000;
    constexpr double kGraceS = 5.0;  // ShutdownGuard::kEndGraceMs

    // --- W1. a SIGNAL-driven stop, wedged: ended by the budget, marker FALSE, the next start counts it --------------
    {
        CHECK(counter(cfg, "telemetryLaunches") == 0 && counter(cfg, "telemetryCrashes") == 0);
        emt::Child run;
        run.setConsoleGroup(true);
        bool started = false;
        {
            const Seams seams(120000, kBudgetMs);
            started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "300"}, cwd, outDir / "w1.out", outDir / "w1.err");
        }
        CHECK(started);
        CHECK(waitErr(outDir / "w1.err", "settings file in use (restored)", 30000));
        CHECK(!run.waitExit(0));
        CHECK(run.requestStop());
        CHECK(waitErr(outDir / "w1.err", "stop requested (", 30000));
        CHECK(waitErr(outDir / "w1.err", kStallMsg, 30000));  // inside the stop, where a plugin's destroy() runs
        const auto wedgedAt = std::chrono::steady_clock::now();
        CHECK(!run.waitExit(0));
        // WHAT THE NEXT START WOULD READ while it is wedged: this run's launch counted, the marker not there.
        CHECK(counter(cfg, "telemetryLaunches") == 1);
        const std::optional<bool> whileWedged = marker(cfg);
        CHECK(whileWedged.has_value() && *whileWedged == false);
        const bool ended = run.waitExit(40000);
        const double took = since(wedgedAt);
        std::printf("  W1: wedged for 120 s asked; ended %s %.2f s after it began (budget %d ms + grace %.0f s), exit code %d\n",
                    ended ? "by the budget" : "NOT", took, kBudgetMs, kGraceS, run.code());
        CHECK(ended);
        CHECK(run.code() == 3);
        CHECK(took > kBudgetMs / 1000.0 + kGraceS - 1.0);  // not before the budget and its grace
        CHECK(took < 25.0);                                // and nowhere near the 120 s the wedge asked for
        const std::string err = emt::readText(outDir / "w1.err");
        std::printf("  W1: stderr:\n%s", err.c_str());
        CHECK(emt::contains(err, std::string(kBudgetMsg) + "3000 ms - ending the process with exit code 3"));
        CHECK(!emt::contains(err, "foxsdr-engine: stopped after "));  // it never got to the end of its stop
        const std::optional<bool> afterEnd = marker(cfg);
        CHECK(afterEnd.has_value() && *afterEnd == false);
        CHECK(counter(cfg, "telemetryCrashes") == 0);  // counted by the NEXT start
        const emt::RunResult next = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "w1n", 120000);
        CHECK(next.started && !next.timedOut && next.exitCode == 0);
        CHECK(counter(cfg, "telemetryCrashes") == 1);
        CHECK(counter(cfg, "telemetryLaunches") == 2);
        CHECK(marker(cfg).value_or(false) == true);
    }

    // --- W2. the `--seconds` stop, wedged: the same -----------------------------------------------------------------
    {
        emt::Child run;
        bool started = false;
        {
            const Seams seams(120000, kBudgetMs);
            started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir / "w2.out", outDir / "w2.err");
        }
        CHECK(started);
        CHECK(waitErr(outDir / "w2.err", kStallMsg, 60000));
        const auto wedgedAt = std::chrono::steady_clock::now();
        const bool ended = run.waitExit(40000);
        const double took = since(wedgedAt);
        std::printf("  W2: ended %s %.2f s after the wedge began, exit code %d\n", ended ? "by the budget" : "NOT", took, run.code());
        CHECK(ended);
        CHECK(run.code() == 3);
        CHECK(took > kBudgetMs / 1000.0 + kGraceS - 1.0 && took < 25.0);
        CHECK(emt::contains(emt::readText(outDir / "w2.err"), std::string(kBudgetMsg) + "3000 ms"));
        CHECK(marker(cfg).value_or(true) == false);
        CHECK(counter(cfg, "telemetryCrashes") == 1);  // W1's, counted by this run's start; this run's is for the next
        // leave the file whole and clean for what follows
        const emt::RunResult next = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir, "w2n", 120000);
        CHECK(next.started && !next.timedOut && next.exitCode == 0);
        CHECK(counter(cfg, "telemetryCrashes") == 2);
    }

    // --- W3. a hermetic run's stop, wedged: the same ----------------------------------------------------------------
    {
        emt::Child run;
        bool started = false;
        {
            const Seams seams(120000, kBudgetMs);
            started = run.start(engineExe, {"--seconds", "1"}, cwd, outDir / "w3.out", outDir / "w3.err");
        }
        CHECK(started);
        CHECK(waitErr(outDir / "w3.err", kStallMsg, 60000));
        const auto wedgedAt = std::chrono::steady_clock::now();
        const bool ended = run.waitExit(40000);
        const double took = since(wedgedAt);
        CHECK(ended);
        CHECK(run.code() == 3);
        CHECK(took > kBudgetMs / 1000.0 + kGraceS - 1.0 && took < 25.0);
        CHECK(emt::contains(emt::readText(outDir / "w3.err"), std::string(kBudgetMsg) + "3000 ms"));
    }

    // --- W4. the positive control: a stall shorter than the budget is waited out ------------------------------------
    {
        emt::Child run;
        bool started = false;
        {
            const Seams seams(1500, kBudgetMs);
            started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir / "w4.out", outDir / "w4.err");
        }
        CHECK(started);
        CHECK(run.waitExit(60000));
        CHECK(run.code() == 0);
        const std::string err = emt::readText(outDir / "w4.err");
        CHECK(emt::contains(err, kStallMsg));
        CHECK(emt::contains(err, "foxsdr-engine: stopped after "));
        CHECK(!emt::contains(err, kBudgetMsg));
        CHECK(marker(cfg).value_or(false) == true);
    }

    // --- W5. a hang report, when one may be written ------------------------------------------------------------------
    {
        const fs::path diag = root / "diag";
        fs::create_directories(diag);
        emt::Child run;
        bool started = false;
        {
            const Seams seams(120000, kBudgetMs, diag.string());
            started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir / "w5.out", outDir / "w5.err");
        }
        CHECK(started);
        CHECK(waitErr(outDir / "w5.err", kStallMsg, 60000));
        const bool ended = run.waitExit(40000);
        CHECK(ended);
        CHECK(run.code() == 3);
        int reports = 0;
        std::string names;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(diag, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            names += " " + n;
            if (n.rfind("hang-", 0) == 0) { ++reports; }
        }
        std::printf("  W5: files under the diagnostics directory:%s\n", names.c_str());
        CHECK(reports >= 1);
    }

    // --- W6. the production budget, no seam --------------------------------------------------------------------------
    {
        emt::Child run;
        bool started = false;
        {
            const Seams seams(120000, 0);  // no budget override: the constant in force
            started = run.start(engineExe, {"--config", cfg.string(), "--seconds", "1"}, cwd, outDir / "w6.out", outDir / "w6.err");
        }
        CHECK(started);
        CHECK(waitErr(outDir / "w6.err", kStallMsg, 60000));
        const auto wedgedAt = std::chrono::steady_clock::now();
        const bool ended = run.waitExit(100000);
        const double took = since(wedgedAt);
        const double expect = cascade::core::HangWatchdog::kShutdownThresholdMs / 1000.0 + kGraceS;
        std::printf("  W6: ended %s %.2f s after the wedge began (kShutdownThresholdMs %u ms + grace %.0f s = %.0f s), exit code %d\n",
                    ended ? "by the budget" : "NOT", took, cascade::core::HangWatchdog::kShutdownThresholdMs, kGraceS, expect,
                    run.code());
        CHECK(ended);
        CHECK(run.code() == 3);
        CHECK(took > expect - 3.0 && took < expect + 12.0);
        CHECK(emt::contains(emt::readText(outDir / "w6.err"),
                            std::string(kBudgetMsg) + std::to_string(cascade::core::HangWatchdog::kShutdownThresholdMs) + " ms"));
        CHECK(marker(cfg).value_or(true) == false);
    }

    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_engine_main_sigterm_wedged");
}
