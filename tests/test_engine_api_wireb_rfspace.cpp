// test_engine_api_wireb_rfspace.cpp - RFspace NetSDR / SDR-IP / Afedri (AscpSource) AS A RADIO OF THE ENGINE, through the wire-b source table: RADIO_ADD "open-rfspace:<address>". The
// peer is the model test's STAND-IN RADIO (tests/test_ascp_source.cpp's `Radio`, included under a renamed main): ASCP over TCP and the UDP I/Q stream on 127.0.0.1.
//
//   A1  the radio opens through the engine, the stand-in sees the receiver switched ON, and its 12.3 kHz tone is in the radio's spectrum
//   A2  a retune reaches the radio (the stand-in decodes the frequency item the engine's source sent)
//   A3  stopping the radio switches the peer's receiver OFF; removing it leaves the first radio untouched
//
// UNVERIFIED on a real NetSDR / SDR-IP / Afedri: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_ascp_model_main
#include "test_ascp_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_rfspace\n");
    engine_api_test::hermetic("wirebrfspace");
    Access::installHooks();
    Radio radio{RadioSpec{}};
    CHECK(radio.start());
    const std::string id = "open-rfspace:" + radio.args();
    Rig r;

    std::printf("A1: it opens and its tone is in the spectrum\n");
    CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "rfspace"; }, 30000));
    CHECK(r.until([&] { return radio.receiverOn() && radio.sent.load() > 20; }, 20000));
    std::printf("  stand-in: %d receiver-on, %d keepalive(s), rate %d\n", radio.onCount.load(), radio.keepalives.load(), radio.rate.load());
    CHECK(radio.onCount.load() >= 1);
    bool tone = false;
    for (int i = 0; i < 60 && !tone; ++i) {
        r.pump(10);
        tone = peakAt(r.spectrum(2), 12345.0, 8);
    }
    CHECK(tone);

    std::printf("A2: a retune reaches the radio\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.1e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return radio.freq.load() == 7100000ull; }, 15000));

    std::printf("A3: stop, remove\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !radio.receiverOn() && radio.offCount.load() >= 1; }, 10000));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    radio.stop();
    return check::finish("test_engine_api_wireb_rfspace");
}
