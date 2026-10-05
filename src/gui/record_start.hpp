// record_start.hpp - opening a recording's file without ever making a frame
// wait for the disk.
//
// THE FIELD REPORT THIS EXISTS FOR. A freeze report from 0.99.58 (Windows 11,
// 151 s into the session). The GUI thread's stack, resolved against the symbol
// archive, was
//
//   main -> AppWindow::run -> AppWindow::drawUi -> AppWindow::drawMenuColumn
//     -> AppWindow::drawRecorderSection -> core::Recorder::start
//       -> the C runtime's file open -> KERNELBASE -> ntdll
//
// and it stayed there past the hang watchdog's five seconds. The Record
// button's handler called Recorder::start on the frame that was drawing the
// button, and start() creates the recordings directory and opens the file
// before it returns: two synchronous filesystem calls that last as long as the
// disk takes to answer. A synchronised or network folder, a drive that has
// spun down and an antivirus holding the path are all ordinary ways for that
// to be seconds. What the user saw was a window that stopped drawing the moment
// they pressed Record. (core/recorder.hpp, "Slow disks", says which three
// steps of start() wait and how it was taken apart.)
//
// WHY THE WATCHDOG PAUSE THAT PROTECTS RESCANPLUGINS IS NOT THE ANSWER. A pause
// deletes the report and keeps the freeze (the argument in gui/audio_open.hpp):
// the window would still not draw. The point is that it does.
//
// THE SHAPE, and it is link_request_poll.hpp's and config_writer.hpp's, not
// audio_open.hpp's: nothing here waits, not even for a bound. AudioOpen holds
// the requesting frame for up to 1.5 s because a user is waiting on a device;
// here the user has just pressed a button and is looking at a window that can
// say "starting". request() starts a worker and returns, poll() - once a frame
// - collects a finished one and returns, and neither can block. The only cost
// of a slow disk is that the recording starts late, and the panel says so.
//
// WHAT THE WORKER DOES, AND WHAT IT DOES NOT. It runs the Recorder's opener,
// the blocking step, on the OpenRequest it was handed and returns an
// OpenedFile: a file that is open with its header written and flushed. It
// never sees a Recorder, an AppWindow or the pipeline. The caller then arms
// the recorder from that file (Recorder::begin) ON ITS OWN THREAD and only
// then installs the pipeline tap, so Pipeline::set*Recorder's contract - start
// FIRST, tap second, no write against a recorder that is not accepting - holds
// exactly as it did when the open was inline.
//
// ONE WORKER AT A TIME, ALWAYS - the second Record. request() while a worker is
// out does nothing and returns false, so a disk that never answers costs one
// parked thread, however many times the button, the Record key or a browser
// asks. It also means two opens can never be racing to the same file name.
//
// STOP WHILE IT IS OPENING. cancel() marks the start withdrawn; the worker
// cannot be interrupted (it is inside the filesystem), so it is let finish and
// its file is CLOSED UNUSED by poll() - no recorder is armed, no tap installed.
// pending() stays true until it comes back, because the worker still owns the
// path: a Record pressed in that gap is refused rather than started alongside
// it. What a withdrawn start leaves on disk is the zero-sample WAV that Record
// followed at once by Stop has always left; nothing is deleted.
//
// QUIT. reap() gives a worker still inside the filesystem kQuitGrace and then
// ABANDONS it: the future is handed to a detached thread, which waits for the
// open to return and lets the OpenedFile go, closing the file. A join would be
// the same hang under another name. The worker owns everything it touches by
// value, which is what makes that safe.
//
// "STILL OPENING" HAS TO BE VISIBLE SOMEWHERE. takeNotice() reports once when
// an open has been out longer than kStuckAfter and once more when it comes back
// - the AppWindow writes both to the log, naming neither the directory nor the
// file, which are in the user's profile - and elapsedS() is what the panel
// shows beside its "starting" button.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_RECORD_START_HPP
#define CASCADE_GUI_RECORD_START_HPP

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include "core/health_events.hpp"
#include "core/recorder.hpp"

namespace cascade::gui {

class RecordStart {
public:
    // How long an open may be out before it is called stuck in the log. The hang
    // watchdog's own threshold, deliberately: an open that has been out that long
    // is one that, run on the GUI thread, would have been reported.
    static constexpr std::chrono::milliseconds kStuckAfter{5000};

    // What ~AppWindow spends on an open still blocked, before the worker is
    // abandoned. Same 250 ms and the same argument as LinkRequestPoll::kQuitGrace
    // and ConfigWriter::kQuitGrace; spent after HangWatchdog::stop(), so outside
    // the shutdown budget.
    static constexpr std::chrono::milliseconds kQuitGrace{250};

    // The once-a-frame ready-poll: zero by construction, named so that a bare
    // literal duration is never what slips a real wait past
    // tests/test_shutdown_budget.cpp's scan.
    static constexpr std::chrono::milliseconds kNoWait{0};

    // What a finished open hands back.
    struct Result {
        // True: `file` is open, its header on disk, ready for Recorder::begin().
        bool ok = false;
        // True: the take was stopped while it was opening. `file` has already
        // been closed unused and there is nothing to arm; `ok` is false and
        // `error` empty whatever the open itself did.
        bool cancelled = false;
        // The reason, exactly as Recorder::start would have given it, when the
        // open failed and the take was not cancelled.
        std::string error;
        cascade::core::Recorder::OpenedFile file;
    };

    RecordStart() = default;
    ~RecordStart() { reap(); }

    RecordStart(const RecordStart&) = delete;
    RecordStart& operator=(const RecordStart&) = delete;

    // TEST-ONLY: how long before an open is called stuck, so a test need not
    // wait out five real seconds to see the notice.
    void setStuckAfterForTest(std::chrono::milliseconds d) { stuckAfter_ = d; }

    // Ask for the file to be opened. NEVER BLOCKS. The opener is the Recorder's
    // (Recorder::opener()), copied here so the worker owns its own; a test binds
    // one that sleeps. False, and nothing started, while a previous open is still
    // out - see "ONE WORKER AT A TIME" - which includes one that was cancelled
    // and has not yet come back.
    bool request(cascade::core::Recorder::Opener opener,
                 cascade::core::Recorder::OpenRequest req) {
        if (future_.valid()) { return false; }
        cancelled_ = false;
        stuckReported_ = false;
        startedAt_ = std::chrono::steady_clock::now();
        future_ = std::async(
            std::launch::async,
            [opener = std::move(opener), req = std::move(req)]() -> Result {
                Result r;
                // A throw out of a worker would be rethrown from future::get()
                // on the GUI thread, which is the one place this file exists to
                // keep clear.
                try {
                    if (!opener) {
                        r.error = "recorder: no way to open the file";
                    } else {
                        r.ok = opener(req, r.file, r.error);
                    }
                } catch (...) {
                    r.ok = false;
                    r.file = cascade::core::Recorder::OpenedFile{};
                    r.error = "recorder: the file could not be opened";
                }
                if (!r.ok) { r.file = cascade::core::Recorder::OpenedFile{}; }
                return r;
            });
        return true;
    }

    // Withdraw the take. NEVER BLOCKS. A no-op when nothing is out. See "STOP
    // WHILE IT IS OPENING".
    void cancel() {
        if (future_.valid()) { cancelled_ = true; }
    }

    // Once per frame. NEVER BLOCKS. True on the frame a finished open is
    // collected, with `out` filled in; false on every other frame, including
    // every frame a slow open is still out.
    bool poll(Result& out) {
        out = Result{};
        if (!future_.valid()) { return false; }
        if (future_.wait_for(kNoWait) != std::future_status::ready) { return false; }
        // The whole open's duration, taken while the future is still the open
        // that is being collected: elapsedS() answers 0 once it is gone.
        const double tookS = sinceStartS();
        out = future_.get();
        future_ = std::future<Result>();
        if (cancelled_) {
            out.cancelled = true;
            out.ok = false;
            out.error.clear();
            out.file = cascade::core::Recorder::OpenedFile{};  // closes it, unused
        }
        cancelled_ = false;
        // A START THAT FAILED (the file could not be opened) is COUNTED,
        // ANONYMOUSLY (0.99.64, core/health_events.hpp): one count, nothing of
        // the reason, the path or the file. A take the user withdrew while it
        // opened is not a failure and is not counted. Here, where the answer is
        // collected - once, on the thread that owns the recorder, never in the
        // worker that was inside the filesystem.
        if (!out.ok && !out.cancelled) { cascade::core::health::noteRecordFailed(); }
        if (stuckReported_) {
            recoveredAfterS_ = tookS;
            recoveredPending_ = true;
        }
        return true;
    }

    // True while a worker owns the path, a cancelled one included.
    bool pending() const { return future_.valid(); }
    // True while an open is out and the take has not been withdrawn: what the
    // panel shows as "starting". pending() && cancelled() is "cancelling".
    bool starting() const { return future_.valid() && !cancelled_; }
    bool cancelled() const { return future_.valid() && cancelled_; }

    // How long the open now out has been out; 0 when none is.
    double elapsedS() const { return future_.valid() ? sinceStartS() : 0.0; }

    enum class Notice {
        None,
        Stuck,      // an open has been out longer than kStuckAfter (once per open)
        Recovered,  // an open that was reported stuck has now come back (once)
    };

    // Once per frame, after poll(). Reports each state ONCE: a stuck open is one
    // log line however many frames it is stuck for, not one per frame. In
    // Recovered, `seconds` is how long the whole open took.
    Notice takeNotice(double& seconds) {
        seconds = 0.0;
        if (recoveredPending_) {
            recoveredPending_ = false;
            seconds = recoveredAfterS_;
            return Notice::Recovered;
        }
        if (future_.valid() && !stuckReported_ &&
            std::chrono::steady_clock::now() - startedAt_ >= stuckAfter_) {
            stuckReported_ = true;
            seconds = elapsedS();
            return Notice::Stuck;
        }
        return Notice::None;
    }

    // QUIT. A bounded grace, then the worker is abandoned rather than joined -
    // ~future would block the destructor for the rest of a filesystem call that
    // may not be coming back. The abandoned worker's thread waits for it, and
    // the file it eventually opens is closed with the Result it is returned in.
    // Idempotent.
    void reap() {
        if (!future_.valid()) { return; }
        if (future_.wait_for(kQuitGrace) == std::future_status::ready) {
            (void)future_.get();
            future_ = std::future<Result>();
            return;
        }
        std::thread([f = std::move(future_)]() mutable { (void)f.get(); }).detach();
        future_ = std::future<Result>();
    }

private:
    double sinceStartS() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_)
            .count();
    }

    std::future<Result> future_;
    std::chrono::milliseconds stuckAfter_ = kStuckAfter;
    std::chrono::steady_clock::time_point startedAt_{};
    bool cancelled_ = false;
    bool stuckReported_ = false;
    bool recoveredPending_ = false;
    double recoveredAfterS_ = 0.0;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_RECORD_START_HPP
