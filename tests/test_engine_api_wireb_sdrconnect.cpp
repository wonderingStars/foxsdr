// test_engine_api_wireb_sdrconnect.cpp - SDRconnect AS A RADIO OF THE ENGINE, through the wire-b source table: RADIO_ADD "open-sdrconnect:<address>". The peer is the model test's own STAND-IN
// (tests/test_sdrconnect_source.cpp: FakeSdrConnect, included here under a renamed main), on 127.0.0.1.
//
//   1  the radio opens through the engine and the stand-in sees it (its WebSocket events arrived); a clear tone is in the spectrum
//   2  a retune is accepted
//   3  stopping the radio and removing it leaves the first radio untouched
//
// UNVERIFIED on the real SDRconnect: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_sdrconnect_model_main
#include "test_sdrconnect_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_sdrconnect\n");
    engine_api_test::hermetic("wirebsdrconnect");
    Access::installHooks();
    FakeSdrConnect fake;
    CHECK(fake.start());
    const std::string id = "open-sdrconnect:" + fake.args();
    Rig r;

    std::printf("1: it opens and the stand-in sees it\n");
    CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "sdrconnect"; }, 30000));
    CHECK(r.until([&] { return !fake.events().empty() && r.radio(2).sampleRateHz > 0.0; }, 20000));
    bool tone = false;
    for (int i = 0; i < 60 && !tone; ++i) {
        r.pump(10);
        tone = standout(r.spectrum(2)) > 15.0f;
    }
    CHECK(tone);

    std::printf("2: a retune\n");
    const auto rt = r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 100.0e6));
    CHECK(rt.status() == FOXAPI_OK || rt.status() == FOXAPI_NO_CHANGE);

    std::printf("3: stop, remove\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    fake.stop();
    return check::finish("test_engine_api_wireb_sdrconnect");
}
