// RAII wrapper over pffft's single-precision complex FFT, with ORDERED output
// (interleaved complex bins in canonical order, via pffft_transform_ordered).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

// Forward declaration of pffft's opaque setup type so this header does not
// leak the C header to every includer; only fft.cpp talks to pffft directly.
struct PFFFT_Setup;

namespace cascade::dsp {

class ComplexFFT {
public:
    // Legal sizes, per the restrictions documented in pffft.h: N must factor
    // as 2^a * 3^b * 5^c with a >= 5 (so N is a multiple of 32; 32 is the
    // smallest legal size). We validate before calling pffft because pffft's
    // own size check is partly an assert() that vanishes in release builds —
    // relying on it would turn a bad size into silent garbage.
    static bool isValidSize(std::size_t n) noexcept;

    // Throws std::invalid_argument if the size is illegal (see isValidSize).
    explicit ComplexFFT(std::size_t size);
    ~ComplexFFT();

    // Non-copyable: the pffft setup and scratch buffers are owned resources.
    ComplexFFT(const ComplexFFT&) = delete;
    ComplexFFT& operator=(const ComplexFFT&) = delete;
    ComplexFFT(ComplexFFT&& other) noexcept;
    ComplexFFT& operator=(ComplexFFT&& other) noexcept;

    std::size_t size() const noexcept { return size_; }

    // Unscaled forward DFT: out[k] = sum_n in[n] * exp(-2*pi*i*k*n/N).
    // `in` and `out` are size() complex floats each and may alias. Caller
    // buffers need no special alignment: data is staged through an internal
    // 16-byte-aligned buffer because pffft requires SIMD alignment on every
    // float pointer it touches (pffft.h), and we do not want to force that
    // requirement onto every std::vector in the pipeline.
    void forward(const std::complex<float>* in, std::complex<float>* out);

    // Inverse DFT scaled by 1/N, so that inverse(forward(x)) == x exactly (up
    // to float rounding). pffft itself is unscaled — BACKWARD(FORWARD(x)) is
    // N*x — and the 1/N is folded in here so callers never see that factor.
    void inverse(const std::complex<float>* in, std::complex<float>* out);

private:
    void transform(const std::complex<float>* in, std::complex<float>* out,
                   bool forwardDirection);
    void release() noexcept;

    PFFFT_Setup* setup_ = nullptr;
    float* buf_ = nullptr;   // 2*N floats, aligned staging buffer (in-place ok)
    float* work_ = nullptr;  // 2*N floats, pffft scratch: allocated explicitly
                             // instead of passing NULL so large waterfall FFTs
                             // never land on the stack (pffft.h stack note)
    std::size_t size_ = 0;
};

}  // namespace cascade::dsp
