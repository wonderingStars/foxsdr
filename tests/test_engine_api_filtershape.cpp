// test_engine_api_filtershape.cpp - THE CHANNEL FILTER SHAPE THROUGH THE ENGINE (engine serial lane A, row F6): SET_FILTER_SHAPE 0x0123, per radio, 0 default / 1 sharp / 2 medium / 3 soft.
//   A  default, each radio its own, the pipeline follows, filterShapeActive follows the bandwidth (a narrow channel has the stage, 150 kHz does not), refusals change nothing;  B  saved per radio and restored.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand shape(std::uint32_t target, std::int64_t s) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_FILTER_SHAPE, 0.0, s, nullptr);
    c.target = target;
    return c;
}
FoxCommand mode(std::uint32_t target, std::int64_t m) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_MODE, 0.0, m, nullptr);
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: default, per radio, active, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).filterShape, 0u);
    CHECK_EQ(r.radio(1).filterShapeActive, 0u);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);       // 12.5 kHz: a narrow channel
    CHECK_EQ(r.send(shape(1, 2)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).filterShape == 2u && r.radio(1).filterShapeActive == 1u; }, 5000));
    CHECK_EQ(Access::pipeline(r.eng, 0).channelShape(), 2);
    CHECK_EQ(r.radio(2).filterShape, 0u);                                   // the other radio is not touched
    CHECK_EQ(Access::pipeline(r.eng, 1).channelShape(), 0);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);       // 150 kHz: no narrow stage, the shape stays chosen and does not apply
    CHECK(r.until([&] { return r.radio(1).filterShapeActive == 0u; }, 5000));
    CHECK_EQ(r.radio(1).filterShape, 2u);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).filterShapeActive == 1u; }, 5000));
    for (const std::int64_t bad : {-1, 4, 99}) { CHECK_EQ(r.send(shape(1, bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(shape(9, 1)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(shape(1, 7), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).filterShape, 2u);
    CHECK_EQ(r.send(shape(1, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).filterShape == 0u && r.radio(1).filterShapeActive == 0u; }, 5000));
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(shape(1, 1)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(shape(2, 3)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.filterShape, 1);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].filterShape, 3);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).filterShape, 1u);
        CHECK_EQ(b.radio(2).filterShape, 3u);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).channelShape() == 1 && Access::pipeline(b.eng, 1).channelShape() == 3; }, 5000));
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"filterShape\":9}", "test", back, error));
    CHECK_EQ(back.filterShape, 0);
    CHECK(ConfigStore::parse("{\"filterShape\":2}", "test", back, error));
    CHECK_EQ(back.filterShape, 2);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_filtershape\n");
    engine_api_test::hermetic("filtershape");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_filtershape");
}
