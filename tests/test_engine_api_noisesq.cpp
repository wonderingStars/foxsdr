// test_engine_api_noisesq.cpp - THE FM NOISE SQUELCH THROUGH THE ENGINE (engine wiring lane 1, group C): SET_NOISE_SQUELCH 0x0119 (ival[0] on; with ival[1] = 1 num[0] the threshold dB
// re full scale -90..0 and num[1] the tail ms 0..2000), per radio, NFM only.
//   A  defaults, each radio its own, refusals at the API and the engine, the pipeline follows, the measurements come back in the state;  B  saved per radio, restored in-process.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand nsq(std::uint32_t target, bool on, bool values, double thr = 0.0, double tail = 0.0) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_NOISE_SQUELCH, thr, on ? 1 : 0, nullptr);
    c.ival[1] = values ? 1 : 0;
    c.num[1] = tail;
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: defaults, per radio, refusals, measurements\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    for (std::uint32_t n : {1u, 2u}) {
        const FoxReceiverState s = r.radio(n);
        CHECK_EQ(s.noiseSquelchOn, 0u);
        CHECK_EQ(s.noiseSquelchThresholdDb, -30.0);
        CHECK_EQ(s.noiseSquelchTailMs, 100.0);
    }
    CHECK_EQ(r.send(nsq(2, true, true, -45.5, 250.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).noiseSquelchOn == 1u; }, 5000));
    CHECK_EQ(r.radio(2).noiseSquelchThresholdDb, -45.5);
    CHECK_EQ(r.radio(2).noiseSquelchTailMs, 250.0);
    CHECK_EQ(r.radio(1).noiseSquelchOn, 0u);
    CHECK_EQ(r.radio(1).noiseSquelchThresholdDb, -30.0);
    CHECK(Access::pipeline(r.eng, 1).noiseSquelchOn());
    CHECK(!Access::pipeline(r.eng, 0).noiseSquelchOn());
    // off alone keeps the values
    CHECK_EQ(r.send(nsq(2, false, false)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).noiseSquelchOn == 0u; }, 5000));
    CHECK_EQ(r.radio(2).noiseSquelchThresholdDb, -45.5);
    // refusals
    CHECK_EQ(r.send(nsq(1, true, true, 1.0, 100.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(nsq(1, true, true, -91.0, 100.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(nsq(1, true, true, -30.0, -1.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(nsq(1, true, true, -30.0, 2001.0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(nsq(9, true, false)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.radio(1).noiseSquelchOn, 0u);
    const FoxCommandResult res = r.eng.applyCommand(nsq(1, true, true, 50.0, 100.0), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).noiseSquelchThresholdDb, -30.0);
    // on a generator radio in NFM (no noise in the discriminator band of a clean tone): the measurement is in the state, and the gate is open
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0.0, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(nsq(1, true, true, -30.0, 100.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return Access::pipeline(r.eng, 0).noiseSquelchOn() && r.radio(1).noiseSquelchLevelDb > -200.0; }, 15000));
    std::printf("  generator radio in NFM: noise level %.1f dB, gate %u\n", r.radio(1).noiseSquelchLevelDb, r.radio(1).noiseSquelchOpen);
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(nsq(1, true, true, -50.0, 40.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(nsq(2, false, true, -20.0, 500.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK(file.noiseSquelchOn);
        CHECK_EQ(file.noiseSquelchThresholdDb, -50.0);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK(!file.radios[0].noiseSquelchOn);
        CHECK_EQ(file.radios[0].noiseSquelchTailMs, 500.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).noiseSquelchOn, 1u);
        CHECK_EQ(b.radio(1).noiseSquelchThresholdDb, -50.0);
        CHECK_EQ(b.radio(2).noiseSquelchOn, 0u);
        CHECK_EQ(b.radio(2).noiseSquelchThresholdDb, -20.0);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).noiseSquelchOn() && !Access::pipeline(b.eng, 1).noiseSquelchOn(); }, 5000));
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"noiseSquelchThresholdDb\":50,\"noiseSquelchTailMs\":99999}", "test", back, error));
    CHECK_EQ(back.noiseSquelchThresholdDb, 0.0);
    CHECK_EQ(back.noiseSquelchTailMs, 2000.0);
    CHECK(ConfigStore::parse("{\"noiseSquelchThresholdDb\":-500,\"noiseSquelchTailMs\":-4}", "test", back, error));
    CHECK_EQ(back.noiseSquelchThresholdDb, -90.0);
    CHECK_EQ(back.noiseSquelchTailMs, 0.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_noisesq\n");
    engine_api_test::hermetic("noisesq");
    Access::installHooks();
    testA();
    testB();
    return check::finish("test_engine_api_noisesq");
}
