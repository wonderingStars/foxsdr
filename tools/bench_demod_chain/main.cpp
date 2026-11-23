// bench_demod_chain - where the demod phase's time goes, per sub-step, per mode (D:/foxsdr-build/prep/PERF-demod-2026-11-23.md section 6).
//
// A MEASURING TOOL, not a test and not shipped: EXCLUDE_FROM_ALL (build it by name: --target bench_demod_chain), not registered with ctest. It runs
// the receive chain's demod phase (tests/demod_chain.hpp: the pipeline's classes in the pipeline's order) on in-memory samples, in blocks of 1024, for a
// fixed number of seconds of signal, and prints microseconds per block for each sub-step (the VFO mixer, the channel filter, the stereo pilot mixer, the
// pilot filter, the PLL loop, the audio filter, the matrix, the RDS mixer, filter and baseband, the discriminator, the decoder tap's resampler, the host
// audio's two resamplers) plus the legacy spectrum estimator (window and FFT, and the dB conversion) per block.
//
// The sub-step timers are DSP_SUBSTEP (src/dsp/dsp_substep.hpp), compiled only into THIS target (it compiles the dsp sources it times itself with
// CASCADE_BENCH_SUBSTEPS defined); the shipped objects are untouched. A timer reads the cycle counter at the two ends of a step, once per step per block.
//
// Usage: bench_demod_chain [--seconds S] [--slices N] [--quick]
//   Every case is timed in N slices (default 5) of S/N seconds of signal each; the table shows the MEDIAN slice and the best. The input is rendered once
//   into memory before any timing, so the source costs nothing in the timed part. Everything printed is from THIS machine at THIS moment.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "demod_chain.hpp"
#include "dsp/dsp_substep.hpp"
#include "dsp/fir.hpp"
#include "dsp/nco.hpp"
#include "dsp/spectrum.hpp"
#include "dsp_signals.hpp"
#include "ref_dsp.hpp"
#include "source/siggen.hpp"

#if !defined(CASCADE_BENCH_SUBSTEPS)
#error "bench_demod_chain must be built with CASCADE_BENCH_SUBSTEPS (its CMake target does)"
#endif

namespace {

using cascade::dsp::DemodMode;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kBlock = 1024;

double nowNs() { return std::chrono::duration<double, std::nano>(Clock::now().time_since_epoch()).count(); }

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// The cycle counter's rate, measured against the steady clock over 200 ms.
double tscPerNs() {
    const double t0 = nowNs();
    const std::uint64_t c0 = __rdtsc();
    while (nowNs() - t0 < 2.0e8) {}
    const std::uint64_t c1 = __rdtsc();
    return static_cast<double>(c1 - c0) / (nowNs() - t0);
}

struct Input {
    std::vector<std::complex<float>> samples;
    double rateHz = 0.0;
};

Input renderGenerator(double rate, double seconds) {
    Input in;
    in.rateHz = rate;
    in.samples.resize(static_cast<std::size_t>(seconds * rate) / kBlock * kBlock);
    cascade::source::SigGen gen(rate);
    gen.setTone(0, 300000.0, -30.0f);   // the engine's default session (engine.cpp initialise)
    gen.setTone(1, -500000.0, -45.0f);
    gen.setNoiseFloorDb(-90.0f);
    gen.generate(in.samples.data(), in.samples.size());
    return in;
}

Input renderStereo(double rate, double seconds) {
    Input in;
    in.rateHz = rate;
    in.samples.resize(static_cast<std::size_t>(seconds * rate) / kBlock * kBlock);
    dspsig::Signal sig(dspsig::Signal::kStereoFm, rate);
    sig.fill(in.samples.data(), in.samples.size());
    return in;
}

struct StepStat {
    std::vector<double> usPerBlock;   // one entry per slice
};

struct CaseResult {
    std::map<std::string, StepStat> steps;   // by text
    std::vector<double> wholeUsPerBlock;     // a timing of the whole chain with the same instrumentation
    double blocksPerSlice = 0.0;
    double rateHz = 0.0;
};

CaseResult runCase(const Input& in, DemodMode mode, double seconds, int slices, double tscRate) {
    CaseResult res;
    res.rateHz = in.rateHz;
    demodchain::Chain chain(in.rateHz, mode, 300000.0, /*keep=*/false);
    const std::size_t totalBlocks = in.samples.size() / kBlock;
    // warm-up: the first fifth of the input (filters fill, locks happen), not timed
    std::size_t pos = 0;
    const std::size_t warm = std::min(totalBlocks / 5 + 1, totalBlocks);
    for (std::size_t b = 0; b < warm; ++b, ++pos) { chain.process(in.samples.data() + (pos % totalBlocks) * kBlock, kBlock); }
    const std::size_t perSlice = std::max<std::size_t>(1, static_cast<std::size_t>(seconds * in.rateHz / static_cast<double>(kBlock)) / static_cast<std::size_t>(slices));
    res.blocksPerSlice = static_cast<double>(perSlice);
    for (int s = 0; s < slices; ++s) {
        for (auto& slot : cascade::dsp::bench::g_substeps) {
            slot.cycles = 0;
            slot.calls = 0;
        }
        const double t0 = nowNs();
        for (std::size_t b = 0; b < perSlice; ++b, ++pos) { chain.process(in.samples.data() + (pos % totalBlocks) * kBlock, kBlock); }
        const double wall = nowNs() - t0;
        res.wholeUsPerBlock.push_back(wall / 1000.0 / static_cast<double>(perSlice));
        for (const auto& slot : cascade::dsp::bench::g_substeps) {
            if (slot.name == nullptr || slot.calls == 0) { continue; }
            res.steps[slot.name].usPerBlock.resize(static_cast<std::size_t>(s) + 1, 0.0);
            // (several literals can carry one text: they add)
            res.steps[slot.name].usPerBlock[static_cast<std::size_t>(s)] += static_cast<double>(slot.cycles) / tscRate / 1000.0 / static_cast<double>(perSlice);
        }
    }
    return res;
}

const char* modeLabel(DemodMode m) {
    switch (m) {
        case DemodMode::WFM: return "WFM";
        case DemodMode::NFM: return "NFM";
        case DemodMode::AM: return "AM";
        case DemodMode::USB: return "USB 3 kHz";
        default: return "?";
    }
}

void printCase(const char* title, const Input& in, const CaseResult& r) {
    const double blocksPerSecond = in.rateHz / static_cast<double>(kBlock);
    const double budgetUs = 1.0e6 * static_cast<double>(kBlock) / in.rateHz;
    const double whole = median(r.wholeUsPerBlock);
    std::printf("\n== %s: rate %.0f S/s, block %zu samples (budget %.0f us), %.0f blocks per slice\n", title, in.rateHz, kBlock, budgetUs, r.blocksPerSlice);
    std::printf("   %-22s %10s %10s %12s %8s\n", "step", "us/block", "best", "ms/s signal", "share");
    // the leaf steps (their parents are the *.total rows)
    static const char* kOrder[] = {"vfo.mix", "vfo.filter", "demod.discriminator", "demod.deemph", "ssb.shift_down", "ssb.filter", "ssb.shift_up", "rds.mix", "rds.filter", "rds.baseband", "stereo.pilot_mix",
                                   "stereo.pilot_filter", "stereo.pll", "stereo.audio_filter", "tap.resampler", "host.resamplers"};
    double leafSum = 0.0;
    for (const char* name : kOrder) {
        const auto it = r.steps.find(name);
        if (it == r.steps.end()) { continue; }
        const double med = median(it->second.usPerBlock);
        double best = it->second.usPerBlock.empty() ? 0.0 : *std::min_element(it->second.usPerBlock.begin(), it->second.usPerBlock.end());
        leafSum += med;
        std::printf("   %-22s %10.2f %10.2f %12.2f %7.1f%%\n", name, med, best, med * blocksPerSecond / 1000.0, whole > 0.0 ? 100.0 * med / whole : 0.0);
    }
    std::printf("   %-22s %10.2f %10s %12.2f\n", "sum of steps", leafSum, "", leafSum * blocksPerSecond / 1000.0);
    std::printf("   %-22s %10.2f %10.2f %12.2f   (whole chain timed in one piece; the difference is the rest of the chain and the timers' own cost)\n",
                "whole chain", whole, *std::min_element(r.wholeUsPerBlock.begin(), r.wholeUsPerBlock.end()), whole * blocksPerSecond / 1000.0);
}

// ---- kernel rows: old ns per operation, new ns per operation, on the same buffers in the same run -----------------------------------------

template <typename F>
double timeBest(F&& f, int repeats) {
    double best = 1e300;
    for (int i = 0; i < repeats; ++i) {
        const double t0 = nowNs();
        f();
        best = std::min(best, nowNs() - t0);
    }
    return best;
}

void kernelRows(double seconds) {
    std::printf("\n== kernels, on the same buffers in the same run (best of 5)\n");
    std::printf("   %-34s %14s %14s %8s\n", "FIR kernel", "old ns/MAC", "new ns/MAC", "factor");
    struct Row {
        const char* name;
        std::size_t taps, decim;
    };
    const Row rows[] = {{"channel 335 taps, decimation 10", 335, 10}, {"stereo pilot 185 taps, decimation 1", 185, 1},
                        {"stereo audio 165 taps, decimation 1", 165, 1}, {"rds 413 taps, decimation 10", 413, 10},
                        {"sideband 1101 taps, decimation 1", 1101, 1}};
    for (const Row& row : rows) {
        const std::size_t n = std::max<std::size_t>(kBlock * 64, static_cast<std::size_t>(seconds * 100000.0)) / kBlock * kBlock;
        dspsig::Rng rng(7);
        std::vector<std::complex<float>> in(n);
        for (auto& s : in) { s = {rng.uni(), rng.uni()}; }
        const std::vector<float> taps = cascade::dsp::windowedSincLowpass(row.taps, 0.1, cascade::dsp::WindowType::Hamming);
        std::vector<std::complex<float>> out(n / row.decim + 2);
        const double macs = static_cast<double>(n / row.decim) * static_cast<double>(row.taps);
        const double oldNs = timeBest(
            [&] {
                refdsp::RefFirDecimator f(taps, row.decim);
                for (std::size_t p = 0; p < n; p += kBlock) { f.process(in.data() + p, kBlock, out.data()); }
            },
            5);
        const double newNs = timeBest(
            [&] {
                cascade::dsp::FirDecimator f(taps, row.decim);
                for (std::size_t p = 0; p < n; p += kBlock) { f.process(in.data() + p, kBlock, out.data()); }
            },
            5);
        std::printf("   %-34s %14.3f %14.3f %7.2fx\n", row.name, oldNs / macs, newNs / macs, oldNs / newNs);
    }
    std::printf("   %-34s %14s %14s %8s\n", "oscillator", "old ns/sample", "new ns/sample", "factor");
    {
        const std::size_t n = kBlock * 1024;
        std::vector<std::complex<float>> in(n, {0.5f, -0.25f}), out(n);
        const double oldNs = timeBest(
            [&] {
                refdsp::RefNco nco;
                nco.setFrequency(0.15);
                for (std::size_t p = 0; p < n; p += kBlock) { nco.mix(in.data() + p, out.data() + p, kBlock); }
            },
            5);
        const double newNs = timeBest(
            [&] {
                cascade::dsp::Nco nco;
                nco.setFrequency(0.15);
                for (std::size_t p = 0; p < n; p += kBlock) { nco.mix(in.data() + p, out.data() + p, kBlock); }
            },
            5);
        std::printf("   %-34s %14.3f %14.3f %7.2fx\n", "mix (the VFO shape)", oldNs / static_cast<double>(n), newNs / static_cast<double>(n), oldNs / newNs);
    }
}

void spectrumRow(const Input& in, int slices) {
    cascade::dsp::SpectrumEstimator est(kBlock, cascade::dsp::WindowType::BlackmanHarris);
    est.setAlpha(0.5f);
    std::vector<float> db(kBlock), power(kBlock);
    const std::size_t totalBlocks = in.samples.size() / kBlock;
    std::vector<double> usFull, usPower, usDb;
    for (int s = 0; s < slices; ++s) {
        double t0 = nowNs();
        for (std::size_t b = 0; b < totalBlocks; ++b) { est.process(in.samples.data() + b * kBlock, db.data()); }
        usFull.push_back((nowNs() - t0) / 1000.0 / static_cast<double>(totalBlocks));
        t0 = nowNs();
        for (std::size_t b = 0; b < totalBlocks; ++b) { est.processPower(in.samples.data() + b * kBlock, power.data()); }
        usPower.push_back((nowNs() - t0) / 1000.0 / static_cast<double>(totalBlocks));
        t0 = nowNs();
        for (std::size_t b = 0; b < totalBlocks; ++b) { cascade::dsp::SpectrumEstimator::powerToDb(power.data(), db.data(), kBlock); }
        usDb.push_back((nowNs() - t0) / 1000.0 / static_cast<double>(totalBlocks));
    }
    std::printf("\n== legacy spectrum estimator (1024-point), per block, median: process() = power + dB %.2f us; processPower() (what the DSP thread runs "
                "now) %.2f us; powerToDb() (paid where a frame is pulled) %.2f us\n",
                median(usFull), median(usPower), median(usDb));
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    double seconds = 4.0;
    int slices = 5;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) { seconds = std::atof(argv[++i]); }
        else if (std::strcmp(argv[i], "--slices") == 0 && i + 1 < argc) { slices = std::max(1, std::atoi(argv[++i])); }
        else if (std::strcmp(argv[i], "--quick") == 0) { seconds = 1.0; slices = 3; }
    }
    const double tsc = tscPerNs();
    std::printf("bench_demod_chain: %.1f s of signal per case in %d slices; cycle counter %.3f GHz (against the steady clock)\n", seconds, slices, tsc);

    struct Case {
        const char* title;
        double rate;
        DemodMode mode;
        bool stereo;
    };
    const Case cases[] = {
        {"WFM, the default session (generator signal)", 2.0e6, DemodMode::WFM, false},
        {"WFM, a stereo multiplex with RDS (the pilot locks, the gate opens)", 2.0e6, DemodMode::WFM, true},
        {"NFM 12.5 kHz (generator signal)", 2.0e6, DemodMode::NFM, false},
        {"AM 10 kHz (generator signal)", 2.0e6, DemodMode::AM, false},
        {"USB 3 kHz (generator signal)", 2.0e6, DemodMode::USB, false},
        {"WFM stereo multiplex, 1 MS/s", 1.0e6, DemodMode::WFM, true},
        {"WFM stereo multiplex, 500 kS/s", 0.5e6, DemodMode::WFM, true},
    };
    for (const Case& c : cases) {
        const Input in = c.stereo ? renderStereo(c.rate, std::max(seconds, 2.0)) : renderGenerator(c.rate, std::max(seconds, 2.0));
        const CaseResult r = runCase(in, c.mode, seconds, slices, tsc);
        printCase(c.title, in, r);
    }
    {
        const Input in = renderGenerator(2.0e6, 2.0);
        spectrumRow(in, slices);
    }
    kernelRows(seconds);
    return 0;
}
