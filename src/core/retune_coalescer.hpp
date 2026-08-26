// Coalesces bursts of hardware retune requests into at most one device call
// per interval, latest value winning.
//
// WHY. Wheel tuning emits one setFrequency per notch per FRAME — 60 to 144
// bursts of synchronous USB control transfers per second into a live bulk
// stream. The 0.62.0 field crashes' most frequent signature was raised on
// exactly that gesture (a retune consuming corrupted libusb state), and even
// on a healthy driver the burst buys nothing: the synthesiser only needs the
// value the user lands on. One apply per ~50 ms with the last value keeps the
// readout latency invisible and cuts the control-transfer exposure during the
// exact gesture that produced the crash reports.
//
// PURE LOGIC, no clock of its own: callers pass a monotonic now in
// milliseconds (AppWindow uses steady_clock), which is what makes this
// testable to the millisecond. Single-threaded by design — every caller is
// the GUI thread, where all tuning already lives.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <optional>

namespace cascade::core {

class RetuneCoalescer {
public:
    explicit RetuneCoalescer(double minIntervalMs) : minIntervalMs_(minIntervalMs) {}

    // A tune request. True: the caller should apply hz to the hardware NOW
    // (the interval has passed — the common single-tune case stays immediate).
    // False: hz is held as the pending value, replacing any earlier pending
    // one, for a later due() to release.
    bool request(double hz, double nowMs) {
        if (nowMs - lastAppliedMs_ >= minIntervalMs_) {
            lastAppliedMs_ = nowMs;
            hasPending_ = false;  // an older held value is superseded by this apply
            return true;
        }
        pendingHz_ = hz;
        hasPending_ = true;
        return false;
    }

    // Poll from the frame loop: the held value, once the interval since the
    // last apply has passed. Returns it exactly once and stamps the apply.
    std::optional<double> due(double nowMs) {
        if (!hasPending_ || (nowMs - lastAppliedMs_ < minIntervalMs_)) {
            return std::nullopt;
        }
        hasPending_ = false;
        lastAppliedMs_ = nowMs;
        return pendingHz_;
    }

    // An apply that happened outside request()/due() — e.g. the carry-across
    // tune on a fresh device open — so the next burst is paced against it.
    void noteApplied(double nowMs) { lastAppliedMs_ = nowMs; }

    // Drops a held value that no longer makes sense (the source it was aimed
    // at is gone).
    void clearPending() { hasPending_ = false; }

    bool hasPending() const { return hasPending_; }

private:
    double minIntervalMs_;
    // Far past, so the first request after construction always applies.
    double lastAppliedMs_ = -1.0e300;
    double pendingHz_ = 0.0;
    bool hasPending_ = false;
};

}  // namespace cascade::core
