// Does the receive chain produce audible, correctly pitched audio at every
// device rate an RTL-SDR can be set to? (Field report, 0.99.58: an NESDR SMArt
// v5 restored at 250,000 S/s, "no audio from my speakers".)
//
// WHAT IS MEASURED. The REAL Pipeline, installed with a source exactly as the
// application installs a radio (setSource, then setInputRateHz to the rate the
// radio reports - AppWindow::followInputRate), tuned to a station carrying a
// 1 kHz tone at an offset from the radio's centre. Two places are read:
//   - the pipeline's audio tap, the samples handed to the sink; and
//   - the SINK ITSELF, drained the way a sound card drains it: a thread that
//     calls AudioOut::pullBlock - the exact function the PortAudio callback
//     is - for 10 ms of frames every 10 ms, so priming, the ring and the
//     starvation counter are the real ones. Its output is what the speakers
//     would be given.
// Two numbers come out of each, by reference arithmetic written in this file:
//   - the audio RMS, which has to be the AGC's steady level whatever the rate;
//   - the share of the audio's power that sits at exactly 1 kHz, which is what
//     separates "a 1 kHz tone" from silence, hiss, a detuned tone or garbage.
// Every rate is compared with the 2.4 MS/s run of the same mode made in the
// same process, so "grossly attenuated" and "wrong-pitched" are relative to a
// rate everyone else's logs show working.
//
// THE RATES are the two windows an RTL2832U can be programmed to
// (225,001-300,000 and 900,001-3,200,000 S/s, Rtl2832u::rateSupported) at
// their edges and at the values a user or a plugin preset actually picks. AM
// runs across all of them; NFM, WFM and USB/LSB run at the rates around 250
// kS/s, where the chain's channel is the input itself (decimation 1).
//
// ONE RATE IS EXPECTED TO BE REFUSED and the test pins what that does to the
// audio: 900,001 S/s has no integer decimation to a 150-300 kHz channel
// (pipeline.hpp, setInputRateHz), so the chain keeps the rate it had while the
// radio streams at the new one, and the tone comes out at the ratio of the two
// rates. The refusal is documented and the panel says so; the number is here
// so nobody has to rediscover it.
//
// Every wait is deadline-bounded on steady_clock, nothing is random (each
// source is a closed-form phasor), and the figures are printed so a failing
// run says which rate and by how much.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "sink/audio_out.hpp"
#include "source/iq_source.hpp"
#include "source/rtl2832u.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::dsp::DemodMode;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kToneHz = 1000.0;
constexpr double kCarrierOffsetHz = 25000.0;  // where on the band the station is
constexpr double kCarrierAmp = 0.3;           // a strong local station, in full-scale units
constexpr auto kDeadline = std::chrono::seconds(60);

// What the transmitter does with its 1 kHz tone.
struct Modulation {
    DemodMode mode;
    const char* name;
    double bandwidthHz;  // the app's default for the mode (kModeDefaultBw)
};
constexpr Modulation kAm{DemodMode::AM, "AM", 10000.0};
constexpr Modulation kNfm{DemodMode::NFM, "NFM", 12500.0};
constexpr Modulation kWfm{DemodMode::WFM, "WFM", 150000.0};
constexpr Modulation kUsb{DemodMode::USB, "USB", 3000.0};
constexpr Modulation kLsb{DemodMode::LSB, "LSB", 3000.0};

// A transmitter at a fixed offset from the radio's centre, synthesised at
// whatever rate the radio runs at, so it is the same waveform at every rate.
// AM and the two SSBs are pairs of unit phasors advanced by a fixed rotation
// (renormalised often enough that the amplitude cannot drift), which costs a
// handful of multiplies per sample at 3.2 MS/s; the FMs accumulate phase.
class TestTransmitter : public cascade::source::IqSource {
public:
    TestTransmitter(double rateHz, const Modulation& mod) : rate_(rateHz), mod_(mod) {
        carrierStep_ = std::polar(1.0, kTwoPi * kCarrierOffsetHz / rate_);
        toneStep_ = std::polar(1.0, kTwoPi * kToneHz / rate_);
        // Single-sideband: one tone 1 kHz above (USB) or below (LSB) the
        // carrier frequency the receiver is tuned to.
        const double sideHz = kCarrierOffsetHz + (mod_.mode == DemodMode::LSB ? -kToneHz : kToneHz);
        ssbStep_ = std::polar(1.0, kTwoPi * sideHz / rate_);
    }

    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double) override { return false; }  // fixed-rate
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override {
        centerHz_ = hz;
        return true;
    }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            std::complex<double> s;
            switch (mod_.mode) {
                case DemodMode::AM: {
                    carrier_ *= carrierStep_;
                    tone_ *= toneStep_;
                    const double env = kCarrierAmp * (1.0 + 0.8 * tone_.imag());
                    s = env * carrier_;
                    break;
                }
                case DemodMode::NFM:
                case DemodMode::WFM: {
                    // 3 kHz peak deviation for narrowband, 50 kHz for broadcast.
                    const double dev = mod_.mode == DemodMode::NFM ? 3000.0 : 50000.0;
                    tone_ *= toneStep_;
                    phase_ += kTwoPi * (kCarrierOffsetHz + dev * tone_.imag()) / rate_;
                    if (phase_ > kTwoPi) { phase_ -= kTwoPi; }
                    s = std::polar(kCarrierAmp, phase_);
                    break;
                }
                default: {  // USB, LSB
                    ssb_ *= ssbStep_;
                    s = kCarrierAmp * ssb_;
                    break;
                }
            }
            if ((++count_ & 0x3FFu) == 0u) {  // keep every phasor unit-length
                carrier_ /= std::abs(carrier_);
                tone_ /= std::abs(tone_);
                ssb_ /= std::abs(ssb_);
            }
            dst[i] = std::complex<float>(static_cast<float>(s.real()), static_cast<float>(s.imag()));
        }
        return n;
    }

    const char* name() const override { return "test transmitter"; }
    const char* lastError() const override { return ""; }

private:
    double rate_;
    Modulation mod_;
    std::complex<double> carrierStep_{1.0, 0.0};
    std::complex<double> toneStep_{1.0, 0.0};
    std::complex<double> ssbStep_{1.0, 0.0};
    std::complex<double> carrier_{1.0, 0.0};
    std::complex<double> tone_{1.0, 0.0};
    std::complex<double> ssb_{1.0, 0.0};
    double phase_ = 0.0;
    std::uint64_t count_ = 0;
    double centerHz_ = 131000000.0;
    bool running_ = false;
};

struct Measured {
    bool accepted = false;       // setInputRateHz said yes
    bool flowed = false;         // audio frames kept arriving
    double chanRateHz = 0.0;
    double rms = 0.0;            // of the last 4096 audio frames, at the tap
    double toneShare = 0.0;      // power at 1 kHz / total power, 0..1
    double dominantHz = 0.0;     // strongest bin of the same window
    double sinkRms = 0.0;        // what the sound card was handed, last 0.5 s
    double sinkToneShare = 0.0;
    bool squelchOpen = false;    // Pipeline::squelchOpen() at the end of the run
    std::uint64_t underruns = 0; // starved callbacks, whole run (printed only)
};

// What the USER has set, as opposed to what the radio is doing.
struct Settings {
    float squelchDb = -120.0f;  // AppConfig::squelchDb's default: open
    bool muted = false;         // the Mute key
};

// Goertzel-style power at exactly `hz` over the window (|X(hz)|^2).
double bandPower(const std::vector<float>& w, double hz) {
    const double rate = Pipeline::kAudioRateHz;
    double re = 0.0;
    double im = 0.0;
    const double step = -kTwoPi * hz / rate;
    for (std::size_t i = 0; i < w.size(); ++i) {
        const double a = step * static_cast<double>(i);
        re += static_cast<double>(w[i]) * std::cos(a);
        im += static_cast<double>(w[i]) * std::sin(a);
    }
    return (re * re + im * im);
}

// Dominant frequency by a direct DFT scan, DC excluded, 20 Hz to 20 kHz.
double dominantHz(const std::vector<float>& w) {
    double best = -1.0;
    double bestHz = 0.0;
    for (double hz = 20.0; hz <= 20000.0; hz += 5.0) {
        const double p = bandPower(w, hz);
        if (p > best) {
            best = p;
            bestHz = hz;
        }
    }
    return bestHz;
}

// RMS and the 1 kHz share of the window. A pure tone of amplitude A at
// exactly 1 kHz has bandPower = (A N / 2)^2 and sum(x^2) = N A^2 / 2, so the
// share is bandPower / (sum(x^2) * N / 2).
void levels(const std::vector<float>& w, double& rms, double& toneShare) {
    double sq = 0.0;
    for (const float v : w) { sq += static_cast<double>(v) * static_cast<double>(v); }
    rms = w.empty() ? 0.0 : std::sqrt(sq / static_cast<double>(w.size()));
    const double total = sq * static_cast<double>(w.size()) / 2.0;
    toneShare = total > 0.0 ? bandPower(w, kToneHz) / total : 0.0;
}

// One run: a fresh pipeline built the way the application builds it (at its
// 2 MS/s generator rate), the radio installed, the chain asked to follow the
// radio's rate, the mode tuned to the carrier, then ~1 s of real time. (The
// whole file runs ~40 runs; ctest's per-test limit is 120 s, so a second each is
// what the budget allows - the AGC settles in a third of that.)
Measured measure(double deviceRateHz, const Modulation& mod, const Settings& user = Settings()) {
    Measured m;
    Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;  // AppWindow's kSampleRateHz
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // no device is opened; the sink still has its ring

    Pipeline p(cfg);
    p.setSource(std::make_unique<TestTransmitter>(deviceRateHz, mod));
    m.accepted = p.setInputRateHz(deviceRateHz);
    m.chanRateHz = p.channelRateHz();

    p.setDemodMode(mod.mode);
    p.setVfoBandwidthHz(mod.bandwidthHz);
    p.setVfoOffsetHz(kCarrierOffsetHz);
    p.setSquelchDb(user.squelchDb);
    p.setAudioMuted(user.muted);
    p.start();

    // THE SOUND CARD: pullBlock is what the PortAudio callback calls, fed here
    // on its own clock, 10 ms of mono frames per 10 ms. The last 0.5 s it
    // delivered is kept for measurement.
    std::atomic<bool> stopPull{false};
    std::vector<float> delivered;  // everything the "device" was handed
    std::thread puller([&] {
        auto next = steady_clock::now();
        std::vector<float> block(480);
        while (!stopPull.load(std::memory_order_relaxed)) {
            next += milliseconds(10);
            cascade::sink::AudioOut::pullBlock(&p.audio(), block.data(), block.size());
            delivered.insert(delivered.end(), block.begin(), block.end());
            std::this_thread::sleep_until(next);
        }
    });

    // About 1 s of audio, then windows that are certainly past the AGC's
    // settling (its decay is ~100-300 ms) and the sink's 120 ms priming.
    const std::uint64_t base = p.audioSamplesProduced();
    const std::uint64_t want = static_cast<std::uint64_t>(1.0 * Pipeline::kAudioRateHz);
    const auto t0 = steady_clock::now();
    while (steady_clock::now() - t0 < kDeadline && p.audioSamplesProduced() - base < want) {
        std::this_thread::sleep_for(milliseconds(5));
    }
    m.flowed = p.audioSamplesProduced() - base >= want;
    m.squelchOpen = p.squelchOpen();

    std::vector<float> w(4096);
    if (p.audioTap(w.data(), w.size()) == w.size()) {
        levels(w, m.rms, m.toneShare);
        m.dominantHz = dominantHz(w);
    }
    stopPull.store(true, std::memory_order_relaxed);
    puller.join();
    p.stop();

    m.underruns = p.audio().underruns();
    constexpr std::size_t kSinkWindow = 24000;  // 0.5 s at 48 kHz
    if (delivered.size() >= kSinkWindow) {
        const std::vector<float> tail(delivered.end() - static_cast<std::ptrdiff_t>(kSinkWindow),
                                      delivered.end());
        levels(tail, m.sinkRms, m.sinkToneShare);
    }
    return m;
}

void print(const char* what, double rate, const Measured& m) {
    std::printf(
        "%-4s device %9.0f S/s  accepted %d  chan %9.0f  tap rms %.4f tone %.3f at %5.0f Hz"
        "  | sink rms %.4f tone %.3f  underruns %llu\n",
        what, rate, m.accepted ? 1 : 0, m.chanRateHz, m.rms, m.toneShare, m.dominantHz, m.sinkRms,
        m.sinkToneShare, static_cast<unsigned long long>(m.underruns));
}

// The checks every working rate has to meet against the same mode's 2.4 MS/s
// baseline.
void checkWorks(const Measured& m, const Measured& ref) {
    CHECK(m.accepted);  // the chain follows a rate the radio can really run at
    CHECK(m.flowed);
    // The same level (the AGC settles on 0.5-peak whatever the rate)...
    CHECK(m.rms > 0.5 * ref.rms);
    CHECK(m.rms < 1.5 * ref.rms);
    // ...the same 1 kHz pitch, and a signal that is a tone rather than hiss.
    CHECK_NEAR(m.dominantHz, kToneHz, 25.0);
    CHECK(m.toneShare > 0.9);
    // And it REACHES THE SOUND CARD as that tone, not as silence.
    CHECK(m.sinkRms > 0.5 * ref.rms);
    CHECK(m.sinkRms < 1.5 * ref.rms);
    CHECK(m.sinkToneShare > 0.9);
}

}  // namespace

int main() {
    // --- AM, every rate an RTL2832U can run at -------------------------------
    const Measured amRef = measure(2400000.0, kAm);
    print("AM", 2400000.0, amRef);
    CHECK(amRef.accepted);
    CHECK(amRef.flowed);
    CHECK(amRef.rms > 0.2 && amRef.rms < 0.6);  // the AGC's 0.5-peak tone
    CHECK_NEAR(amRef.dominantHz, kToneHz, 25.0);
    CHECK(amRef.toneShare > 0.9);
    CHECK(amRef.sinkRms > 0.2 && amRef.sinkRms < 0.6);
    CHECK(amRef.sinkToneShare > 0.9);

    // The edges of both windows, the values the Source panel offers
    // (Rtl2832u::supportedRatesHz) and a few a plugin preset might ask for.
    std::vector<double> rates = {225001.0,  240000.0,  250000.0,  256000.0,  300000.0,
                                 1024000.0, 1920000.0, 2048000.0, 2560000.0, 2880000.0,
                                 3200000.0};
    for (const double r : cascade::source::Rtl2832u::supportedRatesHz()) {
        bool have = false;
        for (const double q : rates) { have = have || q == r; }
        if (!have) { rates.push_back(r); }
    }
    for (const double r : rates) {
        CHECK(cascade::source::Rtl2832u::rateSupported(static_cast<std::uint32_t>(r)));
        const Measured m = measure(r, kAm);
        print("AM", r, m);
        checkWorks(m, amRef);
    }

    // --- 900,001 S/s: refused, and what that does to the audio ---------------
    // The radio is allowed this rate (rateSupported) but the chain is not: no
    // integer decimation lands the channel in 150-300 kHz. The chain stays at
    // the 2 MS/s it was built at, so everything it is handed is 2e6 / 900001 =
    // 2.22 times further out than it believes: the carrier 25 kHz up the band
    // arrives 55.6 kHz up, which is 30.6 kHz from where the VFO sits.
    //
    // WHAT THAT SOUNDS LIKE DEPENDS ON THE CHANNEL FILTER, and this check was
    // first written against the filter that did not narrow (the bandwidth
    // fault: +30 kHz only 5.9 dB down), where the misplaced carrier leaked
    // through and its tone came out at 2222 Hz, level 0.28. With a 10 kHz AM
    // channel that is really 10 kHz, a carrier 30.6 kHz off is rejected and
    // the station is simply not heard (measured: tap rms 0.0000). The thing
    // held here is that one: a refused rate does not deliver the station, and
    // the misplaced carrier does not get through the channel filter.
    {
        CHECK(cascade::source::Rtl2832u::rateSupported(900001u));
        const Measured m = measure(900001.0, kAm);
        print("AM", 900001.0, m);
        CHECK(!m.accepted);
        CHECK(m.rms < 0.1 * amRef.rms);
        CHECK(m.toneShare < 0.1);
    }

    // --- The other modes, at the rates around 250 kS/s -----------------------
    // Decimation 1 means the channel IS the input here (250 kHz for 250 kS/s),
    // which is the case the 2.4 MS/s logs never exercise: the Vfo's filter and
    // each demodulator are built at a channel rate 25% wider than the 200 kHz
    // they are usually given, and WFM needs the whole ~200 kHz of it.
    const Modulation others[] = {kNfm, kWfm, kUsb, kLsb};
    for (const Modulation& mod : others) {
        const Measured ref = measure(2400000.0, mod);
        print(mod.name, 2400000.0, ref);
        CHECK(ref.accepted);
        CHECK(ref.rms > 0.15 && ref.rms < 0.6);
        CHECK_NEAR(ref.dominantHz, kToneHz, 25.0);
        CHECK(ref.toneShare > 0.9);
        for (const double r : {250000.0, 256000.0, 300000.0}) {
            const Measured m = measure(r, mod);
            print(mod.name, r, m);
            checkWorks(m, ref);
        }
    }

    // --- THE OTHER WAY TO HEAR NOTHING: a receiver that is fine and silent ---
    // The field report's log had no "audio:" line in five minutes. That line is
    // the starvation digest, and it only speaks when a callback starved. A
    // radio at 250 kS/s is fed to the sink exactly as at any other rate, so
    // what silences the speakers without leaving a trace is something that
    // changes the SAMPLES and not the CADENCE:
    //   - a squelch threshold above the signal (the gate stays closed and
    //     multiplies the audio by zero);
    //   - the Mute key.
    // Both are measured here at 250 kS/s with the same strong carrier that
    // sounds fine above: the sound card is handed digital silence, the chain
    // keeps feeding it at full rate (flowed), and the gate's own state is what
    // tells the two apart. The carrier's channel power is about -9 dB.
    {
        const Measured open = measure(250000.0, kAm, Settings{-120.0f, false});
        print("AM+", 250000.0, open);
        CHECK(open.squelchOpen);
        CHECK(open.sinkRms > 0.5 * amRef.rms);

        const Measured gated = measure(250000.0, kAm, Settings{-5.0f, false});
        print("SQL", 250000.0, gated);
        CHECK(gated.flowed);               // still being fed, at the normal rate
        CHECK(!gated.squelchOpen);         // the gate is shut...
        CHECK(gated.rms < 1.0e-4);         // ...so the chain's output is silence
        CHECK(gated.sinkRms < 1.0e-4);     // ...and so is the sound card's

        const Measured muted = measure(250000.0, kAm, Settings{-120.0f, true});
        print("MUTE", 250000.0, muted);
        CHECK(muted.flowed);
        CHECK(muted.squelchOpen);          // the gate is open: it is the mute
        CHECK(muted.rms < 1.0e-4);
        CHECK(muted.sinkRms < 1.0e-4);
    }
    return testSummary("test_pipeline_device_rate_audio");
}
