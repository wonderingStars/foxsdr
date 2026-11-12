// test_engine_api_zoom.cpp - THE IF ZOOM THROUGH THE ENGINE (engine serial lane A, row S12): FOXAPP_OP_SET_IF_ZOOM 0x8625 and Engine::zoomFrame.
//   A  off by default (fact.radio.1.zoom reads off); an off with nothing on is NO_CHANGE;
//   B  on: 2,048 bins over 8,000 Hz (applied[0] = the span really shown), the fact says so, frames arrive and a 1,500 Hz carrier is the peak at the bin it belongs to, a repeat is NO_CHANGE;
//   C  refusals: a size that is not a power of two, a width under 100 Hz or over the channel rate (OUT_OF_RANGE), an absent radio (NO_DEVICE); a refusal changes nothing;
//   D  a change of source rate keeps the zoom and it keeps delivering;  E  off again: no frames, the fact reads off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wire1_rig.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

FoxCommand zoom(std::uint32_t target, std::int64_t size, double spanHz) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SET_IF_ZOOM, spanHz, size, nullptr);
    c.target = target;
    return c;
}

class Tone final : public cascade::source::IqSource {
public:
    explicit Tone(double rate) : rate_(rate) {}
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double a = 2.0 * kPi * 1500.0 * static_cast<double>(k_) / rate_;
            dst[i] = {static_cast<float>(0.3 * std::cos(a)), static_cast<float>(0.3 * std::sin(a))};
        }
        return n;
    }
    const char* name() const override { return "tone"; }
    const char* lastError() const override { return ""; }

private:
    double rate_;
    bool running_ = false;
    std::uint64_t k_ = 0;
};

int peakBin(const std::vector<float>& db, std::size_t bins) {
    int best = 0;
    for (std::size_t k = 0; k < bins; ++k) { if (db[k] > db[static_cast<std::size_t>(best)]) { best = static_cast<int>(k); } }
    return best;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_zoom\n");
    engine_api_test::hermetic("zoom");
    Access::installHooks();
    Rig r;
    Access::useSource(r.eng, std::make_unique<Tone>(2400000.0));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);
    std::vector<float> db(16384);

    std::printf("A: off\n");
    CHECK_EQ(r.fact("radio.1.zoom"), std::string("off"));
    CHECK_EQ(r.send(zoom(1, 0, 0.0)).status(), FOXAPI_NO_CHANGE);
    CHECK(r.eng.zoomFrame(1, db.data(), db.size()).frame == 0);

    std::printf("B: on\n");
    testing::Outcome rv = r.send(zoom(1, 2048, 8000.0));
    CHECK_EQ(rv.status(), FOXAPI_OK);
    CHECK(std::fabs(rv.result.applied[0] - 8000.0) < 1e-6);
    CHECK_EQ(r.fact("radio.1.zoom"), std::string("2048 bins of 3.906 Hz over 8000 Hz"));
    CHECK(r.until([&] { return r.eng.zoomFrame(1, db.data(), db.size()).frame >= 3; }, 20000));
    {
        const auto info = r.eng.zoomFrame(1, db.data(), db.size());
        const int want = 1024 + static_cast<int>(std::lround(1500.0 / info.binHz));
        const int got = peakBin(db, info.bins);
        std::printf("  peak at bin %d, wanted %d\n", got, want);
        CHECK(info.bins == 2048 && std::abs(got - want) <= 1);
    }
    CHECK_EQ(r.send(zoom(1, 2048, 8000.0)).status(), FOXAPI_NO_CHANGE);

    std::printf("C: refusals\n");
    CHECK_EQ(r.send(zoom(1, 1000, 8000.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(zoom(1, 2048, 50.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(zoom(1, 2048, 1e7)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(zoom(1, 65536, 8000.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(zoom(9, 2048, 8000.0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.fact("radio.1.zoom"), std::string("2048 bins of 3.906 Hz over 8000 Hz"));

    std::printf("D: a new width\n");
    CHECK_EQ(r.send(zoom(1, 1024, 4000.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.zoom"), std::string("1024 bins of 3.906 Hz over 4000 Hz"));
    CHECK(r.until([&] { return r.eng.zoomFrame(1, db.data(), db.size()).frame >= 2; }, 20000));
    {
        const auto info = r.eng.zoomFrame(1, db.data(), db.size());
        const int want = 512 + static_cast<int>(std::lround(1500.0 / info.binHz));
        const int got = peakBin(db, info.bins);
        std::printf("  peak at bin %d, wanted %d\n", got, want);
        CHECK(info.bins == 1024 && std::abs(got - want) <= 1);
        CHECK(r.eng.zoomFrame(1, db.data(), 512).frame == 0);   // a buffer too small gets nothing
    }

    std::printf("E: off again\n");
    CHECK_EQ(r.send(zoom(1, 0, 0.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.zoom"), std::string("off"));
    CHECK(r.until([&] { return r.eng.zoomFrame(1, db.data(), db.size()).frame == 0; }, 3000));
    return check::finish("test_engine_api_zoom");
}
