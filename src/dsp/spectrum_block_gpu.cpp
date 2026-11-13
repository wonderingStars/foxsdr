// spectrum_block_gpu.cpp - the spectrum block's GPU stage (spectrum_block_gpu.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/spectrum_block_gpu.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <vector>

#include "dsp/gpu_fft.hpp"

namespace cascade::dsp {

struct BlockGpu::Impl {
    std::unique_ptr<GpuSpectrumEngine> engine;
    std::future<std::unique_ptr<GpuSpectrumEngine>> pending;
    bool pendingActive = false;
    int built = -1;
    bool failed = false;
    int failedFor = -1;
    std::size_t size = 0;
    std::vector<std::complex<float>> ordered;
    std::vector<float> out;
    std::atomic<std::uint64_t> onGpu{0}, onCpu{0};
    std::atomic<bool> gpuUp{false};

    void teardown() {
        if (pendingActive) {
            pending.wait();   // (a build is short; a switch-off or a size change waits for it once)
            pending = {};
            pendingActive = false;
        }
        engine.reset();
        built = -1;
        size = 0;
        gpuUp.store(false, std::memory_order_relaxed);
    }
};

BlockGpu::BlockGpu() : p_(std::make_unique<Impl>()) {}
BlockGpu::~BlockGpu() { p_->teardown(); }

const float* BlockGpu::db() const { return p_->out.data(); }
std::uint64_t BlockGpu::framesOnGpu() const { return p_->onGpu.load(std::memory_order_relaxed); }
std::uint64_t BlockGpu::framesOnCpu() const { return p_->onCpu.load(std::memory_order_relaxed); }
bool BlockGpu::usingGpu() const { return p_->gpuUp.load(std::memory_order_relaxed); }

int BlockGpu::step(int wanted, std::size_t n, const float* window, const std::complex<float>* ring, std::size_t oldest) {
    Impl& s = *p_;
    if (wanted <= 0 || wanted > 2 || (wanted == 1 && n < kSpecGpuAutoMinSize) || n < kGpuFftMinSize || n > kGpuFftMaxSize) {
        if (s.engine != nullptr || s.pendingActive) { s.teardown(); }
        s.failed = false;
        return 0;
    }
    if (s.failed && s.failedFor == wanted && s.size == n) { return 0; }
    if (s.engine != nullptr && s.size != n) { s.teardown(); }
    if (s.engine == nullptr) {
        if (!s.pendingActive) {
            s.failed = false;
            s.size = n;
            std::vector<float> w(window, window + n);
            const int mode = wanted;
            s.pending = std::async(std::launch::async, [n, mode, w = std::move(w)]() mutable -> std::unique_ptr<GpuSpectrumEngine> {
                GpuFftConfig cfg;
                cfg.fftSize = n;
                cfg.customWindow = std::move(w);
                cfg.mode = mode == 2 ? GpuFftMode::Gpu : GpuFftMode::Auto;
                cfg.maxBatch = 1;
                cfg.ringSlots = 4;
                try {
                    return std::make_unique<GpuSpectrumEngine>(cfg);
                } catch (...) {
                    return nullptr;
                }
            });
            s.pendingActive = true;
        }
        if (s.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return 0; }
        s.engine = s.pending.get();
        s.pendingActive = false;
        if (s.engine == nullptr || s.engine->backend() != GpuFftBackend::Gpu) {
            // no usable device: the CPU FFT of the block keeps running (no retry until the mode or the size changes)
            s.engine.reset();
            s.failed = true;
            s.failedFor = wanted;
            return 0;
        }
        s.built = wanted;
        s.ordered.assign(n, std::complex<float>());
        s.out.assign(n, -200.0f);
    }
    if (s.built != wanted) {
        s.engine->setMode(wanted == 2 ? GpuFftMode::Gpu : GpuFftMode::Auto);
        s.built = wanted;
    }
    const std::size_t tail = n - oldest;
    std::copy(ring + oldest, ring + n, s.ordered.begin());
    std::copy(ring, ring + oldest, s.ordered.begin() + static_cast<std::ptrdiff_t>(tail));
    (void)s.engine->submit(s.ordered.data(), 1);
    GpuFftBatch info;
    const bool got = s.engine->collect(info, s.out.data(), n);
    s.gpuUp.store(s.engine->backend() == GpuFftBackend::Gpu, std::memory_order_relaxed);
    if (!got) { return 2; }
    (info.backend == GpuFftBackend::Gpu ? s.onGpu : s.onCpu).fetch_add(1, std::memory_order_relaxed);
    return 1;
}

}  // namespace cascade::dsp
