// audio_open.hpp - opening an audio output device without freezing the
// application while Windows thinks about it.
//
// WHAT THIS IS FOR. Field report "hang ntdll.dll @ InitializeWaveHandles"
// (0.96.4, Windows 11 26200, an RTL-SDR Blog V4, 350 s uptime). The user picked
// an output device in the Sinks panel; the GUI thread went
//
//   AppWindow::drawSinksSection -> Pipeline::openAudioDevice
//     -> sink::AudioOut::open -> Pa_OpenStream -> InitializeWaveHandles
//       -> winmmbase -> msacm32 -> wdmaud -> KERNELBASE -> ntdll
//
// and stopped there. Pa_OpenStream on the WMME host API is waveOutOpen, which
// blocks the calling thread for as long as the audio service and the endpoint
// driver need - and when an endpoint is mid-re-enumeration that is not
// milliseconds. The watchdog filed a hang at five seconds; the application's
// own log records the GUI thread coming back 57 seconds later. Nothing had
// crashed and nothing was deadlocked: the frame loop was simply inside a
// synchronous device open, exactly the case core/hang_watchdog.hpp's
// false-positive rule 2b names in advance ("a synchronous device open ... MUST
// take one too").
//
// SO WHY NOT JUST A WatchdogPause. Because a pause alone would delete the
// REPORT and keep the FREEZE. Fifty-seven seconds of dead window is the user's
// complaint; the hang report is only how we came to hear about it. And rule 2b
// itself says a pause that never expires is the watchdog switched off, which is
// what bracketing an unbounded call would amount to - a device that never comes
// back would then be invisible rather than merely unreported.
//
// WHAT THIS DOES INSTEAD, and both halves are needed:
//
//   THE OPEN RUNS ON A WORKER. std::async, polled once per frame, the same
//   shape the radio device open and the SoapySDR scan have used since 0.90.0
//   (AppWindow::pollSourceAsync). The GUI thread therefore cannot be inside
//   waveOutOpen at all.
//
//   THE GUI WAITS A BOUND FOR IT, UNDER A PAUSE. A healthy open is tens of
//   milliseconds, and a device switch that took a frame to appear would look
//   like a bug of its own, so the requesting frame waits up to kOpenBound for
//   the answer and returns Finished - the user sees the old synchronous
//   behaviour, because that behaviour was never the problem. That wait is
//   deliberate blocking work, so it is bracketed in the watchdog pause rule 2b
//   asks for: it cannot reach the 5 s threshold, and the pause also keeps a
//   1.5 s frame out of HangWatchdog::worstGapMs, which is the measurement the
//   threshold is justified against.
//
//   PAST THE BOUND THE FRAME GOES ON WITHOUT IT. The wait expires, the pause is
//   released, the loop keeps rendering and beating, and the panel says "audio
//   device busy" until the worker returns. A device that never returns costs a
//   line of text rather than the application.
//
// WHAT THE CALLER MUST NOT DO WHILE ONE IS IN FLIGHT is touch the sink. The
// worker owns AudioOut for the duration - it is inside open(), which closes the
// old stream, drains the ring and installs a new one - so any other thread
// asking it whether its stream is alive, or asking PortAudio to enumerate, is a
// race. inFlight() is how the GUI knows; AudioOut's own api lock is the
// backstop for a site that forgets.
//
// LIFETIME, AND WHY THE OPENER MUST OWN WHAT IT TOUCHES. reap() abandons a
// worker that is still blocked at quit rather than letting ~future join it (the
// exact hang AppWindow::reapPendingDeviceOpen documents), so the abandoned
// worker can still be inside Pa_OpenStream after the Pipeline that owns the
// sink is gone. Pipeline therefore holds its AudioOut through a shared_ptr and
// Pipeline::audioOpener() hands the worker a copy: the sink outlives the
// abandonment, and the last reference - whichever thread holds it - closes the
// stream. An opener that captures a raw `this` instead would be a
// use-after-free at quit.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_AUDIO_OPEN_HPP
#define CASCADE_GUI_AUDIO_OPEN_HPP

#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <utility>

namespace cascade::gui {

class AudioOpen {
public:
    // HOW LONG THE REQUESTING FRAME WAITS, and why 1500 ms. A working device
    // opens in tens of milliseconds, so this is never spent in a healthy
    // session; it exists so that a device which is merely slow (an endpoint
    // waking, a host API re-enumerating behind a USB change) still completes
    // inside the click that asked for it, rather than flashing "busy" at the
    // user for no reason. It is the same 1500 ms the driver-lock and
    // vendor-call guards use, and it is comfortably under
    // HangWatchdog::kDefaultThresholdMs (5000) so the bounded wait can never
    // itself become the report this class exists to stop.
    static constexpr std::chrono::milliseconds kOpenBound{1500};

    // WHAT QUIT WILL SPEND on an open that is still blocked, before the worker
    // is abandoned. Same 250 ms, and the same argument, as the device-open and
    // scan reapers in app_window.cpp: an open a few milliseconds from finishing
    // is collected here, and anything still inside the driver is let go rather
    // than joined. Spent in ~AppWindow, after HangWatchdog::stop(), so it is
    // outside the shutdown budget.
    static constexpr std::chrono::milliseconds kQuitGrace{250};

    // The ready-poll of the once-a-frame collect: zero by construction, named
    // because a bare literal duration in the source is how a real bounded wait
    // slips past tests/test_shutdown_budget.cpp's scan unnoticed. Same
    // constant, same reason, as app_window.cpp's kNoWait.
    static constexpr std::chrono::milliseconds kNoWait{0};

    // The blocking work, injected: takes the PortAudio device index (-1 = the
    // system default) and returns whether the device opened. It runs on the
    // WORKER thread and must own everything it touches (see the lifetime note
    // in the file header).
    using Opener = std::function<bool(int)>;

    // Default-constructed and then bound, because the owner (AppWindow) can
    // only build the opener once its Pipeline exists and the hooks once its
    // watchdog does. An unbound gate refuses every open rather than crashing.
    AudioOpen() = default;

    // `pause` / `resume` are HangWatchdog's, and either may be empty (a test,
    // or a run with no watchdog).
    void bind(Opener opener, std::function<void()> pause, std::function<void()> resume) {
        opener_ = std::move(opener);
        pause_ = std::move(pause);
        resume_ = std::move(resume);
    }

    ~AudioOpen() { reap(); }

    AudioOpen(const AudioOpen&) = delete;
    AudioOpen& operator=(const AudioOpen&) = delete;

    enum class Outcome {
        Finished,  // the device answered inside the bound; result() is current
        Busy,      // still opening - the caller must carry on rendering
        Queued,    // an open was already in flight; this one runs after it
    };

    struct Result {
        int deviceIndex = -1;  // what was asked for, verbatim (-1 = default)
        bool ok = false;       // what the opener answered
        // The caller's own label for THIS request, carried through the queue
        // and handed back with the answer. AppWindow uses it for "was this the
        // audio watchdog reopening, or the user picking a device", which it
        // cannot keep in a member of its own: a user's click queued behind a
        // watchdog reopen would overwrite the label of the open already in
        // flight and the two results would come back wearing each other's.
        int tag = 0;
    };

    // Ask for `deviceIndex`. Starts the worker, then waits up to kOpenBound for
    // it with the watchdog paused.
    //
    // A request made while another open is in flight is QUEUED rather than
    // started: two waveOutOpen calls racing on one AudioOut is the corruption
    // this whole class is avoiding, and the user clicking three devices while
    // the first is stuck must end on the third, not on all three. Only the
    // latest queued request survives - the earlier ones were superseded by the
    // user's own next click.
    Outcome request(int deviceIndex, int tag = 0) {
        if (inFlight()) {
            queued_ = deviceIndex;
            queuedTag_ = tag;
            hasQueued_ = true;
            return Outcome::Queued;
        }
        start(deviceIndex, tag);
        return waitBounded();
    }

    // Once per frame. Collects a worker that has finished and starts whatever
    // was queued behind it. Returns true when a result was collected - the
    // caller republishes the channel layout and refreshes its device list on
    // exactly those frames.
    bool poll() {
        if (!future_.valid()) { return false; }
        if (future_.wait_for(kNoWait) != std::future_status::ready) {
            return false;
        }
        collect();
        if (hasQueued_) {
            hasQueued_ = false;
            start(queued_, queuedTag_);
            // NOT waitBounded(): this is a polling frame, not the frame the
            // user clicked in, and a frame that renders is worth more here
            // than shaving one frame off a switch nobody is waiting on.
        }
        return true;
    }

    // True while a worker owns the sink. Every other query of the sink, and
    // every PortAudio call, must be skipped while this is true.
    bool inFlight() const { return future_.valid(); }

    // The last completed open. Meaningless until completed() is non-zero.
    const Result& result() const { return result_; }
    unsigned completed() const { return completed_; }

    // The device a queued request is waiting to open, or -2 for none. (-1 is a
    // real request: the system default.)
    int queuedDevice() const { return hasQueued_ ? queued_ : -2; }

    // QUIT. A bounded grace, then the worker is abandoned rather than joined -
    // ~future would block the destructor for the rest of a driver call that may
    // not be coming back, which is the same hang wearing a different hat. Safe
    // to abandon only because the opener owns what it touches; see the file
    // header. Idempotent.
    void reap() {
        if (!future_.valid()) { return; }
        if (future_.wait_for(kQuitGrace) == std::future_status::ready) {
            collect();
            return;
        }
        std::thread([f = std::move(future_)]() mutable { (void)f.get(); }).detach();
        future_ = std::future<bool>();
        hasQueued_ = false;
    }

private:
    void start(int deviceIndex, int tag) {
        pendingDevice_ = deviceIndex;
        pendingTag_ = tag;
        Opener op = opener_;
        future_ = std::async(std::launch::async, [op, deviceIndex]() -> bool {
            return op ? op(deviceIndex) : false;
        });
    }

    // THE BRACKET IS THE POINT, and its order is what a test can hold: pausing
    // AFTER the wait has already blocked would fix nothing. The resume is
    // unconditional - HangWatchdog counts its pauses, so one that is not
    // released disarms the watchdog for the rest of the session.
    Outcome waitBounded() {
        struct Bracket {
            const std::function<void()>& resume;
            ~Bracket() {
                if (resume) { resume(); }
            }
        };
        std::future_status status;
        {
            if (pause_) { pause_(); }
            Bracket bracket{resume_};
            status = future_.wait_for(kOpenBound);
        }
        if (status != std::future_status::ready) { return Outcome::Busy; }
        collect();
        return Outcome::Finished;
    }

    void collect() {
        result_.deviceIndex = pendingDevice_;
        result_.tag = pendingTag_;
        result_.ok = future_.get();
        future_ = std::future<bool>();
        ++completed_;
    }

    Opener opener_;
    std::function<void()> pause_;
    std::function<void()> resume_;
    std::future<bool> future_;
    int pendingDevice_ = -1;
    int pendingTag_ = 0;
    int queued_ = -1;
    int queuedTag_ = 0;
    bool hasQueued_ = false;
    Result result_;
    unsigned completed_ = 0;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_AUDIO_OPEN_HPP
