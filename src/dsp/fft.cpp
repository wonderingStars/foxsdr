// pffft-backed complex FFT. See fft.hpp for the interface contract.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/fft.hpp"

#include <pffft/pffft.h>

#include <cstring>
#include <new>
#include <stdexcept>

namespace cascade::dsp {

bool ComplexFFT::isValidSize(std::size_t n) noexcept {
    // pffft.h: N = (2^a)*(3^b)*(5^c) with a >= 5, b >= 0, c >= 0. Strip the
    // permitted factors; anything left over means an unsupported radix.
    if (n == 0) { return false; }
    std::size_t m = n;
    int twos = 0;
    while (m % 2 == 0) { m /= 2; ++twos; }
    while (m % 3 == 0) { m /= 3; }
    while (m % 5 == 0) { m /= 5; }
    return m == 1 && twos >= 5;
}

ComplexFFT::ComplexFFT(std::size_t size) : size_(size) {
    if (!isValidSize(size)) {
        throw std::invalid_argument(
            "ComplexFFT: size must be 2^a * 3^b * 5^c with a >= 5 (min 32)");
    }
    setup_ = pffft_new_setup(static_cast<int>(size), PFFFT_COMPLEX);
    if (setup_ == nullptr) {
        // pffft signals an unsuitable size by returning NULL (pffft.h); with
        // our own validation this should be unreachable, but a null setup
        // would crash on first use, so fail loudly here instead.
        throw std::invalid_argument("ComplexFFT: pffft rejected this size");
    }
    // A complex FFT needs 2*N floats of data and 2*N floats of scratch, all
    // 16-byte aligned (pffft.h); pffft_aligned_malloc guarantees that.
    const std::size_t bytes = 2 * size * sizeof(float);
    buf_ = static_cast<float*>(pffft_aligned_malloc(bytes));
    work_ = static_cast<float*>(pffft_aligned_malloc(bytes));
    if (buf_ == nullptr || work_ == nullptr) {
        release();
        throw std::bad_alloc();
    }
}

ComplexFFT::~ComplexFFT() { release(); }

ComplexFFT::ComplexFFT(ComplexFFT&& other) noexcept
    : setup_(other.setup_), buf_(other.buf_), work_(other.work_), size_(other.size_) {
    other.setup_ = nullptr;
    other.buf_ = nullptr;
    other.work_ = nullptr;
    other.size_ = 0;
}

ComplexFFT& ComplexFFT::operator=(ComplexFFT&& other) noexcept {
    if (this != &other) {
        release();
        setup_ = other.setup_;
        buf_ = other.buf_;
        work_ = other.work_;
        size_ = other.size_;
        other.setup_ = nullptr;
        other.buf_ = nullptr;
        other.work_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void ComplexFFT::release() noexcept {
    // Guard each free: pffft's docs do not promise NULL-safety, and this runs
    // from the constructor's failure path where some pointers may be null.
    if (setup_ != nullptr) { pffft_destroy_setup(setup_); }
    if (buf_ != nullptr) { pffft_aligned_free(buf_); }
    if (work_ != nullptr) { pffft_aligned_free(work_); }
    setup_ = nullptr;
    buf_ = nullptr;
    work_ = nullptr;
    size_ = 0;
}

void ComplexFFT::transform(const std::complex<float>* in, std::complex<float>* out,
                           bool forwardDirection) {
    // Stage through the aligned buffer in place: pffft_transform_ordered
    // explicitly allows input and output to alias (pffft.h), so one buffer
    // covers both directions of the copy.
    const std::size_t bytes = 2 * size_ * sizeof(float);
    std::memcpy(buf_, in, bytes);
    pffft_transform_ordered(setup_, buf_, buf_, work_,
                            forwardDirection ? PFFFT_FORWARD : PFFFT_BACKWARD);
    std::memcpy(out, buf_, bytes);
}

void ComplexFFT::forward(const std::complex<float>* in, std::complex<float>* out) {
    transform(in, out, true);
}

void ComplexFFT::inverse(const std::complex<float>* in, std::complex<float>* out) {
    transform(in, out, false);
    // pffft's backward transform is unscaled; fold in 1/N here so the
    // documented identity inverse(forward(x)) == x holds.
    const float scale = 1.0f / static_cast<float>(size_);
    for (std::size_t i = 0; i < size_; ++i) { out[i] *= scale; }
}

}  // namespace cascade::dsp
