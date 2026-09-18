// Readouts a person can actually read (0.99.4).
//
// WHY. Some figures on the bench were printed straight from the frame being
// drawn, so their text changed sixty times a second: FRAME TIME took
// ImGui's per-frame delta ("49 % - 8.1 ms" became "52 % - 8.6 ms" became
// ...) and the AUDIO - UNDERRUNS card reprinted its counts and the ring level
// every frame. Reported from the field as "updated so fast they are
// difficult to read". A number that changes faster than it can be read
// carries no information at all, however accurate each frame's value is.
//
// WHAT. Two holds, both driven by a caller-supplied clock so they are pure
// and testable:
//   - HeldMean: for a measurement that jitters frame to frame (frame time).
//     The text shows the MEAN of the last whole period, which is both calmer
//     and more truthful than whichever single frame happened to be sampled.
//   - HeldSample: for values that should be shown as they are but not
//     reprinted every frame (counters, a buffer level). The shown value is
//     the live one, re-read at most once a period.
// Needles and other analogue marks stay live - movement is how a meter is
// read; only the TEXT is held.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>

namespace cascade::gui {

// How long a held readout stays put: twice a second. Slow enough to read a
// changing figure, fast enough that a real change still shows promptly.
inline constexpr double kReadoutHoldS = 0.5;

class HeldMean {
public:
    explicit HeldMean(double periodS = kReadoutHoldS) : period_(periodS) {}

    // Feed one sample at time `nowS` (seconds, any monotonic origin). A
    // non-finite sample is ignored. The very first sample is shown at once,
    // so a readout is never blank for its first half second; after that the
    // shown value changes only when a period closes.
    void add(double nowS, double sample) {
        if (!std::isfinite(sample) || !std::isfinite(nowS)) { return; }
        // A clock that went backwards (a reset origin) restarts the window
        // rather than holding the old figure until time catches up.
        if (!started_ || nowS < start_) {
            started_ = true;
            start_ = nowS;
            sum_ = 0.0;
            n_ = 0;
            if (!have_) {
                shown_ = sample;
                have_ = true;
            }
        }
        sum_ += sample;
        ++n_;
        if (nowS - start_ >= period_) {
            shown_ = sum_ / static_cast<double>(n_);
            start_ = nowS;
            sum_ = 0.0;
            n_ = 0;
        }
    }

    bool have() const { return have_; }
    double value() const { return shown_; }

private:
    double period_;
    bool started_ = false;
    bool have_ = false;
    double start_ = 0.0;
    double sum_ = 0.0;
    long long n_ = 0;
    double shown_ = 0.0;
};

template <class T>
class HeldSample {
public:
    explicit HeldSample(double periodS = kReadoutHoldS) : period_(periodS) {}

    // What to DISPLAY at time `nowS`, given the live value. The first call
    // shows `live` immediately; after that `live` is re-read once a period.
    const T& value(double nowS, const T& live) {
        if (!have_ || !(nowS - last_ < period_) || nowS < last_) {
            shown_ = live;
            last_ = nowS;
            have_ = true;
        }
        return shown_;
    }

private:
    double period_;
    bool have_ = false;
    double last_ = 0.0;
    T shown_{};
};

}  // namespace cascade::gui
