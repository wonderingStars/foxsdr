// Signal-generator source: up to four phase-continuous CW tones plus an
// optional Gaussian-ish noise floor. This is the hardware-free source that
// feeds the render pipeline, and the reference signal for end-to-end tests:
// its output is fully deterministic (fixed-seed noise, closed-form tone
// phase), so every downstream measurement is reproducible run to run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "dsp/nco.hpp"

namespace cascade::source {

// THREADS. Every member function is safe against every other from any thread:
// the configuration calls and generate() take one mutex. Pipeline::sigGen() is
// documented "configure tones before/while running" and the source thread calls
// generate() on this object, so a caller retuning a slot is, by design, on a
// different thread from the one reading it; that was unsynchronised until 0.99.69
// (ThreadSanitizer found it). The lock is held for one generate() call - a few
// thousand samples - and uncontended on every path the application takes.
class SigGen {
public:
    explicit SigGen(double sampleRateHz);

    // Configures tone `slot` (0..3). freqHz may be negative — complex
    // baseband has distinct negative frequencies — and amplitudeDb <= 0 maps
    // 0 dB to amplitude 1.0 (full scale). Retuning a live slot keeps its
    // accumulated phase, so parameter changes are click-free.
    void setTone(int slot, double freqHz, float amplitudeDb);

    void clearTone(int slot);

    // Sets total complex noise power in dB relative to full scale; -300 or
    // lower disables the noise entirely (exact zeros, not a tiny residue).
    void setNoiseFloorDb(float db);

    // Pull model: fills dst with n samples. Tone phase and the noise stream
    // both carry across calls, so any block-size chunking of the same total
    // length produces the identical sample sequence bit-for-bit.
    void generate(std::complex<float>* dst, std::size_t n);

private:
    float gaussian() noexcept;

    struct Tone {
        cascade::dsp::Nco nco;  // owns the phase; survives retune/clear so
                                // re-enabling a slot stays continuous
        float amp = 0.0f;       // linear amplitude, 10^(dB/20)
        bool active = false;
    };

    double sampleRateHz_;
    // Guards tones_, noiseSigma_ and rng_ (sampleRateHz_ never changes).
    std::mutex mutex_;
    std::array<Tone, 4> tones_{};
    float noiseSigma_ = 0.0f;  // per-component (I or Q) std dev; 0 = disabled
    // Fixed seed, not a random device: the project testing protocol bans
    // unseeded randomness, and a deterministic stream is what makes the
    // chunking-invariance guarantee of generate() testable bit-exactly.
    std::uint32_t rng_ = 0x12345678u;
};

}  // namespace cascade::source
