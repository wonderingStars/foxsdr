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
//  7. EVERY rate a native driver offers is accepted, with an exact integer
//     channel rate inside the range the chain is built for. Through 0.99.28
//     round(rate / 200 kHz) was the only decimation tried, so 2.16, 2.56 and
//     2.88 MS/s on an RTL-SDR (and 2.5 MS/s on an Airspy R2, 12.5 on a
//     HackRF, 500 k on an SDRplay, three Pluto rates) were refused: the radio
//     streamed at the new rate while the chain, the spectrum span and every
//     decoder stayed on the old one (a beta tester's RTL-SDR at 2.56 MS/s).
//  8. At one of those rates the audio really is on the right time base: the
//     CW sidetone leg of 3, repeated on a live switch to 2.56 MS/s.
//  9. The device panel's "rate-follow refused" line clears on the next
//     accepted rate and leaves device errors alone.
//
// Leg 7's lists come from the drivers themselves wherever the list is the
// driver's own (RTL-SDR, HackRF, Mirics, RX888, SDRplay). The Airspy, Airspy
// HF+ and Pluto menus are read from the hardware at open, so those are the
// values this repository's own fakes and fixtures give them (Airspy R2 and
// Mini in test_airspy_source, HF+ in airspyhf_fake_usb.hpp, the AD9361 range
// "[2083333 1 61440000]" in test_pluto_source, turned into the menu
// PlutoSource::supportedSampleRatesHz builds from it).
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
#include <string>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "gui/rate_follow_status.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/rtl2832u.hpp"
#include "source/rx888_protocol.hpp"
#include "source/sdrplay_source.hpp"
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

    // --- 7. Every rate every native driver offers is followed. -------------
    {
        struct Menu {
            const char* driver;
            std::vector<double> rates;
        };
        std::vector<Menu> menus;
        menus.push_back({"RTL-SDR", cascade::source::Rtl2832u::supportedRatesHz()});
        menus.push_back({"HackRF", cascade::source::HackRfSource().supportedSampleRatesHz()});
        menus.push_back({"Mirics", cascade::source::MiriSdrSource().supportedSampleRatesHz()});
        menus.push_back({"RX888", cascade::source::rx888::supportedRatesHz(
                                      cascade::source::rx888::kDefaultAdcRateHz)});
        menus.push_back({"SDRplay", cascade::source::sdrPlaySupportedRatesHz()});
        menus.push_back({"Airspy R2", {2.5e6, 10.0e6}});
        menus.push_back({"Airspy Mini", {3.0e6, 6.0e6}});
        menus.push_back({"Airspy HF+", {192000.0, 256000.0, 384000.0, 456000.0, 768000.0,
                                        912000.0}});
        menus.push_back({"Pluto", {2083333.0, 2.5e6, 3.0e6, 4.0e6, 5.0e6, 6.0e6, 8.0e6,
                                   10.0e6, 12.0e6, 15.0e6, 20.0e6, 25.0e6, 30.72e6, 40.0e6,
                                   50.0e6, 61.44e6}});
        // The generator, the SoapySDR fallback menu and the B200 through UHD.
        menus.push_back({"SoapySDR fallback", {1.0e6, 2.0e6, 4.0e6, 8.0e6}});

        Pipeline q(cfg);  // never started: the rebuild path alone
        int refused = 0;
        for (const Menu& m : menus) {
            CHECK(!m.rates.empty());
            for (const double r : m.rates) {
                const bool ok = q.setInputRateHz(r);
                const double chan = q.channelRateHz();
                const bool integral = chan == std::floor(chan);
                // Below 300 kHz the channel IS the input (decimation 1); from
                // 300 kHz up it lies in [150 kHz, 300 kHz), the range every
                // block after the VFO is built for (pipeline.cpp).
                const bool inRange = (r < 300000.0) ? (chan == r)
                                                    : (chan >= 150000.0 && chan < 300000.0);
                if (!ok || q.inputRateHz() != r || !integral || !inRange) {
                    ++refused;
                    std::printf("  %s %.0f S/s: followed=%d chain=%.0f channel=%.2f\n",
                                m.driver, r, ok ? 1 : 0, q.inputRateHz(), chan);
                }
                CHECK(ok);
                CHECK(q.inputRateHz() == r);
                CHECK(integral);
                CHECK(inRange);
                // A rate the round-to-200-kHz rule already accepted keeps
                // exactly the channel it has always had.
                const double nominal = expectedChannelRate(r);
                if (nominal == std::floor(nominal)) { CHECK(chan == nominal); }
            }
        }
        std::printf("leg 7: %d offered rate(s) not followed\n", refused);
        CHECK(refused == 0);

        // The three RTL-SDR rates the tester could not use, with the channel
        // each one is now given: the one nearest 200 kHz among those wide
        // enough to pass the 150 kHz WFM filter unclipped (>= 150 k / 0.9).
        CHECK(q.setInputRateHz(2160000.0));
        CHECK(q.channelRateHz() == 216000.0);
        CHECK(q.setInputRateHz(2560000.0));
        CHECK(q.channelRateHz() == 256000.0);  // not 160 kHz, which would clip WFM
        CHECK(q.setInputRateHz(2880000.0));
        CHECK(q.channelRateHz() == 192000.0);

        // A rate that no decimation turns into an exact integer channel is
        // still refused, and still changes nothing.
        CHECK(q.setInputRateHz(2000001.0) == false);  // 3 x 666667: no divisor fits
        CHECK(q.setInputRateHz(2560000.5) == false);  // not an integer at all
        CHECK(q.inputRateHz() == 2880000.0);
        CHECK(q.channelRateHz() == 192000.0);

        // A pipeline CONSTRUCTED at such a rate builds the same chain the
        // runtime switch does, rather than a 196923.08 Hz channel.
        Pipeline::Config c256 = cfg;
        c256.sampleRateHz = 2560000.0;
        Pipeline q256(c256);
        CHECK(q256.channelRateHz() == 256000.0);
    }

    // --- 8. The time base at 2.56 MS/s: a carrier 100 kHz off the centre,
    //        tuned by the VFO, must come out as the 700 Hz CW sidetone. A
    //        chain left at the old rate would put the VFO 21.9 kHz off the
    //        carrier (100 k * (1 - 2.0/2.56)) and hear nothing near 700 Hz.
    {
        auto src = std::make_unique<cascade::source::SigGenSource>(2560000.0);
        src->sigGen().setTone(0, 100000.0, 0.0f);
        src->sigGen().setNoiseFloorDb(-300.0f);
        p.setSource(std::move(src));
        CHECK(p.setInputRateHz(2560000.0));
        CHECK(p.channelRateHz() == 256000.0);
        p.setDemodMode(cascade::dsp::DemodMode::CW);
        p.setVfoOffsetHz(100000.0);
        p.start();
        CHECK(p.running());
        audioBase = p.audioSamplesProduced();
        CHECK(waitAudioAdvance(p, audioBase, 3 * 4096));
        const double hz = dominantTapHz(p);
        std::printf("leg 8: CW sidetone at 2.56 MS/s = %.1f Hz\n", hz);
        CHECK(std::fabs(hz - 700.0) <= 40.0);
        p.stop();
    }

    // --- 9. The device panel's red line (gui/rate_follow_status.hpp). A
    //        refusal is shown; the next ACCEPTED rate clears it, so going back
    //        to a working rate does not leave "refused" on screen under a
    //        chain that is running fine (a beta tester's screenshot, 0.99.27);
    //        and a device error on the same line is never cleared by a rate
    //        change. Driven through a real Pipeline, so `accepted` is the
    //        chain's own answer.
    {
        using cascade::gui::sourceErrorAfterRateFollow;
        Pipeline::Config c9 = cfg;
        c9.sampleRateHz = 2048000.0;
        Pipeline r(c9);
        std::string line;

        bool ok = r.setInputRateHz(2000001.0);  // no decimation serves it
        line = sourceErrorAfterRateFollow(line, ok, 2000001.0, r.inputRateHz());
        CHECK(!ok);
        CHECK(line == "DSP rate-follow refused 2000001 S/s; chain stays at 2048000");

        ok = r.setInputRateHz(2048000.0);  // back to the rate it was on: a no-op
        line = sourceErrorAfterRateFollow(line, ok, 2048000.0, r.inputRateHz());
        CHECK(ok);
        CHECK(line.empty());

        r.setInputRateHz(2000001.0);
        line = sourceErrorAfterRateFollow(line, false, 2000001.0, r.inputRateHz());
        ok = r.setInputRateHz(2560000.0);  // a different, accepted rate
        line = sourceErrorAfterRateFollow(line, ok, 2560000.0, r.inputRateHz());
        CHECK(ok);
        CHECK(line.empty());

        const std::string deviceError = "rtlsdr: setting the bias tee failed";
        line = sourceErrorAfterRateFollow(deviceError, true, 2048000.0, 2048000.0);
        CHECK(line == deviceError);
        line = sourceErrorAfterRateFollow(std::string(), true, 2048000.0, 2048000.0);
        CHECK(line.empty());
    }

    return testSummary("test_rate_follow");
}
