// Tests for IF shift / passband tuning (dsp/passband.hpp).
//
// The edge maths is checked against hand-computed tables; the filter is checked with real tones: gain in the
// pass band, attenuation outside it, and what happens to a tone when the band is moved while the filter runs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/passband.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

using cascade::dsp::PassbandEdges;
using cascade::dsp::PassbandFilter;
using cascade::dsp::PassbandMode;
using cascade::dsp::PassbandSpec;
using cf = std::complex<float>;
constexpr double kPi2 = 6.283185307179586;

PassbandSpec spec(PassbandMode m, double lo, double hi, double shift = 0.0, double pitch = 700.0) {
    PassbandSpec s;
    s.mode = m;
    s.lowCutHz = lo;
    s.highCutHz = hi;
    s.shiftHz = shift;
    s.cwPitchHz = pitch;
    return s;
}

void edgeCase(const PassbandSpec& s, double lo, double hi, bool clamped, int line) {
    const PassbandEdges e = cascade::dsp::passbandEdges(s, 48000.0);
    ++g_checksRun;
    if (std::fabs(e.loHz - lo) > 1e-9 || std::fabs(e.hiHz - hi) > 1e-9 || e.clamped != clamped) {
        ++g_checksFailed;
        std::printf("FAIL edges line %d: got [%g, %g] clamped=%d, expected [%g, %g] clamped=%d\n", line, e.loHz, e.hiHz, e.clamped, lo, hi,
                    clamped);
    }
}

void testEdgeMaths() {
    using M = PassbandMode;
    edgeCase(spec(M::Usb, 300, 2700), 300, 2700, false, __LINE__);
    edgeCase(spec(M::Usb, 300, 2700, 200), 500, 2900, false, __LINE__);
    edgeCase(spec(M::Usb, 100, 3000, -100), 0, 2900, false, __LINE__);
    edgeCase(spec(M::Lsb, 300, 2700), -2700, -300, false, __LINE__);
    edgeCase(spec(M::Lsb, 300, 2700, 200), -2900, -500, false, __LINE__);     // positive shift = higher audio = more negative RF
    edgeCase(spec(M::Cw, 450, 950), -250, 250, false, __LINE__);               // 700 Hz pitch: zero-beat in the middle
    edgeCase(spec(M::Cw, 450, 950, 100), -150, 350, false, __LINE__);
    edgeCase(spec(M::Cw, 450, 950, 0, 600), -150, 350, false, __LINE__);       // other pitch moves the band the other way
    edgeCase(spec(M::Am, 4000, 5000), -4000, 5000, false, __LINE__);           // AM: extents below / above the carrier
    edgeCase(spec(M::Am, 4000, 5000, 1000), -3000, 6000, false, __LINE__);
    edgeCase(spec(M::Am, 4000, 5000, 4500), -100, 9500, true, __LINE__);      // carrier would fall outside: held at the margin
    edgeCase(spec(M::Am, 4000, 5000, -6000), -10000, 100, true, __LINE__);
    edgeCase(spec(M::Usb, 2700, 300), 300, 2700, true, __LINE__);              // reversed cuts are swapped
    edgeCase(spec(M::Usb, 1000, 1010), 980, 1030, true, __LINE__);             // minimum bandwidth 50 Hz
    edgeCase(spec(M::Usb, 300, 30000), 300, 21600, true, __LINE__);            // 0.45 x the rate
    edgeCase(spec(M::Usb, std::nan(""), 2700), 300, 2700, false, __LINE__);    // non-finite reads as the default
    edgeCase(spec(M::Usb, 300, 2700, INFINITY), 300, 2700, false, __LINE__);
    edgeCase(spec(M::Usb, -50, 2700), 0, 2700, false, __LINE__);               // negative cut reads as 0
    // the sign convention: the same positive shift moves USB up and LSB down, mirror images of each other
    const PassbandEdges u = cascade::dsp::passbandEdges(spec(M::Usb, 300, 2700, 250), 48000.0);
    const PassbandEdges l = cascade::dsp::passbandEdges(spec(M::Lsb, 300, 2700, 250), 48000.0);
    CHECK_NEAR(u.loHz, -l.hiHz, 1e-9);
    CHECK_NEAR(u.hiHz, -l.loHz, 1e-9);
}

// Amplitude of the complex tone at baseband frequency f in y[from, to): one DFT bin with a 4-term Blackman-Harris window (far leakage
// below -100 dB, so a strong tone elsewhere does not mask an -80 dB measurement), normalised so a unit tone reads 1.
double toneAmp(const std::vector<cf>& y, double fs, double f, std::size_t from, std::size_t to) {
    std::complex<double> acc = 0;
    double wsum = 0;
    const double n = static_cast<double>(to - from);
    for (std::size_t i = from; i < to; ++i) {
        const double r = kPi2 * static_cast<double>(i - from) / n;
        const double w = 0.35875 - 0.48829 * std::cos(r) + 0.14128 * std::cos(2 * r) - 0.01168 * std::cos(3 * r);
        acc += w * std::complex<double>(y[i]) * std::polar(1.0, -kPi2 * f * static_cast<double>(i) / fs);
        wsum += w;
    }
    return std::abs(acc) / wsum;
}
double db(double a) { return 20.0 * std::log10(std::max(a, 1e-12)); }

std::vector<cf> tones(std::size_t n, double fs, std::initializer_list<std::pair<double, double>> list, std::size_t startIndex = 0) {
    std::vector<cf> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::complex<double> v = 0;
        for (const auto& t : list) { v += t.second * std::polar(1.0, kPi2 * t.first * static_cast<double>(i + startIndex) / fs); }
        x[i] = cf(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
    return x;
}

// Runs the stream through the filter in 1000-sample chunks; returns the output.
std::vector<cf> run(PassbandFilter& f, const std::vector<cf>& x) {
    std::vector<cf> y(x.size());
    for (std::size_t i = 0; i < x.size(); i += 1000) {
        const std::size_t n = std::min<std::size_t>(1000, x.size() - i);
        f.process(x.data() + i, n, y.data() + i);
    }
    return y;
}

struct Pos {
    double f;
    bool in;
};

// Feeds each tone alone, long enough for the filter to fill, and measures the gain; `in` tones must be 0 dB +-0.05, the others below -78 dB.
void checkResponse(const char* name, double fs, double transition, const PassbandSpec& s, std::initializer_list<Pos> list) {
    PassbandFilter f(fs, transition);
    f.setPassband(s, true);
    for (const Pos& p : list) {
        f.reset();
        const std::size_t n = f.taps() * 3;
        const auto y = run(f, tones(n, fs, {{p.f, 1.0}}));
        const double g = db(toneAmp(y, fs, p.f, f.taps() + 100, n));
        std::printf("  %-22s tone %+8.1f Hz: %+8.2f dB (%s)\n", name, p.f, g, p.in ? "inside" : "outside");
        if (p.in) { CHECK_NEAR(g, 0.0, 0.05); } else { CHECK(g < -78.0); }
    }
}

void testPassbandAndStopband() {
    using M = PassbandMode;
    std::printf("taps at 48 kHz / 300 Hz transition / 80 dB: %zu\n", PassbandFilter(48000.0).taps());
    // edges are -6 dB points, so tones are placed in the flat part (>= transition/2 inside) or in the stop band (>= transition/2 outside)
    checkResponse("USB 300..2700", 48000.0, 300.0, spec(M::Usb, 300, 2700), {{1500, true}, {500, true}, {2500, true}, {100, false}, {2900, false}, {-1500, false}, {8000, false}});
    checkResponse("LSB 300..2700", 48000.0, 300.0, spec(M::Lsb, 300, 2700), {{-1500, true}, {-500, true}, {-100, false}, {-2900, false}, {1500, false}});
    checkResponse("CW 450..950 @700", 12000.0, 60.0, spec(M::Cw, 450, 950), {{0, true}, {-180, true}, {180, true}, {-300, false}, {300, false}, {1000, false}});
    checkResponse("AM -4000..+4000", 12000.0, 300.0, spec(M::Am, 4000, 4000), {{0, true}, {3000, true}, {-3000, true}, {4600, false}, {-4600, false}});
}

// A tone inside the band before a shift and outside after: attenuated by the stop-band figure; the carrier / other side stays.
double settleAndMeasure(PassbandFilter& f, double fs, double tone, double seconds, double* ampBefore) {
    const std::size_t n = static_cast<std::size_t>(seconds * fs);
    const auto y = run(f, tones(n, fs, {{tone, 1.0}}, 0));
    if (ampBefore != nullptr) { *ampBefore = toneAmp(y, fs, tone, n - n / 4, n); }
    return 0;
}

void testShiftMovesTheBand() {
    using M = PassbandMode;
    struct Case {
        const char* name;
        double fs, transition;
        PassbandSpec before, after;
        double tone;            // inside before, outside after
        double keep;            // a second tone inside both (the carrier, for AM)
    };
    const Case cases[] = {
        {"USB shift +500", 48000.0, 300.0, spec(M::Usb, 300, 2700), spec(M::Usb, 300, 2700, 500), 600.0, 1500.0},
        {"USB shift -500", 48000.0, 300.0, spec(M::Usb, 800, 3200), spec(M::Usb, 800, 3200, -500), 3000.0, 1500.0},
        {"LSB shift +500", 48000.0, 300.0, spec(M::Lsb, 300, 2700), spec(M::Lsb, 300, 2700, 500), -600.0, -1500.0},
        {"CW shift +200", 12000.0, 60.0, spec(M::Cw, 450, 950), spec(M::Cw, 450, 950, 200), -200.0, 150.0},
        {"AM shift +1000", 12000.0, 300.0, spec(M::Am, 4000, 4000), spec(M::Am, 4000, 4000, 1000), -3500.0, 0.0},
    };
    for (const Case& c : cases) {
        PassbandFilter f(c.fs, c.transition);
        f.setPassband(c.before, true);
        const std::size_t n1 = f.taps() * 3;
        // the two tones run on continuously across the change
        const std::size_t n2 = std::max<std::size_t>(f.taps() * 4, static_cast<std::size_t>(0.3 * c.fs));   // the 8 ms tap ramp needs ~ 9.2 time constants (74 ms) to reach -80 dB
        const auto x = tones(n1 + n2, c.fs, {{c.tone, 1.0}, {c.keep, 0.5}});
        std::vector<cf> y(x.size());
        f.process(x.data(), n1, y.data());
        const double before = toneAmp(y, c.fs, c.tone, n1 - 2 * f.taps(), n1);   // (the keep tone is a different bin: window holds many cycles)
        f.setPassband(c.after);                                                     // ramped
        f.process(x.data() + n1, n2, y.data() + n1);
        const double after = toneAmp(y, c.fs, c.tone, n1 + n2 - f.taps(), n1 + n2);
        const double keepBefore = toneAmp(y, c.fs, c.keep, n1 - 2 * f.taps(), n1);
        const double keepAfter = toneAmp(y, c.fs, c.keep, n1 + n2 - f.taps(), n1 + n2);
        std::printf("  %-16s tone %+7.1f Hz: %+7.2f dB before, %+8.2f dB after; second tone (%+.0f Hz) %+.2f -> %+.2f dB\n", c.name, c.tone,
                    db(before), db(after), c.keep, db(keepBefore / 0.5), db(keepAfter / 0.5));
        CHECK_NEAR(db(before), 0.0, 0.05);
        CHECK(db(after) < -78.0);                                    // by the stop-band figure (80 dB design, 2 dB measurement margin)
        CHECK_NEAR(db(keepBefore / 0.5), 0.0, 0.05);
        CHECK_NEAR(db(keepAfter / 0.5), 0.0, 0.05);
    }
}

// Two tones per edge: moving the low cut touches only the low-edge tone, moving the high cut only the high-edge tone.
void testIndependentEdges() {
    using M = PassbandMode;
    const double fs = 48000.0;
    struct Case {
        const char* name;
        PassbandSpec base, moved;
        double lowEdgeTone, highEdgeTone;
        bool lowMoves;
    };
    const Case cases[] = {
        {"USB low cut 300 -> 700", spec(M::Usb, 300, 2700), spec(M::Usb, 700, 2700), 500.0, 2400.0, true},
        {"USB high cut 2700 -> 2200", spec(M::Usb, 300, 2700), spec(M::Usb, 300, 2200), 500.0, 2400.0, false},
        {"LSB low cut 300 -> 700", spec(M::Lsb, 300, 2700), spec(M::Lsb, 700, 2700), -500.0, -2400.0, true},
        {"LSB high cut 2700 -> 2200", spec(M::Lsb, 300, 2700), spec(M::Lsb, 300, 2200), -500.0, -2400.0, false},
        {"AM lower extent 4000 -> 2500", spec(M::Am, 4000, 4000), spec(M::Am, 2500, 4000), -3500.0, 3500.0, true},
        {"AM upper extent 4000 -> 2500", spec(M::Am, 4000, 4000), spec(M::Am, 4000, 2500), -3500.0, 3500.0, false},
    };
    for (const Case& c : cases) {
        const bool am = c.base.mode == M::Am;
        const double rate = am ? 12000.0 : fs;
        PassbandFilter f(rate, 300.0);
        f.setPassband(c.base, true);
        const std::size_t n1 = f.taps() * 3, n2 = std::max<std::size_t>(f.taps() * 4, static_cast<std::size_t>(0.3 * rate));
        const auto x = tones(n1 + n2, rate, {{c.lowEdgeTone, 1.0}, {c.highEdgeTone, 1.0}});
        std::vector<cf> y(x.size());
        f.process(x.data(), n1, y.data());
        f.setPassband(c.moved);
        f.process(x.data() + n1, n2, y.data() + n1);
        const std::size_t a = n1 + n2 - f.taps(), b = n1 + n2;
        const double loB = db(toneAmp(y, rate, c.lowEdgeTone, n1 - 2 * f.taps(), n1)), hiB = db(toneAmp(y, rate, c.highEdgeTone, n1 - 2 * f.taps(), n1));
        const double loA = db(toneAmp(y, rate, c.lowEdgeTone, a, b)), hiA = db(toneAmp(y, rate, c.highEdgeTone, a, b));
        std::printf("  %-30s low-edge tone %+8.2f -> %+8.2f dB, high-edge tone %+8.2f -> %+8.2f dB\n", c.name, loB, loA, hiB, hiA);
        CHECK_NEAR(loB, 0.0, 0.05);
        CHECK_NEAR(hiB, 0.0, 0.05);
        // the first tone sits by the first-named edge: USB low cut -> 500; LSB low cut -> -500; AM lower extent -> -3500; the second tone by the other
        const bool firstMoves = c.lowMoves;
        const double movedA = firstMoves ? loA : hiA;
        const double keptA = firstMoves ? hiA : loA;
        CHECK(movedA < -78.0);
        CHECK_NEAR(keptA, 0.0, 0.05);
    }
}

// No click when the band is moved while a steady tone runs.
void testNoClick() {
    using M = PassbandMode;
    const double fs = 48000.0;
    const double f0 = 1500.0, amp = 0.5;
    const double natural = amp * kPi2 * f0 / fs;       // largest sample-to-sample step of the tone itself

    // (a) a sweep of the shift from -200 to +800 Hz in 5 ms steps while the tone stays inside the band: output equals the delayed tone
    {
        PassbandFilter f(fs, 300.0);
        f.setPassband(spec(M::Usb, 300, 2700, -200), true);
        const std::size_t n = static_cast<std::size_t>(1.5 * fs);
        const auto x = tones(n, fs, {{f0, amp}});
        std::vector<cf> y(n);
        const std::size_t step = static_cast<std::size_t>(0.005 * fs);
        for (std::size_t i = 0; i < n; i += step) {
            const double t = static_cast<double>(i) / fs;
            const double shift = -200.0 + 1000.0 * std::clamp((t - 0.3) / 1.0, 0.0, 1.0);
            f.setPassband(spec(M::Usb, 300, 2700, shift));
            f.process(x.data() + i, std::min(step, n - i), y.data() + i);
        }
        double maxErr = 0, maxStep = 0;
        for (std::size_t i = f.taps() + 200; i < n; ++i) {
            const std::complex<double> ref = amp * std::polar(1.0, kPi2 * f0 * (static_cast<double>(i) - static_cast<double>(f.latencySamples())) / fs);
            maxErr = std::max(maxErr, std::abs(std::complex<double>(y[i]) - ref));
            if (i + 1 < n) { maxStep = std::max<double>(maxStep, std::abs(y[i + 1] - y[i])); }
        }
        std::printf("  sweep, tone inside: largest deviation from the delayed tone %.2e (%.1f dB re tone), largest step %.4f (tone's own %.4f)\n", maxErr,
                    db(maxErr / amp), maxStep, natural);
        CHECK(maxErr < 2e-4);                        // -68 dB re the 0.5 tone: pinned (measured 3e-5)
        CHECK(maxStep < 1.05 * natural);
    }
    // (b) one big jump that takes the tone from the middle of the band to the stop band: smooth fade, no step
    for (int ramp = 1; ramp >= 0; --ramp) {
        PassbandFilter f(fs, 300.0);
        if (!ramp) { f.setRampSeconds(0.0); }
        f.setPassband(spec(M::Usb, 300, 2700), true);
        const std::size_t n1 = static_cast<std::size_t>(0.5 * fs), n2 = static_cast<std::size_t>(0.2 * fs);
        const auto x = tones(n1 + n2, fs, {{f0, amp}});
        std::vector<cf> y(x.size());
        f.process(x.data(), n1, y.data());
        f.setPassband(spec(M::Usb, 300, 2700, 1500));    // band moves to 1800..4200: the tone is now outside
        f.process(x.data() + n1, n2, y.data() + n1);
        double maxStep = 0;
        for (std::size_t i = n1 - 400; i + 1 < n1 + n2; ++i) { maxStep = std::max<double>(maxStep, std::abs(y[i + 1] - y[i])); }
        std::printf("  tone driven out of the band, ramp %s: largest step %.4f (tone's own %.4f, ratio %.2f)\n", ramp ? "8 ms" : "off", maxStep, natural,
                    maxStep / natural);
        if (ramp) { CHECK(maxStep < 1.05 * natural); } else { CHECK(maxStep > 1.5 * natural); }
    }
}

// Cost: how long a design takes (control thread) and what the filter costs the DSP thread; both allocation-free.
void testCost() {
    using M = PassbandMode;
    for (double fs : {12000.0, 48000.0, 192000.0}) {
        PassbandFilter f(fs, 300.0);
        const int reps = 200;
        const auto t0 = std::chrono::steady_clock::now();
        {
            AllocProbe probe;
            for (int i = 0; i < reps; ++i) { f.setPassband(spec(M::Usb, 300 + (i % 7) * 50, 2700, (i % 5) * 40)); }
            CHECK(probe.news() == 0);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double perDesign = std::chrono::duration<double, std::micro>(t1 - t0).count() / reps;
        // DSP: one second of samples
        const std::size_t n = static_cast<std::size_t>(fs);
        std::vector<cf> x = tones(n, fs, {{1500.0, 0.5}});
        std::vector<cf> y(n);
        const auto t2 = std::chrono::steady_clock::now();
        {
            AllocProbe probe;
            f.process(x.data(), n, y.data());
            CHECK(probe.news() == 0);
        }
        const auto t3 = std::chrono::steady_clock::now();
        const double dspMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
        std::printf("  rate %6.0f: %zu taps (transition %.0f Hz), design %.0f us each, DSP %.1f ms per second of signal (%.1f%% of one core)\n", fs, f.taps(),
                    f.actualTransitionHz(), perDesign, dspMs, dspMs / 10.0);
        CHECK(perDesign < 3000.0);                       // a design is a control-thread job of well under 3 ms
        CHECK(f.latencySamples() == (f.taps() - 1) / 2);
    }
}

void testRobustness() {
    using M = PassbandMode;
    const double fs = 48000.0;
    // block-split invariance with no change mid-stream
    PassbandFilter a(fs), b(fs);
    a.setPassband(spec(M::Usb, 300, 2700, 100), true);
    b.setPassband(spec(M::Usb, 300, 2700, 100), true);
    const auto x = tones(20000, fs, {{1000.0, 0.5}, {-700.0, 0.3}});
    std::vector<cf> ya(x.size()), yb(x.size());
    a.process(x.data(), x.size(), ya.data());
    for (std::size_t i = 0; i < x.size();) {
        const std::size_t n = std::min<std::size_t>(1 + (i * 7919) % 997, x.size() - i);
        b.process(x.data() + i, n, yb.data() + i);
        i += n;
    }
    // the 16-sample block grid is counted in samples, so splitting changes nothing
    CHECK(ya == yb);
    // in place, and non-finite input
    std::vector<cf> bad = x;
    bad[500] = cf(std::nanf(""), 0.0f);
    bad[501] = cf(0.0f, INFINITY);
    PassbandFilter c(fs);
    c.process(bad.data(), bad.size(), bad.data());
    bool finite = true;
    for (const cf& v : bad) { finite = finite && std::isfinite(v.real()) && std::isfinite(v.imag()); }
    CHECK(finite);
    // capped taps: a 1 kHz transition at 192 kHz with maxTaps 255 reports the wider transition it really has
    PassbandFilter d(192000.0, 100.0, 80.0, 255);
    CHECK(d.taps() == 255);
    CHECK(d.actualTransitionHz() > 100.0);
    CHECK_NEAR(d.actualTransitionHz(), (80.0 - 7.95) / (14.36 * 254.0) * 192000.0, 1e-6);
    // the same group delay for every design (the tap count never changes)
    const std::size_t lat = a.latencySamples();
    a.setPassband(spec(M::Am, 4000, 4000, 900));
    CHECK(a.latencySamples() == lat);
}

}  // namespace

int main() {
    testEdgeMaths();
    std::printf("response:\n");
    testPassbandAndStopband();
    std::printf("shift:\n");
    testShiftMovesTheBand();
    std::printf("independent edges:\n");
    testIndependentEdges();
    std::printf("clicks:\n");
    testNoClick();
    std::printf("cost:\n");
    testCost();
    testRobustness();
    return testSummary("test_passband");
}
