// Measurement tap for a symbol stream: constellation points and eye-diagram traces, at a given symbol rate.
//
// INPUT: complex baseband at `sampleRateHz`, already matched-filtered and carrier-corrected by the demodulator that owns it, with
// 2 to 256 samples per symbol (sps = fs / Rs, need not be an integer). The tap only LOOKS: it does not filter, derotate or decide.
//
// TIMING. A constant-envelope or evenly spread signal after a matched filter has its largest mean power at the symbol instants (the
// transitions pass through the eye's crossings, where the envelope dips). The tap folds |x|^2 modulo the symbol period into min(24, floor(sps)) phase
// bins, each remembering the mean phase of the samples that hit it (a smoothed histogram, a hit adds 1/32 of the difference), takes the strongest bin with a circular parabolic refinement, and
// moves its symbol clock to it by at most 0.02 sample per symbol (so the picture does not jitter). It locks after about 64 symbols. Manual
// timing (setTimingOffsetSamples) replaces this. The method is a measurement heuristic written for this tap (the fold-and-peak idea is
// the textbook "spectral line / energy" timing estimator family; no source was copied or consulted).
// SAMPLING: cubic (4-point Lagrange) interpolation at the symbol instant, so a fractional instant costs well under 1 % of amplitude at
// 4 samples per symbol (measured by the test).
// CONSTELLATION: a ring of the latest N symbol points (default 512). EYE: a ring of the latest M traces (default 64), each of 2 symbol
// periods centred on a symbol instant, resampled to `eyePointsPerSymbol` (default 32) points per symbol, so 2 P + 1 points, I and Q.
// eyeOpening(): for binary or QPSK I (and Q) rails: (smallest positive centre value - largest negative one) divided by twice the mean
// absolute centre value (1 = fully open, 0 = closed).
//
// THREADS: push() from ONE thread; the snapshots from any thread (a sequence check retries or returns fewer points; they never mix a
// torn point). No allocation after construction. Non-finite samples read as 0. Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cascade::dsp {

class SymbolTap {
public:
    static constexpr int kBins = 24;

    SymbolTap(double sampleRateHz, double symbolRateHz, std::size_t constellationPoints = 512, std::size_t eyeTraces = 64,
              std::size_t eyePointsPerSymbol = 32)
        : fs_(sampleRateHz), sps_(symbolRateHz > 0.0 ? sampleRateHz / symbolRateHz : 0.0), cap_(std::max<std::size_t>(constellationPoints, 8)),
          traces_(std::max<std::size_t>(eyeTraces, 2)), perSym_(std::max<std::size_t>(eyePointsPerSymbol, 4)) {
        valid_ = std::isfinite(sps_) && sps_ >= 2.0 && sps_ <= 256.0;
        std::size_t h = 64;
        while (h < static_cast<std::size_t>(2.0 * sps_ + 16.0)) { h <<= 1; }
        hist_.assign(valid_ ? h : 64, std::complex<float>(0, 0));
        hmask_ = hist_.size() - 1;
        pts_.assign(cap_, std::complex<float>(0, 0));
        eyeLen_ = 2 * perSym_ + 1;
        eyeI_.assign(traces_ * eyeLen_, 0.0f);
        eyeQ_.assign(traces_ * eyeLen_, 0.0f);
        nb_ = static_cast<int>(std::min(24.0, std::max(2.0, std::floor(sps_))));
        for (int i = 0; i < kBins; ++i) { bins_[i] = 0.0; phs_[i] = 0.0; seen_[i] = false; }
    }

    bool valid() const { return valid_; }
    double samplesPerSymbol() const { return sps_; }
    std::size_t eyeLength() const { return eyeLen_; }

    // Manual timing: the symbol instants are at offset + k * sps samples from the first sample ever pushed. Pass NaN to go back to auto.
    void setTimingOffsetSamples(double offset) {
        if (std::isfinite(offset)) {
            manual_.store(true);
            manualOffset_.store(std::fmod(std::fmod(offset, sps_) + sps_, sps_));
            retime_.store(true);
        } else {
            manual_.store(false);
        }
    }
    double timingOffsetSamples() const { return tau_.load(); }  // where the symbol clock is, in samples after a multiple of sps
    bool timingLocked() const { return locked_.load(); }
    std::uint64_t symbols() const { return nsym_.load(std::memory_order_acquire); }

    void push(const std::complex<float>* x, std::size_t n) {
        if (!valid_) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            float re = x[i].real(), im = x[i].imag();
            if (!std::isfinite(re) || !std::isfinite(im)) { re = im = 0.0f; }
            hist_[static_cast<std::size_t>(pos_) & hmask_] = std::complex<float>(re, im);
            const bool man = manual_.load();
            if (!man) { fold(static_cast<double>(re) * re + static_cast<double>(im) * im); }
            ++pos_;
            if (retime_.load()) {
                retime_.store(false);
                tau_.store(manualOffset_.load());
                nextT_ = firstInstantAtOrAfter(static_cast<double>(pos_), tau_.load());
            }
            // Emit every symbol whose window (one symbol either side, plus the interpolation taps) is complete.
            while (!std::isnan(nextT_) && static_cast<double>(pos_) >= nextT_ + sps_ + 3.0) {
                emit(nextT_);
                nextT_ += sps_;
            }
            if (!man && std::isnan(nextT_) && count_ >= 4.0 * sps_) { startClock(); }
        }
    }

    // Up to `maxOut` newest constellation points, oldest first.
    std::size_t constellation(std::complex<float>* out, std::size_t maxOut) const {
        for (int tries = 0; tries < 8; ++tries) {
            const std::uint64_t n = nsym_.load(std::memory_order_acquire);
            const std::size_t have = static_cast<std::size_t>(std::min<std::uint64_t>(n, cap_ - 1));
            const std::size_t take = std::min(have, maxOut);
            const std::uint64_t first = n - take;
            for (std::size_t i = 0; i < take; ++i) { out[i] = pts_[static_cast<std::size_t>((first + i) % cap_)]; }
            if (nsym_.load(std::memory_order_acquire) - first < cap_) { return take; }
        }
        return 0;
    }

    // Up to `maxTraces` newest eye traces, oldest first, into iOut / qOut of maxTraces * eyeLength() floats; returns how many.
    std::size_t eye(float* iOut, float* qOut, std::size_t maxTraces) const {
        for (int tries = 0; tries < 8; ++tries) {
            const std::uint64_t n = nsym_.load(std::memory_order_acquire);
            const std::size_t have = static_cast<std::size_t>(std::min<std::uint64_t>(n, traces_ - 1));
            const std::size_t take = std::min(have, maxTraces);
            const std::uint64_t first = n - take;
            for (std::size_t i = 0; i < take; ++i) {
                const std::size_t src = static_cast<std::size_t>((first + i) % traces_) * eyeLen_;
                std::copy(eyeI_.begin() + static_cast<long>(src), eyeI_.begin() + static_cast<long>(src + eyeLen_), iOut + i * eyeLen_);
                std::copy(eyeQ_.begin() + static_cast<long>(src), eyeQ_.begin() + static_cast<long>(src + eyeLen_), qOut + i * eyeLen_);
            }
            if (nsym_.load(std::memory_order_acquire) - first < traces_) { return take; }
        }
        return 0;
    }

    // Opening of the I rail (rail 0) or the Q rail (rail 1) at the centre of the eye; NaN when there are too few traces or only one sign.
    double eyeOpening(int rail) const {
        std::vector<float> i(traces_ * eyeLen_), q(traces_ * eyeLen_);
        const std::size_t n = eye(i.data(), q.data(), traces_);
        if (n < 8) { return std::numeric_limits<double>::quiet_NaN(); }
        const std::vector<float>& v = rail == 0 ? i : q;
        double posMin = 1e300, negMax = -1e300, sumAbs = 0.0;
        std::size_t np = 0, nn = 0;
        for (std::size_t t = 0; t < n; ++t) {
            const double c = v[t * eyeLen_ + perSym_];
            sumAbs += std::fabs(c);
            if (c >= 0) { posMin = std::min(posMin, c); ++np; } else { negMax = std::max(negMax, c); ++nn; }
        }
        if (np == 0 || nn == 0 || sumAbs <= 0.0) { return std::numeric_limits<double>::quiet_NaN(); }
        return (posMin - negMax) / (2.0 * sumAbs / static_cast<double>(n));
    }

private:
    std::complex<double> at(std::int64_t idx) const { return hist_[static_cast<std::size_t>(idx) & hmask_]; }

    std::complex<double> interp(double t) const {
        const std::int64_t i0 = static_cast<std::int64_t>(std::floor(t));
        const double mu = t - static_cast<double>(i0);
        const std::complex<double> a = at(i0 - 1), b = at(i0), c = at(i0 + 1), d = at(i0 + 2);
        // 4-point Lagrange.
        const double wa = -mu * (mu - 1.0) * (mu - 2.0) / 6.0;
        const double wb = (mu + 1.0) * (mu - 1.0) * (mu - 2.0) / 2.0;
        const double wc = -(mu + 1.0) * mu * (mu - 2.0) / 2.0;
        const double wd = (mu + 1.0) * mu * (mu - 1.0) / 6.0;
        return a * wa + b * wb + c * wc + d * wd;
    }

    void fold(double p) {
        count_ += 1.0;
        const double pp = std::fmod(static_cast<double>(pos_), sps_);  // phase of this sample within the symbol, samples
        int b = static_cast<int>(pp / sps_ * nb_);
        if (b >= nb_) { b = nb_ - 1; }
        if (!seen_[b]) {
            seen_[b] = true;
            bins_[b] = p;
            phs_[b] = pp;
        } else {
            bins_[b] += (p - bins_[b]) * (1.0 / 32.0);
            phs_[b] += (pp - phs_[b]) * (1.0 / 32.0);
        }
        // Re-estimate once per symbol period.
        if (++sinceEst_ >= static_cast<std::size_t>(sps_)) {
            sinceEst_ = 0;
            estimate();
        }
    }

    void estimate() {
        int best = -1;
        for (int b = 0; b < nb_; ++b) { if (seen_[b] && (best < 0 || bins_[b] > bins_[best])) { best = b; } }
        if (best < 0) { return; }
        const double a = bins_[(best + nb_ - 1) % nb_], m = bins_[best], c = bins_[(best + 1) % nb_];
        const double den = a - 2.0 * m + c;
        const double d = den < 0.0 ? std::min(std::max(0.5 * (a - c) / den, -0.5), 0.5) : 0.0;
        // The bin's own mean phase, plus the parabola's fraction of a bin (a bin is sps / nb samples wide).
        double est = phs_[best] + d * sps_ / nb_;
        est = std::fmod(est + sps_, sps_);
        estTau_ = est;
        if (std::isnan(nextT_)) { return; }
        // Slew the clock towards the estimate along the shortest way round.
        double diff = est - tau_.load();
        if (diff > 0.5 * sps_) { diff -= sps_; }
        if (diff < -0.5 * sps_) { diff += sps_; }
        const double step = std::min(std::max(diff, -0.02), 0.02);
        nextT_ += step;
        tau_.store(std::fmod(tau_.load() + step + sps_, sps_));
        locked_.store(std::fabs(diff) < 0.25 && nsym_.load() > 64);
    }

    void startClock() {
        tau_.store(estTau_);
        nextT_ = firstInstantAtOrAfter(static_cast<double>(pos_) - 2.0 * sps_ + 4.0, estTau_);
        // Do not emit into the past: the first symbol is the first whose window is complete.
        while (nextT_ < 2.0 * sps_ + 6.0 || nextT_ < static_cast<double>(pos_) - static_cast<double>(hist_.size()) + 2.0 * sps_ + 8.0) { nextT_ += sps_; }
    }

    double firstInstantAtOrAfter(double from, double tau) const {
        const double k = std::ceil((from - tau) / sps_);
        return tau + k * sps_;
    }

    void emit(double t) {
        const std::complex<double> p = interp(t);
        const std::uint64_t n = nsym_.load(std::memory_order_relaxed);
        pts_[static_cast<std::size_t>(n % cap_)] = std::complex<float>(static_cast<float>(p.real()), static_cast<float>(p.imag()));
        const std::size_t row = static_cast<std::size_t>(n % traces_) * eyeLen_;
        for (std::size_t j = 0; j < eyeLen_; ++j) {
            const double tt = t + (static_cast<double>(j) - static_cast<double>(perSym_)) / static_cast<double>(perSym_) * sps_;
            const std::complex<double> v = interp(tt);
            eyeI_[row + j] = static_cast<float>(v.real());
            eyeQ_[row + j] = static_cast<float>(v.imag());
        }
        nsym_.store(n + 1, std::memory_order_release);
    }

    double fs_, sps_;
    std::size_t cap_, traces_, perSym_, eyeLen_ = 0, hmask_ = 0;
    bool valid_ = false;
    std::vector<std::complex<float>> hist_, pts_;
    std::vector<float> eyeI_, eyeQ_;
    std::uint64_t pos_ = 0;
    double count_ = 0.0;
    double bins_[kBins], phs_[kBins];
    bool seen_[kBins];
    int nb_ = 2;
    std::size_t sinceEst_ = 0;
    double estTau_ = 0.0;
    double nextT_ = std::numeric_limits<double>::quiet_NaN();
    std::atomic<double> tau_{0.0}, manualOffset_{0.0};
    std::atomic<bool> manual_{false}, retime_{false}, locked_{false};
    std::atomic<std::uint64_t> nsym_{0};
};

}  // namespace cascade::dsp
