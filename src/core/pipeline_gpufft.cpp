// pipeline_gpufft.cpp - THE GPU SPECTRUM in the pipeline (wire-b; dsp/gpu_fft.hpp GpuSpectrumEngine; docs/multi-radio-records/wire-b.md).
//
// WHAT IT IS. Next to the SpectrumEstimator of the legacy spectrum path (Pipeline::dspThreadBody), the DSP thread can hand each block to a Direct3D compute FFT and publish its frame instead.
// The setting is Pipeline::setGpuFftMode: 0 Cpu (the default: nothing here runs, no device is ever made), 1 Auto (the GPU only from the engine's own size threshold, 65 536 points: at the
// legacy size of this path it stays on the CPU estimator), 2 Gpu (always, to measure it or to use it at a size the owner chose).
//
// NEVER A STALL, NEVER A DIALOG. The device is made on a worker thread (the DSP thread keeps running the CPU estimator until it is up); a frame is SUBMITTED each block and the oldest finished
// one is collected without waiting (the frame arrives a block or two late; a block with none ready publishes none); any failure of the GPU path - no adapter, a compile error, a lost
// device - leaves the engine on its own CPU computation (GpuSpectrumEngine's contract) or, if it could not be built at all, on the CPU estimator: silently, with the sentence in the report
// (no adapter name goes to any log or report). A failed build is not retried until the mode changes.
//
// SAME NUMBERS AS THE CPU PATH: the engine returns single-frame dB (alpha 1); the exponential average the estimator applies to POWER is applied here to the power the dB stands for, with the
// estimator's own alpha, the first block taken as it is and the -200 dB floor.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/gpu_fft.hpp"
#include "dsp/spectrum_block_gpu.hpp"

namespace cascade::core {

struct Pipeline::GpuFftState {
    std::atomic<int> wanted{0};
    // --- the DSP thread's own ---
    std::unique_ptr<cascade::dsp::GpuSpectrumEngine> engine;
    std::future<std::unique_ptr<cascade::dsp::GpuSpectrumEngine>> pending;
    bool pendingActive = false;
    int built = -1;            // the mode the engine was last told
    bool failed = false;       // building it did not work: no retry until the mode changes
    int failedFor = -1;
    std::string buildError;
    std::vector<float> avg;
    std::vector<float> one;
    bool primed = false;
    std::chrono::steady_clock::time_point lastReport{};
    // --- the report (any thread) ---
    std::mutex mu;
    GpuFftReport report;
};

std::shared_ptr<Pipeline::GpuFftState> Pipeline::makeGpuFftState() { return std::make_shared<GpuFftState>(); }

void Pipeline::setGpuFftMode(int mode) {
    const int m = (mode < 0 || mode > 2) ? 0 : mode;
    gpuFft_->wanted.store(m, std::memory_order_release);
    {
        // second pass: the spectrum block (SET_SPECTRUM) follows the same switch (dsp/spectrum_block_gpu.hpp). A block that does not exist yet is made now (idle: the DSP thread feeds it only once
        // SET_SPECTRUM turns it on), so a radio set to GPU before it picks a large size still gets it.
        std::lock_guard<std::mutex> blk(frameMutex_);
        if (!specBlock_ && m != 0) {
            cascade::dsp::SpecConfig c;
            c.sampleRateHz = mirrorInputRateHz_.load(std::memory_order_relaxed);
            specBlock_ = std::make_unique<cascade::dsp::SpectrumBlock>(c);
        }
        if (specBlock_) { specBlock_->setGpuPolicy(m); }
    }
    std::lock_guard<std::mutex> lk(gpuFft_->mu);
    gpuFft_->report.mode = m;
}

Pipeline::GpuFftReport Pipeline::gpuFftReport() const {
    std::lock_guard<std::mutex> lk(gpuFft_->mu);
    GpuFftReport r = gpuFft_->report;
    r.mode = gpuFft_->wanted.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> blk(frameMutex_);
        if (specBlock_) { r.blockGpu = specBlock_->gpuFramesOnGpu(); r.blockCpu = specBlock_->gpuFramesOnCpu(); r.blockUp = specBlock_->gpuUp(); }
    }
    return r;
}

namespace {

constexpr std::size_t kGpuAutoMinSize = 65536;   // GpuFftConfig::autoMinFftSize's default: Auto leaves the legacy size to the CPU estimator

}  // namespace

// 0: not engaged (the caller runs the CPU estimator), 1: a GPU frame is in `db`, 2: engaged but no frame is ready this block.
int Pipeline::gpuFrame(const std::complex<float>* acc, std::size_t n, float* db) {
    GpuFftState& st = *gpuFft_;
    const int want = st.wanted.load(std::memory_order_acquire);
    const auto teardown = [&] {
        if (st.engine == nullptr && !st.pendingActive) { return; }
        if (st.pendingActive) {
            st.pending.wait();   // (the build is short and off this thread's path only while it is wanted; a switch-off waits for it once)
            st.pending = {};
            st.pendingActive = false;
        }
        st.engine.reset();
        st.built = -1;
        st.primed = false;
        estimator_.reset();   // the CPU estimator re-primes: its average is stale
        std::lock_guard<std::mutex> lk(st.mu);
        st.report.engaged = false;
        st.report.backend = 0;
    };
    if (want == 0 || (want == 1 && n < kGpuAutoMinSize)) {
        teardown();
        st.failed = false;
        return 0;
    }
    if (st.failed && st.failedFor == want) { return 0; }
    if (st.engine == nullptr) {
        if (!st.pendingActive) {
            const int mode = want;
            st.failed = false;
            st.pending = std::async(std::launch::async, [n, mode]() -> std::unique_ptr<cascade::dsp::GpuSpectrumEngine> {
                cascade::dsp::GpuFftConfig cfg;
                cfg.fftSize = n;
                cfg.window = cascade::dsp::WindowType::BlackmanHarris;
                cfg.mode = mode == 2 ? cascade::dsp::GpuFftMode::Gpu : cascade::dsp::GpuFftMode::Auto;
                cfg.maxBatch = 1;
                cfg.ringSlots = 4;
                try {
                    return std::make_unique<cascade::dsp::GpuSpectrumEngine>(cfg);
                } catch (...) {
                    return nullptr;
                }
            });
            st.pendingActive = true;
        }
        if (st.pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return 0; }
        st.engine = st.pending.get();
        st.pendingActive = false;
        if (st.engine == nullptr) {
            st.failed = true;
            st.failedFor = want;
            std::lock_guard<std::mutex> lk(st.mu);
            st.report.engaged = false;
            st.report.error = "the GPU spectrum could not be set up; the CPU estimator keeps running";
            return 0;
        }
        st.built = want;
        st.primed = false;
        estimator_.reset();
    }
    if (st.built != want) {
        st.engine->setMode(want == 2 ? cascade::dsp::GpuFftMode::Gpu : cascade::dsp::GpuFftMode::Auto);
        st.built = want;
    }
    if (st.engine->fftSize() != n) {   // (the size follows the pipeline: it never changes at run time today, but a change must not read a wrong-sized frame)
        teardown();
        return 0;
    }
    (void)st.engine->submit(acc, 1);
    cascade::dsp::GpuFftBatch info;
    st.one.resize(n);
    const bool got = st.engine->collect(info, st.one.data(), n);
    // The report, twice a second: the counters, the backend now in use, the engine's own last problem (text only).
    const auto now = std::chrono::steady_clock::now();
    if (now - st.lastReport >= std::chrono::milliseconds(500)) {
        st.lastReport = now;
        const cascade::dsp::GpuFftStats s = st.engine->stats();
        std::lock_guard<std::mutex> lk(st.mu);
        st.report.engaged = true;
        st.report.backend = st.engine->backend() == cascade::dsp::GpuFftBackend::Gpu ? 1 : 0;
        st.report.submitted = s.submitted;
        st.report.completedGpu = s.completedGpu;
        st.report.completedCpu = s.completedCpu;
        st.report.droppedFrames = s.droppedFrames;
        st.report.deviceLost = s.deviceLost;
        st.report.error = st.engine->lastError();
    }
    if (!got) { return 2; }
    // dB -> power -> the estimator's exponential average -> dB, as SpectrumEstimator does on the CPU.
    const float alpha = estimator_.alpha();
    if (st.avg.size() != n) { st.avg.assign(n, 0.0f); st.primed = false; }
    for (std::size_t i = 0; i < n; ++i) {
        const float p = std::pow(10.0f, st.one[i] / 10.0f);
        st.avg[i] = st.primed ? alpha * p + (1.0f - alpha) * st.avg[i] : p;
        const float v = 10.0f * std::log10(std::max(st.avg[i], 1e-30f));
        db[i] = std::max(v, -200.0f);
    }
    st.primed = true;
    return 1;
}

}  // namespace cascade::core
