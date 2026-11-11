// trace_average.hpp - the spectrum trace's AVERAGING CHOICE, its SMOOTHING and its DE-COMB: three blocks that take FFT frames (dB per bin,
// fftshifted as SpectrumFrame::dbBins is) and produce the trace that is drawn.
//
// WHY: core/trace_hold.hpp has one average, a sliding window of the POWER (so "RMS"). Spectrum analysers offer more, and they read differently:
// a carrier sits at the same height in every average, the NOISE FLOOR does not. This file adds the others. Nothing here is wired into the
// engine or the window (wiring hand-off: docs/feature-records/dspmodels.md).
//
// AVERAGING (TraceAverager). A frame's bin of x dB is a = 10^(x/20) (amplitude) and p = a^2 (power). Over the last N frames:
//   Linear       mean of the AMPLITUDES a, shown as 20 log10(mean a).  A steady carrier is exact; noise reads LOW (see below).
//   Rms          square root of the mean of the POWERS, i.e. 10 log10(mean p). The same as TraceHold's average. Noise reads true.
//   Log          mean of the dB values themselves. Steadiest-looking, the lowest-reading noise.
//   Exponential  p <- p + alpha (p_frame - p) with alpha = 1 - exp(-dt / tau), dt = time since the last frame, tau in seconds, in POWER.
//                Frame-rate independent: the same tau gives the same fade on a fast pipeline and a slow one. The first frame seeds it.
// What each reads on pure noise (power exponentially distributed, which is what one FFT bin of white Gaussian noise is), relative to the
// true mean power: Rms 0 dB, Linear -1.05 dB (the mean of a Rayleigh amplitude is sqrt(pi)/2 of its RMS), Log -2.51 dB (Euler's constant:
// 10 log10(exp(-0.5772))). The tests pin all three numbers. A window of N frames is a TRUE sliding window (every frame in it counts equally).
// Memory is bounded: at most kAvgMaxFrames frames, and never more than kAvgMaxCells stored values (bins x frames).
// A frame whose bin count or axis key differs starts the average again. A non-finite bin counts as kAvgFloorDb.
//
// SMOOTHING (smoothMovingAverage, SavitzkyGolay). Along the FREQUENCY axis, on one trace. Moving average: window of odd width, edges use the
// bins that exist. Savitzky-Golay: a least-squares polynomial of the stated ORDER fitted to each window of odd width and evaluated at its
// centre; the filter keeps peaks and widths that a moving average flattens. Coefficients are derived here by solving the normal equations (no
// table copied): the centre row for width 5 / order 2 is the textbook (-3, 12, 17, 12, -3)/35 and the tests pin it, with width 7 / order 2
// (-2, 3, 6, 7, 6, 3, -2)/21. A polynomial of degree <= order passes through unchanged, which the tests pin too. At the two edges the fit of the
// first (last) window is evaluated at the edge bin, so the trace keeps its length and its ends.
// Reference for the method: A. Savitzky and M. J. E. Golay, Analytical Chemistry 36(8), 1964, 1627-1639 (read as a description of the idea
// only; the code is written from the least-squares definition).
//
// DE-COMB (DeComb). OUR OWN DEFINITION - no opened source defines "de-comb" for an FFT trace, so none is claimed. The artefact it removes is
// a regular row of narrow spurs, the same height at every multiple of P bins (an ADC's interleave spur, a leaking clock, a bin pattern left by a
// block-wise decimator), plus the one spike the DC offset leaves in the middle bin. The method:
//   1. BASELINE: a running median of 33 bins (narrow spurs cannot move a median that wide; the noise floor's slope can).
//   2. RESIDUAL r = trace - baseline.
//   3. PERIOD: the one asked for, or found by AUTO-correlation of max(r, 0) over lags 4 .. n/4; the SMALLEST lag whose normalised correlation
//      is within 80 % of the best is taken (so a comb of period P is not reported as 2P), and only when it is at least kMinCorr (0.25).
//   4. PROFILE: for each phase 0 .. P-1 the MEDIAN of r over the n/P periods (a signal that is present in only a few periods cannot enter a
//      median of many), kept only where it exceeds minSpurDb (default 1.5 dB) - negative and small values are noise and are not "corrected" in.
//   5. SUBTRACT strength x profile[i mod P] from the trace (strength 0..1, default 1). Fewer than 3 periods in the trace: bypass.
//   6. DC SPIKE (optional, dcBins = 0 .. 9 on each side): the middle bin n/2 and dcBins either side are replaced by the straight line between
//      the bins just outside them.
// A trace with no comb (no period found) passes through BIT-EXACT. profileMemory (0..0.99) averages the profile over frames at one period
// (a comb is steady, the noise under it is not); changing the period or bin count forgets it.
//
// All three: no allocation after the first frame of a given size (DeComb and TraceAverager allocate when the bin count changes, on the caller's
// thread; process() on an unchanged size does not). Single-threaded objects: one thread calls them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cascade::dsp {

inline constexpr float kAvgFloorDb = -200.0f;
inline constexpr std::size_t kAvgMaxFrames = 128;
inline constexpr std::size_t kAvgMaxCells = 8u * 1024u * 1024u;

enum class AvgMode : int { Off = 0, Linear = 1, Rms = 2, Log = 3, Exponential = 4 };

inline const char* avgModeName(AvgMode m) {
    switch (m) {
        case AvgMode::Linear: return "linear";
        case AvgMode::Rms: return "rms";
        case AvgMode::Log: return "log";
        case AvgMode::Exponential: return "exponential";
        default: return "off";
    }
}
inline AvgMode avgModeFromName(const char* s) {
    if (s == nullptr) { return AvgMode::Off; }
    const std::string_view v(s);
    if (v == "linear") { return AvgMode::Linear; }
    if (v == "rms") { return AvgMode::Rms; }
    if (v == "log") { return AvgMode::Log; }
    if (v == "exponential") { return AvgMode::Exponential; }
    return AvgMode::Off;
}

class TraceAverager {
public:
    static constexpr double kMinTauS = 0.005;
    static constexpr double kMaxTauS = 60.0;

    AvgMode mode() const { return mode_; }
    std::size_t frameCount() const { return count_; }
    double timeConstantS() const { return tauS_; }

    void setMode(AvgMode m) {
        if (m == mode_) { return; }
        mode_ = m;
        reset();
    }
    // Frames in the sliding window of Linear, Rms and Log (1 .. kAvgMaxFrames; NaN keeps the old value).
    void setFrameCount(std::size_t n) {
        const std::size_t c = std::min<std::size_t>(std::max<std::size_t>(n, 1), kAvgMaxFrames);
        if (c == count_) { return; }
        count_ = c;
        reset();
    }
    void setTimeConstantS(double s) {
        if (!std::isfinite(s)) { return; }
        tauS_ = std::min(std::max(s, kMinTauS), kMaxTauS);
    }

    void reset() {
        ring_.clear();
        sum_.clear();
        filled_ = 0;
        head_ = 0;
        pushes_ = 0;
        expo_.clear();
        trace_.clear();
        haveExpo_ = false;
    }

    // One frame of `n` dB values at time `nowS` (the caller's monotonic seconds) on the axis `key`.
    void push(const float* db, int n, double nowS, std::uint64_t key) {
        if (mode_ == AvgMode::Off || db == nullptr || n <= 0) { return; }
        const std::size_t nb = static_cast<std::size_t>(n);
        if (key != key_ || nb != n_) {
            reset();
            key_ = key;
            n_ = nb;
        }
        if (mode_ == AvgMode::Exponential) {
            pushExpo(db, nb, nowS);
            return;
        }
        // Window of N frames, memory-bounded: the frames kept shrink when bins x frames would pass kAvgMaxCells.
        const std::size_t cap = std::max<std::size_t>(1, std::min(count_, kAvgMaxCells / nb));
        if (ring_.size() != cap * nb) {
            ring_.assign(cap * nb, 0.0f);
            sum_.assign(nb, 0.0);
            filled_ = 0;
            head_ = 0;
            pushes_ = 0;
        }
        float* slot = ring_.data() + head_ * nb;
        if (filled_ == cap) {  // the oldest leaves the sum exactly as it entered it
            for (std::size_t i = 0; i < nb; ++i) { sum_[i] -= static_cast<double>(slot[i]); }
        }
        for (std::size_t i = 0; i < nb; ++i) {
            const float v = std::isfinite(db[i]) ? db[i] : kAvgFloorDb;
            const float t = forward(v);
            slot[i] = t;
            sum_[i] += static_cast<double>(t);
        }
        head_ = (head_ + 1) % cap;
        if (filled_ < cap) { ++filled_; }
        if (++pushes_ >= 512) { rebuildSum(nb); }  // the running sum never drifts
        trace_.resize(nb);
        const double inv = 1.0 / static_cast<double>(filled_);
        for (std::size_t i = 0; i < nb; ++i) { trace_[i] = back(sum_[i] * inv); }
    }

    // The averaged trace in dB; empty before the first frame or with the averager Off.
    const std::vector<float>& trace() const { return trace_; }
    // Frames that are in it now.
    std::size_t frames() const { return mode_ == AvgMode::Exponential ? (haveExpo_ ? 1 : 0) : filled_; }

private:
    // dB -> the quantity that is averaged, and the mean of it -> dB.
    float forward(float db) const {
        switch (mode_) {
            case AvgMode::Linear: return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
            case AvgMode::Rms: return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 10.0));
            default: return db;
        }
    }
    float back(double mean) const {
        switch (mode_) {
            case AvgMode::Linear: return static_cast<float>(20.0 * std::log10(std::max(mean, 1e-30)));
            case AvgMode::Rms: return static_cast<float>(10.0 * std::log10(std::max(mean, 1e-30)));
            default: return static_cast<float>(mean);
        }
    }
    void rebuildSum(std::size_t nb) {
        std::fill(sum_.begin(), sum_.end(), 0.0);
        for (std::size_t f = 0; f < filled_; ++f) {
            for (std::size_t i = 0; i < nb; ++i) { sum_[i] += static_cast<double>(ring_[f * nb + i]); }
        }
        pushes_ = 0;
    }
    void pushExpo(const float* db, std::size_t nb, double nowS) {
        double alpha = 1.0;
        if (haveExpo_) {
            double dt = std::isfinite(nowS) ? nowS - lastT_ : 0.0;
            if (!(dt > 0.0)) { dt = 0.0; }  // time standing still or going back: the frame is not counted
            alpha = 1.0 - std::exp(-dt / tauS_);
        }
        if (expo_.size() != nb) { expo_.assign(nb, 0.0); }
        for (std::size_t i = 0; i < nb; ++i) {
            const double v = std::isfinite(db[i]) ? static_cast<double>(db[i]) : static_cast<double>(kAvgFloorDb);
            const double p = std::pow(10.0, v / 10.0);
            expo_[i] = haveExpo_ ? expo_[i] + alpha * (p - expo_[i]) : p;
        }
        haveExpo_ = true;
        if (std::isfinite(nowS)) { lastT_ = nowS; }
        trace_.resize(nb);
        for (std::size_t i = 0; i < nb; ++i) { trace_[i] = static_cast<float>(10.0 * std::log10(std::max(expo_[i], 1e-30))); }
    }

    AvgMode mode_ = AvgMode::Off;
    std::size_t count_ = 8;
    double tauS_ = 0.5;
    std::uint64_t key_ = 0;
    std::size_t n_ = 0;
    std::vector<float> ring_;
    std::vector<double> sum_;
    std::size_t filled_ = 0, head_ = 0, pushes_ = 0;
    std::vector<double> expo_;
    bool haveExpo_ = false;
    double lastT_ = 0.0;
    std::vector<float> trace_;
};

// ---------------------------------------------------------------- smoothing

// Moving average of odd `width` (an even width is raised by one) along the trace; edges average the bins that exist. In dB, or - with
// `inPower` - over the powers. out may be the same array as in. Non-finite bins count as kAvgFloorDb. width <= 1: a copy.
inline void smoothMovingAverage(const float* in, float* out, std::size_t n, std::size_t width, bool inPower = false) {
    if (n == 0 || in == nullptr || out == nullptr) { return; }
    std::vector<double> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double d = std::isfinite(in[i]) ? static_cast<double>(in[i]) : static_cast<double>(kAvgFloorDb);
        v[i] = inPower ? std::pow(10.0, d / 10.0) : d;
    }
    if (width <= 1) {
        for (std::size_t i = 0; i < n; ++i) { out[i] = static_cast<float>(inPower ? 10.0 * std::log10(std::max(v[i], 1e-30)) : v[i]); }
        return;
    }
    const std::size_t half = width / 2;
    std::vector<double> cum(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) { cum[i + 1] = cum[i] + v[i]; }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t lo = i >= half ? i - half : 0;
        const std::size_t hi = std::min(n, i + half + 1);
        const double mean = (cum[hi] - cum[lo]) / static_cast<double>(hi - lo);
        out[i] = static_cast<float>(inPower ? 10.0 * std::log10(std::max(mean, 1e-30)) : mean);
    }
}

class SavitzkyGolay {
public:
    static constexpr std::size_t kMaxWindow = 101;
    static constexpr int kMaxOrder = 8;

    // window: odd, 3 .. 101 (an even value is raised by one); order: 0 .. 8 and < window. A bad pair is repaired (order clamped to window - 1).
    SavitzkyGolay(std::size_t window = 9, int order = 2) { configure(window, order); }

    void configure(std::size_t window, int order) {
        window = std::min(std::max<std::size_t>(window, 3), kMaxWindow);
        if (window % 2 == 0) { ++window; }
        order = std::min(std::max(order, 0), kMaxOrder);
        order = std::min<int>(order, static_cast<int>(window) - 1);
        window_ = window;
        order_ = order;
        build();
    }
    std::size_t window() const { return window_; }
    int order() const { return order_; }
    // The weights that give the fitted value at window position `pos` (0 .. window-1; the centre is window/2).
    const std::vector<double>& row(std::size_t pos) const { return rows_[std::min(pos, window_ - 1)]; }
    const std::vector<double>& centreRow() const { return rows_[window_ / 2]; }

    // out may be the same array as in. n smaller than the window: the window shrinks to the largest odd length that fits (order clamped);
    // n <= order + 1 or n < 3: a copy.
    void apply(const float* in, float* out, std::size_t n) const {
        if (n == 0 || in == nullptr || out == nullptr) { return; }
        std::vector<double> v(n);
        for (std::size_t i = 0; i < n; ++i) { v[i] = std::isfinite(in[i]) ? static_cast<double>(in[i]) : static_cast<double>(kAvgFloorDb); }
        if (n < window_) {
            const std::size_t w = (n % 2 == 1) ? n : n - 1;
            if (w < 3) {
                for (std::size_t i = 0; i < n; ++i) { out[i] = static_cast<float>(v[i]); }
                return;
            }
            const SavitzkyGolay small(w, order_);  // (the constructor clamps the order to w - 1)
            small.apply(in, out, n);
            return;
        }
        const std::size_t m = window_ / 2;
        std::vector<double> o(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t start;
            std::size_t pos;
            if (i < m) { start = 0; pos = i; }
            else if (i + m >= n) { start = n - window_; pos = i - start; }
            else { start = i - m; pos = m; }
            const std::vector<double>& r = rows_[pos];
            double acc = 0.0;
            for (std::size_t j = 0; j < window_; ++j) { acc += r[j] * v[start + j]; }
            o[i] = acc;
        }
        for (std::size_t i = 0; i < n; ++i) { out[i] = static_cast<float>(o[i]); }
    }

private:
    // Weights h_pos[j] = e(pos)^T (A^T A)^-1 A^T, A[j][k] = x_j^k with x_j = (j - m) / m (scaled to [-1, 1] so the normal equations stay
    // well conditioned), e(pos)[k] = x_pos^k.
    void build() {
        const std::size_t w = window_;
        const std::size_t p = static_cast<std::size_t>(order_) + 1;
        const double m = static_cast<double>(w / 2);
        std::vector<double> x(w);
        for (std::size_t j = 0; j < w; ++j) { x[j] = (static_cast<double>(j) - m) / m; }
        // M = A^T A (p x p), inverted by Gauss-Jordan with partial pivoting.
        std::vector<double> M(p * p, 0.0), inv(p * p, 0.0);
        for (std::size_t a = 0; a < p; ++a) {
            for (std::size_t b = 0; b < p; ++b) {
                double s = 0.0;
                for (std::size_t j = 0; j < w; ++j) { s += std::pow(x[j], static_cast<double>(a + b)); }
                M[a * p + b] = s;
            }
            inv[a * p + a] = 1.0;
        }
        for (std::size_t c = 0; c < p; ++c) {
            std::size_t piv = c;
            for (std::size_t r = c + 1; r < p; ++r) {
                if (std::fabs(M[r * p + c]) > std::fabs(M[piv * p + c])) { piv = r; }
            }
            if (piv != c) {
                for (std::size_t k = 0; k < p; ++k) {
                    std::swap(M[c * p + k], M[piv * p + k]);
                    std::swap(inv[c * p + k], inv[piv * p + k]);
                }
            }
            const double d = M[c * p + c];
            for (std::size_t k = 0; k < p; ++k) {
                M[c * p + k] /= d;
                inv[c * p + k] /= d;
            }
            for (std::size_t r = 0; r < p; ++r) {
                if (r == c) { continue; }
                const double f = M[r * p + c];
                if (f == 0.0) { continue; }
                for (std::size_t k = 0; k < p; ++k) {
                    M[r * p + k] -= f * M[c * p + k];
                    inv[r * p + k] -= f * inv[c * p + k];
                }
            }
        }
        // B = inv * A^T (p x w)
        std::vector<double> B(p * w, 0.0);
        for (std::size_t a = 0; a < p; ++a) {
            for (std::size_t j = 0; j < w; ++j) {
                double s = 0.0;
                for (std::size_t b = 0; b < p; ++b) { s += inv[a * p + b] * std::pow(x[j], static_cast<double>(b)); }
                B[a * w + j] = s;
            }
        }
        rows_.assign(w, std::vector<double>(w, 0.0));
        for (std::size_t pos = 0; pos < w; ++pos) {
            for (std::size_t j = 0; j < w; ++j) {
                double s = 0.0;
                for (std::size_t a = 0; a < p; ++a) { s += std::pow(x[pos], static_cast<double>(a)) * B[a * w + j]; }
                rows_[pos][j] = s;
            }
        }
    }

    std::size_t window_ = 9;
    int order_ = 2;
    std::vector<std::vector<double>> rows_;
};

// ------------------------------------------------------------------ de-comb

struct DeCombSettings {
    std::size_t period = 0;       // bins between spurs; 0 = find it
    double strength = 1.0;        // 0 .. 1 of the profile that is subtracted
    double minSpurDb = 1.5;       // spurs smaller than this above the baseline are left alone
    double minCorr = 0.25;        // the auto-correlation a period needs to count as found
    int dcBins = 0;               // 0 = leave the middle bin; else repair it and this many bins each side (max 9)
    double profileMemory = 0.0;   // 0 = each frame stands alone; 0.9 = the profile is a long average
};

class DeComb {
public:
    static constexpr std::size_t kBaselineWidth = 33;
    static constexpr std::size_t kMinPeriod = 4;

    void setSettings(const DeCombSettings& s) {
        s_ = s;
        s_.strength = std::isfinite(s_.strength) ? std::min(std::max(s_.strength, 0.0), 1.0) : 1.0;
        s_.minSpurDb = std::isfinite(s_.minSpurDb) ? std::max(s_.minSpurDb, 0.0) : 1.5;
        s_.minCorr = std::isfinite(s_.minCorr) ? std::min(std::max(s_.minCorr, 0.0), 1.0) : 0.25;
        s_.dcBins = std::min(std::max(s_.dcBins, 0), 9);
        s_.profileMemory = std::isfinite(s_.profileMemory) ? std::min(std::max(s_.profileMemory, 0.0), 0.99) : 0.0;
    }
    const DeCombSettings& settings() const { return s_; }
    void reset() { profile_.clear(); lastPeriod_ = 0; prevPeriod_ = 0; }

    // The period used by the last process() call (0 = none found, nothing subtracted).
    std::size_t lastPeriod() const { return lastPeriod_; }
    // Strength of the strongest spur removed by the last call, in dB (0 = none).
    double lastWorstSpurDb() const { return lastWorst_; }

    // out may be the same array as in. n < 3 x the smallest period: a copy.
    void process(const float* in, float* out, std::size_t n) {
        lastWorst_ = 0.0;
        if (n == 0 || in == nullptr || out == nullptr) { return; }
        x_.resize(n);
        for (std::size_t i = 0; i < n; ++i) { x_[i] = std::isfinite(in[i]) ? in[i] : kAvgFloorDb; }
        const bool combPossible = n >= 3 * kMinPeriod && n >= 64;
        std::size_t period = 0;
        if (combPossible) {
            baseline(n);
            period = s_.period != 0 ? s_.period : findPeriod(n);
            if (period < kMinPeriod || period * 3 > n) { period = 0; }
        }
        lastPeriod_ = period;
        if (period == 0) {
            profile_.clear();
        } else {
            buildProfile(n, period);
        }
        // Output = input minus the profile (when there is one).
        for (std::size_t i = 0; i < n; ++i) {
            float v = x_[i];
            if (period != 0 && s_.strength > 0.0) { v = static_cast<float>(static_cast<double>(v) - s_.strength * profile_[i % period]); }
            out[i] = v;
        }
        prevPeriod_ = period;
        if (s_.dcBins > 0 && n >= 2 * static_cast<std::size_t>(s_.dcBins) + 5) { repairDc(out, n); }
    }

private:
    void baseline(std::size_t n) {
        base_.resize(n);
        resid_.resize(n);
        const std::size_t half = kBaselineWidth / 2;
        tmp_.resize(kBaselineWidth);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t lo = i >= half ? i - half : 0;
            const std::size_t hi = std::min(n, i + half + 1);
            const std::size_t cnt = hi - lo;
            for (std::size_t k = 0; k < cnt; ++k) { tmp_[k] = x_[lo + k]; }
            std::nth_element(tmp_.begin(), tmp_.begin() + static_cast<std::ptrdiff_t>(cnt / 2), tmp_.begin() + static_cast<std::ptrdiff_t>(cnt));
            base_[i] = tmp_[cnt / 2];
            resid_[i] = x_[i] - base_[i];
        }
    }

    std::size_t findPeriod(std::size_t n) {
        pos_.resize(n);
        double mean = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            pos_[i] = std::max(resid_[i], 0.0f);
            mean += pos_[i];
        }
        mean /= static_cast<double>(n);
        double var = 0.0;
        for (std::size_t i = 0; i < n; ++i) { var += (pos_[i] - mean) * (pos_[i] - mean); }
        if (!(var > 1e-9)) { return 0; }
        const std::size_t maxLag = n / 4;
        corr_.assign(maxLag + 1, 0.0);
        double best = 0.0;
        for (std::size_t lag = kMinPeriod; lag <= maxLag; ++lag) {
            double c = 0.0;
            for (std::size_t i = 0; i + lag < n; ++i) { c += (pos_[i] - mean) * (pos_[i + lag] - mean); }
            c /= var;
            corr_[lag] = c;
            best = std::max(best, c);
        }
        if (best < s_.minCorr) { return 0; }
        for (std::size_t lag = kMinPeriod; lag <= maxLag; ++lag) {
            // the smallest lag that is a local peak within 80 % of the best
            const bool peak = corr_[lag] >= 0.8 * best && corr_[lag] >= corr_[lag - 1] && (lag == maxLag || corr_[lag] >= corr_[lag + 1]);
            if (peak) { return lag; }
        }
        return 0;
    }

    void buildProfile(std::size_t n, std::size_t period) {
        if (resid_.size() != n) { baseline(n); }
        std::vector<double>& fresh = fresh_;
        fresh.assign(period, 0.0);
        const std::size_t periods = n / period;  // whole periods only (the tail would give some phases one sample more)
        col_.resize(periods);
        for (std::size_t ph = 0; ph < period; ++ph) {
            for (std::size_t k = 0; k < periods; ++k) { col_[k] = resid_[ph + k * period]; }
            std::nth_element(col_.begin(), col_.begin() + static_cast<std::ptrdiff_t>(periods / 2), col_.end());
            double med = col_[periods / 2];
            if (periods % 2 == 0) {
                const float lower = *std::max_element(col_.begin(), col_.begin() + static_cast<std::ptrdiff_t>(periods / 2));
                med = 0.5 * (med + lower);
            }
            fresh[ph] = med >= s_.minSpurDb ? med : 0.0;
        }
        if (profile_.size() == period && prevPeriod_ == period) {
            const double m = s_.profileMemory;
            for (std::size_t ph = 0; ph < period; ++ph) { profile_[ph] = m * profile_[ph] + (1.0 - m) * fresh[ph]; }
        } else {
            profile_ = fresh;
        }
        for (double v : profile_) { lastWorst_ = std::max(lastWorst_, v); }
    }

    void repairDc(float* out, std::size_t n) const {
        const std::size_t c = n / 2;
        const std::size_t k = static_cast<std::size_t>(s_.dcBins);
        const std::size_t lo = c - k - 1, hi = c + k + 1;
        const float a = out[lo], b = out[hi];
        for (std::size_t i = lo + 1; i < hi; ++i) {
            const float t = static_cast<float>(i - lo) / static_cast<float>(hi - lo);
            out[i] = a + t * (b - a);
        }
    }

    DeCombSettings s_{};
    std::size_t lastPeriod_ = 0;
    std::size_t prevPeriod_ = 0;
    double lastWorst_ = 0.0;
    std::vector<float> x_, base_, resid_, pos_, tmp_, col_;
    std::vector<double> corr_;
    std::vector<double> profile_, fresh_;
};

}  // namespace cascade::dsp
