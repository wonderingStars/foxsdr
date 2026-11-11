// sigmeas_vstime.hpp - AMPLITUDE, FREQUENCY and PHASE VERSUS TIME of a selected signal (the three
// plots inspectrum draws for a selection). Row: "amplitude, frequency and phase versus time plots of
// a selected signal" (REMAINING-GAPS.md, Signal analysis and identification).
//
// METHOD. The signal is shifted to DC by `centreHz` (a rotator, no per-sample sin/cos), low-passed by
// two cascaded boxcar averages of length D (a second-order CIC: zeros at every multiple of rate/D,
// so the alias bands that fall on the output rate are nulled) and decimated by D, where D is chosen
// so the output rate is at least 2.5 times `bandwidthHz` and the point count is at most
// `maxPoints` (0 = no limit). Then, per output point y:
//     amplitude = |y|                      (linear, full scale 1.0; and in dBFS)
//     phase     = unwrapped arg(y)         (radians; the phase of the shifted signal)
//     frequency = d(phase)/dt / 2 pi + centreHz   (Hz from the span centre)
// The group delay of the filter is D-1 input samples: point k belongs to time
// startS + k * decimation / rate, with startS = (D - 1) / rate.
// `removeLinearPhase` subtracts the least-squares line from the phase (the residual after a carrier
// offset, which is what a PSK or FM phase plot is read against).
//
// The decimated frequency track can only follow a signal whose frequency stays inside +-rateOut/2
// of `centreHz`; a signal that leaves it aliases (the caller chooses the bandwidth to cover it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_VSTIME_HPP
#define CASCADE_DSP_SIGMEAS_VSTIME_HPP

#include <cstddef>
#include <vector>

#include "dsp/sigmeas_common.hpp"

namespace cascade::sigmeas {

struct VsTimeParams {
    double centreHz = 0.0;        // the signal's frequency from the span centre (shifted to DC)
    double bandwidthHz = 0.0;     // the signal's width; 0 = the whole span (no decimation)
    std::size_t maxPoints = 0;    // 0 = no limit
    bool removeLinearPhase = false;
};

struct VsTimeResult {
    bool valid = false;
    const char* reason = why::kEmpty;
    double rateOut = 0.0;
    std::size_t decimation = 1;
    double startS = 0.0;
    double stepS = 0.0;
    std::vector<float> amplitude;     // linear
    std::vector<float> amplitudeDb;   // dBFS (20 log10)
    std::vector<double> phaseRad;     // unwrapped
    std::vector<double> freqHz;       // from the span centre
    double meanFreqHz = kNaN;
    double phaseSlopeRadPerS = kNaN;  // the fitted line (always reported)
};

inline VsTimeResult computeVsTime(const Cf* x, std::size_t n, double rate, const VsTimeParams& prm = {}) {
    VsTimeResult r;
    if (const char* bad = checkIq(x, n, rate, 16)) { r.reason = bad; return r; }
    if (!std::isfinite(prm.centreHz) || std::fabs(prm.centreHz) > rate / 2.0 || !(prm.bandwidthHz >= 0.0) ||
        !std::isfinite(prm.bandwidthHz)) {
        r.reason = why::kBadParam;
        return r;
    }
    std::size_t D = 1;
    if (prm.bandwidthHz > 0.0) {
        D = static_cast<std::size_t>(std::max(1.0, std::floor(rate / (2.5 * prm.bandwidthHz))));
    }
    if (prm.maxPoints > 0) { D = std::max(D, (n + prm.maxPoints - 1) / prm.maxPoints); }
    if (n < 4 * D + 4) {
        r.reason = why::kTooShort;
        return r;
    }

    // mix to DC
    std::vector<std::complex<double>> y(n);
    {
        const std::complex<double> rot = std::polar(1.0, -kTwoPi * prm.centreHz / rate);
        std::complex<double> z = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            y[i] = std::complex<double>(x[i].real(), x[i].imag()) * z;
            z *= rot;
            if ((i & 1023u) == 1023u) { z /= std::abs(z); }
        }
    }
    // two boxcars (running sums), output index j covers input j .. j+2D-2
    std::size_t len = n;
    std::vector<std::complex<double>> cs;
    for (int stage = 0; stage < 2 && D > 1; ++stage) {
        cs.assign(len + 1, 0.0);
        for (std::size_t i = 0; i < len; ++i) { cs[i + 1] = cs[i] + y[i]; }
        const std::size_t outLen = len - D + 1;
        for (std::size_t j = 0; j < outLen; ++j) { y[j] = (cs[j + D] - cs[j]) / static_cast<double>(D); }
        len = outLen;
    }
    const std::size_t count = (len + D - 1) / D;
    r.decimation = D;
    r.rateOut = rate / static_cast<double>(D);
    r.stepS = static_cast<double>(D) / rate;
    r.startS = static_cast<double>(D - 1) / rate;
    r.amplitude.resize(count);
    r.amplitudeDb.resize(count);
    r.phaseRad.resize(count);
    r.freqHz.resize(count);
    double prev = 0.0, acc = 0.0;
    for (std::size_t k = 0; k < count; ++k) {
        const std::complex<double> v = y[k * D];
        const double a = std::abs(v);
        r.amplitude[k] = static_cast<float>(a);
        r.amplitudeDb[k] = static_cast<float>(20.0 * std::log10(std::max(a, 1e-12)));
        const double ph = std::atan2(v.imag(), v.real());
        if (k == 0) {
            acc = ph;
        } else {
            double d = ph - prev;
            d -= kTwoPi * std::round(d / kTwoPi);
            acc += d;
        }
        prev = ph;
        r.phaseRad[k] = acc;
    }
    // frequency: central difference, one-sided at the ends
    for (std::size_t k = 0; k < count; ++k) {
        const std::size_t a = k > 0 ? k - 1 : 0;
        const std::size_t b = k + 1 < count ? k + 1 : count - 1;
        const double dt = static_cast<double>(b - a) * r.stepS;
        r.freqHz[k] = dt > 0.0 ? (r.phaseRad[b] - r.phaseRad[a]) / dt / kTwoPi + prm.centreHz : prm.centreHz;
    }
    r.meanFreqHz = meanOf(r.freqHz);
    // least-squares line of the phase
    {
        const double N = static_cast<double>(count);
        double st = 0, sp = 0, stt = 0, stp = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const double t = static_cast<double>(k) * r.stepS;
            st += t;
            sp += r.phaseRad[k];
            stt += t * t;
            stp += t * r.phaseRad[k];
        }
        const double den = N * stt - st * st;
        const double slope = den > 0.0 ? (N * stp - st * sp) / den : 0.0;
        const double icpt = (sp - slope * st) / N;
        r.phaseSlopeRadPerS = slope;
        if (prm.removeLinearPhase) {
            for (std::size_t k = 0; k < count; ++k) {
                r.phaseRad[k] -= icpt + slope * static_cast<double>(k) * r.stepS;
            }
        }
    }
    r.valid = true;
    r.reason = why::kOk;
    return r;
}

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_VSTIME_HPP
