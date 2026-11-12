// test_spectrum_cost.cpp - THE CPU COST OF THE SPECTRUM CHOICE (engine serial lane A, row S1): one second of input at 2.4 MS/s through the legacy estimator (1,024 bins, a frame per
// block) and through the spectrum block at 65,536 and 131,072 bins at the automatic 30 frames a second; each measured three times and reported as the share of ONE core the
// DSP thread would spend at that input rate in real time. The bound is deliberately loose (a loaded machine runs this): the budget it guards is "a 65,536-point spectrum costs
// less than half a core at 2.4 MS/s", which the numbers printed here are the evidence for.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "dsp/spectrum.hpp"
#include "dsp/spectrum_block.hpp"
#include "test_check.hpp"

namespace {

constexpr double kRate = 2400000.0;
constexpr std::size_t kBlock = 1024;   // the DSP thread's block

std::vector<std::complex<float>> oneSecond() {
    std::vector<std::complex<float>> v(static_cast<std::size_t>(kRate));
    for (std::size_t i = 0; i < v.size(); ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * 0.01 * static_cast<double>(i);
        v[i] = std::complex<float>(static_cast<float>(0.5 * std::cos(ph) + 0.001 * std::sin(1.7 * ph * ph * 1e-6)), static_cast<float>(0.5 * std::sin(ph)));
    }
    return v;
}

double secondsOf(const std::chrono::steady_clock::time_point& a) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count(); }

}  // namespace

int main() {
    std::printf("test_spectrum_cost\n");
    const std::vector<std::complex<float>> in = oneSecond();
    double legacy[3] = {}, b64[3] = {}, b128[3] = {};
    for (int rep = 0; rep < 3; ++rep) {
        {
            cascade::dsp::SpectrumEstimator est(kBlock, cascade::dsp::WindowType::BlackmanHarris);
            est.setAlpha(0.5f);
            std::vector<float> db(kBlock);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i + kBlock <= in.size(); i += kBlock) { est.process(in.data() + i, db.data()); }
            legacy[rep] = secondsOf(t0);
        }
        for (const std::size_t n : {std::size_t{65536}, std::size_t{131072}}) {
            cascade::dsp::SpecConfig c;
            c.fftSize = n;
            c.window = cascade::dsp::SpecWindow::BlackmanHarris;
            c.sampleRateHz = kRate;
            c.frameRateHz = 30.0;
            c.mode = cascade::dsp::SpecMode::Average;
            c.alpha = 0.5f;
            cascade::dsp::SpectrumBlock blk(c);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i + kBlock <= in.size(); i += kBlock) { blk.process(in.data() + i, kBlock); }
            (n == 65536 ? b64 : b128)[rep] = secondsOf(t0);
        }
    }
    std::printf("  share of one core at 2.4 MS/s real time (three runs each):\n");
    std::printf("    legacy 1024 bins, a frame per block : %5.1f %%  %5.1f %%  %5.1f %%\n", legacy[0] * 100, legacy[1] * 100, legacy[2] * 100);
    std::printf("    block 65536 bins at 30 frames/s     : %5.1f %%  %5.1f %%  %5.1f %%\n", b64[0] * 100, b64[1] * 100, b64[2] * 100);
    std::printf("    block 131072 bins at 30 frames/s    : %5.1f %%  %5.1f %%  %5.1f %%\n", b128[0] * 100, b128[1] * 100, b128[2] * 100);
    double best64 = b64[0], best128 = b128[0];
    for (int i = 1; i < 3; ++i) {
        best64 = std::fmin(best64, b64[i]);
        best128 = std::fmin(best128, b128[i]);
    }
    CHECK(best64 < 0.5);    // the budget: under half a core at 2.4 MS/s
    CHECK(best128 < 1.0);   // and the ceiling still keeps up in real time
    return testSummary("test_spectrum_cost");
}
