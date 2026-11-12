// Noise squelch for FM: opens on the FALL of out-of-band noise, the way the classic FM squelch does, beside the existing
// level squelch (dsp::Squelch in squelch.hpp, which judges channel power before the demodulator).
//
// PRINCIPLE (textbook; the receiver-design books call it "noise-operated squelch"): the output of an FM discriminator with no
// signal is strong wideband noise whose spectrum RISES with frequency; a received carrier "quiets" it. Speech occupies
// roughly 0.3 to 3 kHz, so the noise energy ABOVE the voice band (default 5 kHz and up) is a clean measure of how much carrier
// the receiver has, with no speech in it. The detector is a 4th-order Butterworth high-pass (audio_biquad.hpp, W3C Audio EQ
// Cookbook sections), a mean-square average (one pole, 5 ms) and two thresholds in dB:
//   - the gate OPENS when the band level has been BELOW openDb for openDelayMs (a debounce: noise is random);
//   - it starts to CLOSE when the level rises above openDb + hysteresisDb (default 3 dB), and closes once that has lasted
//     tailMs (default 100 ms: bridges flutter and a signal dipping; set it short to cut the squelch-tail burst);
//   - between the two thresholds the state holds (hysteresis), exactly as the level squelch does.
// The gate gain moves linearly over 5 ms (no click). Starts CLOSED. Disabled = the gate walks to open over 5 ms and then
// process() returns at once (zero cost, samples untouched).
//
// WHICH SIGNAL: the detector wants the discriminator output BEFORE de-emphasis (de-emphasis would tilt the noise band down by
// about 6 dB per octave and the thresholds would move with it); the gate may be applied to the audio after de-emphasis.
// process(detector, n, audio) takes both; they may be the same buffer (the detector sample is read before the audio sample is
// written). The level is relative to full scale of whatever the wiring feeds in, so the threshold is only meaningful together
// with a reference: thresholdForQuietingDb(noiseOnlyLevelDb, quietingDb) = noise-only level minus the wanted quieting.
//
// THREADS: setters from any thread (atomics, picked up at the next process()); process() from the DSP thread only. No allocation;
// non-finite detector samples read as 0; any chunking gives the same output. Not wired. Header-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

#include "dsp/audio_biquad.hpp"

namespace cascade::dsp {

class NoiseSquelch {
public:
    explicit NoiseSquelch(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 8000.0 ? sampleRateHz : 48000.0) {
        alpha_ = 1.0 - std::exp(-1.0 / (0.005 * fs_));
        rampStep_ = 1.0 / (0.005 * fs_);
        design(5000.0);
    }

    void setEnabled(bool on) { enabled_.store(on); }
    void setThresholdDb(double db) {
        if (std::isfinite(db)) { openDb_.store(db); }
    }
    void setHysteresisDb(double db) {
        if (std::isfinite(db)) { hystDb_.store(std::min(std::max(db, 0.0), 20.0)); }
    }
    void setTailMs(double ms) {
        if (std::isfinite(ms)) { tailMs_.store(std::min(std::max(ms, 0.0), 2000.0)); }
    }
    void setOpenDelayMs(double ms) {
        if (std::isfinite(ms)) { openDelayMs_.store(std::min(std::max(ms, 0.0), 1000.0)); }
    }
    // The lower edge of the noise band (Hz); applied at the next process() (control-thread use, it recomputes coefficients).
    void setBandLowHz(double hz) {
        if (std::isfinite(hz)) { bandLow_.store(std::min(std::max(hz, 1500.0), 0.4 * fs_)); }
    }

    static double thresholdForQuietingDb(double noiseOnlyLevelDb, double quietingDb) { return noiseOnlyLevelDb - quietingDb; }

    bool isOpen() const { return open_; }
    double gain() const { return env_; }
    // Mean-square level of the noise band, dB re full scale (-200 for silence).
    double levelDb() const { return p_ <= 1e-20 ? -200.0 : 10.0 * std::log10(p_); }

    void process(const float* detector, std::size_t n, float* audio) {
        const bool en = enabled_.load();
        if (!en && env_ >= 1.0 && !pending_) { return; }
        // EXACT DIGITAL SILENCE (engine wiring lane 1, the guard the block's record asked for): the high-pass states of a block that follows noise decay into DENORMALS
        // on an all-zero input, and a denormal costs about 68 ns a sample (measured by accident in the block's first run). A block of exact zeros therefore clears the filter's
        // states and runs the level and the gate with a detector sample of 0 without touching the filter; the level average is also flushed to 0 once it is below 1e-30.
        bool silent = true;
        for (std::size_t i = 0; i < n; ++i) {
            if (detector[i] != 0.0f) {
                silent = false;
                break;
            }
        }
        if (silent) { s_[0] = s_[1] = abq::State{}; }
        const double bl = bandLow_.load();
        if (bl != bandCur_) { design(bl); }
        const double openLin = std::pow(10.0, openDb_.load() / 10.0);
        const double closeLin = std::pow(10.0, (openDb_.load() + hystDb_.load()) / 10.0);
        const std::size_t tail = static_cast<std::size_t>(tailMs_.load() * 1e-3 * fs_ + 0.5);
        const std::size_t odel = static_cast<std::size_t>(openDelayMs_.load() * 1e-3 * fs_ + 0.5);
        for (std::size_t i = 0; i < n; ++i) {
            double x = static_cast<double>(detector[i]);
            if (!std::isfinite(x)) { x = 0.0; }
            const double b = silent ? 0.0 : abq::run(c_[1], s_[1], abq::run(c_[0], s_[0], x));
            p_ += alpha_ * (b * b - p_);
            if (p_ < 1e-30) { p_ = 0.0; }   // (never a denormal level)
            if (p_ < openLin) {
                closeRun_ = 0;
                if (!open_) {
                    if (++openRun_ >= odel) { open_ = true; }
                }
            } else if (p_ > closeLin) {
                openRun_ = 0;
                if (open_) {
                    if (++closeRun_ >= tail) { open_ = false; }
                }
            } else {
                // Inside the hysteresis band: hold the state, and a run towards either change starts over.
                openRun_ = 0;
                closeRun_ = 0;
            }
            const double target = (!en || open_) ? 1.0 : 0.0;
            if (env_ < target) {
                env_ = std::min(target, env_ + rampStep_);
            } else if (env_ > target) {
                env_ = std::max(target, env_ - rampStep_);
            }
            audio[i] = static_cast<float>(static_cast<double>(audio[i]) * env_);
        }
        pending_ = !en && env_ < 1.0;
    }

    void reset() {
        s_[0] = s_[1] = abq::State{};
        p_ = 0.0;
        env_ = 0.0;
        open_ = false;
        openRun_ = closeRun_ = 0;
    }

private:
    void design(double hz) {
        bandCur_ = hz;
        c_[0] = abq::highpass(fs_, hz, 0.5411961);
        c_[1] = abq::highpass(fs_, hz, 1.3065630);
    }

    double fs_, alpha_, rampStep_;
    std::atomic<bool> enabled_{true};
    std::atomic<double> openDb_{-30.0}, hystDb_{3.0}, tailMs_{100.0}, openDelayMs_{20.0}, bandLow_{5000.0};
    abq::Coef c_[2];
    abq::State s_[2];
    double bandCur_ = 0.0;
    double p_ = 0.0, env_ = 0.0;
    bool open_ = false, pending_ = false;
    std::size_t openRun_ = 0, closeRun_ = 0;
};

}  // namespace cascade::dsp
