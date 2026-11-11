// test_pipeline_notchbank.cpp - THE MANUAL NOTCH BANK IN A RUNNING CHAIN (engine wiring lane 1, group C).
//
// dsp/notch_bank.hpp is proved on its own (test_notch_bank). This proves the PIPELINE runs it on the finished audio of both channels: an AM carrier modulated by two tones
// (1000 Hz and 1700 Hz) goes through the real chain; a notch on 1000 Hz takes that tone down 30 dB or more and leaves the 1700 Hz tone where it was; switching it off brings
// it back; a bad slot or number is refused and changes nothing; with no slot on the audio is the audio it always was (the same RMS to the last digit across two runs is not
// claimed - the source is real time - but the bank's early-out is the block's own proof).
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

// An AM carrier, modulated by 1000 Hz and 1700 Hz (each 0.25), looped over one second (both tones are whole cycles in it).
class AmSource final : public cascade::source::IqSource {
public:
    AmSource() : clip_(static_cast<std::size_t>(kRate)) {
        for (std::size_t i = 0; i < clip_.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            clip_[i] = static_cast<float>(0.25 * std::sin(2.0 * kPi * 1000.0 * t) + 0.25 * std::sin(2.0 * kPi * 1700.0 * t));
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
    std::printf("test_pipeline_notchbank\n");
    Pipeline::Config cfg;
    cfg.sampleRateHz = kRate;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    // refusals first (nothing runs yet)
    CHECK(!p.setManualNotch(-1, 1000.0, 50.0, true));
    CHECK(!p.setManualNotch(10, 1000.0, 50.0, true));
    CHECK(!p.setManualNotch(0, std::nan(""), 50.0, true));
    CHECK(!p.setManualNotch(0, 1000.0, HUGE_VAL, true));
    CHECK(p.manualNotchCount() == 0);
    p.setSource(std::make_unique<AmSource>());
    p.setDemodMode(DemodMode::AM);
    p.setVfoBandwidthHz(12500.0);
    p.setSquelchDb(-100.0f);
    p.start();
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4800; }, 20000));
    letAudioRun(p, 1.0);
    const double a1 = powerAt(p, 1000.0);
    const double a2 = powerAt(p, 1700.0);
    std::printf("  no notch: 1000 Hz %.3g, 1700 Hz %.3g\n", a1, a2);
    CHECK(a1 > 1e-6 && a2 > 1e-6);
    CHECK(p.setManualNotch(0, 1000.0, 50.0, true));
    CHECK(p.manualNotchCount() == 1);
    letAudioRun(p, 1.0);
    const double b1 = powerAt(p, 1000.0);
    const double b2 = powerAt(p, 1700.0);
    std::printf("  notch on 1000 Hz: 1000 Hz %.1f dB, 1700 Hz %.1f dB\n", db(b1, a1), db(b2, a2));
    CHECK(db(b1, a1) < -30.0);
    CHECK(std::fabs(db(b2, a2)) < 1.5);
    // a second notch on the other tone, in the other slot
    CHECK(p.setManualNotch(7, 1700.0, 60.0, true));
    CHECK(p.manualNotchCount() == 2);
    letAudioRun(p, 1.0);
    CHECK(db(powerAt(p, 1700.0), a2) < -30.0);
    // both off: the tones are back
    CHECK(p.setManualNotch(0, 1000.0, 50.0, false));
    CHECK(p.setManualNotch(7, 1700.0, 60.0, false));
    CHECK(p.manualNotchCount() == 0);
    letAudioRun(p, 1.0);
    CHECK(std::fabs(db(powerAt(p, 1000.0), a1)) < 1.5);
    CHECK(std::fabs(db(powerAt(p, 1700.0), a2)) < 1.5);
    p.stop();
    return testSummary("test_pipeline_notchbank");
}
