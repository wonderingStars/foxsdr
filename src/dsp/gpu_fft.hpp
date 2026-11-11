// GPU spectrum engine: a batched windowed FFT -> power -> dB stage on a Direct3D 11 COMPUTE shader, with the CPU FFT as the fallback and the
// default.
//
// STAND-ALONE: nothing in the engine calls it yet (wiring is a hand-off, docs/feature-records/gpufft.md). It needs no vendor SDK and no
// download: d3d11.dll, dxgi.dll and the shader compiler d3dcompiler_47.dll all ship with Windows and are LOADED AT RUN TIME, so a machine
// without them (or a non-Windows build) simply runs the CPU path; nothing is linked.
//
// WHAT IT COMPUTES. Same contract as dsp::SpectrumEstimator with alpha = 1 (one frame, no averaging; averaging stays with the caller):
//   outDb[i] = 10*log10(max(|FFT(in*window)[k]|^2 / N^2, 1e-20)),  k = (i + N/2) mod N   (fftshift: DC at N/2, floor -200 dB)
// per frame, for a batch of frames in one call. Input: N complex floats per frame (interleaved re, im). Output: N floats per frame.
//
// ALGORITHM (GPU). Stockham autosort FFT, radix-4 passes (one radix-2 pass first when log2 N is odd), ping-pong buffers, the window fused
// into the first pass, twiddles from a table computed in double on the CPU, then one power + fftshift + log pass. One dispatch per pass
// covers the whole batch (the frame number is the Y dimension of the dispatch).
//
// THREADS. One mutex guards every call, so submit() on the DSP thread and collect() on another is safe; the intended use is a single
// caller. Nothing here blocks on the GPU: submit() issues work and returns, collect() returns only batches the GPU has finished (an event
// query, then a non-waiting map). A full ring DROPS the new batch (counted), it never waits. The audio path is never held up by this class.
//
// MODES. Cpu: always the CPU FFT (the default). Gpu: the GPU when it is usable, else the CPU, with the reason in lastError(). Auto: the GPU
// only when it is usable AND fftSize >= autoMinFftSize, else the CPU. The mode can be changed at any time with setMode().
//
// SAFETY. The GPU is shared with the desktop: the engine (a) runs on a headless device (no swap chain, no window), (b) lowers its GPU
// thread priority, (c) caps the GPU time it uses per second (gpuBudgetFraction; a batch that would exceed it runs on the CPU instead),
// (d) never selects a software adapter. A lost or removed device (TDR, driver reset) is detected after every operation: in-flight batches
// are counted as dropped, the GPU objects are released, the CPU path takes over at once and, after a cool-down, the GPU is rebuilt (up to
// maxRecoveries times in a row). Shader compile errors are kept (shaderLog()) and never abort.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dsp/window.hpp"

namespace cascade::dsp {

enum class GpuFftMode { Auto, Cpu, Gpu };
enum class GpuFftBackend { Cpu, Gpu };

constexpr std::size_t kGpuFftMinSize = 32;
constexpr std::size_t kGpuFftMaxSize = 524288;

// One display adapter as DXGI reports it (read-only enumeration: creates no device, no window).
struct GpuAdapterInfo {
    int index = 0;
    std::string name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoBytes = 0;
    bool software = false;  // the Microsoft Basic Render / WARP adapter: never used by the engine
};

// Empty when Direct3D / DXGI is not available (non-Windows, or the DLLs cannot be loaded).
std::vector<GpuAdapterInfo> enumerateGpuAdapters();

struct GpuFftConfig {
    std::size_t fftSize = 4096;            // power of two, kGpuFftMinSize .. kGpuFftMaxSize (throws std::invalid_argument otherwise)
    WindowType window = WindowType::BlackmanHarris;
    std::vector<float> customWindow;       // when non-empty (size must equal fftSize) it replaces `window`
    GpuFftMode mode = GpuFftMode::Cpu;     // OFF by default: the CPU FFT
    std::size_t maxBatch = 8;              // frames per submit(); clamped so one batch is <= 64 MB of complex input
    int ringSlots = 4;                     // batches in flight (1 .. 16); a full ring drops the new batch
    int adapterIndex = -1;                 // -1 = the hardware adapter with the most dedicated video memory
    bool lowPriority = true;               // IDXGIDevice::SetGPUThreadPriority(-7)
    double gpuBudgetFraction = 0.25;       // at most this share of each second may be spent on the GPU (0 < f <= 1)
    std::size_t autoMinFftSize = 16384;    // Auto uses the GPU only from this size (the measured break-even, see the record)
    int retryCooldownMs = 2000;            // after a device loss, wait this long before rebuilding the GPU path
    int maxRecoveries = 3;                 // consecutive rebuild attempts after losses before the engine stays on the CPU
    std::string shaderSourceOverride;      // tests: replaces the built-in HLSL (empty = built-in)
};

struct GpuFftStats {
    std::uint64_t submitted = 0;       // batches accepted
    std::uint64_t completedGpu = 0;    // batches finished on the GPU
    std::uint64_t completedCpu = 0;    // batches computed on the CPU (mode, fallback or budget)
    std::uint64_t droppedRingFull = 0; // batches refused because every slot was in flight (frames counted in droppedFrames)
    std::uint64_t droppedLost = 0;     // batches in flight when the device was lost
    std::uint64_t droppedFrames = 0;   // frames in the dropped batches (both kinds)
    std::uint64_t rejected = 0;        // submit() with 0 frames, more than maxBatch(), or a null pointer
    std::uint64_t budgetFallbacks = 0; // batches sent to the CPU because the per-second GPU budget was spent
    std::uint64_t deviceLost = 0;      // times the device was found lost / removed
    std::uint64_t recoveries = 0;      // times the GPU path was rebuilt after a loss
    double gpuSecondsTotal = 0.0;      // GPU time measured by timestamp queries
    double gpuSecondsLastBatch = 0.0;
    double gpuSecondsThisWindow = 0.0; // inside the current one-second budget window
    bool gpuActive = false;            // a device is up right now
};

struct GpuFftBatch {
    std::uint64_t ticket = 0;
    std::size_t frames = 0;
    GpuFftBackend backend = GpuFftBackend::Cpu;
};

class GpuSpectrumEngine {
public:
    // Throws std::invalid_argument for a bad fftSize / window / config value. A GPU problem never throws: it leaves the engine on the CPU
    // with lastError() set.
    explicit GpuSpectrumEngine(const GpuFftConfig& cfg);
    ~GpuSpectrumEngine();
    GpuSpectrumEngine(const GpuSpectrumEngine&) = delete;
    GpuSpectrumEngine& operator=(const GpuSpectrumEngine&) = delete;

    std::size_t fftSize() const noexcept;
    std::size_t maxBatch() const noexcept;  // after the clamp

    void setMode(GpuFftMode mode);          // takes effect at the next submit(); Gpu / Auto try to bring the GPU up at once
    GpuFftMode mode() const;
    // What the next submit() would use, before the budget: Gpu only when mode allows it and a device is up.
    GpuFftBackend backend() const;

    // Issue `nFrames` (1 .. maxBatch()) frames of fftSize() samples each, back to back in `frames`. Returns a ticket (> 0), or 0 when the
    // batch was dropped (ring full, counted) or rejected (counted). Never waits for the GPU.
    std::uint64_t submit(const std::complex<float>* frames, std::size_t nFrames);

    // Oldest finished batch, in submit order. Copies nFrames * fftSize() floats to outDb (capacity in floats). False when nothing is ready
    // or the capacity is too small (the batch is then kept). Never waits.
    bool collect(GpuFftBatch& info, float* outDb, std::size_t capacityFloats);

    // Synchronous convenience (tests, non-real-time callers): submit + wait up to timeoutMs + collect. Returns the frames produced, 0 on a
    // drop / timeout.
    std::size_t process(const std::complex<float>* frames, std::size_t nFrames, float* outDb, int timeoutMs = 2000);

    GpuFftStats stats() const;
    std::string lastError() const;   // the latest GPU problem, empty if none (text only: no identifiers)
    std::string shaderLog() const;   // the compiler's text from the last compile, errors included
    GpuAdapterInfo adapter() const;  // the adapter in use (name empty when none)

    // TEST HOOKS. forceDeviceLostForTest(): the next device check sees DXGI_ERROR_DEVICE_REMOVED, so the genuine loss path runs (a real TDR is
    // never provoked on a shared display). forceCreateFailureForTest(n): the next n GPU bring-ups fail as if no device could be made.
    void forceDeviceLostForTest();
    void forceCreateFailureForTest(int n);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The CPU reference the GPU path is held against: exactly SpectrumEstimator's arithmetic with alpha = 1, over a batch.
void gpuFftCpuReference(const std::complex<float>* frames, std::size_t nFrames, std::size_t fftSize, const float* window, float* outDb);

// The built-in HLSL source text (gpu_fft_shader.hpp), for the record and the tests.
const char* gpuFftShaderSource();

}  // namespace cascade::dsp
