// test_watchdog_startup.cpp - HangWatchdog::beginStartup(), the start-up budget.
//
// Two field hang reports (0.99.42 and 0.99.43, one Windows 10 laptop on an
// older Intel GPU with an RTL-SDR open) froze for more than 5 s in the FIRST
// frames, on a machine whose start-up already took 8-10 s before the first
// frame. The 5 s threshold was measured against this desk's ~11 ms start-up
// gap. beginStartup() judges the first frames against a longer budget and then
// drops back; these tests pin that it drops, that a start-up stall inside the
// budget writes nothing, that a stall after the drop still reports, and that a
// shutdown budget set during start-up is never overwritten by the drop.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/hang_watchdog.hpp"

#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;

namespace {

std::string scratchDir(const char* tag) {
    const fs::path d = fs::temp_directory_path() /
                       ("foxsdr-wdstartup-" + std::string(tag) + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(d, ec);
    return d.string();
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The threshold in force: the start-up budget for exactly `frames` heartbeats,
// then what start() was given.
void testThresholdDropsAfterTheStartupFrames() {
    std::printf("the start-up budget covers its frames, then drops to start()'s threshold\n");
    HangWatchdog w;
    w.start(std::string(), 5000);
    CHECK(w.thresholdMs() == 5000u);
    w.beginStartup(30000, 3);
    CHECK(w.thresholdMs() == 30000u);
    w.heartbeat();
    w.heartbeat();
    CHECK(w.thresholdMs() == 30000u);  // two of three frames: still start-up
    w.heartbeat();
    CHECK(w.thresholdMs() == 5000u);   // third frame: back to the frame budget
    w.heartbeat();
    CHECK(w.thresholdMs() == 5000u);
    w.stop();
}

// A shutdown that begins inside the start-up frames keeps ITS budget: the drop
// is a compare-and-swap against the start-up value, not a blind store.
void testShutdownDuringStartupIsNotOverwritten() {
    std::printf("a shutdown budget set during start-up survives the remaining frames\n");
    HangWatchdog w;
    w.start(std::string(), 5000);
    w.beginStartup(30000, 3);
    w.heartbeat();
    w.beginShutdown(27000);
    CHECK(w.thresholdMs() == 27000u);
    w.heartbeat();
    w.heartbeat();
    w.heartbeat();
    CHECK(w.thresholdMs() == 27000u);
    w.stop();
}

// Nothing to do when the "budget" would not be longer, or covers no frames.
void testDegenerateCallsChangeNothing() {
    std::printf("a start-up budget no longer than the frame budget, or of 0 frames, is ignored\n");
    HangWatchdog w;
    w.start(std::string(), 5000);
    w.beginStartup(4000, 3);
    CHECK(w.thresholdMs() == 5000u);
    w.beginStartup(30000, 0);
    CHECK(w.thresholdMs() == 5000u);
    w.stop();
}

// The behaviour that matters: a first-frame stall longer than the frame budget
// but inside the start-up budget writes NO report; the same stall after the
// start-up frames does.
void testStartupStallIsNotReportedButALaterOneIs() {
    std::printf("a 2.5 s stall in the first frames writes nothing; the same stall later reports\n");
    const std::string dir = scratchDir("report");
    HangWatchdog w;
    w.start(dir, 1500);
    w.beginStartup(30000, 2);
    w.heartbeat();       // frame 1 begins...
    sleepMs(2500);       // ...and takes 2.5 s: over 1.5 s, inside 30 s
    CHECK(w.reportsWritten() == 0u);
    w.heartbeat();       // frame 2: the start-up frames are done
    CHECK(w.thresholdMs() == 1500u);
    w.heartbeat();
    sleepMs(2500);       // the same stall, now judged against 1.5 s
    CHECK(w.reportsWritten() >= 1u);
    w.heartbeat();
    w.stop();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

int main() {
    testThresholdDropsAfterTheStartupFrames();
    testShutdownDuringStartupIsNotOverwritten();
    testDegenerateCallsChangeNothing();
    testStartupStallIsNotReportedButALaterOneIs();
    std::printf("test_watchdog_startup: %d checks, %d failed\n", g_checksRun, g_checksFailed);
    return g_checksFailed == 0 ? 0 : 1;
}
