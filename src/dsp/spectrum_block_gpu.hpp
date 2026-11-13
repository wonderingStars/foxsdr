// spectrum_block_gpu.hpp - THE GPU STAGE OF THE SPECTRUM BLOCK (wire-b second pass, item 8 of not-wired-after-merge-3.md; dsp/spectrum_block.hpp, dsp/gpu_fft.hpp).
//
// The spectrum block (SET_SPECTRUM: 1 024 .. 524 288 points, chosen window, fixed frame rate) computes each frame with the CPU FFT. For the large sizes the GPU spectrum engine (GpuSpectrumEngine, a
// Direct3D 11 compute FFT with the CPU as its fallback) is faster; this class lets SpectrumBlock::Engine::frame() hand a frame to it. It is the SAME contract the legacy path already has in
// core/pipeline_gpufft.cpp: OFF by default, the device is made on a worker thread (the DSP thread never waits for a device), a frame is submitted each time and the oldest finished one collected without
// waiting (so a frame arrives a block or two late and a block with none ready publishes none), any failure leaves the CPU FFT running, and no adapter name reaches a log.
//
// THE POLICY. Each SpectrumBlock carries one switch (SpectrumBlock::setGpuPolicy: 0 CPU, the default; 1 Auto: only from kSpecGpuAutoMinSize = 65 536 points; 2 GPU). Pipeline::setGpuFftMode (the wire-b
// operation GPU_FFT_MODE, which already sets every open radio) writes it to that radio's block, so the one operation covers the standard path AND the spectrum block with no edit of pipeline.cpp
// (lane A's). Engines read it at every frame, so a change takes effect on a running block with no rebuild; a block that does not exist yet is made by the setter (idle until SET_SPECTRUM turns it on).
//
// SAME NUMBERS AS THE CPU PATH. GpuSpectrumEngine returns single-frame dB of |FFT(x * w)|^2 / N^2 for the window w it is given (the block's own window, handed over as a custom window), fftshifted;
// the block multiplies the power back by N^2 and its own scale (1/(sum w)^2 for tones, the PSD scale otherwise), so the frame is the CPU frame to float precision and the block's averaging, hold
// modes and column reduction run on it unchanged.
//
// THREADS. step() is the DSP thread's. It allocates ONCE when the GPU is first engaged for a size (two buffers and the build's worker), as the legacy GPU path does. framesOnGpu() / usingGpu() may be
// read from any thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace cascade::dsp {

inline constexpr std::size_t kSpecGpuAutoMinSize = 65536;   // GpuFftConfig::autoMinFftSize's default

class BlockGpu {
public:
    BlockGpu();
    ~BlockGpu();
    BlockGpu(const BlockGpu&) = delete;
    BlockGpu& operator=(const BlockGpu&) = delete;

    // DSP THREAD. `ring` holds n samples in a ring whose OLDEST sample is at index `oldest`. `window` is the block's n window coefficients.
    // Returns 0 = not engaged (run the CPU FFT), 1 = a frame is ready: db() (n floats, fftshifted, DC at n/2, floor -200 dB), 2 = engaged but none finished yet (publish nothing).
    int step(int wanted, std::size_t n, const float* window, const std::complex<float>* ring, std::size_t oldest);
    const float* db() const;

    std::uint64_t framesOnGpu() const;   // frames the Direct3D device finished
    std::uint64_t framesOnCpu() const;   // frames the engine's own CPU computation finished (a device lost, or the budget)
    bool usingGpu() const;               // a device is up right now

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace cascade::dsp
