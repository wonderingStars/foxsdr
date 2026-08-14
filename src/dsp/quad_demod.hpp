// Quadrature (polar) discriminator for FM demodulation.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>

namespace cascade::dsp {

// FM discriminator: y[n] = gain * arg(x[n] * conj(x[n-1])).
//
// The product x[n]*conj(x[n-1]) is a phasor whose argument is the phase advance
// between consecutive samples — the instantaneous frequency in radians/sample —
// so a tone at normalized frequency f (cycles/sample) demodulates to the
// constant 2*pi*f*gain. Taking atan2 of the product, rather than differencing
// two absolute phases, sidesteps phase unwrapping entirely: the result is
// already confined to (-pi, pi], which is exact whenever the per-sample phase
// step stays below pi (i.e. the deviation fits inside Nyquist). It is also
// amplitude-insensitive, since a common magnitude scales both atan2 arguments.
//
// The previous input sample is carried across process() calls so that block
// boundaries are invisible to the output: the pipeline may split a stream at
// any point without producing a spurious click at the seam.
class QuadDemod {
public:
    explicit QuadDemod(float gain = 1.0f) : gain_(gain) {}

    float gain() const { return gain_; }
    void setGain(float g) { gain_ = g; }

    // Forgets the carried sample, as if no input had ever been seen.
    void reset() { prev_ = {1.0f, 0.0f}; }

    // Demodulates n samples. In-place-adjacent use is fine (out is real,
    // in is complex, so they never alias in practice).
    void process(const std::complex<float>* in, float* out, std::size_t n) {
        std::complex<float> prev = prev_;
        for (std::size_t i = 0; i < n; ++i) {
            const std::complex<float> d = in[i] * std::conj(prev);
            out[i] = gain_ * std::atan2(d.imag(), d.real());
            prev = in[i];
        }
        prev_ = prev;
    }

private:
    float gain_;
    // Unit phasor at zero phase: the first output after reset is then the
    // absolute phase of the first input instead of an atan2(0,0) special case.
    std::complex<float> prev_{1.0f, 0.0f};
};

}  // namespace cascade::dsp
