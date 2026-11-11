// Tests for the CW pitch helper (dsp/cw_pitch.hpp): a table of hand-computed values and the clamps; then the numbers driven through a Weaver
// chain built exactly like Demodulator::processSsb (mix, real-tap low-pass, mix, real part): a carrier on the tuned frequency lands at the
// chosen pitch within 1 Hz for every pitch and both sidebands, and the pass band is centred on it. The existing Demodulator in CW mode is
// measured for comparison (700 Hz is where it puts a carrier; its filter is the voice SSB filter).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/cw_pitch.hpp"

#include "dsp/demod.hpp"
#include "dsp/fft.hpp"
#include "dsp/fir.hpp"
#include "test_check.hpp"

#include <complex>
#include <cstdio>
#include <vector>

using namespace cascade::dsp;
using Cx = std::complex<float>;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

std::vector<Cx> carrier(double offsetHz, double rate, std::size_t n) {
    std::vector<Cx> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = kTwoPi * std::fmod(offsetHz * static_cast<double>(i) / rate, 1.0);
        v[i] = Cx(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
    return v;
}

void mix(std::vector<Cx>& x, double hz, double rate) {
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double a = kTwoPi * std::fmod(hz * static_cast<double>(i) / rate, 1.0);
        x[i] *= Cx(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
}

// The Weaver chain driven by a tuning: mix, real-tap low-pass, mix, real part.
std::vector<float> chain(const CwTuning& t, double rate, const std::vector<Cx>& in) {
    std::vector<Cx> x = in;
    mix(x, t.firstMixHz, rate);
    double tr = 0;
    const std::size_t taps = cwFilterTaps(rate, t.lowpassTransitionHz, &tr);
    FirDecimator f(windowedSincLowpass(taps, t.lowpassCutoffHz / rate, WindowType::Hamming), 1);
    std::vector<Cx> y(f.outputCapacity(x.size()));
    const std::size_t m = f.process(x.data(), x.size(), y.data());
    y.resize(m);
    mix(y, t.secondMixHz, rate);
    std::vector<float> out(m);
    for (std::size_t i = 0; i < m; ++i) { out[i] = y[i].real(); }
    return out;
}

double rmsTail(const std::vector<float>& v, std::size_t skip) {
    double s = 0;
    for (std::size_t i = skip; i < v.size(); ++i) { s += static_cast<double>(v[i]) * v[i]; }
    return std::sqrt(s / static_cast<double>(v.size() - skip));
}

// Frequency of the strongest component of a real signal (Hann window, 32768-point FFT of the tail, parabolic peak).
double peakHz(const std::vector<float>& v, double rate) {
    const std::size_t N = 32768;
    std::vector<Cx> a(N), b(N);
    const std::size_t from = v.size() - N;
    for (std::size_t i = 0; i < N; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(N));
        a[i] = Cx(static_cast<float>(v[from + i] * w), 0.0f);
    }
    ComplexFFT fft(N);
    fft.forward(a.data(), b.data());
    std::size_t k = 1;
    for (std::size_t i = 2; i < N / 2 - 1; ++i) { if (std::abs(b[i]) > std::abs(b[k])) { k = i; } }
    const double l = std::log(std::abs(b[k - 1]) + 1e-30), c = std::log(std::abs(b[k]) + 1e-30), r = std::log(std::abs(b[k + 1]) + 1e-30);
    const double d = 0.5 * (l - r) / (l - 2 * c + r);
    return (static_cast<double>(k) + d) * rate / static_cast<double>(N);
}

void testTable() {
    CwTuning t = cwTuning(700.0, CwSideband::Upper, 500.0);
    CHECK(t.pitchHz == 700.0 && t.widthHz == 500.0 && !t.pitchClamped && !t.widthClamped);
    CHECK(t.bfoHz == 700.0 && t.firstMixHz == 0.0 && t.secondMixHz == 700.0);
    CHECK(t.lowpassCutoffHz == 250.0 && t.lowEdgeHz == 450.0 && t.highEdgeHz == 950.0 && t.lowpassTransitionHz == 250.0);
    t = cwTuning(700.0, CwSideband::Lower, 500.0);
    CHECK(t.firstMixHz == 0.0 && t.secondMixHz == -700.0 && t.bfoHz == 700.0);
    t = cwTuning(300.0, CwSideband::Upper, 100.0);
    CHECK(t.lowEdgeHz == 250.0 && t.highEdgeHz == 350.0 && t.lowpassTransitionHz == 50.0);
    t = cwTuning(1200.0, CwSideband::Upper, 2000.0);
    CHECK(t.widthHz == 2000.0 && t.lowpassTransitionHz == 400.0);
    // clamps
    t = cwTuning(100.0, CwSideband::Upper);
    CHECK(t.pitchHz == 300.0 && t.pitchClamped);
    t = cwTuning(5000.0, CwSideband::Lower);
    CHECK(t.pitchHz == 1200.0 && t.pitchClamped && t.secondMixHz == -1200.0);
    t = cwTuning(std::nan(""), CwSideband::Upper, std::nan(""));
    CHECK(t.pitchHz == 700.0 && t.widthHz == 500.0);
    t = cwTuning(700.0, CwSideband::Upper, 10.0);
    CHECK(t.widthHz == 50.0 && t.widthClamped);
    t = cwTuning(300.0, CwSideband::Upper, 5000.0);  // wider than the pitch allows: low edge keeps 50 Hz from DC
    CHECK(t.widthHz == 500.0 && t.widthClamped && t.lowEdgeHz == 50.0);
    t = cwTuning(1200.0, CwSideband::Upper, 1e9, 8000.0);  // 0.4 x 8 kHz = 3.2 kHz ceiling
    CHECK(t.highEdgeHz <= 3200.0 + 1e-9 && t.widthClamped);
    t = cwTuning(700.0, CwSideband::Upper, 500.0, -1.0);  // bad rate reads as 48 kHz
    CHECK(t.widthHz == 500.0);
    // taps
    double tr = 0;
    std::size_t n = cwFilterTaps(48000.0, 250.0, &tr);
    CHECK(n % 2 == 1 && n >= 63 && n <= 2047 && std::fabs(tr - 250.0) < 2.0);
    n = cwFilterTaps(2400000.0, 50.0, &tr);  // capped: reports its wider transition
    CHECK(n == 2047 && tr > 3000.0);
    CHECK(cwFilterTaps(48000.0, 1e9) == 63);
    CHECK(cwFilterTaps(std::nan(""), std::nan("")) % 2 == 1);
}

void testCarrierAtPitch() {
    double worst = 0;
    int runs = 0;
    for (double rate : {48000.0, 192000.0}) {
        for (double p = 300.0; p <= 1200.0; p += (rate == 48000.0 ? 50.0 : 300.0)) {
            for (CwSideband sb : {CwSideband::Upper, CwSideband::Lower}) {
                for (double w : {100.0, 500.0}) {
                    const CwTuning t = cwTuning(p, sb, w, rate);
                    const std::size_t N = static_cast<std::size_t>(rate * 0.8);
                    const std::vector<float> out = chain(t, rate, carrier(0.0, rate, N));
                    const double f = peakHz(out, rate);
                    worst = std::max(worst, std::fabs(f - p));
                    CHECK(std::fabs(f - p) < 1.0);
                    ++runs;
                }
            }
        }
    }
    std::printf("carrier on the tuned frequency: worst pitch error %.4f Hz over %d runs\n", worst, runs);
    // which way a signal above the zero-beat moves the pitch: upper raises it, lower lowers it
    for (double p : {400.0, 700.0, 1100.0}) {
        const std::size_t N = 40000;
        const double up = peakHz(chain(cwTuning(p, CwSideband::Upper, 500.0), 48000.0, carrier(100.0, 48000.0, N)), 48000.0);
        const double lo = peakHz(chain(cwTuning(p, CwSideband::Lower, 500.0), 48000.0, carrier(100.0, 48000.0, N)), 48000.0);
        CHECK(std::fabs(up - (p + 100.0)) < 1.0);
        CHECK(std::fabs(lo - (p - 100.0)) < 1.0);
    }
}

// -6 dB edges of the audio pass band. A complex signal at offset d from the tuned frequency goes through mix 1, the real-tap low-pass and
// mix 2 (a pure rotation), so its gain is |H(d + firstMix)| of the low-pass the chain uses (evaluated by a direct DFT of its taps) and its
// output frequency is pitch + s with s = d (Upper) or -d (Lower). The mapping d -> pitch + s is what testCarrierAtPitch measures through the
// whole chain; here the gain curve is walked in 1 Hz steps.
void edges(const CwTuning& t, CwSideband sb, double rate, double* lo, double* hi) {
    double tr = 0;
    const std::size_t taps = cwFilterTaps(rate, t.lowpassTransitionHz, &tr);
    const std::vector<float> h = windowedSincLowpass(taps, t.lowpassCutoffHz / rate, WindowType::Hamming);
    auto gain = [&](double f) {
        std::complex<double> a(0, 0);
        for (std::size_t n = 0; n < h.size(); ++n) { a += static_cast<double>(h[n]) * std::polar(1.0, -kTwoPi * f * static_cast<double>(n) / rate); }
        return std::abs(a);
    };
    const double step = 1.0;
    std::vector<double> g, audio;
    for (double s = -1.2 * t.widthHz; s <= 1.2 * t.widthHz + 1e-9; s += step) {
        const double d = (sb == CwSideband::Upper) ? s : -s;
        g.push_back(gain(d + t.firstMixHz));
        audio.push_back(t.pitchHz + s);
    }
    double peak = 0;
    for (double v : g) { peak = std::max(peak, v); }
    *lo = *hi = 0;
    for (std::size_t i = 1; i < g.size(); ++i) {
        if (g[i - 1] < 0.5 * peak && g[i] >= 0.5 * peak) { *lo = audio[i - 1] + (0.5 * peak - g[i - 1]) / (g[i] - g[i - 1]) * step; }
        if (g[i - 1] >= 0.5 * peak && g[i] < 0.5 * peak) { *hi = audio[i - 1] + (g[i - 1] - 0.5 * peak) / (g[i - 1] - g[i]) * step; }
    }
}

void testPassbandCentred() {
    struct C { double pitch, width; };
    const C cases[] = {{700, 500}, {700, 250}, {400, 300}, {1100, 1000}, {300, 100}, {1200, 600}, {900, 150}};
    double worstCentre = 0, worstWidth = 0;
    for (const C& c : cases) {
        for (CwSideband sb : {CwSideband::Upper, CwSideband::Lower}) {
            const CwTuning t = cwTuning(c.pitch, sb, c.width, 48000.0);
            double lo = 0, hi = 0;
            edges(t, sb, 48000.0, &lo, &hi);
            const double centre = 0.5 * (lo + hi), width = hi - lo;
            worstCentre = std::max(worstCentre, std::fabs(centre - t.pitchHz));
            worstWidth = std::max(worstWidth, std::fabs(width - t.widthHz) / t.widthHz);
            CHECK(lo > 0 && hi > 0);
            CHECK(std::fabs(centre - t.pitchHz) < 2.0);
            CHECK(std::fabs(width - t.widthHz) < 0.04 * t.widthHz + 2.0);
        }
    }
    std::printf("pass band: worst centre error %.3f Hz, worst width error %.2f %% (7 cases, both sidebands)\n", worstCentre, 100.0 * worstWidth);
}

// The EXISTING demodulator in CW mode.
void testExistingDemodulator() {
    const double rate = 48000.0;
    auto run = [&](double offset, double ssbBandwidth) {
        Demodulator d(rate);
        d.setSsbBandwidthHz(ssbBandwidth);
        d.setMode(DemodMode::CW);
        const std::vector<Cx> in = carrier(offset, rate, offset == 0.0 || offset == 150.0 ? 48000 : 16000);
        std::vector<float> out(in.size());
        d.process(in.data(), in.size(), out.data());
        return out;
    };
    const double f0 = peakHz(run(0.0, 3000.0), rate);
    std::printf("existing CW mode: carrier on the tuned frequency comes out at %.3f Hz\n", f0);
    CHECK(std::fabs(f0 - 700.0) < 1.0);
    const double f2 = peakHz(run(150.0, 3000.0), rate);
    CHECK(std::fabs(f2 - 850.0) < 1.0);  // upper-sideband sense, like the helper's Upper
    // its pass band, by the same -6 dB sweep (audio = 700 + d)
    for (double bw : {3000.0, 1000.0}) {
        std::vector<double> g, a;
        for (double d = -900.0; d <= 4500.0; d += 50.0) {
            g.push_back(rmsTail(run(d, bw), 8000));
            a.push_back(700.0 + d);
        }
        double peak = 0;
        for (double v : g) { peak = std::max(peak, v); }
        double lo = 0, hi = 0;
        for (std::size_t i = 1; i < g.size(); ++i) {
            if (g[i - 1] < 0.5 * peak && g[i] >= 0.5 * peak && lo == 0) { lo = a[i]; }
            if (g[i - 1] >= 0.5 * peak && g[i] < 0.5 * peak) { hi = a[i]; }
        }
        std::printf("existing CW mode, SSB width setting %.0f Hz: audio pass band %.0f .. %.0f Hz, centre %.0f Hz (the pitch is 700 Hz)\n", bw, lo, hi,
                    0.5 * (lo + hi));
        // at the default 3 kHz setting the voice filter is centred far above the pitch; its narrowest setting (1 kHz, the clamp) is
        // still about 1 kHz wide, 20 times wider than a 50 Hz CW filter
        if (bw > 2000.0) { CHECK(0.5 * (lo + hi) > 1500.0); }
        CHECK(hi - lo > 900.0);
    }
}

}  // namespace

int main() {
    testTable();
    testCarrierAtPitch();
    testPassbandCentred();
    testExistingDemodulator();
    return testSummary("test_cw_pitch");
}
