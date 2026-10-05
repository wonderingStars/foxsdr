// link_request_poll.hpp - looking for the beta-tester link-request file without
// ever making a frame wait for the disk.
//
// THE FIELD REPORT THIS EXISTS FOR. "hang ntdll.dll @ __std_fs_get_stats"
// (0.99.59, Windows 10.0.19045, an RTL-SDR open and streaming, 880 s uptime).
// The GUI thread's stack, resolved against the symbol archive, was
//
//   main -> AppWindow::run -> AppWindow::drawUi -> AppWindow::testerLinkPoll
//     -> core::claimLinkRequestFile -> __std_fs_get_stats -> KERNELBASE -> ntdll
//
// and it stayed there for five seconds or more. testerLinkPoll() runs once a
// second in EVERY session - tester or not, because it has to be listening
// before a link click can arrive - and what it asks is "does
// %APPDATA%\foxsdr\link-request exist". That was std::filesystem::exists() on
// the GUI thread, justified in a comment as "a stat() is cheap". It is cheap
// until the directory it names is on a redirected or network profile, a cloud-
// synced folder, behind an antivirus holding the directory, or on a disk that
// has spun down - and %APPDATA% is also where config.json is written, which
// 0.97.2 had already had to move off this thread (gui/config_writer.hpp) for
// the same reason. The poll's only job is to notice a file that, in nearly
// every second of nearly every session, is not there; it is the last thing
// that should be able to stop a window.
//
// WHY THE WATCHDOG PAUSE THAT PROTECTS RESCANPLUGINS WAS NOT THE ANSWER, AND IS
// NOT USED. A pause deletes the report and keeps the freeze (the argument in
// gui/audio_open.hpp), and a poll that fires every second would have the
// watchdog paused for the whole session.
//
// THE SHAPE, and it is config_writer.hpp's rather than audio_open.hpp's:
// nothing here waits, not even for a bound. AudioOpen::request() holds the
// requesting frame for up to 1.5 s because a user is waiting on a device; no
// one is waiting on this. A frame that finds the poll due calls request(),
// which starts a worker and returns; every frame calls poll(), which collects
// a finished worker and returns; neither can block. The only cost of a slow
// directory is that the answer arrives late.
//
// ONE WORKER AT A TIME, ALWAYS. request() while a probe is still in flight does
// nothing. A directory that never answers therefore costs one parked thread,
// not one per second for the rest of the session - which is the other way a
// once-a-second poll against a dead disk turns into a fault of its own.
//
// "STILL WORKING" HAS TO BE VISIBLE SOMEWHERE, and a poll nobody sees has no
// panel: takeNotice() reports once when a probe has been out longer than
// kStuckAfter and once more when it finally comes back, and AppWindow writes
// both to the log. That is the whole record a user's report will carry of a
// configuration directory that went away for a while.
//
// WHAT THE WORKER OWNS. Everything it touches, by value: the directory string
// and a copy of the claimer. It never sees the AppWindow. That is what lets
// reap() abandon a worker still blocked inside the filesystem at quit, exactly
// as ConfigWriter does, without the abandoned thread outliving anything it
// uses.
//
// THE TOKEN IS A CREDENTIAL (core/tester_link.hpp). It is returned by value
// from poll() and never logged here; the notices carry a duration and nothing
// else, and in particular not the directory, which is a user's profile path.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_LINK_REQUEST_POLL_HPP
#define CASCADE_GUI_LINK_REQUEST_POLL_HPP

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include "core/tester_link.hpp"

namespace cascade::gui {

class LinkRequestPoll {
public:
    // How long a probe may be out before it is called stuck in the log. The
    // hang watchdog's own threshold, deliberately: a probe that has been out
    // that long is one that, run on the GUI thread, would have been reported.
    static constexpr std::chrono::milliseconds kStuckAfter{5000};

    // What ~AppWindow spends on a probe still blocked, before the worker is
    // abandoned. Same 250 ms and the same argument as AudioOpen::kQuitGrace and
    // ConfigWriter::kQuitGrace: a probe a few milliseconds from finishing is
    // collected, and one parked inside the filesystem is let go rather than
    // joined. Spent in ~AppWindow, after HangWatchdog::stop(), so it is outside
    // the shutdown budget.
    static constexpr std::chrono::milliseconds kQuitGrace{250};

    // The once-a-frame ready-poll: zero by construction, named so a bare
    // literal duration is never what slips a real wait past
    // tests/test_shutdown_budget.cpp's scan (same constant, same reason, as
    // ConfigWriter::kNoWait).
    static constexpr std::chrono::milliseconds kNoWait{0};

    // The blocking work, injected: takes the configuration directory, returns
    // the app token a link-request file carried or "" for none. Runs on the
    // WORKER thread. Production is core::claimLinkRequestFile (below); a test
    // binds one that sleeps, and a sleeping claimer IS a slow disk as far as
    // the frame loop can tell.
    using Claimer = std::function<std::string(const std::string& configDir)>;

    // THE ONLY CALL TO core::claimLinkRequestFile IN src/gui, and
    // tests/test_link_request_poll.cpp holds it to that: every other file
    // under src/gui is scanned for a direct call.
    static std::string productionClaim(const std::string& configDir) {
        return cascade::core::claimLinkRequestFile(configDir);
    }

    // Bound to production, so there is no window in which an AppWindow polls
    // with nothing behind it and no call the owner can forget.
    LinkRequestPoll() : claimer_(&LinkRequestPoll::productionClaim) {}
    ~LinkRequestPoll() { reap(); }

    LinkRequestPoll(const LinkRequestPoll&) = delete;
    LinkRequestPoll& operator=(const LinkRequestPoll&) = delete;

    // Replaces the claimer; takes effect for the NEXT probe. A probe already
    // in flight keeps the copy it started with.
    void bind(Claimer claimer) { claimer_ = std::move(claimer); }

    // TEST-ONLY: how long before a probe is called stuck, so a test need not
    // wait out five real seconds to see the notice.
    void setStuckAfterForTest(std::chrono::milliseconds d) { stuckAfter_ = d; }

    // Ask whether the file is there. NEVER BLOCKS. A no-op (returns false)
    // while a previous probe is still out - see "ONE WORKER AT A TIME" - and
    // for an empty directory, which is the hermetic run's way of saying the
    // disk is not to be touched.
    bool request(const std::string& configDir) {
        if (future_.valid() || configDir.empty()) { return false; }
        Claimer claim = claimer_;
        startedAt_ = std::chrono::steady_clock::now();
        stuckReported_ = false;
        future_ = std::async(std::launch::async,
                             [claim = std::move(claim), dir = configDir]() -> std::string {
                                 // A throw out of a worker would be rethrown from
                                 // future::get() on the GUI thread, which is the
                                 // one place this file exists to keep clear.
                                 try {
                                     return claim ? claim(dir) : std::string();
                                 } catch (...) {
                                     return std::string();
                                 }
                             });
        return true;
    }

    // Once per frame. NEVER BLOCKS. Returns true on the frame a probe's answer
    // is collected, with `token` set to what it found ("" for nothing); false
    // on every other frame, including every frame a slow probe is still out.
    bool poll(std::string& token) {
        token.clear();
        if (!future_.valid()) { return false; }
        if (future_.wait_for(kNoWait) != std::future_status::ready) { return false; }
        token = future_.get();
        future_ = std::future<std::string>();
        if (stuckReported_) {
            recoveredAfterS_ = elapsedS();
            recoveredPending_ = true;
        }
        return true;
    }

    // True while a worker owns the directory.
    bool inFlight() const { return future_.valid(); }

    enum class Notice {
        None,
        Stuck,      // a probe has been out longer than kStuckAfter (once per probe)
        Recovered,  // a probe that was reported stuck has now come back (once)
    };

    // Once per frame, after poll(). Reports each state ONCE: a stuck probe is
    // one log line however many frames it is stuck for, not one per frame. In
    // Recovered, `seconds` is how long the whole probe took.
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
    // may not be coming back, which is the same hang wearing a different hat.
    // Idempotent.
    void reap() {
        if (!future_.valid()) { return; }
        if (future_.wait_for(kQuitGrace) == std::future_status::ready) {
            (void)future_.get();
            future_ = std::future<std::string>();
            return;
        }
        std::thread([f = std::move(future_)]() mutable { (void)f.get(); }).detach();
        future_ = std::future<std::string>();
    }

private:
    double elapsedS() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt_).count();
    }

    Claimer claimer_;
    std::future<std::string> future_;
    std::chrono::milliseconds stuckAfter_ = kStuckAfter;
    std::chrono::steady_clock::time_point startedAt_{};
    bool stuckReported_ = false;
    bool recoveredPending_ = false;
    double recoveredAfterS_ = 0.0;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_LINK_REQUEST_POLL_HPP
