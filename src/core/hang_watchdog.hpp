// hang_watchdog.hpp - the half a crash handler never catches.
//
// WHY THIS EXISTS AT ALL. Every fault this product has actually shipped was a
// hang, not a crash: a 120 s freeze while a CAT client was shut down, a map
// window that could not be closed, an audio stream that had died while every
// number on screen still read healthy. A crash handler would have caught none
// of them - the process was alive and, from its own point of view, fine. The
// only observable that distinguishes "busy" from "wedged" is whether the GUI
// thread comes back round its loop, so that is what is measured.
//
// WHY EVERY THREAD, NOT JUST THE STUCK ONE. A deadlock is only legible as a
// pair. "GUI thread is blocked in WaitForSingleObject" says nothing; "GUI
// thread waits on the CAT server's mutex while the CAT thread waits inside a
// socket close" is the bug, and it is only visible if both stacks are in the
// same report. So the watchdog suspends every thread in the process in turn,
// unwinds it, and resumes it.
//
// WHY IT IS SAFE TO USE MORE MACHINERY HERE THAN IN THE CRASH HANDLER. A hung
// process is not a corrupted one: the heap is intact, the loader is intact,
// nothing has faulted. The watchdog runs on its own thread and may allocate.
// The one thing it must not do is take a lock the hung threads might hold,
// which is why it never logs through the file path while capturing and never
// calls back into the application.
//
// AND THE LOCK THAT IS EASY TO MISS. Unwinding an x64 stack calls
// RtlLookupFunctionEntry, which reads the loader's inverted function table
// under an SRW lock LoadLibrary holds EXCLUSIVELY while inserting a module.
// Doing that with a thread SUSPENDED can wedge the process permanently - the
// diagnostic becoming the fault it was describing. So no thread is ever
// suspended across an unwind: the suspend window contains GetThreadContext and
// nothing else, and the walk happens afterwards. The report is also written
// incrementally, identifying half first, the same discipline
// crash_handler.cpp applies. See the phase-1 note in hang_watchdog.cpp.
//
// THE THRESHOLD, AND WHY 5 SECONDS.
//
//   - The GUI paces itself off vsync (glfwSwapInterval(1)), so the normal
//     interval between heartbeats is one display frame - 16.7 ms at 60 Hz.
//     5 s is 300 consecutive missed frames. Nothing that is merely slow gets
//     anywhere near it.
//   - The largest legitimate gap in a real run is start-up (GL context, font
//     atlas upload, first plugin scan), and it is MEASURED rather than
//     asserted from memory: `cascade --frames N` prints the worst heartbeat
//     gap it observed, and tests/test_diag_hang.cpp runs exactly that and
//     requires the measured worst gap to be under half this threshold. If a
//     future change makes a frame legitimately slow, that test goes red
//     before a user gets a false report.
//   - The real hangs above were 120 s and permanent. A threshold of 5 s
//     reports all of them, and reports them while the user is still looking
//     at the frozen window rather than after they have killed it.
//
// THE SHUTDOWN BUDGET, AND WHY THE TEARDOWN DOES NOT USE THOSE 5 SECONDS.
//
// The teardown deliberately runs UN-HEARTBEATEN, so a shutdown that wedges is
// reported like any other hang. That is not an oversight: the worst freeze
// this product ever shipped was 120 s inside a CAT client shutdown, and a
// watchdog stopped before the teardown would make that exact bug invisible.
//
// But the teardown is also where the deliberate bounded waits of 0.68.0 and
// after are spent. SoapySource::stop() will wait up to kControlLockWait
// (1.5 s) for the driver lock and then up to kVendorCallWait (1.5 s) for the
// one vendor call it makes with that lock held, and Pipeline::stop() up to
// kSourceJoinWait (3 s) for a source thread parked in a vendor read() - 6 s
// against a 5 s threshold, with nothing left at all for the DSP join, the
// config write, both ImGui shutdowns and GLFW. Even the first two of those,
// at 4.5 s, left only about 500 ms for the rest, and a field report proves
// that was already not enough: the threshold was crossed
// 478 ms after the second guard returned, and the watchdog filed a hang
// report against its OWN clean shutdown, naming the main thread as stuck in
// HangWatchdog::stop() - the join that was waiting for the capture being
// written. The application had already exited normally. Making every driver
// wait bounded made that false report near-certain on exactly the sessions
// the guards exist for.
//
// So the teardown gets its own, longer budget rather than being excused.
// AppWindow::run() calls beginShutdown() the instant the frame loop ends: it
// restarts the stall clock, so the teardown is measured from its own start
// rather than from the tail of the last frame, and raises the threshold to
// kShutdownThresholdMs. Nothing is paused, nothing is disarmed and the report
// path is untouched - a shutdown that genuinely wedges still crosses that
// budget and is still captured with every thread's stack, and threshold-ms in
// the report says which budget was in force. tests/test_shutdown_budget.cpp
// holds the arithmetic to every bounded wait it can FIND in the shipping
// source - not to a list of names it was given, which is how the third of
// them went uncounted the day it was added - and runs the shipped binary
// through both halves: a teardown longer than the frame threshold that must
// NOT report, and one longer than the shutdown budget that must.
//
// FALSE POSITIVES, and the three separate things that cause them:
//
//   1. A DEBUGGER at a breakpoint stops the GUI thread for as long as the
//      developer is reading. IsDebuggerPresent() suppresses reporting
//      outright - a break is not a hang.
//   2. A MODAL WINDOWS LOOP. Dragging or resizing the window, or holding a
//      system menu open, runs a nested message loop inside Windows and the
//      application's own loop does not turn over at all. This can legitimately
//      last minutes. GetGUIThreadInfo reports exactly this state
//      (GUI_INMOVESIZE / GUI_INMENUMODE / GUI_POPUPMENUMODE) and it is
//      suppressed.
//   2c. A NESTED LOOP THAT SETS NONE OF THOSE FLAGS. Pressing and holding a
//      caption button, or resting the pointer on the maximise button long
//      enough for Windows 11 to offer its snap layouts, parks the GUI thread
//      inside uxtheme's own tracking loop - reached through DefWindowProc,
//      which GLFW falls through to for every non-client message it does not
//      handle - and GUITHREADINFO has no bit for that state; its five flags
//      date from Windows 2000. One such stall of 5.4 s was reported and
//      uploaded as a hang on 2026-09-04, from a 0.75.0 build idling on the
//      signal generator, with the thread blocked in win32u.dll's message wait
//      the whole time. So the thread's own instruction pointer is consulted:
//      the application's frame loop only ever calls the NON-blocking
//      PeekMessage, and the only way its GUI thread can sit inside win32u's
//      wait syscalls for seconds is for a loop Windows owns to have taken the
//      thread over and gone idle in it. A stalled thread whose Rip is inside
//      win32u.dll is pumping, not hung, whatever the flags say.
//   2b. BLOCKING WORK THE APPLICATION ENTERS KNOWINGLY, which is WatchdogPause.
//      One path in this application takes it today, and it is named rather
//      than described in the abstract because a mitigation nobody calls is not
//      a mitigation: AppWindow::rescanPlugins(), which unloads and then
//      LoadLibrary-s every installed plugin on the GUI thread. Twelve modules
//      off a cold disk is a legitimate multi-second gap. Anything else added
//      later that blocks the GUI thread - a synchronous device open, a native
//      modal dialog, neither of which exists here yet (the Soapy open and scan
//      are async, and there is no native file dialog) - MUST take one too.
//      `cascade --frames N` prints how many pauses the run took and
//      tests/test_diag_hang.cpp requires at least one, so this stops being
//      true loudly rather than quietly.
//   3. THE WHOLE MACHINE STOPPING. Sleep, hibernate, or a VM being paused
//      freezes the watchdog thread too. The watchdog therefore checks its OWN
//      overshoot: if its 500 ms poll took longer than the threshold, the
//      process was not running and nothing is reported.
//   4. THE APPLICATION'S OWN TEARDOWN, which is the shutdown budget above -
//      and, at the very end of it, stop() itself. Once stop() has been asked
//      for, the GUI thread is inside it joining this thread; it is not
//      stalled, and a capture begun after that point can only describe the
//      watchdog. So the poll loop leaves rather than starting one. Nothing is
//      lost by that: a shutdown that really wedges never reaches stop() at
//      all, and is reported against the budget while it is still stuck.
//
// RECOVERY. A hang is reported once, and the watchdog then stays quiet until
// the heartbeat resumes. If the application comes back - and the 120 s CAT
// freeze did come back - the recovery is logged with the stall duration and
// the watchdog re-arms. The app is never killed; a diagnostic that terminated
// the user's session would be worse than the hang.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_HANG_WATCHDOG_HPP
#define CASCADE_CORE_HANG_WATCHDOG_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace cascade::core {

class HangWatchdog {
public:
    // See the header comment for the derivation. Milliseconds.
    static constexpr unsigned kDefaultThresholdMs = 5000;

    // How often the watchdog looks. Short enough that the reported stall
    // duration is meaningful, long enough to cost nothing. The wait is
    // INTERRUPTIBLE (see threadMain), so this is a polling interval and not a
    // floor on how long stop() takes.
    static constexpr unsigned kPollMs = 500;

    // THE SHUTDOWN BUDGET. See the header comment for the field report that
    // forced it; the three numbers are the arithmetic that report exposed.
    //
    // The bounded waits on the shutdown path, in milliseconds. Traced
    // through pipeline_.stop(), each spent at most once because the first
    // abandonment condemns the device and every later call on that path is
    // skipped rather than attempted:
    //
    //   kControlLockWait (1500, src/source/soapy_source.cpp) - the driver
    //     lock, waited for by SoapySource::stop() on the GUI thread;
    //   kVendorCallWait  (1500, src/source/soapy_source.cpp) - the
    //     abandonable deactivateStream inside stopLocked(), once that lock
    //     is held;
    //   kSourceJoinWait  (3000, src/core/pipeline.cpp) - the source thread,
    //     after both of the above.
    //
    // Copied rather than included, because all three are file-local
    // constants of modules this one must not depend on - so
    // tests/test_shutdown_budget.cpp DISCOVERS every bounded wait declared
    // under src/ and goes red both when this number stops matching their sum
    // and when a wait appears that it has never been introduced to. That
    // second half is not hypothetical: the test's first version scanned for
    // the first and third of the three by name, the same change-set added
    // kVendorCallWait, and the sum still came out at the 4500 this constant
    // then held - green, and wrong, on the very first drift. The teardown's
    // cost and the threshold it had to fit inside had already drifted apart
    // over three releases because nothing related the two.
    static constexpr unsigned kShutdownBoundedWaitsMs = 6000;

    // What the REST of the teardown gets: the DSP join, the crash-upload
    // cancel, two config writes, both ImGui shutdowns, glfwDestroyWindow and
    // glfwTerminate. None of them has a named bound and all of them together
    // are milliseconds in a healthy run - the field log that produced the
    // false report crossed the old threshold 478 ms after the last bounded
    // guard returned. Three seconds is six times that.
    static constexpr unsigned kShutdownReserveMs = 3000;

    // The threshold in force from beginShutdown() until stop(). At least
    // twice the worst LEGITIMATE shutdown (bounded waits plus reserve), so a
    // report written against it means something really did wedge rather than
    // that a guard was slow; and short enough that the report is written
    // while the user is still looking at a window that will not close.
    // tests/test_shutdown_budget.cpp pins the "at least twice".
    static constexpr unsigned kShutdownThresholdMs = 20000;

    HangWatchdog() = default;
    ~HangWatchdog();

    HangWatchdog(const HangWatchdog&) = delete;
    HangWatchdog& operator=(const HangWatchdog&) = delete;

    // `reportDir` empty DISABLES on-disk capture: the watchdog still runs and
    // still logs a recovered stall to the ring, but writes no file. Starting
    // twice is a no-op; the first configuration wins until stop().
    void start(const std::string& reportDir, unsigned thresholdMs = kDefaultThresholdMs);

    // THE TEARDOWN'S OWN BUDGET, and the answer to a false report the product
    // filed against its own clean shutdown. Called once, from the GUI thread,
    // the instant the frame loop ends and before any teardown work runs.
    //
    // Two effects, and both are needed. The stall clock restarts, so the
    // teardown is measured from its own beginning rather than from whatever
    // was left of the last frame's interval; and the threshold becomes
    // `thresholdMs`, which is sized against the bounded waits the shutdown
    // path actually contains (kShutdownBoundedWaitsMs above).
    //
    // WHAT IT DELIBERATELY DOES NOT DO is pause or disarm anything. The
    // teardown still runs un-heartbeaten and still reports, because a
    // shutdown that wedges is the fault class this product ships most; it
    // simply gets judged against a budget it can meet. Idempotent, and safe
    // to call whether or not the watchdog is running.
    void beginShutdown(unsigned thresholdMs = kShutdownThresholdMs);

    // Stops the watchdog thread and joins it. The poll wait is interruptible,
    // so this returns in about as long as an in-flight capture takes rather
    // than in the rest of a poll interval.
    void stop();

    // The threshold currently in force: what start() was given until
    // beginShutdown() is called, kShutdownThresholdMs after it. This is the
    // number a report writes as threshold-ms, so a reader can tell a stall in
    // the frame loop from one in the teardown.
    unsigned thresholdMs() const;

    // THE USER'S SWITCH, APPLIED TO A WATCHDOG THAT IS ALREADY RUNNING.
    //
    // start() is called once, before the frame loop, and is a no-op afterwards
    // - so for the whole life of a session the constructor argument above was
    // the only thing that ever decided whether a hang report reaches the disk.
    // Unticking Settings > Diagnostics disarmed the crash handler and the log
    // and left the watchdog writing hang-<pid>-N.txt on the next stall, which
    // is exactly what PRIVACY.md, README.md and docs/DIAGNOSTICS.md promise
    // does not happen ("off means off: no directory, no log file, no report").
    // The mirror image was equally wrong: switching diagnostics ON mid-session
    // armed everything except the one component that catches the fault this
    // product actually ships.
    //
    // Empty disables on-disk capture with immediate effect; a non-empty
    // directory re-arms it. The watchdog keeps running either way - a
    // recovered stall is still logged to the ring, which costs nothing and
    // leaves no file. Called from the GUI thread while the watchdog thread is
    // reading, hence the lock.
    void setReportDir(const std::string& reportDir);
    std::string reportDir() const;

    // Called once per rendered frame from the GUI thread. One relaxed store;
    // it must stay cheap enough that nobody is tempted to call it less often.
    //
    // `recordGap` false marks the thread alive WITHOUT folding the elapsed
    // interval into worstGapMs(). Exactly one caller passes false: the
    // --diag-stall test hook, whose deliberate multi-second stall would
    // otherwise become "the worst frame gap this build measured" and destroy
    // the measurement the threshold is justified against.
    void heartbeat(bool recordGap = true);

    bool running() const;

    // Bracket blocking work the application enters deliberately. Nested calls
    // are counted, so an inner pause cannot un-pause an outer one.
    void pause();
    void resume();

    // Hang reports written this session, and the newest one's path.
    unsigned reportsWritten() const;
    std::string lastReportPath() const;

    // Worst heartbeat interval observed, in milliseconds. This is the number
    // the threshold is justified against, and `cascade --frames N` prints it
    // so a test can hold the justification to a real measurement.
    double worstGapMs() const;

    // TEST HOOK. The three suppression rules above are single API calls
    // whose OUTCOME cannot be staged in a test - there is no way to attach a
    // debugger or start a window drag from ctest - but their WIRING can be,
    // and the wiring is the part that silently rots. NeverSuppress proves a
    // stall is reported; AlwaysSuppress proves the same stall is not.
    enum class SuppressionForTest { Normal, NeverSuppress, AlwaysSuppress };
    void setSuppressionForTest(SuppressionForTest mode);

    // TEST HOOK for rule 2c. Where a stalled GUI thread's instruction pointer
    // happens to be cannot be staged from ctest either - a test cannot park
    // its thread inside Windows' caption-button loop on demand - so this
    // names the module the peek would have found, and the rule is judged on
    // that name. It is consulted in every suppression mode, NeverSuppress
    // included, because that mode exists to switch off the two rules a test
    // cannot control (the debugger, the modal flags) and this one is the
    // rule under test. An empty name (the default) means "peek for real".
    void setStalledModuleForTest(const char* moduleName);

    // TEST HOOK, for the incremental-write discipline. A wedged unwinder
    // cannot be staged from ctest - it needs another thread to be inside the
    // loader at the instant of capture - but the property the incremental
    // write exists for can be: stop the capture where a wedge would stop it,
    // and the file already on disk must still name the bug.
    enum class CaptureAbortForTest { None, AfterHeader };
    void setCaptureAbortForTest(CaptureAbortForTest mode);

    // Pauses taken by this PROCESS, counted from process start rather than
    // from start(): the first blocking work in a session - the plugin scan -
    // happens while the window is still being built, before the frame loop and
    // therefore before the watchdog is running. `cascade --frames N` prints
    // this, so a test can hold "WatchdogPause is wired into the product" to an
    // observation instead of to a sentence in a header.
    unsigned pausesTaken() const;

private:
    void threadMain();
    void captureAllThreads(const std::string& path, double stalledMs);

    // The three false-positive rules of the header comment, in one place, so
    // the wiring a test CAN reach is the same wiring the debugger and modal
    // checks a test CANNOT reach go through.
    bool suppressed() const;
    // Rule 2c: whether the stalled GUI thread is parked in the window
    // manager's own message wait. Answers from the injected module name when
    // one is set, otherwise from one register read of the GUI thread.
    bool guiThreadIsPumping() const;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};

    // Milliseconds from a steady clock, stored as a double so the watchdog
    // never has to reason about a wrapping tick count.
    std::atomic<double> lastBeatMs_{0.0};
    std::atomic<double> worstGapMs_{0.0};
    std::atomic<int> paused_{0};
    std::atomic<bool> reported_{false};
    std::atomic<unsigned> reports_{0};
    std::atomic<int> suppression_{0};   // SuppressionForTest, as an int
    std::atomic<int> captureAbort_{0};  // CaptureAbortForTest, as an int
    // NOT reset by start(), unlike everything above it: see pausesTaken().
    std::atomic<unsigned> pauses_{0};

    // Set when the clock is restarted deliberately (start, and the release of
    // the last WatchdogPause), so the first frame afterwards is not counted as
    // a multi-second frame gap. Without it a device open would become "the
    // worst frame gap this build measured" and the threshold's justification
    // would be measured against the wrong thing.
    std::atomic<bool> skipGap_{true};

    // ATOMIC because beginShutdown() raises it from the GUI thread while the
    // watchdog thread is reading it every poll and writing it into a report.
    // It decides whether a report is written at all, so a torn read here
    // would be the false positive it exists to prevent, wearing a different
    // hat.
    std::atomic<unsigned> thresholdMs_{kDefaultThresholdMs};

    // THE POLL WAIT, MADE INTERRUPTIBLE. stop() used to set a flag that was
    // only read at the top of the loop while the loop slept in a plain
    // sleep_for, so asking the watchdog to stop cost the rest of a poll -
    // half a second of a shutdown, on average, for nothing. The flag is now
    // set under this mutex and the sleep is a wait_for on this variable, so
    // the notify cannot be missed by a thread that has just checked the flag
    // and not yet begun to wait.
    mutable std::mutex stopMutex_;
    std::condition_variable stopCv_;
    // Guarded because setReportDir() is the Settings checkbox on the GUI
    // thread and threadMain() reads it on the watchdog thread. It decides
    // whether a file is written at all, so a torn read here is a privacy bug
    // rather than a cosmetic one.
    mutable std::mutex dirMutex_;
    std::string reportDir_;
    // ATOMIC because heartbeat() (the GUI thread) writes it and the watchdog
    // thread reads it for the modal-loop check. A plain unsigned long here
    // would be a data race, and the value it feeds decides whether a report is
    // written at all.
    std::atomic<unsigned long> guiThreadId_{0};

    mutable std::mutex pathMutex_;
    std::string lastPath_;

    // Rule 2c's test injection. Guarded for the same reason reportDir_ is: a
    // test sets it on its own thread and the watchdog thread reads it.
    mutable std::mutex stalledModuleMutex_;
    std::string stalledModuleForTest_;
};

// Scope guard for blocking work: pauses on construction, resumes on
// destruction, including on the exception path.
class WatchdogPause {
public:
    explicit WatchdogPause(HangWatchdog& w) : w_(w) { w_.pause(); }
    ~WatchdogPause() { w_.resume(); }
    WatchdogPause(const WatchdogPause&) = delete;
    WatchdogPause& operator=(const WatchdogPause&) = delete;

private:
    HangWatchdog& w_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_HANG_WATCHDOG_HPP
