// config_writer.hpp - saving the config file without blocking the GUI thread.
//
// THE FIELD REPORT THIS EXISTS FOR. "hang ntdll.dll @
// cascade::core::ConfigStore::save" (0.96.3, Windows 10.0.26200). The log
// tail shows a mode change, two "gui thread recovered after a stall"
// warnings, and a plugin rescan just before the report - i.e. this was the
// PERIODIC debounced save (AppWindow::maybeSaveConfig, called once a frame
// from the frame loop), not a one-off. Its stack was
//
//   AppWindow::run -> AppWindow::maybeSaveConfig -> ConfigStore::save
//     -> std::ofstream::write/flush -> ucrtbase's buffered file I/O -> ntdll
//
// and it stopped there. ConfigStore::save() opens a temp file, writes the
// JSON, flushes, and renames it over the target (see core/config.hpp for why
// that shape exists); every one of those is a synchronous filesystem call
// that blocks the calling thread for as long as the OS and the disk need -
// and %APPDATA% can be a cloud-synced folder (OneDrive, in this product's own
// case), can sit behind an antivirus holding a handle on the file, or can
// simply be a slow spinning disk. None of that is exotic, and all of it is
// something the application has to survive rather than merely hope never
// happens - exactly the argument gui/audio_open.hpp makes for the audio
// device open this class is modelled on.
//
// THE SHAPE, straight from that file. THE SAVE RUNS ON A WORKER
// (std::async), so the GUI thread is never inside the write. Unlike
// AudioOpen, nothing here makes the requesting frame wait for an answer -
// there is no user waiting on a config save the way there is on a device
// open, and the periodic caller (maybeSaveConfig) must not cost the frame
// loop so much as a bounded pause every debounce window a slow disk stays
// slow. So request()/poll() never block at all: a request starts the worker
// if none is running, or COALESCES into a pending one if a write is already
// in flight - the burst of saves a dragged slider produces collapses to the
// LAST content asked for, exactly like AudioOpen's queued device opens
// collapse to the last click. poll(), called once a frame, collects a
// finished worker and starts whatever was queued behind it.
//
// THE ONE PLACE THIS DOES BLOCK is finishOrAbandon(), and it is deliberate:
// the application needs the LAST save to have a real chance of landing before
// it exits, or a session's final state (and the clean-exit marker that says
// this run did not crash) is lost every time the disk is merely slow rather
// than gone. AppWindow::run() calls it once, late in the teardown, bounded by
// kSaveBound and charged in the shutdown budget
// (HangWatchdog::kShutdownBoundedWaitsMs, tests/test_shutdown_budget.cpp) -
// so a save that outlives that bound is ABANDONED rather than waited for,
// the same choice AudioOpen::reap() makes for a wedged device open at quit,
// and for the same reason: a slow disk must cost a line in the log, never
// the application's exit.
//
// WHAT THE ON-DISK FORMAT IS. Unchanged. core::ConfigStore::serialize()
// builds the exact JSON text save() has always produced, and
// core::ConfigStore::writeFile() is the exact atomic temp-file-then-rename
// save() has always performed (see core/config.hpp) - this class only moves
// WHERE writeFile() runs, never what it does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_CONFIG_WRITER_HPP
#define CASCADE_GUI_CONFIG_WRITER_HPP

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>

namespace cascade::gui {

class ConfigWriter {
public:
    // THE SHUTDOWN DRAIN. How long AppWindow::run()'s teardown waits, once,
    // for the LAST queued save to land before giving up on it. A healthy
    // write is sub-millisecond, so this is never spent in a normal session;
    // it exists for the slow-disk case the field report was, and it is
    // comfortably under HangWatchdog::kDefaultThresholdMs (5000) - the same
    // margin audio_open.hpp's kOpenBound keeps for the same reason. Also used
    // as the bound for a specific caller that wants to know a save has
    // landed (there is none in this product today; it is here because a
    // future one should reach for this rather than inventing its own
    // number).
    static constexpr std::chrono::milliseconds kSaveBound{1500};

    // WHAT ~AppWindow SPENDS on a save still blocked when the object is
    // destroyed, before the worker is abandoned. Same value and the same
    // argument as AudioOpen::kQuitGrace: runs from ~ConfigWriter, which is
    // reached from ~AppWindow after watchdog_.stop() - outside the shutdown
    // budget - and it exists only as a safety net behind the explicit
    // kSaveBound drain in run(); a session that reaches this at all has
    // already had its one deliberate chance.
    static constexpr std::chrono::milliseconds kQuitGrace{250};

    // The once-a-frame ready-poll of poll(): zero by construction, named for
    // the same reason app_window.cpp's kNoWait and audio_open.hpp's kNoWait
    // are - a bare literal duration in the source is how a real bounded wait
    // slips past tests/test_shutdown_budget.cpp's scan unnoticed.
    static constexpr std::chrono::milliseconds kNoWait{0};

    // The blocking work, injected: writes `text` to `path` and returns
    // whether it succeeded, `error` set on failure. Runs on the WORKER
    // thread. Production code binds core::ConfigStore::writeFile; tests bind
    // a fake that can block on demand.
    using Writer = std::function<bool(const std::string& path, const std::string& text,
                                       std::string& error)>;

    ConfigWriter() = default;

    // Abandons rather than joins a save still in flight - see kQuitGrace
    // above and the file header. Safe because writeFile() touches only the
    // path and text it was given by value; there is no object lifetime this
    // could outlive the way AudioOpen's opener can outlive a sink.
    ~ConfigWriter() { finishOrAbandon(kQuitGrace); }

    ConfigWriter(const ConfigWriter&) = delete;
    ConfigWriter& operator=(const ConfigWriter&) = delete;

    void bind(Writer writer) { writer_ = std::move(writer); }

    // Ask for `text` to be written to `path`. NEVER BLOCKS, and takes no
    // watchdog pause - see "THE SHAPE" in the file header for why this
    // differs from AudioOpen::request(). A request made while another write
    // is already in flight is COALESCED into the pending one rather than
    // queued behind it as a second write: only the latest survives, because
    // a burst of saves means the last state is the only one that matters.
    void requestAsync(std::string path, std::string text) {
        if (inFlight()) {
            queuedPath_ = std::move(path);
            queuedText_ = std::move(text);
            hasQueued_ = true;
            return;
        }
        start(std::move(path), std::move(text));
    }

    // Once per frame. Collects a worker that has finished and starts
    // whatever was coalesced behind it. Returns true when a result was
    // collected, so the caller can react to lastOk()/lastError() on exactly
    // the frames that matter.
    bool poll() {
        if (!future_.valid()) { return false; }
        if (future_.wait_for(kNoWait) != std::future_status::ready) { return false; }
        collect();
        if (hasQueued_) {
            hasQueued_ = false;
            start(std::move(queuedPath_), std::move(queuedText_));
        }
        return true;
    }

    // True while a worker owns the write (or one is queued behind it - see
    // inFlight() below, which is the future alone; hasQueued() names the
    // queued case separately for a caller that needs to tell them apart).
    bool inFlight() const { return future_.valid(); }
    bool hasQueued() const { return hasQueued_; }

    // The last completed write. Meaningless until completed() is non-zero.
    bool lastOk() const { return lastOk_; }
    const std::string& lastError() const { return lastError_; }
    unsigned completed() const { return completed_; }

    // THE SHUTDOWN DRAIN. Waits up to `bound` for whatever is in flight; if
    // it finishes AND nothing was coalesced behind it, returns true - the
    // config is on disk (or lastOk() says why it is not). If something WAS
    // coalesced behind it, that write is started and waited for too, within
    // the SAME overall bound - the case that matters most, since the last
    // request queued during a slow write is usually the one carrying the
    // clean-exit marker. Past the bound, whatever is left (in flight, or
    // queued and never started) is ABANDONED exactly like AudioOpen::reap():
    // detached onto its own thread so its eventual completion cannot block
    // anything, the future released, and the queue cleared. Returns false on
    // that path so the caller can log it - see the file header.
    bool finishOrAbandon(std::chrono::milliseconds bound = kSaveBound) {
        const auto deadline = std::chrono::steady_clock::now() + bound;
        while (future_.valid()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline || future_.wait_for(deadline - now) != std::future_status::ready) {
                abandon();
                return false;
            }
            collect();
            if (hasQueued_) {
                hasQueued_ = false;
                start(std::move(queuedPath_), std::move(queuedText_));
                continue;
            }
        }
        return true;
    }

private:
    struct Outcome {
        bool ok = false;
        std::string error;
    };

    void start(std::string path, std::string text) {
        Writer w = writer_;
        future_ = std::async(std::launch::async,
                              [w, path = std::move(path), text = std::move(text)]() mutable -> Outcome {
                                  Outcome o;
                                  o.ok = w ? w(path, text, o.error) : false;
                                  return o;
                              });
    }

    void collect() {
        const Outcome o = future_.get();
        lastOk_ = o.ok;
        lastError_ = o.error;
        future_ = std::future<Outcome>();
        ++completed_;
    }

    // Detaches a still-running worker onto its own thread so its eventual
    // completion (or non-completion, against a target that is simply gone)
    // cannot block anything here - the same shape as AudioOpen::reap().
    void abandon() {
        if (future_.valid()) {
            std::thread([f = std::move(future_)]() mutable { (void)f.get(); }).detach();
            future_ = std::future<Outcome>();
        }
        hasQueued_ = false;
    }

    Writer writer_;
    std::future<Outcome> future_;
    std::string queuedPath_;
    std::string queuedText_;
    bool hasQueued_ = false;
    bool lastOk_ = false;
    std::string lastError_;
    unsigned completed_ = 0;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_CONFIG_WRITER_HPP
