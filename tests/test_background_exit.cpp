// A phone has no quit, so "shut down properly" has to mean something else.
//
// THE REPORT (Android tester, through the owner, 2026-09-21): "The app can
// only be turned off by closing it the standard Android way. Upon restarting,
// I would always get an 'improperly shut down' notification."
//
// Every launch was being counted as a crash, because the clean-exit marker is
// written at the end of a desktop-shaped shutdown that an Android process
// never gets to run. core/background_exit.hpp moves the mark onto the
// lifecycle; this is its truth table.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/background_exit.hpp"

#include "test_check.hpp"

int main() {
    using cascade::core::ExitMark;
    using cascade::core::markForVisibility;

    // --- ANDROID: the transitions that matter -----------------------------
    //
    // Going to the background is the last moment the process is certainly
    // alive, so that is where "this run ended properly" is recorded.
    CHECK(markForVisibility(/*wasHidden=*/false, /*nowHidden=*/true, /*onAndroid=*/true) ==
          ExitMark::Clean);
    // Coming back takes it away again: a fault while the user is looking at
    // FoxSDR is a real crash and must still be counted as one.
    CHECK(markForVisibility(true, false, true) == ExitMark::Running);

    // --- AND THE STEADY STATES WRITE NOTHING ------------------------------
    //
    // This is asked every frame. A mark on every frame would hand the config
    // writer sixty saves a second, which on a phone is the battery and the
    // flash both, and would make the marker meaningless anyway.
    CHECK(markForVisibility(false, false, true) == ExitMark::None);
    CHECK(markForVisibility(true, true, true) == ExitMark::None);

    // --- DESKTOP IS NOT TOUCHED -------------------------------------------
    //
    // Minimising is not the last moment of a desktop process: nothing is
    // going to kill it, the user is coming back, and marking a clean exit
    // here would hide every crash that happens with the window down. All four
    // combinations, because the whole point is that none of them writes.
    CHECK(markForVisibility(false, true, false) == ExitMark::None);
    CHECK(markForVisibility(true, false, false) == ExitMark::None);
    CHECK(markForVisibility(false, false, false) == ExitMark::None);
    CHECK(markForVisibility(true, true, false) == ExitMark::None);

    // --- AND THE PLATFORM ANSWER IS THE PLATFORM'S ------------------------
    //
    // The tests run on the desktop, so this is the answer they must see; an
    // Android build compiles the other branch, and the ctest suite runs there
    // too (the Linux clean-clone judge builds the same file).
#if defined(__ANDROID__)
    CHECK(cascade::core::platformEndsWithoutShutdown());
#else
    CHECK(!cascade::core::platformEndsWithoutShutdown());
#endif

    return testSummary("test_background_exit");
}
