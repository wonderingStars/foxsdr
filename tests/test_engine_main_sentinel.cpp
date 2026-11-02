// test_engine_main_sentinel.cpp - A HEADLESS foxsdr-engine UNDER A SENTINEL (deep review of engine/stage4, batch A; docs/engine-stage4.md 6.31).
//
// When FOXSDR_DIAG_DIR names a directory the engine process arms a sentinel exactly as the window application does (boot's step 13 is kind-blind; the
// directory is the engine's one door to diagnostics on disk, and the exit recorder of 7d reads "exited 0" through it). But the engine runs with
// `breadcrumbs = false` - "foxsdr-engine has never set a sentinel phase" - so the breadcrumb stayed at BuildingApp (set by boot) for the life of the
// process: a clean stop (exit 0, the orderly shutdown behind it) never reached the sentinel's "shutdown finished" (ShutdownClosingWindow), and was filed
// as a crash-kind report ("exit code 0 before the shutdown had finished") - one report for every healthy `systemctl stop` of a service that has
// diagnostics on; a kill an hour in was filed as a START-UP death (the phase never left "building the application"). The record (6.18, "NO SENTINEL
// CHANGE ... Not examined further") had left this open.
//
// The headless host now owns its own timeline, as the window host owns the window's: BuildingApp (boot) -> AwaitingFirstFrame (the control thread has
// started) -> Running (its second beat: the pass is the engine's frame loop, see test_sentinel_beat_source) -> ShutdownBegun (the stop's guard) ->
// ShutdownClosingWindow (the stop returned: the marker is written) -> Finished.
//
//   A  A CLEAN RUN LEAVES NO REPORT. `foxsdr-engine --seconds 3` with FOXSDR_DIAG_DIR set: exit 0, the sentinel was watching (the log says so), and once
//      the watcher has had its time to finish there is no report in the crashes folder.
//   B  A KILL OF A HEALTHY ENGINE IS "ENDED FROM OUTSIDE", NOT A START-UP DEATH. A run with no bound, up for several seconds, ended with
//      TerminateProcess: the one report is the Outside class (kept on this machine, never sent), in the phase "running" - not "ended before the first
//      frame", not "building the application".
//
// HERMETIC: every per-user folder is a scratch tree; the diagnostics directory is a scratch directory of this test; the telemetry endpoint is a closed
// local port; no settings file is named, so nothing is stored. The real %APPDATA% is never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "core/sentinel.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

namespace {

std::vector<fs::path> reportsIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".txt") == 0 && (name.rfind("crash-", 0) == 0 || name.rfind("hang-", 0) == 0)) {
            out.push_back(e.path());
        }
    }
    return out;
}

// The first line of a report that starts with `name: `, or "".
std::string field(const std::string& text, const char* name) {
    const std::string key = std::string(name) + ": ";
    const std::size_t at = text.rfind("\n" + key, 0) == 0 ? 0 : text.find("\n" + key);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t from = (at == 0 ? 0 : at + 1) + key.size();
    const std::size_t to = text.find('\n', from);
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

bool waitFor(const std::function<bool()>& done, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return done();
}

}  // namespace

int main() {
    std::printf("test_engine_main_sentinel\n");
    const fs::path root = emt::scratch("engine-sentinel");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));

    // ---- A: a clean run -------------------------------------------------------------------------------------------------------------------
    {
        const fs::path diag = root / "diag-a";
        fs::create_directories(diag);
        emt::setEnv("FOXSDR_DIAG_DIR", diag.string());
        emt::Child c;
        CHECK(c.start(engineExe, {"--seconds", "3"}, home, root / "a.out", root / "a.err"));
        const bool ended = c.waitExit(60000);
        CHECK(ended);
        CHECK(c.code() == 0);
        emt::unsetEnv("FOXSDR_DIAG_DIR");
        // The watcher is a process of its own that outlives the engine by a moment: give it time to decide, then look.
        std::this_thread::sleep_for(std::chrono::milliseconds(6000));
        const std::string log = emt::readText(diag / "logs" / "foxsdr.log");
        CHECK(emt::contains(log, "sentinel: watching this session"));   // the premise: a sentinel WAS armed for this engine
        const std::vector<fs::path> reports = reportsIn(diag / "crashes");
        if (!reports.empty()) { std::printf("FAIL a clean headless run filed: %s\n%s\n", reports.front().string().c_str(), emt::readText(reports.front()).substr(0, 400).c_str()); }
        CHECK(reports.empty());
    }

    // ---- B: a healthy engine ended from outside ---------------------------------------------------------------------------------------------
    {
        const fs::path diag = root / "diag-b";
        fs::create_directories(diag);
        emt::setEnv("FOXSDR_DIAG_DIR", diag.string());
        emt::Child c;
        CHECK(c.start(engineExe, {}, home, root / "b.out", root / "b.err"));
        // up and running: the engine's own line says so
        const bool up = waitFor([&] { return emt::contains(emt::readText(root / "b.err"), "running"); }, 30000);
        CHECK(up);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));   // several seconds of life, beyond the 30-frame start-up allowance
        c.kill();
        emt::unsetEnv("FOXSDR_DIAG_DIR");
        const bool filed = waitFor([&] { return !reportsIn(diag / "crashes").empty(); }, 30000);
        CHECK(filed);
        const std::vector<fs::path> reports = reportsIn(diag / "crashes");
        CHECK(reports.size() == 1);
        if (!reports.empty()) {
            const std::string text = emt::readText(reports.front());
            const std::string reason = field(text, "reason");
            std::printf("    the report's reason: %s\n", reason.c_str());
            CHECK(reason.rfind(cascade::core::kSentinelReasonOutside, 0) == 0);                 // ended from outside, the class that is kept on this machine
            CHECK(reason.find("phase running") != std::string::npos);                          // while it was running
            CHECK(reason.rfind(cascade::core::kSentinelReasonStartup, 0) != 0);                // never "ended before the first frame"
            CHECK(reason.find("building the application") == std::string::npos);
        }
    }

    // ---- C: ended from outside DURING the shutdown: the watcher is told which stage ----------------------------------------------------------
    // The service sets the sentinel's shutdown stages as it goes (ServiceOptions::breadcrumbs, set by this host): a stop that hangs in the plugins' unload is
    // "shutting down: unloading plugins" in the report, as it is for the window. (FOXSDR_ENGINE_UNLOAD_STALL_MS is the bounded run's seam that wedges that stage.)
    {
        const fs::path diag = root / "diag-c";
        fs::create_directories(diag);
        emt::setEnv("FOXSDR_DIAG_DIR", diag.string());
        emt::setEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS", "60000");
        emt::Child c;
        CHECK(c.start(engineExe, {"--seconds", "2"}, home, root / "c.out", root / "c.err"));
        const bool inside = waitFor([&] { return emt::contains(emt::readText(root / "c.err"), "--diag-unload-stall wedging the plugin unload"); }, 60000);
        CHECK(inside);
        c.kill();
        emt::unsetEnv("FOXSDR_ENGINE_UNLOAD_STALL_MS");
        emt::unsetEnv("FOXSDR_DIAG_DIR");
        const bool filed = waitFor([&] { return !reportsIn(diag / "crashes").empty(); }, 30000);
        CHECK(filed);
        const std::vector<fs::path> reports = reportsIn(diag / "crashes");
        CHECK(reports.size() == 1);
        if (!reports.empty()) {
            const std::string reason = field(emt::readText(reports.front()), "reason");
            std::printf("    the report's reason: %s\n", reason.c_str());
            CHECK(reason.find("phase shutting down: unloading plugins") != std::string::npos);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_sentinel");
}
