// test_engine_api_cwpeak.cpp - THE CW PEAK FILTER THROUGH THE ENGINE (engine serial lane A, row F8): SET_CW_PEAK 0x011F, per radio, a switch, a width of 30 to 500 Hz (default 100) and a boost of 0 to 24 dB (default 12).
//   A  default, each radio its own, a switch alone keeps the numbers, refusals change nothing, the pipeline follows;  B  saved per radio and restored in an in-process restart, junk in a file lands in range.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand peak(std::uint32_t target, std::int64_t on, double width, double boost, bool withNumbers = true) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_CW_PEAK, width, on, nullptr);
    c.target = target;
    c.ival[1] = withNumbers ? 1 : 0;
    c.num[0] = width;
    c.num[1] = boost;
    return c;
}

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).cwPeakOn, 0u);
    CHECK_EQ(r.radio(1).cwPeakWidthHz, 100.0);
    CHECK_EQ(r.radio(1).cwPeakBoostDb, 12.0);
    CHECK_EQ(r.send(peak(2, 1, 60.0, 18.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).cwPeakOn == 1u && r.radio(2).cwPeakWidthHz == 60.0 && r.radio(2).cwPeakBoostDb == 18.0; }, 5000));
    CHECK_EQ(r.radio(1).cwPeakOn, 0u);
    CHECK(Access::pipeline(r.eng, 1).cwPeakOn());
    CHECK(!Access::pipeline(r.eng, 0).cwPeakOn());
    CHECK_EQ(Access::pipeline(r.eng, 1).cwPeakWidthHz(), 60.0);
    CHECK_EQ(r.send(peak(2, 0, 0.0, 0.0, false)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).cwPeakOn == 0u; }, 5000));
    CHECK_EQ(r.radio(2).cwPeakWidthHz, 60.0);
    CHECK_EQ(r.radio(2).cwPeakBoostDb, 18.0);
    for (const double w : {0.0, 29.0, 501.0, -1.0}) { CHECK_EQ(r.send(peak(1, 1, w, 12.0)).status(), FOXAPI_OUT_OF_RANGE); }
    for (const double g : {-1.0, 25.0}) { CHECK_EQ(r.send(peak(1, 1, 100.0, g)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(peak(1, 1, std::nan(""), 12.0)).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(peak(9, 1, 100.0, 12.0)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(peak(1, 1, 9999.0, 12.0), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).cwPeakOn, 0u);
    CHECK(!Access::pipeline(r.eng, 0).cwPeakOn());
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(peak(1, 1, 150.0, 6.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(peak(2, 1, 40.0, 20.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK(file.cwPeakOn);
        CHECK_EQ(file.cwPeakWidthHz, 150.0);
        CHECK_EQ(file.cwPeakBoostDb, 6.0);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK(file.radios[0].cwPeakOn);
        CHECK_EQ(file.radios[0].cwPeakWidthHz, 40.0);
        CHECK_EQ(file.radios[0].cwPeakBoostDb, 20.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).cwPeakWidthHz, 150.0);
        CHECK_EQ(b.radio(2).cwPeakBoostDb, 20.0);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).cwPeakOn() && Access::pipeline(b.eng, 1).cwPeakBoostDb() == 20.0; }, 5000));
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"cwPeakWidthHz\":5,\"cwPeakBoostDb\":99}", "test", back, error));
    CHECK_EQ(back.cwPeakWidthHz, 100.0);
    CHECK_EQ(back.cwPeakBoostDb, 24.0);
    CHECK(ConfigStore::parse("{\"cwPeakWidthHz\":9999,\"cwPeakBoostDb\":-4}", "test", back, error));
    CHECK_EQ(back.cwPeakWidthHz, 500.0);
    CHECK_EQ(back.cwPeakBoostDb, 12.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_cwpeak\n");
    engine_api_test::hermetic("cwpeak");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_cwpeak");
}
