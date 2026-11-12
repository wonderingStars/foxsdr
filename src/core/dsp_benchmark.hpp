// dsp_benchmark - times the existing DSP blocks on THIS machine and reports samples per second and real-time multiples.
//
// STANDALONE, header-only; nothing is wired. It is used by tests/test_dsp_benchmark.cpp (a program registered as a test: it passes when it runs
// to the end, never on a speed) and could be called by a command-line option (hand-off in docs/feature-records/core-formats.md).
//
// WHAT IS TIMED (each against a stated reference rate, so the multiple means something):
//   FFT      dsp::ComplexFFT forward, sizes 1024 .. 262144 (powers of two) and 3072, 15360 (the 2^a 3^b 5^c sizes the class allows); reference
//            2.4 MS/s (an RTL-SDR's usual rate): the multiple is how many such streams one core could transform back to back with no overlap.
//   Channel  dsp::Vfo (mixer + decimating filter chain) at 2.4 MS/s in -> 48 kHz channel (decimation 50), 10 MS/s in -> 50 kHz (decimation 200)
//            and 20 MS/s in -> 200 kHz (decimation 100); the reference is the INPUT rate.
//   Demod    dsp::Demodulator in every mode, at the channel rate that mode normally runs at (WFM 250 kHz, the rest 48 kHz), complex samples in,
//            audio out; the reference is the channel rate.
//   Resample dsp::RationalResampler on real audio: 48000 -> 44100 (147/160), 250000 -> 48000 (24/125), 200000 -> 48000 (6/25), 16 taps per phase;
//            the reference is the input rate.
// A "sample" is one input sample (complex for FFT / Channel / Demod, real for Resample). The multiple is samples per second divided by the
// reference rate. Timing: a warm-up, then `slices` slices of `secondsPerItem / slices` each; the MEDIAN slice is reported and the best one
// beside it (the machine is shared and loaded, so the gap between them is the noise).
//
// A block that throws is reported with ok = false and its message, and the run goes on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "dsp/demod.hpp"
#include "dsp/fft.hpp"
#include "dsp/resampler.hpp"
#include "dsp/vfo.hpp"

namespace cascade::core::bench {

struct BenchOptions {
    double secondsPerItem = 0.2;     // wall time spent on each item (over all slices)
    int slices = 3;                  // timed slices per item (median reported)
};

struct BenchResult {
    std::string group;               // "FFT", "Channel", "Demod", "Resample"
    std::string name;                // "forward 65536", "Vfo 2.4 MS/s -> 48 kHz", "NFM", ...
    bool ok = false;
    std::string error;
    double samplesPerSec = 0.0;      // median slice
    double bestSamplesPerSec = 0.0;  // fastest slice
    double referenceRateHz = 0.0;
    double realTimeMultiple = 0.0;   // samplesPerSec / referenceRateHz
    std::uint64_t samples = 0;       // processed in the timed slices
    double seconds = 0.0;            // spent in the timed slices
};

namespace detail {

inline std::vector<std::complex<float>> noise(std::size_t n, unsigned seed) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<std::complex<float>> v(n);
    for (auto& x : v) { x = {u(g), u(g)}; }
    return v;
}

using Clock = std::chrono::steady_clock;

// The middle value (the upper middle for an even count) of a non-empty list.
inline double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Runs `step` (which processes `perStep` samples and returns a checksum contribution) for the requested time. Returns false if it never ran.
template <typename Step>
bool timeIt(const BenchOptions& o, std::size_t perStep, Step&& step, BenchResult* r) {
    volatile double sink = 0.0;
    for (int i = 0; i < 3; ++i) { sink = sink + step(); }   // warm-up: caches, branch predictors, first-block set-up
    const int slices = std::max(1, o.slices);
    const double sliceSec = std::max(0.001, o.secondsPerItem) / slices;
    std::vector<double> rates;
    std::uint64_t total = 0;
    double totalSec = 0.0;
    for (int s = 0; s < slices; ++s) {
        std::uint64_t n = 0;
        const auto t0 = Clock::now();
        double elapsed = 0.0;
        do {
            for (int k = 0; k < 8; ++k) { sink = sink + step(); n += perStep; }
            elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        } while (elapsed < sliceSec);
        rates.push_back(static_cast<double>(n) / elapsed);
        total += n;
        totalSec += elapsed;
    }
    r->samplesPerSec = median(rates);
    r->bestSamplesPerSec = *std::max_element(rates.begin(), rates.end());
    r->samples = total;
    r->seconds = totalSec;
    r->realTimeMultiple = r->referenceRateHz > 0.0 ? r->samplesPerSec / r->referenceRateHz : 0.0;
    r->ok = std::isfinite(r->samplesPerSec) && r->samplesPerSec > 0.0 && std::isfinite(static_cast<double>(sink));
    if (!r->ok && r->error.empty()) { r->error = "no usable timing"; }
    return r->ok;
}

inline BenchResult fftItem(std::size_t n, const BenchOptions& o) {
    BenchResult r;
    r.group = "FFT";
    r.name = "forward " + std::to_string(n);
    r.referenceRateHz = 2.4e6;
    try {
        dsp::ComplexFFT fft(n);
        const auto in = noise(n, 1);
        std::vector<std::complex<float>> out(n);
        timeIt(o, n, [&] { fft.forward(in.data(), out.data()); return static_cast<double>(out[n / 3].real()) * 1e-30; }, &r);
    } catch (const std::exception& e) { r.ok = false; r.error = e.what(); }
    return r;
}

inline BenchResult vfoItem(double inHz, unsigned decim, double bwHz, const char* label, const BenchOptions& o) {
    BenchResult r;
    r.group = "Channel";
    r.name = std::string("Vfo ") + label;
    r.referenceRateHz = inHz;
    try {
        dsp::Vfo vfo(inHz, decim, bwHz);
        vfo.setOffsetHz(inHz * 0.05);
        const std::size_t block = 65536;
        const auto in = noise(block, 2);
        std::vector<std::complex<float>> out(block / decim + 64);
        timeIt(o, block, [&] { const std::size_t n = vfo.process(in.data(), block, out.data(), out.size()); return n > 0 ? static_cast<double>(out[n - 1].real()) * 1e-30 : 0.0; }, &r);
    } catch (const std::exception& e) { r.ok = false; r.error = e.what(); }
    return r;
}

inline BenchResult demodItem(dsp::DemodMode mode, const char* name, double rateHz, const BenchOptions& o) {
    BenchResult r;
    r.group = "Demod";
    r.name = std::string(name) + " @ " + std::to_string(static_cast<long>(rateHz / 1000)) + " kHz";
    r.referenceRateHz = rateHz;
    try {
        dsp::Demodulator d(rateHz);
        d.setMode(mode);
        const std::size_t block = 16384;
        const auto in = noise(block, 3);
        std::vector<float> out(block * 2);
        timeIt(o, block, [&] { const std::size_t n = d.process(in.data(), block, out.data()); return n > 0 ? static_cast<double>(out[n - 1]) * 1e-30 : 0.0; }, &r);
    } catch (const std::exception& e) { r.ok = false; r.error = e.what(); }
    return r;
}

inline BenchResult resampleItem(unsigned interp, unsigned decim, double inHz, const char* label, const BenchOptions& o) {
    BenchResult r;
    r.group = "Resample";
    r.name = std::string("Rational ") + label;
    r.referenceRateHz = inHz;
    try {
        dsp::RationalResampler rs(interp, decim, 16);
        const std::size_t block = 16384;
        std::vector<float> in(block);
        {
            std::mt19937 g(4);
            std::uniform_real_distribution<float> u(-0.5f, 0.5f);
            for (auto& x : in) { x = u(g); }
        }
        std::vector<float> out(rs.maxOut(block) + 16);
        timeIt(o, block, [&] { const std::size_t n = rs.process(in.data(), block, out.data(), out.size()); return n > 0 ? static_cast<double>(out[n - 1]) * 1e-30 : 0.0; }, &r);
    } catch (const std::exception& e) { r.ok = false; r.error = e.what(); }
    return r;
}

}  // namespace detail

inline std::vector<BenchResult> runDspBenchmark(const BenchOptions& o = BenchOptions{}) {
    std::vector<BenchResult> all;
    for (std::size_t n : {1024u, 4096u, 16384u, 65536u, 262144u, 3072u, 15360u}) { all.push_back(detail::fftItem(n, o)); }
    all.push_back(detail::vfoItem(2.4e6, 50, 12500.0, "2.4 MS/s -> 48 kHz", o));
    all.push_back(detail::vfoItem(10.0e6, 200, 25000.0, "10 MS/s -> 50 kHz", o));
    all.push_back(detail::vfoItem(20.0e6, 100, 150000.0, "20 MS/s -> 200 kHz", o));
    struct M { dsp::DemodMode m; const char* n; double rate; };
    for (const M& m : {M{dsp::DemodMode::NFM, "NFM", 48000.0}, M{dsp::DemodMode::WFM, "WFM", 250000.0}, M{dsp::DemodMode::AM, "AM", 48000.0}, M{dsp::DemodMode::DSB, "DSB", 48000.0},
                       M{dsp::DemodMode::USB, "USB", 48000.0}, M{dsp::DemodMode::LSB, "LSB", 48000.0}, M{dsp::DemodMode::CW, "CW", 48000.0}, M{dsp::DemodMode::RAW, "RAW", 48000.0}}) {
        all.push_back(detail::demodItem(m.m, m.n, m.rate, o));
    }
    all.push_back(detail::resampleItem(147, 160, 48000.0, "48000 -> 44100 (147/160)", o));
    all.push_back(detail::resampleItem(24, 125, 250000.0, "250000 -> 48000 (24/125)", o));
    all.push_back(detail::resampleItem(6, 25, 200000.0, "200000 -> 48000 (6/25)", o));
    return all;
}

// A fixed-width table, one line per item (a "samples" figure is in mega-samples per second).
inline std::string formatTable(const std::vector<BenchResult>& rs) {
    std::string out;
    char line[200];
    std::snprintf(line, sizeof line, "%-9s %-34s %12s %12s %12s %10s\n", "group", "item", "median MS/s", "best MS/s", "reference", "x real time");
    out += line;
    for (const BenchResult& r : rs) {
        if (!r.ok) {
            std::snprintf(line, sizeof line, "%-9s %-34s FAILED: %s\n", r.group.c_str(), r.name.c_str(), r.error.c_str());
        } else {
            std::snprintf(line, sizeof line, "%-9s %-34s %12.2f %12.2f %9.3f MS/s %10.1f\n", r.group.c_str(), r.name.c_str(), r.samplesPerSec / 1e6, r.bestSamplesPerSec / 1e6, r.referenceRateHz / 1e6,
                          r.realTimeMultiple);
        }
        out += line;
    }
    return out;
}

}  // namespace cascade::core::bench
