// test_engine_api_stereoplace.cpp - STEREO PLACEMENT THROUGH THE ENGINE (engine serial lane A, rows F20 and D5): SET_STEREO_PLACE 0x0120, per radio: 0 off, 1 pan (-1..1), 2 pseudo-stereo (0..1), 3 binaural (-90..90).
//   A  default, each radio its own, refusals change nothing, the pipeline follows;  B  saved per radio and restored in an in-process restart, junk in a file lands on off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand place(std::uint32_t target, std::int64_t mode, double value) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_STEREO_PLACE, value, mode, nullptr);
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).stereoPlaceMode, 0u);
    CHECK_EQ(r.radio(1).stereoPlaceValue, 0.0);
    CHECK_EQ(r.send(place(2, 1, -0.5)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).stereoPlaceMode == 1u && r.radio(2).stereoPlaceValue == -0.5; }, 5000));
    CHECK_EQ(r.radio(1).stereoPlaceMode, 0u);
    CHECK_EQ(Access::pipeline(r.eng, 1).stereoPlaceMode(), 1);
    CHECK_EQ(Access::pipeline(r.eng, 0).stereoPlaceMode(), 0);
    CHECK_EQ(r.send(place(1, 3, 45.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).stereoPlaceMode == 3u && r.radio(1).stereoPlaceValue == 45.0; }, 5000));
    CHECK_EQ(r.send(place(1, 2, 0.7)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).stereoPlaceMode == 2u; }, 5000));
    struct Bad { std::int64_t mode; double value; };
    for (const Bad b : {Bad{4, 0.0}, Bad{-1, 0.0}, Bad{1, 1.5}, Bad{1, -1.5}, Bad{2, -0.1}, Bad{2, 1.1}, Bad{3, 91.0}, Bad{3, -91.0}}) { CHECK_EQ(r.send(place(1, b.mode, b.value)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(place(1, 1, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(place(9, 1, 0.0)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(place(1, 1, 9.0), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).stereoPlaceMode, 2u);
    CHECK_EQ(Access::pipeline(r.eng, 0).stereoPlaceMode(), 2);
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(place(1, 1, 0.25)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(place(2, 3, -30.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.stereoPlaceMode, 1);
        CHECK_EQ(file.stereoPlaceValue, 0.25);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].stereoPlaceMode, 3);
        CHECK_EQ(file.radios[0].stereoPlaceValue, -30.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).stereoPlaceValue, 0.25);
        CHECK_EQ(b.radio(2).stereoPlaceMode, 3u);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).stereoPlaceMode() == 1 && Access::pipeline(b.eng, 1).stereoPlaceValue() == -30.0; }, 5000));
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"stereoPlaceMode\":9,\"stereoPlaceValue\":5}", "test", back, error));
    CHECK_EQ(back.stereoPlaceMode, 0);
    CHECK(ConfigStore::parse("{\"stereoPlaceMode\":1,\"stereoPlaceValue\":5}", "test", back, error));
    CHECK_EQ(back.stereoPlaceMode, 1);
    CHECK_EQ(back.stereoPlaceValue, 0.0);   // a pan of 5 is not a pan
    CHECK(ConfigStore::parse("{\"stereoPlaceMode\":3,\"stereoPlaceValue\":60}", "test", back, error));
    CHECK_EQ(back.stereoPlaceValue, 60.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_stereoplace\n");
    engine_api_test::hermetic("stereoplace");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_stereoplace");
}
