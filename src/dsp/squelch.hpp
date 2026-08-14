// Signal power meter (S-meter backend) and hysteresis squelch gate.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>

namespace cascade::dsp {

// Exponential-moving-average power meter over |x|^2.
//
// The EMA of instantaneous power (not magnitude) is used because power is the
// quantity S-meters and squelch thresholds are defined on: it is additive for
// independent signal + noise, and a single-pole smoother of it is the cheapest
// unbiased estimator of mean power there is. One pole (rather than a windowed
// average) keeps state to a single float and gives an exponential time
// constant of roughly 1/alpha samples — alpha trades meter ballistics for
// variance, exactly like the mechanical damping of an analog S-meter needle.
class PowerMeter {
public:
    explicit PowerMeter(float alpha = 0.05f) : alpha_(alpha) {}

    // Folds n samples into the average. The EMA carries across calls, so
    // block boundaries are invisible to the reading.
    void process(const std::complex<float>* in, std::size_t n) {
        float ema = ema_;
        for (std::size_t i = 0; i < n; ++i) {
            // std::norm is re^2+im^2 (no sqrt): instantaneous power.
            ema += alpha_ * (std::norm(in[i]) - ema);
        }
        ema_ = ema;
    }

    // Average power in dB (10*log10 of the EMA), floored at -200 dB so that
    // a freshly-reset meter (EMA exactly 0) reports a finite number instead
    // of -inf, which would poison any arithmetic a caller does with it.
    float powerDb() const {
        if (ema_ <= kFloorLinear) { return -200.0f; }
        return 10.0f * std::log10(ema_);
    }

    // Forgets all signal history; the meter reads the -200 dB floor again.
    void reset() { ema_ = 0.0f; }

private:
    // The squelch compares the meter against its thresholds once per sample;
    // exposing the raw linear EMA privately lets it compare in the linear
    // domain (thresholds converted once in setThresholdDb) instead of paying
    // a log10 per sample at RF rates.
    friend class Squelch;
    float powerLinear() const { return ema_; }

    // 10*log10(1e-20) = -200: the linear value at the dB floor.
    static constexpr float kFloorLinear = 1e-20f;

    float alpha_;
    float ema_ = 0.0f;
};

// Power squelch with hysteresis, hold time, and a click-free linear ramp.
//
// Three standard refinements over a bare comparator, each preventing a
// distinct audible artifact:
//   - Hysteresis: the gate opens when the measured power exceeds the
//     threshold but only closes below (threshold - 3 dB). A signal sitting
//     right at the threshold therefore cannot chatter the gate open/closed
//     on every noise wiggle — it must genuinely drop 3 dB to close.
//   - Hold: after the level falls below the close threshold the gate stays
//     open for holdMs more. This bridges the brief gaps inside speech
//     (syllables, squelch tails) that would otherwise clip word endings.
//   - Ramp: the audio is gated by an envelope that moves linearly between
//     0 and 1 over ~5 ms rather than switching instantly; a step multiply
//     would put a wideband click into the audio at every transition.
//
// The gate starts closed (envelope 0): an SDR receiver must come up muted
// until a signal actually crosses the threshold.
class Squelch {
public:
    Squelch(double sampleRateHz) : sampleRate_(sampleRateHz) {
        // ~5 ms of samples, minimum 1 so the envelope always makes progress
        // (and the arithmetic below never divides by zero).
        std::size_t ramp = msToSamples(sampleRateHz, 5.0);
        if (ramp < 1) { ramp = 1; }
        rampStep_ = 1.0f / static_cast<float>(ramp);
        setThresholdDb(-50.0f);  // sensible default for an FM noise floor
        setHoldMs(100.0f);
    }

    // Open above db; close below db - 3 (hysteresis). Thresholds are stored
    // in the linear power domain so the per-sample comparison is a float
    // compare, not a log.
    void setThresholdDb(float db) {
        openLin_ = static_cast<float>(std::pow(10.0, static_cast<double>(db) / 10.0));
        closeLin_ = static_cast<float>(
            std::pow(10.0, static_cast<double>(db - kHysteresisDb) / 10.0));
    }

    void setHoldMs(float ms) {
        holdSamples_ = msToSamples(sampleRate_, static_cast<double>(ms));
    }

    bool isOpen() const { return open_; }

    // Measures power on in (the pre-demod baseband) and gates audioInOut in
    // place. The two arrays share n: the squelch runs at the audio block
    // cadence, one gate decision per sample.
    void process(const std::complex<float>* in, std::size_t n, float* audioInOut) {
        for (std::size_t i = 0; i < n; ++i) {
            meter_.process(in + i, 1);
            const float p = meter_.powerLinear();
            if (p > openLin_) {
                open_ = true;
                holdLeft_ = holdSamples_;
            } else if (p < closeLin_) {
                // Below the close threshold: run down the hold, then close.
                if (open_) {
                    if (holdLeft_ > 0) { --holdLeft_; }
                    if (holdLeft_ == 0) { open_ = false; }
                }
            } else {
                // Inside the hysteresis band: keep the current state. An open
                // gate gets its hold refreshed here so that a later drop
                // below the close threshold always earns the full hold time.
                if (open_) { holdLeft_ = holdSamples_; }
            }

            // Envelope walks linearly toward the gate state and clamps AT the
            // endpoint, so a closed gate reaches exactly 0 (true silence, not
            // a denormal residue) and an open one exactly unity gain.
            const float target = open_ ? 1.0f : 0.0f;
            if (env_ < target) {
                env_ += rampStep_;
                if (env_ > target) { env_ = target; }
            } else if (env_ > target) {
                env_ -= rampStep_;
                if (env_ < target) { env_ = target; }
            }
            audioInOut[i] *= env_;
        }
    }

private:
    static std::size_t msToSamples(double sampleRateHz, double ms) {
        const double s = sampleRateHz * ms * 1e-3;
        return (s <= 0.0) ? 0u : static_cast<std::size_t>(s + 0.5);
    }

    // 3 dB: the conventional squelch hysteresis width — big enough that
    // Rayleigh-ish noise fading rarely spans it, small enough that a weak
    // signal only slightly above threshold still keys the gate reliably.
    static constexpr float kHysteresisDb = 3.0f;

    double sampleRate_;
    PowerMeter meter_;
    float openLin_ = 0.0f;
    float closeLin_ = 0.0f;
    std::size_t holdSamples_ = 0;
    std::size_t holdLeft_ = 0;
    float rampStep_ = 1.0f;
    float env_ = 0.0f;
    bool open_ = false;
};

}  // namespace cascade::dsp
