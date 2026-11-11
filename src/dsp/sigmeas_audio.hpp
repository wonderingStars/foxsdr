// sigmeas_audio.hpp - AUDIO MEASUREMENTS (SINAD, THD, THD+N, SFDR, ENOB) and DRIFT INTEGRATION
// ("de-chirp"). Rows: DEPTH-GAPS.md Part B, "audio measurements (SINAD, THD, SFDR, ENOB), drift
// integration ('de-chirp') ... (Baudline)".
//
// DEFINITIONS (a single sine of frequency f0 and amplitude A is the test signal)
//  - Spectrum: a 4-term Blackman-Harris window (-92 dB sidelobes, main lobe +-4 bins) over a power
//    of two N <= the span, one-sided, normalised so that the bin powers add up to the mean square
//    of the signal. A component's power is the sum of its bins within +-4 bins of its peak.
//  - SINAD = P(tone) / P(everything else in the band except DC), in dB. "Everything else" is noise
//    plus harmonic and spurious distortion (IEEE Std 1241-2010 3.1 "signal-to-noise and distortion
//    ratio"; the band is `bandLowHz`..`bandHighHz`, default 20 Hz .. Nyquist).
//  - THD = sum of P(harmonic h) for h = 2 .. `maxHarmonic` over P(tone), in dB (IEC 60268-3 /
//    AES17: the ratio of the rms of the harmonics to the rms of the fundamental). Harmonics above
//    Nyquist are folded back (aliased) to where a sampled system really puts them. `thdPercent` is
//    the same as a percentage of amplitude.
//  - THD+N = P(everything but the tone) / P(tone) = -SINAD, reported as a negative dB and a
//    percentage, the figure audio analysers print.
//  - SFDR = P(tone) over the power of the largest spur outside the tone's lobe and DC, in dBc
//    (IEEE 1241 3.1 "spurious-free dynamic range"). A spur's power is the lobe sum around a local
//    maximum of the spectrum.
//  - ENOB = (SINAD - 1.76 + 20 log10(full scale / A)) / 6.02 (IEEE 1241 4.1 "effective number of
//    bits": the converter whose ideal quantisation noise would equal the measured noise and
//    distortion). `fullScaleAmplitude` (default 1.0) is the converter's full-scale peak value.
//
// ACCURACY, pinned in tests/test_sigmeas_audio.cpp on a synthetic tone with known distortion and a
// known white-noise floor: SINAD and THD within 0.3 dB, SFDR within 0.5 dB, ENOB of an ideal 10-, 12-
// and 13-bit quantiser within 0.1 bit. LIMIT: the window's own skirts sit about 90 dB under the tone,
// so a SINAD above roughly 82 dB reads low (an ideal 14-bit quantiser reads 13.87 bits, 85.2 dB).
//
// DRIFT INTEGRATION. A tone whose frequency drifts at `a` Hz per second smears across a number of FFT
// bins and loses its peak. `dechirp` multiplies the analytic signal by exp(-j pi a t^2), which
// removes exactly that drift; `searchDrift` tries a range of rates (a grid, then golden-section
// refinement) and returns the one that makes the strongest, sharpest line, the frequency that line
// has at the start of the span, and the gain over the un-corrected spectrum. `analyticFromReal`
// turns audio into the complex form (FFT, negative frequencies removed).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_AUDIO_HPP
#define CASCADE_DSP_SIGMEAS_AUDIO_HPP

#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct AudioParams {
    double toneHz = 0.0;              // 0 = the strongest line above 5 bins
    std::size_t maxHarmonic = 9;      // harmonics 2 .. maxHarmonic
    double bandLowHz = 20.0;
    double bandHighHz = 0.0;          // 0 = Nyquist
    std::size_t nfft = 0;             // 0 = the largest power of two <= span (at most 2^20)
    double fullScaleAmplitude = 1.0;  // for ENOB
};

struct AudioResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double toneHz = kNaN;
    double toneAmplitude = kNaN;      // peak amplitude of the tone
    double toneDbfs = kNaN;           // rms level re a full-scale sine of fullScaleAmplitude
    double sinadDb = kNaN;
    double thdDb = kNaN;
    double thdPercent = kNaN;
    double thdnDb = kNaN;
    double thdnPercent = kNaN;
    double sfdrDbc = kNaN;
    double spurHz = kNaN;
    double enob = kNaN;
    double noiseFloorDbfs = kNaN;     // everything but tone and harmonics, rms, dBFS
    std::vector<double> harmonicsDbc; // h = 2 .. maxHarmonic (-300 for one that falls outside)
};

namespace detail {

constexpr std::size_t kLobe = 4;  // Blackman-Harris 4-term: main lobe half-width in bins

// One-sided power per bin (sums to the mean square). size N/2+1.
inline std::vector<double> oneSidedPower(const float* x, std::size_t N, Win win) {
    const std::vector<double> w = makeWindow(win, N);
    double sw2 = 0.0;
    for (double v : w) { sw2 += v * v; }
    std::vector<std::complex<double>> buf(N);
    for (std::size_t i = 0; i < N; ++i) { buf[i] = {static_cast<double>(x[i]) * w[i], 0.0}; }
    Fft(N).forward(buf.data());
    std::vector<double> p(N / 2 + 1);
    const double norm = 1.0 / (static_cast<double>(N) * sw2);
    for (std::size_t k = 0; k <= N / 2; ++k) {
        p[k] = std::norm(buf[k]) * norm * ((k == 0 || k == N / 2) ? 1.0 : 2.0);
    }
    return p;
}

inline double lobeSum(const std::vector<double>& p, double centreBin) {
    const long c = std::lround(centreBin);
    double s = 0.0;
    for (long k = c - static_cast<long>(kLobe); k <= c + static_cast<long>(kLobe); ++k) {
        if (k >= 0 && k < static_cast<long>(p.size())) { s += p[static_cast<std::size_t>(k)]; }
    }
    return s;
}

}  // namespace detail

inline AudioResult measureAudio(const float* x, std::size_t n, double rate, const AudioParams& prm = {}) {
    AudioResult r;
    if (x == nullptr || n == 0) { r.reason = why::kEmpty; return r; }
    if (!rateOk(rate)) { r.reason = why::kBadRate; return r; }
    if (n < 1024) { r.reason = why::kTooShort; return r; }
    if (!allFinite(x, n)) { r.reason = why::kNonFinite; return r; }
    if (!(prm.toneHz >= 0.0) || !std::isfinite(prm.toneHz) || !(prm.bandLowHz >= 0.0) || !(prm.bandHighHz >= 0.0) ||
        !std::isfinite(prm.bandHighHz) || !(prm.fullScaleAmplitude > 0.0) || !std::isfinite(prm.fullScaleAmplitude) ||
        prm.maxHarmonic < 2 || prm.maxHarmonic > 64) {
        r.reason = why::kBadParam;
        return r;
    }
    std::size_t N = prm.nfft;
    if (N == 0) { N = std::min<std::size_t>(1u << 20, floorPow2(n)); }
    if (!isPow2(N) || N > n || N < 512) { r.reason = N > n ? why::kTooShort : why::kBadParam; return r; }
    const double binHz = rate / static_cast<double>(N);
    const double nyq = rate / 2.0;
    const double lo = prm.bandLowHz;
    const double hi = prm.bandHighHz > 0.0 ? std::min(prm.bandHighHz, nyq) : nyq;
    if (!(hi > lo)) { r.reason = why::kBadParam; return r; }

    const std::vector<double> p = detail::oneSidedPower(x, N, Win::BlackmanHarris4);
    const std::size_t half = N / 2;
    const std::size_t dcGuard = detail::kLobe + 1;

    // the tone
    double tonePos;  // in bins
    if (prm.toneHz > 0.0) {
        tonePos = prm.toneHz / binHz;
    } else {
        std::size_t pk = dcGuard + 1;
        for (std::size_t k = dcGuard + 1; k + 1 < half; ++k) { if (p[k] > p[pk]) { pk = k; } }
        const double a = std::log(std::max(p[pk - 1], 1e-300));
        const double b = std::log(std::max(p[pk], 1e-300));
        const double c = std::log(std::max(p[pk + 1], 1e-300));
        const double den = a - 2.0 * b + c;
        const double delta = den != 0.0 ? std::min(0.5, std::max(-0.5, 0.5 * (a - c) / den)) : 0.0;
        tonePos = static_cast<double>(pk) + delta;
        // a line must stand 20 dB over the median bin; DC alone, or nothing, is not a tone
        std::vector<double> sorted(p.begin() + static_cast<std::ptrdiff_t>(dcGuard + 1), p.end());
        const double med = percentileInPlace(sorted, 0.5);
        if (!(p[pk] >= 100.0 * med) || !(p[pk] > 0.0)) {
            r.reason = why::kNoSignal;
            return r;
        }
    }
    if (!(tonePos > static_cast<double>(dcGuard)) || tonePos >= static_cast<double>(half) - 1.0) {
        r.reason = why::kNoSignal;
        return r;
    }
    const double pTone = detail::lobeSum(p, tonePos);
    double pTotal = 0.0;
    for (double v : p) { pTotal += v; }
    // a "tone" more than 60 dB under the whole signal (a DC offset, say) is the window's skirt
    if (!(pTone > 0.0) || !(pTone >= 1e-6 * pTotal)) { r.reason = why::kNoSignal; return r; }
    r.toneHz = tonePos * binHz;
    r.toneAmplitude = std::sqrt(2.0 * pTone);

    // mask of bins that belong to the tone, to DC, or lie outside the band
    std::vector<char> inTone(half + 1, 0);
    {
        const long c = std::lround(tonePos);
        for (long k = c - static_cast<long>(detail::kLobe); k <= c + static_cast<long>(detail::kLobe); ++k) {
            if (k >= 0 && k <= static_cast<long>(half)) { inTone[static_cast<std::size_t>(k)] = 1; }
        }
    }
    const std::size_t kLo = std::max<std::size_t>(dcGuard, static_cast<std::size_t>(std::ceil(lo / binHz)));
    const std::size_t kHi = std::min<std::size_t>(half, static_cast<std::size_t>(std::floor(hi / binHz)));
    if (kHi <= kLo + 8) { r.reason = why::kBadParam; return r; }
    double pNd = 0.0;
    for (std::size_t k = kLo; k <= kHi; ++k) { if (!inTone[k]) { pNd += p[k]; } }

    // harmonics, folded to the first Nyquist zone
    std::vector<char> inHarm(half + 1, 0);
    double pHarm = 0.0;
    for (std::size_t h = 2; h <= prm.maxHarmonic; ++h) {
        double f = static_cast<double>(h) * r.toneHz;
        const double fs = rate;
        f = std::fabs(f - fs * std::round(f / fs));          // alias into 0 .. fs/2
        const double pos = f / binHz;
        double ph = 0.0;
        if (pos > static_cast<double>(dcGuard) && pos < static_cast<double>(half) - 1.0 &&
            f >= lo && f <= hi && std::fabs(pos - tonePos) > static_cast<double>(2 * detail::kLobe + 1)) {
            // only count bins not already taken by the tone or another harmonic
            const long c = std::lround(pos);
            for (long k = c - static_cast<long>(detail::kLobe); k <= c + static_cast<long>(detail::kLobe); ++k) {
                if (k >= 0 && k <= static_cast<long>(half) && !inTone[static_cast<std::size_t>(k)] &&
                    !inHarm[static_cast<std::size_t>(k)]) {
                    ph += p[static_cast<std::size_t>(k)];
                    inHarm[static_cast<std::size_t>(k)] = 1;
                }
            }
            pHarm += ph;
        }
        r.harmonicsDbc.push_back(ph > 0.0 ? toDb(ph / pTone) : -300.0);
    }

    // largest spur: a local maximum outside the tone lobe, power as its lobe sum (bins of the tone
    // lobe excluded from the sum)
    double pSpur = 0.0;
    std::size_t spurBin = 0;
    for (std::size_t k = kLo; k <= kHi; ++k) {
        if (inTone[k] || !(p[k] >= p[k - 1] && p[k] >= p[k + 1 < p.size() ? k + 1 : k])) { continue; }
        double s = 0.0;
        for (long j = static_cast<long>(k) - static_cast<long>(detail::kLobe); j <= static_cast<long>(k) + static_cast<long>(detail::kLobe); ++j) {
            if (j >= static_cast<long>(dcGuard) && j <= static_cast<long>(half) && !inTone[static_cast<std::size_t>(j)]) {
                s += p[static_cast<std::size_t>(j)];
            }
        }
        if (s > pSpur) { pSpur = s; spurBin = k; }
    }

    r.sinadDb = toDb(pTone / std::max(pNd, 1e-30));
    r.thdDb = pHarm > 0.0 ? toDb(pHarm / pTone) : -300.0;
    r.thdPercent = 100.0 * std::sqrt(pHarm / pTone);
    r.thdnDb = toDb(pNd / pTone);
    r.thdnPercent = 100.0 * std::sqrt(pNd / pTone);
    r.sfdrDbc = pSpur > 0.0 ? toDb(pTone / pSpur) : 300.0;
    r.spurHz = static_cast<double>(spurBin) * binHz;
    r.enob = (r.sinadDb - 1.76 + 20.0 * std::log10(prm.fullScaleAmplitude / r.toneAmplitude)) / 6.02;
    r.toneDbfs = toDb(pTone / (0.5 * prm.fullScaleAmplitude * prm.fullScaleAmplitude));
    r.noiseFloorDbfs = toDb(std::max(pNd - pHarm, 1e-30) / (0.5 * prm.fullScaleAmplitude * prm.fullScaleAmplitude));
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

// ---- drift integration ("de-chirp") -----------------------------------------------------------------
// Analytic signal of real audio (FFT, zero negative frequencies, doubled positive), n samples. The span
// is zero-padded to a power of two for the transform; the first and last few percent are the edges.
inline std::vector<Cf> analyticFromReal(const float* x, std::size_t n) {
    std::vector<Cf> out;
    if (x == nullptr || n < 8 || !allFinite(x, n)) { return out; }
    std::size_t N = 8;
    while (N < n) { N *= 2; }
    std::vector<std::complex<double>> buf(N, {0.0, 0.0});
    for (std::size_t i = 0; i < n; ++i) { buf[i] = {static_cast<double>(x[i]), 0.0}; }
    const Fft fft(N);
    fft.forward(buf.data());
    for (std::size_t k = 1; k < N / 2; ++k) { buf[k] *= 2.0; }
    for (std::size_t k = N / 2 + 1; k < N; ++k) { buf[k] = 0.0; }
    // inverse by conjugation
    for (auto& v : buf) { v = std::conj(v); }
    fft.forward(buf.data());
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::complex<double> v = std::conj(buf[i]) / static_cast<double>(N);
        out[i] = Cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
    return out;
}

// Removes a linear frequency drift of `driftHzPerS` (positive = rising): multiplies by
// exp(-j pi a t^2), t = n / rate. Returns false on bad input; writes n samples to out.
inline bool dechirp(const Cf* x, std::size_t n, double rate, double driftHzPerS, Cf* out) {
    if (x == nullptr || out == nullptr || n == 0 || !rateOk(rate) || !std::isfinite(driftHzPerS)) { return false; }
    // phase pi a t^2 by the recurrence phi[n+1] = phi[n] + pi a (2n+1) / rate^2 (no growth in the argument)
    const double k = kPi * driftHzPerS / (rate * rate);
    double ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double c = std::cos(ph), s = -std::sin(ph);
        out[i] = Cf(static_cast<float>(x[i].real() * c - x[i].imag() * s),
                    static_cast<float>(x[i].real() * s + x[i].imag() * c));
        ph += k * static_cast<double>(2 * i + 1);
        if (ph > kTwoPi || ph < -kTwoPi) { ph = std::fmod(ph, kTwoPi); }
    }
    return true;
}

struct DriftParams {
    double minDriftHzPerS = -50.0;
    double maxDriftHzPerS = 50.0;
    std::size_t gridSteps = 64;       // coarse candidates over the range (at least 8)
    std::size_t maxFft = 1u << 16;    // the span is cut to the largest power of two <= this
};

struct DriftResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double driftHzPerS = kNaN;
    double startFreqHz = kNaN;     // the corrected line, from the span centre, at the start of the span
    double peakDbfs = kNaN;        // its power
    double gainDb = kNaN;          // over the peak of the uncorrected spectrum
};

namespace detail {

// Power of the strongest line (sum of three bins) of the de-chirped span; also its frequency.
inline double linePower(const Cf* x, std::size_t N, double rate, double drift, std::vector<Cf>& tmp,
                        std::vector<std::complex<double>>& buf, const std::vector<double>& w,
                        const Fft& fft, double* freqOut) {
    dechirp(x, N, rate, drift, tmp.data());
    for (std::size_t i = 0; i < N; ++i) { buf[i] = {tmp[i].real() * w[i], tmp[i].imag() * w[i]}; }
    fft.forward(buf.data());
    std::size_t pk = 1;
    double best = 0.0;
    for (std::size_t k = 1; k + 1 < N; ++k) {
        const double s = std::norm(buf[k - 1]) + std::norm(buf[k]) + std::norm(buf[k + 1]);
        if (s > best) { best = s; pk = k; }
    }
    if (freqOut != nullptr) {
        const double a = std::log(std::max(std::norm(buf[pk - 1]), 1e-300));
        const double b = std::log(std::max(std::norm(buf[pk]), 1e-300));
        const double c = std::log(std::max(std::norm(buf[pk + 1]), 1e-300));
        const double den = a - 2.0 * b + c;
        const double delta = den != 0.0 ? std::min(0.5, std::max(-0.5, 0.5 * (a - c) / den)) : 0.0;
        double bin = static_cast<double>(pk) + delta;
        if (bin >= static_cast<double>(N) / 2.0) { bin -= static_cast<double>(N); }
        *freqOut = bin * rate / static_cast<double>(N);
    }
    return best;
}

}  // namespace detail

inline DriftResult searchDrift(const Cf* x, std::size_t n, double rate, const DriftParams& prm = {}) {
    DriftResult r;
    if (const char* bad = checkIq(x, n, rate, 1024)) { r.reason = bad; return r; }
    if (!(prm.maxDriftHzPerS > prm.minDriftHzPerS) || !std::isfinite(prm.minDriftHzPerS) ||
        !std::isfinite(prm.maxDriftHzPerS) || prm.gridSteps < 8 || !isPow2(prm.maxFft) || prm.maxFft < 1024) {
        r.reason = why::kBadParam;
        return r;
    }
    const std::size_t N = std::min(floorPow2(n), prm.maxFft);
    const std::vector<double> w = makeWindow(Win::Hann, N);
    const Fft fft(N);
    std::vector<Cf> tmp(N);
    std::vector<std::complex<double>> buf(N);
    const double p0 = detail::linePower(x, N, rate, 0.0, tmp, buf, w, fft, nullptr);
    // coarse grid (include the ends)
    const double step = (prm.maxDriftHzPerS - prm.minDriftHzPerS) / static_cast<double>(prm.gridSteps);
    double bestA = prm.minDriftHzPerS, bestP = -1.0;
    for (std::size_t i = 0; i <= prm.gridSteps; ++i) {
        const double a = prm.minDriftHzPerS + step * static_cast<double>(i);
        const double pw = detail::linePower(x, N, rate, a, tmp, buf, w, fft, nullptr);
        if (pw > bestP) { bestP = pw; bestA = a; }
    }
    // golden-section refinement within +-1 step of the best
    double lo = std::max(prm.minDriftHzPerS, bestA - step), hi = std::min(prm.maxDriftHzPerS, bestA + step);
    const double gr = 0.6180339887498949;
    double x1 = hi - gr * (hi - lo), x2 = lo + gr * (hi - lo);
    double f1 = detail::linePower(x, N, rate, x1, tmp, buf, w, fft, nullptr);
    double f2 = detail::linePower(x, N, rate, x2, tmp, buf, w, fft, nullptr);
    for (int it = 0; it < 28; ++it) {
        if (f1 > f2) { hi = x2; x2 = x1; f2 = f1; x1 = hi - gr * (hi - lo); f1 = detail::linePower(x, N, rate, x1, tmp, buf, w, fft, nullptr); }
        else { lo = x1; x1 = x2; f1 = f2; x2 = lo + gr * (hi - lo); f2 = detail::linePower(x, N, rate, x2, tmp, buf, w, fft, nullptr); }
    }
    r.driftHzPerS = 0.5 * (lo + hi);
    double f0 = 0.0;
    const double pBest = detail::linePower(x, N, rate, r.driftHzPerS, tmp, buf, w, fft, &f0);
    double sw2 = 0.0;
    for (double v : w) { sw2 += v * v; }
    r.startFreqHz = f0;
    r.peakDbfs = toDb(pBest / (static_cast<double>(N) * sw2));
    r.gainDb = p0 > 0.0 ? toDb(pBest / p0) : kNaN;
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_AUDIO_HPP
