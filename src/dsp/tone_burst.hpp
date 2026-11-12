// A TONE-BURST GENERATOR: a sine of set frequency and length with a shaped attack and release - the 1750 Hz burst that opens a European FM repeater.
//
// THIS IS A TONE, AND ONLY A TONE. It produces float samples into a buffer the caller gives it. It does not key a transmitter, it knows nothing of a
// transmitter, a radio, a sink or a socket, and this header includes nothing but the C++ standard library (a test scans the source to keep it so). Whether a
// burst may ever be sent to a transmit path is an OWNER DECISION, recorded in docs/feature-records/tuning.md; nothing here may be wired to one without it.
//
// SHAPE. A burst of `lengthMs` milliseconds is exactly N = round(lengthMs x sampleRate / 1000) samples. The first A = round(edgeMs x sampleRate / 1000) samples
// rise and the last A samples fall along a raised cosine (0.5 - 0.5 cos(pi (k + 0.5) / A)), so the burst starts and ends at nearly zero and has no click; the
// edge is limited to half the burst. The sine is phase continuous across render() calls (any chunking gives the same samples) and starts at phase 0 at each
// trigger. Defaults: 1750 Hz, 500 ms, 10 ms edges, amplitude 0.5.
//
// LIMITS: frequency 300..3400 Hz (and below a third of the sample rate), length 50..5000 ms, edge 1..100 ms, amplitude 0..1; a value outside is refused and the
// old value kept (the setter returns false). trigger() while a burst is playing is ignored (false): a held key does not stack bursts.
//
// THREADS: one thread (the audio thread that renders); the setters are for the thread that renders too. No allocation after construction.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstddef>

namespace cascade::dsp {

class ToneBurst {
public:
    explicit ToneBurst(double sampleRateHz = 48000.0) : fs_(sampleRateHz >= 8000.0 && sampleRateHz <= 384000.0 ? sampleRateHz : 48000.0) {}

    bool setFrequencyHz(double hz) {
        if (!std::isfinite(hz) || hz < 300.0 || hz > 3400.0 || hz >= fs_ / 3.0 || active_) { return false; }
        freq_ = hz;
        return true;
    }
    bool setLengthMs(double ms) {
        if (!std::isfinite(ms) || ms < 50.0 || ms > 5000.0 || active_) { return false; }
        lengthMs_ = ms;
        return true;
    }
    bool setEdgeMs(double ms) {
        if (!std::isfinite(ms) || ms < 1.0 || ms > 100.0 || active_) { return false; }
        edgeMs_ = ms;
        return true;
    }
    bool setAmplitude(double a) {
        if (!std::isfinite(a) || a < 0.0 || a > 1.0 || active_) { return false; }
        amp_ = a;
        return true;
    }

    double frequencyHz() const { return freq_; }
    std::size_t totalSamples() const { return static_cast<std::size_t>(std::llround(lengthMs_ * fs_ / 1000.0)); }
    std::size_t edgeSamples() const {
        const std::size_t a = static_cast<std::size_t>(std::llround(edgeMs_ * fs_ / 1000.0));
        const std::size_t half = totalSamples() / 2;
        return a > half ? half : (a == 0 ? 1 : a);
    }

    // Starts a burst. false (nothing changes) while one is playing.
    bool trigger() {
        if (active_) { return false; }
        active_ = true;
        pos_ = 0;
        phase_ = 0.0;
        total_ = totalSamples();
        edge_ = edgeSamples();
        return true;
    }
    // Stops a burst at once (the output goes silent from the next sample: use only for a reset).
    void abort() { active_ = false; pos_ = 0; }
    bool active() const { return active_; }

    // Writes n samples (zeros when no burst is playing) and returns how many of them belong to the burst.
    std::size_t render(float* out, std::size_t n) {
        std::size_t produced = 0;
        const double dphi = 2.0 * 3.14159265358979323846 * freq_ / fs_;
        for (std::size_t i = 0; i < n; ++i) {
            if (!active_) { out[i] = 0.0f; continue; }
            double env = 1.0;
            if (pos_ < edge_) {
                env = 0.5 - 0.5 * std::cos(3.14159265358979323846 * (static_cast<double>(pos_) + 0.5) / static_cast<double>(edge_));
            } else if (pos_ >= total_ - edge_) {
                const std::size_t k = total_ - 1 - pos_;   // 0 at the last sample
                env = 0.5 - 0.5 * std::cos(3.14159265358979323846 * (static_cast<double>(k) + 0.5) / static_cast<double>(edge_));
            }
            out[i] = static_cast<float>(amp_ * env * std::sin(phase_));
            phase_ += dphi;
            if (phase_ > 2.0 * 3.14159265358979323846) { phase_ -= 2.0 * 3.14159265358979323846; }
            ++pos_;
            ++produced;
            if (pos_ >= total_) { active_ = false; }
        }
        return produced;
    }

private:
    double fs_;
    double freq_ = 1750.0, lengthMs_ = 500.0, edgeMs_ = 10.0, amp_ = 0.5;
    bool active_ = false;
    std::size_t pos_ = 0, total_ = 0, edge_ = 1;
    double phase_ = 0.0;
};

}  // namespace cascade::dsp
