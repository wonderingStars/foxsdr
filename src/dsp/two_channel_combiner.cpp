// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/two_channel_combiner.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::dsp {

namespace combiner {

std::complex<double> dominantSteering(const Cov2& r) {
    const double a = r.r00, d = r.r11;
    const double b2 = std::norm(r.r01);
    if (a + d <= 0.0) { return {0.0, 0.0}; }
    if (b2 <= 1e-30 * (a + d) * (a + d)) {
        // No cross term: the stronger channel alone.
        return a >= d ? std::complex<double>(0.0, 0.0) : std::complex<double>(1e9, 0.0);
    }
    const double lam = 0.5 * (a + d) + std::sqrt(0.25 * (a - d) * (a - d) + b2);
    return (lam - a) / r.r01;  // e = [b, lam - a], normalised to e0 = 1
}

Gains2 maxRatio(std::complex<double> rho) {
    const double n = 1.0 + std::norm(rho);
    Gains2 g;
    g.g0 = {1.0 / n, 0.0};
    g.g1 = std::conj(rho) / n;
    return g;
}

Gains2 capon(const Cov2& r, std::complex<double> rho, double loading) {
    const double load = loading * 0.5 * (r.r00 + r.r11);
    const double a = r.r00 + load, d = r.r11 + load;
    const std::complex<double> b = r.r01;
    const double det = a * d - std::norm(b);
    if (!(det > 0.0)) { return maxRatio(rho); }
    // R^-1 = [[d, -b], [-conj(b), a]] / det ; u = R^-1 v with v = [1, rho]
    const std::complex<double> u0 = (d - b * rho) / det;
    const std::complex<double> u1 = (-std::conj(b) + a * rho) / det;
    // v^H u
    const std::complex<double> vhu = u0 + std::conj(rho) * u1;
    if (std::abs(vhu) < 1e-300) { return maxRatio(rho); }
    Gains2 g;
    g.g0 = std::conj(u0 / vhu);  // y = w^H x, g = conj(w)
    g.g1 = std::conj(u1 / vhu);
    return g;
}

Gains2 referenceNull(const Cov2& r) {
    Gains2 g;
    if (r.r11 > 0.0) { g.g1 = -r.r01 / r.r11; }
    return g;
}

}  // namespace combiner

void TwoChannelCombiner::setManual(double amplitude, double phaseRad) {
    manual_.g0 = {1.0, 0.0};
    manual_.g1 = std::polar(amplitude, phaseRad);
    if (mode_ == Mode::Manual) { g_ = manual_; }
}

void TwoChannelCombiner::setForgetting(double tc) {
    tc = std::max(16.0, tc);
    lambda_ = std::exp(-1.0 / tc);
}

void TwoChannelCombiner::reset() {
    r_ = Cov2{};
    sinceSolve_ = seen_ = 0;
    g_ = (mode_ == Mode::Manual) ? manual_ : Gains2{};
}

void TwoChannelCombiner::solve() {
    std::complex<double> rho = rho_;
    switch (mode_) {
        case Mode::Manual: g_ = manual_; return;
        case Mode::MaxRatio:
            if (!steeringSet_) { rho = combiner::dominantSteering(r_); }
            g_ = combiner::maxRatio(rho);
            return;
        case Mode::Capon:
            if (!steeringSet_) { rho = combiner::dominantSteering(r_); }
            g_ = combiner::capon(r_, rho, loading_);
            return;
        case Mode::ReferenceNull: g_ = combiner::referenceNull(r_); return;
    }
}

void TwoChannelCombiner::solveFrom(const Cov2& r) {
    r_ = r;
    solve();
    frozen_ = true;
}

void TwoChannelCombiner::process(const std::complex<float>* x0, const std::complex<float>* x1,
                                 std::size_t n, std::complex<float>* y) {
    const bool adaptive = mode_ != Mode::Manual && !frozen_;
    const double lam = lambda_, one = 1.0 - lambda_;
    for (std::size_t i = 0; i < n; ++i) {
        const std::complex<double> a(x0[i].real(), x0[i].imag());
        const std::complex<double> b(x1[i].real(), x1[i].imag());
        if (adaptive) {
            r_.r00 = lam * r_.r00 + one * std::norm(a);
            r_.r11 = lam * r_.r11 + one * std::norm(b);
            r_.r01 = lam * r_.r01 + one * a * std::conj(b);
            ++seen_;
            if (++sinceSolve_ >= interval_) {
                sinceSolve_ = 0;
                if (seen_ >= 32) { solve(); }
            }
        }
        const std::complex<double> o = g_.g0 * a + g_.g1 * b;
        y[i] = std::complex<float>(static_cast<float>(o.real()), static_cast<float>(o.imag()));
    }
}

}  // namespace cascade::dsp
