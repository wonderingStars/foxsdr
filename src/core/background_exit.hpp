// background_exit.hpp - what "shut down properly" means on a phone.
//
// THE REPORT THIS EXISTS FOR. An Android tester, through the owner
// (2026-09-21): "The app can only be turned off by closing it the standard
// Android way. Upon restarting, I would always get an 'improperly shut down'
// notification."
//
// They are right on both counts, and the second follows from the first.
// FoxSDR marks a clean exit by writing telemetryCleanExit=true to the config
// at the END of its shutdown, after the pipeline has joined - a desktop shape,
// where quitting is something the user does and the process then gets to
// finish. Android has no quit: the user swipes the task away or the system
// reclaims the process, and in both cases the process can be killed outright
// with no shutdown path run at all. The marker stays false, and the next
// launch reports a crash that never happened - to the user, as the offer to
// send a report, and to us, as a crash counted in the usage figures.
//
// SO THE MARK MOVES TO THE LIFECYCLE. Going to the background is the last
// moment an Android application is certainly alive, so that is where "this run
// ended properly" is recorded; coming back to the foreground takes it away
// again, so a fault while the user is actually using the application is still
// counted exactly as before.
//
// WHAT THIS GIVES UP, stated rather than hidden: a crash that happens while
// FoxSDR is in the BACKGROUND is recorded as a clean exit. That is the right
// trade - being killed in the background is the normal end of an Android
// process, not a fault - but it does mean the Android crash count is a count
// of foreground faults.
//
// DESKTOP IS NOT TOUCHED. Minimising a window there is not the last moment of
// a process: the user is coming back, nothing is going to kill it in the
// meantime, and marking a clean exit on minimise would hide every crash that
// happens with the window down.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_BACKGROUND_EXIT_HPP
#define CASCADE_CORE_BACKGROUND_EXIT_HPP

namespace cascade::core {

// What the visibility change asks the config to record.
enum class ExitMark {
    None,     // nothing to write
    Clean,    // "this run ended properly" - written going to the background
    Running,  // "a fault from here is real" - written coming back
};

// `hidden` transitions only: a steady state writes nothing, because this is
// called every frame and the config writer must not be handed a save per
// frame. `onAndroid` is a compile-time fact passed in as a value so the
// decision stays testable on the machine the tests run on.
inline ExitMark markForVisibility(bool wasHidden, bool nowHidden, bool onAndroid) {
    if (!onAndroid) { return ExitMark::None; }
    if (nowHidden == wasHidden) { return ExitMark::None; }
    return nowHidden ? ExitMark::Clean : ExitMark::Running;
}

// True on the platform whose lifecycle this exists for. A function rather than
// a bare #ifdef at the call site so the call site reads the same everywhere.
inline constexpr bool platformEndsWithoutShutdown() {
#if defined(__ANDROID__)
    return true;
#else
    return false;
#endif
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_BACKGROUND_EXIT_HPP
