// transmitter.hpp - the thread between a microphone and a radio, and the
// rules about when it is allowed to run.
//
// ============================================================================
// THE HARD RULE
// ============================================================================
//
// TRANSMISSION HAPPENS ONLY WHILE AN OPERATOR IS ASKING FOR IT. Not at
// startup, not because a config file said the transmitter was on last time,
// not because a window was left open, and not because a plugin asked.
//
// WHO MAY ASK, AND FOR HOW LONG, IS DECIDED BY THE ENGINE API'S RULES, not
// here (engine extraction stage 4, the owner's decision of 2026-09-29: API
// 0.2's "THE KEY", implemented in core/tx_key_rules.hpp and applied by the
// engine's API host, engine/fox_engine_api.cpp, once a control pass). Every
// interface - the standard window included - keys through its own SESSION:
//
//   - TX_PTT 1 IS A HOLD, NOT A SWITCH: it keys, or extends the key, for 2 s
//     (FOXAPI_PTT_HOLD_MS) and no more; the interface re-asserts it while a
//     hand is on the key, and silence opens it. TX_PTT 0 opens it now.
//   - THE LATCH belongs to a PRINCIPAL (the operator at this machine, for
//     every LOCAL session), is refused to any remote session, opens itself 60
//     s after it closed, is never re-armed within 1 s of ANY latch ending,
//     and after any end but its owner's own release it MARKS the operator: no
//     session of theirs latches again until THAT session has sent LATCH 0.
//     An interface's latch key is edge-triggered (a press on a released key
//     sends LATCH 0 then LATCH 1 in one batch; on a latched key, LATCH 0).
//   - A REMOTE PTT (the app's web remote, and any remote session) needs the
//     local operator's standing consent - TX_REMOTE_ARM 1 from a LOCAL
//     session, the Transmit page's "Allow remote transmit" switch - which
//     lapses when that session withdraws it, closes, or stops answering.
//   - EVERY SESSION HAS A KEEP-ALIVE (1 s local, 2 s remote) and only its
//     heartbeat() counts; a session that stops answering, closes or is
//     detached loses any key it holds. A stop, a fault, and a transmitter
//     change or close each end any key.
//
// This class does what those rules decide - setPttHeld, setLatched and
// keyRemote/releaseRemote are the rules' levels, driven by the API host -
// and keeps, on its own thread, the protections that must hold even when
// the rules' own thread has stopped:
//
//   - THE CONFIG CANNOT KEY IT. The mode, the power, the input and the split
//     are saved; the key is not, and there is no code path that could
//     restore it. core/config.hpp says the same thing from its side.
//
//   - TWO INDEPENDENT DEAD-MAN'S HANDLES, both watched by the TX thread itself
//     (kKeyAliveWait): tick(), stamped once a control pass by the thread that
//     applies the rules - ANY key opens if it goes stale, because the thing
//     that would release the key is the thing that has stopped - and
//     frontEndAlive(), stamped by the KEY HOLDER'S heartbeat (the API host
//     calls it for that session's heartbeat, and when it grants a key) - a
//     LOCAL key (PTT, LATCH) opens if it goes stale, however the control side
//     is doing. So a frozen window, and a wedged control thread, each release
//     the key within kKeyAliveWait on a thread neither of them can stop.
//
//   - A LATCH CANNOT BE FORGOTTEN. The rules end it after 60 s; this class
//     keeps its own kLatchTimeout as a second, independent deadline.
//
//   - A FAULT UNKEYS IT. A sink that faults mid-transmission stops the key
//     rather than being retried; the API host sees the key go and tells the
//     rules, which mark a latch's operator as for any other end.
//
//   - THE REMOTE KEY IS A DEAD-MAN'S HANDLE OF ITS OWN. keyRemote() buys
//     kRemotePttHoldMs and no more, enforced here as well as by the rules, so
//     a closed tab, a phone put in a pocket or a Wi-Fi link that drops all
//     open the key inside two seconds. THE REMOTE CANNOT LATCH.
//
// ============================================================================
// THE RATE PROBLEM, AND WHY IT IS SOLVED IN TWO STAGES
// ============================================================================
//
// The modulator makes complex baseband at the AUDIO rate - 48 kHz - and an
// AD9361 cannot be clocked below about 2.08 MS/s. So something has to
// interpolate by roughly fifty, and the obvious tool is the wrong one:
// dsp::RationalResampler reduces L/M and builds L polyphase branches, so a
// board sitting at 2,083,333 S/s (a prime-ish number against 48000) would ask
// it for two million branches and hundreds of megabytes of taps.
//
// TxInterpolator below does it in two stages instead, and the split is chosen
// so the cheap stage only ever has an easy job:
//
//   1. an INTEGER interpolation by L = round(sinkRate / audioRate), through
//      RationalResampler(L, 1) - a real windowed-sinc anti-imaging filter,
//      L branches of 16 taps, which for L around 50 is a few thousand
//      coefficients. This is the stage that removes the images at multiples
//      of 48 kHz, and it is the stage that matters: they are what would be
//      transmitted either side of the carrier if it were skipped.
//
//   2. a LINEAR resample of the remainder, from audioRate*L to the board's
//      actual rate. That ratio is within about half a percent of 1 by
//      construction, and the signal arriving at this stage is oversampled L
//      times - a 3 kHz component against a 2.5 MHz rate - so linear
//      interpolation's error is of order (pi*f/fs)^2/2, which is about two
//      parts in a million. A linear interpolator straight from 48 kHz would
//      have been indefensible; after stage 1 it is not approximately right,
//      it is right to further decimal places than the DAC has bits.
//
// ============================================================================
//
// NOTHING HERE HAS BEEN RUN INTO AN ANTENNA. There is no Pluto on this bench
// and nothing on it may transmit. What is proven is the arithmetic and the
// rules: tests/test_transmitter.cpp drives the whole path into a sink that
// records what it was given, with no device anywhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TRANSMITTER_HPP
#define CASCADE_CORE_TRANSMITTER_HPP

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dsp/modulator.hpp"
#include "dsp/resampler.hpp"
#include "sink/audio_in.hpp"
#include "source/tx_sink.hpp"

namespace cascade::core {

// Where the audio comes from. MICROPHONE is what a transmitter is for; TONE
// is what answers "is anything coming out at all" without needing a
// microphone, a quiet room, or a second receiver to listen on.
enum class TxInput : int { Microphone = 0, Tone, Count };

inline constexpr int kTxInputCount = static_cast<int>(TxInput::Count);

const char* txInputName(TxInput in);
TxInput txInputFromIndex(int index);
bool txInputFromName(const char* name, TxInput& out);

// --- the rate matcher --------------------------------------------------------
//
// See the file header for why it is two stages. Separated out and ImGui-free
// so tests can put a tone through it and measure what comes out the far end
// with no thread and no radio involved.
class TxInterpolator {
public:
    // audioRateHz is what the modulator produces; sinkRateHz is what the
    // radio is clocked at. Refuses (and reports 1:1) for a sink rate below
    // the audio rate, which is not a transmit path this product has.
    void configure(double audioRateHz, double sinkRateHz);

    double audioRateHz() const { return audioRateHz_; }
    double sinkRateHz() const { return sinkRateHz_; }
    // The integer factor stage 1 uses. 1 when nothing is needed.
    unsigned interpolation() const { return interp_; }

    // Tight upper bound on what process() will produce from nIn inputs, so a
    // caller can size its output buffer once instead of guessing. One spare
    // sample beyond the arithmetic, because the fractional phase can be
    // anywhere in a sample when the block starts.
    std::size_t maxOut(std::size_t nIn) const;

    void reset();

    // nIn complex samples at audioRateHz -> up to outCap complex samples at
    // sinkRateHz. Returns how many were produced. Stream state (both filter
    // histories and the fractional phase) carries across calls, so any block
    // split of a stream produces the same output as one big call.
    std::size_t process(const std::complex<float>* in, std::size_t nIn,
                        std::complex<float>* out, std::size_t outCap);

private:
    double audioRateHz_ = 48000.0;
    double sinkRateHz_ = 48000.0;
    unsigned interp_ = 1;
    // Output samples per input sample of stage 2 - sinkRate / (audioRate*L),
    // which is within about half a percent of 1 by construction.
    double step_ = 1.0;
    double phase_ = 0.0;  // fractional position inside stage 1's output
    std::complex<float> last_{0.0f, 0.0f};
    bool primed_ = false;

    std::unique_ptr<dsp::RationalResampler> upI_;
    std::unique_ptr<dsp::RationalResampler> upQ_;
    std::vector<float> inI_, inQ_, midI_, midQ_;
};

// --- the transmitter ---------------------------------------------------------

class Transmitter {
public:
    // --- the bounded waits ---------------------------------------------------
    //
    // Named because tests/test_shutdown_budget.cpp discovers every
    // `constexpr std::chrono` constant under src/ and refuses to go green
    // until each one is classified in its kKnownWaits table.

    // The bound on joining the TX thread, and the first half of the transmit
    // column of the shutdown budget (the second half is
    // PlutoTx::kWriterJoinWait). The TX thread's longest legitimate stall is
    // one kAudioPollWait plus one sink write, both of which are milliseconds;
    // half a second is that with a wide margin, and a thread that has not
    // come back by then is abandoned rather than waited for.
    static constexpr std::chrono::milliseconds kThreadJoinWait{500};

    // How long the TX thread waits for a block of audio to exist before it
    // gives up and modulates silence for that block. Spent on the TX thread
    // only. Short, because this thread is also the one that has to notice the
    // key has been released.
    static constexpr std::chrono::milliseconds kAudioPollWait{5};

    // THE DEAD-MAN'S HANDLE. If the control side has not called tick()
    // within this long - or, with a local key held, the front end has not
    // called frontEndAlive() - the TX thread unkeys itself - see the hard
    // rule at the top of this file. Not a wait: nothing sleeps or blocks on it, it is a
    // staleness bound on a timestamp.
    static constexpr std::chrono::milliseconds kKeyAliveWait{1000};

    // How long a LATCHED key stays closed before it opens itself. Not a wait
    // either: it is a deadline the tick compares against.
    static constexpr std::chrono::milliseconds kLatchTimeout{60000};

    // HOW LONG ONE ASSERTION FROM THE WEB REMOTE IS WORTH. Not a wait either:
    // it is a deadline stamped by keyRemote() and compared against once a
    // frame, exactly like the latch's - and it is the reason a remote PTT can
    // exist at all (see the hard rule at the top of this file).
    //
    // TWO SECONDS, AGAINST A BROWSER THAT RE-ASSERTS EVERY 500 ms. The page
    // repeats while the key is held, so the hold is four missed repeats deep:
    // long enough that one dropped packet, one garbage-collection pause or
    // one slow frame on a phone does not chop the transmission into
    // fragments, and short enough that a link which has genuinely gone costs
    // two seconds of carrier rather than a minute. Shorter would make the
    // product unusable on the exact networks it is for; longer would make the
    // failure a report rather than an embarrassment.
    static constexpr std::chrono::milliseconds kRemotePttHoldMs{2000};

    Transmitter();
    ~Transmitter();

    Transmitter(const Transmitter&) = delete;
    Transmitter& operator=(const Transmitter&) = delete;

    // --- the radio -----------------------------------------------------------

    // Takes ownership. Any sink already installed is stopped (which silences
    // it) and destroyed first. Passing nullptr is how a radio is removed.
    void setSink(std::unique_ptr<source::IqSink> sink);
    bool haveSink() const;
    // Borrowed, and only valid while no other thread is changing the sink -
    // i.e. from the GUI thread, between frames. For the panel's readouts.
    source::IqSink* sink();
    const source::IqSink* sink() const;

    // --- the settings --------------------------------------------------------
    // All of these are safe to change while transmitting; the ones the radio
    // has to be told about are written through to it.

    void setMode(dsp::TxMode m);
    dsp::TxMode mode() const;

    void setInput(TxInput in);
    TxInput input() const;

    // The transmit attenuation, in the board's own negative decibels. See
    // source/tx_sink.hpp for why the direction is stated so loudly.
    void setPowerDb(double db);
    double powerDb() const;

    bool setFrequencyHz(double hz);
    double frequencyHz() const;

    void setToneHz(double hz);
    double toneHz() const;

    // The microphone. Owned here because the transmitter is the only thing
    // that reads it, and because opening a microphone is itself something
    // that should not happen until somebody asks for a transmitter.
    //
    // HELD BY shared_ptr for the reason Pipeline holds its AudioOut that way:
    // opening a device is a blocking driver call (Pa_OpenStream on WMME is
    // waveInOpen, with no timeout), so the GUI runs it on a gui::AudioOpen
    // worker and abandons that worker at quit rather than joining it - and an
    // abandoned worker can still be inside the driver after this Transmitter is
    // gone. Do not call audioIn().open() from the frame loop; use
    // microphoneOpener().
    sink::AudioIn& audioIn() { return *audioIn_; }
    const sink::AudioIn& audioIn() const { return *audioIn_; }

    // The open, packaged to run on a worker that may outlive this object: the
    // callable owns a reference to the microphone and touches nothing else of
    // the transmitter. Takes the PortAudio input index (-1 = the system
    // default) and opens it at the modulator's 48 kHz.
    std::function<bool(int)> microphoneOpener();

    // TEST-ONLY: how many owners the microphone has - 1 for this object, plus
    // one per live opener. It is how tests/test_transmitter.cpp proves the
    // opener holds the microphone rather than a pointer back into here.
    long microphoneOwners() const { return audioIn_.use_count(); }

    // --- the key -------------------------------------------------------------
    //
    // DRIVEN BY THE ENGINE'S KEY RULES (core/tx_key_rules.hpp), once a control
    // pass, by the engine API's host - never by an interface directly.

    // A LOCAL session's PTT, held: true while the rules say a local session
    // holds the key on a PTT whose hold has not run out; false is the resting
    // state. The TX thread opens it if the holder's frontEndAlive() stamp goes
    // stale.
    void setPttHeld(bool held);
    bool pttHeld() const;

    // The LATCH, as the rules hold it. Also releases itself after
    // kLatchTimeout (a second deadline beside the rules' own), and is cleared
    // by every stop, every fault and every sink change - a latch that survived
    // a radio being swapped would key the new one.
    void setLatched(bool on);
    bool latched() const;

    // --- A REMOTE SESSION'S KEY (the web remote's since 0.95.1) --------------
    //
    // Keyed only when the rules grant a REMOTE session's PTT - which needs the
    // local operator's consent (TX_REMOTE_ARM) - and released the moment
    // they end it.
    //
    // Deliberately NOT a setter taking a bool, which is what every other
    // control here is. A bool setter is a switch, and a switch left on by a
    // client that then disappears is a keyed radio nobody is holding; these
    // two are a key that has to be re-pressed instead.

    // Asserts the remote key, or extends an assertion already made, for
    // kRemotePttHoldMs from now. The caller is expected to have checked that
    // a transmitter is open - but this is not the place that decides, and a
    // key asserted with no radio behind it is refused by tick() like any
    // other and cleared rather than left pending.
    void keyRemote();

    // Opens the remote key at once, with a sentence for the log naming what
    // did it. A no-op - and silent - when the remote key is not held, so
    // every caller that releases "just in case" costs nothing.
    void releaseRemote(const char* why);

    bool remoteKeyed() const;

    // Milliseconds left on the current hold; 0 when the remote key is not
    // held. Published to the browser so the page can show what it is
    // holding, and read by nothing that decides anything.
    std::int64_t remoteHoldRemainingMs() const;

    // What is actually happening, as against what was asked for. False
    // whenever there is no sink, the sink refused to start, or a fault has
    // been seen.
    bool transmitting() const;

    // ONCE A CONTROL PASS, FROM THE THREAD THAT APPLIES THE KEY RULES (the
    // engine API's host, on the engine's control thread - or the window's
    // frame while it still pumps the engine itself). Applies the key the
    // rules left, enforces the latch timeout, and - the part that matters -
    // stamps the control liveness the TX thread watches. A loop that stops running stops calling
    // this, and the key opens. The remote key's hold is enforced by the TX
    // thread; this only drops an assertion gone stale with nothing
    // transmitting, before it could key for it. A TX thread that played the
    // key down itself (the key let go) is tidied up without wiping a key
    // asked for since; one that broke off (a fault, a dead-man's handle)
    // takes every key with it.
    void tick();

    // THE KEY HOLDER'S HEARTBEAT: one atomic store saying "the session that
    // holds the local key is alive", made by the engine API's host for that
    // session's heartbeat() (and when it grants the key). With a PTT or
    // LATCH asserted, the TX thread opens the key when this has not been
    // called for kKeyAliveWait - however the control side is doing.
    void frontEndAlive();
    // The same stamp, at a given steady-clock millisecond count (the key
    // holder's last heartbeat, when a key is granted): so a grant - a PTT
    // re-assertion included - never counts as a heartbeat of its own.
    void frontEndAliveAt(std::int64_t steadyMs);

    // Unkeys and stops everything, bounded (kThreadJoinWait plus whatever the
    // sink's own stop costs). Idempotent; safe before any sink is installed;
    // called by the destructor.
    void stop();

    // --- what happened -------------------------------------------------------

    // Blocks of modulation actually handed to the radio since the last key
    // down. Zero is how "the PTT is down and nothing is going out" is told
    // apart from "the PTT is not down".
    std::uint64_t blocksSent() const;
    // Blocks the sink would not take in full. A climbing count is the radio
    // not keeping up with the modulator.
    std::uint64_t blocksShort() const;
    // The microphone's peak since the last read, in [0, 1] - straight through
    // from AudioIn::takePeak, and cleared by asking.
    float takeInputPeak();

    // Empty when nothing has gone wrong. Set from the TX thread, read from
    // the GUI thread, so it is snapshotted rather than handed out by pointer.
    std::string lastError() const;
    // The same text copied into a caller's buffer (cut on a character, never
    // in the middle of one; always NUL-terminated; nothing written with no
    // room), under the same lock and without allocating - for the engine's
    // per-pass publish, where lastError()'s std::string copy would be a heap
    // allocation every pass.
    void lastErrorInto(char* out, std::size_t cap) const;

    // Why the key last opened on its own, for the panel to letter: empty when
    // the operator opened it themselves.
    std::string lastAutoUnkeyReason() const;

    // Tests only: shorten the latch's own deadline so the failsafe can be
    // watched working without a test that takes a minute. The same device
    // PlutoSource::setStreamHealthWindowForTest uses, and for the same
    // reason - a safety property nobody can afford to exercise is one nobody
    // exercises.
    void setLatchTimeoutForTest(std::chrono::milliseconds t);

private:
    void startThread();
    // Stops and joins the TX thread, bounded by kThreadJoinWait. playTail
    // lowers the key and lets the thread play the modulator's ramp down and
    // silence the radio itself (every key-up the operator makes); without it
    // the thread is told to stop at once (a thread that already let go).
    // stateMutex_ must NOT be held.
    void stopThread(bool playTail);
    bool keyDownLocked();   // stateMutex_ held
    void keyUpLocked(const char* reason);
    void threadBody();
    void setError(std::string msg);

    // The audio block the whole chain is quantised to. 10 ms at 48 kHz: short
    // enough that releasing the PTT stops the modulation inside a syllable,
    // long enough that the resampler is not called ten thousand times a
    // second.
    static constexpr std::size_t kAudioBlock = 480;

    mutable std::mutex stateMutex_;
    std::unique_ptr<source::IqSink> sink_;

    dsp::Modulator modulator_;
    dsp::ToneGenerator tone_;
    std::shared_ptr<sink::AudioIn> audioIn_ = std::make_shared<sink::AudioIn>();
    TxInterpolator interp_;
    TxInput input_ = TxInput::Tone;

    // THE DEFAULT INPUT IS THE TONE, AND IT IS NOT AN ACCIDENT. A
    // transmitter that comes up pointed at a microphone is one that would put
    // a room on the air the first time somebody pressed the big key to see
    // what it did. The tone is a signal the operator chose; the room is not.

    std::thread thread_;
    std::atomic<bool> run_{false};
    // Set by the TX thread as it ends: true when it ended by playing the key
    // down because nothing asserted it any more (a key let go - key-up needs
    // no tick), false when it broke off (a dead-man's handle, a fault). tick()
    // tidies up the first without wiping a key asked for since.
    std::atomic<bool> endedReleased_{false};
    std::atomic<bool> transmitting_{false};
    std::atomic<bool> pttHeld_{false};
    std::atomic<bool> latched_{false};
    // The web remote's key, and when it was last asserted. Steady-clock
    // milliseconds as a plain count, like controlAliveMs_, so tick() can compare
    // it without taking anything.
    std::atomic<bool> remoteKeyed_{false};
    std::atomic<std::int64_t> remoteKeyedAtMs_{0};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> shortBlocks_{0};

    // Steady-clock milliseconds, stored as a count so the TX thread can read
    // it without a lock. Stamped by tick() (the control side) and by
    // frontEndAlive() (the window); both watched by the TX thread.
    std::atomic<std::int64_t> controlAliveMs_{0};
    std::atomic<std::int64_t> frontEndAliveMs_{0};
    std::chrono::steady_clock::time_point latchedAt_{};
    std::chrono::milliseconds latchTimeout_ = kLatchTimeout;

    std::mutex waitMutex_;
    std::condition_variable waitCv_;
    bool exited_ = false;

    mutable std::mutex errorMutex_;
    std::string lastError_;
    std::string autoUnkeyReason_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TRANSMITTER_HPP
