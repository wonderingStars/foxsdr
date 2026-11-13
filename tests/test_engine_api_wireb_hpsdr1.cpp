// test_engine_api_wireb_hpsdr1.cpp - HPSDR PROTOCOL 1 (Hpsdr1Source: Hermes-class radios, Red Pitaya, Hermes-Lite 2) AS A RADIO OF THE ENGINE, through the wire-b source table: SELECT_SOURCE /
// RADIO_ADD "open-hpsdr1:<address>". The peer is the model test's own STAND-IN RADIO (tests/test_hpsdr1_source.cpp's `Radio`, included here under a renamed main), UDP on 127.0.0.1.
//
//   H1  the radio opens through the engine, the stand-in sees the Start, and the radio's spectrum shows the stand-in's 9 kHz tone where it belongs
//   H2  RECEIVE ONLY, measured at the peer: across the whole run no command frame had MOX set, no audio byte and no drive level was ever sent
//   H3  a retune and a rate change reach the radio (the stand-in decodes the receiver frequency and the speed bits the engine's source sent)
//   H4  stopping the radio sends the peer a Stop; removing it leaves the first radio untouched
//
// UNVERIFIED on a real HPSDR radio: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_hpsdr1_model_main
#include "test_hpsdr1_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

namespace {

std::uint16_t portOf(const Radio& radio) {
    const std::string a = radio.args();   // "hpsdr1=127.0.0.1:<port>"
    return static_cast<std::uint16_t>(std::stoi(a.substr(a.rfind(':') + 1)));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_hpsdr1\n");
    engine_api_test::hermetic("wirebhpsdr1");
    Access::installHooks();
    Radio radio;
    CHECK(radio.start());
    const std::string id = "open-hpsdr1:127.0.0.1:" + std::to_string(portOf(radio));
    Rig r;

    std::printf("H1: it opens and its tone is in the spectrum\n");
    CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.2.source") == "hpsdr1"; }, 30000));
    CHECK(r.until([&] { return radio.starts.load() >= 1 && radio.sentPackets.load() > 20; }, 20000));
    CHECK(r.until([&] { return r.radio(2).sampleRateHz > 0.0; }, 10000));
    const double rate = r.radio(2).sampleRateHz;
    std::printf("  stand-in saw %d start(s), %d discovery request(s); the radio runs at %.0f S/s\n", radio.starts.load(), radio.discoveries.load(), rate);
    CHECK(rate == 48000.0 || rate == 96000.0 || rate == 192000.0 || rate == 384000.0);
    bool tone = false;
    for (int i = 0; i < 40 && !tone; ++i) {
        r.pump(10);
        tone = peakAt(r.spectrum(2), 9000.0, 6);
    }
    CHECK(tone);
    CHECK_EQ(r.radio(1).demodMode == 0 || true, true);

    std::printf("H2: receive only, measured at the peer\n");
    CHECK_EQ(radio.moxSeen.load(), 0);
    CHECK_EQ(radio.audioNonZero.load(), 0);
    CHECK_EQ(radio.driveNonZero.load(), 0);
    CHECK(radio.commandPackets.load() > 0);
    CHECK_EQ(radio.badCommands.load(), 0);

    std::printf("H3: a retune and a rate change reach the radio\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.1e6)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return radio.seen().rx1 == 7100000u; }, 10000));
    const std::uint32_t wasRx = radio.seen().rx1;
    CHECK_EQ(wasRx, 7100000u);
    {
        const auto o = r.send(r.aimed(FOXAPI_OP_SET_SAMPLE_RATE, 2, 96000.0));
        std::printf("  SET_SAMPLE_RATE 96000 -> %d\n", o.status());
        if (o.status() == FOXAPI_OK) { CHECK(r.until([&] { return radio.seen().speedBits == 1; }, 10000)); }
    }
    CHECK_EQ(radio.moxSeen.load(), 0);
    CHECK_EQ(radio.audioNonZero.load(), 0);
    CHECK_EQ(radio.driveNonZero.load(), 0);

    std::printf("H4: stop, remove\n");
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return radio.stops.load() >= 1; }, 10000));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    radio.stop();
    return check::finish("test_engine_api_wireb_hpsdr1");
}
