// Two-channel combiner for two PHASE-COHERENT complex streams: manual weight,
// maximum-ratio combining and automatic null steering.
//
// Method and sources (opened; no SDR program's source was read):
//  * Minimum-variance distortionless response (Capon) beamformer: minimise the
//    output power w^H R w subject to w^H v = 1, solution
//        w = R^-1 v / (v^H R^-1 v)
//    S. Loyka, ELG5132 Smart Antennas, Lecture 7 "Optimum Beamforming: Basic
//    Concepts" (Univ. of Ottawa, 26-Jan-13), slides 5-6, equations (7.8) to
//    (7.14); https://www.site.uottawa.ca/~sloyka/elg5132/Lec_7_ELG5132.pdf
//    (which cites H.L. Van Trees, Optimum Array Processing, Wiley 2002). The
//    same slides note that for R = sigma^2 I the weight reduces to w ~ v, the
//    matched filter, which is maximum-ratio combining.
//  * With N = 2 every matrix is 2x2, so R^-1 = adj(R)/det(R) is written out
//    and the dominant eigenvector of the Hermitian R is closed form.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

namespace cascade::dsp {

// 2x2 Hermitian covariance R = E[x x^H]: r00 = E|x0|^2, r11 = E|x1|^2,
// r01 = E[x0 conj(x1)]  (r10 = conj(r01)).
struct Cov2 {
    double r00 = 0.0, r11 = 0.0;
    std::complex<double> r01{0.0, 0.0};
};

// y = g0 x0 + g1 x1.
struct Gains2 {
    std::complex<double> g0{1.0, 0.0}, g1{0.0, 0.0};
};

namespace combiner {

// Dominant eigenvector of R scaled so its first entry is 1: the steering ratio
// rho (x1 = rho x0) of the strongest single source. Returns rho = 0 when the
// covariance is empty. Closed form: lambda = (a+d)/2 + sqrt(((a-d)/2)^2 + |b|^2),
// e = [b, lambda - a].
std::complex<double> dominantSteering(const Cov2& r);

// Maximum-ratio combining for a wanted signal with steering [1, rho] in equal
// noise on both channels: y = (x0 + conj(rho) x1) / (1 + |rho|^2). The wanted
// signal comes out at the same amplitude and phase as it has on channel 0.
Gains2 maxRatio(std::complex<double> rho);

// MVDR / Capon weights for steering [1, rho] and covariance r (diagonal loading
// `loading` x mean diagonal added first, which also keeps a rank-1 covariance
// invertible). The wanted signal passes with unit gain (referred to channel 0);
// the output power of everything else in r is minimised. y = conj(w)^T x.
Gains2 capon(const Cov2& r, std::complex<double> rho, double loading);

// Reference-constrained power minimisation, needs NO knowledge of the wanted
// signal: y = x0 - c x1 with c = r01 / r11 minimises E|y|^2 (the sidelobe-
// canceller / Wiener solution). It nulls whatever dominates the covariance, so
// it is right only when the interferer is much stronger than the wanted signal.
Gains2 referenceNull(const Cov2& r);

}  // namespace combiner

class TwoChannelCombiner {
public:
    enum class Mode {
        Manual,        // y = x0 + amp e^{j phase} x1
        MaxRatio,      // steering from setSteering(), or the dominant eigenvector (auto)
        Capon,         // steering from setSteering(); covariance adapted from the data
        ReferenceNull  // blind; covariance adapted from the data
    };

    TwoChannelCombiner() { setForgetting(4096.0); }

    void setMode(Mode m) noexcept { mode_ = m; if (m == Mode::Manual) { g_ = manual_; } }
    Mode mode() const noexcept { return mode_; }

    // Manual weight on channel 1 relative to channel 0 (amplitude, radians).
    void setManual(double amplitude, double phaseRad);
    // Wanted-signal steering ratio rho = (wanted on ch1) / (wanted on ch0).
    // Without it MaxRatio and Capon use the dominant eigenvector of the adapted
    // covariance.
    void setSteering(std::complex<double> rho) noexcept { rho_ = rho; steeringSet_ = true; }
    void clearSteering() noexcept { steeringSet_ = false; }

    // Covariance forgetting: exponential, time constant in samples (>= 16).
    void setForgetting(double timeConstantSamples);
    // Weights are re-solved every `n` samples (default 64).
    void setUpdateInterval(std::size_t n) noexcept { interval_ = n < 1 ? 1 : n; }
    // Diagonal loading for Capon, fraction of mean diagonal (default 1e-4).
    void setLoading(double f) noexcept { loading_ = f < 0.0 ? 0.0 : f; }
    // Stop / resume adapting the weights.
    void freeze() noexcept { frozen_ = true; }
    void unfreeze() noexcept { frozen_ = false; }

    // Solves the weights once from an externally measured covariance (a capture
    // of the interferer alone, say) and freezes. The mode decides the solver.
    void solveFrom(const Cov2& r);

    // y[i] = g0 x0[i] + g1 x1[i]; the streams must be sample-aligned and
    // phase-coherent. In the adaptive modes the covariance is updated per
    // sample and the weights re-solved every updateInterval samples.
    void process(const std::complex<float>* x0, const std::complex<float>* x1,
                 std::size_t n, std::complex<float>* y);

    const Cov2& covariance() const noexcept { return r_; }
    Gains2 gains() const noexcept { return g_; }
    void reset();

private:
    void solve();

    Mode mode_ = Mode::Manual;
    Gains2 g_{};
    Gains2 manual_{};
    std::complex<double> rho_{1.0, 0.0};
    bool steeringSet_ = false;
    double lambda_ = 0.0;
    std::size_t interval_ = 64, sinceSolve_ = 0, seen_ = 0;
    double loading_ = 1e-4;
    bool frozen_ = false;
    Cov2 r_{};
};

}  // namespace cascade::dsp
