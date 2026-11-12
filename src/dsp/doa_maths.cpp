// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/doa_maths.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace cascade::dsp::doa {

namespace {
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kTwoPi = 2.0 * kPi;
}  // namespace

Matrix sampleCovariance(const std::complex<float>* const* channels, int numChannels, std::size_t count) {
    if (numChannels < 1 || numChannels > kMaxChannels || count == 0 || channels == nullptr) { return Matrix(); }
    Matrix r(numChannels);
    for (int i = 0; i < numChannels; ++i) {
        for (int j = i; j < numChannels; ++j) {
            cd acc(0.0, 0.0);
            const std::complex<float>* a = channels[i];
            const std::complex<float>* b = channels[j];
            for (std::size_t t = 0; t < count; ++t) {
                acc += cd(a[t].real(), a[t].imag()) * std::conj(cd(b[t].real(), b[t].imag()));
            }
            acc /= static_cast<double>(count);
            r.at(i, j) = acc;
            r.at(j, i) = std::conj(acc);
        }
    }
    return r;
}

Eigen hermitianEigen(const Matrix& h) {
    Eigen out;
    const int n = h.n;
    if (n < 1) { out.converged = true; return out; }
    Matrix a(n), e(n);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) { a.at(i, j) = 0.5 * (h.at(i, j) + std::conj(h.at(j, i))); }
        e.at(i, i) = 1.0;
    }
    for (int i = 0; i < n; ++i) { a.at(i, i) = cd(a.at(i, i).real(), 0.0); }

    auto offNorm = [&]() {
        double s = 0.0;
        for (int i = 0; i < n; ++i) { for (int j = i + 1; j < n; ++j) { s += std::norm(a.at(i, j)); } }
        return std::sqrt(s);
    };
    double total = 0.0;
    for (int i = 0; i < n; ++i) { for (int j = 0; j < n; ++j) { total += std::norm(a.at(i, j)); } }
    total = std::sqrt(total);
    const double tol = 1e-15 * std::max(total, 1e-300);

    int sweep = 0;
    for (; sweep < 60; ++sweep) {
        if (offNorm() <= tol) { out.converged = true; break; }
        for (int p = 0; p < n - 1; ++p) {
            for (int q = p + 1; q < n; ++q) {
                const cd g = a.at(p, q);
                const double ag = std::abs(g);
                if (ag <= 1e-300) { continue; }
                const cd ph = g / ag;           // e^{j phi}
                const cd phc = std::conj(ph);   // e^{-j phi}
                const double theta = (a.at(q, q).real() - a.at(p, p).real()) / (2.0 * ag);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                // U = V R with V = diag(1, e^{-j phi}) on (p, q), R = [[c, s], [-s, c]]:
                // U_pp = c, U_pq = s, U_qp = -s e^{-j phi}, U_qq = c e^{-j phi}.
                const cd upp = c, upq = s, uqp = -s * phc, uqq = c * phc;
                // columns: A <- A U, E <- E U
                for (int k = 0; k < n; ++k) {
                    const cd akp = a.at(k, p), akq = a.at(k, q);
                    a.at(k, p) = akp * upp + akq * uqp;
                    a.at(k, q) = akp * upq + akq * uqq;
                    const cd ekp = e.at(k, p), ekq = e.at(k, q);
                    e.at(k, p) = ekp * upp + ekq * uqp;
                    e.at(k, q) = ekp * upq + ekq * uqq;
                }
                // rows: A <- U^H A
                for (int k = 0; k < n; ++k) {
                    const cd apk = a.at(p, k), aqk = a.at(q, k);
                    a.at(p, k) = std::conj(upp) * apk + std::conj(uqp) * aqk;
                    a.at(q, k) = std::conj(upq) * apk + std::conj(uqq) * aqk;
                }
                a.at(p, q) = 0.0;
                a.at(q, p) = 0.0;
            }
        }
    }
    if (!out.converged && offNorm() <= tol * 10.0) { out.converged = true; }
    out.sweeps = sweep;

    std::vector<int> idx(static_cast<std::size_t>(n));
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int x, int y) { return a.at(x, x).real() > a.at(y, y).real(); });
    out.values.resize(static_cast<std::size_t>(n));
    out.vectors = Matrix(n);
    for (int k = 0; k < n; ++k) {
        out.values[static_cast<std::size_t>(k)] = a.at(idx[static_cast<std::size_t>(k)], idx[static_cast<std::size_t>(k)]).real();
        for (int r = 0; r < n; ++r) { out.vectors.at(r, k) = e.at(r, idx[static_cast<std::size_t>(k)]); }
    }
    return out;
}

std::vector<cd> steering(const Geometry& g, double theta) {
    std::vector<cd> v(static_cast<std::size_t>(std::max(g.elements, 0)));
    const double lambda = g.frequencyHz > 0.0 ? kSpeedOfLight / g.frequencyHz : 1.0;
    const double k = kTwoPi / lambda;
    for (int m = 0; m < g.elements; ++m) {
        double phase;
        if (g.type == ArrayType::Circular) {
            const double phi = kTwoPi * m / g.elements;
            const double x = g.sizeMetres * std::cos(phi), y = g.sizeMetres * std::sin(phi);
            phase = k * (x * std::cos(theta) + y * std::sin(theta));
        } else {
            phase = k * m * g.sizeMetres * std::cos(theta);
        }
        v[static_cast<std::size_t>(m)] = std::polar(1.0, phase);
    }
    return v;
}

std::vector<double> bearingGrid(const Geometry& g, double step) {
    std::vector<double> grid;
    if (!(step > 0.0)) { return grid; }
    const double span = g.type == ArrayType::Circular ? kTwoPi : kPi;
    const std::size_t n = static_cast<std::size_t>(std::floor(span / step + 1e-9)) +
                          (g.type == ArrayType::Linear ? 1 : 0);
    grid.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { grid.push_back(static_cast<double>(i) * step); }
    return grid;
}

std::vector<double> bartlettSpectrum(const Matrix& r, const Geometry& g, const std::vector<double>& grid) {
    std::vector<double> out(grid.size(), 0.0);
    const int n = r.n;
    if (n != g.elements || n < 1) { return out; }
    for (std::size_t i = 0; i < grid.size(); ++i) {
        const std::vector<cd> a = steering(g, grid[i]);
        cd acc(0.0, 0.0);
        for (int p = 0; p < n; ++p) {
            cd row(0.0, 0.0);
            for (int q = 0; q < n; ++q) { row += r.at(p, q) * a[static_cast<std::size_t>(q)]; }
            acc += std::conj(a[static_cast<std::size_t>(p)]) * row;
        }
        out[i] = acc.real() / (static_cast<double>(n) * n);
    }
    return out;
}

std::vector<double> musicSpectrum(const Eigen& eig, const Geometry& g, const std::vector<double>& grid, int sources) {
    std::vector<double> out(grid.size(), 0.0);
    const int n = eig.vectors.n;
    if (n != g.elements || n < 2 || sources < 1 || sources >= n) { return out; }
    for (std::size_t i = 0; i < grid.size(); ++i) {
        const std::vector<cd> a = steering(g, grid[i]);
        double denom = 0.0;
        for (int k = sources; k < n; ++k) {
            cd proj(0.0, 0.0);
            for (int m = 0; m < n; ++m) { proj += std::conj(eig.vectors.at(m, k)) * a[static_cast<std::size_t>(m)]; }
            denom += std::norm(proj);
        }
        denom /= static_cast<double>(n);  // |a|^2 = N: normalise a to unit length
        out[i] = 1.0 / std::max(denom, 1e-300);
    }
    return out;
}

std::vector<Peak> pickPeaks(const std::vector<double>& s, const std::vector<double>& grid, bool wrap, int count) {
    std::vector<Peak> peaks;
    const std::size_t n = s.size();
    if (n < 3 || grid.size() != n || count < 1) { return peaks; }
    std::vector<double> sorted(s);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n / 2), sorted.end());
    const double median = std::max(sorted[n / 2], 1e-300);
    const double step = grid[1] - grid[0];
    struct Cand { std::size_t i; double h; };
    std::vector<Cand> cands;
    for (std::size_t i = 0; i < n; ++i) {
        const bool hasPrev = wrap || i > 0, hasNext = wrap || i + 1 < n;
        const double prev = hasPrev ? s[(i + n - 1) % n] : -1e300;
        const double next = hasNext ? s[(i + 1) % n] : -1e300;
        if (s[i] > prev && s[i] >= next) { cands.push_back({i, s[i]}); }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.h > b.h; });
    for (const Cand& c : cands) {
        if (static_cast<int>(peaks.size()) >= count) { break; }
        Peak p;
        p.bearing = grid[c.i];
        p.height = c.h;
        const bool interior = wrap || (c.i > 0 && c.i + 1 < n);
        if (interior) {
            const double y0 = s[(c.i + n - 1) % n], y1 = s[c.i], y2 = s[(c.i + 1) % n];
            const double d = y0 - 2.0 * y1 + y2;
            if (d < 0.0) {
                const double off = 0.5 * (y0 - y2) / d;  // in grid steps, within (-1, 1)
                p.bearing += off * step;
                p.height = y1 - 0.25 * (y0 - y2) * off;
            }
        }
        p.peakToMedianDb = 10.0 * std::log10(std::max(p.height, 1e-300) / median);
        p.confidence = 1.0 - std::pow(10.0, -std::max(p.peakToMedianDb, 0.0) / 20.0);
        if (wrap) {
            p.bearing = std::fmod(p.bearing, kTwoPi);
            if (p.bearing < 0.0) { p.bearing += kTwoPi; }
        }
        peaks.push_back(p);
    }
    return peaks;
}

int countAboveNoise(const Eigen& eig, double factor) {
    if (eig.values.empty()) { return 0; }
    const double floorV = std::max(eig.values.back(), 1e-300);
    int c = 0;
    for (double v : eig.values) { if (v > factor * floorV) { ++c; } }
    return c;
}

Calibration calibrateFromCommonSource(const Matrix& r) {
    Calibration cal;
    const int n = r.n;
    if (n < 2) { return cal; }
    const Eigen e = hermitianEigen(r);
    const double e0 = std::abs(e.vectors.at(0, 0));
    if (!(e0 > 1e-12)) { return cal; }
    double rest = 0.0;
    for (std::size_t k = 1; k < e.values.size(); ++k) { rest += std::max(e.values[k], 0.0); }
    cal.dominance = e.values[0] / std::max(rest, 1e-300);
    cal.factor.assign(static_cast<std::size_t>(n), cd(1.0, 0.0));
    cal.phaseDeg.assign(static_cast<std::size_t>(n), 0.0);
    cal.gainDb.assign(static_cast<std::size_t>(n), 0.0);
    const cd ref = e.vectors.at(0, 0);
    for (int i = 0; i < n; ++i) {
        const cd ei = e.vectors.at(i, 0);
        if (std::abs(ei) < 1e-12) { return Calibration(); }
        const cd k = ref / ei;  // x_i = c_i s, e_i ~ c_i  =>  k_i x_i ~ c_0 s
        cal.factor[static_cast<std::size_t>(i)] = k;
        cal.phaseDeg[static_cast<std::size_t>(i)] = std::arg(k) * 180.0 / kPi;
        cal.gainDb[static_cast<std::size_t>(i)] = 20.0 * std::log10(std::abs(k));
    }
    cal.ok = true;
    return cal;
}

Matrix applyCalibration(const Matrix& r, const std::vector<cd>& factor) {
    Matrix o(r.n);
    if (static_cast<int>(factor.size()) != r.n) { return r; }
    for (int i = 0; i < r.n; ++i) {
        for (int j = 0; j < r.n; ++j) {
            o.at(i, j) = factor[static_cast<std::size_t>(i)] * r.at(i, j) * std::conj(factor[static_cast<std::size_t>(j)]);
        }
    }
    return o;
}

}  // namespace cascade::dsp::doa
