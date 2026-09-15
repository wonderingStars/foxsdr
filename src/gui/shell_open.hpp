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
// LINUX ADDENDUM (2026-09-15). There is no ShellExecute on Linux, so
// AppWindow::shellOpen hands the target to xdg-open instead. That call gets
// its own free function here, posixShellOpen(), for the same reason the
// bracket above is a free function: every call site is inside an ImGui button
// body that no test can press, so the part worth pinning is pulled out where a
// test can reach it directly, without constructing an AppWindow (and the GL
// context it owns) at all. tests/test_shell_open_posix.cpp points
// FOXSDR_SHELL_OPEN_EXE at a stand-in script instead of the real xdg-open.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_SHELL_OPEN_HPP
#define CASCADE_GUI_SHELL_OPEN_HPP

#include <functional>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>
#endif

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

#if !defined(_WIN32)
// Hands `target` to xdg-open (or, when FOXSDR_SHELL_OPEN_EXE names one, a
// test's stand-in) as a single argv entry, exec'd directly - never through
// system()/popen() - so a target containing shell metacharacters (a folder
// named with a `;`, a URL with `&`) is never reinterpreted.
//
// WHY DOUBLE-FORK. xdg-open can legitimately outlive this call by a long time
// - it may just be handing off to an already-running browser, or it may cold
// start one - and this function must not block waiting for it: the caller
// wraps it in a watchdog pause sized for a native modal dialog, not for an
// arbitrarily long-lived helper process. So the immediate child forks a
// grandchild and exits at once; the parent's single waitpid() therefore
// returns immediately (it is waiting on a fork(), not on xdg-open), and the
// grandchild that actually execs is reparented to init/subreaper and reaped
// there whenever it exits, so nothing is left as a zombie no matter how long
// it runs.
//
// WHY A PIPE TOO. The double fork alone can only report "a grandchild was
// forked", not "the exec worked" - the immediate child exits before the
// grandchild has even called execvp. A CLOEXEC pipe answers that without
// waiting for xdg-open to finish: the grandchild inherits the write end, and
// a successful execvp closes it as a side effect of exec() (CLOEXEC), so the
// parent's read() returns EOF the instant exec succeeds. If execvp fails
// instead (xdg-open missing, say), the grandchild writes errno first and the
// read() returns it. Either way this read only waits on the exec() syscall
// itself, never on the process it starts.
inline bool posixShellOpen(const std::string& target) {
    const char* exeEnv = std::getenv("FOXSDR_SHELL_OPEN_EXE");
    const std::string exe = (exeEnv != nullptr && exeEnv[0] != '\0') ? exeEnv : "xdg-open";

    int pipeFds[2] = {-1, -1};
    if (::pipe2(pipeFds, O_CLOEXEC) != 0) { return false; }
    const int readFd = pipeFds[0];
    const int writeFd = pipeFds[1];

    const pid_t child = fork();
    if (child < 0) {
        ::close(readFd);
        ::close(writeFd);
        return false;
    }
    if (child == 0) {
        // First child: fork the grandchild that does the real exec, then exit
        // immediately so the parent never blocks on anything longer-lived
        // than this fork.
        ::close(readFd);
        const pid_t grandchild = fork();
        if (grandchild < 0) { _exit(1); }
        if (grandchild > 0) { _exit(0); }

        // Grandchild: detach from the parent's session/controlling terminal
        // and give it quiet, closed stdio so nothing it writes lands in
        // FoxSDR's own console. writeFd is CLOEXEC, so it does not need to be
        // closed by hand on the success path - only on the failure path,
        // where execvp never happens.
        ::setsid();
        const int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) { ::close(devNull); }
        }
        std::vector<char> exeBuf(exe.begin(), exe.end());
        exeBuf.push_back('\0');
        std::vector<char> targetBuf(target.begin(), target.end());
        targetBuf.push_back('\0');
        char* argv[] = {exeBuf.data(), targetBuf.data(), nullptr};
        ::execvp(exe.c_str(), argv);
        // execvp only returns on failure - report it, then exit.
        const int err = errno;
        (void)!::write(writeFd, &err, sizeof err);
        _exit(127);
    }
    // Parent: reap the first child, which exits right away - this is not
    // "waiting for xdg-open", it is waiting for a fork() that already
    // happened. Then close our copy of the write end (the grandchild holds
    // the only other one) and read: EOF means the grandchild's execvp closed
    // it for us by succeeding, a filled buffer means it reported a failure.
    ::close(writeFd);
    int status = 0;
    const bool firstChildOk = (::waitpid(child, &status, 0) == child) && WIFEXITED(status) &&
                              WEXITSTATUS(status) == 0;
    int execErrno = 0;
    const ssize_t n = ::read(readFd, &execErrno, sizeof execErrno);
    ::close(readFd);
    return firstChildOk && n == 0;
}
#endif  // !defined(_WIN32)

}  // namespace cascade::gui

#endif  // CASCADE_GUI_SHELL_OPEN_HPP
