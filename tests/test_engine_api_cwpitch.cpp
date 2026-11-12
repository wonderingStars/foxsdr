// test_engine_api_cwpitch.cpp - THE CW PITCH THROUGH THE ENGINE (engine wiring lane 1, group C): SET_CW_PITCH 0x0118, per radio, 300..1200 Hz, default 700.
//   A  default, each radio its own, refusals at the API and the engine change nothing, the pipeline follows;  B  saved per radio and restored in an in-process restart.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand pitch(std::uint32_t target, double hz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_CW_PITCH, hz, 0, nullptr);
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).cwPitchHz, 700.0);
    CHECK_EQ(r.radio(2).cwPitchHz, 700.0);
    CHECK_EQ(r.send(pitch(2, 550.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).cwPitchHz == 550.0; }, 5000));
    CHECK_EQ(r.radio(1).cwPitchHz, 700.0);
    CHECK_EQ(r.send(pitch(1, 1100.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).cwPitchHz == 1100.0; }, 5000));
    CHECK_EQ(r.radio(2).cwPitchHz, 550.0);
    CHECK_EQ(Access::pipeline(r.eng, 0).cwPitchHz(), 1100.0);
    CHECK_EQ(Access::pipeline(r.eng, 1).cwPitchHz(), 550.0);
    for (const double bad : {299.9, 1200.1, 0.0, -5.0}) { CHECK_EQ(r.send(pitch(1, bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(pitch(1, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(pitch(9, 700.0)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(pitch(1, 5000.0), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).cwPitchHz, 1100.0);
    CHECK_EQ(Access::pipeline(r.eng, 0).cwPitchHz(), 1100.0);
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(pitch(1, 600.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(pitch(2, 800.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.cwPitchHz, 600.0);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].cwPitchHz, 800.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).cwPitchHz, 600.0);
        CHECK_EQ(b.radio(2).cwPitchHz, 800.0);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).cwPitchHz() == 600.0 && Access::pipeline(b.eng, 1).cwPitchHz() == 800.0; }, 5000));
    }
    // a junk value in a file lands in range
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"cwPitchHz\":5000}", "test", back, error));
    CHECK_EQ(back.cwPitchHz, 1200.0);
    CHECK(ConfigStore::parse("{\"cwPitchHz\":10}", "test", back, error));
    CHECK_EQ(back.cwPitchHz, 700.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_cwpitch\n");
    engine_api_test::hermetic("cwpitch");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_cwpitch");
}
