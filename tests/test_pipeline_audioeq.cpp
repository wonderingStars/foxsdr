// test_pipeline_notchbank.cpp - THE MANUAL NOTCH BANK IN A RUNNING CHAIN (engine wiring lane 1, group C).
//
// dsp/audio_eq.hpp is proved on its own (test_audio_eq). This proves the PIPELINE runs it on the finished audio of both channels: an AM carrier modulated by two tones (1000 Hz and
// 4000 Hz) goes through the real chain; the treble control at -12 dB takes the 4000 Hz tone down by 6 dB or more and leaves 1000 Hz alone; a 2400 Hz low-pass takes it down 15 dB
// or more; the 300 Hz high-pass leaves both; everything back flat and off, both are as they were; a band or choice that is not one is refused and changes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::dsp::DemodMode;

namespace {

constexpr double kRate = 240000.0;
constexpr double kPi = 3.14159265358979323846;

// An AM carrier, modulated by 1000 Hz and 4000 Hz (each 0.25), looped over one second (both tones are whole cycles in it).
class AmSource final : public cascade::source::IqSource {
public:
    AmSource() : clip_(static_cast<std::size_t>(kRate)) {
        for (std::size_t i = 0; i < clip_.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            clip_[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * t) + 0.25 * std::sin(2.0 * kPi * 4000.0 * t));
        }
    }
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
            dst[i] = std::complex<float>(0.5f * (1.0f + clip_[pos_]), 0.0f);
            if (++pos_ == clip_.size()) { pos_ = 0; }
        }
        return n;
    }
    const char* name() const override { return "AM stand-in"; }
    const char* lastError() const override { return ""; }

private:
    std::vector<float> clip_;
    std::size_t pos_ = 0;
    bool running_ = false;
};

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

double db(double a, double b) { return 10.0 * std::log10(std::max(a, 1e-30) / std::max(b, 1e-30)); }

}  // namespace

int main() {
    std::printf("test_pipeline_audioeq\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    CHECK(p.eqBypassed());
    CHECK(!p.setEqBand(-1, 0.0));
    CHECK(!p.setEqBand(8, 0.0));
    CHECK(!p.setEqBand(0, 12.5));
    CHECK(!p.setEqBand(0, -12.5));
    CHECK(!p.setEqBand(0, std::nan("")));
    CHECK(!p.setAudioFilters(150, 0));
    CHECK(!p.setAudioFilters(0, 2500));
    CHECK(!p.setAudioFilters(-100, 0));
    p.setSource(std::make_unique<AmSource>());
    p.setDemodMode(DemodMode::AM);
    p.setVfoBandwidthHz(12500.0);
    p.setSquelchDb(-100.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 20000));
    letAudioRun(p, 1.0);
    const double a1 = powerAt(p, 1000.0);
    const double a4 = powerAt(p, 4000.0);
    std::printf("  flat: 1000 Hz %.3g, 4000 Hz %.3g\n", a1, a4);
    CHECK(a1 > 1e-6 && a4 > 1e-6);
    CHECK(p.eqBypassed());
    CHECK(p.setEqBand(2, -12.0));   // treble
    letAudioRun(p, 1.0);
    std::printf("  treble -12 dB: 1000 Hz %.1f dB, 4000 Hz %.1f dB\n", db(powerAt(p, 1000.0), a1), db(powerAt(p, 4000.0), a4));
    CHECK(db(powerAt(p, 4000.0), a4) < -6.0);   // (measured -7.4 dB: the treble shelf is at 3.5 kHz)
    CHECK(std::fabs(db(powerAt(p, 1000.0), a1)) < 1.5);
    CHECK(p.setEqBand(2, 0.0));
    CHECK(p.setAudioFilters(0, 2400));
    letAudioRun(p, 1.0);
    std::printf("  low-pass 2400: 1000 Hz %.1f dB, 4000 Hz %.1f dB\n", db(powerAt(p, 1000.0), a1), db(powerAt(p, 4000.0), a4));
    CHECK(db(powerAt(p, 4000.0), a4) < -15.0);   // (measured -18.3 dB: a 4th-order low-pass at 2400 Hz)
    CHECK(std::fabs(db(powerAt(p, 1000.0), a1)) < 1.5);
    CHECK(p.setAudioFilters(300, 0));
    letAudioRun(p, 1.0);
    CHECK(std::fabs(db(powerAt(p, 1000.0), a1)) < 1.5);
    CHECK(std::fabs(db(powerAt(p, 4000.0), a4)) < 1.5);
    CHECK(p.setAudioFilters(0, 0));
    letAudioRun(p, 1.0);
    CHECK(p.eqBypassed());
    CHECK(std::fabs(db(powerAt(p, 1000.0), a1)) < 1.5);
    CHECK(std::fabs(db(powerAt(p, 4000.0), a4)) < 1.5);
    p.stop();
    return testSummary("test_pipeline_audioeq");
}
