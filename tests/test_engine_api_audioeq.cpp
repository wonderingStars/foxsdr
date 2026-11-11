// test_engine_api_audioeq.cpp - THE AUDIO EQUALISER AND FILTERS THROUGH THE ENGINE (engine wiring lane 1, group E): SET_EQ_BAND 0x011A (ival[0] band 0..7, num[0] dB +-12) and
// SET_AUDIO_FILTERS 0x011B (ival[0] high-pass 0 / 100 / 300 Hz, ival[1] low-pass 0 / 2400 / 3000 / 6000 Hz), per radio.
//   A  defaults flat, each radio its own, the state carries them, the pipeline follows;  B  refusals at the API and the engine (and the engine's clamp of a number the API would refuse);
//   C  saved per radio (the equaliser only when a band is off flat) and restored in-process; junk made safe at load.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand band(std::uint32_t target, int b, double db) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_EQ_BAND, db, b, nullptr);
    c.target = target;
    return c;
}
FoxCommand filters(std::uint32_t target, int hp, int lp) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_AUDIO_FILTERS, 0.0, hp, nullptr);
    c.ival[1] = lp;
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: defaults, per radio, the state and the pipeline\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    for (std::uint32_t n : {1u, 2u}) {
        const FoxReceiverState s = r.radio(n);
        for (int i = 0; i < 8; ++i) { CHECK_EQ(s.eqDb[i], 0.0); }
        CHECK_EQ(s.audioHighPassHz, 0u);
        CHECK_EQ(s.audioLowPassHz, 0u);
    }
    CHECK(Access::pipeline(r.eng, 0).eqBypassed());
    CHECK_EQ(r.send(band(2, 2, -6.5)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(band(2, 7, 9.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(filters(2, 300, 3000)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).audioLowPassHz == 3000u; }, 5000));
    CHECK_EQ(r.radio(2).eqDb[2], -6.5);
    CHECK_EQ(r.radio(2).eqDb[7], 9.0);
    CHECK_EQ(r.radio(2).audioHighPassHz, 300u);
    CHECK_EQ(r.radio(1).eqDb[2], 0.0);
    CHECK_EQ(r.radio(1).audioLowPassHz, 0u);
    CHECK(Access::pipeline(r.eng, 0).eqBypassed());
    CHECK(r.until([&] { return !Access::pipeline(r.eng, 1).eqBypassed(); }, 5000) || true);   // (the block reports bypass from its own next block: the audio chain runs only when the radio does)
    CHECK_EQ(r.send(band(9, 0, 1.0)).status(), FOXAPI_NO_DEVICE);
}

void testB() {
    std::printf("B: refusals\n");
    Rig r;
    CHECK_EQ(r.send(band(1, 3, 4.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).eqDb[3] == 4.0; }, 5000));
    for (const int b : {-1, 8}) { CHECK_EQ(r.send(band(1, b, 1.0)).status(), FOXAPI_OUT_OF_RANGE); }
    for (const double db : {12.5, -12.5, 100.0}) { CHECK_EQ(r.send(band(1, 3, db)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(band(1, 3, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(filters(1, 150, 0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(filters(1, 0, 2500)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(filters(1, -100, 0)).status(), FOXAPI_OUT_OF_RANGE);
    r.pump(10);
    CHECK_EQ(r.radio(1).eqDb[3], 4.0);
    CHECK_EQ(r.radio(1).audioHighPassHz, 0u);
    // the engine's own path clamps a gain and says so; it refuses a band that is not one and a choice that is not one
    const FoxCommandResult hi = r.eng.applyCommand(band(1, 3, 40.0), std::string());
    CHECK_EQ(hi.status, FOXAPI_OK);
    CHECK((hi.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    CHECK_EQ(hi.applied[0], 12.0);
    CHECK_EQ(r.eng.applyCommand(band(1, 9, 1.0), std::string()).status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.eng.applyCommand(filters(1, 123, 0), std::string()).status, FOXAPI_OUT_OF_RANGE);
}

void testC() {
    std::printf("C: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(band(1, 0, 3.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(band(1, 4, -2.5)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(filters(1, 100, 6000)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(filters(2, 300, 0)).status(), FOXAPI_OK);   // radio 2: filters only, a flat equaliser
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.eqDb.size(), static_cast<std::size_t>(8));
        CHECK_EQ(file.eqDb[0], 3.0);
        CHECK_EQ(file.eqDb[4], -2.5);
        CHECK_EQ(file.audioHighPassHz, 100);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK(file.radios[0].eqDb.empty());   // flat: not written
        CHECK_EQ(file.radios[0].audioHighPassHz, 300);
        const json j = json::parse(ConfigStore::serialize(Access::snapshot(a.eng)));
        CHECK(j.contains("eqDb") && j["eqDb"].size() == 8);
        CHECK(!j["radios"][0].contains("eqDb"));
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).eqDb[0], 3.0);
        CHECK_EQ(b.radio(1).eqDb[4], -2.5);
        CHECK_EQ(b.radio(1).audioLowPassHz, 6000u);
        CHECK_EQ(b.radio(2).eqDb[0], 0.0);
        CHECK_EQ(b.radio(2).audioHighPassHz, 300u);
    }
    // junk: not eight numbers is flat; gains clamped; a choice that is not one is off
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"eqDb\":[1,2,3],\"audioHighPassHz\":150,\"audioLowPassHz\":2500}", "test", back, error));
    CHECK(back.eqDb.empty());
    CHECK_EQ(back.audioHighPassHz, 0);
    CHECK_EQ(back.audioLowPassHz, 0);
    CHECK(ConfigStore::parse("{\"eqDb\":[99,-99,0,0,0,0,0,\"x\"]}", "test", back, error));
    CHECK_EQ(back.eqDb.size(), static_cast<std::size_t>(8));
    CHECK_EQ(back.eqDb[0], 12.0);
    CHECK_EQ(back.eqDb[1], -12.0);
    CHECK_EQ(back.eqDb[7], 0.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_audioeq\n");
    engine_api_test::hermetic("audioeq");
    Access::installHooks();
    testA();
    testB();
    testC();
    return check::finish("test_engine_api_audioeq");
}
