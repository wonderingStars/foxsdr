// test_engine_api_afc.cpp - AUTOMATIC FREQUENCY CONTROL THROUGH THE ENGINE (engine serial lane A, row F13): SET_AFC 0x011E, per radio, a switch and a pull-in range of 50 to 20000 Hz (default 2500).
//   A  default, each radio its own, refusals change nothing, the pipeline follows;  B  saved per radio and restored in an in-process restart, junk in a file lands in range.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand afc(std::uint32_t target, std::int64_t on, double pullIn, bool withRange = true) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_AFC, pullIn, on, nullptr);
    c.target = target;
    c.ival[1] = withRange ? 1 : 0;
    c.num[0] = pullIn;
    return c;
}

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).afcOn, 0u);
    CHECK_EQ(r.radio(1).afcPullInHz, 2500.0);
    CHECK_EQ(r.radio(1).afcCorrectionHz, 0.0);
    CHECK_EQ(r.send(afc(2, 1, 4000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).afcOn == 1u && r.radio(2).afcPullInHz == 4000.0; }, 5000));
    CHECK_EQ(r.radio(1).afcOn, 0u);
    CHECK(Access::pipeline(r.eng, 1).afcOn());
    CHECK(!Access::pipeline(r.eng, 0).afcOn());
    CHECK_EQ(Access::pipeline(r.eng, 1).afcPullInHz(), 4000.0);
    // a switch without a range keeps the range
    CHECK_EQ(r.send(afc(2, 0, 0.0, false)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).afcOn == 0u; }, 5000));
    CHECK_EQ(r.radio(2).afcPullInHz, 4000.0);
    for (const double bad : {0.0, 49.9, 20000.1, -1.0}) { CHECK_EQ(r.send(afc(1, 1, bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(afc(1, 1, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(afc(9, 1, 1000.0)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(afc(1, 1, 99999.0), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).afcOn, 0u);
    CHECK(!Access::pipeline(r.eng, 0).afcOn());
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(afc(1, 1, 1500.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(afc(2, 1, 6000.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK(file.afcOn);
        CHECK_EQ(file.afcPullInHz, 1500.0);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK(file.radios[0].afcOn);
        CHECK_EQ(file.radios[0].afcPullInHz, 6000.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).afcPullInHz, 1500.0);
        CHECK_EQ(b.radio(2).afcPullInHz, 6000.0);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).afcOn() && Access::pipeline(b.eng, 1).afcOn() && Access::pipeline(b.eng, 1).afcPullInHz() == 6000.0; }, 5000));
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"afcPullInHz\":5}", "test", back, error));
    CHECK_EQ(back.afcPullInHz, 2500.0);
    CHECK(ConfigStore::parse("{\"afcPullInHz\":99999}", "test", back, error));
    CHECK_EQ(back.afcPullInHz, 20000.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_afc\n");
    engine_api_test::hermetic("afc");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_afc");
}
