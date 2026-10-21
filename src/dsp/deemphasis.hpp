// The FM de-emphasis network on its own: one pole, one time constant.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstddef>

namespace cascade::dsp {

// FM de-emphasis. Broadcast FM pre-emphasizes highs with an RC network - 75 us
// in the Americas, 50 us elsewhere, which is what setTimeUs picks between - and
// the receiver undoes it with the matching one-pole low-pass
// H(s) = 1/(1 + s*tau). Discretized by pole matching (impulse invariance): the
// analog pole at s = -1/tau maps to z = p = exp(-T/tau) with T = 1/rate, and the
// numerator is scaled so DC gain is exactly 1:
//     H(z) = (1 - p) / (1 - p*z^-1)
// Pole matching over bilinear because it preserves both the time constant and
// the DC gain exactly with no frequency prewarping decision; the de-emphasis
// corner (1/(2*pi*tau) ~ 3.2 kHz at 50 us) sits far below Nyquist at any
// channel rate this project runs, where the two mappings agree closely anyway.
//
// IT LIVES HERE, not inside the demodulator, because two owners need exactly
// this filter: dsp::Demodulator (the receiver's VFO, both FM modes) and
// core::patch::Strip (one channel of the airband monitor, FM rows only). It was
// inline in the demodulator's process() until 0.99.69; a strip's FM audio is
// the same radians-per-sample the discriminator makes, so the same network
// belongs behind it, and a second copy of the arithmetic is how the two would
// come to disagree about what 50 us means.
//
// Default 50 us: the standard everywhere except the Americas and South Korea,
// so the correct global default; setTimeUs makes it a choice. 0 (or anything
// non-finite or negative) means "off", implemented as a pole of exactly 0 -
// y = (1-p)*x + p*y collapses to y = x, so an owner that de-emphasises further
// downstream (the pipeline's WFM, inside StereoFm) can set 0 and no signal is
// ever de-emphasised twice.
//
// Pole and state are double so the filter matches its analytic transfer
// function to well below any audio-relevant error. Changing the time constant
// or the rate clears the state: carrying one charged at the old constant is an
// audible thump.
class Deemphasis {
public:
    explicit Deemphasis(double rateHz = 48000.0) : rate_(rateHz) { design(kDefaultTauSec); }

    // The rate the filter runs at. Keeps the time constant (the pole is
    // redesigned for the new rate) and clears the state.
    void setRate(double rateHz) {
        rate_ = rateHz;
        state_ = 0.0;
        design(tauSec_);
    }

    // Anything not a positive finite number means "off", so a bad value can
    // never poison the filter with a NaN pole.
    void setTimeUs(double us) {
        state_ = 0.0;
        design((us > 0.0 && std::isfinite(us)) ? us * 1.0e-6 : 0.0);
    }

    // The time constant in force, in microseconds; 0 when off.
    double timeUs() const { return tauSec_ * 1.0e6; }

    // True when the filter does anything. A pole of exactly 0 is a
    // pass-through, so a caller may skip the call entirely.
    bool enabled() const { return pole_ > 0.0; }

    // Forgets the filter's memory, as if no input had ever been seen.
    void reset() { state_ = 0.0; }

    float step(float x) {
        state_ = (1.0 - pole_) * static_cast<double>(x) + pole_ * state_;
        return static_cast<float>(state_);
    }

    // In place over n samples; any block split of a stream gives the same
    // output as one call.
    void process(float* x, std::size_t n) {
        const double p = pole_;
        const double g = 1.0 - p;
        double s = state_;
        for (std::size_t i = 0; i < n; ++i) {
            s = g * static_cast<double>(x[i]) + p * s;
            x[i] = static_cast<float>(s);
        }
        state_ = s;
    }

private:
    static constexpr double kDefaultTauSec = 50e-6;

    // A rate that is not positive has no pole to place: off, not a pole
    // outside the unit circle.
    void design(double tauSec) {
        tauSec_ = tauSec;
        pole_ = (tauSec > 0.0 && rate_ > 0.0) ? std::exp(-1.0 / (rate_ * tauSec)) : 0.0;
    }

    double rate_;
    double tauSec_ = 0.0;
    double pole_ = 0.0;
    double state_ = 0.0;
};

}  // namespace cascade::dsp
