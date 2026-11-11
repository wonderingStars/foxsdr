// I/Q correction for a quadrature receiver: automatic DC removal, automatic gain and phase balance (image rejection), and an
// I/Q swap flag. Runs on the complex baseband at the SOURCE rate, in place.
//
// MODEL. A quadrature front end with gain error g and phase error phi turns a clean signal x into  y = a x + b conj(x)  (a image of
// every component appears at the mirrored frequency, with amplitude |b / a|); a DC offset d is added. Image rejection = |a|^2 / |b|^2.
//
// DC. A one-pole average of the samples (time constant dcTauS, default 0.5 s, faster for the first dcTauS so it settles at start) is
// subtracted. A signal at a few Hz from the centre is attenuated by the same high-pass (the cut-off is 1 / (2 pi tau) = 0.3 Hz).
//
// BALANCE: second-order statistics ("circularity"; the method is the textbook blind I/Q imbalance compensation for a signal whose
// positive and negative frequencies are statistically alike: Windisch and Fettweis, "Blind I/Q imbalance parameter estimation and
// compensation in low-IF receivers", ISCCSP 2004, and Anttila, Valkama and Renfors, "Circularity-based I/Q imbalance compensation
// in wideband direct-conversion receivers", IEEE Trans. Vehicular Technology 57(4), 2008 (both from memory, UNVERIFIED against the
// papers; the maths below was derived and is checked by the tests). With the exponentially averaged pseudo-covariance Pp = E[y^2]
// and power Pw = E[|y|^2] (time constant balanceTauS, default 1 s) let rho = Pp / Pw. A signal that is circular before the
// imbalance has E[x^2] = 0, so E[y^2] = 2 a b E|x|^2 and E|y|^2 = (|a|^2 + |b|^2) E|x|^2. The output  z = k (y - gamma conj(y))
// with gamma = (1 - sqrt(1 - |rho|^2)) / conj(rho) has E[z^2] = 0 exactly (the image is gone to the accuracy of the estimate) and k
// restores the power. |gamma| is limited to 0.3 (about 10 dB of image to start from): beyond that the signal is not circular
// (BPSK, DSB-SC, a lone real tone) and the correction is held at the limit instead of eating the signal. LIMIT OF THE METHOD: the wanted signal must be statistically alike either side of the centre before the imbalance (noise, many
// signals, a band): then everything in E[y^2] is the imbalance. A signal that is itself improper (BPSK, DSB-SC, one real tone) is
// not, and the estimator reads its structure as an imbalance (held at the 0.3 limit).
//
// SWAP: the I/Q swap flag exchanges I and Q ( z = Q + jI = j conj(x) ): the spectrum is mirrored. It is applied FIRST (it
// repairs a wiring mistake) and crossfaded over 2 ms.
//
// ZERO COST WHEN OFF: all three off (and settled) -> process() returns at once, samples untouched. Switching DC or balance on or off
// ramps the correction in or out over 20 ms (the estimators run only while enabled and restart fast). No allocation. Non-finite
// samples read as 0. Any chunking gives the same output. THREADS: setters any thread (atomics), process() on the DSP thread.
// Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>

namespace cascade::dsp {

class IqCorrector {
public:
    explicit IqCorrector(double sampleRateHz = 2400000.0) : fs_(sampleRateHz > 1000.0 ? sampleRateHz : 2400000.0) {
        rampStep_ = 1.0 / (0.020 * fs_);
        swapStep_ = 1.0 / (0.002 * fs_);
    }

    void setDcRemoval(bool on) { dcOn_.store(on); }
    void setBalance(bool on) { balOn_.store(on); }
    void setSwap(bool on) { swap_.store(on); }
    void setDcTauS(double s) { put(dcTau_, s, 0.01, 30.0); }
    void setBalanceTauS(double s) { put(balTau_, s, 0.01, 60.0); }

    // What the estimators hold (for a display).
    std::complex<double> dcEstimate() const { return {dcRe_.load(), dcIm_.load()}; }
    // Estimated image rejection of the INPUT, dB (large when balanced); 200 when nothing has been measured.
    double imageRejectionDb() const { return irr_.load(); }
    std::complex<double> gammaEstimate() const { return {gRe_.load(), gIm_.load()}; }

    bool isBypassed() const { return !dcOn_.load() && !balOn_.load() && !swap_.load() && sDc_ == 0.0 && sBal_ == 0.0 && swapMix_ == 0.0; }

    void process(std::complex<float>* x, std::size_t n) {
        const bool dcWant = dcOn_.load(), balWant = balOn_.load(), swapWant = swap_.load();
        if (!dcWant && !balWant && !swapWant && sDc_ == 0.0 && sBal_ == 0.0 && swapMix_ == 0.0) {
            dcCount_ = balCount_ = 0;  // the next enable starts fast
            return;
        }
        const double aDc = 1.0 / (dcTau_.load() * fs_), aBal = 1.0 / (balTau_.load() * fs_);
        for (std::size_t i = 0; i < n; ++i) {
            float re = x[i].real(), im = x[i].imag();
            if (!std::isfinite(re) || !std::isfinite(im)) { re = im = 0.0f; }
            std::complex<double> v(re, im);
            // Swap, crossfaded.
            if (swapWant) { swapMix_ = std::min(1.0, swapMix_ + swapStep_); } else { swapMix_ = std::max(0.0, swapMix_ - swapStep_); }
            if (swapMix_ > 0.0) {
                const std::complex<double> sw(v.imag(), v.real());
                v = v + (sw - v) * swapMix_;
            }
            // DC.
            if (dcWant) { sDc_ = std::min(1.0, sDc_ + rampStep_); } else { sDc_ = std::max(0.0, sDc_ - rampStep_); }
            if (dcWant || sDc_ > 0.0) {
                if (dcWant) {
                    ++dcCount_;
                    const double a = std::max(aDc, 1.0 / static_cast<double>(dcCount_));
                    dc_ += a * (v - dc_);
                }
                v -= sDc_ * dc_;
            }
            // Balance.
            if (balWant) { sBal_ = std::min(1.0, sBal_ + rampStep_); } else { sBal_ = std::max(0.0, sBal_ - rampStep_); }
            if (balWant || sBal_ > 0.0) {
                if (balWant) {
                    ++balCount_;
                    const double a = std::max(aBal, 1.0 / static_cast<double>(balCount_));
                    pp_ += a * (v * v - pp_);
                    pw_ += a * (std::norm(v) - pw_);
                    if (++sub_ >= 256) {
                        sub_ = 0;
                        updateGamma();
                    }
                }
                const std::complex<double> z = (v - sBal_ * gamma_ * std::conj(v)) * (1.0 + sBal_ * (k_ - 1.0));
                v = z;
            }
            x[i] = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        dcRe_.store(dc_.real());
        dcIm_.store(dc_.imag());
        gRe_.store(gamma_.real());
        gIm_.store(gamma_.imag());
    }

    void reset() {
        dc_ = pp_ = gamma_ = 0.0;
        pw_ = 0.0;
        k_ = 1.0;
        dcCount_ = balCount_ = 0;
        sub_ = 0;
        irr_.store(200.0);
    }

private:
    static void put(std::atomic<double>& a, double v, double lo, double hi) {
        if (std::isfinite(v)) { a.store(std::min(std::max(v, lo), hi)); }
    }

    void updateGamma() {
        if (!(pw_ > 1e-30)) { gamma_ = 0.0; k_ = 1.0; return; }
        std::complex<double> rho = pp_ / pw_;
        double m = std::abs(rho);
        if (m > 0.99) { rho *= 0.99 / m; m = 0.99; }
        std::complex<double> g = 0.0;
        if (m > 1e-12) { g = (1.0 - std::sqrt(1.0 - m * m)) / std::conj(rho); }
        const double gm = std::abs(g);
        if (gm > 0.3) { g *= 0.3 / gm; }
        gamma_ = g;
        const double pz = 1.0 + std::norm(g) - 2.0 * (std::conj(g) * rho).real();
        k_ = pz > 1e-9 ? 1.0 / std::sqrt(pz) : 1.0;
        // Image-to-signal amplitude ratio of the input: |b / a| from  rho = 2ab / (|a|^2 + |b|^2)  ->  gamma = b* / a*  (to first order).
        const double r = std::abs(g);
        irr_.store(r > 1e-9 ? 20.0 * std::log10(1.0 / r) : 200.0);
    }

    double fs_, rampStep_, swapStep_;
    std::atomic<bool> dcOn_{false}, balOn_{false}, swap_{false};
    std::atomic<double> dcTau_{0.5}, balTau_{1.0}, dcRe_{0}, dcIm_{0}, irr_{200.0}, gRe_{0}, gIm_{0};
    std::complex<double> dc_{0, 0}, pp_{0, 0}, gamma_{0, 0};
    double pw_ = 0.0, k_ = 1.0, sDc_ = 0.0, sBal_ = 0.0, swapMix_ = 0.0;
    std::size_t dcCount_ = 0, balCount_ = 0;
    int sub_ = 0;
};

}  // namespace cascade::dsp
