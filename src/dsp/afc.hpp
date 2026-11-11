// Automatic frequency control (AFC): a tracker that watches the channel baseband and produces a TUNING CORRECTION in Hz for
// the wiring lane to add to the tuned frequency (a retune of the VFO / NCO). It does not move anything itself.
//
// LOOP: the input is the channel baseband AFTER the receiver has applied the current correction, so each update measures the
// RESIDUAL offset e of the carrier from the channel centre and the correction steps towards it: corr += clamp(gain * e, rate
// limit). Two estimators, one per block of `blockSamples` (default 2048: 43 ms at 48 kHz):
//   MeanFrequency  e = arg( sum conj(x[n-1]) x[n] ) fs / 2 pi: the power-weighted mean frequency of the block (the lag-one
//                  autocorrelation phase; textbook frequency estimator). Right for a carrier-dominated signal: AM with its
//                  carrier, and FM, whose mean frequency over a block is the carrier offset because the deviation is symmetric.
//                  Biased by anything lopsided (selective fading, an adjacent signal, a single sideband): SSB has no carrier and
//                  its mean frequency is the middle of the voice band, not a frequency to centre on, so SSB is NOT a job for
//                  this mode.
//   Peak           a Hann-windowed FFT (ComplexFFT, 2048 points) of each block, the strongest bin inside the search range with
//                  parabolic interpolation of the log magnitudes; the block counts only when the peak stands at least `minPeakDb`
//                  (default 15 dB) above the median bin (the maximum of 2048 noise-only bins is about 10.4 dB above the median).
//                  Right for an unmodulated or keyed carrier: CW, RTTY / FSK-less carriers, pilot tones. Resolution fs / 2048.
// PULL-IN, HOLD: while NOT locked a block is accepted only when the carrier's absolute offset (corr + e) is within pullInHz;
// once locked it is followed out to holdHz and the correction is clamped there (lock is lost beyond it and the carrier has to come
// back inside the pull-in range to be caught again). RATE LIMIT: the correction moves at most maxRateHzPerS. LOCK: |e| under
// lockBandHz for 3 blocks in a row; lost when |e| exceeds 4 x lockBandHz for 3 blocks. DEAD BAND: |e| under deadbandHz moves nothing.
// FREEZE: a block changes nothing (the correction is held) when the channel level is below freezeLevelDb (hysteresis 3 dB), when
// Peak mode finds no peak, or when the wiring says no signal (setSignalPresent(false): drive it from the squelch). Frozen
// time does not unlock the loop.
// Disabled: the correction is reset to 0 once (the wiring must apply 0) and process() returns at once.
//
// THREADS: setters and readers from any thread (atomics); process() from the DSP thread only. No allocation after construction.
// Non-finite samples read as 0. Not wired. Header-only (links the FFT in cascade_engine like the other blocks).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace cascade::dsp {

enum class AfcMode : int { MeanFrequency = 0, Peak = 1 };

class AfcTracker {
public:
    explicit AfcTracker(double channelRateHz = 48000.0, std::size_t blockSamples = 2048)
        : fs_(channelRateHz > 2000.0 ? channelRateHz : 48000.0),
          n_(ComplexFFT::isValidSize(blockSamples) ? blockSamples : 2048),
          fft_(n_),
          win_(makeWindow(WindowType::Hann, n_)),
          buf_(n_),
          spec_(n_),
          mag_(n_),
          scratch_(n_) {}

    void setEnabled(bool on) { enabled_.store(on); }
    void setMode(AfcMode m) { mode_.store(static_cast<int>(m)); }
    void setPullInHz(double hz) { put(pullIn_, hz, 10.0, 0.45 * fs_); }
    void setHoldHz(double hz) { put(hold_, hz, 10.0, 0.45 * fs_); }
    void setMaxRateHzPerS(double hz) { put(rate_, hz, 1.0, 20000.0); }
    void setLoopGain(double g) { put(gain_, g, 0.01, 1.0); }
    void setDeadbandHz(double hz) { put(dead_, hz, 0.0, 200.0); }
    void setLockBandHz(double hz) { put(lockBand_, hz, 1.0, 500.0); }
    void setFreezeLevelDb(double db) { put(freezeDb_, db, -200.0, 0.0); }
    void setMinPeakDb(double db) { put(minPeakDb_, db, 6.0, 60.0); }
    void setSignalPresent(bool present) { present_.store(present); }

    double correctionHz() const { return corr_.load(); }
    bool locked() const { return locked_.load(); }
    bool frozen() const { return frozen_.load(); }
    double lastErrorHz() const { return err_.load(); }
    double levelDb() const { return level_.load(); }
    long updates() const { return updates_.load(); }
    long acceptedUpdates() const { return accepted_.load(); }
    std::size_t blockSamples() const { return n_; }

    void process(const std::complex<float>* x, std::size_t n) {
        if (!enabled_.load()) {
            if (corr_.load() != 0.0 || locked_.load()) { resetState(); }
            return;
        }
        const bool peakMode = mode_.load() == static_cast<int>(AfcMode::Peak);
        for (std::size_t i = 0; i < n; ++i) {
            float re = x[i].real(), im = x[i].imag();
            if (!std::isfinite(re) || !std::isfinite(im)) { re = im = 0.0f; }
            const std::complex<double> v(re, im);
            r1_ += std::conj(prev_) * v;
            pw_ += std::norm(v);
            prev_ = v;
            if (peakMode) { buf_[fill_] = std::complex<float>(static_cast<float>(re * win_[fill_]), static_cast<float>(im * win_[fill_])); }
            if (++fill_ == n_) {
                update(peakMode);
                fill_ = 0;
                r1_ = 0;
                pw_ = 0;
            }
        }
    }

    void reset() { resetState(); }

private:
    static void put(std::atomic<double>& a, double v, double lo, double hi) {
        if (std::isfinite(v)) { a.store(std::min(std::max(v, lo), hi)); }
    }

    void resetState() {
        corr_.store(0.0);
        locked_.store(false);
        frozen_.store(false);
        err_.store(0.0);
        goodRun_ = badRun_ = 0;
        fill_ = 0;
        r1_ = 0;
        pw_ = 0;
        prev_ = 0;
    }

    void update(bool peakMode) {
        updates_.fetch_add(1);
        const double dt = static_cast<double>(n_) / fs_;
        const double levelDb = 10.0 * std::log10(std::max(pw_ / static_cast<double>(n_), 1e-20));
        level_.store(levelDb);
        const double corr = corr_.load();
        const bool wasLocked = locked_.load();
        // Freeze decisions.
        const double fz = freezeDb_.load();
        if (levelLow_) {
            if (levelDb > fz + 3.0) { levelLow_ = false; }
        } else if (levelDb < fz) {
            levelLow_ = true;
        }
        bool freeze = levelLow_ || !present_.load();
        const double limit = wasLocked ? hold_.load() : pullIn_.load();
        double e = 0.0;
        if (!freeze) {
            if (peakMode) {
                fft_.forward(buf_.data(), spec_.data());
                for (std::size_t k = 0; k < n_; ++k) { mag_[k] = std::norm(spec_[k]); }
                // Search range for the RESIDUAL e: the absolute offset corr + e must lie inside +-limit.
                const double lo = std::max(-limit - corr, -0.48 * fs_), hi = std::min(limit - corr, 0.48 * fs_);
                const double binHz = fs_ / static_cast<double>(n_);
                long best = 0;
                bool found = false;
                float bestMag = 0.0f;
                for (long b = static_cast<long>(std::floor(lo / binHz)); b <= static_cast<long>(std::ceil(hi / binHz)); ++b) {
                    const std::size_t k = static_cast<std::size_t>((b + static_cast<long>(n_)) % static_cast<long>(n_));
                    if (mag_[k] > bestMag) { bestMag = mag_[k]; best = b; found = true; }
                }
                std::copy(mag_.begin(), mag_.end(), scratch_.begin());
                std::nth_element(scratch_.begin(), scratch_.begin() + static_cast<long>(n_ / 2), scratch_.end());
                const double median = std::max(static_cast<double>(scratch_[n_ / 2]), 1e-30);
                if (!found || bestMag <= 0.0f || 10.0 * std::log10(static_cast<double>(bestMag) / median) < minPeakDb_.load()) {
                    freeze = true;
                } else {
                    auto at = [&](long b) {
                        return static_cast<double>(mag_[static_cast<std::size_t>((b + static_cast<long>(n_)) % static_cast<long>(n_))]);
                    };
                    const double a = std::log(std::max(at(best - 1), 1e-30)), b0 = std::log(std::max(at(best), 1e-30)),
                                 c = std::log(std::max(at(best + 1), 1e-30));
                    const double den = a - 2.0 * b0 + c;
                    const double delta = den < 0.0 ? std::min(std::max(0.5 * (a - c) / den, -0.5), 0.5) : 0.0;
                    e = (static_cast<double>(best) + delta) * binHz;
                }
            } else {
                if (pw_ <= 0.0) {
                    freeze = true;
                } else {
                    e = std::atan2(r1_.imag(), r1_.real()) / 6.283185307179586 * fs_;
                }
            }
        }
        frozen_.store(freeze);
        if (freeze) { return; }
        err_.store(e);
        const double target = corr + e;  // the carrier's absolute offset from the original tuning
        // Pull-in / hold acceptance.
        double newCorr = corr;
        if (std::fabs(target) > limit) {
            if (wasLocked) {
                // Beyond the hold range: follow to the edge, lose lock.
                const double edge = target > 0 ? limit : -limit;
                newCorr = stepTowards(corr, edge, dt);
                locked_.store(false);
                goodRun_ = 0;
                badRun_ = 0;
                corr_.store(newCorr);
            }
            return;  // not locked and outside the pull-in range: ignored
        }
        accepted_.fetch_add(1);
        if (std::fabs(e) >= dead_.load()) { newCorr = stepTowards(corr, target, dt, gain_.load()); }
        corr_.store(newCorr);
        const double lb = lockBand_.load();
        if (std::fabs(e) < lb) {
            badRun_ = 0;
            if (++goodRun_ >= 3) { locked_.store(true); }
        } else if (std::fabs(e) > 4.0 * lb) {
            goodRun_ = 0;
            if (++badRun_ >= 3) { locked_.store(false); }
        } else {
            goodRun_ = 0;
            badRun_ = 0;
        }
    }

    double stepTowards(double corr, double target, double dt, double g = 1.0) const {
        double step = (target - corr) * g;
        const double maxStep = rate_.load() * dt;
        step = std::min(std::max(step, -maxStep), maxStep);
        return corr + step;
    }

    double fs_;
    std::size_t n_;
    ComplexFFT fft_;
    std::vector<float> win_;
    std::vector<std::complex<float>> buf_, spec_;
    std::vector<float> mag_, scratch_;
    std::size_t fill_ = 0;
    std::complex<double> r1_{0, 0}, prev_{0, 0};
    double pw_ = 0.0;
    int goodRun_ = 0, badRun_ = 0;
    bool levelLow_ = true;
    std::atomic<bool> enabled_{false}, present_{true}, locked_{false}, frozen_{false};
    std::atomic<int> mode_{0};
    std::atomic<double> pullIn_{2500.0}, hold_{5000.0}, rate_{500.0}, gain_{0.5}, dead_{2.0}, lockBand_{20.0}, freezeDb_{-80.0},
        minPeakDb_{15.0}, corr_{0.0}, err_{0.0}, level_{-200.0};
    std::atomic<long> updates_{0}, accepted_{0};
};

}  // namespace cascade::dsp
