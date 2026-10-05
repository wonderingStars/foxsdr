// disk_job.hpp - one piece of disk work, done on a worker, so that no frame ever
// waits for a disk (0.99.64).
//
// WHY THIS EXISTS. Three field freezes in 0.99.59 - 0.99.63 were one defect: a
// file-system call on the thread that draws the window, made against a disk that
// was slow to answer (a synchronised or network folder, a drive that had spun
// down, a scanner holding the file). The settings-folder poll became
// gui/link_request_poll.hpp, the recording's file open became gui/record_start.hpp
// and the config write became gui/config_writer.hpp. Each is the same class with
// the work and the result type written out; 0.99.64's audit of what was LEFT on
// the GUI thread (docs/DIAGNOSTICS.md, "The window does no disk work") found the
// same defect in a dozen more places - an I/Q file's header, a recordings
// listing, a screenshot, a picture, an exported list - and a dozen copies of
// record_start.hpp was not the answer. This is that class, once, for any
// "do this blocking thing and give me what it made".
//
// THE SHAPE is record_start.hpp's, line for line, minus what is specific to a
// recording:
//
//   * request() starts a worker and returns; poll() - once a frame - collects a
//     finished one and returns; NEITHER CAN BLOCK, not even for a bound. The only
//     cost of a slow disk is that the answer arrives late.
//   * ONE WORKER AT A TIME. request() while a worker is out does nothing and
//     returns false, so a disk that never answers costs one parked thread however
//     many times the button is pressed - and two requests can never be racing for
//     one file. A caller that must not lose a second request asks pending() first
//     and says so (the screenshot logs that it skipped one).
//   * THE WORKER OWNS EVERYTHING IT TOUCHES, BY VALUE. The work is a closure the
//     caller builds; it must capture copies, never `this` and never a reference
//     into the window. That is what lets reap() abandon a worker still inside the
//     filesystem at quit without the abandoned thread outliving anything it uses.
//   * A THROW IS CONTAINED. An exception out of the work would be rethrown from
//     future::get() on the GUI thread, the one place this file exists to keep
//     clear; it becomes a default-constructed Result and threw() says so.
//   * QUIT. reap() gives a worker still inside the filesystem kQuitGrace (250 ms)
//     and then ABANDONS it: the future is handed to a detached thread that waits
//     for the call to return and lets the Result go (which closes whatever it
//     holds). A join would be the same hang under another name. Spent from
//     ~DiskJob, i.e. from ~AppWindow after HangWatchdog::stop(), so outside the
//     shutdown budget; tests/test_shutdown_budget.cpp classifies it.
//   * "STILL WORKING" IS SAID, ONCE. poll() writes one warning to the log when a
//     job has been out longer than kStuckAfter and one line when it comes back;
//     neither names a file or a folder, which are in the user's profile. elapsedS()
//     is what a panel shows beside its "working" state.
//
// WHAT IT IS NOT: it does not pause the hang watchdog (a pause deletes the report
// and keeps the freeze - the argument in gui/audio_open.hpp - and the point of
// moving the work is that the window keeps drawing), and it does not order two
// writes to one file; a caller that needs "the newest content wins, and an older
// write never lands after a newer one" wants gui/background_saver.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DISK_JOB_HPP
#define CASCADE_GUI_DISK_JOB_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "core/diag_log.hpp"

namespace cascade::gui {

template <class Result>
class DiskJob {
public:
    // How long a job may be out before it is called stuck in the log. The hang
    // watchdog's own threshold, deliberately: a job out that long is one that,
    // run on the GUI thread, would have been reported.
    static constexpr std::chrono::milliseconds kStuckAfter{5000};

    // What ~AppWindow spends on a job still blocked, before the worker is
    // abandoned. The same 250 ms and the same argument as RecordStart::kQuitGrace,
    // LinkRequestPoll::kQuitGrace and ConfigWriter::kQuitGrace.
    static constexpr std::chrono::milliseconds kQuitGrace{250};

    // The once-a-frame ready-poll: zero by construction, named so that a bare
    // literal duration is never what slips a real wait past
    // tests/test_shutdown_budget.cpp's scan.
    static constexpr std::chrono::milliseconds kNoWait{0};

    // The blocking work. Runs on the WORKER thread; owns its inputs by value.
    using Work = std::function<Result()>;

    // `label` names the job in the log ("I/Q file open"); it is a fixed English
    // string and never a path. It must outlive the job (a literal does).
    explicit DiskJob(const char* label) : label_(label) {}
    ~DiskJob() { reap(); }

    DiskJob(const DiskJob&) = delete;
    DiskJob& operator=(const DiskJob&) = delete;

    // TEST-ONLY: how long before a job is called stuck, so a test need not wait
    // out five real seconds to see the notice.
    void setStuckAfterForTest(std::chrono::milliseconds d) { stuckAfter_ = d; }

    // Start the work. NEVER BLOCKS. False, and nothing started, while a previous
    // job is still out or when `work` is empty.
    bool request(Work work) {
        if (future_.valid() || !work) { return false; }
        stuckReported_ = false;
        startedAt_ = std::chrono::steady_clock::now();
        threw_ = std::make_shared<std::atomic<bool>>(false);
        future_ = std::async(std::launch::async,
                             [work = std::move(work), threw = threw_]() -> Result {
                                 try {
                                     return work();
                                 } catch (...) {
                                     threw->store(true);
                                     return Result{};
                                 }
                             });
        return true;
    }

    // Once per frame. NEVER BLOCKS. True on the frame a finished job is
    // collected, with `out` filled in; false on every other frame, including
    // every frame a slow job is still out. Also writes the two log lines (see
    // "STILL WORKING").
    bool poll(Result& out) {
        if (!future_.valid()) { return false; }
        if (future_.wait_for(kNoWait) != std::future_status::ready) {
            noteStuck();
            return false;
        }
        // The whole job's duration, taken while it is still the job being
        // collected: elapsedS() answers 0 once it is gone.
        const double tookS = sinceStartS();
        out = future_.get();
        future_ = std::future<Result>();
        lastThrew_ = threw_ != nullptr && threw_->load();
        threw_.reset();
        if (stuckReported_) {
            cascade::core::diagLogf("%s: finished after %.0f s", label_, tookS);
            stuckReported_ = false;
        }
        return true;
    }

    // True while a worker owns what it was asked to touch.
    bool pending() const { return future_.valid(); }

    // How long the job now out has been out; 0 when none is.
    double elapsedS() const { return future_.valid() ? sinceStartS() : 0.0; }

    // True when the job collected by the last poll() threw (its Result is then
    // default-constructed).
    bool threw() const { return lastThrew_; }

    // QUIT. A bounded grace, then the worker is abandoned rather than joined -
    // ~future would block the destructor for the rest of a filesystem call that
    // may not be coming back. Idempotent.
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

    void noteStuck() {
        if (stuckReported_ ||
            std::chrono::steady_clock::now() - startedAt_ < stuckAfter_) {
            return;
        }
        stuckReported_ = true;
        cascade::core::diagWarnf(
            "%s: has not finished for %.0f s - it is waiting on a worker thread, the window "
            "is not",
            label_, sinceStartS());
    }

    const char* label_;
    std::future<Result> future_;
    std::shared_ptr<std::atomic<bool>> threw_;
    std::chrono::milliseconds stuckAfter_ = kStuckAfter;
    std::chrono::steady_clock::time_point startedAt_{};
    bool stuckReported_ = false;
    bool lastThrew_ = false;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DISK_JOB_HPP
