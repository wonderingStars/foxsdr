// A window thread that is slow to come back after the display wakes is not a freeze of FoxSDR's (freeze diagnosis P5, report 1338E56F, 0.99.104).
//
// THE REPORT: "display: on - drawing resumed after 432 s off", then the window thread sat 35 to 65 s in a Windows message wait (win32u) with no FoxSDR frame above the
// message pump, and the freeze watchdog filed it. The 432 s was how long the display was off, not the stall. HangWatchdog::isDisplayWakeStall is the decision (pure,
// over module names and the age of the last display-on edge); the capture turns a wake into a `kind: stall` report that stays on the machine and counts it.
//
// WHAT MUST STAY A FREEZE, and is pinned here: the same stall with any frame that is not one of Windows' own modules in the walked frames (the program, a plugin, a
// driver, a frame the module table cannot name); a stall whose top frame is not the system's window code; a stall with no display-on edge; and a stall older than the
// edge window (kDisplayGraceMs + kDisplayWakeCapMs = 130 s).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/hang_watchdog.hpp"

#include "test_check.hpp"

#include <string>
#include <vector>

namespace {

using cascade::core::HangWatchdog;

bool wake(const std::vector<const char*>& frames, long long edgeAgeMs) {
    return HangWatchdog::isDisplayWakeStall(frames.data(), static_cast<int>(frames.size()), edgeAgeMs);
}

constexpr long long kSec = 1000;

void checkTheWindowOfTime() {
    // The report's own shape: the system's window code on top and under it, nothing of ours.
    const std::vector<const char*> pump = {"win32u.dll", "user32.dll", "user32.dll", "ntdll.dll", "kernel32.dll"};
    CHECK(wake(pump, 20 * kSec));        // 20 s after the display came back: a display wake
    CHECK(wake(pump, 120 * kSec));       // 120 s: still inside grace + cap (130 s)
    CHECK(wake(pump, 129999));           // the last millisecond inside
    CHECK(!wake(pump, 130000));          // the first one outside
    CHECK(!wake(pump, 10 * 60 * kSec));  // ten minutes after: a freeze, filed
    CHECK(!wake(pump, -1));              // no display-on edge ever seen: a freeze, filed
    CHECK(wake(pump, 0));                // at the edge itself
    CHECK(static_cast<long long>(HangWatchdog::kDisplayGraceMs) + static_cast<long long>(HangWatchdog::kDisplayWakeCapMs) == 130000LL);
}

void checkAnOwnFrameKeepsItAFreeze() {
    // The same stall with a FoxSDR frame in the walked frames: filed, at every age.
    for (long long age : {20 * kSec, 120 * kSec, 10 * 60 * kSec}) {
        CHECK(!wake({"win32u.dll", "user32.dll", "cascade.exe"}, age));
        CHECK(!wake({"win32u.dll", "foxsdr-slint.exe", "user32.dll"}, age));
        CHECK(!wake({"cascade.exe", "win32u.dll", "user32.dll"}, age));              // an own frame ABOVE the pump
        CHECK(!wake({"win32u.dll", "user32.dll", "some-plugin-1.0.dll"}, age));      // a plugin's
        CHECK(!wake({"win32u.dll", "user32.dll", "atio6axx.dll"}, age));             // a driver's
        CHECK(!wake({"win32u.dll", "user32.dll", nullptr}, age));                    // a frame no module claims: not evidence of a system stall
    }
    // The own frame is found anywhere in the walked frames, not only next to the pump.
    CHECK(!wake({"win32u.dll", "user32.dll", "ntdll.dll", "kernel32.dll", "cascade.exe"}, 20 * kSec));
}

void checkTheTopFrameMustBeTheSystemsWindowCode() {
    CHECK(!wake({"ntdll.dll", "user32.dll"}, 20 * kSec));                // a plain kernel wait on top is not a message wait
    CHECK(!wake({"kernelbase.dll", "win32u.dll"}, 20 * kSec));
    CHECK(wake({"user32.dll", "ntdll.dll"}, 20 * kSec));                 // user32 on top counts
    CHECK(wake({"WIN32U.DLL", "USER32.DLL"}, 20 * kSec));                // the loader's spelling varies
    CHECK(wake({"win32u.dll"}, 20 * kSec));                              // a walk that found only the one frame
    CHECK(!wake({}, 20 * kSec));                                         // no frames: nothing to go on
    CHECK(!HangWatchdog::isDisplayWakeStall(nullptr, 3, 20 * kSec));
}

void checkOnlyTheScannedFramesAreJudged() {
    // The frame loop is under EVERY stall, so the rule looks only as deep as the display rule does (kDisplayStallScanFrames).
    std::vector<const char*> deep(HangWatchdog::kDisplayStallScanFrames, "user32.dll");
    deep[0] = "win32u.dll";
    deep.push_back("cascade.exe");
    CHECK(wake(deep, 20 * kSec));
    deep[HangWatchdog::kDisplayStallScanFrames - 1] = "cascade.exe";   // inside the scan: filed
    CHECK(!wake(deep, 20 * kSec));
}

void checkTheOlderDisplayRuleIsUntouched() {
    // A display-driver stall is still the older rule's, and a wake never claims what that rule refuses.
    CHECK(HangWatchdog::isDisplayPresentationStall(std::vector<const char*>{"ntdll.dll", "atio6axx.dll", "opengl32.dll", "cascade.exe"}.data(), 4));
    CHECK(!wake({"ntdll.dll", "atio6axx.dll", "opengl32.dll", "cascade.exe"}, 20 * kSec));
}

void checkTheEdgeAge() {
    HangWatchdog dog;
    CHECK(dog.displayEdgeAgeMs() == -1LL);        // no edge yet
    dog.noteDisplayEdge();
    const long long age = dog.displayEdgeAgeMs();
    CHECK(age >= 0 && age < 5000);
    dog.setDisplayEdgeAgeForTest(125 * kSec);
    CHECK(dog.displayEdgeAgeMs() == 125 * kSec);
    dog.setDisplayEdgeAgeForTest(-1);
    CHECK(dog.displayEdgeAgeMs() == -1LL);
    CHECK(dog.displayWakesRecorded() == 0u);
}

}  // namespace

int main() {
    checkTheWindowOfTime();
    checkAnOwnFrameKeepsItAFreeze();
    checkTheTopFrameMustBeTheSystemsWindowCode();
    checkOnlyTheScannedFramesAreJudged();
    checkTheOlderDisplayRuleIsUntouched();
    checkTheEdgeAge();
    return testSummary("test_display_wake");
}
