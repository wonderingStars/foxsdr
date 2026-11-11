// pseudo_doppler.hpp - PSEUDO-DOPPLER DIRECTION FINDING with ONE receiver and a switched four-antenna array: the bearing of a carrier from the
// phase pattern the switching leaves in the I/Q stream.
//
// STAND-ALONE (lane rfmon): nothing in the engine uses it. THE SWITCHING ITSELF IS HARDWARE (an antenna switch driven from a clock or a GPIO):
// not built here; this block takes the I/Q the receiver delivers while such a switch steps through the antennas, and the switching clock as a
// nominal number of samples per dwell with a start sample. See the record for the hand-off.
//
// GEOMETRY AND SIGN CONVENTION. Four antennas on a circle, number k (0..3) at the angle k * 90 degrees counter-clockwise seen from above, the
// array's own zero (antenna 0's direction) being the reference. They are connected one at a time, in the order 0, 1, 2, 3, 0, ..., each for
// one dwell of D samples. A plane wave from the direction THETA (the direction TOWARDS the transmitter, counter-clockwise from antenna 0)
// reaches antenna k earlier by r cos(theta - k pi/2) / c, so its phase there is advanced by
//         a_k = beta cos(theta - k pi/2),   beta = 2 pi r / lambda,     (a_0, a_1, a_2, a_3) = beta (cos t, sin t, -cos t, -sin t).
// THE BEARING RETURNED IS THETA in degrees in [0, 360): 0 = towards antenna 0, 90 = towards antenna 1, counter-clockwise. (A compass bearing
// is clockwise: the caller converts with its own mounting offset.)
//
// METHOD. (1) Align: the phase steps at the dwell boundaries show where the clock really is; blocks of 8 rotations (32 dwells) each refine the
// boundary offset within +-D/4 of the previous block's (+-D/2 for the first), so a switching clock that runs a little fast or slow is followed.
// (2) Frequency: the carrier's offset (unknown, and a ramp that would be read as a bearing) is estimated inside the dwells only, lag 1 then a
// longer lag, and removed. (3) Each dwell's complex mean z_j (guard samples at both ends dropped, where a real switch settles). (4) The phase
// steps between neighbouring dwells, summed over all rotations for each of the four step positions k: Q_k = sum z_j conj(z_{j-1}); phi_k =
// arg Q_k = rho D + d_k with d_0 = a_0 - a_3, d_1 = a_1 - a_0, d_2 = a_2 - a_1, d_3 = a_3 - a_2. (5) The sum of the four d_k is zero, so the
// mean of the four phi_k is the leftover frequency term rho D; remove it. Then (d_0 - d_2) / 2 = beta (cos t + sin t) and
// (d_3 - d_1) / 2 = beta (cos t - sin t), so theta = atan2(u - v, u + v): beta (the ring's radius in wavelengths) drops out of the bearing.
//
// QUALITY in [0, 1] = (agreement of the steps over the rotations, mean |sum q| / sum |q| per step position) x (agreement of the two
// independent estimates of the leftover frequency term, (phi_0 + phi_2)/2 against (phi_1 + phi_3)/2: 1 when they agree, 0 at a quarter turn
// apart). It falls to a small value on noise alone; it is a consistency figure, NOT a calibrated confidence.
//
// LIMITS. beta sqrt(2) + |leftover frequency term| must stay below pi, which holds for a ring radius up to about 0.3 wavelengths; the carrier
// offset must be below about fs / 10 (the longer lag); the clock may be off by up to about 0.5 percent (a block's boundaries move less than a
// quarter dwell); D is at least 8 samples (16 or more is sensible, so the guard leaves something to average); at least 8 dwells (2 rotations)
// must be present. One carrier only: two signals on one channel read as an in-between bearing with a low quality. NOT modelled: an antenna
// whose gain or phase differs from the others (a real ring must be calibrated), multipath, polarisation.
//
// THREADS: stateless free functions; allocation of a few small vectors per call (control thread / worker, not the DSP callback).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace cascade::dsp {

struct PseudoDopplerResult {
    bool valid = false;
    double bearingDeg = 0.0;        // [0, 360), theta as defined in the header
    double quality = 0.0;           // [0, 1], consistency figure
    double freqOffsetRadPerSample = 0.0;   // the carrier offset that was removed (mean over blocks)
    double clockOffsetSamples = 0.0;       // how far the aligned boundary moved from the nominal one by the end of the record (positive: later)
    std::size_t rotations = 0;      // whole rotations used
};

inline constexpr std::size_t kPdMinDwellSamples = 8;
inline constexpr std::size_t kPdMinDwells = 8;
inline constexpr std::size_t kPdBlockDwells = 32;

namespace pd_detail {

inline double wrapPi(double a) {
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    a = std::fmod(a + 3.14159265358979323846, kTwoPi);
    if (a < 0) { a += kTwoPi; }
    return a - 3.14159265358979323846;
}

using cd = std::complex<double>;

}  // namespace pd_detail

// x: n complex samples. samplesPerDwell: nominal (may be fractional). startSample: where antenna 0's first dwell begins (nominal; refined).
inline PseudoDopplerResult estimatePseudoDopplerBearing(const std::complex<float>* x, std::size_t n, double samplesPerDwell, double startSample) {
    using namespace pd_detail;
    PseudoDopplerResult res;
    const double D = samplesPerDwell;
    if (x == nullptr || !std::isfinite(D) || D < static_cast<double>(kPdMinDwellSamples) || !std::isfinite(startSample) || startSample < 0.0) { return res; }
    if (static_cast<double>(n) < startSample + D * (static_cast<double>(kPdMinDwells) + 1.0)) { return res; }
    const long nTotal = static_cast<long>(std::floor((static_cast<double>(n) - startSample) / D)) - 1;   // dwells that fit with room for the offset search
    if (nTotal < static_cast<long>(kPdMinDwells)) { return res; }

    const long guard = std::max<long>(2, static_cast<long>(D / 8.0));
    cd G[4] = {cd(0, 0), cd(0, 0), cd(0, 0), cd(0, 0)};
    double absSum[4] = {0, 0, 0, 0};
    double rhoSum = 0.0;
    std::size_t rhoBlocks = 0, rotations = 0;
    double offset = 0.0;           // aligned boundary offset from nominal, tracked block to block
    bool firstBlock = true;
    long j0 = 0;
    while (j0 + static_cast<long>(kPdMinDwells) <= nTotal) {
        long nd = std::min<long>(static_cast<long>(kPdBlockDwells), nTotal - j0);
        nd -= nd % 4;                                           // whole rotations only
        if (nd < static_cast<long>(kPdMinDwells)) { break; }
        auto boundary = [&](long j, double o) { return startSample + static_cast<double>(j) * D + o; };
        // ---- (1) alignment: where do the phase steps sit?
        // Mean carrier phase step from every lag-1 product of the block (the dwell-boundary steps average out over a rotation).
        const long bLo = static_cast<long>(std::max(1.0, std::floor(boundary(j0, offset - D))));
        const long bHi = std::min<long>(static_cast<long>(n) - 1, static_cast<long>(std::ceil(boundary(j0 + nd, offset + D))));
        cd sumAll(0, 0);
        for (long i = bLo; i < bHi; ++i) { sumAll += cd(x[i].real(), x[i].imag()) * std::conj(cd(x[i - 1].real(), x[i - 1].imag())); }
        const double rho0 = std::arg(sumAll);
        // A boundary step is read between the mean of w samples before it and w samples after it (w = D/8: SNR gain of w over a single
        // sample pair, which is what keeps the alignment alive when the signal is below the noise per sample); the carrier turns rho0 w
        // between the centres of the two windows.
        const long w = std::max<long>(2, static_cast<long>(D / 8.0));
        const cd rotBack = std::polar(1.0, -rho0 * static_cast<double>(w));
        const double search = firstBlock ? D / 2.0 : D / 4.0;
        double bestScore = -1.0, bestO = offset;
        for (long sft = -static_cast<long>(std::ceil(search)); sft <= static_cast<long>(std::ceil(search)); ++sft) {
            const double o = offset + static_cast<double>(sft);
            double score = 0.0;
            for (long j = j0; j < j0 + nd; ++j) {
                const long b = static_cast<long>(std::llround(boundary(j, o)));
                if (b - w < 0 || b + w > static_cast<long>(n)) { continue; }
                cd after(0, 0), before(0, 0);
                for (long i = 0; i < w; ++i) {
                    after += cd(x[b + i].real(), x[b + i].imag());
                    before += cd(x[b - w + i].real(), x[b - w + i].imag());
                }
                const cd y = after * std::conj(before) * rotBack;
                const double m = std::abs(y);
                score += m > 0.0 ? 1.0 - y.real() / m : 0.0;     // 1 - cos(phase deviation)
            }
            if (score > bestScore) { bestScore = score; bestO = o; }
        }
        offset = bestO;
        firstBlock = false;
        // ---- (2) frequency inside the dwells: lag 1, then a longer lag.
        auto dwellLo = [&](long j) { return static_cast<long>(std::llround(boundary(j, offset))) + guard; };
        auto dwellHi = [&](long j) { return static_cast<long>(std::llround(boundary(j + 1, offset))) - guard; };
        const long first = dwellLo(j0);
        const long last = dwellHi(j0 + nd - 1);
        if (first < 1 || last > static_cast<long>(n) || last - first < 4) { break; }
        cd s1(0, 0);
        for (long j = j0; j < j0 + nd; ++j) {
            for (long i = dwellLo(j) + 1; i < dwellHi(j); ++i) { s1 += cd(x[i].real(), x[i].imag()) * std::conj(cd(x[i - 1].real(), x[i - 1].imag())); }
        }
        double rho = std::arg(s1);
        // Longer lags in steps of three, each refining the last (a lag is only unambiguous while the estimate so far is good to about a
        // radian over it; stepping by three keeps that true down to a signal well below the noise per sample).
        const long maxLag = std::max<long>(2, (static_cast<long>(D) - 2 * guard) / 2);
        for (long lag = 3; lag <= maxLag || lag == 3; lag *= 3) {
            const long L = std::min(lag, maxLag);
            cd sL(0, 0);
            const cd back = std::polar(1.0, -rho * static_cast<double>(L));
            for (long j = j0; j < j0 + nd; ++j) {
                for (long i = dwellLo(j) + L; i < dwellHi(j); ++i) { sL += cd(x[i].real(), x[i].imag()) * std::conj(cd(x[i - L].real(), x[i - L].imag())) * back; }
            }
            if (std::abs(sL) > 0.0) { rho += std::arg(sL) / static_cast<double>(L); }
            if (L == maxLag) { break; }
        }
        rhoSum += rho;
        ++rhoBlocks;
        // ---- (3) dwell means, derotated to the block's first sample.
        std::vector<cd> z(static_cast<std::size_t>(nd));
        for (long j = j0; j < j0 + nd; ++j) {
            cd acc(0, 0);
            for (long i = dwellLo(j); i < dwellHi(j); ++i) {
                acc += cd(x[i].real(), x[i].imag()) * std::polar(1.0, -rho * static_cast<double>(i - first));
            }
            z[static_cast<std::size_t>(j - j0)] = acc;
        }
        // ---- (4) phase steps between neighbouring dwells, by step position k = dwell index mod 4 (global numbering from the nominal start).
        for (long jj = 1; jj < nd; ++jj) {
            const long j = j0 + jj;
            const cd q = z[static_cast<std::size_t>(jj)] * std::conj(z[static_cast<std::size_t>(jj - 1)]);
            const int k = static_cast<int>(((j % 4) + 4) % 4);
            G[k] += q;
            absSum[k] += std::abs(q);
        }
        rotations += static_cast<std::size_t>(nd / 4);
        j0 += nd;
    }
    if (rhoBlocks == 0 || rotations < 2) { return res; }
    for (int k = 0; k < 4; ++k) { if (!(absSum[k] > 0.0)) { return res; } }
    // ---- (5) the bearing.
    double phi[4];
    for (int k = 0; k < 4; ++k) { phi[k] = std::arg(G[k]); }
    const double rhoD = 0.25 * (phi[0] + phi[1] + phi[2] + phi[3]);
    double d[4];
    for (int k = 0; k < 4; ++k) { d[k] = phi[k] - rhoD; }
    const double u = 0.5 * (d[0] - d[2]);
    const double v = 0.5 * (d[3] - d[1]);
    double deg = std::atan2(u - v, u + v) * 180.0 / 3.14159265358979323846;
    if (deg < 0.0) { deg += 360.0; }
    if (deg >= 360.0) { deg -= 360.0; }
    double coh = 0.0;
    for (int k = 0; k < 4; ++k) { coh += std::abs(G[k]) / absSum[k]; }
    coh *= 0.25;
    const double ra = 0.5 * (phi[0] + phi[2]);
    const double rb = 0.5 * (phi[1] + phi[3]);
    const double agree = std::max(0.0, 1.0 - std::fabs(ra - rb) / (3.14159265358979323846 / 2.0));
    res.valid = true;
    res.bearingDeg = deg;
    res.quality = std::min(1.0, std::max(0.0, coh * agree));
    res.freqOffsetRadPerSample = rhoSum / static_cast<double>(rhoBlocks);
    res.clockOffsetSamples = offset;
    res.rotations = rotations;
    return res;
}

// Smallest angular difference between two bearings in degrees, in [0, 180].
inline double pdBearingErrorDeg(double a, double b) {
    double e = std::fmod(a - b, 360.0);
    if (e < 0) { e += 360.0; }
    return e > 180.0 ? 360.0 - e : e;
}

}  // namespace cascade::dsp
