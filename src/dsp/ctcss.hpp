// CTCSS (sub-audible tone squelch) for NFM audio: the standard tone table, a high-pass
// that takes the tone out of the audio, and a detector that reports which tone is present
// and whether a CHOSEN tone is.
//
// STANDALONE BLOCK. Nothing here is wired into the pipeline, the engine, the settings or the
// screen (docs/feature-records/signal-blocks.md, "Wiring hand-off", says where it goes).
//
// SOURCES (clean room: published tables and textbook filter design only).
//  * The tone table: the 39 tones of the Wikipedia article "Continuous Tone-Coded Squelch
//    System" (read 2026-11-11), which cites EIA RS-220-A (1979) and TIA-603-E (2016, p. 10) as
//    the standards. THE STANDARD ITSELF WAS NOT READ: the table is UNVERIFIED against it. The
//    same article marks 69.3 Hz as "some radios use 69.4" and lists a second group of eleven
//    extra tones (159.8 ... 254.1), which it marks as NOT in TIA-603-E. 150.0 Hz (NATO radios)
//    is non-standard and is not offered here.
//  * The high-pass is an inverse-Chebyshev (Chebyshev type II) design: analytic pole/zero
//    formulas from any filter-design textbook (e.g. Oppenheim and Schafer, "Discrete-Time
//    Signal Processing", the Chebyshev type II low-pass), low-pass to high-pass transformed
//    s -> w0/s and mapped to the digital domain by the bilinear transform with the edge
//    frequency pre-warped. The coefficients are computed here for the sample rate in use.
//  * The detector is a bank of exponentially-weighted single-bin DFTs (one complex
//    demodulator and one-pole smoother per tone). That is plain signal processing, not a copy
//    of any radio program. The detection and release times are this block's own measured
//    engineering numbers (the facts report found no authoritative figure; the only published
//    statement is that low tones take longer to decode than high ones).
//
// RATES. Both classes take the sample rate of the audio they are fed (any rate from
// kMinRateHz up). Nothing assumes 48 kHz: the detector decimates internally to 1.5-2.4 kHz
// whatever it is given, and the high-pass computes its coefficients from the rate.
//
// THREADS AND ALLOCATION. configure() / the constructors allocate and belong to set-up (control
// thread, with the DSP thread parked or before it starts). process() does not allocate, lock,
// log or wait: it may be called from the DSP thread only. The read-outs (detectedIndex(),
// gateOpen(), ...) are atomics and may be read from any thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::dsp {

// ---------------------------------------------------------------------------------------
// The tone table.
// ---------------------------------------------------------------------------------------

// The 39 tones of TIA-603-E per the Wikipedia article above (UNVERIFIED against the
// standard text), ascending.
inline constexpr std::array<double, 39> kCtcssStandardTones = {
    67.0,  69.3,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,  94.8,  97.4,  100.0,
    103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8, 136.5, 141.3, 146.2, 151.4, 156.7,
    162.2, 167.9, 173.8, 179.9, 186.2, 192.8, 203.5, 210.7, 218.1, 225.7, 233.6, 241.8, 250.3};

// The eleven additional tones of the "50 tone" sets (same article; not in TIA-603-E).
inline constexpr std::array<double, 11> kCtcssExtraTones = {159.8, 165.5, 171.3, 177.3, 183.5, 189.9,
                                                            196.6, 199.5, 206.5, 229.1, 254.1};

inline constexpr int kCtcssStandardCount = 39;
inline constexpr int kCtcssAllCount = 50;

// Tone `i` of the combined list, ascending by frequency: 50 entries (the 39 standard tones and
// the 11 extras merged). Returns 0.0 for an index outside 0..49.
inline double ctcssToneHz(int i) {
    static const std::array<double, kCtcssAllCount> all = [] {
        std::array<double, kCtcssAllCount> a{};
        std::merge(kCtcssStandardTones.begin(), kCtcssStandardTones.end(), kCtcssExtraTones.begin(),
                   kCtcssExtraTones.end(), a.begin());
        return a;
    }();
    if (i < 0 || i >= kCtcssAllCount) { return 0.0; }
    return all[static_cast<std::size_t>(i)];
}

// True when `hz` is one of the 39 standard tones (to within 0.05 Hz).
inline bool ctcssIsStandardTone(double hz) {
    for (double t : kCtcssStandardTones) {
        if (std::fabs(t - hz) < 0.05) { return true; }
    }
    return false;
}

// Index in the combined 50-tone list of the tone nearest `hz` if it is within 0.05 Hz, else -1.
inline int ctcssIndexOfHz(double hz) {
    for (int i = 0; i < kCtcssAllCount; ++i) {
        if (std::fabs(ctcssToneHz(i) - hz) < 0.05) { return i; }
    }
    return -1;
}

// ---------------------------------------------------------------------------------------
// The high-pass that removes the tone from the audio.
// ---------------------------------------------------------------------------------------

// Tenth-order inverse-Chebyshev high-pass (five biquads, double precision state).
//
// Design: the stop-band edge is the highest tone, 254.1 Hz, with kStopbandDb of attenuation
// from there downwards; the pass-band edge is 300 Hz. The inverse-Chebyshev pass-band is
// monotone (no ripple), so the loss rises smoothly towards the edge. Measured (see the block
// test): at least 40 dB of rejection at 254.1 Hz and below, about 1 dB or less at 300 Hz,
// under 0.3 dB from 400 Hz, flat above.
//
// The filter is not linear-phase: it has the group delay of any IIR high-pass of its sharpness
// near the edge (some milliseconds at 300-400 Hz), none to speak of above 1 kHz. It settles in
// about 50 ms from a start.
//
// Thread rules: configure() allocates nothing but is not safe against a concurrent process();
// process() is DSP-thread only and allocation-free.
class CtcssHighpass {
public:
    static constexpr int kOrder = 10;
    static constexpr int kSections = kOrder / 2;
    static constexpr double kStopEdgeHz = 254.1;
    static constexpr double kPassEdgeHz = 300.0;
    static constexpr double kStopbandDb = 40.0;
    static constexpr double kMinRateHz = 4000.0;

    CtcssHighpass() = default;
    explicit CtcssHighpass(double sampleRateHz) { configure(sampleRateHz); }

    // False (and the filter left unconfigured, a pass-through) when the rate is below
    // kMinRateHz, not finite, or not above 2.5 times the pass edge.
    bool configure(double sampleRateHz) {
        configured_ = false;
        if (!std::isfinite(sampleRateHz) || sampleRateHz < kMinRateHz) { return false; }
        constexpr double kPi = 3.14159265358979323846;
        const double fs = sampleRateHz;
        const double c = 2.0 * fs;  // bilinear: s = c (1 - z^-1) / (1 + z^-1)
        // Prototype: inverse Chebyshev low-pass, stop-band edge 1 rad/s, kStopbandDb.
        const double eps = 1.0 / std::sqrt(std::pow(10.0, kStopbandDb / 10.0) - 1.0);
        const double a = std::asinh(1.0 / eps) / kOrder;
        // The low-pass stop-band edge 1 maps (s -> w0/s) onto the high-pass stop-band edge, which
        // is the pre-warped analogue frequency of kStopEdgeHz.
        const double w0 = c * std::tan(kPi * kStopEdgeHz / fs);
        for (int k = 0; k < kSections; ++k) {
            const double theta = (2.0 * (k + 1) - 1.0) * kPi / (2.0 * kOrder);
            // Low-pass prototype pole: 1 / (sigma + j omega) with the Chebyshev-I-like pair below.
            const double sigma = -std::sinh(a) * std::sin(theta);
            const double omega = std::cosh(a) * std::cos(theta);
            const std::complex<double> pLp = 1.0 / std::complex<double>(sigma, omega);
            const double zLp = 1.0 / std::cos(theta);  // zero at +-j zLp
            // Low-pass -> high-pass: s -> w0 / s maps pole p to w0/p and zero jz to w0/(jz).
            const std::complex<double> pHp = w0 / pLp;
            const double zHp = w0 / zLp;  // zero at +-j zHp
            // Analogue section: (s^2 + zHp^2) / (s^2 - 2 Re(p) s + |p|^2), unit gain at s -> inf.
            const double b0 = zHp * zHp;
            const double a1 = -2.0 * pHp.real();
            const double a0 = std::norm(pHp);
            // Bilinear: substitute s = c (1 - q)/(1 + q), q = z^-1; multiply through by (1+q)^2.
            const double c2 = c * c;
            const double nb0 = c2 + b0, nb1 = -2.0 * c2 + 2.0 * b0, nb2 = c2 + b0;
            const double da0 = c2 + a1 * c + a0, da1 = -2.0 * c2 + 2.0 * a0, da2 = c2 - a1 * c + a0;
            Section& s = sec_[static_cast<std::size_t>(k)];
            s.b0 = nb0 / da0;
            s.b1 = nb1 / da0;
            s.b2 = nb2 / da0;
            s.a1 = da1 / da0;
            s.a2 = da2 / da0;
            s.z1 = s.z2 = 0.0;
        }
        rate_ = fs;
        configured_ = true;
        return true;
    }

    bool configured() const { return configured_; }
    double sampleRateHz() const { return rate_; }

    // Forgets the filter's memory (a new carrier, a retune).
    void reset() {
        for (Section& s : sec_) { s.z1 = s.z2 = 0.0; }
    }

    // In place or not (in == out is fine). A non-finite input sample is treated as 0 so one bad
    // sample cannot poison the recursion. Unconfigured: a copy.
    void process(const float* in, float* out, std::size_t n) {
        if (!configured_) {
            if (in != out) { std::copy(in, in + n, out); }
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            double x = static_cast<double>(in[i]);
            if (!std::isfinite(x)) { x = 0.0; }
            for (Section& s : sec_) {
                const double y = s.b0 * x + s.z1;  // transposed direct form II
                s.z1 = s.b1 * x - s.a1 * y + s.z2;
                s.z2 = s.b2 * x - s.a2 * y;
                x = y;
            }
            out[i] = static_cast<float>(x);
        }
    }

private:
    struct Section {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    };
    std::array<Section, kSections> sec_{};
    double rate_ = 0.0;
    bool configured_ = false;
};

// ---------------------------------------------------------------------------------------
// The detector.
// ---------------------------------------------------------------------------------------

// Decides, about every 10 ms, which CTCSS tone (if any) dominates the sub-audible band of the audio
// it is fed, and (optionally) whether a CHOSEN tone is the one present.
//
// FRONT END. Mean-of-D1 decimation to about 9.6 kHz (or none, below 12 kHz), a fourth-order
// Butterworth low-pass at 320 Hz, decimation to 1.5-2.4 kHz, a one-pole DC blocker (about 10
// Hz: a discriminator's carrier-offset DC would otherwise lean on the lowest tones).
//
// BANK. One complex demodulator per tone with a one-pole smoother. The smoother's time constant
// follows the tone's distance to the nearest other tone of the SET IN USE: tau = 0.62 / spacing
// clamped to [0.06 s, 0.30 s]. That is what makes 67.0 vs 69.3 Hz (2.3 Hz apart) separable (the
// neighbour is about 12 dB down) and makes the high tones quick; it is also why low tones take
// longer, as the published description says they do.
//
// DECISION, per hop (10 ms of the decimated stream). The candidate is the tone with the
// largest smoothed power P. It is PRESENT in a hop when
//   (1) P > kPeakToMedian x the median of all tone powers (a peak, not a smear), and
//   (2) P > kPeakToNeighbour x the power of each of the two tones next to it in the set (a
//       tone half-way between two standard tones, or the start-up transient of a tone, gives
//       two similar bins and decides nothing), and
//   (3) the NORMALISED SCORE P / (band mean square x g) exceeds kMinScore, where g = a/(2-a) is
//       what a one-pole smoother of weight a lets through of white noise: for noise alone the
//       score is exponentially distributed with mean 1, so the false-lock rate is set by
//       kMinScore directly and equally for the fast high tones and the slow low ones, and
//   (4) the band has some signal at all (P > 1e-12).
// It LOCKS when the same candidate is present for kLockHops hops in a row, and RELEASES when it
// has been absent (or another tone has taken over) for kReleaseHops hops. With a chosen tone,
// gateOpen() is true only while the lock is on THAT tone.
//
// Measured times are in the block test and the record; they are NOT a standard.
class CtcssDetector {
public:
    static constexpr double kMinRateHz = 4000.0;
    static constexpr double kHopS = 0.010;
    static constexpr double kMinTauS = 0.06;
    static constexpr double kMaxTauS = 0.30;
    static constexpr double kPeakToMedian = 10.0;
    static constexpr double kPeakToNeighbour = 2.0;
    static constexpr double kMinScore = 30.0;
    static constexpr int kLockHops = 6;
    static constexpr int kReleaseHops = 15;

    CtcssDetector() = default;
    CtcssDetector(double sampleRateHz, bool includeExtraTones) { configure(sampleRateHz, includeExtraTones); }

    // Set-up. Allocates. False (detector unconfigured, process() a no-op) for a rate below
    // kMinRateHz or not finite.
    bool configure(double sampleRateHz, bool includeExtraTones) {
        configured_ = false;
        if (!std::isfinite(sampleRateHz) || sampleRateHz < kMinRateHz) { return false; }
        constexpr double kPi = 3.14159265358979323846;
        rate_ = sampleRateHz;
        d1_ = std::max(1, static_cast<int>(std::floor(rate_ / 9600.0)));
        fs1_ = rate_ / d1_;
        d2_ = std::max(1, static_cast<int>(std::floor(fs1_ / 1500.0)));
        fs2_ = fs1_ / d2_;
        // 4th-order Butterworth low-pass at 320 Hz, at fs1 (two biquads), bilinear with pre-warp.
        {
            const double k = std::tan(kPi * 320.0 / fs1_);
            const double qs[2] = {0.5411961001461970, 1.3065629648763766};  // Butterworth-4 section Qs
            for (int i = 0; i < 2; ++i) {
                const double norm = 1.0 / (1.0 + k / qs[i] + k * k);
                Bq& b = lp_[static_cast<std::size_t>(i)];
                b.b0 = k * k * norm;
                b.b1 = 2.0 * b.b0;
                b.b2 = b.b0;
                b.a1 = 2.0 * (k * k - 1.0) * norm;
                b.a2 = (1.0 - k / qs[i] + k * k) * norm;
                b.z1 = b.z2 = 0.0;
            }
        }
        dcR_ = std::exp(-2.0 * kPi * 10.0 / fs2_);
        dcX_ = dcY_ = 0.0;
        // The tones in use.
        tones_.clear();
        for (int i = 0; i < kCtcssAllCount; ++i) {
            const double hz = ctcssToneHz(i);
            if (includeExtraTones || ctcssIsStandardTone(hz)) { tones_.push_back(Tone{}); tones_.back().hz = hz; tones_.back().allIndex = i; }
        }
        const std::size_t n = tones_.size();
        for (std::size_t i = 0; i < n; ++i) {
            double spacing = 1e9;
            if (i > 0) { spacing = std::min(spacing, tones_[i].hz - tones_[i - 1].hz); }
            if (i + 1 < n) { spacing = std::min(spacing, tones_[i + 1].hz - tones_[i].hz); }
            const double tau = std::clamp(0.62 / spacing, kMinTauS, kMaxTauS);
            Tone& t = tones_[i];
            t.alpha = 1.0 - std::exp(-1.0 / (tau * fs2_));
            t.noiseGain = t.alpha / (2.0 - t.alpha);
            t.dphi = 2.0 * kPi * t.hz / fs2_;
            t.osc = {1.0, 0.0};
            t.rot = {std::cos(t.dphi), std::sin(t.dphi)};
            t.acc = {0.0, 0.0};
            t.power = 0.0;
        }
        scratch_.assign(n, 0.0);
        bandAlpha_ = 1.0 - std::exp(-1.0 / (0.30 * fs2_));
        hopLen2_ = std::max(1, static_cast<int>(std::lround(kHopS * fs2_)));
        configured_ = true;
        reset();
        return true;
    }

    bool configured() const { return configured_; }
    double sampleRateHz() const { return rate_; }
    int toneCount() const { return static_cast<int>(tones_.size()); }
    // Tone `i` of THIS detector's set, ascending; 0.0 out of range.
    double toneHz(int i) const {
        return (i >= 0 && i < toneCount()) ? tones_[static_cast<std::size_t>(i)].hz : 0.0;
    }
    // The decimated rate the bank runs at (for the block test and the record).
    double decimatedRateHz() const { return fs2_; }

    // Chooses the tone whose presence gateOpen() reports: hz <= 0 means none chosen
    // (gateOpen() then reports any locked tone). A frequency that is not in the set in use
    // selects nothing and is refused (false), leaving the previous choice. Control thread or
    // DSP thread.
    bool setSelectedToneHz(double hz) {
        if (hz <= 0.0) {
            selected_.store(-1, std::memory_order_relaxed);
            return true;
        }
        for (std::size_t i = 0; i < tones_.size(); ++i) {
            if (std::fabs(tones_[i].hz - hz) < 0.05) {
                selected_.store(static_cast<int>(i), std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }
    int selectedIndex() const { return selected_.load(std::memory_order_relaxed); }

    // Forgets everything (a retune, a new channel). Selection is kept.
    void reset() {
        for (Bq& b : lp_) { b.z1 = b.z2 = 0.0; }
        accum1_ = 0.0;
        count1_ = 0;
        count2_ = 0;
        dcX_ = dcY_ = 0.0;
        bandPower_ = 0.0;
        hopCount_ = 0;
        for (Tone& t : tones_) {
            t.osc = {1.0, 0.0};
            t.acc = {0.0, 0.0};
            t.power = 0.0;
        }
        candidate_ = -1;
        runLength_ = 0;
        absentRun_ = 0;
        locked_ = -1;
        lockedPub_.store(-1, std::memory_order_relaxed);
        gatePub_.store(false, std::memory_order_relaxed);
    }

    // Feeds audio. Reads it, never writes it. Non-finite samples count as 0.
    // DSP thread only; no allocation.
    void process(const float* in, std::size_t n) {
        if (!configured_) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            double x = static_cast<double>(in[i]);
            if (!std::isfinite(x)) { x = 0.0; }
            accum1_ += x;
            if (++count1_ < d1_) { continue; }
            double v = accum1_ / d1_;
            accum1_ = 0.0;
            count1_ = 0;
            for (Bq& b : lp_) {
                const double y = b.b0 * v + b.z1;
                b.z1 = b.b1 * v - b.a1 * y + b.z2;
                b.z2 = b.b2 * v - b.a2 * y;
                v = y;
            }
            if (++count2_ < d2_) { continue; }
            count2_ = 0;
            stepDecimated(v);
        }
    }

    // --- read-outs (atomic; any thread) ---------------------------------------------

    // Index (in THIS detector's set) of the locked tone, -1 when none is locked.
    int detectedIndex() const { return lockedPub_.load(std::memory_order_relaxed); }
    // Its frequency in Hz, 0.0 when none.
    double detectedHz() const {
        const int i = detectedIndex();
        return i < 0 ? 0.0 : tones_[static_cast<std::size_t>(i)].hz;
    }
    // The tone gate: with a chosen tone, true while the lock is on that tone; with none chosen,
    // true while any tone is locked.
    bool gateOpen() const { return gatePub_.load(std::memory_order_relaxed); }

private:
    struct Bq {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    };
    struct Tone {
        double hz = 0, alpha = 0, noiseGain = 0, dphi = 0, power = 0;
        int allIndex = 0;
        std::complex<double> osc{1, 0}, rot{1, 0}, acc{0, 0};
    };

    void stepDecimated(double v) {
        // DC blocker.
        const double d = v - dcX_ + dcR_ * dcY_;
        dcX_ = v;
        dcY_ = d;
        bandPower_ += bandAlpha_ * (d * d - bandPower_);
        for (Tone& t : tones_) {
            // y += alpha * (x * conj(osc) - y); osc advances by rot (unit phasor, renormalised).
            const std::complex<double> m = d * std::conj(t.osc);
            t.acc += t.alpha * (m - t.acc);
            t.osc *= t.rot;
            t.power = std::norm(t.acc);
        }
        if (++renorm_ >= 256) {
            renorm_ = 0;
            for (Tone& t : tones_) { t.osc /= std::abs(t.osc); }
        }
        if (++hopCount_ >= hopLen2_) {
            hopCount_ = 0;
            decide();
        }
    }

    void decide() {
        const std::size_t n = tones_.size();
        std::size_t best = 0;
        double bestP = -1.0;
        for (std::size_t i = 0; i < n; ++i) {
            scratch_[i] = tones_[i].power;
            if (tones_[i].power > bestP) {
                bestP = tones_[i].power;
                best = i;
            }
        }
        std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(n / 2), scratch_.end());
        const double median = scratch_[n / 2];
        const double left = best > 0 ? tones_[best - 1].power : 0.0;
        const double right = best + 1 < n ? tones_[best + 1].power : 0.0;
        const bool present = bestP > 1e-12 && bestP > kPeakToMedian * median &&
                             bestP > kPeakToNeighbour * std::max(left, right) &&
                             bestP > kMinScore * bandPower_ * tones_[best].noiseGain;
        if (present && static_cast<int>(best) == candidate_) {
            absentRun_ = 0;
            if (runLength_ < 1000000) { ++runLength_; }
        } else if (present) {
            candidate_ = static_cast<int>(best);
            runLength_ = 1;
            absentRun_ = 0;
        } else {
            runLength_ = 0;
            if (absentRun_ < 1000000) { ++absentRun_; }
        }
        if (runLength_ >= kLockHops) { locked_ = candidate_; }
        if (locked_ >= 0 && absentRun_ >= kReleaseHops) { locked_ = -1; }
        // A different tone taking over the peak ends the old lock at once (it will lock anew
        // after kLockHops): the locked tone is, by definition, the one that dominates.
        if (locked_ >= 0 && present && static_cast<int>(best) != locked_ && runLength_ >= kLockHops) {
            locked_ = candidate_;
        }
        lockedPub_.store(locked_, std::memory_order_relaxed);
        const int sel = selected_.load(std::memory_order_relaxed);
        gatePub_.store(locked_ >= 0 && (sel < 0 || locked_ == sel), std::memory_order_relaxed);
    }

    bool configured_ = false;
    double rate_ = 0, fs1_ = 0, fs2_ = 0, dcR_ = 0, bandAlpha_ = 0;
    int d1_ = 1, d2_ = 1, hopLen2_ = 1;
    std::array<Bq, 2> lp_{};
    double accum1_ = 0, dcX_ = 0, dcY_ = 0, bandPower_ = 0;
    int count1_ = 0, count2_ = 0, hopCount_ = 0, renorm_ = 0;
    std::vector<Tone> tones_;
    std::vector<double> scratch_;
    int candidate_ = -1, runLength_ = 0, absentRun_ = 0, locked_ = -1;
    std::atomic<int> selected_{-1};
    std::atomic<int> lockedPub_{-1};
    std::atomic<bool> gatePub_{false};
};

}  // namespace cascade::dsp
