// test_engine_api_ppmauto.cpp - THE PILOT PPM MEASUREMENT THROUGH THE ENGINE (engine serial lane A, row R18): FOXAPP_OP_PPM_MEASURE 0x8624.
//   A  refusals: not in WFM (UNSUPPORTED), seconds out of range (OUT_OF_RANGE), a cancel with nothing running (NO_CHANGE), an absent radio (NO_DEVICE); state reads idle;
//   B  a station whose pilot is heard from a crystal 30 ppm fast: start (applied echoes the time), a second start is NO_CHANGE, state reads measuring with a rising progress, then done with a ppm within 3 of +30 and
//      a stated uncertainty; nothing was applied (the radio's ppm correction is untouched);
//   C  a carrier with no pilot: the measurement ends failed after its time; a cancel part-way returns to idle.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fm_stereo_rds_source.hpp"
#include "wire1_rig.hpp"

namespace {

FoxCommand measure(std::uint32_t target, std::int64_t on, double seconds) {
    FoxCommand c = testing::cmd(FOXAPP_OP_PPM_MEASURE, seconds, on, nullptr);
    c.target = target;
    return c;
}
FoxCommand mode(std::uint32_t target, std::int64_t m) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_MODE, 0.0, m, nullptr);
    c.target = target;
    return c;
}

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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_ppmauto\n");
    engine_api_test::hermetic("ppmauto");
    Access::installHooks();
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);

    std::printf("A: refusals\n");
    CHECK_EQ(r.fact("radio.1.ppmauto.state"), std::string("idle"));
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(measure(1, 1, 10.0)).status(), FOXAPI_UNSUPPORTED);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(measure(1, 1, 301.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(measure(1, 1, -1.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(measure(1, 0, 0.0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(measure(9, 1, 10.0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.fact("radio.1.ppmauto.state"), std::string("idle"));

    std::printf("B: a station heard from a crystal 30 ppm fast\n");
    {
        auto src = std::make_unique<fmrds::FmStereoSource>(std::vector<int>{});
        src->setClockPpm(30.0);
        src->setPeakDeviationHz(75000.0);
        Access::useSource(r.eng, std::move(src));
    }
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, fmrds::kVfoBandwidthHz)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    const double ppmBefore = std::atof(r.fact("radio.1.ppm").c_str());
    CHECK_EQ(r.send(measure(1, 1, 14.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(measure(1, 1, 14.0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.fact("radio.1.ppmauto.state"), std::string("measuring"));
    r.until([] { return false; }, 3000);
    const double progress = std::atof(r.fact("radio.1.ppmauto.progress").c_str());
    std::printf("  after 3 s: progress %.2f\n", progress);
    CHECK(progress > 0.1 && progress < 0.6);
    CHECK(r.until([&] { return r.fact("radio.1.ppmauto.state") == "done"; }, 25000));
    {
        const double ppm = std::atof(r.fact("radio.1.ppmauto.ppm").c_str());
        const double unc = std::atof(r.fact("radio.1.ppmauto.uncertainty").c_str());
        const int n = std::atoi(r.fact("radio.1.ppmauto.readings").c_str());
        std::printf("  measured %+.2f ppm +-%.2f from %d readings\n", ppm, unc, n);
        CHECK(std::fabs(ppm - 30.0) < 3.0);
        CHECK(unc >= 0.0 && unc < 3.0);
        CHECK(n > 80);
        CHECK(std::atof(r.fact("radio.1.ppm").c_str()) == ppmBefore);   // a suggestion: nothing was applied
    }

    std::printf("C: no pilot, and a cancel\n");
    Access::useSource(r.eng, std::make_unique<QuietCarrier>());
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);
    CHECK_EQ(r.send(measure(1, 1, 10.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.1.ppmauto.state") == "failed"; }, 20000));
    CHECK_EQ(r.send(measure(1, 1, 60.0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 500);
    CHECK_EQ(r.send(measure(1, 0, 0.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.ppmauto.state"), std::string("idle"));
    return check::finish("test_engine_api_ppmauto");
}
