// sigmeas_common.hpp - the shared parts of the signal-measurement blocks (lane sigmeas).
//
// WHAT THE SIGMEAS BLOCKS ARE. Reusable, HOST-SIDE measurement functions on a SELECTED
// time-and-frequency box of I/Q or audio: the caller crops the span it wants measured (and,
// for a signal that is not at DC, says where it is) and gets a result struct back with a
// validity flag. Pure functions of (samples, rate, parameters): no clock, no file, no host,
// no network, nothing written to stdout or stderr, nothing kept after the call. They are for
// a host panel, the web page and the analyser project; the Demod Analyzer plugin keeps its
// own measurements (a plugin cannot link host code, and the host cannot link a plugin's), so
// the definitions here are written out in each header.
//
// CONVENTIONS, the same in every block
//  - Complex samples are std::complex<float>; every internal sum is double.
//  - A frequency is Hz from the CENTRE of the span (the baseband frequency, -rate/2 .. +rate/2).
//  - A result has `valid` and, when it is false, a `reason` that says why in one word-group
//    (the tests pin the reasons). A NaN or infinity anywhere in the input is `non-finite
//    input`; an empty span, a rate that is not a positive finite number and a span too short
//    for the measurement each have a reason of their own. No block throws or asserts on bad
//    input.
//  - Nothing is allocated inside a per-sample loop: buffers are sized once before it.
//  - A SAMPLE-RATE ERROR is not corrected here (a block cannot know it); every frequency or
//    rate it returns is in the units of the rate it was given. A true sample rate that is
//    (1+e) times the given one scales every reported frequency by (1+e) and every reported
//    period by 1/(1+e): the tests pin that as a property.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_COMMON_HPP
#define CASCADE_DSP_SIGMEAS_COMMON_HPP

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cascade::sigmeas {

using Cf = std::complex<float>;

inline constexpr double kPi = 3.14159265358979323846264338327950288;
inline constexpr double kTwoPi = 2.0 * kPi;
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Why a measurement was refused. One short phrase per case; tests compare these exactly.
namespace why {
inline constexpr const char* kOk = "";
inline constexpr const char* kEmpty = "empty span";
inline constexpr const char* kBadRate = "bad sample rate";
inline constexpr const char* kNonFinite = "non-finite input";
inline constexpr const char* kTooShort = "span too short";
inline constexpr const char* kNoSignal = "no signal found";
inline constexpr const char* kBadParam = "bad parameter";
inline constexpr const char* kNoStructure = "no on/off structure";
}  // namespace why

inline bool rateOk(double rate) { return std::isfinite(rate) && rate > 0.0; }

inline bool allFinite(const Cf* x, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i].real()) || !std::isfinite(x[i].imag())) { return false; }
    }
    return true;
}
inline bool allFinite(const float* x, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) { return false; }
    }
    return true;
}
inline bool allFinite(const double* x, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) { return false; }
    }
    return true;
}

// Checks shared by every I/Q measurement. Returns nullptr when the span can be measured.
inline const char* checkIq(const Cf* x, std::size_t n, double rate, std::size_t minSamples) {
    if (x == nullptr || n == 0) { return why::kEmpty; }
    if (!rateOk(rate)) { return why::kBadRate; }
    if (n < minSamples) { return why::kTooShort; }
    if (!allFinite(x, n)) { return why::kNonFinite; }
    return nullptr;
}

inline double toDb(double power) { return 10.0 * std::log10(std::max(power, 1e-30)); }
inline double fromDb(double db) { return std::pow(10.0, db / 10.0); }

inline bool isPow2(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }
inline std::size_t floorPow2(std::size_t n) {
    std::size_t p = 1;
    while (p * 2 <= n) { p *= 2; }
    return p;
}

// ---- FFT (iterative radix-2, double, in place) -----------------------------------------------
// Own, so the blocks need nothing but this header. N must be a power of two (isPow2).
class Fft {
public:
    explicit Fft(std::size_t n) : n_(n), tw_(n / 2), rev_(n) {
        for (std::size_t i = 0; i < n / 2; ++i) {
            const double a = -kTwoPi * static_cast<double>(i) / static_cast<double>(n);
            tw_[i] = {std::cos(a), std::sin(a)};
        }
        std::size_t bits = 0;
        while ((std::size_t{1} << bits) < n) { ++bits; }
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t r = 0;
            for (std::size_t b = 0; b < bits; ++b) {
                if (i & (std::size_t{1} << b)) { r |= std::size_t{1} << (bits - 1 - b); }
            }
            rev_[i] = r;
        }
    }
    std::size_t size() const { return n_; }

    // Forward DFT: X[k] = sum x[n] exp(-2 pi i k n / N), unscaled.
    void forward(std::complex<double>* a) const {
        for (std::size_t i = 0; i < n_; ++i) {
            if (i < rev_[i]) { std::swap(a[i], a[rev_[i]]); }
        }
        for (std::size_t len = 2; len <= n_; len <<= 1) {
            const std::size_t half = len / 2;
            const std::size_t step = n_ / len;
            for (std::size_t i = 0; i < n_; i += len) {
                for (std::size_t j = 0; j < half; ++j) {
                    const std::complex<double> w = tw_[j * step];
                    const std::complex<double> u = a[i + j];
                    const std::complex<double> v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

private:
    std::size_t n_;
    std::vector<std::complex<double>> tw_;
    std::vector<std::size_t> rev_;
};

// ---- windows ----------------------------------------------------------------------------------
enum class Win { Rect, Hann, BlackmanHarris4 };

inline std::vector<double> makeWindow(Win type, std::size_t n) {
    std::vector<double> w(n, 1.0);
    if (n < 2) { return w; }
    const double N = static_cast<double>(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = kTwoPi * static_cast<double>(i) / N;  // periodic form, for spectra
        switch (type) {
            case Win::Rect: w[i] = 1.0; break;
            case Win::Hann: w[i] = 0.5 - 0.5 * std::cos(x); break;
            case Win::BlackmanHarris4:
                w[i] = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) -
                       0.01168 * std::cos(3 * x);
                break;
        }
    }
    return w;
}

// ---- order statistics -------------------------------------------------------------------------
// q in [0,1]. Partially sorts v. NaN if v is empty.
inline double percentileInPlace(std::vector<double>& v, double q) {
    if (v.empty()) { return kNaN; }
    q = std::min(1.0, std::max(0.0, q));
    const double pos = q * static_cast<double>(v.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(v.size() - 1, lo + 1);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(lo), v.end());
    const double a = v[lo];
    double b = a;
    if (hi != lo) { b = *std::min_element(v.begin() + static_cast<std::ptrdiff_t>(hi), v.end()); }
    return a + (b - a) * (pos - static_cast<double>(lo));
}

inline double meanOf(const std::vector<double>& v) {
    if (v.empty()) { return 0.0; }
    double s = 0.0;
    for (double d : v) { s += d; }
    return s / static_cast<double>(v.size());
}

// Otsu's two-class threshold over `v` (maximises the between-class variance of a 128-bin
// histogram). Returns the threshold; `separation` (0..1) is between-class variance over the
// total variance - near 1 for two clean levels, near 0 for one level plus noise.
inline double otsuThreshold(const double* v, std::size_t n, double* separation = nullptr) {
    if (separation != nullptr) { *separation = 0.0; }
    if (n == 0) { return 0.0; }
    double lo = v[0], hi = v[0];
    for (std::size_t i = 1; i < n; ++i) {
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
    }
    if (!(hi > lo)) { return lo; }
    constexpr int kBins = 128;
    double hist[kBins] = {};
    const double scale = (kBins - 1) / (hi - lo);
    for (std::size_t i = 0; i < n; ++i) {
        hist[static_cast<int>((v[i] - lo) * scale)] += 1.0;
    }
    double total = 0.0, sumAll = 0.0;
    for (int b = 0; b < kBins; ++b) {
        total += hist[b];
        sumAll += hist[b] * b;
    }
    double wB = 0.0, sumB = 0.0, best = -1.0;
    int bestBin = 0;
    for (int b = 0; b < kBins; ++b) {
        wB += hist[b];
        if (wB <= 0.0) { continue; }
        const double wF = total - wB;
        if (wF <= 0.0) { break; }
        sumB += hist[b] * b;
        const double mB = sumB / wB;
        const double mF = (sumAll - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best) {
            best = between;
            bestBin = b;
        }
    }
    if (separation != nullptr) {
        double mean = sumAll / total, var = 0.0;
        for (int b = 0; b < kBins; ++b) { var += hist[b] * (b - mean) * (b - mean); }
        *separation = var > 0.0 ? std::min(1.0, best / (total * var)) : 0.0;
    }
    // Threshold between bin `bestBin` and the next: the upper edge of the lower class.
    return lo + (static_cast<double>(bestBin) + 1.0) / scale;
}

// ---- Welch power spectrum ---------------------------------------------------------------------
// Power per bin, in FFT-shifted order (bin 0 = -rate/2, bin nfft/2 = 0 Hz). Normalised so that
// the sum over all bins equals the mean power of the signal (Parseval): a bin is the power in
// one bin-width of spectrum, not a density.
struct Psd {
    bool valid = false;
    const char* reason = why::kOk;
    double rate = 0.0;
    std::size_t nfft = 0;
    std::size_t segments = 0;
    double binHz = 0.0;
    std::vector<double> p;  // linear power per bin, size nfft
    double freqOf(std::size_t bin) const {
        return (static_cast<double>(bin) - static_cast<double>(nfft) / 2.0) * binHz;
    }
};

// nfft = 0 picks a power of two giving at least ~8 segments (256 .. 16384). 50 % overlap.
inline Psd welchPsd(const Cf* x, std::size_t n, double rate, std::size_t nfft = 0,
                    Win win = Win::Hann) {
    Psd r;
    if (const char* bad = checkIq(x, n, rate, 64)) {
        r.reason = bad;
        return r;
    }
    if (nfft == 0) {
        nfft = std::min<std::size_t>(16384, std::max<std::size_t>(256, floorPow2(n / 8)));
        if (nfft > floorPow2(n)) { nfft = floorPow2(n); }
    }
    if (!isPow2(nfft) || nfft > n) {
        r.reason = nfft > n ? why::kTooShort : why::kBadParam;
        return r;
    }
    const std::vector<double> w = makeWindow(win, nfft);
    double sw2 = 0.0;
    for (double v : w) { sw2 += v * v; }
    const Fft fft(nfft);
    std::vector<std::complex<double>> buf(nfft);
    r.p.assign(nfft, 0.0);
    const std::size_t hop = nfft / 2;
    std::size_t segs = 0;
    for (std::size_t start = 0; start + nfft <= n; start += hop) {
        for (std::size_t i = 0; i < nfft; ++i) {
            buf[i] = {static_cast<double>(x[start + i].real()) * w[i],
                      static_cast<double>(x[start + i].imag()) * w[i]};
        }
        fft.forward(buf.data());
        for (std::size_t k = 0; k < nfft; ++k) {
            const std::size_t shifted = (k + nfft / 2) % nfft;
            r.p[shifted] += std::norm(buf[k]);
        }
        ++segs;
    }
    const double norm = 1.0 / (static_cast<double>(segs) * static_cast<double>(nfft) * sw2);
    for (double& v : r.p) { v *= norm; }
    r.valid = true;
    r.rate = rate;
    r.nfft = nfft;
    r.segments = segs;
    r.binHz = rate / static_cast<double>(nfft);
    return r;
}

// ---- small shared helpers ---------------------------------------------------------------------
// Instantaneous frequency in Hz from the phase step of consecutive samples,
// f[n] = arg(x[n] conj(x[n-1])) rate / 2 pi, with f[0] = f[1]. out has n entries.
inline void instantaneousFrequency(const Cf* x, std::size_t n, double rate, double* out) {
    if (n == 0) { return; }
    const double k = rate / kTwoPi;
    for (std::size_t i = 1; i < n; ++i) {
        const double re = static_cast<double>(x[i].real()) * x[i - 1].real() +
                          static_cast<double>(x[i].imag()) * x[i - 1].imag();
        const double im = static_cast<double>(x[i].imag()) * x[i - 1].real() -
                          static_cast<double>(x[i].real()) * x[i - 1].imag();
        out[i] = std::atan2(im, re) * k;
    }
    out[0] = n > 1 ? out[1] : 0.0;
}

// Centred moving average of odd length `len` (edges shrink the window). In place via scratch.
inline void movingAverage(std::vector<double>& v, std::size_t len, std::vector<double>& scratch) {
    if (len < 2 || v.size() < 2) { return; }
    const std::size_t n = v.size();
    const std::size_t half = len / 2;
    scratch.assign(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) { scratch[i + 1] = scratch[i] + v[i]; }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t a = i >= half ? i - half : 0;
        const std::size_t b = std::min(n, i + half + 1);
        v[i] = (scratch[b] - scratch[a]) / static_cast<double>(b - a);
    }
}

// Linear-interpolated time (in samples, fractional) at which the series crosses `level`
// between samples i-1 and i.
inline double crossingPos(double a, double b, double level, std::size_t i) {
    const double d = b - a;
    const double frac = d != 0.0 ? (level - a) / d : 0.5;
    return static_cast<double>(i) - 1.0 + std::min(1.0, std::max(0.0, frac));
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_COMMON_HPP
