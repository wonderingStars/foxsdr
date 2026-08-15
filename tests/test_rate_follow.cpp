// Tests for Pipeline::setInputRateHz — runtime sample-rate switching where
// the DSP chain follows a rate the CALLER set on the device side (here: by
// installing a SigGenSource built at the new rate, since the built-in
// generator is fixed-rate by contract — exactly how a hardware source is
// commanded to a new rate before the chain follows).
//
// What each leg proves:
//  1. The documented decimation policy decim = round(rate / 200 kHz),
//     clamped >= 1, lands the channel rate in [150 kHz, 250 kHz] for
//     {1, 2, 4, 8} MHz — expectations computed in-test from the policy, never
//     read back from the implementation.
//  2. A live 2 MHz -> 4 MHz switch keeps spectrum frames flowing
//     (deadline-bounded, seq-gated) and a +400 kHz tone generated AT 4 MHz
//     lands on the fftshifted bin computed for the 4 MHz span.
//  3. Audio keeps flowing across a switch and the CW sidetone stays 700 Hz.
//     The carrier is placed at DC (0 Hz tone, VFO offset 0): a DC phasor is
//     the same waveform at EVERY sample rate, so the 700 Hz sidetone in the
//     48 kHz output grid depends only on the chain's own rate bookkeeping —
//     that rate-independence is the point of the test. The switch used here
//     (2 MHz -> 300 kHz) CHANGES the channel rate (200 kHz -> 150 kHz),
//     because a switch between rates sharing one channel rate (2 MHz -> 4 MHz
//     both give 200 kHz) could never expose a demodulator that was not
//     rebuilt: a stale 200 kHz demod at a 150 kHz channel emits
//     700 * 150/200 = 525 Hz — far outside the +/-40 Hz gate.
//  4. A same-rate call is a no-op returning true.
//  5. Invalid rates (below 8 kHz, above 61.44 MHz, non-integer channel rate)
//     return false and change nothing — frames and audio keep flowing at the
//     old rate.
//  6. stop() immediately after a switch joins cleanly. (stop() DURING a
//     switch is impossible by construction — setInputRateHz holds the same
//     control mutex stop() takes, per the header contract — so the joinable
//     boundary case is the instant after the switch returns.)
//
// Every wait loop is deadline-bounded on steady_clock (<= 30 s) so a liveness
// bug fails the test instead of hanging it; ctest's 120 s per-test timeout is
// the backstop for a hung join. No randomness is used (the generator noise
// floor is disabled). Audio frequency is measured by a direct DFT scan — an
// independent reference, the same method --selftest uses.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "source/siggen_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace {

constexpr auto kDeadline = std::chrono::seconds(30);
constexpr double kPi = 3.14159265358979323846;

// The DOCUMENTED decimation policy, restated here so every expectation is
// computed from the contract rather than from implementation output.
double expectedChannelRate(double rateHz) {
    double decim = std::round(rateHz / 200000.0);
    if (decim < 1.0) { decim = 1.0; }
    return rateHz / decim;
}

// Polls getLatestFrame until a frame with seq > out.seq arrives or the
// deadline expires; callers CHECK the result so a stalled pipeline fails
// loudly instead of hanging.
bool waitForNewFrame(Pipeline& p, SpectrumFrame& out) {
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline) {
        if (p.getLatestFrame(out)) { return true; }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}

// Deadline-bounded wait for the audio producer counter to advance by at
// least `delta` beyond `base` — proves the audio chain is alive.
bool waitAudioAdvance(Pipeline& p, std::uint64_t base, std::uint64_t delta) {
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline) {
        if (p.audioSamplesProduced() - base >= delta) { return true; }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return false;
}

std::size_t argmax(const std::vector<float>& v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) { best = i; }
    }
    return best;
}

// |a - b| without size_t underflow.
std::size_t absDiff(std::size_t a, std::size_t b) {
    return (a > b) ? (a - b) : (b - a);
}

// Dominant frequency of the last 4096 tap samples, interpreted on the 48 kHz
// output grid. Direct DFT over bins 1..N/2 (DC excluded so it can never win),
// rectangular window — an in-test reference computation, independent of the
// project's FFT wrapper. Returns -1 if the tap is not yet full.
double dominantTapHz(Pipeline& p) {
    std::vector<float> w(4096);
    if (p.audioTap(w.data(), w.size()) != w.size()) { return -1.0; }
    const std::size_t nWin = w.size();
    std::size_t bestBin = 1;
    double bestPow = -1.0;
    for (std::size_t k = 1; k <= nWin / 2; ++k) {
        double re = 0.0;
        double im = 0.0;
        const double s = -2.0 * kPi * static_cast<double>(k) /
                         static_cast<double>(nWin);
        for (std::size_t i = 0; i < nWin; ++i) {
            const double angle = s * static_cast<double>(i);
            const double x = static_cast<double>(w[i]);
            re += x * std::cos(angle);
            im += x * std::sin(angle);
        }
        const double pw = re * re + im * im;
        if (pw > bestPow) {
            bestPow = pw;
            bestBin = k;
        }
    }
    return static_cast<double>(bestBin) * Pipeline::kAudioRateHz /
           static_cast<double>(nWin);
}

}  // namespace

int main() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // chain still runs headless; no device needed

    Pipeline p(cfg);

    // Construction state follows the same policy as runtime switches.
    CHECK(p.inputRateHz() == cfg.sampleRateHz);
    CHECK(p.channelRateHz() == expectedChannelRate(cfg.sampleRateHz));

    // --- 1. Decimation policy across {1, 2, 4, 8} MHz (switched while
    //        stopped: no threads exist, the rebuild path alone is exercised).
    {
        const double rates[] = {1.0e6, 2.0e6, 4.0e6, 8.0e6};
        for (double r : rates) {
            CHECK(p.setInputRateHz(r));
            CHECK(p.inputRateHz() == r);
            const double chan = p.channelRateHz();
            CHECK(chan == expectedChannelRate(r));
            CHECK(chan >= 150000.0);
            CHECK(chan <= 250000.0);
        }
        CHECK(p.setInputRateHz(2.0e6));  // back to the construction rate
    }

    // --- 2. Live switch 2 MHz -> 4 MHz: frames keep flowing and the
    //        spectrum bin mapping follows the new span. --------------------
    std::uint64_t audioBase = 0;
    SpectrumFrame f;
    {
        // Baseline at 2 MHz: 125 kHz is exactly +64 bins from DC at
        // 2 MS/s / 1024 (fftshifted DC at 512 -> bin 576), computed in-test.
        p.sigGen().setTone(0, 125000.0, 0.0f);
        p.sigGen().setNoiseFloorDb(-300.0f);
        p.start();
        CHECK(p.running());
        CHECK(waitForNewFrame(p, f));
        const std::size_t baseBin =
            cfg.fftSize / 2 +
            static_cast<std::size_t>(std::lround(
                125000.0 / 2.0e6 * static_cast<double>(cfg.fftSize)));
        CHECK(f.dbBins.size() == cfg.fftSize);
        CHECK(absDiff(argmax(f.dbBins), baseBin) <= 1);

        // The caller sets the device rate: a generator source BUILT at 4 MHz
        // (SigGenSource is fixed-rate, like an IQ file), tone at +400 kHz.
        auto src = std::make_unique<cascade::source::SigGenSource>(4.0e6);
        src->sigGen().setTone(0, 400000.0, 0.0f);
        src->sigGen().setNoiseFloorDb(-300.0f);
        p.setSource(std::move(src));

        // ...and the DSP chain follows.
        CHECK(p.setInputRateHz(4.0e6));
        CHECK(p.inputRateHz() == 4.0e6);
        CHECK(p.channelRateHz() == expectedChannelRate(4.0e6));

        // 25 strictly newer frames: proves frames keep flowing after the
        // switch AND gives the estimator's EMA (alpha 0.5) 25 halvings to
        // forget the 2 MHz-era tone (~75 dB down — far below the new peak).
        for (int i = 0; i < 25; ++i) { CHECK(waitForNewFrame(p, f)); }

        // Expected bin for the 4 MHz span, computed in-test: fftshifted DC at
        // 512, +400 kHz at 4 MS/s / 1024 bins = +102.4 bins -> nearest 614.
        const std::size_t expBin =
            cfg.fftSize / 2 +
            static_cast<std::size_t>(std::lround(
                400000.0 / 4.0e6 * static_cast<double>(cfg.fftSize)));
        CHECK(f.dbBins.size() == cfg.fftSize);
        CHECK(absDiff(argmax(f.dbBins), expBin) <= 1);
    }

    // --- 3. Audio across a CHANNEL-RATE-CHANGING switch: CW sidetone stays
    //        700 Hz (see the file header for why the carrier sits at DC and
    //        why 2 MHz -> 300 kHz rather than -> 4 MHz). --------------------
    {
        p.setSource(nullptr);           // back to the built-in 2 MHz generator
        CHECK(p.setInputRateHz(2.0e6));
        p.sigGen().setTone(0, 0.0, 0.0f);  // DC carrier, rate-independent
        p.sigGen().setNoiseFloorDb(-300.0f);
        p.setDemodMode(cascade::dsp::DemodMode::CW);
        p.setVfoOffsetHz(0.0);

        // Let 3 tap windows of POST-mode-switch audio flow so the last 4096
        // samples are steady-state (AGC converged, squelch open, resampler
        // primed), then measure. 700 Hz on the 4096-point 48 kHz grid
        // quantizes to 703.125 Hz; +/-40 covers that with margin.
        audioBase = p.audioSamplesProduced();
        CHECK(waitAudioAdvance(p, audioBase, 3 * 4096));
        const double preHz = dominantTapHz(p);
        CHECK(std::fabs(preHz - 700.0) <= 40.0);

        // The switch: 300 kHz -> decim round(1.5) = 2 -> 150 kHz channel
        // (integer, accepted; and a different channel rate than before).
        CHECK(p.setInputRateHz(300000.0));
        CHECK(p.inputRateHz() == 300000.0);
        CHECK(p.channelRateHz() == expectedChannelRate(300000.0));
        CHECK(p.channelRateHz() == 150000.0);

        // audioTap advances after the switch (audio keeps flowing), and once
        // the tap holds only post-switch samples the sidetone is STILL 700 Hz
        // — a demodulator not rebuilt for the 150 kHz channel would emit
        // 700 * 150/200 = 525 Hz here and fail the +/-40 Hz gate.
        audioBase = p.audioSamplesProduced();
        CHECK(waitAudioAdvance(p, audioBase, 3 * 4096));
        const double postHz = dominantTapHz(p);
        CHECK(std::fabs(postHz - 700.0) <= 40.0);
    }

    // --- 4. Same-rate call: cheap no-op, returns true, changes nothing. ----
    {
        CHECK(p.setInputRateHz(300000.0) == true);
        CHECK(p.inputRateHz() == 300000.0);
        CHECK(p.channelRateHz() == 150000.0);
        CHECK(waitForNewFrame(p, f));
    }

    // --- 5. Invalid rates: false, and NOTHING changes — frames and audio
    //        keep flowing at the old rate. ---------------------------------
    {
        CHECK(p.setInputRateHz(3.0) == false);        // below the 8 kHz floor
        CHECK(p.setInputRateHz(100.0e6) == false);    // above 61.44 MHz
        // In range, but decim = round(2000001/200000) = 10 gives a channel
        // rate of 200000.1 Hz — not an integer, so the 48 kHz resampler ratio
        // could not be exact; the contract refuses it.
        CHECK(p.setInputRateHz(2000001.0) == false);

        CHECK(p.inputRateHz() == 300000.0);           // unchanged
        CHECK(p.channelRateHz() == 150000.0);         // unchanged
        CHECK(p.running());
        CHECK(waitForNewFrame(p, f));                 // frames still flowing
        audioBase = p.audioSamplesProduced();
        CHECK(waitAudioAdvance(p, audioBase, 4096));  // audio still flowing
    }

    // --- 6. stop() immediately after a switch joins cleanly. ---------------
    {
        CHECK(p.setInputRateHz(2.0e6));
        const auto t0 = steady_clock::now();
        p.stop();
        const double stopSec =
            std::chrono::duration<double>(steady_clock::now() - t0).count();
        CHECK(p.running() == false);
        CHECK(stopSec < 5.0);
    }

    return testSummary("test_rate_follow");
}
