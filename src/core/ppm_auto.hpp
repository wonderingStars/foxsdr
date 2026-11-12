// ppm_auto.hpp - AUTOMATIC PPM MEASUREMENT FROM THE 19 kHz FM STEREO PILOT (engine serial lane A, row R18). Pure: no engine, no thread, the clock is the caller's.
//
// THE IDEA. A broadcast FM station's stereo pilot is 19 kHz at the transmitter. A receiver whose sample clock runs p ppm fast (the sign of core/ppm_correction.hpp: positive = the crystal runs FAST) sees
// every frequency in its baseband scaled by 1 / (1 + p x 10^-6), so the pilot lands at 19 kHz - 19,000 x p x 10^-6 Hz: the pilot PLL's residual frequency, df = -0.019 x p Hz, and p = -df / 19,000 x 10^6.
// One ppm is 19 mHz at the pilot. The station's own tuning does not enter: the pilot is baseband, so a mistuned carrier changes nothing.
//
// WHAT THE NUMBER IS WORTH. It is only as good as the station's pilot. The standard allows the pilot +-2 Hz (105 ppm of 19 kHz), a figure so loose it would make this useless if stations used it; whether
// a given station keeps its pilot to a fraction of a ppm is NOT known here and cannot be known from the signal alone: measure two or three stations and trust the ones that agree. The uncertainty this
// class reports is the STATISTICAL one only (the scatter of the PLL's readings over the measurement, on the stated assumption that the readings are independent: the engine takes one every 100 ms).
//
// Readings count only while the pilot is locked and its level is at least 0.5 (a standard pilot reads 1), and not in the first two seconds (the loop settling). A measurement needs 20 readings; one that
// ends with fewer is "failed" (no pilot). Nothing here applies the number: it is shown, and the user (or a screen) sets the correction.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdint>

namespace cascade::core::ppmauto {

inline constexpr double kPilotHz = 19000.0;
inline constexpr double kWarmupS = 2.0;
inline constexpr int kMinReadings = 20;
inline constexpr double kMinLevel = 0.5;

enum class State { Idle, Measuring, Done, Failed };

struct Result {
    State state = State::Idle;
    int readings = 0;
    double progress = 0.0;        // 0..1 of the time asked
    double meanOffsetHz = 0.0;    // the PLL's mean residual frequency
    double ppm = 0.0;             // the receiver's crystal error, positive = fast
    double uncertaintyPpm = 0.0;  // one standard error of the mean, statistical only
};

class Measurement {
public:
    // seconds clamped to [10, 300]. Restarts from nothing.
    void start(double seconds, double nowS) {
        seconds_ = seconds < 10.0 ? 10.0 : (seconds > 300.0 ? 300.0 : seconds);
        t0_ = nowS;
        n_ = 0;
        sum_ = sumSq_ = 0.0;
        state_ = State::Measuring;
    }
    void cancel() { state_ = State::Idle; }
    bool active() const { return state_ == State::Measuring; }
    double seconds() const { return seconds_; }

    // One reading of the pilot PLL. Counted when locked, strong enough and past the warm-up; the measurement ends (Done, or Failed with too few readings) when the time is up.
    void add(double nowS, bool locked, double level, double offsetHz) {
        if (state_ != State::Measuring) { return; }
        if (locked && level >= kMinLevel && std::isfinite(offsetHz) && nowS - t0_ >= kWarmupS) {
            ++n_;
            sum_ += offsetHz;
            sumSq_ += offsetHz * offsetHz;
        }
        if (nowS - t0_ >= seconds_) { state_ = n_ >= kMinReadings ? State::Done : State::Failed; }
    }

    Result result(double nowS) const {
        Result r;
        r.state = state_;
        r.readings = n_;
        r.progress = state_ == State::Idle ? 0.0 : (state_ == State::Measuring ? std::fmin(1.0, std::fmax(0.0, (nowS - t0_) / seconds_)) : 1.0);
        if (n_ > 0) {
            const double mean = sum_ / n_;
            const double var = n_ > 1 ? std::fmax(0.0, (sumSq_ - n_ * mean * mean) / (n_ - 1)) : 0.0;
            r.meanOffsetHz = mean;
            r.ppm = -mean / kPilotHz * 1e6;
            r.uncertaintyPpm = std::sqrt(var / n_) / kPilotHz * 1e6;
        }
        return r;
    }

private:
    State state_ = State::Idle;
    double seconds_ = 30.0, t0_ = 0.0;
    int n_ = 0;
    double sum_ = 0.0, sumSq_ = 0.0;
};

}  // namespace cascade::core::ppmauto
