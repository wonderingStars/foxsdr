// A window thread that is slow to come back after the display wakes is not a freeze of FoxSDR's (freeze diagnosis P5, report 1338E56F, 0.99.104), NARROWED by the
// review of fix/freezes part 3 (F4).
//
// THE REPORT: "display: on - drawing resumed after 432 s off", then the window thread sat 35 to 65 s in a Windows message wait (win32u) with no FoxSDR frame above the
// message pump, and the freeze watchdog filed it. HangWatchdog::isDisplayWakeStall is the decision (pure, over module names, the age of the last display-on edge,
// how long the stall had lasted and the frames presented since the edge); the capture turns a wake into a `kind: stall` report that stays on the machine and
// counts it; and the watchdog files it as a freeze after all if it has not ended by the cap.
//
// WHAT MUST STAY A FREEZE, and is pinned here, each by its own check: a walk of fewer than four frames; a combase or rpcrt4 frame in the first twelve; a frame of
// the program's, a plugin's, a driver's or one no module claims; a top frame that is not the system's window code; no display-on edge; a stall that BEGAN
// 120 s after the edge (judged by its start, not by the capture); a frame presented since the edge; and a stall that never ends.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/hang_watchdog.hpp"

#include "test_check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;

// The report's own shape is five frames; every default below is a stall that qualifies, and each check changes ONE thing.
bool wake(const std::vector<const char*>& frames, long long edgeAgeMs, long long stalledMs = 5000, unsigned presented = 0) {
    return HangWatchdog::isDisplayWakeStall(frames.data(), static_cast<int>(frames.size()), edgeAgeMs, stalledMs, presented);
}

constexpr long long kSec = 1000;
const std::vector<const char*> kPump = {"win32u.dll", "user32.dll", "user32.dll", "ntdll.dll", "kernel32.dll"};

void checkTheWindowOfTimeIsJudgedByTheStart() {
    // The stall lasted 5 s at the capture, so it began (edge age - 5 s) after the edge.
    CHECK(wake(kPump, 20 * kSec));
    CHECK(wake(kPump, 0, 0));                                    // at the edge itself
    const long long window = static_cast<long long>(HangWatchdog::kDisplayGraceMs) + static_cast<long long>(HangWatchdog::kDisplayWakeCapMs);
    CHECK(window < 120 * kSec);                                  // so a start 120 s after the edge can never qualify
    // The last millisecond inside and the first outside, for the START.
    CHECK(wake(kPump, window - 1 + 5 * kSec, 5 * kSec));
    CHECK(!wake(kPump, window + 5 * kSec, 5 * kSec));
    // A stall that began 120 s after the edge and is captured 5 s later (edge age 125 s, stalled 5 s): FILED.
    CHECK(!wake(kPump, 125 * kSec, 5 * kSec));
    // The same capture age with a long stall is a start near the edge: judged by the start, it qualifies (the capture age alone must not decide).
    CHECK(wake(kPump, 125 * kSec, 120 * kSec));
    CHECK(!wake(kPump, 10 * 60 * kSec));                         // ten minutes after: a freeze
    CHECK(!wake(kPump, -1));                                     // no display-on edge ever seen
    // A stall already under way long before the edge is not the wake's.
    CHECK(!wake(kPump, 1 * kSec, 60 * kSec));
}

void checkAFramePresentedSinceTheEdgeKeepsItAFreeze() {
    CHECK(wake(kPump, 20 * kSec, 5 * kSec, 0));
    CHECK(!wake(kPump, 20 * kSec, 5 * kSec, 1));
    CHECK(!wake(kPump, 20 * kSec, 5 * kSec, 40));
}

void checkAShortWalkKeepsItAFreeze() {
    CHECK(!wake({"win32u.dll"}, 20 * kSec));
    CHECK(!wake({"win32u.dll", "user32.dll"}, 20 * kSec));
    CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll"}, 20 * kSec));
    CHECK(wake({"win32u.dll", "user32.dll", "ntdll.dll", "kernel32.dll"}, 20 * kSec));   // four is the least
    CHECK(!wake({}, 20 * kSec));
    CHECK(!HangWatchdog::isDisplayWakeStall(nullptr, 5, 20 * kSec, 5 * kSec, 0));
}

void checkCombaseAndRpcrtAreNotSystemWindowCode() {
    // A cross-process call blocked on a hung server has combase and rpcrt4 under the wait: that is not the window's own code.
    CHECK(!wake({"win32u.dll", "user32.dll", "combase.dll", "ntdll.dll"}, 20 * kSec));
    CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", "rpcrt4.dll"}, 20 * kSec));
    CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", "kernel32.dll", "COMBASE.DLL"}, 20 * kSec));
    // imm32 and msctf (text services) stay on the list.
    CHECK(wake({"win32u.dll", "user32.dll", "imm32.dll", "msctf.dll", "kernelbase.dll"}, 20 * kSec));
}

void checkAnOwnFrameKeepsItAFreeze() {
    for (long long age : {20 * kSec, 60 * kSec}) {
        CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", "cascade.exe"}, age));
        CHECK(!wake({"win32u.dll", "foxsdr-slint.exe", "user32.dll", "ntdll.dll"}, age));
        CHECK(!wake({"cascade.exe", "win32u.dll", "user32.dll", "ntdll.dll"}, age));              // an own frame ABOVE the pump
        CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", "some-plugin-1.0.dll"}, age));      // a plugin's
        CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", "atio6axx.dll"}, age));             // a driver's
        CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", nullptr}, age));                    // a frame no module claims: not evidence of a system stall
    }
}

void checkTheTopFrameMustBeTheSystemsWindowCode() {
    CHECK(!wake({"ntdll.dll", "user32.dll", "kernel32.dll", "ntdll.dll"}, 20 * kSec));
    CHECK(!wake({"kernelbase.dll", "win32u.dll", "user32.dll", "ntdll.dll"}, 20 * kSec));
    CHECK(wake({"user32.dll", "ntdll.dll", "kernel32.dll", "ntdll.dll"}, 20 * kSec));
    CHECK(wake({"WIN32U.DLL", "USER32.DLL", "NTDLL.DLL", "KERNEL32.DLL"}, 20 * kSec));
}

void checkOnlyTheScannedFramesAreJudged() {
    std::vector<const char*> deep(HangWatchdog::kDisplayStallScanFrames, "user32.dll");
    deep[0] = "win32u.dll";
    deep.push_back("cascade.exe");
    CHECK(wake(deep, 20 * kSec));
    deep[HangWatchdog::kDisplayStallScanFrames - 1] = "cascade.exe";
    CHECK(!wake(deep, 20 * kSec));
}

void checkTheOlderDisplayRuleIsUntouched() {
    CHECK(HangWatchdog::isDisplayPresentationStall(std::vector<const char*>{"ntdll.dll", "atio6axx.dll", "opengl32.dll", "cascade.exe"}.data(), 4));
    CHECK(!wake({"ntdll.dll", "atio6axx.dll", "opengl32.dll", "cascade.exe"}, 20 * kSec));
}

void checkTheEdgeAge() {
    HangWatchdog dog;
    CHECK(dog.displayEdgeAgeMs() == -1LL);
    dog.noteDisplayEdge();
    const long long age = dog.displayEdgeAgeMs();
    CHECK(age >= 0 && age < 5000);
    dog.setDisplayEdgeAgeForTest(125 * kSec);
    CHECK(dog.displayEdgeAgeMs() == 125 * kSec);
    dog.setDisplayEdgeAgeForTest(-1);
    CHECK(dog.displayEdgeAgeMs() == -1LL);
    CHECK(dog.displayWakesRecorded() == 0u);
}

fs::path scratchDir(const char* tag) {
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const fs::path d = fs::temp_directory_path() / ("foxsdr-display-wake-" + std::to_string(pid) + "-" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// (d) THE WATCHDOG'S OWN LOOP, real thread, real capture; the frames cannot be staged so the wake classification is forced (setForceDisplayWakeForTest) and
// everything after it is the product's code.
void checkAStallThatNeverEndsIsFiledAfterAll() {
    const fs::path reports = scratchDir("never");
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.setForceDisplayWakeForTest(true);
    w.setDisplayWakeCapMsForTest(2000);
    w.start(reports.string(), 600);
    for (int i = 0; i < 100 && w.reportsWritten() == 0u; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(w.reportsWritten() == 1u);
    CHECK(w.displayWakesRecorded() == 1u);
    const std::string first = readFile(fs::path(w.lastReportPath()));
    CHECK(first.find("kind: stall") != std::string::npos);        // first: kept on the machine as a wake
    CHECK(w.displayWakesRefiled() == 0u);
    // Never beaten: the stall does not end. After the cap it is filed as a freeze.
    for (int i = 0; i < 150 && w.reportsWritten() < 2u; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(w.reportsWritten() == 2u);
    CHECK(w.displayWakesRefiled() == 1u);
    const std::string second = readFile(fs::path(w.lastReportPath()));
    CHECK(second.find("kind: hang") != std::string::npos);
    CHECK(second.find("display wake") != std::string::npos);      // and it says it began at one
    // One refile per stall: no third report follows.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(w.reportsWritten() == 2u);
    w.stop();
}

void checkAStallThatEndsInTimeIsNotRefiled() {
    const fs::path reports = scratchDir("ends");
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.setForceDisplayWakeForTest(true);
    w.setDisplayWakeCapMsForTest(3000);
    w.start(reports.string(), 600);
    for (int i = 0; i < 100 && w.reportsWritten() == 0u; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    CHECK(w.reportsWritten() == 1u);
    CHECK(readFile(fs::path(w.lastReportPath())).find("kind: stall") != std::string::npos);
    // The window thread came back and keeps drawing, past the cap.
    for (int i = 0; i < 45; ++i) {
        w.heartbeat(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(w.displayWakesRefiled() == 0u);
    CHECK(w.reportsWritten() == 1u);
    w.stop();
}

}  // namespace

int main() {
    checkTheWindowOfTimeIsJudgedByTheStart();
    checkAFramePresentedSinceTheEdgeKeepsItAFreeze();
    checkAShortWalkKeepsItAFreeze();
    checkCombaseAndRpcrtAreNotSystemWindowCode();
    checkAnOwnFrameKeepsItAFreeze();
    checkTheTopFrameMustBeTheSystemsWindowCode();
    checkOnlyTheScannedFramesAreJudged();
    checkTheOlderDisplayRuleIsUntouched();
    checkTheEdgeAge();
    checkAStallThatNeverEndsIsFiledAfterAll();
    checkAStallThatEndsInTimeIsNotRefiled();
    return testSummary("test_display_wake");
}
