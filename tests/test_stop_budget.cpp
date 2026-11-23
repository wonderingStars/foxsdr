// test_stop_budget.cpp - THE CONTROL THREAD'S RECEIVER STOP IS JUDGED AGAINST ITS OWN BOUNDED WAITS (report C9E6E94B, 0.99.112).
//
// A normal stop was filed as a freeze: the control pass spent 10.8 s inside a receiver stop made of waits that are each bounded on purpose (the 3 s source-thread join among
// them), and the control watchdog judges a pass at 5 s. core/stall_context.hpp holds ONE table (kStopWaits) of those waits and the Stop phase's threshold built from it, the way
// HangWatchdog::kShutdownBoundedWaitsMs is built for the teardown. This test:
//   A. reads every constant of the table out of the source that declares it and compares the number (a changed bound without the table fails);
//   B. scans the files of the stop path for `constexpr std::chrono` declarations and fails on one that is neither in the table nor named exempt below (what is NOT covered: waits in
//      files not listed here, and waits written inline - test_shutdown_budget.cpp scans the whole tree for the teardown, not for this);
//   C. stages the watchdog: inside PassPhase::Stop a control pass that stalls past 5 s-equivalent writes nothing; outside it, the same stall is reported.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>

#include "core/hang_watchdog.hpp"
#include "core/stall_context.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace stall = cascade::core::stall;
using cascade::core::HangWatchdog;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// "constexpr std::chrono::<unit> NAME{N}" -> milliseconds, or -1.
long declared(const std::string& text, const std::string& name) {
    std::size_t at = 0;
    while ((at = text.find(name, at)) != std::string::npos) {
        const std::size_t ls = text.rfind('\n', at);
        const std::string line = text.substr(ls == std::string::npos ? 0 : ls + 1, text.find('\n', at) - (ls == std::string::npos ? 0 : ls + 1));
        at += name.size();
        const std::size_t c = line.find("std::chrono::");
        const std::size_t b = line.find('{', line.find(name));
        if (c == std::string::npos || b == std::string::npos || line.find("constexpr") == std::string::npos) { continue; }
        const long n = std::atol(line.c_str() + b + 1);
        if (line.compare(c, 26, "std::chrono::milliseconds ") == 0) { return n; }
        if (line.compare(c, 21, "std::chrono::seconds ") == 0) { return n * 1000; }
    }
    return -1;
}

}  // namespace

int main() {
    const fs::path root(CASCADE_SOURCE_DIR);
    // A. the table against the source
    std::set<std::string> known;
    for (const stall::StopWait& w : stall::kStopWaits) {
        const long ms = declared(readFile(root / w.file), w.name);
        std::printf("  %-34s %-18s table %5u ms  source %5ld ms\n", w.file, w.name, w.ms, ms);
        CHECK(ms == static_cast<long>(w.ms));
        known.insert(w.name);
    }
    std::printf("stop's bounded waits %u ms + reserve %u ms; the Stop phase's threshold %u ms (the watchdog's own: %u ms)\n", stall::stopBoundedWaitsMs(), stall::kStopReserveMs,
                stall::kStopThresholdMs, HangWatchdog::kDefaultThresholdMs);
    CHECK(stall::kStopThresholdMs == 2u * (stall::stopBoundedWaitsMs() + stall::kStopReserveMs));
    CHECK(stall::stopBoundedWaitsMs() > HangWatchdog::kDefaultThresholdMs);   // the defect, in one line: the sum does not fit the pass's threshold
    // B. no unlisted chrono constant in the stop path's files
    const std::set<std::string> exempt = {"kStreamHealthWindow", "kStallWindow", "kLateCallGrace", "kSilenceStep"};   // detector thresholds and grace times: nothing waits on them in a stop
    for (const char* f : {"src/core/pipeline.cpp", "src/source/soapy_source.cpp", "src/source/soapy_source.hpp"}) {
        const std::string text = readFile(root / f);
        std::size_t at = 0;
        while ((at = text.find("constexpr std::chrono::", at)) != std::string::npos) {
            const std::size_t s = text.find(' ', at + 23);
            const std::size_t e = text.find_first_of("{= ;", s + 1);
            const std::string name = text.substr(s + 1, e - s - 1);
            at += 23;
            if (name.empty() || name[0] != 'k') { continue; }
            if (known.count(name) == 0 && exempt.count(name) == 0) { std::printf("FAIL %s declares %s: add it to stall::kStopWaits or to the exempt list\n", f, name.c_str()); }
            CHECK(known.count(name) == 1 || exempt.count(name) == 1);
        }
    }
    // C. the watchdog
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-stopbudget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.setRole(HangWatchdog::Role::Control);
    w.start(dir.string(), 700);
    w.heartbeat();
    {
        const stall::PassPhaseScope inStop(stall::PassPhase::Stop);
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));   // a stop pass that spends 3x the threshold in bounded waits
        CHECK(w.reportsWritten() == 0u);                                  // no freeze report
    }
    w.heartbeat();
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));        // the same stall in any other phase
    CHECK(w.reportsWritten() >= 1u);                                       // IS reported
    w.stop();
    fs::remove_all(dir, ec);
    return testSummary("test_stop_budget");
}
