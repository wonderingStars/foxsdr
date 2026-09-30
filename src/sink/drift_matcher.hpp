// Holds the sound card's lead steady against a radio on a different clock.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// WHY THIS EXISTS (a beta tester on an RTL-SDR and a quad-core laptop:
// "audio being choppy and seeing Audio Underruns start to climb when simply
// listening to FM broadcasts ... Sometimes restarting FoxSDR resolves the
// issue").
//
// The demodulated audio is produced at the RADIO's clock - 48 000 frames for
// every 48 000 frames' worth of I/Q the dongle delivers - and consumed at the
// SOUND CARD's clock. Nothing tied the two together. AudioOut primes a 120 ms
// lead and then simply hopes, so the lead is a quantity that only ever goes
// DOWN:
//
//   * a hiccup that loses I/Q (a USB stall, a DSP block that ran late enough
//     for the source to overflow) takes its length out of the lead for good.
//     Measured on the bench RTL at 2.4 MS/s WFM stereo: the lead stepped from
//     ~130 ms to ~50 ms in one event 84 s in and stayed there, because the
//     producer runs at exactly the consumer's rate and never gets ahead again;
//   * two crystals disagree. A cheap dongle is tens to a hundred ppm off, a
//     laptop codec tens more, so the lead drifts one way for the whole session.
//
// Each lost millisecond stays lost until one more hiccup empties the ring. The
// sink then counts an underrun, plays 120 ms of silence to re-prime and starts
// over: the tester's climbing counter and choppy audio. A restart puts back a
// full 120 ms, which is why that "sometimes" helps.
//
// WHAT IT DOES. Before every write to the sink, the producer tells observe()
// how full the ring is. A slow PI controller turns the distance from
// kTargetFrames into a rate correction of at most kMaxCorrectionPpm, and
// process() resamples the block by that ratio (4-point cubic Hermite). A
// lead that has been knocked down is rebuilt in seconds by playing very
// slightly more audio than arrived, and a steady clock mismatch is absorbed
// by the integral term. At the limit the pitch moves 0.5 % (8.6 cents) for
// the few seconds a big loss takes to rebuild; ordinary drift needs a few
// hundredths of that.
//
// PRODUCER-THREAD ONLY. Everything here runs on the thread that writes the
// sink, and only that thread: no atomics, no locks. The fill level it is fed
// comes from SpscRing::size(), which the producer may read at any time.
#pragma once

#include <array>
#include <cstddef>

namespace cascade::sink {

class DriftMatcher {
public:
    // 160 ms at 48 kHz. Above the 120 ms prime so a freshly primed ring is
    // steered UP to it (a little more headroom against the next hiccup than
    // the prime alone gave), and under half of the 341 ms stereo ring so a
    // burst of producer catch-up still fits without dropping.
    static constexpr double kTargetFrames = 7680.0;

    // The ceiling on the correction: 0.5 %. A realistic two-crystal
    // mismatch is well under 1000 ppm and settles far below this; the
    // headroom is for rebuilding a lead after a large loss in seconds rather
    // than minutes.
    static constexpr double kMaxCorrectionPpm = 5000.0;

    // The integral (learned clock mismatch) is limited separately, to what a
    // real pair of crystals can plausibly disagree by, so a long outage
    // cannot wind it up into a pitch error that outlives the outage.
    static constexpr double kMaxDriftPpm = 1000.0;

    explicit DriftMatcher(double rateHz = 48000.0) : rateHz_(rateHz) {}

    // Forget the learned mismatch and the interpolator's history: a new
    // source or a new sound card is a new pair of clocks.
    void reset();

    // One controller step, called just before a block of `blockFrames` is
    // written. `fillFrames` is how many frames the sink's ring holds right
    // now; `playing` is false while the sink is priming (or has no device),
    // when the fill says nothing about the clocks - the controller then holds
    // what it has learned and waits.
    void observe(std::size_t fillFrames, bool playing, std::size_t blockFrames);

    // Output frames per input frame. 1.0 before the first observation.
    double ratio() const { return 1.0 + correctionPpm_ * 1e-6; }
    double correctionPpm() const { return correctionPpm_; }
    double driftPpm() const { return integralPpm_; }

    // The most process() can produce from `frames` input frames at any
    // ratio this class will ever use: sizes the caller's buffer once.
    static std::size_t maxOut(std::size_t frames) {
        return frames + frames / 150 + 2;
    }

    // `frames` interleaved frames of `channels` (1 or 2) -> up to
    // `outCapFrames` frames at ratio(). Returns frames written. The stream
    // state (the last three input frames and the fractional read position)
    // carries across calls, so any block split resamples the same stream.
    // Fixed latency: two input frames (42 us at 48 kHz).
    std::size_t process(const float* in, std::size_t frames, std::size_t channels,
                        float* out, std::size_t outCapFrames);

private:
    double rateHz_;
    // Controller state.
    double correctionPpm_ = 0.0;
    double integralPpm_ = 0.0;
    double fillAvgFrames_ = 0.0;
    bool fillAvgValid_ = false;
    // Interpolator state: three frames of history and the read position,
    // counted in input frames from the oldest of them.
    std::size_t channels_ = 0;
    std::array<float, 6> hist_{};  // 3 frames of channels_, interleaved
    double pos_ = 1.0;
};

}  // namespace cascade::sink
