// Biquad band-stop (notch) filter, plus an automatic tone-seeking notch.
//
// Both are deliberately classical, unencumbered DSP. The notch is the bilinear
// transform of the textbook analog band-stop prototype
//
//     H(s) = (s^2 + w0^2) / (s^2 + (w0/Q) s + w0^2)
//
// which places a pair of transmission zeros exactly ON the unit circle at
// +/-w0 and the matching poles just inside it. That construction predates
// software patents by decades and is the standard second-order notch found in
// every filter-design text, which is exactly why it was chosen for a product
// that ships commercially: there is nothing proprietary to infringe.
//
// The auto-notch is a peak-picker over a windowed DFT plus a Schmitt-trigger
// state machine — no adaptive-filter (LMS/ANF) patent surface, no vendor
// algorithm. See the comment above AutoNotch for the search and the
// hysteresis rules.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace cascade::dsp {

// Second-order band-stop ("notch") biquad.
//
// Implemented as transposed direct form II with double-precision state. A
// high-Q notch puts its poles within ~1e-5 of the unit circle; in float state
// the accumulated rounding of that nearly-undamped resonance is audible as a
// slow drift, and doubles cost nothing at audio rates.
//
// Two clamps keep the design in the region where the topology is actually
// stable, and both matter for a user-facing control that can be dragged to
// its endpoints:
//
//   - Frequency is clamped away from DC and Nyquist by kEdgeMarginNorm of the
//     sample rate. At exactly 0 or Nyquist, sin(w0) is 0, so alpha is 0 and
//     the poles land ON the unit circle on top of the zeros: in exact
//     arithmetic the filter degenerates to the identity, but the recursion is
//     marginally stable and float rounding makes it drift or ring forever.
//     Note the consequence: a notch "at DC" has unity gain at exactly 0 Hz
//     (|H(z=1)| = 1 for every legal design), so this is not a DC blocker.
//   - alpha itself has a floor, so a huge Q at a very low frequency still
//     leaves the poles strictly inside the unit circle.
class Notch {
public:
    Notch(double sampleRateHz, double freqHz, double qFactor = 30.0)
        : sampleRate_(sampleRateHz > 0.0 ? sampleRateHz : 1.0),
          freq_(freqHz),
          q_(qFactor) {
        design();
    }

    // Both setters clamp (see the class comment); the accessors report the
    // clamped value that is actually in use, not the requested one.
    void setFrequencyHz(double hz) {
        freq_ = hz;
        design();
    }
    void setQ(double q) {
        q_ = q;
        design();
    }

    // A disabled notch is a bit-exact passthrough: process() touches neither
    // the samples nor the filter state. Re-enabling therefore resumes from the
    // state the filter had when it was switched off; call reset() first if a
    // clean restart is wanted.
    void setEnabled(bool on) { enabled_ = on; }
    bool isEnabled() const { return enabled_; }

    double sampleRateHz() const { return sampleRate_; }
    double frequencyHz() const { return freq_; }
    double q() const { return q_; }

    // Filters n samples in place. State carries across calls, so any split of
    // a stream into blocks gives the same result as one big call.
    void process(float* inout, std::size_t n) {
        if (!enabled_) { return; }
        double z1 = z1_;
        double z2 = z2_;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = static_cast<double>(inout[i]);
            const double y = b0_ * x + z1;
            z1 = b1_ * x - a1_ * y + z2;
            z2 = b2_ * x - a2_ * y;
            inout[i] = static_cast<float>(y);
        }
        z1_ = z1;
        z2_ = z2;
    }

    // Forgets the filter memory; coefficients are untouched.
    void reset() {
        z1_ = 0.0;
        z2_ = 0.0;
    }

private:
    // 1e-4 of the sample rate: 4.8 Hz at 48 kHz. Far enough from the unit
    // circle's degenerate points to be numerically safe, close enough that no
    // frequency a user would type is meaningfully moved.
    static constexpr double kEdgeMarginNorm = 1e-4;
    static constexpr double kMinQ = 0.1;
    static constexpr double kMaxQ = 1e4;
    static constexpr double kMinAlpha = 1e-6;

    void design() {
        const double nyquist = 0.5 * sampleRate_;
        const double margin = sampleRate_ * kEdgeMarginNorm;
        // Written as "not greater" so a NaN request lands on the low clamp
        // instead of poisoning every coefficient.
        double f = freq_;
        if (!(f > margin)) { f = margin; }
        if (f > nyquist - margin) { f = nyquist - margin; }
        freq_ = f;

        double qq = q_;
        if (!(qq >= kMinQ)) { qq = kMinQ; }
        if (qq > kMaxQ) { qq = kMaxQ; }
        q_ = qq;

        constexpr double kTwoPi = 6.283185307179586476925286766559;
        const double w0 = kTwoPi * f / sampleRate_;
        const double cw = std::cos(w0);
        double alpha = std::sin(w0) / (2.0 * qq);
        if (alpha < kMinAlpha) { alpha = kMinAlpha; }

        // Bilinear-transformed prototype, normalized by a0 so the recursion
        // needs no per-sample division.
        const double a0 = 1.0 + alpha;
        b0_ = 1.0 / a0;
        b1_ = -2.0 * cw / a0;
        b2_ = 1.0 / a0;
        a1_ = -2.0 * cw / a0;
        a2_ = (1.0 - alpha) / a0;
    }

    double sampleRate_;
    double freq_;
    double q_;
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
    bool enabled_ = true;
};

// Automatic notch: finds the strongest narrowband tone in the incoming audio
// and parks a Notch on it.
//
// SEARCH. Every analysisSize input samples form one analysis frame: Hann
// window, forward FFT, magnitude-squared over the positive half spectrum. DC
// and Nyquist are excluded from the peak search (a "tone" at DC is a bias
// offset and at Nyquist an aliasing artifact — neither is something a notch
// should chase). The peak bin's frequency is refined by parabolic
// interpolation over the LOG magnitudes of the peak and its two neighbours,
// which is the standard sub-bin estimator for a windowed sinusoid and gets the
// estimate to a small fraction of a bin. Without it a 93.75 Hz bin spacing
// (512 bins at 48 kHz) would routinely miss a Q=30 notch whose null is only
// f0/Q = 100 Hz wide.
//
// The tone/no-tone decision uses PROMINENCE: the peak power divided by the
// MEDIAN power across the half spectrum, in dB. The median is used rather than
// the mean because it is insensitive to the tone itself — one huge bin out of
// several hundred cannot move it — so it estimates the broadband floor even
// while the tone is present. Broadband noise alone gives a peak/median ratio
// of roughly 10-12 dB (the max of a few hundred exponentially distributed
// bins over their median), so the thresholds below sit clear of it.
//
// The detector always looks at the INPUT, before the notch. Analysing the
// output would be a feedback loop: the moment the notch engaged, the tone
// would vanish from the analysis and the detector would immediately release.
//
// HYSTERESIS — three separate mechanisms, because a notch that hunts is worse
// than no notch at all:
//   1. Schmitt trigger on prominence: engage above kEngageDb, release only
//      below kReleaseDb. A tone fading around one threshold cannot chatter.
//   2. Confirmation count: a new frequency must be picked by kConfirmFrames
//      consecutive frames (and agree with itself to within the deadband)
//      before the notch retunes. One anomalous frame — a transient, a key
//      click — cannot move a parked notch.
//   3. Deadband: while engaged, a candidate within kDeadbandBins of the
//      current notch is treated as the SAME tone and ignored, so sub-bin
//      jitter in the estimator never rewrites the coefficients.
// On release the last frequency is retained rather than reset, so a tone that
// comes and goes re-engages where it left off instead of hunting.
class AutoNotch {
public:
    // analysisSize must be a legal ComplexFFT size (see fft.hpp); the
    // ComplexFFT constructor throws std::invalid_argument otherwise.
    explicit AutoNotch(double sampleRateHz, std::size_t analysisSize = 512,
                       double qFactor = 30.0)
        : sampleRate_(sampleRateHz > 0.0 ? sampleRateHz : 1.0),
          n_(analysisSize),
          fft_(analysisSize),
          window_(makeWindow(WindowType::Hann, analysisSize)),
          buf_(analysisSize, 0.0f),
          frame_(analysisSize),
          spec_(analysisSize),
          mag_(analysisSize / 2 + 1, 0.0f),
          scratch_(analysisSize / 2 + 1, 0.0f),
          notch_(sampleRateHz, 0.25 * sampleRateHz, qFactor) {
        // Starts disengaged: nothing has been detected yet, so the module must
        // be a passthrough until it earns the right to filter.
        notch_.setEnabled(false);
    }

    // Master switch. Disabled means bit-exact passthrough AND no analysis, so
    // a disabled auto-notch costs one branch per block.
    void setEnabled(bool on) { enabled_ = on; }
    bool isEnabled() const { return enabled_; }

    void setQ(double q) { notch_.setQ(q); }
    double q() const { return notch_.q(); }

    // True while a tone is being notched.
    bool isEngaged() const { return engaged_; }
    // Last frequency the notch parked on; 0 before anything has been found.
    // Retained across a release (see the hysteresis note above).
    double frequencyHz() const { return freq_; }
    // Prominence of the most recent analysis frame, in dB. Exposed because it
    // is the single number that explains every engage/release decision.
    float prominenceDb() const { return prominenceDb_; }
    std::size_t analysisSize() const { return n_; }

    // Analyses and filters n samples in place. Analysis frames are aligned to
    // the sample stream, not to call boundaries: samples are captured for
    // analysis and filtered with the CURRENT coefficients first, and a retune
    // only takes effect on the sample after a frame completes. That ordering
    // is what makes any chunking of the stream produce identical output.
    void process(float* inout, std::size_t n) {
        if (!enabled_) { return; }
        std::size_t i = 0;
        while (i < n) {
            const std::size_t take = std::min(n - i, n_ - fill_);
            std::copy(inout + i, inout + i + take, buf_.data() + fill_);
            notch_.process(inout + i, take);
            fill_ += take;
            i += take;
            if (fill_ == n_) {
                analyse();
                fill_ = 0;
            }
        }
    }

    // Back to the construction state: nothing detected, notch disabled and
    // cleared, analysis buffer empty.
    void reset() {
        std::fill(buf_.begin(), buf_.end(), 0.0f);
        fill_ = 0;
        engaged_ = false;
        freq_ = 0.0;
        candidateFreq_ = 0.0;
        candidateCount_ = 0;
        belowCount_ = 0;
        prominenceDb_ = 0.0f;
        notch_.reset();
        notch_.setEnabled(false);
    }

private:
    // Peak/median in dB. See the class comment for where the numbers come
    // from: broadband noise alone tops out around 12 dB (measured at 11.8 dB
    // over 40 blocks of white noise in tests/test_notch.cpp), so 20 dB to
    // engage leaves ~8 dB of margin and 16 dB to release leaves ~4 dB — and
    // the 4 dB between the two thresholds is the Schmitt hysteresis.
    static constexpr float kEngageDb = 20.0f;
    static constexpr float kReleaseDb = 16.0f;
    static constexpr int kConfirmFrames = 2;
    static constexpr int kReleaseFrames = 3;
    static constexpr double kDeadbandBins = 1.0;
    // Guards the log/divide when a frame is digital silence.
    static constexpr float kPowerEps = 1e-30f;

    void analyse() {
        for (std::size_t i = 0; i < n_; ++i) {
            frame_[i] = std::complex<float>(buf_[i] * window_[i], 0.0f);
        }
        fft_.forward(frame_.data(), spec_.data());

        const std::size_t half = n_ / 2;
        for (std::size_t k = 0; k <= half; ++k) { mag_[k] = std::norm(spec_[k]); }

        scratch_.assign(mag_.begin(), mag_.end());
        const std::size_t mid = scratch_.size() / 2;
        std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(mid),
                         scratch_.end());
        const float median = scratch_[mid];

        std::size_t peak = 1;
        float peakMag = -1.0f;
        for (std::size_t k = 1; k < half; ++k) {
            if (mag_[k] > peakMag) {
                peakMag = mag_[k];
                peak = k;
            }
        }
        prominenceDb_ = 10.0f * std::log10((peakMag + kPowerEps) / (median + kPowerEps));

        const double candidate = interpolatedFreq(peak);
        const double deadbandHz = kDeadbandBins * sampleRate_ / static_cast<double>(n_);

        if (engaged_) {
            if (prominenceDb_ < kReleaseDb) {
                candidateCount_ = 0;
                if (++belowCount_ >= kReleaseFrames) {
                    engaged_ = false;
                    notch_.setEnabled(false);
                }
                return;  // frequency retained: releasing is not forgetting
            }
            belowCount_ = 0;
            if (std::fabs(candidate - freq_) <= deadbandHz) {
                candidateCount_ = 0;  // same tone, jittering within a bin
                return;
            }
            if (prominenceDb_ < kEngageDb) {
                candidateCount_ = 0;  // not strong enough to justify moving
                return;
            }
        } else {
            belowCount_ = 0;
            if (prominenceDb_ < kEngageDb) {
                candidateCount_ = 0;
                return;
            }
        }

        // A candidate must repeat itself before it is believed.
        if (candidateCount_ > 0 && std::fabs(candidate - candidateFreq_) <= deadbandHz) {
            ++candidateCount_;
        } else {
            candidateFreq_ = candidate;
            candidateCount_ = 1;
        }
        if (candidateCount_ >= kConfirmFrames) {
            freq_ = candidateFreq_;
            notch_.setFrequencyHz(freq_);
            notch_.setEnabled(true);
            engaged_ = true;
            candidateCount_ = 0;
        }
    }

    // Parabolic (quadratic) interpolation of the peak location over log
    // magnitudes. Logs rather than raw powers because a windowed sinusoid's
    // main lobe is close to a parabola in dB, which is what makes the estimate
    // sub-bin accurate.
    double interpolatedFreq(std::size_t peak) const {
        double delta = 0.0;
        if (peak >= 1 && peak + 1 <= n_ / 2) {
            const double lm = std::log(static_cast<double>(mag_[peak - 1]) + kPowerEps);
            const double lc = std::log(static_cast<double>(mag_[peak]) + kPowerEps);
            const double lr = std::log(static_cast<double>(mag_[peak + 1]) + kPowerEps);
            const double denom = lm - 2.0 * lc + lr;
            if (denom < 0.0) {  // a genuine maximum; flat or cupped means no fit
                delta = 0.5 * (lm - lr) / denom;
                if (delta > 0.5) { delta = 0.5; }
                if (delta < -0.5) { delta = -0.5; }
            }
        }
        return (static_cast<double>(peak) + delta) * sampleRate_ / static_cast<double>(n_);
    }

    double sampleRate_;
    std::size_t n_;
    ComplexFFT fft_;
    std::vector<float> window_;
    std::vector<float> buf_;
    std::vector<std::complex<float>> frame_;
    std::vector<std::complex<float>> spec_;
    std::vector<float> mag_;
    std::vector<float> scratch_;
    Notch notch_;
    std::size_t fill_ = 0;
    bool enabled_ = true;
    bool engaged_ = false;
    double freq_ = 0.0;
    double candidateFreq_ = 0.0;
    int candidateCount_ = 0;
    int belowCount_ = 0;
    float prominenceDb_ = 0.0f;
};

}  // namespace cascade::dsp
