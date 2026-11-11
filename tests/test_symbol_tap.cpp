// Constellation and eye-diagram tap (src/dsp/symbol_tap.hpp), tested on generated BPSK / QPSK / 16-QAM streams with root-raised-cosine
// (beta 0.35) transmit and matched receive filters, a known fractional timing offset, optional noise, and integer and non-integer
// samples per symbol. Nothing here is a recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <complex>
#include <cstdio>
#include <limits>
#include <random>
#include <thread>

#include "alloc_probe.hpp"
#include "dsp/symbol_tap.hpp"
#include "test_check.hpp"

using cascade::dsp::SymbolTap;
using cd = std::complex<double>;
using cf = std::complex<float>;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Root-raised-cosine impulse response at time t (symbol periods), roll-off beta, unit symbol period.
double rrc(double t, double beta) {
    if (std::fabs(t) < 1e-9) { return 1.0 - beta + 4.0 * beta / kPi; }
    if (std::fabs(std::fabs(4.0 * beta * t) - 1.0) < 1e-9) {
        return beta / std::sqrt(2.0) * ((1.0 + 2.0 / kPi) * std::sin(kPi / (4.0 * beta)) + (1.0 - 2.0 / kPi) * std::cos(kPi / (4.0 * beta)));
    }
    return (std::sin(kPi * t * (1.0 - beta)) + 4.0 * beta * t * std::cos(kPi * t * (1.0 + beta))) / (kPi * t * (1.0 - std::pow(4.0 * beta * t, 2.0)));
}

enum class Mod { Bpsk, Qpsk, Qam16 };

cd symbolFor(Mod m, std::mt19937& g) {
    std::uniform_int_distribution<int> d(0, 15);
    const int v = d(g);
    switch (m) {
        case Mod::Bpsk: return cd((v & 1) ? 1.0 : -1.0, 0.0);
        case Mod::Qpsk: return cd((v & 1) ? 1.0 : -1.0, (v & 2) ? 1.0 : -1.0) / std::sqrt(2.0);
        case Mod::Qam16: {
            static const double lv[4] = {-3, -1, 1, 3};
            return cd(lv[v & 3], lv[(v >> 2) & 3]) / std::sqrt(10.0);
        }
    }
    return 0.0;
}

// nSym symbols at sps samples per symbol, symbol instants at tau0 + k sps, matched-filtered, noise sigma per component.
std::vector<cf> makeStream(Mod m, double sps, double tau0, std::size_t nSym, double sigma, std::uint32_t seed) {
    std::mt19937 g(seed);
    std::vector<cd> sym(nSym);
    for (auto& s : sym) { s = symbolFor(m, g); }
    const std::size_t n = static_cast<std::size_t>(sps * static_cast<double>(nSym));
    const int span = 8;  // symbols each side for the transmit pulse
    std::vector<cd> tx(n, cd(0, 0));
    for (std::size_t k = 0; k < nSym; ++k) {
        const double tk = tau0 + static_cast<double>(k) * sps;
        const long lo = static_cast<long>(std::floor(tk - span * sps)), hi = static_cast<long>(std::ceil(tk + span * sps));
        for (long i = std::max<long>(lo, 0); i <= hi && i < static_cast<long>(n); ++i) { tx[static_cast<std::size_t>(i)] += sym[k] * rrc((static_cast<double>(i) - tk) / sps, 0.35); }
    }
    // Matched filter: the same pulse sampled at integer n, symmetric, normalised so the overall pulse peaks at 1.
    const long half = static_cast<long>(span * sps);
    std::vector<double> h(static_cast<std::size_t>(2 * half + 1));
    double e = 0;
    for (long i = -half; i <= half; ++i) { h[static_cast<std::size_t>(i + half)] = rrc(static_cast<double>(i) / sps, 0.35); e += h[static_cast<std::size_t>(i + half)] * h[static_cast<std::size_t>(i + half)]; }
    for (auto& v : h) { v /= e; }  // sum h^2 = 1 gives unit gain at the optimum instant for the RRC pair sampled at sps samples/symbol (scaled below)
    std::mt19937 gn(seed + 99);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<cd> noisy(n);
    for (std::size_t i = 0; i < n; ++i) { noisy[i] = tx[i] + cd(sigma * nd(gn), sigma * nd(gn)); }
    std::vector<cf> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        cd s = 0;
        for (long j = -half; j <= half; ++j) {
            const long idx = static_cast<long>(i) + j;
            if (idx >= 0 && idx < static_cast<long>(n)) { s += noisy[static_cast<std::size_t>(idx)] * h[static_cast<std::size_t>(j + half)]; }
        }
        out[i] = cf(static_cast<float>(s.real()), static_cast<float>(s.imag()));
    }
    return out;
}

// EVM (rms error to the nearest ideal point over the rms of the ideal points), after fitting the overall amplitude.
double evmPercent(const std::vector<cf>& pts, Mod m) {
    std::vector<cd> ideal;
    switch (m) {
        case Mod::Bpsk: ideal = {cd(-1, 0), cd(1, 0)}; break;
        case Mod::Qpsk: for (int a : {-1, 1}) for (int b : {-1, 1}) ideal.push_back(cd(a, b) / std::sqrt(2.0)); break;
        case Mod::Qam16: for (double a : {-3.0, -1.0, 1.0, 3.0}) for (double b : {-3.0, -1.0, 1.0, 3.0}) ideal.push_back(cd(a, b) / std::sqrt(10.0)); break;
    }
    double pw = 0;
    for (auto p : pts) { pw += std::norm(cd(p.real(), p.imag())); }
    pw /= static_cast<double>(pts.size());
    const double scale = 1.0 / std::sqrt(pw);  // every ideal set above has unit mean power
    double err = 0;
    for (auto p : pts) {
        const cd q = cd(p.real(), p.imag()) * scale;
        double best = 1e30;
        for (auto id : ideal) { best = std::min(best, std::norm(q - id)); }
        err += best;
    }
    return 100.0 * std::sqrt(err / static_cast<double>(pts.size()));
}

double circDiff(double a, double b, double period) {
    double d = std::fmod(a - b + 1.5 * period, period) - 0.5 * period;
    return d;
}

struct TapResult {
    std::vector<cf> pts;
    double tau = 0;
    bool locked = false;
    double eyeI = 0, eyeQ = 0;
    std::uint64_t symbols = 0;
};

TapResult feed(SymbolTap& tap, const std::vector<cf>& x, std::size_t chunk = 4096) {
    for (std::size_t p = 0; p < x.size(); p += chunk) { tap.push(x.data() + p, std::min(chunk, x.size() - p)); }
    TapResult r;
    std::vector<cf> all(512);
    const std::size_t n = tap.constellation(all.data(), all.size());
    r.pts.assign(all.begin(), all.begin() + static_cast<long>(n));
    r.tau = tap.timingOffsetSamples();
    r.locked = tap.timingLocked();
    r.eyeI = tap.eyeOpening(0);
    r.eyeQ = tap.eyeOpening(1);
    r.symbols = tap.symbols();
    return r;
}

void testTiming() {
    struct Row { Mod m; const char* name; double sps; double tau0; };
    const Row rows[] = {
        {Mod::Qpsk, "QPSK", 8.0, 0.0}, {Mod::Qpsk, "QPSK", 8.0, 2.6}, {Mod::Qpsk, "QPSK", 8.0, 5.3}, {Mod::Qpsk, "QPSK", 8.0, 7.9},
        {Mod::Qpsk, "QPSK", 9.6, 3.3}, {Mod::Qpsk, "QPSK", 4.0, 1.4}, {Mod::Bpsk, "BPSK", 8.0, 4.4}, {Mod::Qam16, "16-QAM", 8.0, 6.1},
        {Mod::Qpsk, "QPSK", 16.0, 11.7},
    };
    for (const Row& r : rows) {
        const double fs = 48000.0;
        SymbolTap tap(fs, fs / r.sps);
        const std::vector<cf> x = makeStream(r.m, r.sps, r.tau0, 3000, 0.0, 7);
        const TapResult t = feed(tap, x);
        const double err = circDiff(t.tau, std::fmod(r.tau0, r.sps), r.sps);
        const double evm = evmPercent(t.pts, r.m);
        std::printf("MEAS %s, %.1f samples/symbol, true timing %.1f: tap timing %.2f (error %+.2f sample), locked %d, EVM %.2f %%, eye opening I %.3f Q %.3f, %llu symbols\n",
                    r.name, r.sps, r.tau0, t.tau, err, t.locked ? 1 : 0, evm, t.eyeI, std::isnan(t.eyeQ) ? 0.0 : t.eyeQ, static_cast<unsigned long long>(t.symbols));
        CHECK(t.locked);
        CHECK(std::fabs(err) < 0.2);
        // Cubic interpolation measured 0.63..0.98 % (sps 8 and 16), 1.74 % at 4 samples per symbol; linear interpolation measured 0.88..1.00 and
        // 2.86 %. Non-integer sps: the timing is good to about 0.1 sample, which is 1 % of a symbol and costs about 3 % EVM.
        const double limit = r.sps != std::floor(r.sps) ? 4.0 : (r.sps <= 4.0 ? 2.2 : (r.m == Mod::Qam16 ? 1.3 : 0.9));
        CHECK(evm < limit);
        CHECK(t.symbols > 2900);
        if (r.m != Mod::Qam16) { CHECK(t.eyeI > 0.9); }
    }
}

void testNoiseAndManual() {
    double prev = 0;
    for (double sigma : {0.0, 0.02, 0.05, 0.1, 0.2}) {
        SymbolTap tap(48000.0, 6000.0);
        const TapResult t = feed(tap, makeStream(Mod::Qpsk, 8.0, 3.3, 3000, sigma, 8));
        const double evm = evmPercent(t.pts, Mod::Qpsk);
        std::printf("MEAS QPSK with noise sigma %.2f per component: EVM %.1f %%, eye opening %.2f, timing %.2f (true 3.3)\n", sigma, evm, t.eyeI, t.tau);
        CHECK(evm >= prev - 0.3);
        prev = evm;
        if (sigma <= 0.1) { CHECK(std::fabs(circDiff(t.tau, 3.3, 8.0)) < 0.6); }
    }
    // Manual timing: right and wrong. A half-symbol error closes the eye and spreads the points.
    const std::vector<cf> x = makeStream(Mod::Qpsk, 8.0, 3.3, 3000, 0.0, 9);
    SymbolTap good(48000.0, 6000.0), bad(48000.0, 6000.0);
    good.setTimingOffsetSamples(3.3);
    bad.setTimingOffsetSamples(3.3 + 4.0);
    const TapResult g = feed(good, x), b = feed(bad, x);
    std::printf("MEAS manual timing right: EVM %.2f %%, eye %.3f; half a symbol wrong: EVM %.1f %%, eye %.3f\n", evmPercent(g.pts, Mod::Qpsk), g.eyeI, evmPercent(b.pts, Mod::Qpsk), b.eyeI);
    CHECK(evmPercent(g.pts, Mod::Qpsk) < 3.0);
    CHECK(evmPercent(b.pts, Mod::Qpsk) > 30.0);
    CHECK(g.eyeI > 0.9);
    CHECK(b.eyeI < 0.35 || std::isnan(b.eyeI));
    // Back to auto from manual.
    SymbolTap sw(48000.0, 6000.0);
    sw.setTimingOffsetSamples(0.0);
    sw.setTimingOffsetSamples(std::nan(""));
    const TapResult s = feed(sw, x);
    CHECK(std::fabs(circDiff(s.tau, 3.3, 8.0)) < 0.2);
}

void testRingsAndEye() {
    SymbolTap tap(48000.0, 6000.0, 64, 16, 32);
    const std::vector<cf> x = makeStream(Mod::Qpsk, 8.0, 2.2, 1000, 0.0, 10);
    for (std::size_t p = 0; p < x.size(); p += 100) { tap.push(x.data() + p, std::min<std::size_t>(100, x.size() - p)); }
    std::vector<cf> pts(64);
    const std::size_t np = tap.constellation(pts.data(), 64);
    CHECK(np == 63);  // capacity - 1
    CHECK(tap.constellation(pts.data(), 5) == 5);
    const std::size_t len = tap.eyeLength();
    CHECK(len == 65);
    std::vector<float> ei(16 * len), eq(16 * len);
    const std::size_t nt = tap.eye(ei.data(), eq.data(), 16);
    CHECK(nt == 15);
    // The newest trace's centre equals the newest constellation point (same instant, same interpolation).
    std::vector<cf> last(1);
    tap.constellation(last.data(), 1);
    CHECK_NEAR(ei[(nt - 1) * len + 32], last[0].real(), 1e-6);
    CHECK_NEAR(eq[(nt - 1) * len + 32], last[0].imag(), 1e-6);
    // Traces: at +-1 symbol from the centre the value is again at a symbol instant (an ISI-free point): |I| about 0.7 for QPSK.
    double worst = 0;
    for (std::size_t t = 0; t < nt; ++t) {
        worst = std::max({worst, std::fabs(std::fabs(ei[t * len + 32]) - 0.7071), std::fabs(std::fabs(ei[t * len + 0]) - 0.7071), std::fabs(std::fabs(ei[t * len + 64]) - 0.7071)});
    }
    std::printf("MEAS eye traces: |I| at the centre and at one symbol either side stays within %.4f of 1/sqrt(2)\n", worst);
    CHECK(worst < 0.03);
    // Too few traces: NaN opening.
    SymbolTap fresh(48000.0, 6000.0);
    CHECK(std::isnan(fresh.eyeOpening(0)));
}

void testInvalidAndBasics() {
    SymbolTap bad(48000.0, 30000.0);  // 1.6 samples per symbol
    CHECK(!bad.valid());
    std::vector<cf> x(1000, cf(1, 0));
    bad.push(x.data(), x.size());
    CHECK(bad.symbols() == 0);
    SymbolTap zero(48000.0, 0.0);
    CHECK(!zero.valid());
    SymbolTap nanr(48000.0, std::nan(""));
    CHECK(!nanr.valid());
    // NaN samples read as zero; chunking invariance of the points.
    const std::vector<cf> y = makeStream(Mod::Qpsk, 8.0, 1.5, 600, 0.0, 11);
    std::vector<cf> z = y;
    z[100] = cf(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    SymbolTap t1(48000.0, 6000.0);
    feed(t1, z);
    std::vector<cf> a(512), b(512);
    for (std::size_t chunk : {1u, 37u, 5000u}) {
        SymbolTap t2(48000.0, 6000.0);
        feed(t2, z, chunk);
        SymbolTap t3(48000.0, 6000.0);
        feed(t3, z, 4096);
        const std::size_t na = t2.constellation(a.data(), 512), nb = t3.constellation(b.data(), 512);
        CHECK(na == nb && std::equal(a.begin(), a.begin() + static_cast<long>(na), b.begin()));
    }
    // No allocation in push or the snapshots.
    SymbolTap al(48000.0, 6000.0);
    long news = 0;
    std::vector<float> ei(64 * al.eyeLength()), eq(64 * al.eyeLength());
    for (int i = 0; i < 20; ++i) {
        AllocProbe p;
        al.push(y.data() + 1000 * i % (y.size() - 1000), 1000);
        al.constellation(a.data(), 512);
        al.eye(ei.data(), eq.data(), 64);
        news += p.news();
    }
    std::printf("MEAS operator new in 20 push + snapshot rounds: %ld\n", news);
    CHECK(news == 0);
    // Concurrent reader: snapshots taken while the producer runs are finite and bounded (the tear check itself is the sequence test of
    // the SNR tap, which has an invariant a torn copy would break; QPSK points would still look valid if two were mixed).
    SymbolTap cc(48000.0, 6000.0, 64, 16, 32);
    const std::vector<cf> long_ = makeStream(Mod::Qpsk, 8.0, 3.0, 20000, 0.0, 12);
    std::atomic<bool> stop{false};
    std::atomic<long> bad2{0}, reads{0};
    std::thread rd([&] {
        std::vector<cf> p(64);
        while (!stop.load()) {
            const std::size_t n = cc.constellation(p.data(), 64);
            if (cc.symbols() > 400) {
                for (std::size_t i = 0; i < n; ++i) { if (!std::isfinite(p[i].real()) || !std::isfinite(p[i].imag()) || std::abs(p[i]) > 2.0f) { ++bad2; } }
            }
            ++reads;
        }
    });
    for (int rep = 0; rep < 20; ++rep) { for (std::size_t p = 0; p < long_.size(); p += 256) { cc.push(long_.data() + p, std::min<std::size_t>(256, long_.size() - p)); } }
    stop = true;
    rd.join();
    std::printf("MEAS concurrent reader: %ld snapshots during 3.2 million samples, %ld non-finite or out-of-range points\n", reads.load(), bad2.load());
    CHECK(bad2.load() == 0);
}
}  // namespace

int main() {
    testTiming();
    testNoiseAndManual();
    testRingsAndEye();
    testInvalidAndBasics();
    return testSummary("test_symbol_tap");
}
