// sigmeas_bandwidth.hpp - OCCUPIED BANDWIDTH and X-dB BANDWIDTH of a selected signal, and its
// centre and signal-to-noise ratio. Row: "occupied-bandwidth and x-dB bandwidth readout for any
// selected signal" (REMAINING-GAPS.md, Signal analysis and identification).
//
// DEFINITIONS
//  - Occupied bandwidth (beta-percent): "the width of a frequency band such that, below its lower
//    and above its upper frequency limits, the mean powers emitted are each equal to beta/2 of
//    the total mean power of a given emission" - ITU Radio Regulations No. 1.153, as quoted in
//    ITU-R Recommendation SM.328-12 (opened: the percentage beta/2 is taken as 0.5 % unless an
//    ITU-R Recommendation says otherwise, i.e. 99 % of the power lies inside). The total here is the signal's power ABOVE THE
//    NOISE FLOOR inside the signal region: the floor is measured from the lowest quartile of the
//    bins and subtracted bin by bin, so a noisy capture does not widen the answer by the noise's
//    own tails.
//  - x-dB bandwidth: "the width of the frequency band such that, beyond its upper and lower
//    limits, any discrete spectrum component or continuous spectral power density is at least
//    x dB lower than a predetermined 0 dB reference level" (ITU-R SM.328-12 section 1.8, opened;
//    its "x dBsd" variant takes the maximum of the power spectral density as the 0 dB reference,
//    which is what is done here, on a lightly smoothed spectrum; ITU-R SM.443 describes the
//    measurement, but its PDF could not be text-extracted and was not read). It is the OUTERMOST pair of
//    crossings inside the signal region, so a two-lobed FSK spectrum reads as one wide signal.
//  - Centre: the power centroid of the signal region (the signal's own centre, not the peak).
//
// METHOD. Welch spectrum (Hann, 50 % overlap), floor from the 25th percentile corrected for the
// averaging (Wilson-Hilferty chi-square quantile), signal region = bins whose 3-bin average is
// `detectMarginDb` over the floor with gaps up to `bridgeFraction` of the range closed, plus a
// guard of a quarter of its width. Cumulative signed excess power with linear interpolation inside
// a bin gives the edges.
//
// ACCURACY, pinned in tests/test_sigmeas_bandwidth.cpp: OBW within 2 % of the known 99 %-power
// bandwidth of generated signals (flat and Gaussian spectra) at 20 dB SNR, with a frequency offset
// and a 300 ppm sample-rate error.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_BANDWIDTH_HPP
#define CASCADE_DSP_SIGMEAS_BANDWIDTH_HPP

#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct XdbEntry {
    double db = 0.0;
    bool valid = false;  // false when the level lies at or under the noise floor
    double widthHz = kNaN;
    double lowHz = kNaN;
    double highHz = kNaN;
};

struct BandwidthParams {
    double obwFraction = 0.99;            // (0.5, 1): the share of the power inside the band
    double bandLowHz = kNaN;              // optional search window (the selected box); NaN = all
    double bandHighHz = kNaN;
    double floorPower = 0.0;              // > 0: linear power per bin to use as the floor
    double detectMarginDb = 3.0;          // signal region = this far over the floor
    double bridgeFraction = 0.05;         // gaps this small (of the range) do not split a signal
    std::size_t nfft = 0;                 // 0 = automatic
    std::vector<double> xDb = {3.0, 6.0, 20.0, 26.0};
};

struct BandwidthResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double centreHz = kNaN;               // power centroid of the signal
    double peakHz = kNaN;                 // strongest bin (smoothed)
    double obwHz = kNaN;
    double obwLowHz = kNaN;
    double obwHighHz = kNaN;
    double signalPowerDbfs = kNaN;        // power above the floor, full scale = 0 dB
    double floorDbPerBin = kNaN;          // noise floor, dBFS in one bin of binHz
    double snrDb = kNaN;                  // signal over the floor in the same width
    double binHz = 0.0;
    double occupancy = 0.0;               // share of the range inside the signal region
    bool floorReliable = false;           // false when the signal fills most of the range
    std::vector<XdbEntry> xdb;
};

namespace detail {

// The p-quantile (0..1) of a gamma distribution with shape k, mean 1 (Wilson-Hilferty).
inline double gammaQuantileOverMean(double k, double z) {
    const double a = 1.0 - 1.0 / (9.0 * k) + z / (3.0 * std::sqrt(k));
    return std::max(a, 0.05) * std::max(a, 0.05) * std::max(a, 0.05);
}

}  // namespace detail

inline BandwidthResult measureBandwidth(const Cf* x, std::size_t n, double rate,
                                        const BandwidthParams& prm = {}) {
    BandwidthResult r;
    if (!(prm.obwFraction > 0.5 && prm.obwFraction < 1.0) || !(prm.detectMarginDb > 0.0) ||
        !(prm.bridgeFraction >= 0.0 && prm.bridgeFraction < 1.0)) {
        r.reason = why::kBadParam;
        return r;
    }
    const Psd psd = welchPsd(x, n, rate, prm.nfft);
    if (!psd.valid) {
        r.reason = psd.reason;
        return r;
    }
    const std::size_t N = psd.nfft;
    r.binHz = psd.binHz;

    // The search range [k0, k1) of bins.
    std::size_t k0 = 0, k1 = N;
    if (std::isfinite(prm.bandLowHz)) {
        const double b = prm.bandLowHz / psd.binHz + static_cast<double>(N) / 2.0;
        k0 = static_cast<std::size_t>(std::min(std::max(std::floor(b), 0.0), static_cast<double>(N)));
    }
    if (std::isfinite(prm.bandHighHz)) {
        const double b = prm.bandHighHz / psd.binHz + static_cast<double>(N) / 2.0;
        k1 = static_cast<std::size_t>(std::min(std::max(std::ceil(b), 0.0), static_cast<double>(N)));
    }
    if (k1 <= k0 + 8) {
        r.reason = why::kBadParam;
        return r;
    }
    const std::size_t range = k1 - k0;

    // Floor.
    const double kEff = static_cast<double>(psd.segments) / 1.056;
    double floorP = prm.floorPower;
    if (!(floorP > 0.0)) {
        std::vector<double> tmp(psd.p.begin() + static_cast<std::ptrdiff_t>(k0),
                                psd.p.begin() + static_cast<std::ptrdiff_t>(k1));
        const double q25 = percentileInPlace(tmp, 0.25);
        floorP = q25 / detail::gammaQuantileOverMean(kEff, -0.6745);
    }
    if (!(floorP > 0.0)) {
        r.reason = why::kNoSignal;
        return r;
    }
    r.floorDbPerBin = toDb(floorP);

    // Smoothed spectrum (3 bins) for the detection.
    std::vector<double> sm(psd.p.begin(), psd.p.end());
    std::vector<double> scratch;
    movingAverage(sm, 3, scratch);
    const double thr = floorP * fromDb(prm.detectMarginDb);

    std::size_t peak = k0;
    for (std::size_t k = k0; k < k1; ++k) {
        if (sm[k] > sm[peak]) { peak = k; }
    }
    if (!(sm[peak] > thr)) {
        r.reason = why::kNoSignal;
        return r;
    }
    r.peakHz = psd.freqOf(peak);

    // Region around the peak: walk out, closing gaps up to `bridge` bins.
    const std::size_t bridge = static_cast<std::size_t>(
        std::max(3.0, std::floor(prm.bridgeFraction * static_cast<double>(range))));
    std::size_t lo = peak, hi = peak;
    {
        std::size_t gap = 0;
        for (std::size_t k = peak; k-- > k0;) {
            if (sm[k] > thr) { lo = k; gap = 0; }
            else if (++gap > bridge) { break; }
        }
        gap = 0;
        for (std::size_t k = peak + 1; k < k1; ++k) {
            if (sm[k] > thr) { hi = k; gap = 0; }
            else if (++gap > bridge) { break; }
        }
    }
    const std::size_t width = hi - lo + 1;
    r.occupancy = static_cast<double>(width) / static_cast<double>(range);
    r.floorReliable = r.occupancy < 0.6;
    const std::size_t guard = width / 4 + 4;
    const std::size_t g0 = lo > k0 + guard ? lo - guard : k0;
    const std::size_t g1 = std::min(k1, hi + guard + 1);  // exclusive

    // Cumulative signed excess power over the extended region.
    const std::size_t m = g1 - g0;
    std::vector<double> cum(m + 1, 0.0);  // cum[i] = power in bins g0 .. g0+i-1
    for (std::size_t i = 0; i < m; ++i) {
        cum[i + 1] = cum[i] + (psd.p[g0 + i] - floorP);
        if (cum[i + 1] < 0.0) { cum[i + 1] = 0.0; }
    }
    const double S = cum[m];
    if (!(S > 0.0)) {
        r.reason = why::kNoSignal;
        return r;
    }
    // Edge frequency where the cumulative power reaches `target`.
    auto edgeAt = [&](double target) {
        std::size_t i = 1;
        while (i < m && cum[i] < target) { ++i; }
        const double c0 = cum[i - 1], c1 = cum[i];
        const double frac = c1 > c0 ? (target - c0) / (c1 - c0) : 0.5;
        // bin (i-1) spans [f - bin/2, f + bin/2]; the fraction runs across it.
        return psd.freqOf(g0 + i - 1) - 0.5 * psd.binHz + frac * psd.binHz;
    };
    const double tail = (1.0 - prm.obwFraction) / 2.0;
    r.obwLowHz = edgeAt(tail * S);
    r.obwHighHz = edgeAt((1.0 - tail) * S);
    r.obwHz = r.obwHighHz - r.obwLowHz;

    double cf = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
        const double s = std::max(0.0, psd.p[g0 + i] - floorP);
        cf += s * psd.freqOf(g0 + i);
    }
    double sPos = 0.0;
    for (std::size_t i = 0; i < m; ++i) { sPos += std::max(0.0, psd.p[g0 + i] - floorP); }
    r.centreHz = sPos > 0.0 ? cf / sPos : r.peakHz;

    r.signalPowerDbfs = toDb(S);
    const double noiseInBand = floorP * (r.obwHz / psd.binHz);
    r.snrDb = noiseInBand > 0.0 ? toDb(S / noiseInBand) : kNaN;

    // x-dB widths: outermost crossings in the extended region of a 5-bin smoothed spectrum.
    std::vector<double> sm5(psd.p.begin(), psd.p.end());
    movingAverage(sm5, 5, scratch);
    // The 0 dB reference is the top of a spectrum smoothed over a sixteenth of the signal's
    // width: the strongest single bin of a broad signal is a random excursion, not its level.
    std::vector<double> smTop(psd.p.begin(), psd.p.end());
    movingAverage(smTop, std::max<std::size_t>(5, (width / 16) | 1u), scratch);
    double top = 0.0;
    for (std::size_t k = g0; k < g1; ++k) { top = std::max(top, smTop[k]); }
    for (double xd : prm.xDb) {
        XdbEntry e;
        e.db = xd;
        const double level = top / fromDb(xd);
        if (std::isfinite(xd) && xd > 0.0 && level > floorP * 1.5) {
            std::size_t a = g1, b = g0;
            bool any = false;
            for (std::size_t k = g0; k < g1; ++k) {
                if (sm5[k] >= level) {
                    if (!any) { a = k; any = true; }
                    b = k;
                }
            }
            if (any) {
                auto interp = [&](std::size_t inside, std::size_t outside) {
                    const double pi = sm5[inside], po = sm5[outside];
                    const double t = pi != po ? (pi - level) / (pi - po) : 0.0;
                    return psd.freqOf(inside) + (psd.freqOf(outside) - psd.freqOf(inside)) *
                                                    std::min(1.0, std::max(0.0, t));
                };
                e.lowHz = a > g0 ? interp(a, a - 1) : psd.freqOf(a);
                e.highHz = b + 1 < g1 ? interp(b, b + 1) : psd.freqOf(b);
                e.widthHz = e.highHz - e.lowHz;
                e.valid = true;
            }
        }
        r.xdb.push_back(e);
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_BANDWIDTH_HPP
