// Impulse noise blanker for complex baseband at the channel rate.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::dsp {

// Removes short wide-band clicks (ignition, electric fences, switching
// supplies) from a complex stream that is still much wider than the wanted
// signal, so the click is cut out before a narrow filter smears it into a
// thump that lasts as long as the filter's impulse response.
//
// METHOD, per sample:
//   1. Track the average MAGNITUDE (about 20 ms) of the samples that are not
//      part of a click. A cumulative mean for the first 20 ms, so a start-up
//      does not begin from a made-up level.
//   2. A sample whose magnitude exceeds threshold x average is a click. It
//      opens a blanking window; every further click sample extends it.
//   3. The output is the input delayed by kLookAheadS (a delay line), so the
//      window can be placed on the DELAYED stream knowing the click is coming:
//      the gain is ramped to zero over kRampS ending kPreGuardS before the
//      click reaches the output, held at zero through the click and
//      kPostGuardS after the last click sample, and ramped back.
//   4. Stand-down: if more than a quarter of the last 100 ms would be blanked,
//      the blanker lets the signal through (and tracks the average on every
//      sample, so it re-learns the level). A strong signal that has just
//      appeared, or noise so dense it is not a click, is not blanked. It
//      resumes when the share falls below a fifth (hysteresis, so it does not
//      chatter on the edge).
//
// COSTS. While disabled the block is not in the signal path at all: process()
// is never called and there is no delay. While enabled the output lags the
// input by lookAheadSamples() (0.15 ms: 30 samples at 200 kHz).
//
// THE DSP THREAD. Nothing here allocates, locks or logs per sample.
// setSampleRateHz() allocates (the delay line and the 100 ms share ring) and
// belongs to construction and rate changes; everything else is free of it.
class NoiseBlanker {
public:
    // The threshold is dB above the average MAGNITUDE (20 log10 of the ratio).
    static constexpr float kMinThresholdDb = 6.0f;
    static constexpr float kMaxThresholdDb = 30.0f;
    static constexpr float kDefaultThresholdDb = 15.0f;

    static constexpr double kAverageS = 0.020;
    static constexpr double kLookAheadS = 0.00015;
    static constexpr double kPreGuardS = 0.000025;
    static constexpr double kPostGuardS = 0.000025;
    static constexpr double kRampS = 0.000025;
    static constexpr double kShareWindowS = 0.100;
    static constexpr double kStandDownShare = 0.25;
    static constexpr double kResumeShare = 0.20;
    // No decision before this many samples have been averaged, and never on a
    // sample below the floor (an all-zero stream has an average of zero, and
    // every sample above zero is "above threshold").
    static constexpr std::size_t kMinAveraged = 8;
    static constexpr float kAbsoluteFloor = 1.0e-6f;
    // The guard before and after a click is never shorter than this many samples, at any channel rate. Before
    // this floor the pre-guard was round(0.05 ms x rate), 0 samples under 10 kHz, and the post-guard the same with a
    // floor of 1 sample (now both 0.025 ms: they would round to 0 under 20 kHz).
    static constexpr std::size_t kMinGuardSamples = 2;

    explicit NoiseBlanker(double sampleRateHz = 200000.0) { setSampleRateHz(sampleRateHz); }

    // Allocates. Resets the state.
    void setSampleRateHz(double hz) {
        if (!(hz > 0.0)) { hz = 200000.0; }
        rateHz_ = hz;
        look_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kLookAheadS * hz)));
        pre_ = std::max<std::size_t>(kMinGuardSamples, static_cast<std::size_t>(std::lround(kPreGuardS * hz)));
        post_ = std::max<std::size_t>(kMinGuardSamples, static_cast<std::size_t>(std::lround(kPostGuardS * hz)));
        ramp_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kRampS * hz)));
        // The look-ahead must cover the ramp-down and the pre-guard, or the
        // window would have to start before the click was seen.
        look_ = std::max(look_, pre_ + ramp_);
        avgN_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kAverageS * hz)));
        bucketLen_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(hz / 1000.0)));
        const std::size_t buckets =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(kShareWindowS * 1000.0)));
        delay_.assign(look_, std::complex<float>(0.0f, 0.0f));
        wouldBuckets_.assign(buckets, 0u);
        doneBuckets_.assign(buckets, 0u);
        reset();
    }
    double sampleRateHz() const { return rateHz_; }
    std::size_t lookAheadSamples() const { return look_; }

    // Enabling starts from a clean state (an old average or a delay line from
    // before the switch-off would blank against the wrong level).
    void setEnabled(bool on) {
        if (on && !enabled_) { reset(); }
        enabled_ = on;
    }
    bool enabled() const { return enabled_; }

    // Clamped to [kMinThresholdDb, kMaxThresholdDb]; a NaN is the default.
    void setThresholdDb(float db) {
        if (!(db == db)) { db = kDefaultThresholdDb; }
        db = std::min(std::max(db, kMinThresholdDb), kMaxThresholdDb);
        thresholdDb_ = db;
        thresholdLin_ = std::pow(10.0f, db * (1.0f / 20.0f));
    }
    float thresholdDb() const { return thresholdDb_; }

    // Clears the signal state; the settings (enabled, threshold) stay.
    void reset() {
        std::fill(delay_.begin(), delay_.end(), std::complex<float>(0.0f, 0.0f));
        wpos_ = 0;
        avg_ = 0.0f;
        averaged_ = 0;
        hold_ = 0;
        now_ = 0;
        startAt_ = 0;
        endAt_ = 0;
        gain_ = 0.0f;  // ramps up from silence: the first look-ahead is empty
        std::fill(wouldBuckets_.begin(), wouldBuckets_.end(), 0u);
        std::fill(doneBuckets_.begin(), doneBuckets_.end(), 0u);
        wouldSum_ = 0;
        doneSum_ = 0;
        bucketPos_ = 0;
        bucketFill_ = 0;
        curWould_ = 0;
        curDone_ = 0;
        standing_ = false;
        publishedPct_ = 0.0f;
    }

    // The share of the last 100 ms that was actually blanked, in percent
    // (0 while standing down or off), and whether the blanker is standing down. Both
    // are plain reads of values the DSP thread updates once per millisecond of
    // signal; the pipeline copies them to atomics once per block.
    float blankedPercent() const { return enabled_ ? publishedPct_ : 0.0f; }
    bool standingDown() const { return enabled_ && standing_; }

    // In-place operation (out == in) is allowed.
    void process(const std::complex<float>* in, std::complex<float>* out, std::size_t n) {
        const std::size_t L = look_;
        const float thr = thresholdLin_;
        const float step = 1.0f / static_cast<float>(ramp_);
        for (std::size_t i = 0; i < n; ++i) {
            std::complex<float> x = in[i];
            float m = std::sqrt(x.real() * x.real() + x.imag() * x.imag());
            // A non-finite sample is a click by definition, and must not reach
            // the average or the delay line (0 * NaN is NaN, not 0).
            bool trigger = false;
            if (!(m <= 3.0e38f)) {
                x = std::complex<float>(0.0f, 0.0f);
                m = 0.0f;
                trigger = true;
            } else if (averaged_ >= kMinAveraged && m > kAbsoluteFloor && m > thr * avg_) {
                trigger = true;
            }
            const bool clean = !trigger && hold_ == 0;
            if (trigger) {
                hold_ = post_;
                // This sample reaches the output at emission time e. The window
                // is zero from e - pre_ (after a ramp that began ramp_ earlier)
                // to e + post_, extended by every further click sample.
                const std::uint64_t e = now_ + L;
                if (now_ >= endAt_) { startAt_ = e - pre_ - ramp_; }
                endAt_ = std::max(endAt_, e + post_ + 1);
            } else if (hold_ > 0) {
                --hold_;
            }
            // The tracker leaves the clicks out - unless the blanker is
            // standing down, when it must follow the signal wherever it goes.
            if (clean || standing_ || averaged_ < kMinAveraged) {
                const float a = 1.0f / static_cast<float>(std::min(averaged_ + 1, avgN_));
                avg_ += (m - avg_) * a;
                ++averaged_;
            }
            // Delay line: out = in delayed by L samples.
            const std::complex<float> y = delay_[wpos_];
            delay_[wpos_] = x;
            if (++wpos_ == L) { wpos_ = 0; }
            // The window, on the delayed stream.
            const bool would = now_ >= startAt_ && now_ < endAt_;
            ++now_;
            const bool blank = would && !standing_;
            const float target = blank ? 0.0f : 1.0f;
            if (gain_ < target) {
                gain_ = std::min(gain_ + step, target);
            } else if (gain_ > target) {
                gain_ = std::max(gain_ - step, target);
            }
            out[i] = (gain_ == 1.0f) ? y : y * gain_;
            // The share ring: one bucket per millisecond of signal.
            if (would) { ++curWould_; }
            if (blank) { ++curDone_; }
            if (++bucketFill_ == bucketLen_) { closeBucket(); }
        }
    }

private:
    void closeBucket() {
        wouldSum_ -= wouldBuckets_[bucketPos_];
        doneSum_ -= doneBuckets_[bucketPos_];
        wouldBuckets_[bucketPos_] = curWould_;
        doneBuckets_[bucketPos_] = curDone_;
        wouldSum_ += curWould_;
        doneSum_ += curDone_;
        curWould_ = 0;
        curDone_ = 0;
        bucketFill_ = 0;
        if (++bucketPos_ == wouldBuckets_.size()) { bucketPos_ = 0; }
        const double den = static_cast<double>(bucketLen_) * static_cast<double>(wouldBuckets_.size());
        const double would = static_cast<double>(wouldSum_) / den;
        if (!standing_ && would > kStandDownShare) {
            standing_ = true;
        } else if (standing_ && would < kResumeShare) {
            standing_ = false;
        }
        publishedPct_ = static_cast<float>(100.0 * static_cast<double>(doneSum_) / den);
    }

    double rateHz_ = 200000.0;
    std::size_t look_ = 30;
    std::size_t pre_ = 10;
    std::size_t post_ = 10;
    std::size_t ramp_ = 5;
    std::size_t avgN_ = 4000;
    std::size_t bucketLen_ = 200;

    bool enabled_ = false;
    float thresholdDb_ = kDefaultThresholdDb;
    float thresholdLin_ = 5.6234131f;  // 10^(15/20)

    std::vector<std::complex<float>> delay_;
    std::size_t wpos_ = 0;
    float avg_ = 0.0f;
    std::size_t averaged_ = 0;
    std::size_t hold_ = 0;       // input side: samples left of the post-click guard
    std::uint64_t now_ = 0;      // emission-time sample counter
    std::uint64_t startAt_ = 0;  // blanking window [startAt_, endAt_), emission time
    std::uint64_t endAt_ = 0;
    float gain_ = 0.0f;

    std::vector<std::uint32_t> wouldBuckets_;
    std::vector<std::uint32_t> doneBuckets_;
    std::size_t wouldSum_ = 0;
    std::size_t doneSum_ = 0;
    std::size_t bucketPos_ = 0;
    std::size_t bucketFill_ = 0;
    std::uint32_t curWould_ = 0;
    std::uint32_t curDone_ = 0;
    bool standing_ = false;
    float publishedPct_ = 0.0f;
};

}  // namespace cascade::dsp
