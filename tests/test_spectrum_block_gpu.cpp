// test_spectrum_block_gpu.cpp - THE GPU STAGE OF THE SPECTRUM BLOCK (dsp/spectrum_block_gpu.{hpp,cpp} inside dsp/spectrum_block.hpp; wire-b second pass, item 8).
//
//   B1  OFF by default: a 65 536-point block with a tone makes frames and finishes none on the GPU
//   B2  policy GPU on a hardware adapter: frames come from the Direct3D device (the counter moves), the tone sits at the CPU block's bin and its level (Tone scale) and the noise floor level (PSD
//       scale) are within 0.5 dB of a CPU block fed the same samples; on a machine with NO hardware adapter the stage never engages and the CPU frames go on (printed, the checks that name the GPU
//       are not counted)
//   B3  Auto leaves a 4 096-point block on the CPU (nothing is made, the counters stay 0)
//   B4  policy back to CPU: frames go on, the GPU counters stop
//   B5  an invalid policy value is CPU
//
// UNVERIFIED on every GPU but the one this was run on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "dsp/gpu_fft.hpp"
#include "dsp/spectrum_block.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 2.4e6;

// A tone exactly on bin `bin` of an n-point frame (fs/n spacing, DC at n/2) plus a deterministic noise floor.
struct Signal {
    std::size_t n;
    double toneHz;
    std::uint64_t k = 0, lcg = 99;
    explicit Signal(std::size_t fft, std::size_t bin) : n(fft), toneHz((static_cast<double>(bin) - static_cast<double>(fft) / 2.0) * kFs / static_cast<double>(fft)) {}
    void fill(std::vector<std::complex<float>>& v) {
        for (auto& s : v) {
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            const double a = (static_cast<double>((lcg >> 33) & 0xFFFF) / 65535.0 - 0.5) * 0.02;
            lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
            const double b = (static_cast<double>((lcg >> 33) & 0xFFFF) / 65535.0 - 0.5) * 0.02;
            const double ph = 2.0 * kPi * toneHz * static_cast<double>(k++) / kFs;
            s = {static_cast<float>(0.25 * std::cos(ph) + a), static_cast<float>(0.25 * std::sin(ph) + b)};
        }
    }
};

SpecConfig cfgOf(std::size_t n, SpecScale scale) {
    SpecConfig c;
    c.fftSize = n;
    c.window = SpecWindow::BlackmanHarris;
    c.sampleRateHz = kFs;
    c.frameRateHz = 30.0;
    c.mode = SpecMode::Instant;
    c.scale = scale;
    return c;
}

struct Frames {
    std::vector<float> db;
    std::uint64_t seq = 0;
    bool any = false;
};

// Feeds both blocks the same samples for up to `maxMs`, pulling frames, until `until` says stop.
template <class Until>
void feed(SpectrumBlock& a, SpectrumBlock* b, Signal& sig, Frames& fa, Frames* fb, int maxMs, Until until) {
    std::vector<std::complex<float>> chunk(65536);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> out(a.columns());
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < maxMs) {
        sig.fill(chunk);
        a.process(chunk.data(), chunk.size());
        if (b != nullptr) { b->process(chunk.data(), chunk.size()); }
        std::uint64_t s = 0;
        if (a.latest(out.data(), out.size(), &s) > 0) { fa.db = out; fa.seq = s; fa.any = true; }
        if (b != nullptr && fb != nullptr) {
            std::vector<float> o2(b->columns());
            if (b->latest(o2.data(), o2.size(), &s) > 0) { fb->db = o2; fb->seq = s; fb->any = true; }
        }
        if (until()) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

std::size_t peakBin(const std::vector<float>& v) { return static_cast<std::size_t>(std::max_element(v.begin(), v.end()) - v.begin()); }

double medianOf(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

bool hardwareAdapter() {
    for (const GpuAdapterInfo& a : enumerateGpuAdapters()) { if (!a.software) { return true; } }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_spectrum_block_gpu\n");
    constexpr std::size_t kBig = 65536;
    const bool hw = hardwareAdapter();
    std::printf("hardware adapter: %s\n", hw ? "yes" : "NO (the GPU checks are not counted)");

    std::printf("B1: off by default\n");
    {
        SpectrumBlock blk(cfgOf(kBig, SpecScale::Tone));
        Signal sig(kBig, 40000);
        Frames f;
        feed(blk, nullptr, sig, f, nullptr, 3000, [&] { return blk.framesProduced() >= 4; });
        std::printf("  %llu frames, on GPU %llu\n", static_cast<unsigned long long>(blk.framesProduced()), static_cast<unsigned long long>(blk.gpuFramesOnGpu()));
        CHECK(blk.framesProduced() >= 4);
        CHECK(blk.gpuFramesOnGpu() == 0 && blk.gpuFramesOnCpu() == 0 && !blk.gpuUp());
        CHECK(f.any && peakBin(f.db) == 40000);
    }

    for (const SpecScale scale : {SpecScale::Tone, SpecScale::Psd}) {
        std::printf("B2: policy GPU, scale %s\n", scale == SpecScale::Tone ? "tone" : "psd");
        SpectrumBlock gpuBlk(cfgOf(kBig, scale)), cpuBlk(cfgOf(kBig, scale));
        gpuBlk.setGpuPolicy(2);
        Signal sig(kBig, 40000);
        Frames fg, fc;
        feed(gpuBlk, &cpuBlk, sig, fg, &fc, 20000, [&] { return gpuBlk.gpuFramesOnGpu() >= 6 && fc.any; });
        const std::uint64_t onGpu = gpuBlk.gpuFramesOnGpu();
        std::printf("  GPU block: %llu frames, %llu finished on the device; CPU block %llu frames\n", static_cast<unsigned long long>(gpuBlk.framesProduced()), static_cast<unsigned long long>(onGpu),
                    static_cast<unsigned long long>(cpuBlk.framesProduced()));
        CHECK(fg.any && fc.any);
        if (hw) {
            CHECK(onGpu >= 6);
            CHECK(gpuBlk.gpuUp());
        } else {
            CHECK(onGpu == 0);   // no device: the stage never engages and the block stays on its CPU FFT
        }
        if (fg.any && fc.any) {
            CHECK(peakBin(fg.db) == 40000 && peakBin(fc.db) == 40000);
            const double dTone = std::fabs(static_cast<double>(fg.db[40000]) - static_cast<double>(fc.db[40000]));
            const double dFloor = std::fabs(medianOf(fg.db) - medianOf(fc.db));
            std::printf("  tone %.2f dB (cpu %.2f), floor median %.2f dB (cpu %.2f)\n", fg.db[40000], fc.db[40000], medianOf(fg.db), medianOf(fc.db));
            CHECK(dTone < 0.5);
            CHECK(dFloor < 0.5);
        }
    }

    std::printf("B3: Auto at 4096 points stays on the CPU\n");
    {
        SpectrumBlock blk(cfgOf(4096, SpecScale::Tone));
        blk.setGpuPolicy(1);
        Signal sig(4096, 1500);
        Frames f;
        feed(blk, nullptr, sig, f, nullptr, 1500, [&] { return blk.framesProduced() >= 20; });
        CHECK(blk.framesProduced() >= 20);
        CHECK(blk.gpuFramesOnGpu() == 0 && blk.gpuFramesOnCpu() == 0 && !blk.gpuUp());
        CHECK(f.any && peakBin(f.db) == 1500);
    }

    std::printf("B4: back to CPU\n");
    {
        SpectrumBlock blk(cfgOf(kBig, SpecScale::Tone));
        blk.setGpuPolicy(2);
        Signal sig(kBig, 40000);
        Frames f;
        feed(blk, nullptr, sig, f, nullptr, 20000, [&] { return hw ? blk.gpuFramesOnGpu() >= 3 : blk.framesProduced() >= 4; });
        blk.setGpuPolicy(0);
        const std::uint64_t gpuAt = blk.gpuFramesOnGpu(), cpuAt = blk.gpuFramesOnCpu(), framesAt = blk.framesProduced();
        feed(blk, nullptr, sig, f, nullptr, 8000, [&] { return blk.framesProduced() >= framesAt + 4; });
        std::printf("  frames %llu -> %llu, GPU counter %llu -> %llu\n", static_cast<unsigned long long>(framesAt), static_cast<unsigned long long>(blk.framesProduced()), static_cast<unsigned long long>(gpuAt),
                    static_cast<unsigned long long>(blk.gpuFramesOnGpu()));
        CHECK(blk.framesProduced() >= framesAt + 4);
        CHECK(blk.gpuFramesOnGpu() == gpuAt && blk.gpuFramesOnCpu() == cpuAt);
        CHECK(f.any && peakBin(f.db) == 40000);
    }

    std::printf("B5: an invalid policy is CPU\n");
    {
        SpectrumBlock blk(cfgOf(kBig, SpecScale::Tone));
        blk.setGpuPolicy(7);
        Signal sig(kBig, 40000);
        Frames f;
        feed(blk, nullptr, sig, f, nullptr, 3000, [&] { return blk.framesProduced() >= 4; });
        CHECK(blk.framesProduced() >= 4);
        CHECK(blk.gpuFramesOnGpu() == 0 && blk.gpuFramesOnCpu() == 0);
        blk.setGpuPolicy(-1);
        CHECK(blk.gpuFramesOnGpu() == 0);
    }
    return testSummary("test_spectrum_block_gpu");
}
