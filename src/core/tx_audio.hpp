// tx_audio.hpp - the host side of CASCADE_CAP_TX_AUDIO: the pieces that decide
// whether a PLUGIN may key the transmitter, with no ImGui, no thread and no radio
// in them so that every rule can be tested by itself.
//
// THE RULE THIS FILE CHANGES. Until 0.99.74 the sentence in core/transmitter.hpp
// and docs/TRANSMITTING.md was "only a hand can key it ... not a plugin". The
// owner approved a Winlink-compatible radio link (2026-10-12), which needs a
// plugin to send audio while the transmitter is keyed, and the sentence is now
// "a hand, or a plugin the operator has granted and armed". Everything in this
// file exists to make the second half as hard to satisfy by accident as the
// first half always was:
//
//   - A GRANT, per plugin, off by default, kept in config.json by plugin name
//     like the tune and settings grants - and worth nothing on its own;
//   - AN ARMING WINDOW the operator opens on the TRANSMIT page (5, 15 or 60
//     minutes), that closes itself, that is never saved and never restored, and
//     that closes at once on a fault, on the page closing, on the radio being
//     closed and on the grant being taken back;
//   - THE PAGE LIVE AND THE FRAME LOOP TICKING, the two things the hand-keyed
//     paths already need;
//   - THE PLUGIN'S OWN active(), polled recently - a stale "yes" from a
//     receiver that has since stopped is not a request;
//   - A HOST THAT NEVER LETS THE PLUGIN CHOOSE frequency, power or mode.
//
// Any one of those false and nothing is keyed. pluginMayKey() below is that
// sentence as a function, and tests/test_tx_audio.cpp flips each input in turn.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_TX_AUDIO_HPP
#define CASCADE_CORE_TX_AUDIO_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "dsp/spsc_ring.hpp"

namespace cascade::core {

// The one rate CascadeTxAudioApi is accepted at: the modulator's own (48 kHz), so
// the host never resamples a plugin's transmit audio.
inline constexpr std::uint32_t kTxAudioRateHz = 48000u;

// --- the keying decision -------------------------------------------------------

// Every condition that has to hold for a plugin to key the transmitter, each one
// stated so that FALSE means "do not". `faulted` is the one exception, a bad
// thing rather than a good one, and it is spelled that way because "has a fault
// been seen" is the question the code can answer.
struct PluginKeyInputs {
    bool granted = false;    // the plugin that would hold the transmitter has the TRANSMIT grant
    bool armed = false;      // the operator's arming window is open
    bool pageLive = false;   // the TRANSMIT page was drawn with its controls on screen this frame
    bool tickFresh = false;  // the frame loop has ticked the transmitter inside Transmitter::kKeyAliveWait
    bool active = false;     // the plugin wants the key: a FRESH active() reading (see TxAudioLink::wantsKey)
    bool radioOk = false;    // a transmit radio is open and has not faulted
    bool handsOff = false;   // no PTT, LATCH or web-remote key is in use - a hand has precedence
    bool faulted = false;    // a fault (the cap, the radio, the frozen window) since the window was armed
};

// The decision. True only when every positive input is true and `faulted` is false.
// Defined in tx_audio.cpp and NOT inline, so there is exactly one copy of the rule in
// the binary and the tests exercise the one the application runs.
bool pluginMayKey(const PluginKeyInputs& in);

// --- what a plugin's audio is made safe with -----------------------------------

// Replaces every NaN and infinity with silence and clamps the rest to [-1, 1], in place. The
// modulators guard their own input as well, and this is the host's guarantee at the trust
// boundary all the same (plugin_abi.h promises it to plugin authors): a plugin is third-party
// code, and a promise that rested on a modulator's incidental guard is one a new mode could
// break without anyone noticing. Applied to every block a plugin supplies, nothing else.
void sanitiseTxAudio(float* audio, std::size_t frames);

// --- the arming window ---------------------------------------------------------

// THE OPERATOR'S WINDOW. A time and nothing else: session-only (there is no
// field for it in config.json and no code that could restore one), closed by
// its own timer, by disarm(), and refused when it could not mean anything. The
// application passes the clock in, so a test can run a 60 minute window in no time
// at all.
class TxArming {
public:
    using Clock = std::chrono::steady_clock;

    // The three lengths the page offers. Anything else is refused: a window is a
    // decision the operator makes from a short list, not a number a hand-edited
    // file or a stray call can make up.
    static constexpr int kChoicesMinutes[3] = {5, 15, 60};

    enum class Refusal {
        None = 0,
        NoRadio,       // no transmit radio is open, so there is nothing to arm
        NoGrant,       // no plugin holds the TRANSMIT grant, so there is nothing to let in
        BadMinutes,    // not one of kChoicesMinutes
    };

    // Opens (or restarts) the window for `minutes`. Refused, and left as it was, with the reason.
    Refusal arm(int minutes, bool radioOpen, bool anyGrant, Clock::time_point now);

    // Closes the window, saying why (kept for the page and the log). True when it was open.
    bool disarm(const char* reason);

    // True while the window is open as of `now`. A window whose time has come is
    // closed HERE, as a side effect, so there is no moment at which it is both
    // over and still honoured - and `expired` says it was this call that did it.
    bool update(Clock::time_point now, bool* expired = nullptr);

    bool armed() const { return armed_; }
    int minutes() const { return minutes_; }
    // Whole seconds left, rounded UP so the last second reads 1 and not 0; 0 when closed.
    int secondsLeft(Clock::time_point now) const;
    // Why the window last closed ("" if it never has, or while it is open).
    const std::string& lastReason() const { return lastReason_; }

    static bool validMinutes(int minutes);

private:
    bool armed_ = false;
    int minutes_ = 0;
    Clock::time_point until_{};
    std::string lastReason_;
};

// --- the ring between the real-time audio thread and the transmit thread --------

// WHAT THE PLUGIN RUNNER WRITES AND THE TRANSMITTER READS. One writer (the DSP
// thread, inside PluginRunner::pollTxAudio) and one reader (the TX thread), neither
// ever blocking the other, plus the handful of facts the GUI thread needs to decide
// whether the plugin may keep the key. Shared by both through a shared_ptr, so the
// transmitter and the runner have no pointer to one another.
class TxAudioLink {
public:
    // A power of two (SpscRing's rule): 65536 frames is 1.37 s of 48 kHz audio. The
    // runner keeps it no deeper than kLeadFrames, so this is headroom and not a delay.
    static constexpr std::size_t kRingFrames = 65536;
    // How far ahead of the air the runner keeps the ring: 100 ms. It is also the
    // longest a plugin's last sample can still be waiting when active() reads zero.
    static constexpr std::size_t kLeadFrames = 4800;
    // The most the runner asks pull() for in one call.
    static constexpr std::size_t kMaxPullFrames = kLeadFrames;
    // A reading of active() older than this is NOT a request (milliseconds, a plain
    // integer on purpose: a std::chrono constant would have to be classified in
    // tests/test_shutdown_budget.cpp's table of waits, and this one is not a wait).
    // The DSP thread polls once per audio block, which is milliseconds, so half a second
    // is a receiver that has stopped, not a busy one.
    static constexpr std::int64_t kActiveFreshMs = 500;
    // The longest queued audio is allowed to hold the key after active() has gone to
    // zero. The lead is 100 ms, so this is that with a wide margin, and it is the bound
    // that makes "play out what was handed over" safe if the transmit thread stalls.
    static constexpr std::int64_t kDrainMaxMs = 250;

    TxAudioLink() : ring_(kRingFrames) {}
    TxAudioLink(const TxAudioLink&) = delete;
    TxAudioLink& operator=(const TxAudioLink&) = delete;

    // ---- the audio -----------------------------------------------------------
    // Producer (the DSP thread). Accepts what fits and never blocks.
    std::size_t write(const float* mono, std::size_t frames) { return ring_.write(mono, frames); }
    // Consumer (the TX thread, or whoever has stopped it).
    std::size_t read(float* mono, std::size_t frames) { return ring_.read(mono, frames); }
    // Frames queued now.
    std::size_t backlog() const { return ring_.size(); }
    // Throws away what is queued. Consumer side: call it only while the transmit thread
    // is not running (key-down, after the join).
    void clear();

    // ---- what the GUI thread is told ----------------------------------------
    // Producer side, once per audio block: the plugin holding the transmitter said
    // `active` at `nowMs` (steady clock, milliseconds). The edge from yes to no starts
    // the drain window if audio is still queued.
    void publishActive(bool active, std::int64_t nowMs);
    // The holder is gone or has stopped for good: nobody wants the key and nothing
    // queued is worth sending.
    void publishNone(std::int64_t nowMs);
    // GUI thread: does the plugin want the key as of `nowMs`? The reading must be fresh
    // and either say yes or be the tail of audio already handed over (the drain).
    bool wantsKey(std::int64_t nowMs) const;
    // GUI thread: the last published reading was a yes (fresh or not), for the plate.
    bool lastActive() const { return holderActive_.load(std::memory_order_relaxed); }

    // ---- what the DSP thread is told ----------------------------------------
    // The host has keyed the transmitter FOR THE PLUGIN: pull() may be called. Set by
    // the GUI thread through PluginRunner::setTxKeyed, which also waits for a pull in
    // flight, so that once it has returned false no more audio is on its way.
    void setKeyed(bool keyed) { keyed_.store(keyed, std::memory_order_release); }
    bool keyed() const { return keyed_.load(std::memory_order_acquire); }

    // The steady-clock milliseconds the rest of the host uses for these figures.
    static std::int64_t nowMs();

private:
    dsp::SpscRing<float> ring_;
    std::atomic<bool> keyed_{false};
    std::atomic<bool> holderActive_{false};
    std::atomic<std::int64_t> activeAtMs_{0};
    std::atomic<std::int64_t> drainUntilMs_{0};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_TX_AUDIO_HPP
