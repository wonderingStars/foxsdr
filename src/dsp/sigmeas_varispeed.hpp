// sigmeas_varispeed.hpp - VARISPEED PLAYBACK of an audio buffer (Baudline's "varispeed"): play a
// recording faster or slower, forwards or backwards, with the pitch moving with the speed (a tape
// machine, not a time-stretcher), the speed changing smoothly while it plays. Row: "varispeed
// playback" (DEPTH-GAPS.md Part B, Baudline).
//
// METHOD. A read position in source samples (a double) advances by `speed` per output sample; each
// output is a cubic Hermite (Catmull-Rom) interpolation of the four source samples around it, with
// indices clamped to the buffer. At speed +1 and a whole-sample position the output is the source
// sample exactly. `setSlew` limits how fast the speed follows a new target (change of speed per
// output sample; 0 = at once), so a speed knob can be turned without a click. Speed is limited to
// +-16 (a negative speed plays backwards; 0 holds the position and outputs the sample under it).
// Reading stops at the end (forwards) or the start (backwards) and `atEnd()` says so.
//
// The block does not copy the buffer; the caller keeps it alive and unchanged while it plays.
// `read` allocates nothing, so it can run on the audio thread. Not thread-safe: one thread drives
// it (a host puts a lock-free command in front of it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_DSP_SIGMEAS_VARISPEED_HPP
#define CASCADE_DSP_SIGMEAS_VARISPEED_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace cascade::sigmeas {

class Varispeed {
public:
    static constexpr double kMaxSpeed = 16.0;

    // Takes a view of `n` samples (n >= 4). False (and no change) for a null pointer or a short buffer.
    bool setSource(const float* data, std::size_t n) {
        if (data == nullptr || n < 4) { return false; }
        data_ = data;
        n_ = n;
        pos_ = 0.0;
        atEnd_ = false;
        return true;
    }
    // Target speed; clamped to +-kMaxSpeed. False (no change) for NaN or infinity.
    bool setSpeed(double s) {
        if (!std::isfinite(s)) { return false; }
        target_ = std::min(kMaxSpeed, std::max(-kMaxSpeed, s));
        if (slew_ <= 0.0) { speed_ = target_; }
        return true;
    }
    // Change of speed per output sample (>= 0; 0 = jump to the target). False for a bad value.
    bool setSlew(double perSample) {
        if (!std::isfinite(perSample) || perSample < 0.0) { return false; }
        slew_ = perSample;
        if (slew_ <= 0.0) { speed_ = target_; }
        return true;
    }
    // Source position in samples, clamped to the buffer. False for NaN.
    bool seek(double samplePos) {
        if (!std::isfinite(samplePos) || n_ == 0) { return false; }
        pos_ = std::min(static_cast<double>(n_ - 1), std::max(0.0, samplePos));
        atEnd_ = false;
        return true;
    }
    double position() const { return pos_; }
    double speed() const { return speed_; }
    double targetSpeed() const { return target_; }
    bool atEnd() const { return atEnd_; }
    std::size_t sourceLength() const { return n_; }

    // Writes up to `count` samples; returns how many (fewer at the end or start of the source).
    std::size_t read(float* out, std::size_t count) {
        if (out == nullptr || data_ == nullptr || count == 0) { return 0; }
        std::size_t produced = 0;
        const double last = static_cast<double>(n_ - 1);
        while (produced < count) {
            if (pos_ < 0.0 || pos_ > last) { atEnd_ = true; break; }
            // follow the target
            if (speed_ != target_) {
                if (speed_ < target_) { speed_ = std::min(target_, speed_ + slew_); }
                else { speed_ = std::max(target_, speed_ - slew_); }
            }
            out[produced++] = sample(pos_);
            pos_ += speed_;
        }
        if (produced < count) { atEnd_ = true; }
        if (pos_ < 0.0 || pos_ > last) { atEnd_ = true; }
        return produced;
    }

private:
    float at(long i) const {
        const long hi = static_cast<long>(n_) - 1;
        return data_[static_cast<std::size_t>(std::min(hi, std::max(0L, i)))];
    }
    float sample(double p) const {
        const long i = static_cast<long>(std::floor(p));
        const double t = p - static_cast<double>(i);
        const double p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        const double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
        const double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
        const double c = -0.5 * p0 + 0.5 * p2;
        return static_cast<float>(((a * t + b) * t + c) * t + p1);
    }

    const float* data_ = nullptr;
    std::size_t n_ = 0;
    double pos_ = 0.0;
    double speed_ = 1.0;
    double target_ = 1.0;
    double slew_ = 0.0;
    bool atEnd_ = false;
};

}  // namespace cascade::sigmeas

#endif  // CASCADE_DSP_SIGMEAS_VARISPEED_HPP
