// FastNco: the receive oscillator and the generator's tones, with the maths library called once every 32 samples instead of twice every sample.
//
// A SEPARATE CLASS, NOT A CHANGE TO Nco (2026-11-23, the demod performance pass). Nco (dsp/nco.hpp) is unchanged, bit for bit; this class is used only
// where the 2026-11 diagnosis found the per-sample cosine and sine to be a quarter of the demod phase: Vfo's mixer and the built-in generator's tones.
//
// HOW IT STAYS ACCURATE. The phase accumulator, its wrap and phase() are Nco's, line for line, so retuning continuity, block-split invariance, the
// dyadic-frequency exactness and phase() are the same bits as Nco's. What differs is only how the two output components of a sample are made: every
// 32nd sample OF THE STREAM (a counter in the object, not the start of a call) is an ANCHOR - the library cos and sin of the accumulator's own phase,
// exactly Nco's value - and the 31 samples after it are the anchor times a table entry W[k] = (cos, sin)(2*pi*f*k), one complex multiply in double,
// then the cast to float. There is no recurrence: every sample is at most one multiply away from a library value, so the error is a few units in the last
// place of a DOUBLE (about 1e-15, from the table's angle, the library and the product) and does not grow with the length of the run. Against Nco the
// float result differs only when the double value lies within that distance of a float rounding boundary: about one component in ten million, by one
// unit in the last place of a float (6e-8 of full scale).
//
// A frequency change rebuilds the table and re-anchors at the next sample; reset() zeroes the phase and re-anchors. The interface is Nco's.
// dsp/dsp_reference.hpp's kRefNco puts a FastNco back on Nco's per-sample library path for a test that compares the two.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>

#include "dsp/dsp_reference.hpp"

namespace cascade::dsp {

class FastNco {
public:
    // Frequency 0, phase 0: the table is all (1, 0), so an oscillator that is never given a frequency is Nco's constant 1 + 0j (the Vfo's default offset).
    FastNco() {
        for (std::size_t k = 0; k < kAnchorInterval; ++k) {
            wc_[k] = 1.0;
            ws_[k] = 0.0;
        }
    }

    // The number of samples between two library evaluations.
    static constexpr std::size_t kAnchorInterval = 32;

    // Cycles per sample, folded into [-0.5, 0.5) exactly as Nco::setFrequency does.
    void setFrequency(double normalized) {
        freq_ = normalized - std::floor(normalized + 0.5);
        for (std::size_t k = 0; k < kAnchorInterval; ++k) {
            // The angle of the k-th step after an anchor, from the cycle count reduced into one turn before the multiply by 2*pi.
            // f*k is computed exactly as a sum hi + lo (a fused multiply-add gives the product's rounding error), so the rounding of a product as large as
            // 15 turns does not leak into the angle; the whole turns are then removed from hi (an exact subtraction).
            const double fk = freq_ * static_cast<double>(k);
            const double fkLow = std::fma(freq_, static_cast<double>(k), -fk);
            const double reduced = (fk - std::floor(fk + 0.5)) + fkLow;
            const double a = kTwoPi * reduced;
            wc_[k] = std::cos(a);
            ws_[k] = std::sin(a);
        }
        pos_ = 0;   // re-anchor at the next sample
    }

    double frequency() const noexcept { return freq_; }

    // Phase in radians in [-pi, pi): the phase of the NEXT sample (Nco::phase).
    double phase() const noexcept { return kTwoPi * phaseCycles_; }

    // Zeroes the phase; the frequency is kept.
    void reset() noexcept {
        phaseCycles_ = 0.0;
        pos_ = 0;
    }

    // n oscillator samples e^{j(2*pi*f*k + phi0)}; the phase carries across calls.
    void generate(std::complex<float>* out, std::size_t n) {
        if (referenceKernel(kRefNco)) {
            for (std::size_t i = 0; i < n; ++i) {
                const double a = kTwoPi * phaseCycles_;
                out[i] = std::complex<float>(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
                step();
            }
            pos_ = 0;
            return;
        }
        std::size_t i = 0;
        while (i < n) {
            takeAnchorIfDue();
            std::size_t k = pos_;
            for (; k < kAnchorInterval && i < n; ++k, ++i) {
                out[i] = std::complex<float>(static_cast<float>(ac_ * wc_[k] - as_ * ws_[k]), static_cast<float>(ac_ * ws_[k] + as_ * wc_[k]));
                step();
            }
            pos_ = k == kAnchorInterval ? 0 : k;
        }
    }

    // out[k] = in[k] * oscillator[k]; in == out is allowed (the input is read before the output is written).
    void mix(const std::complex<float>* in, std::complex<float>* out, std::size_t n) {
        if (referenceKernel(kRefNco)) {
            for (std::size_t i = 0; i < n; ++i) {
                const double a = kTwoPi * phaseCycles_;
                const std::complex<float> osc(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
                out[i] = in[i] * osc;
                step();
            }
            pos_ = 0;
            return;
        }
        std::size_t i = 0;
        while (i < n) {
            takeAnchorIfDue();
            std::size_t k = pos_;
            for (; k < kAnchorInterval && i < n; ++k, ++i) {
                const std::complex<float> osc(static_cast<float>(ac_ * wc_[k] - as_ * ws_[k]), static_cast<float>(ac_ * ws_[k] + as_ * wc_[k]));
                out[i] = in[i] * osc;
                step();
            }
            pos_ = k == kAnchorInterval ? 0 : k;
        }
    }

private:
    static constexpr double kTwoPi = 6.283185307179586476925286766559;

    // Nco::step, unchanged.
    void step() noexcept {
        phaseCycles_ += freq_;
        if (phaseCycles_ >= 0.5) {
            phaseCycles_ -= 1.0;
        } else if (phaseCycles_ < -0.5) {
            phaseCycles_ += 1.0;
        }
    }

    void takeAnchorIfDue() {
        if (pos_ != 0) { return; }
        const double a = kTwoPi * phaseCycles_;
        ac_ = std::cos(a);
        as_ = std::sin(a);
    }

    double freq_ = 0.0;
    double phaseCycles_ = 0.0;
    std::size_t pos_ = 0;   // samples since the last anchor, in [0, 32); 0 = the next sample is an anchor
    double ac_ = 1.0, as_ = 0.0;
    double wc_[kAnchorInterval];
    double ws_[kAnchorInterval];
};

}  // namespace cascade::dsp
