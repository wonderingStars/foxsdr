// test_engine_api_wireb_hpsdr2.cpp - HPSDR PROTOCOL 2 (Hpsdr2Source: Orion, Angelia, Hermes boards that speak it) AS A RADIO OF THE ENGINE, through the wire-b source table: RADIO_ADD
// "open-hpsdr2:<address>,<options>". The peer is the model test's STAND-IN RADIO (tests/test_hpsdr2.cpp's `P2Radio`, included under a renamed main), UDP on 127.0.0.1.
//
//   P1  the radio opens through the engine; with the centre 10 kHz under the stand-in's signal the tone is in the radio's spectrum 10 kHz up
//   P2  RECEIVE ONLY, measured at the peer: no PTT, no drive level, no unexpected byte, across the whole run
//   P3  a retune reaches the radio (the stand-in decodes the frequency word of the High Priority packets the engine's source sends)
//   P4  stopping the radio clears the peer's run bit; removing it leaves the first radio untouched
//
// UNVERIFIED on a real HPSDR protocol-2 radio: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_hpsdr2_model_main
#include "test_hpsdr2.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

// The stand-in keeps the frequency WORD the High Priority packets carry (a phase word: 2^32 f / the DSP clock of 122.88 MHz, which is what the reply says this board uses).
double tunedHz(P2Radio& radio) { return static_cast<double>(radio.word.load()) * 122880000.0 / 4294967296.0; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_hpsdr2\n");
    engine_api_test::hermetic("wirebhpsdr2");
    Access::installHooks();
    P2Radio radio;
    CHECK(radio.start());
    const std::string id = "open-hpsdr2:" + radio.args("timeout=3000");
    Rig r;

    std::printf("P1: it opens and its tone is in the spectrum\n");
    CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "hpsdr2"; }, 30000));
    CHECK(r.until([&] { return radio.run.load() == 1 && radio.sentPackets.load() > 10; }, 20000));
    std::printf("  stand-in saw %d discovery request(s), %d general, %d specific, %d high-priority packet(s)\n", radio.discoveries.load(), radio.generals.load(), radio.specifics.load(), radio.hps.load());
    CHECK(radio.discoveries.load() >= 1);
    CHECK(radio.generals.load() >= 1 && radio.specifics.load() >= 1 && radio.hps.load() >= 1);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, radio.rfHz.load() - 10000.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return std::fabs(tunedHz(radio) - 7064200.0) < 3.0; }, 15000));
    bool tone = false;
    for (int i = 0; i < 60 && !tone; ++i) {
        r.pump(10);
        tone = peakAt(r.spectrum(2), 10000.0, 6);
    }
    CHECK(tone);

    std::printf("P2: receive only, measured at the peer\n");
    CHECK_EQ(radio.pttSeen.load(), 0);
    CHECK_EQ(radio.driveSeen.load(), 0);
    CHECK_EQ(radio.unexpectedBytes.load(), 0);

    std::printf("P3: a retune reaches the radio\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.2e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return std::fabs(tunedHz(radio) - 7200000.0) < 3.0; }, 15000));
    CHECK_EQ(radio.pttSeen.load(), 0);
    CHECK_EQ(radio.driveSeen.load(), 0);

    std::printf("P4: stop, remove\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return radio.run.load() == 0; }, 10000));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    radio.stop();
    return check::finish("test_engine_api_wireb_hpsdr2");
}
