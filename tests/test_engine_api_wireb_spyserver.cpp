// test_engine_api_wireb_spyserver.cpp - the SpyServer AS A RADIO OF THE ENGINE, through the wire-b source table: RADIO_ADD "open-spyserver:<address>". The peer is the model test's own STAND-IN
// (tests/test_spyserver_source.cpp: Server, included here under a renamed main), on 127.0.0.1.
//
//   1  the radio opens through the engine and the stand-in sees it (its hello arrived); its 7 kHz tone is in the spectrum
//   2  a retune reaches the server as a setting
//   3  stopping the radio and removing it leaves the first radio untouched
//
// UNVERIFIED on the real the SpyServer: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_spyserver_model_main
#include "test_spyserver_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_spyserver\n");
    engine_api_test::hermetic("wirebspyserver");
    Access::installHooks();
    Server srv{Spec{}};
    CHECK(srv.start());
    const std::string id = "open-spyserver:" + srv.args();
    Rig r;

    std::printf("1: it opens and the stand-in sees it\n");
    CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "spyserver"; }, 30000));
    CHECK(r.until([&] { return srv.hellos.load() >= 1 && r.radio(2).sampleRateHz > 0.0; }, 20000));
    bool tone = false;
    for (int i = 0; i < 60 && !tone; ++i) {
        r.pump(10);
        tone = peakAt(r.spectrum(2), 7000.0, 10);
    }
    CHECK(tone);

    std::printf("2: a retune\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 100.0e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return srv.sets().size() >= 2; }, 15000));

    std::printf("3: stop, remove\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    srv.stop();
    return check::finish("test_engine_api_wireb_spyserver");
}
