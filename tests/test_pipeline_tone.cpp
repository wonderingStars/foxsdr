// test_pipeline_tone.cpp - THE TONE SQUELCH IN A RUNNING CHAIN (engine wiring lane 1, group B).
//
// The detectors (test_ctcss, test_dcs) and the squelch's gate (test_tone_squelch) are proved on their own. This proves the PIPELINE hands them the right signal and uses the
// verdict: a generated narrow-FM transmission (speech-like audio through the 300 Hz high-pass a transmitter has, plus a CTCSS tone or a DCS data stream) goes through the
// real chain on its real threads and
//   A  with the tone squelch Off nothing changes: the audio plays, the carrier gate and the audio gate are the same, nothing is detected;
//   B  with the chosen CTCSS tone the gate opens and the audio plays; with another tone chosen the carrier is open and the audio is exactly silent; "any tone" opens;
//   C  the same for a DCS code, the wrong code and the wrong polarity;
//   D  outside NFM the setting is kept and does nothing;
//   E  the tone remover takes the tone out of what is heard;
//   F  the settings and the refusals: a tone that is not one of the 50, a code that is not one of the 83, a polarity out of range change nothing.
//
// GENERATED SIGNALS ONLY: no real repeater audio was run through this (UNVERIFIED on air, as the blocks' records say).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/ctcss.hpp"
#include "dsp/dcs.hpp"
#include "dsp/demod.hpp"
#include "dsp/tone_squelch.hpp"
#include "signal_block_gen.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::dsp::DemodMode;
namespace tone = cascade::dsp::tone;

namespace {

constexpr double kRate = 240000.0;   // channel rate = input rate (decimation 1)
constexpr double kPi = 3.14159265358979323846;

// A narrow-FM transmission from a looped audio clip: phase accumulates 2 pi dev a / fs. Free-running (the pipeline paces it).
class FmSource final : public cascade::source::IqSource {
public:
    FmSource(std::vector<float> audio, double devHz) : audio_(std::move(audio)), dev_(devHz) {}
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double hz) override { return hz == kRate; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) {
            phase_ += 2.0 * kPi * dev_ * static_cast<double>(audio_[pos_]) / kRate;
            if (phase_ > kPi) { phase_ -= 2.0 * kPi; }
            if (phase_ < -kPi) { phase_ += 2.0 * kPi; }
            dst[i] = std::complex<float>(static_cast<float>(std::cos(phase_)), static_cast<float>(std::sin(phase_)));
            if (++pos_ == audio_.size()) { pos_ = 0; }
        }
        return n;
    }
    const char* name() const override { return "FM stand-in"; }
    const char* lastError() const override { return ""; }

private:
    std::vector<float> audio_;
    double dev_;
    std::size_t pos_ = 0;
    double phase_ = 0.0;
    bool running_ = false;
};

// Speech-like audio after the 300 Hz high-pass a transmitter has (peak about `peak`), `seconds` long, at kRate.
std::vector<float> speech(double seconds, double peak, std::uint32_t seed) {
    std::vector<float> v = sbgen::speechLike(static_cast<std::size_t>(seconds * kRate), kRate, peak, seed);
    cascade::dsp::CtcssHighpass hp(kRate);
    hp.process(v.data(), v.data(), v.size());
    return v;
}

std::vector<float> withCtcss(double toneHz, double toneAmp) {
    std::vector<float> v = speech(4.0, 0.5, 11);
    sbgen::addTone(v, kRate, toneHz, toneAmp, 0, v.size());
    return v;
}

// A DCS stream of `word` (bit j sent j-th), NRZ +-amp at 134.4 bit/s, shaped by two 350 Hz one-pole filters, repeated for `seconds`.
std::vector<float> dcsStream(std::uint32_t word, double amp, double seconds) {
    const std::size_t n = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> v(n);
    const double bitS = 1.0 / 134.4;
    const double a = 1.0 - std::exp(-2.0 * kPi * 350.0 / kRate);
    double y1 = 0.0, y2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const int bit = static_cast<int>(std::floor(t / bitS)) % 23;
        const double x = ((word >> bit) & 1u) ? amp : -amp;
        y1 += a * (x - y1);
        y2 += a * (y1 - y2);
        v[i] = static_cast<float>(y2);
    }
    return v;
}

std::vector<float> withDcs(std::uint32_t word) {
    // 23 words long, so the loop of the source is seamless for the data stream (a jump in the bit clock would drop the lock every pass)
    const double seconds = 23.0 * 23.0 / 134.4;
    std::vector<float> v = speech(seconds, 0.5, 12);
    const std::vector<float> d = dcsStream(word, 0.18, seconds);
    for (std::size_t i = 0; i < v.size() && i < d.size(); ++i) { v[i] += d[i]; }
    return v;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

void letAudioRun(Pipeline& p, double seconds) {
    const std::uint64_t need = static_cast<std::uint64_t>(seconds * Pipeline::kAudioRateHz);
    const std::uint64_t mark = p.audioSamplesProduced();
    waitFor([&] { return p.audioSamplesProduced() - mark >= need; }, 60000);
}

// The RMS of the last ~85 ms of finished audio.
double audioRms(const Pipeline& p) {
    std::vector<float> x(4096);
    const std::size_t n = p.audioTap(x.data(), x.size());
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) { s += static_cast<double>(x[i]) * x[i]; }
    return n != 0 ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

// The power of the last ~85 ms at `hz` (the projection onto a sinusoid there).
double powerAt(const Pipeline& p, double hz) {
    std::vector<float> x(4096);
    const std::size_t n = p.audioTap(x.data(), x.size());
    double c = 0.0, s = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * hz * static_cast<double>(i) / Pipeline::kAudioRateHz;
        c += x[i] * std::cos(ph);
        s += x[i] * std::sin(ph);
    }
    return n != 0 ? (c * c + s * s) / static_cast<double>(n) / static_cast<double>(n) : 0.0;
}

struct Rig {
    std::unique_ptr<Pipeline> p;
    explicit Rig(std::vector<float> audio, DemodMode mode = DemodMode::NFM) {
        Pipeline::Config cfg;
        cfg.sampleRateHz = kRate;
        cfg.fftSize = 1024;
        cfg.audioEnabled = false;
        p = std::make_unique<Pipeline>(cfg);
        p->setSource(std::make_unique<FmSource>(std::move(audio), 3000.0));
        p->setDemodMode(mode);
        p->setVfoBandwidthHz(12500.0);
        p->setSquelchDb(-60.0f);
        p->start();
        CHECK(waitFor([&] { return p->audioSamplesProduced() > 4800; }, 20000));
    }
    ~Rig() { p->stop(); }
};

void testOff() {
    std::printf("A: tone squelch Off changes nothing\n");
    Rig r(withCtcss(100.0, 0.15));
    letAudioRun(*r.p, 1.0);
    CHECK(r.p->toneKind() == 0);
    CHECK(r.p->squelchOpen());
    CHECK(r.p->audioGateOpen());
    CHECK(r.p->toneGateOpen());
    CHECK(r.p->toneDetectedHz() == 0.0);
    CHECK(r.p->toneDetectedCode() == 0);
    CHECK(audioRms(*r.p) > 0.01);
}

void testCtcss() {
    std::printf("B: CTCSS\n");
    Rig r(withCtcss(100.0, 0.15));
    letAudioRun(*r.p, 1.0);
    CHECK(audioRms(*r.p) > 0.01);
    // the transmitted tone is chosen
    CHECK(r.p->setToneSquelch(tone::kCtcss, 100.0, 0, 0));
    CHECK(r.p->toneKind() == 1 && r.p->toneHz() == 100.0);
    CHECK(waitFor([&] { return std::fabs(r.p->toneDetectedHz() - 100.0) < 0.05 && r.p->toneGateOpen() && r.p->audioGateOpen(); }, 15000));
    letAudioRun(*r.p, 0.5);
    CHECK(std::fabs(r.p->toneDetectedHz() - 100.0) < 0.05);
    CHECK(r.p->squelchOpen());
    CHECK(audioRms(*r.p) > 0.01);
    // another tone is chosen: the carrier is open and the audio is silent
    CHECK(r.p->setToneSquelch(tone::kCtcss, 123.0, 0, 0));
    CHECK(waitFor([&] { return !r.p->toneGateOpen(); }, 15000));
    letAudioRun(*r.p, 0.5);
    CHECK(r.p->squelchOpen());                  // the carrier gate alone is open
    CHECK(!r.p->audioGateOpen());               // the audio gate is not
    CHECK(audioRms(*r.p) == 0.0);               // exact digital silence once the ramp is over
    CHECK(std::fabs(r.p->toneDetectedHz() - 100.0) < 0.05);   // the detector still says what it hears
    // any tone
    CHECK(r.p->setToneSquelch(tone::kCtcss, 0.0, 0, 0));
    CHECK(waitFor([&] { return r.p->toneGateOpen(); }, 15000));
    letAudioRun(*r.p, 0.5);
    CHECK(audioRms(*r.p) > 0.01);
    // the right tone again, then off: the audio is back as it was
    CHECK(r.p->setToneSquelch(tone::kCtcss, 123.0, 0, 0));
    CHECK(waitFor([&] { return !r.p->audioGateOpen(); }, 15000));
    CHECK(r.p->setToneSquelch(tone::kOff, 123.0, 0, 0));
    CHECK(waitFor([&] { return r.p->audioGateOpen(); }, 15000));
    letAudioRun(*r.p, 0.5);
    CHECK(audioRms(*r.p) > 0.01);
    CHECK(r.p->toneDetectedHz() == 0.0);
}

void testDcs() {
    std::printf("C: DCS\n");
    const unsigned code = 023;   // written 23
    const std::uint32_t word = cascade::dsp::dcs::encode(code);
    Rig r(withDcs(word));
    letAudioRun(*r.p, 1.0);
    CHECK(r.p->setToneSquelch(tone::kDcs, 0.0, 23, 0));
    CHECK(waitFor([&] { return r.p->toneDetectedCode() == 23 && r.p->toneGateOpen() && r.p->audioGateOpen(); }, 30000));
    letAudioRun(*r.p, 0.5);
    CHECK(r.p->toneDetectedCode() == 23);
    CHECK(r.p->toneDetectedPolarity() == 1);
    CHECK(audioRms(*r.p) > 0.01);
    // another code: closed
    CHECK(r.p->setToneSquelch(tone::kDcs, 0.0, 25, 0));
    CHECK(waitFor([&] { return !r.p->toneGateOpen(); }, 15000));
    letAudioRun(*r.p, 0.5);
    CHECK(r.p->squelchOpen());
    CHECK(!r.p->audioGateOpen());
    CHECK(audioRms(*r.p) == 0.0);
    // the right code, the wrong polarity: closed (a chosen polarity is held to)
    CHECK(r.p->setToneSquelch(tone::kDcs, 0.0, 23, 2));
    letAudioRun(*r.p, 0.5);
    CHECK(waitFor([&] { return !r.p->toneGateOpen(); }, 15000));
    // any code
    CHECK(r.p->setToneSquelch(tone::kDcs, 0.0, 0, 0));
    CHECK(waitFor([&] { return r.p->toneGateOpen(); }, 30000));
}

void testNotNfm() {
    std::printf("D: outside NFM the setting is kept and does nothing\n");
    Rig r(withCtcss(100.0, 0.15), DemodMode::USB);
    CHECK(r.p->setToneSquelch(tone::kCtcss, 123.0, 0, 0));
    letAudioRun(*r.p, 1.0);
    CHECK(r.p->toneKind() == 1);
    CHECK(r.p->toneGateOpen());      // no tone is required outside NFM
    CHECK(r.p->audioGateOpen());
    CHECK(r.p->toneDetectedHz() == 0.0);
    CHECK(audioRms(*r.p) > 0.0);
    // back in NFM the same setting takes hold
    r.p->setDemodMode(DemodMode::NFM);
    CHECK(waitFor([&] { return !r.p->toneGateOpen(); }, 15000));
}

void testRemove() {
    std::printf("E: the tone remover\n");
    Rig r(withCtcss(100.0, 0.2));
    letAudioRun(*r.p, 1.0);
    const double before = powerAt(*r.p, 100.0);
    CHECK(before > 1e-6);
    r.p->setToneRemove(true);
    CHECK(r.p->toneRemove());
    letAudioRun(*r.p, 1.0);
    const double after = powerAt(*r.p, 100.0);
    std::printf("  100 Hz power before %.3g, after %.3g (%.1f dB)\n", before, after, 10.0 * std::log10(before / std::max(after, 1e-30)));
    CHECK(after < before * 1e-3);   // at least 30 dB down (the high-pass is 40 dB there)
    CHECK(audioRms(*r.p) > 0.01);   // the speech above 300 Hz is still there
    r.p->setToneRemove(false);
    letAudioRun(*r.p, 1.0);
    CHECK(powerAt(*r.p, 100.0) > before * 0.2);
}

void testRefusals() {
    std::printf("F: settings and refusals\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(p.toneKind() == 0 && p.toneHz() == 0.0 && p.toneDcsDigits() == 0 && p.tonePolarity() == 0 && !p.toneRemove());
    CHECK(p.setToneSquelch(tone::kCtcss, 88.5, 0, 0));
    CHECK(p.setToneSquelch(tone::kDcs, 0.0, 754, 1));
    // each refusal changes nothing (the last good choice stays)
    CHECK(!p.setToneSquelch(3, 0.0, 0, 0));
    CHECK(!p.setToneSquelch(-1, 0.0, 0, 0));
    CHECK(!p.setToneSquelch(tone::kCtcss, 99.0, 0, 0));
    CHECK(!p.setToneSquelch(tone::kDcs, 0.0, 24, 0));
    CHECK(!p.setToneSquelch(tone::kDcs, 0.0, 23, 3));
    CHECK(!p.setToneSquelch(tone::kDcs, 0.0, 19, 1));
    CHECK(p.toneKind() == 2 && p.toneDcsDigits() == 754 && p.tonePolarity() == 1);
    p.setToneRemove(true);
    CHECK(p.toneRemove());
}

}  // namespace

int main() {
    std::printf("test_pipeline_tone\n");
    testRefusals();
    testOff();
    testCtcss();
    testDcs();
    testNotNfm();
    testRemove();
    return testSummary("test_pipeline_tone");
}
