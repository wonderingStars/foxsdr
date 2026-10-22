// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// A BOUND ON THE HEADLESS ENGINE'S STOP (engine extraction stage 7d, finding F5 of stage 7c).
//
// THE GAP. The window application's teardown runs under its hang watchdog: AppWindow::run() calls
// `watchdog_.beginShutdown()` the instant the frame loop ends, which restarts the stall clock and raises the
// threshold to HangWatchdog::kShutdownThresholdMs, sized against every bounded wait the teardown can spend - so a
// shutdown that wedges (the 120 s CAT freeze, a plugin's destroy(), a driver's close) is captured with every thread's
// stack. The headless engine had nothing: its own control watchdog stops with the control thread, in the quiesce, and a
// stop wedged in the pipeline join or in a plugin's destroy() afterwards was watched by nothing. Under a service
// manager that is a unit "deactivating" until the manager's own timeout kills it with SIGKILL - no report, no line,
// and no say in what the exit looked like.
//
// WHAT THIS DOES. Armed immediately before EngineService::stop() - for a stop asked by a signal, by `--seconds`, and by
// a run that failed to start, all alike - and disarmed when it returns:
//   1. when a report directory is given (the run may write diagnostics and the stored switch is on), a HangWatchdog is
//      started and told `beginShutdown(budget)`: the window's own protection, the same constant, the same report with
//      every thread's stack, judged against the same budget;
//   2. a guard thread waits for the budget plus a short grace (kEndGraceMs, so the report above has been written) and,
//      if the stop has not returned by then, ENDS THE PROCESS with exit code kExitWedged (3), after one line on
//      stderr naming the budget. The window's watchdog never ends anything ("a diagnostic that terminated the user's
//      session would be worse than the hang"); a service is the other way round - a stop that will not finish is the
//      one thing a supervisor cannot wait on - so this one ends it, and says so.
//
// WHAT THE ENDING LEAVES ON DISK. The clean-exit marker is written last of all, after the plugins are released
// (EngineService::stop), so a stop wedged anywhere before it leaves the marker FALSE and the next start counts the run
// as a crash - which is the truth. A stop that had already written the marker and wedged in the drain after it ends
// with the marker TRUE and exit code 3: the settings were saved, and the exit code still says the stop was ended.
//
// HOW IT ENDS THE PROCESS. Windows: TerminateProcess on itself - ExitProcess would run the detach routines of every
// module, and the wedge may be inside one. POSIX: _exit, after the exit status is written into the exit record's page
// (core/exit_record.hpp, breadcrumb::noteExit) so a watching sentinel reads "exited 3" and not "no record". The line is
// written with the raw write of the platform, not through stdio: the thread that is wedged may be holding stderr's lock.
// The guard takes no other lock the stop could hold, and does not log to the diagnostics file for the same reason.
//
// THE BUDGET'S SIZE is HangWatchdog::kShutdownThresholdMs (53000 ms, tests/test_shutdown_budget.cpp holds it to the
// arithmetic of the bounded waits), so the longest a stop is allowed is that plus kEndGraceMs: 58 s. A service
// manager's stop timeout must be longer than that (docs/headless-service.md: TimeoutStopSec=90).
//
// A bounded run's TEST SEAM, FOXSDR_ENGINE_SHUTDOWN_BUDGET_MS, shrinks the budget (engine_main.cpp reads it only with
// `--seconds`, like the unload stall it is used with).
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL.
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/hang_watchdog.hpp"

namespace cascade::engine {

class ShutdownGuard {
public:
    // The exit code of a run whose stop was ended by the budget. 0 is a clean stop, 1 a refused or failed run, 2 a
    // refused helper argument (engine_main.cpp); this is the fourth thing a headless run can say.
    static constexpr int kExitWedged = 3;
    // How long after the budget the process is ended: the hang report's capture (the watchdog polls twice a second and
    // writes the report incrementally) has had this long to be written when the budget is crossed.
    static constexpr unsigned kEndGraceMs = 5000;

    struct Options {
        unsigned budgetMs = cascade::core::HangWatchdog::kShutdownThresholdMs;
        unsigned graceMs = kEndGraceMs;
        // Where the hang report goes; empty: no report is written and no watchdog is started.
        std::string reportDir;
        int exitCode = kExitWedged;
        // What ends the process. Empty: the real ending (endProcess). A test hands its own, which is called instead.
        std::function<void(int exitCode)> ender;
    };

    explicit ShutdownGuard(Options options);
    ~ShutdownGuard();
    ShutdownGuard(const ShutdownGuard&) = delete;
    ShutdownGuard& operator=(const ShutdownGuard&) = delete;

    // Starts the budget. Idempotent; a guard that has been disarmed is not armed again.
    void arm();
    // Stops the guard and the watchdog. Returns promptly (the wait is interruptible), however much of the budget is
    // left. Idempotent; the destructor calls it.
    void disarm();

    bool armed() const;
    // True once the budget ran out and the ender was called (only a test's ender returns).
    bool fired() const;

    // The real ending: the platform's way of ending this process now, with `exitCode`, running nothing of the stop.
    [[noreturn]] static void endProcess(int exitCode);

private:
    void threadMain();

    Options options_;
    cascade::core::HangWatchdog watchdog_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool armed_ = false;
    bool disarmed_ = false;
    bool fired_ = false;
};

}  // namespace cascade::engine
