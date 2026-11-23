// test_watchdog_startup_wired.cpp - the start-up budget is CALLED, by both windows' watchdogs.
//
// HangWatchdog::beginStartup() (tests/test_watchdog_startup.cpp pins what it does) was called by AppWindow::run in 0.99.101 and 0.99.104 and by
// nothing from 0.99.105 on, so the first frames were judged against the 5 s steady threshold: two field freeze reports (docs: DIAG-freezes P3).
// Nothing failed when the call went, because nothing checked that anybody makes it. This does, on the source, comments stripped:
//   - the classic window's AppWindow::run calls watchdog_.beginStartup() after watchdog_.start();
//   - the Slint window has no freeze watchdog of its own (slint/host_duties.hpp); the watchdog that sees its first moments is the control thread's, so
//     EngineService must set ControlThreadOptions::startupBudget and Engine::startControlThread must act on it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// The file's text with // comments removed (a call named only in a comment is not a call).
std::string codeOf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    std::string out;
    std::istringstream lines(raw);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t c = line.find("//");
        if (c != std::string::npos) { line.erase(c); }
        out += line;
        out += '\n';
    }
    return out;
}

bool has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

int main() {
    std::printf("test_watchdog_startup_wired\n");
#ifdef CASCADE_SOURCE_DIR
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
#else
    const fs::path src = fs::current_path() / "src";
#endif
    const std::string window = codeOf(src / "gui" / "app_window.cpp");
    const std::string service = codeOf(src / "engine" / "engine_service.cpp");
    const std::string engine = codeOf(src / "engine" / "engine.cpp");
    CHECK(!window.empty() && !service.empty() && !engine.empty());

    std::printf("the classic window starts its watchdog and then gives the first frames their budget\n");
    const std::size_t start = window.find("watchdog_.start(");
    const std::size_t begin = window.find("watchdog_.beginStartup(");
    CHECK(start != std::string::npos);
    CHECK(begin != std::string::npos);
    CHECK(begin > start);

    std::printf("the control thread's watchdog (the Slint window's only one) gets it through the service\n");
    CHECK(has(service, "opt.startupBudget = true"));
    CHECK(has(engine, "controlWatchdog_.beginStartupByTime("));   // by time and passes, not by beats (review of fix/freezes, finding 5)
    CHECK(has(engine, "options.startupBudget"));
    CHECK(has(engine, "controlWatchdog_.notePassCompleted()"));
    // ...and the watchdog is armed BEFORE the first pass the service runs on its own thread
    const std::size_t arm = service.find("engine_.armControlWatchdog(opt)");
    const std::size_t first = service.find("engine_.controlPass()");
    CHECK(arm != std::string::npos && first != std::string::npos && arm < first);

    return testSummary("test_watchdog_startup_wired");
}
