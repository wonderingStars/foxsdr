// test_vfo_shape.cpp - THE CHANNEL FILTER'S SHAPE IN THE VFO (engine serial lane A, row F6): Vfo::setChannelShape.
//
// A 6 kHz channel at a 2.4 MS/s input (channel rate 200 kHz, so the narrow stage exists) is swept with tones and the response read off the output: the -6 dB width is the bandwidth asked for (within 10 %)
// in every shape, and the shape factor (width at -60 dB over width at -6 dB) is 1.3 for sharp, 1.8 for medium and 3.157 for soft, within the tolerance the design helper itself states; the default shape
// (0) is the filter the VFO has always had and has none of the three. A wide channel has no narrow stage: the shape is accepted and does not apply (channelShapeActive() false) and the filter is unchanged.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "dsp/cw_filter.hpp"
#include "dsp/vfo.hpp"
#include "test_check.hpp"

using cascade::dsp::Vfo;

namespace {

constexpr double kRate = 2400000.0;
constexpr double kPi = 3.14159265358979323846;

// The gain, in dB against a tone at 0 Hz, of a tone `f` Hz from the tuned frequency, read from the output after the filter has settled.
double gainDb(Vfo& v, double f, double ref) {
    const std::size_t n = 120000;
    std::vector<std::complex<float>> in(n), out(n / 12 + 8);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * f * static_cast<double>(i) / kRate;
        in[i] = std::complex<float>(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    const std::size_t m = v.process(in.data(), n, out.data(), out.size());
    double p = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = m / 2; i < m; ++i, ++cnt) { p += std::norm(out[i]); }
    return 10.0 * std::log10(std::max(p / static_cast<double>(std::max<std::size_t>(cnt, 1)), 1e-30)) - ref;
}

// The frequency above 0 where the response first falls below `levelDb`, found by a coarse sweep and a bisection.
double crossing(Vfo& v, double ref, double levelDb, double stepHz, double maxHz) {
    double lo = 0.0;
    for (double f = stepHz; f <= maxHz; f += stepHz) {
        if (gainDb(v, f, ref) < levelDb) {
            double a = lo, b = f;
            for (int i = 0; i < 12; ++i) {
                const double mid = 0.5 * (a + b);
                if (gainDb(v, mid, ref) < levelDb) { b = mid; } else { a = mid; }
            }
            return 0.5 * (a + b);
        }
        lo = f;
    }
    return maxHz;
}

struct Shape { double w6, w60; };
Shape measure(Vfo& v) {
    const double ref = gainDb(v, 0.0, 0.0);
    return {2.0 * crossing(v, ref, -6.0, 250.0, 40000.0), 2.0 * crossing(v, ref, -60.0, 500.0, 90000.0)};
}

}  // namespace

int main() {
    std::printf("test_vfo_shape\n");
    {
        const auto d = cascade::dsp::designChannelFilter(200000.0, 6000.0, cascade::dsp::ChannelShape::Soft);
        std::printf("  design helper, soft at 200 kHz / 6 kHz: %zu taps, bw6 %.0f, bw60 %.0f, factor %.3f, capped %d\n", d.taps.size(), d.bw6Hz, d.bw60Hz, d.shapeFactor, d.capped ? 1 : 0);
        CHECK(!d.taps.empty());
    }
    const double bw = 6000.0;
    const double want[4] = {0.0, 1.3, 1.8, 3.157};
    Shape got[4];
    for (int s = 0; s <= 3; ++s) {
        Vfo v(kRate, 12, bw);
        CHECK(v.setChannelShape(s));
        CHECK(v.channelShape() == s);
        CHECK(v.channelShapeActive() == (s != 0));
        got[s] = measure(v);
        std::printf("  shape %d: %.5f multiply-adds per input sample\n", s, v.macsPerInputSample());
        std::printf("  shape %d: -6 dB width %.0f Hz, -60 dB width %.0f Hz, factor %.3f\n", s, got[s].w6, got[s].w60, got[s].w60 / got[s].w6);
    }
    for (int s = 1; s <= 3; ++s) {
        const double factor = got[s].w60 / got[s].w6;
        CHECK(std::fabs(got[s].w6 - bw) < 0.10 * bw);
        CHECK(std::fabs(factor - want[s]) < (s == 3 ? 0.4 : 0.2));
    }
    CHECK(got[1].w60 / got[1].w6 < got[2].w60 / got[2].w6 && got[2].w60 / got[2].w6 < got[3].w60 / got[3].w6);   // sharp < medium < soft
    // the default is its own filter and, measured, a soft one (a Blackman-Harris windowed sinc is nearly Gaussian): -6 dB at the bandwidth, a shape factor near 3.16 - the outputs below prove it is not the soft design
    CHECK(std::fabs(got[0].w6 - bw) < 0.10 * bw);
    CHECK(got[0].w60 / got[0].w6 > 2.9 && got[0].w60 / got[0].w6 < 3.4);

    // each shape is a different filter: the same noise through two of them comes out different
    {
        std::vector<std::complex<float>> in(24000), a(3000), b(3000);
        unsigned long long x = 12345;
        for (auto& v : in) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            const float re = static_cast<float>((x >> 33) & 0xFFFF) / 32768.0f - 1.0f;
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            v = {re, static_cast<float>((x >> 33) & 0xFFFF) / 32768.0f - 1.0f};
        }
        for (int s1 = 0; s1 < 4; ++s1) {
            for (int s2 = s1 + 1; s2 < 4; ++s2) {
                Vfo v1(kRate, 12, bw), v2(kRate, 12, bw);
                v1.setChannelShape(s1);
                v2.setChannelShape(s2);
                const std::size_t n1 = v1.process(in.data(), in.size(), a.data(), a.size());
                const std::size_t n2 = v2.process(in.data(), in.size(), b.data(), b.size());
                double diff = 0.0;
                for (std::size_t i = 0; i < std::min(n1, n2); ++i) { diff = std::max(diff, static_cast<double>(std::abs(a[i] - b[i]))); }
                std::printf("  shapes %d and %d: largest difference between the outputs %.4f\n", s1, s2, diff);
                CHECK(diff > 1e-3);
            }
        }
    }

    // refusals and a wide channel
    Vfo v(kRate, 12, bw);
    CHECK(!v.setChannelShape(-1) && !v.setChannelShape(4));
    CHECK(v.channelShape() == 0);
    Vfo wide(kRate, 12, 150000.0);
    CHECK(wide.setChannelShape(2));
    CHECK(!wide.channelShapeActive());
    const double refA = gainDb(wide, 0.0, 0.0);
    Vfo wide2(kRate, 12, 150000.0);
    CHECK(std::fabs(gainDb(wide, 60000.0, refA) - gainDb(wide2, 60000.0, gainDb(wide2, 0.0, 0.0))) < 0.01);   // a wide channel: tap for tap the filter it was
    // changing the bandwidth keeps the shape
    Vfo keep(kRate, 12, bw);
    CHECK(keep.setChannelShape(1));
    keep.setBandwidthHz(3000.0);
    CHECK(keep.channelShape() == 1 && keep.channelShapeActive());
    keep.setBandwidthHz(150000.0);
    CHECK(keep.channelShape() == 1 && !keep.channelShapeActive());
    return testSummary("test_vfo_shape");
}
