// present_grace.hpp - the frames in which this application is not expected to
// present, and must not be reported for failing to.
//
// WHAT IT IS FOR. Field report "hang ntdll.dll @ cascade::gui::AppWindow::run"
// (0.96.3, Windows 11 26200, 1177 s uptime): the log carries "GLFW error 65544:
// Win32: Failed to query display settings" and then the GUI thread sits in
// glfwSwapBuffers inside atio6axx.dll - AMD's display driver - for over five
// seconds, and the hang watchdog files a report. Nothing in this application
// was wrong. A monitor had been switched off, or a mode changed, or a session
// moved, and the driver had nowhere to present to until it was settled.
//
// TWO SITUATIONS, ONE STATE MACHINE, because they must share a single pause:
//
//   THE DISPLAY CHANGED. A BOUNDED grace - HangWatchdog::kDisplayGraceMs - from
//   the moment the change is seen, restarted by each new change. Bounded
//   because a pause that never expires is the watchdog switched off, and a
//   display that never comes back really is worth a report.
//
//   THE WINDOW IS NOT BEING SHOWN. Iconified, or hidden. Held for as long as
//   that lasts and NOT bounded: a window minimised for an hour is not a hang,
//   it is a window minimised for an hour, and every frame of it is skipped by
//   the compositor rather than by this program.
//
// WHY A SEPARATE CLASS RATHER THAN TWO BOOLS IN AppWindow. The pause is
// COUNTED (HangWatchdog::pause/resume nest), so a pause taken twice and
// released once leaves the watchdog disarmed for the rest of the session -
// silently, and precisely on the sessions where something did go wrong. This
// holds at most one pause, ever, whichever of the two reasons is in force, and
// tests/test_present_grace.cpp drives every transition between them. It is also
// the only part of the fix a test can reach at all: nothing in ctest can switch
// a monitor off or minimise a window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PRESENT_GRACE_HPP
#define CASCADE_GUI_PRESENT_GRACE_HPP

#include <functional>

namespace cascade::gui {

class PresentGrace {
public:
    // `graceS` is how long a display change buys, in seconds. The hooks are the
    // watchdog's pause/resume; either may be empty (a run with no watchdog).
    PresentGrace(std::function<void()> pause, std::function<void()> resume, double graceS)
        : pause_(std::move(pause)), resume_(std::move(resume)), graceS_(graceS) {}

    ~PresentGrace() { release(); }

    PresentGrace(const PresentGrace&) = delete;
    PresentGrace& operator=(const PresentGrace&) = delete;

    // Called once per frame, before presenting.
    //
    //   `displayChanged` - a display change was seen since the last call (the
    //                      WM_DISPLAYCHANGE counter moved, a monitor callback
    //                      fired, or GLFW reported a display-settings error).
    //   `notPresenting`  - the window is iconified or otherwise not visible.
    void update(double nowS, bool displayChanged, bool notPresenting) {
        hidden_ = notPresenting;
        if (displayChanged) {
            // RESTARTED, not extended from whatever is left: a second change
            // during the grace is a second settling period, and the first one's
            // remaining time says nothing about it.
            graceUntilS_ = nowS + graceS_;
            sawChange_ = true;
        }
        const bool graceLive = sawChange_ && nowS < graceUntilS_;
        if (!graceLive) { sawChange_ = false; }
        const bool want = hidden_ || graceLive;
        if (want && !paused_) {
            paused_ = true;
            if (pause_) { pause_(); }
        } else if (!want && paused_) {
            paused_ = false;
            if (resume_) { resume_(); }
        }
        reason_ = !paused_ ? Reason::None : (hidden_ ? Reason::Hidden : Reason::DisplayChange);
    }

    // Releases the pause if one is held. Called from the destructor and from
    // the teardown: the shutdown gets its OWN budget (HangWatchdog::
    // beginShutdown) and must be measured, not skipped, so a window that was
    // minimised at quit must not carry its pause into the teardown.
    void release() {
        if (!paused_) { return; }
        paused_ = false;
        reason_ = Reason::None;
        sawChange_ = false;
        if (resume_) { resume_(); }
    }

    enum class Reason { None, DisplayChange, Hidden };

    bool paused() const { return paused_; }
    Reason reason() const { return reason_; }

private:
    std::function<void()> pause_;
    std::function<void()> resume_;
    double graceS_ = 0.0;
    double graceUntilS_ = 0.0;
    bool sawChange_ = false;
    bool hidden_ = false;
    bool paused_ = false;
    Reason reason_ = Reason::None;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PRESENT_GRACE_HPP
