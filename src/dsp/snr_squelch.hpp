// snr_squelch.hpp - a SIGNAL-TO-NOISE SQUELCH for one channel: it opens when the channel's power stands a set number of dB above its own NOISE FLOOR,
// not above a fixed level, so one setting works on a quiet channel and on a noisy one and does not need retuning when the band's noise moves. Not
// wired (core/airband_monitor.hpp, the place that builds the squelches, is a forbidden file for this lane).
//
// WHAT EXISTS: dsp/squelch.hpp is a LEVEL squelch (one threshold in dB of channel power, 3 dB hysteresis, a hold, a 5 ms ramp). The airband monitor
// gives every channel the same single value. This block is the other kind and is meant to sit beside it per channel.
//
// THE RULES (each tested; the numbers are OUR OWN definition, no outside source is claimed):
//   - POWER: the mean |x|^2 of 5 ms slots, smoothed one-pole in LINEAR power (smoothMs, default 20 ms), read in dB. Every decision is taken once per
//     slot, so any chunking of the input gives the same decisions (the gain ramp is per sample).
//   - NOISE FLOOR (an estimator that follows the minimum): when the smoothed power is BELOW the floor, the floor follows it down with a 50 ms time
//     constant; when it is above, the floor rises no faster than floorRiseDbPerS (default 1 dB/s) while the gate is closed and openRiseDbPerS (default
//     0.05 dB/s) while it is open - so a transmission, however long, does not teach the squelch that it is noise, and a noise floor that really moves
//     up is followed within seconds. For the first learnMs (default 500) the gate stays closed and the floor learns. Known limit: a floor that jumps
//     up by more than the open threshold looks like a signal and holds the gate open until relearn() or until the floor creeps up to it (set
//     maxOpenS > 0 to relearn by itself after that long open; off by default because a continuous broadcast is a legal carrier).
//   - SNR = smoothed power - floor, in dB.
//   - GATE: opens after the SNR has been at least openDb for openDelayMs without a break; closes after it has been below openDb - hysteresisDb for
//     hangMs without a break; between the two thresholds nothing changes (hysteresis). The audio, when given, is multiplied by a gain that moves
//     linearly over 5 ms toward 0 or 1 (no click).
//   - Non-finite samples read as 0 power; disabled = the gate is held open at unity gain and the estimator still runs.
// No allocation. Settings and process() from one thread (the DSP thread; the engine posts changes to it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>

namespace cascade::dsp {

struct SnrSquelchSettings {
    bool enabled = true;
    double openDb = 10.0;
    double hysteresisDb = 3.0;
    double hangMs = 400.0;
    double openDelayMs = 20.0;
    double smoothMs = 20.0;
    double floorRiseDbPerS = 1.0;
    double openRiseDbPerS = 0.05;
    double learnMs = 500.0;
    double maxOpenS = 0.0;
};

class SnrSquelch {
public:
    using Cf = std::complex<float>;
    static constexpr double kSlotSeconds = 0.005;

    explicit SnrSquelch(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 1000.0 ? sampleRateHz : 48000.0) {
        slotLen_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kSlotSeconds * fs_)));
        slotSeconds_ = static_cast<double>(slotLen_) / fs_;
        rampStep_ = 1.0f / static_cast<float>(std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(0.005 * fs_))));
        setSettings(SnrSquelchSettings{});
        reset();
    }

    void setSettings(const SnrSquelchSettings& in) {
        auto fin = [](double v, double old, double lo, double hi) { return std::isfinite(v) ? std::min(std::max(v, lo), hi) : old; };
        SnrSquelchSettings n = s_;
        n.enabled = in.enabled;
        n.openDb = fin(in.openDb, n.openDb, 1.0, 60.0);
        n.hysteresisDb = fin(in.hysteresisDb, n.hysteresisDb, 0.0, 30.0);
        n.hangMs = fin(in.hangMs, n.hangMs, 0.0, 10000.0);
        n.openDelayMs = fin(in.openDelayMs, n.openDelayMs, 0.0, 2000.0);
        n.smoothMs = fin(in.smoothMs, n.smoothMs, 1.0, 1000.0);
        n.floorRiseDbPerS = fin(in.floorRiseDbPerS, n.floorRiseDbPerS, 0.0, 60.0);
        n.openRiseDbPerS = fin(in.openRiseDbPerS, n.openRiseDbPerS, 0.0, 60.0);
        n.learnMs = fin(in.learnMs, n.learnMs, 0.0, 10000.0);
        n.maxOpenS = fin(in.maxOpenS, n.maxOpenS, 0.0, 86400.0);
        s_ = n;
    }
    const SnrSquelchSettings& settings() const { return s_; }

    // Forgets the floor and learns again (the gate closes).
    void relearn() {
        haveFloor_ = false;
        open_ = false;
        above_ = below_ = 0.0;
        learnLeft_ = s_.learnMs * 1e-3;
        openFor_ = 0.0;
    }
    void reset() {
        relearn();
        pl_ = 0.0;
        havePower_ = false;
        slotSum_ = 0.0;
        slotCount_ = 0;
        env_ = 0.0f;
    }

    bool isOpen() const { return !s_.enabled || open_; }
    double snrDb() const { return snr_; }
    double floorDb() const { return floorDb_; }
    double powerDb() const { return powerDb_; }
    bool learning() const { return learnLeft_ > 0.0; }
    float gain() const { return env_; }

    // Judges n samples of the channel's complex baseband and, when `audio` is given, gates it in place (same n).
    void process(const Cf* x, std::size_t n, float* audio = nullptr) {
        if (x == nullptr) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            const double re = std::isfinite(x[i].real()) ? static_cast<double>(x[i].real()) : 0.0;
            const double im = std::isfinite(x[i].imag()) ? static_cast<double>(x[i].imag()) : 0.0;
            slotSum_ += re * re + im * im;
            if (++slotCount_ >= slotLen_) {
                slot(slotSum_ / static_cast<double>(slotCount_));
                slotSum_ = 0.0;
                slotCount_ = 0;
            }
            const float target = (!s_.enabled || open_) ? 1.0f : 0.0f;
            if (env_ < target) { env_ = std::min(target, env_ + rampStep_); }
            else if (env_ > target) { env_ = std::max(target, env_ - rampStep_); }
            if (audio != nullptr) { audio[i] *= env_; }
        }
    }

    // One decision step from a power value (linear, mean |x|^2 over `dtS` seconds). Public so tests and callers that already have a power
    // estimate can drive it directly; process() calls it once per slot.
    void slot(double meanPower, double dtS = 0.0) {
        const double dt = dtS > 0.0 ? dtS : slotSeconds_;
        const double p = std::isfinite(meanPower) ? std::max(meanPower, 0.0) : 0.0;
        const double a = 1.0 - std::exp(-dt / (s_.smoothMs * 1e-3));
        if (!havePower_) { pl_ = p; havePower_ = true; } else { pl_ += a * (p - pl_); }
        powerDb_ = 10.0 * std::log10(std::max(pl_, 1e-20));
        if (!haveFloor_) {
            floorDb_ = powerDb_;
            haveFloor_ = true;
        } else if (powerDb_ < floorDb_) {
            floorDb_ += (powerDb_ - floorDb_) * (1.0 - std::exp(-dt / 0.05));
        } else {
            const double rate = open_ ? s_.openRiseDbPerS : s_.floorRiseDbPerS;
            floorDb_ = std::min(powerDb_, floorDb_ + rate * dt);
        }
        snr_ = powerDb_ - floorDb_;
        if (learnLeft_ > 0.0) {
            learnLeft_ -= dt;
            open_ = false;
            return;
        }
        if (!open_) {
            if (snr_ >= s_.openDb) {
                above_ += dt;
                if (above_ >= s_.openDelayMs * 1e-3) {
                    open_ = true;
                    below_ = 0.0;
                    openFor_ = 0.0;
                }
            } else {
                above_ = 0.0;
            }
        } else {
            openFor_ += dt;
            if (snr_ < s_.openDb - s_.hysteresisDb) {
                below_ += dt;
                if (below_ >= s_.hangMs * 1e-3) {
                    open_ = false;
                    above_ = 0.0;
                }
            } else {
                below_ = 0.0;
            }
            if (s_.maxOpenS > 0.0 && openFor_ >= s_.maxOpenS) { relearn(); }
        }
    }

private:
    double fs_;
    std::size_t slotLen_ = 240;
    double slotSeconds_ = 0.005;
    float rampStep_ = 0.004f;
    SnrSquelchSettings s_{};
    bool open_ = false, haveFloor_ = false, havePower_ = false;
    double pl_ = 0.0, powerDb_ = -200.0, floorDb_ = -200.0, snr_ = 0.0;
    double above_ = 0.0, below_ = 0.0, openFor_ = 0.0, learnLeft_ = 0.0;
    double slotSum_ = 0.0;
    std::size_t slotCount_ = 0;
    float env_ = 0.0f;
};

}  // namespace cascade::dsp
