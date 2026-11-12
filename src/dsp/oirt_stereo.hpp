// OIRT (polar-modulation) FM stereo decoder: the stereo system used on the
// 65.8 to 74 MHz band in Russia, Belarus, Ukraine, Moldova and Armenia.
//
// Implemented from Recommendation ITU-R BS.450-3 (2001, editorial amendments
// October 2018; the same clauses stand in BS.450-4), Annex 1, section 2.1
// "Polar-modulation system", clauses 2.1.2.1 to 2.1.2.5 (free PDF at
// https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.450-3-200111-S!!PDF-E.pdf)
// and ordinary PLL/DSP theory. No other receiver's source was read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <vector>

namespace cascade::dsp {

// What the standard says, as this block uses it (all clause numbers are
// BS.450-3 section 2.1.2):
//
//  2.1.2.1  M = (A + B) / 2, pre-emphasised like a mono signal (50 us here).
//  2.1.2.2  S = (A - B) / 2, pre-emphasised the same way, amplitude-modulates
//           a sub-carrier at 31.25 kHz. "The spectrum of the amplitude-
//           modulated sub-carrier is formed so that the sub-carrier amplitude
//           is reduced by 14 dB and the spectral components of the given
//           modulating signal appear to be transformed" by
//               K(f) = (1 + j 6.4 f) / (5 + j 6.4 f),   f in kHz.
//           K(0) = 1/5 (-14 dB, the carrier) and K -> 1 at high f. K is a real
//           one-pole/one-zero network applied to the AM envelope: zero at
//           1/(2 pi tau) = 156 Hz, pole at 5x that, tau = 6.4/(2 pi 1000) s.
//  2.1.2.3  Multiplex = M + (K-shaped AM of an unsuppressed carrier by S).
//  2.1.2.4  Levels, relative to the maximum multiplex (= maximum deviation):
//           M 80 % max, S 80 % max, reduced sub-carrier 20 %.
//  2.1.2.5  Positive multiplex = positive deviation.
//  A = left, B = right (the clause calls them "left-hand signal, A" and
//  "right-hand signal, B").
//
// The decoder therefore:
//  1. recovers the 31.25 kHz sub-carrier with a quadrature PLL (the carrier is
//     only 20 % of full scale and S rides on it as AM, so the phase detector
//     uses the quadrature component, on which in-phase AM has no effect);
//  2. product-detects with the recovered carrier: the envelope e(t) is
//     K * (C (1 + s(t))), with C = 1 the unreduced carrier;
//  3. RESTORES THE SUPPRESSED LEVEL by running the envelope through K^-1 =
//     (5 + s tau)/(1 + s tau), which gives C (1 + s(t)) = 1 + S_pre(t); the
//     constant 1 is removed. Without this step S would be reduced by up to
//     14 dB at low audio frequencies and tilted, so separation would be poor
//     at bass frequencies (the break-it test removes exactly this step);
//  4. low-passes M and S identically (15 kHz), matrixes
//     A = (M + S) / 0.8, B = (M - S) / 0.8 (undoing the 80 % scaling), then
//     de-emphasises both channels.
//
// SCALING AND INPUT: the input is the FM discriminator output normalised so
// that +-1 is the maximum deviation of the transmitter (75 kHz or 50 kHz; the
// caller divides by whichever it set). The decoder takes the nominal levels
// from the standard (carrier 20 %, M and S 80 %) and does NOT estimate them;
// a station running different levels gets proportionally different output
// levels and a proportionally shifted lamp threshold, but separation is not
// affected by level.
//
// Absolute phase of the sub-carrier is not defined by the clause and is not
// needed: the carrier is the reference, an in-phase envelope means A > B.
//
// Output rate is compositeRate / decimation. Thread-safety: none (one owner).
class OirtStereo {
public:
    // compositeRateHz >= 100 kHz is needed (the multiplex reaches 46.25 kHz);
    // below that the object is a permanent mono decoder (stereoCapable() false).
    // decimation: integer >= 1; the audio output rate is rate / decimation and
    // must stay at least 32 kHz (else decimation is reduced).
    OirtStereo(double compositeRateHz, int decimation);

    // n composite samples in; returns the number of output sample pairs
    // written (n / decimation over a long run, exact carry across calls).
    // `left`, `right` need capacity outputsFor(n). Block-split independent.
    std::size_t process(const float* mpx, std::size_t n, float* left, float* right);

    // Upper bound on outputs of a process() call of n samples.
    std::size_t outputsFor(std::size_t n) const noexcept { return n / decim_ + 1; }

    double outputRate() const noexcept { return rate_ / decim_; }
    int decimation() const noexcept { return decim_; }

    void reset();

    // The stereo lamp: the 31.25 kHz carrier is present and the PLL is locked
    // to it (carrier magnitude above 30 % of nominal and quadrature below half
    // the in-phase component, held for 40 ms). Never true on a mono signal.
    bool stereoLamp() const noexcept { return lamp_; }
    // Recovered carrier magnitude as a fraction of the nominal 20 % (about 1
    // for a standard transmission, about 0 for mono or noise).
    float carrierLevel() const noexcept { return carrierLevel_; }

    void setForceMono(bool on) noexcept { forceMono_ = on; }
    bool forceMono() const noexcept { return forceMono_; }
    // De-emphasis microseconds (50 default, 0 = off).
    void setDeemphasisUs(double us);
    bool stereoCapable() const noexcept { return stereoCapable_; }

    // Test hook for the break-it check: bypass step 3 (the K^-1 restoration).
    void setRestoreLevelForTest(bool on) noexcept { restore_ = on; }

private:
    struct Slide {  // sliding sum over len samples, double accumulator
        std::vector<double> buf;
        std::size_t pos = 0;
        double sum = 0.0;
        void init(std::size_t len) { buf.assign(len, 0.0); pos = 0; sum = 0.0; }
        double push(double v) {
            sum += v - buf[pos];
            buf[pos] = v;
            if (++pos == buf.size()) { pos = 0; }
            return sum / static_cast<double>(buf.size());
        }
    };

    double rate_;
    int decim_;
    bool stereoCapable_;
    bool forceMono_ = false;
    bool restore_ = true;

    // PLL (runs at the composite rate for the mixer, updates every cicLen_).
    double theta_ = 0.0;     // mixer phase, radians
    double freq_ = 0.0;      // loop frequency correction, rad/sample
    double w0_ = 0.0;        // nominal 31.25 kHz, rad/sample
    double kp_ = 0.0, ki_ = 0.0, freqMax_ = 0.0;
    std::size_t cicLen_ = 1, cicCount_ = 0;
    Slide ci_[3], cq_[3];
    double iMean_ = 0.0, qMean_ = 0.0, meanAlpha_ = 0.0;
    double iSlow_ = 0.2;
    int lampHold_ = 0, lampHoldMax_ = 0;
    bool lamp_ = false;
    float carrierLevel_ = 0.0f;

    // K^-1 on the envelope (bilinear, composite rate).
    double kb0_ = 0, kb1_ = 0, ka1_ = 0, kx1_ = 0, ky1_ = 0;

    // Linear-phase low-pass (shared taps for M and the envelope).
    std::vector<float> taps_;
    std::vector<float> ringM_, ringE_;  // doubled ring buffers
    std::size_t ringPos_ = 0;
    int phaseCount_ = 0;

    // Output stage.
    double hpState_ = 0.0, hpPrev_ = 0.0, hpA_ = 0.0;
    double gate_ = 0.0, gateStep_ = 0.0;
    double dePole_ = 0.0, deL_ = 0.0, deR_ = 0.0, deTauSec_ = 0.0;
};

}  // namespace cascade::dsp
