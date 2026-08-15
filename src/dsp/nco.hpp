// Numerically controlled oscillator: unit-magnitude complex exponential
// generation and frequency translation (mixing).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>

namespace cascade::dsp {

// Phase-accumulator NCO. The phase is kept in *cycles* (turns) in a double and
// wrapped to [-0.5, 0.5) every step, for two reasons:
//   - wrapping by the exactly-representable constant 1.0 adds no rounding
//     error of its own, so a dyadic-rational frequency's phase stays bit-exact
//     indefinitely and any other frequency accumulates at most one ulp-of-1
//     per sample — no long-run drift;
//   - every output sample is computed fresh from cos/sin of the wrapped phase,
//     so magnitude is that of the libm result (1.0 to float rounding) forever.
//     A rotating-phasor recurrence would instead lose magnitude and phase
//     accuracy proportionally to the run length unless renormalized.
class Nco {
public:
    // Sets the oscillator frequency in cycles per sample (normalized to the
    // sample rate), nominal range [-0.5, 0.5]. Values outside are folded back
    // into that range: a sampled oscillator aliases there anyway, and keeping
    // the step bounded lets the per-sample wrap be one conditional add rather
    // than an fmod.
    void setFrequency(double normalized);

    double frequency() const noexcept { return freq_; }

    // Phase in radians, in [-pi, pi): the phase of the *next* sample that
    // generate()/mix() would produce. Exposed so callers (and the long-run
    // test) can compare against the closed-form 2*pi*f*k.
    double phase() const noexcept;

    // Zeroes the phase; the frequency is retained.
    void reset() noexcept { phaseCycles_ = 0.0; }

    // Writes n oscillator samples e^{j*(2*pi*f*k + phi0)} to out. The phase
    // carries across calls, so consecutive blocks are seamless.
    void generate(std::complex<float>* out, std::size_t n);

    // out[k] = in[k] * oscillator[k]: translates the input spectrum by +f.
    // In-place operation (in == out) is allowed.
    void mix(const std::complex<float>* in, std::complex<float>* out,
             std::size_t n);

private:
    void step() noexcept;

    double freq_ = 0.0;         // cycles per sample, folded into [-0.5, 0.5)
    double phaseCycles_ = 0.0;  // current phase in cycles, in [-0.5, 0.5)
};

}  // namespace cascade::dsp
