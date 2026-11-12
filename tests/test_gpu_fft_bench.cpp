// Timing of the GPU spectrum engine against the CPU path (dsp/gpu_fft.hpp). Prints raw numbers; the only checks are that the runs completed and
// the numbers are sane, so this is safe in the suite (default: short runs). GPUFFT_BENCH_FULL=1 in the environment lengthens every run.
//
// Per size: three timings of (a) the CPU path, per frame, wall and thread CPU time; (b) the GPU path single-frame round trip (submit to
// result in hand, spin-wait, so no timer granularity is counted); (c) the GPU path pipelined throughput with batches in flight, and the thread
// CPU time per frame it costs (polling by sleeping, as a real consumer would). GPU compute time per frame comes from the timestamp queries.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "dsp/gpu_fft.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;
using cf = std::complex<float>;
using Clock = std::chrono::steady_clock;

namespace {

double threadSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    auto v = [](const FILETIME& f) { return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return v(k) + v(u);
#else
    return 0.0;
#endif
}

double now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::vector<cf> makeFrames(std::size_t n, std::size_t frames) {
    std::vector<cf> x(n * frames);
    std::uint32_t s = 12345;
    auto r = [&] { s = s * 1664525u + 1013904223u; return static_cast<float>(s) * (2.0f / 4294967296.0f) - 1.0f; };
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < n; ++i) {
            const double a = 6.283185307179586 * 0.1234567 * static_cast<double>(i);
            x[f * n + i] = cf(static_cast<float>(0.7 * std::cos(a)) + 1e-3f * r(), static_cast<float>(0.7 * std::sin(a)) + 1e-3f * r());
        }
    }
    return x;
}

GpuFftConfig cfgFor(std::size_t n, GpuFftMode mode, std::size_t batch) {
    GpuFftConfig c;
    c.fftSize = n;
    c.mode = mode;
    c.maxBatch = batch;
    c.ringSlots = 4;
    c.gpuBudgetFraction = 1.0;  // measuring: no cap
    c.lowPriority = std::getenv("GPUFFT_BENCH_NOLOWPRIO") == nullptr;  // GPUFFT_BENCH_NOLOWPRIO=1: compare against normal GPU priority
    return c;
}

struct Cpu { double wallPerFrame; double threadPerFrame; };
struct Rt { double seconds; };
struct Thr { double fps; double threadPerFrame; double gpuPerFrame; };

Cpu measureCpu(std::size_t n, double runSeconds) {
    GpuSpectrumEngine e(cfgFor(n, GpuFftMode::Cpu, 1));
    const auto x = makeFrames(n, 1);
    std::vector<float> out(n);
    e.process(x.data(), 1, out.data());  // warm
    std::size_t frames = 0;
    const double w0 = now(), t0 = threadSeconds();
    while (now() - w0 < runSeconds) { e.process(x.data(), 1, out.data()); ++frames; }
    const double w1 = now(), t1 = threadSeconds();
    return {(w1 - w0) / static_cast<double>(frames), (t1 - t0) / static_cast<double>(frames)};
}

// Spin-wait collect of one batch.
bool spinCollect(GpuSpectrumEngine& e, GpuFftBatch& b, float* out, std::size_t cap, double timeout) {
    const double end = now() + timeout;
    while (now() < end) {
        if (e.collect(b, out, cap)) { return true; }
        std::this_thread::yield();
    }
    return false;
}

Rt measureRoundTrip(GpuSpectrumEngine& e, std::size_t n, double runSeconds) {
    const auto x = makeFrames(n, 1);
    std::vector<float> out(n);
    GpuFftBatch b;
    e.submit(x.data(), 1);
    spinCollect(e, b, out.data(), n, 5.0);  // warm
    std::size_t k = 0;
    const double w0 = now();
    while (now() - w0 < runSeconds || k < 5) {
        if (e.submit(x.data(), 1) == 0) { break; }
        if (!spinCollect(e, b, out.data(), n, 5.0)) { break; }
        ++k;
    }
    return {(now() - w0) / static_cast<double>(std::max<std::size_t>(k, 1))};
}

Thr measureThroughput(GpuSpectrumEngine& e, std::size_t n, std::size_t batch, double runSeconds) {
    const auto x = makeFrames(n, batch);
    std::vector<float> out(n * batch);
    GpuFftBatch b;
    const auto s0 = e.stats();
    std::size_t frames = 0;
    const double w0 = now(), t0 = threadSeconds();
    while (now() - w0 < runSeconds) {
        bool progressed = false;
        while (e.collect(b, out.data(), out.size())) { frames += b.frames; progressed = true; }
        if (e.submit(x.data(), batch) != 0) { progressed = true; }
        if (!progressed) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    // drain
    const double dend = now() + 5.0;
    while (now() < dend) {
        if (e.collect(b, out.data(), out.size())) { frames += b.frames; } else { break; }
    }
    const double w1 = now(), t1 = threadSeconds();
    const auto s1 = e.stats();
    Thr t;
    t.fps = static_cast<double>(frames) / (w1 - w0);
    t.threadPerFrame = (t1 - t0) / static_cast<double>(std::max<std::size_t>(frames, 1));
    t.gpuPerFrame = (s1.gpuSecondsTotal - s0.gpuSecondsTotal) / static_cast<double>(std::max<std::size_t>(frames, 1));
    return t;
}

}  // namespace

int main() {
    const bool full = std::getenv("GPUFFT_BENCH_FULL") != nullptr;
    const double run = full ? 1.5 : 0.25;
    const auto adapters = enumerateGpuAdapters();
    bool hw = false;
    for (const auto& a : adapters) { hw = hw || !a.software; }
    std::printf("bench: %s runs; adapters:", full ? "full" : "quick");
    for (const auto& a : adapters) { std::printf(" [%s%s]", a.name.c_str(), a.software ? " software" : ""); }
    std::printf("\n");

    std::vector<std::size_t> sizes = {1024, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288};
    if (const char* only = std::getenv("GPUFFT_BENCH_SIZES")) {  // e.g. 8192,32768
        sizes.clear();
        for (const char* q = only; *q != '\0';) {
            sizes.push_back(static_cast<std::size_t>(std::strtoull(q, nullptr, 10)));
            while (*q != '\0' && *q != ',') { ++q; }
            if (*q == ',') { ++q; }
        }
    }
    struct Row { std::size_t n; double cpuUs; double cpuThreadUs; double gpuThreadUs; double gpuFps; double cpuFps; };
    std::vector<Row> rows;
    for (std::size_t n : sizes) {
        const std::size_t batch = n >= 262144 ? 4 : 8;
        Cpu c[3];
        for (auto& v : c) { v = measureCpu(n, run); }
        std::printf("N=%zu CPU per frame (wall us, thread-CPU us) x3: (%.1f, %.1f) (%.1f, %.1f) (%.1f, %.1f)\n", n, c[0].wallPerFrame * 1e6,
                    c[0].threadPerFrame * 1e6, c[1].wallPerFrame * 1e6, c[1].threadPerFrame * 1e6, c[2].wallPerFrame * 1e6, c[2].threadPerFrame * 1e6);
        for (const auto& v : c) { CHECK(v.wallPerFrame > 0.0 && std::isfinite(v.wallPerFrame)); }
        if (!hw) { ++g_checksSkipped; std::printf("SKIP: no hardware adapter\n"); continue; }
        GpuSpectrumEngine e(cfgFor(n, GpuFftMode::Gpu, batch));
        if (e.backend() != GpuFftBackend::Gpu) { ++g_checksSkipped; std::printf("SKIP: GPU path did not come up: %s\n", e.lastError().c_str()); continue; }
        Rt r[3];
        for (auto& v : r) { v = measureRoundTrip(e, n, run); }
        std::printf("N=%zu GPU 1-frame round trip (us) x3: %.1f %.1f %.1f\n", n, r[0].seconds * 1e6, r[1].seconds * 1e6, r[2].seconds * 1e6);
        Thr t[3];
        for (auto& v : t) { v = measureThroughput(e, n, batch, run); }
        std::printf("N=%zu GPU pipelined batch=%zu: frames/s x3: %.0f %.0f %.0f; thread-CPU us/frame x3: %.1f %.1f %.1f; GPU-compute us/frame x3: %.1f %.1f %.1f\n", n,
                    batch, t[0].fps, t[1].fps, t[2].fps, t[0].threadPerFrame * 1e6, t[1].threadPerFrame * 1e6, t[2].threadPerFrame * 1e6,
                    t[0].gpuPerFrame * 1e6, t[1].gpuPerFrame * 1e6, t[2].gpuPerFrame * 1e6);
        for (const auto& v : t) { CHECK(v.fps > 0.0 && std::isfinite(v.fps)); }
        std::sort(t, t + 3, [](const Thr& a, const Thr& b) { return a.fps < b.fps; });
        std::sort(c, c + 3, [](const Cpu& a, const Cpu& b) { return a.wallPerFrame < b.wallPerFrame; });
        std::vector<double> tp{t[0].threadPerFrame, t[1].threadPerFrame, t[2].threadPerFrame};
        std::sort(tp.begin(), tp.end());
        rows.push_back({n, c[1].wallPerFrame * 1e6, c[1].threadPerFrame * 1e6, tp[1] * 1e6, t[1].fps, 1.0 / c[1].wallPerFrame});
    }

    std::printf("\nMEDIANS: N, CPU us/frame (wall), CPU thread us/frame, GPU thread-CPU us/frame, CPU frames/s, GPU frames/s, GPU/CPU throughput\n");
    for (const Row& r : rows) {
        std::printf("%zu, %.1f, %.1f, %.1f, %.0f, %.0f, %.2fx\n", r.n, r.cpuUs, r.cpuThreadUs, r.gpuThreadUs, r.cpuFps, r.gpuFps, r.gpuFps / r.cpuFps);
    }
    std::printf("\nCPU LOAD (percent of one core) to analyse every sample with no overlap (frames/s = fs / N); '-' = the path cannot keep up\n");
    for (double fs : {2.4e6, 10e6}) {
        for (const Row& r : rows) {
            if (r.n != 1024 && r.n != 4096 && r.n != 65536) { continue; }
            const double fps = fs / static_cast<double>(r.n);
            const double cpuLoad = fps * r.cpuThreadUs * 1e-6 * 100.0;
            const double gpuLoad = fps * r.gpuThreadUs * 1e-6 * 100.0;
            std::printf("fs=%.1f MS/s N=%zu frames/s=%.0f: CPU path %.1f%%%s, GPU path %.1f%%%s\n", fs / 1e6, r.n, fps, cpuLoad,
                        fps > r.cpuFps ? " (-)" : "", gpuLoad, fps > r.gpuFps ? " (-)" : "");
        }
    }
    return testSummary("test_gpu_fft_bench");
}
