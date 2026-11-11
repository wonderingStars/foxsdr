// Audio equaliser and switchable high-pass / low-pass for the demodulated audio (48 kHz mono; any rate works).
//
// WHAT: bass (low shelf, 150 Hz), mid (peak, 1 kHz, Q 0.9), treble (high shelf, 3.5 kHz), all within +-12 dB; a 5-band
// graphic form (peaks at 100, 300, 1000, 2500 and 5000 Hz, Q 1.2, +-12 dB); a high-pass choice (Off / 100 / 300 Hz) and a
// low-pass choice (Off / 2400 / 3000 / 6000 Hz), each a 4th-order Butterworth (two sections, Q 0.5412 and 1.3066: the textbook
// values 1 / (2 cos(pi/8)) and 1 / (2 cos(3 pi/8))). Section formulas: audio_biquad.hpp (W3C Audio EQ Cookbook).
//
// ZERO COST OFF: with every gain 0 dB (settled) and both choices Off, process() returns at once without touching the samples
// (bit-exact). CLICK-FREE: gains move linearly (1200 dB/s, so 12 dB in 10 ms) in 16-sample steps with the coefficients
// recomputed per step; a high-pass / low-pass choice change runs the old and the new filter side by side and crossfades over
// 10 ms. NO ALLOCATION (fixed arrays). Non-finite input reads as 0. Any chunking of the input gives the same output.
// THREADS: setters from any thread (atomics, picked up at the next process()); process() from the DSP thread only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

#include "dsp/audio_biquad.hpp"

namespace cascade::dsp {

enum class AudioHighPass : int { Off = 0, Hz100 = 100, Hz300 = 300 };
enum class AudioLowPass : int { Off = 0, Hz2400 = 2400, Hz3000 = 3000, Hz6000 = 6000 };

class AudioEq {
public:
    static constexpr int kTone = 3;  // bass, mid, treble
    static constexpr int kGraphic = 5;
    static constexpr int kBands = kTone + kGraphic;
    static constexpr double kMaxGainDb = 12.0;

    static double graphicHz(int band) {
        static constexpr double hz[kGraphic] = {100.0, 300.0, 1000.0, 2500.0, 5000.0};
        return hz[band < 0 ? 0 : (band >= kGraphic ? kGraphic - 1 : band)];
    }

    explicit AudioEq(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 1000.0 ? sampleRateHz : 48000.0) {
        fadeStep_ = 1.0 / (0.010 * fs_);
        hp_.high = true;
        for (auto& g : target_) { g.store(0.0f); }
    }

    void setBassDb(double db) { setGain(0, db); }
    void setMidDb(double db) { setGain(1, db); }
    void setTrebleDb(double db) { setGain(2, db); }
    void setGraphicDb(int band, double db) {  // band 0..4
        if (band >= 0 && band < kGraphic) { setGain(kTone + band, db); }
    }
    void setHighPass(AudioHighPass c) { hpReq_.store(static_cast<int>(c)); }
    void setLowPass(AudioLowPass c) { lpReq_.store(static_cast<int>(c)); }

    // True when the last process() call left the samples alone (everything flat, settled and off).
    bool isBypassed() const { return idle_ && !hp_.fading && !lp_.fading && hp_.curHz == 0 && lp_.curHz == 0; }

    void process(float* x, std::size_t n) {
        pickUp();
        if (isBypassed()) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            if (phase_ == 0) { stepRamps(); }
            phase_ = (phase_ + 1) & 15u;
            double v = static_cast<double>(x[i]);
            if (!std::isfinite(v)) { v = 0.0; }
            if (!idle_) {
                for (int b = 0; b < kBands; ++b) { v = abq::run(coef_[b], st_[b], v); }
            }
            v = hp_.run(v, fadeStep_);
            v = lp_.run(v, fadeStep_);
            x[i] = static_cast<float>(v);
        }
        // Settled at flat gains: let the sections' residue die out, then stop running them.
        if (!idle_ && allFlat_) {
            double r = 0;
            for (int b = 0; b < kBands; ++b) { r += std::fabs(st_[b].z1) + std::fabs(st_[b].z2); }
            if (r < 1e-12) {
                idle_ = true;
                for (auto& s : st_) { s = abq::State{}; }
            }
        }
    }

    void reset() {
        for (auto& s : st_) { s = abq::State{}; }
        hp_.resetAll();
        lp_.resetAll();
    }

private:
    // A 4th-order Butterworth high-pass or low-pass that can be switched on, off or to another cutoff without a click.
    struct Switch4 {
        struct Slot {
            abq::Coef c[2];
            abq::State s[2];
            bool on = false;
            double run(double x) {
                if (!on) { return x; }
                return abq::run(c[1], s[1], abq::run(c[0], s[0], x));
            }
        };
        Slot slot[2];
        int cur = 0;
        int curHz = 0;
        bool fading = false;
        double t = 0.0;
        bool high = false;

        void design(Slot& sl, double fs, int hz) const {
            sl.s[0] = sl.s[1] = abq::State{};
            sl.on = hz > 0;
            if (!sl.on) { return; }
            if (high) {
                sl.c[0] = abq::highpass(fs, hz, 0.5411961);
                sl.c[1] = abq::highpass(fs, hz, 1.3065630);
            } else {
                sl.c[0] = abq::lowpass(fs, hz, 0.5411961);
                sl.c[1] = abq::lowpass(fs, hz, 1.3065630);
            }
        }
        void request(double fs, int hz) {
            if (hz == curHz || fading) { return; }
            design(slot[1 - cur], fs, hz);
            curHz = hz;
            fading = true;
            t = 0.0;
        }
        double run(double x, double step) {
            if (!fading) { return slot[cur].on ? slot[cur].run(x) : x; }
            const double a = slot[cur].run(x);
            const double b = slot[1 - cur].run(x);
            t += step;
            if (t >= 1.0) {
                fading = false;
                cur = 1 - cur;
                return b;
            }
            return a + (b - a) * t;
        }
        void resetAll() {
            for (auto& s : slot) { s.s[0] = s.s[1] = abq::State{}; }
        }
    };

    void setGain(int band, double db) {
        if (!std::isfinite(db)) { return; }
        db = std::fmin(std::fmax(db, -kMaxGainDb), kMaxGainDb);
        target_[static_cast<std::size_t>(band)].store(static_cast<float>(db));
    }
    void pickUp() {
        hp_.request(fs_, hpReq_.load());
        lp_.request(fs_, lpReq_.load());
        for (int b = 0; b < kBands; ++b) {
            tgt_[b] = static_cast<double>(target_[static_cast<std::size_t>(b)].load());
            if (idle_ && tgt_[b] != 0.0) { idle_ = false; }
        }
    }
    void design(int b) {
        const double g = cur_[b];
        if (b == 0) {
            coef_[b] = abq::lowShelf(fs_, 150.0, 0.7071, g);
        } else if (b == 1) {
            coef_[b] = abq::peak(fs_, 1000.0, 0.9, g);
        } else if (b == 2) {
            coef_[b] = abq::highShelf(fs_, 3500.0, 0.7071, g);
        } else {
            coef_[b] = abq::peak(fs_, graphicHz(b - kTone), 1.2, g);
        }
    }
    void stepRamps() {
        if (idle_) { return; }
        const double maxStep = 1200.0 * 16.0 / fs_;  // dB per 16-sample step
        allFlat_ = true;
        for (int b = 0; b < kBands; ++b) {
            if (cur_[b] != tgt_[b]) {
                const double d = tgt_[b] - cur_[b];
                cur_[b] = std::fabs(d) <= maxStep ? tgt_[b] : cur_[b] + (d > 0 ? maxStep : -maxStep);
                design(b);
            }
            if (cur_[b] != 0.0) { allFlat_ = false; }
        }
    }

    double fs_;
    double fadeStep_;
    std::array<std::atomic<float>, kBands> target_;
    std::atomic<int> hpReq_{0}, lpReq_{0};
    double tgt_[kBands] = {};
    double cur_[kBands] = {};
    abq::Coef coef_[kBands];
    abq::State st_[kBands];
    bool allFlat_ = true;
    bool idle_ = true;
    unsigned phase_ = 0;
    Switch4 hp_, lp_;
};

}  // namespace cascade::dsp
