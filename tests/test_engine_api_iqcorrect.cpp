// test_engine_api_iqcorrect.cpp - THE I/Q CORRECTION THROUGH THE ENGINE (engine serial lane A, row F16): SET_IQ_CORRECT 0x011D, per radio, a sum of 1 DC removal, 2 balance, 4 swap I/Q; default 0.
//   A  default, each radio its own, refusals change nothing, the pipeline follows;  B  saved per radio and restored in an in-process restart, junk in a file lands on 0.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand iq(std::uint32_t target, std::int64_t mask) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_IQ_CORRECT, 0.0, mask, nullptr);
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).iqCorrectMask, 0u);
    CHECK_EQ(r.radio(2).iqCorrectMask, 0u);
    CHECK_EQ(r.radio(1).iqImageRejectionDb, 200.0);   // nothing measured
    CHECK_EQ(r.send(iq(2, 5)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).iqCorrectMask == 5u; }, 5000));
    CHECK_EQ(r.radio(1).iqCorrectMask, 0u);
    CHECK_EQ(Access::pipeline(r.eng, 1).iqCorrection(), 5);
    CHECK_EQ(Access::pipeline(r.eng, 0).iqCorrection(), 0);
    CHECK_EQ(r.send(iq(1, 7)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).iqCorrectMask == 7u; }, 5000));
    for (const std::int64_t bad : {-1, 8, 255}) { CHECK_EQ(r.send(iq(1, bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(iq(9, 1)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(iq(1, 99), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).iqCorrectMask, 7u);
    CHECK_EQ(Access::pipeline(r.eng, 0).iqCorrection(), 7);
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(iq(1, 3)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(iq(2, 4)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.iqCorrect, 3);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].iqCorrect, 4);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).iqCorrectMask, 3u);
        CHECK_EQ(b.radio(2).iqCorrectMask, 4u);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).iqCorrection() == 3 && Access::pipeline(b.eng, 1).iqCorrection() == 4; }, 5000));
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"iqCorrect\":99}", "test", back, error));
    CHECK_EQ(back.iqCorrect, 0);
    CHECK(ConfigStore::parse("{\"iqCorrect\":-3}", "test", back, error));
    CHECK_EQ(back.iqCorrect, 0);
    CHECK(ConfigStore::parse("{\"iqCorrect\":6}", "test", back, error));
    CHECK_EQ(back.iqCorrect, 6);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_iqcorrect\n");
    engine_api_test::hermetic("iqcorrect");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_iqcorrect");
}
