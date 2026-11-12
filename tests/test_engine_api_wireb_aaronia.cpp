// test_engine_api_wireb_aaronia.cpp - the Aaronia RTSA server AS A RADIO OF THE ENGINE, through the wire-b source table: RADIO_ADD "open-aaronia:<address>". The peer is the model test's own STAND-IN
// (tests/test_aaronia_source.cpp: Server, included here under a renamed main), on 127.0.0.1.
//
//   1  the radio opens through the engine and the stand-in sees it (the stream was requested); its 7 kHz tone is in the spectrum
//   2  a retune is accepted by the radio (the server takes its tuning through the stream request)
//   3  stopping the radio and removing it leaves the first radio untouched
//
// UNVERIFIED on the real the Aaronia RTSA server: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_aaronia_model_main
#include "test_aaronia_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_aaronia\n");
    engine_api_test::hermetic("wirebaaronia");
    Access::installHooks();
    Server srv{Spec{}};
    CHECK(srv.start());
    const std::string id = "open-aaronia:" + srv.args();
    Rig r;

    std::printf("1: it opens and the stand-in sees it\n");
    CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "aaronia"; }, 30000));
    CHECK(r.until([&] { return srv.streamRequests.load() >= 1 && r.radio(2).sampleRateHz > 0.0; }, 20000));
    bool tone = false;
    for (int i = 0; i < 60 && !tone; ++i) {
        r.pump(10);
        tone = peakAt(r.spectrum(2), 7000.0, 10);
    }
    CHECK(tone);

    std::printf("2: a retune\n");
    CHECK(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 1.1e9)).status() == FOXAPI_OK || r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 1.1e9)).status() == FOXAPI_NO_CHANGE);

    std::printf("3: stop, remove\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    srv.stop();
    return check::finish("test_engine_api_wireb_aaronia");
}
