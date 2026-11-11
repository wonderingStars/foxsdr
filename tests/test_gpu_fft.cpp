// Tests for dsp/gpu_fft.hpp: the batched GPU spectrum engine (Direct3D 11 compute) and its CPU fallback.
//
// The GPU is held against two references: the existing CPU path (dsp::SpectrumEstimator with alpha = 1, which the CPU backend must equal
// bit for bit) and a direct DFT in double precision computed in the test (small sizes). Where there is no hardware adapter the GPU lines
// are SKIPPED and printed as skipped, never passed silently. A real TDR is never provoked on a shared display: the device-loss path is
// driven through the engine's test hook, which runs the genuine loss handling (see the record).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/gpu_fft.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"
#include "test_check.hpp"

using namespace cascade::dsp;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
using cf = std::complex<float>;

struct Lcg {
    std::uint32_t s;
    explicit Lcg(std::uint32_t seed) : s(seed) {}
    float next() {  // uniform [-1, 1)
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s) * (2.0f / 4294967296.0f) - 1.0f;
    }
};

// Two tones off the bin grid (a frequency offset), a sample-rate error folded into the second, and noise.
std::vector<cf> signalFrame(std::size_t n, std::uint32_t seed, float noise) {
    Lcg r(seed);
    std::vector<cf> x(n);
    const double f1 = 0.1234567 + 0.01 * (seed % 7);
    const double f2 = -0.3171 * (1.0 + 50e-6);  // 50 ppm sample-rate error
    for (std::size_t i = 0; i < n; ++i) {
        const double a1 = kTwoPi * f1 * static_cast<double>(i);
        const double a2 = kTwoPi * f2 * static_cast<double>(i);
        x[i] = cf(static_cast<float>(0.7 * std::cos(a1) + 0.2 * std::cos(a2)) + noise * r.next(),
                  static_cast<float>(0.7 * std::sin(a1) + 0.2 * std::sin(a2)) + noise * r.next());
    }
    return x;
}

std::vector<cf> noiseFrame(std::size_t n, std::uint32_t seed) {
    Lcg r(seed);
    std::vector<cf> x(n);
    for (auto& v : x) { v = cf(r.next(), r.next()); }
    return x;
}

std::vector<cf> toneFrame(std::size_t n, double cycles, float amp) {
    std::vector<cf> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = kTwoPi * cycles * static_cast<double>(i) / static_cast<double>(n);
        x[i] = cf(static_cast<float>(amp * std::cos(a)), static_cast<float>(amp * std::sin(a)));
    }
    return x;
}

std::vector<cf> concat(const std::vector<std::vector<cf>>& v) {
    std::vector<cf> out;
    for (const auto& f : v) { out.insert(out.end(), f.begin(), f.end()); }
    return out;
}

double dbToPow(double db) { return std::pow(10.0, db / 10.0); }

struct Err {
    double maxRelToPeak = 0.0;  // max |P_gpu - P_ref| / P_peak(ref), linear power
    double maxDbAbove = 0.0;    // max |dB diff| over bins at least 80 dB above the frame's own peak... below it (P_ref > peak * 1e-8)
};

Err compare(const float* got, const float* ref, std::size_t n) {
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) { peak = std::max(peak, dbToPow(ref[i])); }
    Err e;
    for (std::size_t i = 0; i < n; ++i) {
        const double pg = dbToPow(got[i]);
        const double pr = dbToPow(ref[i]);
        e.maxRelToPeak = std::max(e.maxRelToPeak, std::fabs(pg - pr) / peak);
        if (pr > peak * 1e-8) { e.maxDbAbove = std::max(e.maxDbAbove, std::fabs(static_cast<double>(got[i]) - static_cast<double>(ref[i]))); }
    }
    return e;
}

// Direct DFT in double (the independent reference), windowed, fftshifted, in dB with the documented floor.
std::vector<float> directDb(const std::vector<cf>& x, WindowType wt) {
    const std::size_t n = x.size();
    const auto w = makeWindow(wt, n);
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t k = (i + n / 2) % n;
        std::complex<double> acc(0, 0);
        for (std::size_t m = 0; m < n; ++m) {
            const double ang = -kTwoPi * static_cast<double>((k * m) % n) / static_cast<double>(n);
            acc += std::complex<double>(x[m].real() * static_cast<double>(w[m]), x[m].imag() * static_cast<double>(w[m])) *
                   std::complex<double>(std::cos(ang), std::sin(ang));
        }
        const double p = std::norm(acc) / (static_cast<double>(n) * static_cast<double>(n));
        out[i] = static_cast<float>(10.0 * std::log10(p > 1e-20 ? p : 1e-20));
    }
    return out;
}

GpuFftConfig baseCfg(std::size_t n, GpuFftMode mode) {
    GpuFftConfig c;
    c.fftSize = n;
    c.mode = mode;
    c.window = WindowType::BlackmanHarris;
    c.maxBatch = 4;
    c.retryCooldownMs = 0;
    return c;
}

bool gpuUp(std::size_t n) {
    GpuSpectrumEngine e(baseCfg(n, GpuFftMode::Gpu));
    return e.backend() == GpuFftBackend::Gpu;
}

}  // namespace

int main() {
    // ---- the adapters: printed, and a software adapter is never counted
    const auto adapters = enumerateGpuAdapters();
    bool haveHardware = false;
    for (const auto& a : adapters) {
        std::printf("adapter %d: %s vendor 0x%04X device 0x%04X vram %llu MB software=%d\n", a.index, a.name.c_str(), a.vendorId, a.deviceId,
                    static_cast<unsigned long long>(a.dedicatedVideoBytes >> 20), a.software ? 1 : 0);
        if (!a.software) { haveHardware = true; }
    }
    const bool gpuOk = haveHardware && gpuUp(1024);
    if (!gpuOk) {
        ++g_checksSkipped;
        std::printf("SKIP: no usable hardware adapter here; the GPU lines below are not run\n");
    }

    // ---- validation
    {
        auto bad = [](std::size_t n) {
            try { GpuSpectrumEngine e(baseCfg(n, GpuFftMode::Cpu)); } catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(bad(0));
        CHECK(bad(16));
        CHECK(bad(1000));
        CHECK(bad(48));
        CHECK(bad(1u << 20));
        CHECK(!bad(32));
        CHECK(!bad(524288));
        GpuFftConfig c = baseCfg(1024, GpuFftMode::Cpu);
        c.customWindow.assign(10, 1.0f);
        bool t = false;
        try { GpuSpectrumEngine e(c); } catch (const std::invalid_argument&) { t = true; }
        CHECK(t);
        c = baseCfg(1024, GpuFftMode::Cpu);
        c.ringSlots = 0;
        t = false;
        try { GpuSpectrumEngine e(c); } catch (const std::invalid_argument&) { t = true; }
        CHECK(t);
        c = baseCfg(1024, GpuFftMode::Cpu);
        c.gpuBudgetFraction = 0.0;
        t = false;
        try { GpuSpectrumEngine e(c); } catch (const std::invalid_argument&) { t = true; }
        CHECK(t);
        c = baseCfg(1024, GpuFftMode::Cpu);
        c.maxBatch = 0;
        t = false;
        try { GpuSpectrumEngine e(c); } catch (const std::invalid_argument&) { t = true; }
        CHECK(t);
    }

    // ---- OFF by default: the default config is Cpu, and a Cpu engine never creates a device
    {
        GpuFftConfig d;
        CHECK(d.mode == GpuFftMode::Cpu);
        GpuSpectrumEngine e(baseCfg(1024, GpuFftMode::Cpu));
        CHECK(e.backend() == GpuFftBackend::Cpu);
        CHECK(!e.stats().gpuActive);
        CHECK(e.adapter().name.empty());
        CHECK(e.maxBatch() == 4);
    }

    // ---- the CPU backend is the existing CPU path, bit for bit, for all four windows
    for (WindowType wt : {WindowType::Rectangular, WindowType::Hann, WindowType::Hamming, WindowType::BlackmanHarris}) {
        const std::size_t n = 1024;
        GpuFftConfig c = baseCfg(n, GpuFftMode::Cpu);
        c.window = wt;
        GpuSpectrumEngine e(c);
        std::vector<std::vector<cf>> fr{signalFrame(n, 1, 1e-3f), noiseFrame(n, 2), toneFrame(n, 100.25, 0.5f)};
        const auto in = concat(fr);
        std::vector<float> out(3 * n);
        CHECK(e.process(in.data(), 3, out.data()) == 3);
        SpectrumEstimator est(n, wt);
        est.setAlpha(1.0f);
        bool same = true;
        for (std::size_t f = 0; f < 3; ++f) {
            std::vector<float> ref(n);
            est.reset();
            est.process(fr[f].data(), ref.data());
            same = same && std::equal(ref.begin(), ref.end(), out.begin() + static_cast<std::ptrdiff_t>(f * n));
        }
        CHECK(same);
        CHECK(e.stats().completedCpu == 1);
    }

    // ---- zero input floors at -200 dB, on the CPU and on the GPU
    for (GpuFftMode mode : {GpuFftMode::Cpu, GpuFftMode::Gpu}) {
        if (mode == GpuFftMode::Gpu && !gpuOk) { continue; }
        GpuSpectrumEngine e(baseCfg(256, mode));
        std::vector<cf> z(256, cf(0, 0));
        std::vector<float> out(256, 1.0f);
        CHECK(e.process(z.data(), 1, out.data()) == 1);
        bool floor = true;
        for (float v : out) { floor = floor && std::fabs(v + 200.0f) < 1e-3f; }
        CHECK(floor);
        CHECK(e.backend() == (mode == GpuFftMode::Gpu ? GpuFftBackend::Gpu : GpuFftBackend::Cpu));
    }

    if (gpuOk) {
        // ---- small sizes against the double-precision direct DFT, both parities of log2 N (32 = 2^5 odd, 64 = 2^6 even, 2048 = 2^11 odd)
        for (std::size_t n : {std::size_t{32}, std::size_t{64}, std::size_t{256}, std::size_t{2048}}) {
            GpuFftConfig c = baseCfg(n, GpuFftMode::Gpu);
            c.window = WindowType::Hann;
            GpuSpectrumEngine e(c);
            CHECK(e.backend() == GpuFftBackend::Gpu);
            const auto x = signalFrame(n, 5, 1e-3f);
            std::vector<float> out(n);
            CHECK(e.process(x.data(), 1, out.data()) == 1);
            const auto ref = directDb(x, WindowType::Hann);
            const Err er = compare(out.data(), ref.data(), n);
            std::printf("direct DFT n=%zu: max rel-to-peak %.3g, max dB diff above -80 dB %.3g\n", n, er.maxRelToPeak, er.maxDbAbove);
            CHECK(er.maxRelToPeak < 1e-6);
            CHECK(er.maxDbAbove < 0.02);
        }

        // ---- the pinned sizes against the existing CPU path: noise, tones and tones in noise, batched
        struct Pin { std::size_t n; std::size_t frames; double relPeak; double dbAbove; };
        const Pin pins[] = {{1024, 4, 2e-6, 0.01}, {8192, 4, 4e-6, 0.01}, {65536, 3, 4e-6, 0.01}, {524288, 1, 1e-6, 0.01}};
        for (const Pin& pn : pins) {
            GpuSpectrumEngine e(baseCfg(pn.n, GpuFftMode::Gpu));
            CHECK(e.backend() == GpuFftBackend::Gpu);
            std::vector<std::vector<cf>> fr;
            for (std::size_t f = 0; f < pn.frames; ++f) {
                fr.push_back(f % 3 == 0 ? signalFrame(pn.n, static_cast<std::uint32_t>(10 + f), 1e-3f)
                                        : (f % 3 == 1 ? noiseFrame(pn.n, static_cast<std::uint32_t>(20 + f)) : toneFrame(pn.n, 37.5, 0.9f)));
            }
            const auto in = concat(fr);
            std::vector<float> got(pn.frames * pn.n), ref(pn.frames * pn.n);
            CHECK(e.process(in.data(), pn.frames, got.data()) == pn.frames);
            gpuFftCpuReference(in.data(), pn.frames, pn.n, makeWindow(WindowType::BlackmanHarris, pn.n).data(), ref.data());
            double worstRel = 0.0, worstDb = 0.0;
            for (std::size_t f = 0; f < pn.frames; ++f) {
                const Err er = compare(got.data() + f * pn.n, ref.data() + f * pn.n, pn.n);
                worstRel = std::max(worstRel, er.maxRelToPeak);
                worstDb = std::max(worstDb, er.maxDbAbove);
            }
            std::printf("vs CPU n=%zu frames=%zu: max rel-to-peak %.3g, max dB diff above -80 dB %.3g (pinned %.1g / %.2g)\n", pn.n, pn.frames,
                        worstRel, worstDb, pn.relPeak, pn.dbAbove);
            CHECK(worstRel < pn.relPeak);
            CHECK(worstDb < pn.dbAbove);
        }

        // ---- every window shape the engine takes, and a custom window, at 4096
        for (WindowType wt : {WindowType::Rectangular, WindowType::Hann, WindowType::Hamming, WindowType::BlackmanHarris}) {
            GpuFftConfig c = baseCfg(4096, GpuFftMode::Gpu);
            c.window = wt;
            GpuSpectrumEngine e(c);
            const auto x = signalFrame(4096, 3, 1e-3f);
            std::vector<float> got(4096), ref(4096);
            CHECK(e.process(x.data(), 1, got.data()) == 1);
            gpuFftCpuReference(x.data(), 1, 4096, makeWindow(wt, 4096).data(), ref.data());
            CHECK(compare(got.data(), ref.data(), 4096).maxRelToPeak < 4e-6);
        }
        {
            GpuFftConfig c = baseCfg(4096, GpuFftMode::Gpu);
            c.customWindow.resize(4096);
            for (std::size_t i = 0; i < 4096; ++i) { c.customWindow[i] = 0.25f + 0.75f * static_cast<float>(i) / 4096.0f; }
            GpuSpectrumEngine e(c);
            const auto x = signalFrame(4096, 4, 1e-3f);
            std::vector<float> got(4096), ref(4096);
            CHECK(e.process(x.data(), 1, got.data()) == 1);
            gpuFftCpuReference(x.data(), 1, 4096, c.customWindow.data(), ref.data());
            CHECK(compare(got.data(), ref.data(), 4096).maxRelToPeak < 4e-6);
        }

        // ---- a tone lands on its bin with the documented level (0 dBFS under a rectangular window), orientation right
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            c.window = WindowType::Rectangular;
            GpuSpectrumEngine e(c);
            const auto pos = toneFrame(1024, 77.0, 1.0f);
            const auto neg = toneFrame(1024, -77.0, 1.0f);
            std::vector<float> out(1024);
            CHECK(e.process(pos.data(), 1, out.data()) == 1);
            CHECK(std::max_element(out.begin(), out.end()) - out.begin() == 512 + 77);
            CHECK_NEAR(out[512 + 77], 0.0, 1e-3);
            CHECK(e.process(neg.data(), 1, out.data()) == 1);
            CHECK(std::max_element(out.begin(), out.end()) - out.begin() == 512 - 77);
        }

        // ---- the asynchronous path: tickets, order, backend flag, the ring's drop counter, rejections, a too-small buffer
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            c.ringSlots = 2;
            GpuSpectrumEngine e(c);
            const auto a = signalFrame(1024, 1, 1e-3f);
            const auto b = noiseFrame(1024, 2);
            const auto d = toneFrame(1024, 9, 0.5f);
            const auto t1 = e.submit(a.data(), 1);
            const auto t2 = e.submit(b.data(), 1);
            const auto t3 = e.submit(d.data(), 1);  // ring of two is full
            CHECK(t1 > 0 && t2 > t1);
            CHECK(t3 == 0);
            CHECK(e.stats().droppedRingFull == 1);
            CHECK(e.stats().droppedFrames == 1);
            CHECK(e.submit(nullptr, 1) == 0);
            CHECK(e.submit(a.data(), 0) == 0);
            CHECK(e.submit(a.data(), 99) == 0);
            CHECK(e.stats().rejected == 3);
            std::vector<float> out(1024), small(10);
            GpuFftBatch info;
            auto wait = [&](float* buf, std::size_t cap) {
                const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < end) {
                    if (e.collect(info, buf, cap)) { return true; }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
                return false;
            };
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CHECK(!e.collect(info, small.data(), small.size()));  // too small: kept
            CHECK(wait(out.data(), out.size()));
            CHECK(info.ticket == t1 && info.frames == 1 && info.backend == GpuFftBackend::Gpu);
            CHECK(wait(out.data(), out.size()));
            CHECK(info.ticket == t2);
            CHECK(!e.collect(info, out.data(), out.size()));  // nothing left
            CHECK(e.stats().completedGpu == 2);
            CHECK(e.stats().gpuSecondsTotal > 0.0);
            // the ring is free again
            CHECK(e.submit(d.data(), 1) > 0);
        }
        // the same ring rule on the CPU backend
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Cpu);
            c.ringSlots = 1;
            GpuSpectrumEngine e(c);
            const auto a = noiseFrame(1024, 2);
            CHECK(e.submit(a.data(), 1) > 0);
            CHECK(e.submit(a.data(), 1) == 0);
            CHECK(e.stats().droppedRingFull == 1);
        }

        // ---- runtime switch Cpu -> Gpu -> Auto (below / above the threshold) -> Cpu, results agree throughout
        {
            GpuFftConfig c = baseCfg(2048, GpuFftMode::Cpu);
            c.autoMinFftSize = 4096;
            GpuSpectrumEngine e(c);
            const auto x = signalFrame(2048, 8, 1e-3f);
            std::vector<float> ref(2048), out(2048);
            CHECK(e.process(x.data(), 1, ref.data()) == 1);
            CHECK(e.backend() == GpuFftBackend::Cpu);
            e.setMode(GpuFftMode::Gpu);
            CHECK(e.mode() == GpuFftMode::Gpu);
            CHECK(e.backend() == GpuFftBackend::Gpu);
            CHECK(e.process(x.data(), 1, out.data()) == 1);
            CHECK(compare(out.data(), ref.data(), 2048).maxRelToPeak < 4e-6);
            e.setMode(GpuFftMode::Auto);  // 2048 < 4096: CPU
            CHECK(e.backend() == GpuFftBackend::Cpu);
            CHECK(e.process(x.data(), 1, out.data()) == 1);
            CHECK(std::equal(ref.begin(), ref.end(), out.begin()));
            GpuFftConfig c2 = baseCfg(8192, GpuFftMode::Auto);
            c2.autoMinFftSize = 4096;  // 8192 >= 4096: GPU
            GpuSpectrumEngine e2(c2);
            CHECK(e2.backend() == GpuFftBackend::Gpu);
            e2.setMode(GpuFftMode::Cpu);
            CHECK(e2.backend() == GpuFftBackend::Cpu);
            const auto s0 = e.stats();
            CHECK(s0.completedGpu == 1 && s0.completedCpu == 2);
        }

        // ---- Gpu requested but the bring-up is refused: clean fallback, reason kept, result right, and a later setMode brings it up
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            GpuSpectrumEngine e(c);
            CHECK(e.backend() == GpuFftBackend::Gpu);
            GpuFftConfig fc = baseCfg(1024, GpuFftMode::Cpu);
            fc.retryCooldownMs = 600000;  // a failed bring-up is not retried inside the cool-down; setMode re-arms it
            GpuSpectrumEngine f(fc);
            f.forceCreateFailureForTest(1);
            f.setMode(GpuFftMode::Gpu);
            CHECK(f.backend() == GpuFftBackend::Cpu);
            CHECK(!f.lastError().empty());
            const auto x = signalFrame(1024, 9, 1e-3f);
            std::vector<float> out(1024), ref(1024);
            CHECK(f.process(x.data(), 1, out.data()) == 1);
            gpuFftCpuReference(x.data(), 1, 1024, makeWindow(WindowType::BlackmanHarris, 1024).data(), ref.data());
            CHECK(std::equal(ref.begin(), ref.end(), out.begin()));
            f.setMode(GpuFftMode::Gpu);  // the hook is spent
            CHECK(f.backend() == GpuFftBackend::Gpu);
            CHECK(f.process(x.data(), 1, out.data()) == 1);
            CHECK(compare(out.data(), ref.data(), 1024).maxRelToPeak < 4e-6);
        }

        // ---- a shader that does not compile: never an abort, the text is kept, the CPU answers
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            c.shaderSourceOverride = "this is not HLSL @@@";
            GpuSpectrumEngine e(c);
            CHECK(e.backend() == GpuFftBackend::Cpu);
            CHECK(!e.shaderLog().empty());
            CHECK(e.lastError().find("shader compile failed") != std::string::npos);
            const auto x = noiseFrame(1024, 3);
            std::vector<float> out(1024), ref(1024);
            CHECK(e.process(x.data(), 1, out.data()) == 1);
            gpuFftCpuReference(x.data(), 1, 1024, makeWindow(WindowType::BlackmanHarris, 1024).data(), ref.data());
            CHECK(std::equal(ref.begin(), ref.end(), out.begin()));
            // and a source that compiles but has the wrong entry points
            GpuFftConfig c2 = baseCfg(1024, GpuFftMode::Gpu);
            c2.shaderSourceOverride = "[numthreads(1,1,1)] void CSOther() {}";
            GpuSpectrumEngine e2(c2);
            CHECK(e2.backend() == GpuFftBackend::Cpu);
            CHECK(!e2.lastError().empty());
        }

        // ---- adapter choice: a bad index and a software adapter are refused, the engine stays on the CPU
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            c.adapterIndex = 99;
            GpuSpectrumEngine e(c);
            CHECK(e.backend() == GpuFftBackend::Cpu);
            CHECK(e.lastError().find("not found") != std::string::npos);
            int sw = -1;
            for (const auto& a : adapters) { if (a.software) { sw = a.index; } }
            if (sw >= 0) {
                GpuFftConfig c2 = baseCfg(1024, GpuFftMode::Gpu);
                c2.adapterIndex = sw;
                GpuSpectrumEngine e2(c2);
                CHECK(e2.backend() == GpuFftBackend::Cpu);
                CHECK(e2.lastError().find("software") != std::string::npos);
            } else {
                ++g_checksSkipped;
                std::printf("SKIP: no software adapter is listed on this machine\n");
            }
            GpuSpectrumEngine ok(baseCfg(1024, GpuFftMode::Gpu));
            CHECK(!ok.adapter().name.empty());
            CHECK(!ok.adapter().software);
        }

        // ---- the per-second GPU budget: once it is spent, batches go to the CPU (and are still right)
        {
            GpuFftConfig c = baseCfg(4096, GpuFftMode::Gpu);
            c.gpuBudgetFraction = 1e-9;
            GpuSpectrumEngine e(c);
            const auto x = signalFrame(4096, 6, 1e-3f);
            std::vector<float> out(4096), ref(4096);
            CHECK(e.process(x.data(), 1, out.data()) == 1);  // first one runs on the GPU and is measured
            CHECK(e.stats().completedGpu == 1);
            CHECK(e.process(x.data(), 1, out.data()) == 1);  // budget spent
            CHECK(e.stats().budgetFallbacks >= 1);
            CHECK(e.stats().completedCpu >= 1);
            gpuFftCpuReference(x.data(), 1, 4096, makeWindow(WindowType::BlackmanHarris, 4096).data(), ref.data());
            CHECK(std::equal(ref.begin(), ref.end(), out.begin()));
        }

        // ---- device lost: in-flight batches are dropped and counted, the CPU answers at once, the GPU is rebuilt after the cool-down
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            c.retryCooldownMs = 300;
            GpuSpectrumEngine e(c);
            const auto x = signalFrame(1024, 12, 1e-3f);
            std::vector<float> out(1024), ref(1024);
            gpuFftCpuReference(x.data(), 1, 1024, makeWindow(WindowType::BlackmanHarris, 1024).data(), ref.data());
            CHECK(e.submit(x.data(), 1) > 0);  // in flight, never collected
            e.forceDeviceLostForTest();
            // this submit finds the device lost: the in-flight one is dropped, this one runs on the CPU
            const auto t = e.submit(x.data(), 1);
            CHECK(t > 0);
            auto st = e.stats();
            CHECK(st.deviceLost == 1);
            CHECK(st.droppedLost == 1);
            CHECK(st.droppedFrames == 1);
            CHECK(!st.gpuActive);
            CHECK(e.backend() == GpuFftBackend::Cpu);
            CHECK(e.lastError().find("device lost") != std::string::npos);
            GpuFftBatch info;
            CHECK(e.collect(info, out.data(), out.size()));
            CHECK(info.ticket == t && info.backend == GpuFftBackend::Cpu);
            CHECK(std::equal(ref.begin(), ref.end(), out.begin()));
            // inside the cool-down the GPU stays down
            CHECK(e.process(x.data(), 1, out.data()) == 1);
            CHECK(!e.stats().gpuActive);
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            CHECK(e.process(x.data(), 1, out.data()) == 1);  // rebuilt, runs on the GPU
            st = e.stats();
            CHECK(st.gpuActive);
            CHECK(st.recoveries == 1);
            CHECK(e.backend() == GpuFftBackend::Gpu);
            CHECK(compare(out.data(), ref.data(), 1024).maxRelToPeak < 4e-6);
        }
        // repeated losses: after maxRecoveries the engine stays on the CPU, and setMode re-arms it
        {
            GpuFftConfig c = baseCfg(1024, GpuFftMode::Gpu);
            c.maxRecoveries = 1;
            c.retryCooldownMs = 0;
            GpuSpectrumEngine e(c);
            const auto x = noiseFrame(1024, 1);
            std::vector<float> out(1024);
            for (int i = 0; i < 4; ++i) {
                e.forceDeviceLostForTest();
                e.submit(x.data(), 1);  // loss found; the CPU answers (this may or may not rebuild)
                GpuFftBatch info;
                while (e.collect(info, out.data(), out.size())) {}
                // do not collect a GPU batch in between: the streak is only reset by a finished GPU batch
            }
            CHECK(e.stats().deviceLost >= 2);
            CHECK(!e.stats().gpuActive);
            CHECK(e.process(x.data(), 1, out.data()) == 1);
            CHECK(!e.stats().gpuActive);  // gave up
            e.setMode(GpuFftMode::Gpu);
            CHECK(e.stats().gpuActive);
        }

        // ---- concurrency: a producer submits, a consumer collects, in order, every result right
        {
            const std::size_t n = 1024;
            GpuFftConfig c = baseCfg(n, GpuFftMode::Gpu);
            c.ringSlots = 4;
            c.gpuBudgetFraction = 1.0;
            GpuSpectrumEngine e(c);
            constexpr int kBatches = 150;
            std::atomic<int> accepted{0};
            std::atomic<bool> done{false};
            std::vector<std::uint64_t> tickets;
            std::vector<int> seqOf;  // seq per accepted ticket, in submit order (only the producer writes before `done`)
            std::thread prod([&] {
                for (int s = 0; s < kBatches; ++s) {
                    const auto fr = toneFrame(n, 3.0 + (s % 100), 0.5f);
                    std::uint64_t t = 0;
                    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                    while (t == 0 && std::chrono::steady_clock::now() < until) {
                        t = e.submit(fr.data(), 1);
                        if (t == 0) { std::this_thread::sleep_for(std::chrono::microseconds(200)); }
                    }
                    if (t != 0) {
                        tickets.push_back(t);
                        seqOf.push_back(s);
                        ++accepted;
                    }
                }
                done = true;
            });
            std::vector<float> out(n);
            std::size_t got = 0;
            bool inOrder = true, allRight = true;
            std::uint64_t last = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            std::vector<std::uint64_t> seen;
            std::vector<int> peaks;
            while (std::chrono::steady_clock::now() < deadline) {
                GpuFftBatch info;
                if (e.collect(info, out.data(), out.size())) {
                    inOrder = inOrder && info.ticket > last;
                    last = info.ticket;
                    seen.push_back(info.ticket);
                    peaks.push_back(static_cast<int>(std::max_element(out.begin(), out.end()) - out.begin()));
                    ++got;
                } else if (done.load() && got == static_cast<std::size_t>(accepted.load())) {
                    break;
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            prod.join();
            CHECK(got == static_cast<std::size_t>(kBatches));
            CHECK(inOrder);
            // every collected batch peaks at the bin its sequence number chose (re-run quickly on the CPU to be sure of the mapping)
            CHECK(seen.size() == tickets.size());
            CHECK(peaks.size() == seqOf.size());
            for (std::size_t i = 0; i < seen.size() && i < seqOf.size() && i < peaks.size(); ++i) {
                allRight = allRight && seen[i] == tickets[i] && peaks[i] == 512 + 3 + seqOf[i] % 100;
            }
            CHECK(allRight);
            CHECK(e.stats().completedGpu + e.stats().completedCpu == static_cast<std::uint64_t>(kBatches));
        }
    }

    // ---- the engine file names no vendor SDK and loads the system libraries at run time only
    {
        // (a build-time fact, checked by the build itself: this test links with no d3d11.lib; reaching here proves it.)
        CHECK(std::string(gpuFftShaderSource()).find("CSStage4") != std::string::npos);
    }

    return testSummary("test_gpu_fft");
}
