// C-QUAM AM stereo decoder and pilot detector (src/dsp/cquam.hpp), tested on a GENERATED C-QUAM signal: the test's encoder builds
// s = (1 + L + R) e^{j (arctan((L - R + pilot) / (1 + L + R)))} on a carrier with an offset and an unknown phase, optionally with noise.
// The formula is the reconstruction described in the header (47 CFR 73.128 gives the description, not the readable equation).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <complex>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>

#include "alloc_probe.hpp"
#include "blk4_util.hpp"
#include "dsp/cquam.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

using cascade::dsp::CquamDecoder;
using cd = std::complex<double>;
using cf = std::complex<float>;

namespace {
constexpr double kFs = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = 3.14159265358979323846;

struct Enc {
    std::function<double(double)> left, right;  // audio programme, functions of time
    double pilot = 0.05;                        // radians of carrier phase, 0 = no pilot
    double offsetHz = 0.0, phase0 = 1.0;
    double noise = 0.0;                          // sigma per component
    std::mt19937 rng{3};
    std::normal_distribution<double> nd{0.0, 1.0};
    double t = 0.0, carrierPhase = 0.0;
    double maxPhase = 0.0, maxEnvErr = 0.0;
    cf next() {
        const double L = left(t), R = right(t);
        const double S = L + R, D = L - R;
        const double p = pilot * std::sin(kTwoPi * 25.0 * t);
        const double phi = std::atan2(D + p, 1.0 + S);
        maxPhase = std::max(maxPhase, std::fabs(phi));
        carrierPhase += kTwoPi * offsetHz / kFs;
        const cd s = std::polar(1.0 + S, phi + carrierPhase + phase0);
        maxEnvErr = std::max(maxEnvErr, std::fabs(std::abs(s) - (1.0 + S)));
        t += 1.0 / kFs;
        const cd o = s + cd(noise * nd(rng), noise * nd(rng));
        return cf(static_cast<float>(o.real()), static_cast<float>(o.imag()));
    }
};

struct Result {
    std::vector<float> l, r;
    std::vector<cf> iq;
};

Result runDecoder(CquamDecoder& dec, Enc& e, double seconds, std::size_t block = 480) {
    Result o;
    const std::size_t n = static_cast<std::size_t>(seconds * kFs);
    o.iq.resize(n);
    for (auto& s : o.iq) { s = e.next(); }
    o.l.resize(n);
    o.r.resize(n);
    for (std::size_t p = 0; p < n; p += block) {
        const std::size_t m = std::min(block, n - p);
        dec.process(o.iq.data() + p, m, o.l.data() + p, o.r.data() + p);
    }
    return o;
}

double amp(const std::vector<float>& v, double fHz) { return blk4::toneAmp(v, v.size() - 24000, v.size(), kFs, fHz); }

auto tone = [](double a, double f) { return [a, f](double t) { return a * std::sin(kTwoPi * f * t); }; };
auto zero = [](double) { return 0.0; };

void testStandardPoints() {
    // The numbers the regulation gives, checked on the generator (so the reconstruction at least meets them).
    CHECK_NEAR(std::atan2(1.0, 1.0), 0.785, 0.001);                // L - R = 1, L + R = 0 -> 100 % difference = 0.785 rad
    CHECK(std::fabs(std::atan2(-0.75, 1.0 - 0.75)) <= 1.25);       // single channel at -0.75 -> 1.249 rad (limit 1.25)
    CHECK_NEAR(std::fabs(std::atan2(-0.75, 0.25)), 1.249, 0.001);
    // Left advances the phase: a positive left envelope excursion with R = 0 gives a positive phase.
    CHECK(std::atan2(0.3, 1.3) > 0.0);
    CHECK(std::atan2(-0.3, 1.0 + 0.3) < 0.0);  // right positive: D = -0.3
    // The compatible envelope: |s| is exactly 1 + L + R (what a plain AM detector sees).
    Enc e;
    e.left = tone(0.4, 1000.0);
    e.right = tone(0.3, 1700.0);
    for (int i = 0; i < 48000; ++i) { e.next(); }
    std::printf("MEAS generator: envelope equals 1 + L + R to %.1e, largest phase %.3f rad (limit 1.25)\n", e.maxEnvErr, e.maxPhase);
    CHECK(e.maxEnvErr < 1e-12);
    CHECK(e.maxPhase < 1.25);
}

void testSeparation() {
    // wantLeft: the programme is in the left channel (wanted = L at f, leak = R at f); otherwise right.
    struct Row { const char* name; std::function<double(double)> l, r; double f; bool wantLeft; double a; };
    const Row rows[] = {
        {"left only, 1 kHz 0.4", tone(0.4, 1000.0), zero, 1000.0, true, 0.4},
        {"right only, 1 kHz 0.4", zero, tone(0.4, 1000.0), 1000.0, false, 0.4},
        {"left only, 300 Hz 0.4", tone(0.4, 300.0), zero, 300.0, true, 0.4},
        {"left only, 3 kHz 0.4", tone(0.4, 3000.0), zero, 3000.0, true, 0.4},
        {"left 1 kHz 0.3 + right 2 kHz 0.3 (left tone vs right)", tone(0.3, 1000.0), tone(0.3, 2000.0), 1000.0, true, 0.3},
    };
    for (double off : {0.0, 12.0, -15.0}) {
        for (const Row& r : rows) {
            Enc e;
            e.left = r.l;
            e.right = r.r;
            e.offsetHz = off;
            CquamDecoder dec(kFs);
            const Result o = runDecoder(dec, e, 8.0);
            const double wanted = r.wantLeft ? amp(o.l, r.f) : amp(o.r, r.f);
            const double leak = r.wantLeft ? amp(o.r, r.f) : amp(o.l, r.f);
            const double sep = blk4::db(wanted / std::max(leak, 1e-9));
            std::printf("MEAS carrier offset %+.0f Hz, %s: wanted channel %.4f, other channel %.5f: separation %.1f dB; stereo %d, PLL offset %.2f Hz\n",
                        off, r.name, wanted, leak, sep, dec.stereo() ? 1 : 0, dec.carrierOffsetHz());
            CHECK(dec.stereo());
            CHECK(sep > 28.0);
            CHECK_NEAR(wanted, r.a, 0.015);
        }
    }
}

void testWithNoise() {
    for (double cnr : {40.0, 30.0, 20.0}) {
        Enc e;
        e.left = tone(0.4, 1000.0);
        e.right = zero;
        e.offsetHz = 7.0;
        e.noise = std::sqrt(1.0 / (2.0 * std::pow(10.0, cnr / 10.0)));  // carrier power 1 over two components
        CquamDecoder dec(kFs);
        const Result o = runDecoder(dec, e, 10.0);
        const double sep = blk4::db(amp(o.l, 1000.0) / std::max(amp(o.r, 1000.0), 1e-9));
        std::printf("MEAS carrier-to-noise %.0f dB (48 kHz wide), left only: separation %.1f dB, stereo %d\n", cnr, sep, dec.stereo() ? 1 : 0);
        if (cnr >= 30.0) { CHECK(sep > 25.0); }
    }
}

void testPilot() {
    // Present at 5 %: detected, with the time it took.
    {
        Enc e;
        e.left = tone(0.3, 1000.0);
        e.right = tone(0.2, 1500.0);
        CquamDecoder dec(kFs);
        const std::size_t n = static_cast<std::size_t>(5.0 * kFs);
        std::vector<cf> iq(n);
        for (auto& s : iq) { s = e.next(); }
        std::vector<float> l(480), r(480);
        double onAt = -1;
        for (std::size_t p = 0; p < n; p += 480) {
            dec.process(iq.data() + p, 480, l.data(), r.data());
            if (onAt < 0 && dec.stereo()) { onAt = static_cast<double>(p + 480) / kFs; }
        }
        std::printf("MEAS pilot 5 %%: stereo on after %.2f s, measured pilot level %.4f (nominal 0.05)\n", onAt, dec.pilotLevel());
        CHECK(onAt > 0.5 && onAt < 2.0);
        CHECK_NEAR(dec.pilotLevel(), 0.05, 0.015);
    }
    // Level sweep, and absent.
    const double levels[] = {0.0, 0.01, 0.02, 0.03, 0.05, 0.08, 0.12, 0.3};
    for (double lv : levels) {
        Enc e;
        e.left = tone(0.3, 1000.0);
        e.right = tone(0.2, 1500.0);
        e.pilot = lv;
        CquamDecoder dec(kFs);
        runDecoder(dec, e, 6.0);
        const bool expect = lv >= 0.03 && lv <= 0.08;
        std::printf("MEAS pilot level %.2f rad: measured %.4f, stereo %s\n", lv, dec.pilotLevel(), dec.stereo() ? "ON" : "off");
        if (lv == 0.0 || lv == 0.01 || lv == 0.05 || lv == 0.12 || lv == 0.3) { CHECK(dec.stereo() == (lv == 0.05)); }
        if (lv == 0.03 || lv == 0.08) { CHECK(dec.stereo() == expect); }
    }
    // A program tone at 25 Hz at a normal level (0.2) is not a pilot; a 25 Hz tone at the PILOT level in the programme is, by
    // construction, indistinguishable (documented): it is accepted.
    {
        Enc e;
        e.left = [](double t) { return 0.15 * std::sin(kTwoPi * 25.0 * t) + 0.1 * std::sin(kTwoPi * 1000.0 * t); };
        e.right = [](double t) { return -0.15 * std::sin(kTwoPi * 25.0 * t); };
        e.pilot = 0.0;
        CquamDecoder dec(kFs);
        runDecoder(dec, e, 6.0);
        std::printf("MEAS programme with a 0.3 (L-R) tone at 25 Hz and no pilot: pilot level read %.3f, stereo %d\n", dec.pilotLevel(), dec.stereo() ? 1 : 0);
        CHECK(!dec.stereo());
    }
    // Bass in L-R: tones at 15, 25 and 35 Hz, each at the pilot level, no pilot. The 25 Hz tone alone is in range; the
    // neighbours at 15 and 35 Hz are as strong, so it is not a pilot.
    {
        Enc e;
        e.left = [](double t) { return 0.5 * 0.05 * (std::sin(kTwoPi * 15.0 * t) + std::sin(kTwoPi * 25.0 * t + 2.0) + std::sin(kTwoPi * 35.0 * t + 4.0)); };
        e.right = [](double t) { return -0.5 * 0.05 * (std::sin(kTwoPi * 15.0 * t) + std::sin(kTwoPi * 25.0 * t + 2.0) + std::sin(kTwoPi * 35.0 * t + 4.0)); };
        e.pilot = 0.0;
        CquamDecoder dec(kFs);
        runDecoder(dec, e, 8.0);
        std::printf("MEAS bass in L-R (15, 25, 35 Hz at 0.05 each), no pilot: 25 Hz reads %.3f, stereo %d\n", dec.pilotLevel(), dec.stereo() ? 1 : 0);
        CHECK(!dec.stereo());
    }
    // Broadband programme in L-R (speech-like) with the pilot: still found, and without it still not.
    for (int withPilot = 0; withPilot < 2; ++withPilot) {
        const std::vector<float> sp = sbgen::speechLike(static_cast<std::size_t>(12 * kFs), kFs, 0.3, 17);
        const std::vector<float> sp2 = sbgen::speechLike(static_cast<std::size_t>(12 * kFs), kFs, 0.3, 18);
        Enc e;
        e.left = [&](double t) { return static_cast<double>(sp[static_cast<std::size_t>(t * kFs)]); };
        e.right = [&](double t) { return static_cast<double>(sp2[static_cast<std::size_t>(t * kFs)]); };
        e.pilot = withPilot ? 0.05 : 0.0;
        CquamDecoder dec(kFs);
        runDecoder(dec, e, 11.5);
        std::printf("MEAS two different speech-like channels, pilot %s: stereo %s (pilot level read %.3f)\n", withPilot ? "5 %" : "absent", dec.stereo() ? "ON" : "off", dec.pilotLevel());
        CHECK(dec.stereo() == (withPilot != 0));
    }
    // Noise alone: no pilot.
    {
        Enc e;
        e.left = zero;
        e.right = zero;
        e.pilot = 0.0;
        e.noise = 0.05;
        CquamDecoder dec(kFs);
        runDecoder(dec, e, 20.0);
        CHECK(!dec.stereo());
    }
}

void testMonoAndForce() {
    // Pilot absent: the output is mono, L = R = S / 2 (what the compatible envelope detector gives, halved).
    Enc e;
    e.left = tone(0.3, 1000.0);
    e.right = tone(0.3, 1000.0);
    e.pilot = 0.0;
    CquamDecoder dec(kFs);
    const Result o = runDecoder(dec, e, 6.0);
    double worst = 0;
    for (std::size_t i = o.l.size() - 24000; i < o.l.size(); ++i) { worst = std::max(worst, static_cast<double>(std::fabs(o.l[i] - o.r[i]))); }
    std::printf("MEAS mono programme, no pilot: |L - R| at most %.1e, L amplitude %.4f (programme 0.3)\n", worst, amp(o.l, 1000.0));
    CHECK(worst == 0.0);
    CHECK_NEAR(amp(o.l, 1000.0), 0.3, 0.01);
    // Force stereo with no pilot, force mono with it.
    Enc e2;
    e2.left = tone(0.4, 1000.0);
    e2.right = zero;
    e2.pilot = 0.0;
    CquamDecoder d2(kFs);
    d2.setForce(1);
    const Result o2 = runDecoder(d2, e2, 8.0);
    CHECK(blk4::db(amp(o2.l, 1000.0) / std::max(amp(o2.r, 1000.0), 1e-9)) > 30.0);
    Enc e3;
    e3.left = tone(0.4, 1000.0);
    e3.right = zero;
    CquamDecoder d3(kFs);
    d3.setForce(-1);
    const Result o3 = runDecoder(d3, e3, 8.0);
    CHECK_NEAR(amp(o3.l, 1000.0), amp(o3.r, 1000.0), 1e-6);
    // The pilot does not reach the ears: level of 25 Hz in L and R (the notch).
    Enc e4;
    e4.left = tone(0.3, 1000.0);
    e4.right = tone(0.2, 1500.0);
    CquamDecoder d4(kFs);
    const Result o4 = runDecoder(d4, e4, 8.0);
    const double p25 = std::max(amp(o4.l, 25.0), amp(o4.r, 25.0));
    std::printf("MEAS 25 Hz left in the outputs with the pilot on: %.5f (pilot in D is 0.05, so 0.025 in each channel without the notch)\n", p25);
    CHECK(p25 < 0.005);
}

void testBasics() {
    // Disabled: silence out. NaN input: finite out. Chunking invariance.
    CquamDecoder off(kFs);
    off.setEnabled(false);
    std::vector<cf> x(480, cf(1.0f, 0.0f));
    std::vector<float> l(480, 9.0f), r(480, 9.0f);
    off.process(x.data(), 480, l.data(), r.data());
    CHECK(l[10] == 0.0f && r[10] == 0.0f);
    CquamDecoder nan(kFs);
    std::vector<cf> bad(4800, cf(std::numeric_limits<float>::quiet_NaN(), 0.0f));
    std::vector<float> bl(4800), br(4800);
    nan.process(bad.data(), bad.size(), bl.data(), br.data());
    bool fin = true;
    for (std::size_t i = 0; i < bl.size(); ++i) { fin = fin && std::isfinite(bl[i]) && std::isfinite(br[i]); }
    CHECK(fin);
    Enc e;
    e.left = tone(0.4, 1000.0);
    e.right = tone(0.1, 777.0);
    e.offsetHz = 5.0;
    CquamDecoder a(kFs);
    const Result whole = runDecoder(a, e, 3.0, 144000);
    for (std::size_t blk : {1u, 61u, 4800u}) {
        CquamDecoder b(kFs);
        std::vector<float> ol(whole.iq.size()), orr(whole.iq.size());
        for (std::size_t p = 0; p < whole.iq.size(); p += blk) { b.process(whole.iq.data() + p, std::min<std::size_t>(blk, whole.iq.size() - p), ol.data() + p, orr.data() + p); }
        CHECK(ol == whole.l && orr == whole.r);
    }
    // No allocation.
    CquamDecoder al(kFs);
    long news = 0;
    for (int i = 0; i < 40; ++i) {
        AllocProbe p;
        al.setForce(i % 3 - 1);
        al.setLoopBandwidthHz(5.0 + i);
        al.process(whole.iq.data() + 480 * i, 480, bl.data(), br.data());
        news += p.news();
    }
    std::printf("MEAS operator new in 40 process calls: %ld\n", news);
    CHECK(news == 0);
}
}  // namespace

int main() {
    testStandardPoints();
    testSeparation();
    testWithNoise();
    testPilot();
    testMonoAndForce();
    testBasics();
    return testSummary("test_cquam");
}
