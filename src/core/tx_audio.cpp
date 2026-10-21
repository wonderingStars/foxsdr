// The pieces behind CASCADE_CAP_TX_AUDIO's keying rule. See tx_audio.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tx_audio.hpp"

#include <cmath>
#include <vector>

namespace cascade::core {

// --- the keying decision -------------------------------------------------------

bool pluginMayKey(const PluginKeyInputs& in) {
    return in.granted && in.armed && in.pageLive && in.tickFresh && in.active && in.radioOk &&
           in.handsOff && !in.faulted;
}

// --- what a plugin's audio is made safe with -----------------------------------

void sanitiseTxAudio(float* audio, std::size_t frames) {
    if (audio == nullptr) { return; }
    for (std::size_t i = 0; i < frames; ++i) {
        const float s = audio[i];
        // A NaN or an infinity is not audio and becomes silence (the first test: every
        // comparison against a NaN is false, so an "if (s > 1) ... else if (s < -1) ..." alone
        // would let one straight through); anything finite is held to the range the
        // modulators are scaled for.
        if (!std::isfinite(s)) {
            audio[i] = 0.0f;
        } else if (s > 1.0f) {
            audio[i] = 1.0f;
        } else if (s < -1.0f) {
            audio[i] = -1.0f;
        }
    }
}

// --- the arming window ---------------------------------------------------------

bool TxArming::validMinutes(int minutes) {
    for (const int m : kChoicesMinutes) {
        if (m == minutes) { return true; }
    }
    return false;
}

TxArming::Refusal TxArming::arm(int minutes, bool radioOpen, bool anyGrant, Clock::time_point now) {
    // The order is the order an operator can fix them in: a radio first, then a grant.
    if (!radioOpen) { return Refusal::NoRadio; }
    if (!anyGrant) { return Refusal::NoGrant; }
    if (!validMinutes(minutes)) { return Refusal::BadMinutes; }
    armed_ = true;
    minutes_ = minutes;
    until_ = now + std::chrono::minutes(minutes);
    lastReason_.clear();
    return Refusal::None;
}

bool TxArming::disarm(const char* reason) {
    const bool was = armed_;
    armed_ = false;
    minutes_ = 0;
    until_ = Clock::time_point{};
    if (was) { lastReason_ = reason != nullptr ? reason : "no reason given"; }
    return was;
}

bool TxArming::update(Clock::time_point now, bool* expired) {
    if (expired != nullptr) { *expired = false; }
    if (!armed_) { return false; }
    if (now >= until_) {
        disarm("the arming window ran out");
        if (expired != nullptr) { *expired = true; }
        return false;
    }
    return true;
}

int TxArming::secondsLeft(Clock::time_point now) const {
    if (!armed_ || now >= until_) { return 0; }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until_ - now).count();
    return static_cast<int>((left + 999) / 1000);
}

// --- the link ------------------------------------------------------------------

std::int64_t TxAudioLink::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void TxAudioLink::clear() {
    // Read it all out. The consumer side is the only side that may take from the ring,
    // so this is only called with the transmit thread stopped.
    std::vector<float> sink(4096);
    while (ring_.read(sink.data(), sink.size()) != 0) {}
}

void TxAudioLink::publishActive(bool active, std::int64_t nowMs) {
    const bool was = holderActive_.load(std::memory_order_relaxed);
    if (was && !active) {
        // THE EDGE. Whatever is still queued was handed over by the plugin before it
        // said it was done, and is played out - for at most kDrainMaxMs.
        drainUntilMs_.store(ring_.size() != 0 ? nowMs + kDrainMaxMs : 0,
                            std::memory_order_relaxed);
    } else if (active) {
        drainUntilMs_.store(0, std::memory_order_relaxed);
    }
    holderActive_.store(active, std::memory_order_relaxed);
    activeAtMs_.store(nowMs, std::memory_order_release);
}

void TxAudioLink::publishNone(std::int64_t nowMs) {
    holderActive_.store(false, std::memory_order_relaxed);
    drainUntilMs_.store(0, std::memory_order_relaxed);
    activeAtMs_.store(nowMs, std::memory_order_release);
}

bool TxAudioLink::wantsKey(std::int64_t nowMs) const {
    const std::int64_t at = activeAtMs_.load(std::memory_order_acquire);
    // NEVER READ: a link nobody has published to wants nothing.
    if (at == 0) { return false; }
    // STALE: the thread that asks the plugin has not asked lately - the receiver was
    // stopped, or the DSP thread is wedged. An old "yes" is not a request.
    if (nowMs - at > kActiveFreshMs) { return false; }
    if (holderActive_.load(std::memory_order_relaxed)) { return true; }
    // active() is zero. The key stays down only to play out audio already handed over.
    const std::int64_t until = drainUntilMs_.load(std::memory_order_relaxed);
    return until != 0 && nowMs < until && ring_.size() != 0;
}

}  // namespace cascade::core
