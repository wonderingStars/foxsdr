// test_engine_api_nrmethod.cpp - THE NOISE-REDUCTION METHOD (engine wiring lane 1, group C; docs/multi-radio-records/engine-wiring-1.md).
//
// SET_NR_METHOD 0x0116 (standard / minimum-statistics spectral / adaptive line enhancer) through the real engine, plus the pipeline under it:
//   A  defaults and each radio's own choice; refusals past 2 (API and engine);
//   B  the pipeline runs what was chosen: with the noise reduction on, spectral is in use in any mode, the line enhancer only in CW and RAW (the standard one runs in its place
//      elsewhere), and nothing alternative runs while the noise reduction is off; the strength is SET_NR's for every method;
//   C  the choice is saved per radio and comes back in an in-process restart, in the engine and in its pipeline.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand method(std::uint32_t target, long long m) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_NR_METHOD, 0.0, m, nullptr);
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: defaults, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).nrMethod, 0u);
    CHECK_EQ(r.radio(2).nrMethodActive, 0u);
    CHECK_EQ(r.send(method(2, FOXAPI_NR_SPECTRAL)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).nrMethod == FOXAPI_NR_SPECTRAL; }, 5000));
    CHECK_EQ(r.radio(1).nrMethod, 0u);
    CHECK_EQ(r.send(method(1, FOXAPI_NR_LINE)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).nrMethod == FOXAPI_NR_LINE; }, 5000));
    CHECK_EQ(r.radio(2).nrMethod, static_cast<std::uint32_t>(FOXAPI_NR_SPECTRAL));
    CHECK_EQ(r.send(method(1, 3)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(method(1, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(method(9, 1)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.radio(1).nrMethod, static_cast<std::uint32_t>(FOXAPI_NR_LINE));
    CHECK_EQ(Access::pipeline(r.eng, 0).noiseReductionMethod(), 2);
    CHECK_EQ(Access::pipeline(r.eng, 1).noiseReductionMethod(), 1);
    const FoxCommandResult res = r.eng.applyCommand(method(1, 7), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
}

void testB() {
    std::printf("B: what the pipeline runs\n");
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0.0, FOXAPI_DEMOD_USB)).status(), FOXAPI_OK);
    r.pump(40);
    auto active = [&] { return r.radio(1).nrMethodActive; };
    CHECK_EQ(r.send(method(1, FOXAPI_NR_SPECTRAL)).status(), FOXAPI_OK);
    r.pump(40);
    CHECK_EQ(active(), 0u);   // the noise reduction is off: nothing alternative runs
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_NR, 1, 0.5, 1)).status(), FOXAPI_OK);   // on
    CHECK(r.until([&] { return active() == FOXAPI_NR_SPECTRAL; }, 15000));
    CHECK_EQ(r.send(method(1, FOXAPI_NR_LINE)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).nrMethod == FOXAPI_NR_LINE; }, 5000));
    r.pump(40);
    CHECK_EQ(active(), 0u);   // the line enhancer is for CW and carriers: the standard one runs in USB
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0.0, FOXAPI_DEMOD_CW)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return active() == FOXAPI_NR_LINE; }, 15000));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0.0, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return active() == 0u; }, 15000));
    CHECK_EQ(r.send(method(1, FOXAPI_NR_STANDARD)).status(), FOXAPI_OK);
    r.pump(40);
    CHECK_EQ(active(), 0u);
    CHECK_EQ(r.send(method(1, FOXAPI_NR_SPECTRAL)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return active() == FOXAPI_NR_SPECTRAL; }, 15000));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_NR, 1, 0.5, 0)).status(), FOXAPI_OK);   // off again
    CHECK(r.until([&] { return active() == 0u; }, 15000));
}

void testC() {
    std::printf("C: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(method(1, FOXAPI_NR_LINE)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(method(2, FOXAPI_NR_SPECTRAL)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.nrMethod, 2);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].nrMethod, 1);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).nrMethod, static_cast<std::uint32_t>(FOXAPI_NR_LINE));
        CHECK_EQ(b.radio(2).nrMethod, static_cast<std::uint32_t>(FOXAPI_NR_SPECTRAL));
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).noiseReductionMethod() == 2 && Access::pipeline(b.eng, 1).noiseReductionMethod() == 1; }, 5000));
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_nrmethod\n");
    engine_api_test::hermetic("nrmethod");
    Access::installHooks();
    testA();
    testB();
    testC();
    return check::finish("test_engine_api_nrmethod");
}
