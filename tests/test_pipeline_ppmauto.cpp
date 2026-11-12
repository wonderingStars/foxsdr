// test_pipeline_ppmauto.cpp - THE PILOT PPM READ FROM A REAL CHAIN (engine serial lane A, row R18): Pipeline::pilotOffsetHz() through core/ppm_auto.hpp.
//
// A synthetic FM stereo transmitter (tests/fm_stereo_rds_source.hpp) is heard through a receiver whose crystal is 0, +25 and -40 ppm off (the transmitter's time axis scaled, so the pilot lands 1/(1+p) off 19 kHz).
// Each reading goes through the measurement the engine uses; the result must come back within 2 ppm of the truth with the right sign. A carrier with no pilot must come back Failed.
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
#include "core/ppm_auto.hpp"
#include "dsp/demod.hpp"
#include "fm_stereo_rds_source.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::ppmauto::Measurement;
using cascade::core::ppmauto::State;

namespace {

class QuietCarrier final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return fmrds::kInputRateHz; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {1.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "carrier"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
};

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Measure for `seconds` of real time through a chain fed by `src`.
cascade::core::ppmauto::Result measure(std::unique_ptr<cascade::source::IqSource> src, double seconds) {
    Pipeline::Config cfg;
    cfg.sampleRateHz = fmrds::kInputRateHz;
    cfg.fftSize = 1024;
    cfg.audioEnabled = false;
    Pipeline p(cfg);
    p.setVfoBandwidthHz(fmrds::kVfoBandwidthHz);
    p.setSquelchDb(-120.0f);
    p.setDemodMode(cascade::dsp::DemodMode::WFM);
    p.setSource(std::move(src));
    p.start();
    Measurement m;
    m.start(seconds, nowS());
    while (m.active()) {
        m.add(nowS(), p.pilotLocked(), static_cast<double>(p.pilotLevel()), p.pilotOffsetHz());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto r = m.result(nowS());
    p.stop();
    return r;
}

}  // namespace

int main() {
    std::printf("test_pipeline_ppmauto\n");
    for (const double truth : {0.0, 25.0, -40.0}) {
        auto src = std::make_unique<fmrds::FmStereoSource>(std::vector<int>{});
        src->setClockPpm(truth);
        src->setPeakDeviationHz(75000.0);
        const auto r = measure(std::move(src), 14.0);
        std::printf("  crystal %+.0f ppm: measured %+.2f ppm +-%.2f (%d readings, state %d)\n", truth, r.ppm, r.uncertaintyPpm, r.readings, static_cast<int>(r.state));
        CHECK(r.state == State::Done);
        CHECK(std::fabs(r.ppm - truth) < 2.0);
    }
    {   // no pilot on the air
        const auto r = measure(std::make_unique<QuietCarrier>(), 11.0);
        std::printf("  no pilot: state %d, %d readings\n", static_cast<int>(r.state), r.readings);
        CHECK(r.state == State::Failed);
    }
    return testSummary("test_pipeline_ppmauto");
}
