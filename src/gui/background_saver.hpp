// background_saver.hpp - a small file that is saved when it changes, written off
// the GUI thread, newest content winning (0.99.64).
//
// WHAT IT IS FOR. The bookmark list (bookmarks.json) and the waterfall markers
// (markers.json) are saved a moment after the user last changed them, by a
// temporary file written and renamed over the target in the settings folder.
// AppWindow did both on the GUI thread, from drawUi, every frame it had one due -
// the same synchronous write that froze a window in 0.96.3 for the CONFIG file and
// that gui/config_writer.hpp moved off the thread. The two lists live in the same
// folder, on the same disk, so they were the same freeze waiting for the same slow
// answer.
//
// THE SHAPE IS ConfigWriter's, USED AS IT IS. That class is already "write this
// text to this path on one worker, coalesce a burst to the last content asked for,
// never block the caller, and drain once at exit within a bound"; nothing about it
// is about a config. This wrapper adds only what a caller of two small files wants
// beside it:
//
//   * ORDER AND LOSS. One write is ever out. A request made while it is out is
//     held (replacing any request already held) and starts the moment the first
//     one comes back - so the file on disk is always some request's complete text,
//     requests land in the order they were made, an older write can never land
//     after a newer one, and the last request is never dropped. Two quick edits
//     therefore end as the SECOND edit's content.
//   * THE SNAPSHOT IS TAKEN BY THE CALLER, ON THE GUI THREAD. request() takes the
//     finished text, not a pointer to the list, so the worker never reads a
//     container the GUI is changing.
//   * THE RESULT COMES BACK ON A LATER FRAME. poll(), once a frame, reports the
//     write that finished with the words the writer gave - the same words the
//     synchronous save gave - so a failure is shown exactly as it was.
//   * "STILL WRITING" IS SAID, ONCE: a warning when a write has been out for
//     kStuckAfter, a line when it comes back, neither naming a file or a folder.
//   * EXIT. finishOrAbandon(bound) is ConfigWriter's drain: the last request gets
//     a real chance to land within the bound, and past it the write is abandoned
//     (the caller logs that) rather than the application being held.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_BACKGROUND_SAVER_HPP
#define CASCADE_GUI_BACKGROUND_SAVER_HPP

#include <chrono>
#include <string>
#include <utility>

#include "core/diag_log.hpp"
#include "gui/config_writer.hpp"

namespace cascade::gui {

class BackgroundSaver {
public:
    // The age at which a write still out is called stuck in the log: the hang
    // watchdog's own threshold, as every worker in gui/ uses.
    static constexpr std::chrono::milliseconds kStuckAfter{5000};

    using Writer = ConfigWriter::Writer;

    // `label` names the file in the log ("bookmarks"): a fixed English word, never
    // a path. It must outlive the saver (a literal does).
    BackgroundSaver(const char* label, Writer writer) : label_(label) {
        writer_.bind(std::move(writer));
    }

    BackgroundSaver(const BackgroundSaver&) = delete;
    BackgroundSaver& operator=(const BackgroundSaver&) = delete;

    // TEST-ONLY: replaces the writer (takes effect for the next write started)
    // and shortens the stuck age.
    void bind(Writer writer) { writer_.bind(std::move(writer)); }
    void setStuckAfterForTest(std::chrono::milliseconds d) { stuckAfter_ = d; }

    // THIS FILE MUST NOT BE WRITTEN THIS SESSION (0.99.65): it is damaged and could not be kept
    // aside, so a save would destroy the only copy (core/damaged_file.hpp). Every request after
    // this is dropped; the red line the list already shows stays up, since no write ever lands to
    // clear it.
    void forbidWrites(std::string reason) { writer_.forbidWrites(std::move(reason)); }
    bool writesForbidden() const { return writer_.writesForbidden(); }

    // Ask for `text` to be written to `path`. NEVER BLOCKS.
    void request(std::string path, std::string text) {
        if (!writer_.inFlight()) { busySince_ = std::chrono::steady_clock::now(); }
        writer_.requestAsync(std::move(path), std::move(text));
    }

    // Once per frame. NEVER BLOCKS. True on the frame a finished write is
    // collected, with `ok` and `error` as the writer left them (`error` is empty
    // on success); false on every other frame. A write held behind it starts here.
    bool poll(bool& ok, std::string& error) {
        if (writer_.poll()) {
            ok = writer_.lastOk();
            error = ok ? std::string() : writer_.lastError();
            if (stuckReported_) {
                cascade::core::diagLogf("%s: the save finished after %.0f s", label_,
                                        sinceBusyS());
                stuckReported_ = false;
            }
            // A held write has just been started: its clock starts now.
            busySince_ = std::chrono::steady_clock::now();
            return true;
        }
        if (writer_.inFlight() && !stuckReported_ &&
            std::chrono::steady_clock::now() - busySince_ >= stuckAfter_) {
            stuckReported_ = true;
            cascade::core::diagWarnf(
                "%s: the save has not finished for %.0f s - it is waiting on a worker "
                "thread, the window is not",
                label_, sinceBusyS());
        }
        return false;
    }

    // True while a worker owns the write, or one is held behind it.
    bool inFlight() const { return writer_.inFlight(); }
    bool hasQueued() const { return writer_.hasQueued(); }

    // How many writes have been collected - how a test sees the cost.
    unsigned completed() const { return writer_.completed(); }

    // THE EXIT DRAIN: waits up to `bound` for the write in flight and for the one
    // held behind it. True when nothing is left (lastOk()/lastError() say how the
    // last one went); false when something was abandoned.
    bool finishOrAbandon(std::chrono::milliseconds bound) {
        return writer_.finishOrAbandon(bound);
    }
    bool lastOk() const { return writer_.lastOk(); }
    const std::string& lastError() const { return writer_.lastError(); }

private:
    double sinceBusyS() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - busySince_)
            .count();
    }

    const char* label_;
    ConfigWriter writer_;
    std::chrono::milliseconds stuckAfter_ = kStuckAfter;
    std::chrono::steady_clock::time_point busySince_{};
    bool stuckReported_ = false;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_BACKGROUND_SAVER_HPP
