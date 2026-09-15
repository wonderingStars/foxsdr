// shell_open.hpp - handing something to the Windows shell from the GUI thread,
// without the hang watchdog filing a report about the dialog the user is
// reading.
//
// WHY THIS IS ITS OWN FILE. ShellExecute is not a launcher; it is a call into
// the shell that BLOCKS the calling thread for as long as the shell needs,
// which for an elevation prompt or a SmartScreen warning is as long as the user
// takes to answer. Field report "hang ntdll.dll @
// cascade::gui::AppWindow::launchInstaller" (0.96.2, Windows 11 26200, a German
// install path, 28 s into the session): the user pressed the in-app update key,
// ShellExecuteW blocked inside Windows.Storage.dll for over five seconds, and
// the watchdog filed a hang against a perfectly healthy application sitting on
// a consent dialog.
//
// THE WATCHDOG ALREADY HAS THE ANSWER, and core/hang_watchdog.hpp argues for it
// by name. Its false-positive rule 2b is "BLOCKING WORK THE APPLICATION ENTERS
// KNOWINGLY, which is WatchdogPause", and it says in as many words that
// anything added later that blocks the GUI thread - "a synchronous device open,
// a NATIVE MODAL DIALOG, neither of which exists here yet" - MUST take one. A
// shell call that shows an elevation prompt is precisely the native modal
// dialog that header was anticipating.
//
// SO NOT A HELPER THREAD. The alternative - posting the ShellExecute to a
// detached thread so the GUI thread never blocks - would keep the frame loop
// turning, and would be wrong for three reasons. The shell call wants a UI
// thread with a message queue to parent its prompt to; a detached call cannot
// report failure back to the button that made it (launchInstaller's return
// value IS the "could not start the installer" message); and the frames that
// would be rendered behind a modal prompt are frames of a window the user
// cannot interact with, so the "liveness" it buys is cosmetic. The pause says
// the true thing instead: the application is deliberately blocked, and a report
// about it would be noise.
//
// WHY A SEAM RATHER THAN A PAUSE AT EACH CALL SITE. There are four shell calls
// in app_window.cpp - the installer, the reports folder in both places it is
// offered, and the privacy-policy link - and every one of them is inside an
// ImGui button body, which no test in this suite can press. Bracketing is the
// kind of thing that is done correctly three times out of four for ever, and
// the one that is missed is the one that files the report. runShellOpen() is
// what the call
// sites use, it is a plain function, and tests/test_shell_open.cpp proves the
// bracket is really there and really ordered - including on the throwing path,
// where a missing resume would leave the watchdog paused for the rest of the
// session and silently disarm the one component that catches the fault this
// product ships most.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_SHELL_OPEN_HPP
#define CASCADE_GUI_SHELL_OPEN_HPP

#include <functional>

namespace cascade::gui {

// The watchdog bracket, as two callables so this header depends on nothing.
// Either may be empty - a build with no watchdog, or a test that only wants to
// observe one side.
struct ShellPauseHooks {
    std::function<void()> pause;
    std::function<void()> resume;
};

// Runs `call` between hooks.pause and hooks.resume and returns what it returned.
//
// THE RESUME IS UNCONDITIONAL, including when `call` throws - the exception
// still propagates, but a watchdog left paused would be a diagnostic switched
// off for the rest of the session by an error nobody saw. HangWatchdog counts
// its pauses, so an unbalanced pair is not merely untidy: it never un-pauses.
inline bool runShellOpen(const ShellPauseHooks& hooks, const std::function<bool()>& call) {
    struct Bracket {
        const ShellPauseHooks& h;
        explicit Bracket(const ShellPauseHooks& hooks) : h(hooks) {
            if (h.pause) { h.pause(); }
        }
        ~Bracket() {
            if (h.resume) { h.resume(); }
        }
        Bracket(const Bracket&) = delete;
        Bracket& operator=(const Bracket&) = delete;
    } bracket(hooks);
    if (!call) { return false; }
    return call();
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_SHELL_OPEN_HPP
