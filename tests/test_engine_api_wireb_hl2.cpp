// test_engine_api_wireb_hl2.cpp - THE HERMES-LITE 2 PROFILE OF HPSDR PROTOCOL 1 AS A RADIO OF THE ENGINE (wire-b second pass, item 34): SELECT_SOURCE / RADIO_ADD "open-hpsdr1:<address>,profile=hl2". The peer is
// the HL2 STAND-IN RADIO of tests/test_hl2.cpp (its own `Radio`, included here under a renamed main): the extended discovery reply of board id 6, UDP on 127.0.0.1.
//
//   L1  the radio opens through the engine on the HL2 profile (the stand-in answers the extended discovery, sees the Start), and its 9 kHz tone is in the radio's spectrum
//   L2  RECEIVE ONLY, measured at the peer across the whole run: only the receive-side command addresses (general, TX NCO, RX1 NCO, drive, LNA), no MOX, drive 0, no payload, no bad frame, no
//       wide-band stream
//   L3  a retune reaches the radio (the stand-in decodes the receiver frequency)
//   L4  stopping the radio sends the peer a Stop; the radio is SAVED with profile=hl2 in its arguments, and a restart from the file alone puts it back on the stand-in
//   L5  a Hermes (board id 0x01) asked for on the HL2 profile is refused through the engine with the source's own sentence, and the radio stays on the generator
//
// UNVERIFIED on a real Hermes-Lite 2: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_hl2_model_main
#include "test_hl2.cpp"
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

bool toneIn(Rig& r, std::size_t radio) {
    for (int i = 0; i < 60; ++i) {
        r.pump(10);
        if (peakAt(r.spectrum(static_cast<std::uint32_t>(radio)), 9000.0, 6)) { return true; }
    }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_hl2\n");
    engine_api_test::hermetic("wirebhl2");
    Access::installHooks();
    Radio radio;   // board id 6: a Hermes-Lite 2
    CHECK(radio.start());
    const std::string typed = "127.0.0.1:" + std::to_string(portOf(radio)) + ",profile=hl2";
    AppConfig file;

    std::printf("L1: it opens on the HL2 profile and its tone is in the spectrum\n");
    {
        Rig r;
        CHECK_EQ(r.add(2, "open-hpsdr1:" + typed, true).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.source") == "hpsdr1"; }, 30000));
        CHECK(r.until([&] { return radio.seen().packets > 20 && !radio.seen().startBytes.empty(); }, 20000));
        CHECK(r.until([&] { return r.radio(2).sampleRateHz > 0.0; }, 10000));
        std::printf("  the radio runs at %.0f S/s, %d discovery request(s)\n", r.radio(2).sampleRateHz, radio.seen().discoveries);
        CHECK(radio.seen().discoveries >= 1);
        CHECK(toneIn(r, 2));

        std::printf("L2: receive only, measured at the peer\n");
        {
            const Radio::Seen sn = radio.seen();
            const std::set<int> allowed = {0, 1, 2, 9, 10};
            for (int a : sn.addresses) { CHECK(allowed.count(a) == 1); }
            for (int bad : {0x0B, 0x0E, 0x0F, 0x10, 0x12, 0x17, 0x2B, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3F}) { CHECK(sn.addresses.count(bad) == 0); }
            CHECK(sn.moxFrames == 0 && sn.drive == 0 && !sn.nonZeroPayload && sn.badFrames == 0);
            for (int b : sn.startBytes) { CHECK((b & 0x02) == 0); }   // never the wide-band stream
        }

        std::printf("L3: a retune reaches the radio\n");
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.1e6)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return radio.seen().rx1 == 7100000u; }, 10000));
        {
            const Radio::Seen sn = radio.seen();
            CHECK(sn.moxFrames == 0 && sn.drive == 0 && !sn.nonZeroPayload && sn.badFrames == 0);
        }
        r.pump(20);
        file = throughTheFile(r.eng);

        std::printf("L4: stop, and the saved radio\n");
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return !radio.seen().stopBytes.empty(); }, 10000));
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        if (file.radios.size() == 1) {
            CHECK_EQ(file.radios[0].sourceKind, std::string("hpsdr1"));
            CHECK(file.radios[0].args.find("profile=hl2") != std::string::npos);
            CHECK(file.radios[0].args.rfind("hpsdr1=127.0.0.1:", 0) == 0);
            std::printf("  saved: %s\n", file.radios[0].args.c_str());
        }
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    }
    {
        const int startsBefore = static_cast<int>(radio.seen().startBytes.size());
        Rig y(&file);   // a restart from the file alone (the saved radio was running when it was saved? it was stopped: it comes back stopped, on the same source)
        CHECK(y.until([&] { return y.fact("radio.2.source") == "hpsdr1"; }, 30000));
        CHECK_EQ(y.fact("radio.restore"), std::string("<absent>"));
        CHECK_EQ(y.send(y.aimed(FOXAPI_OP_RUN, 2, 0, 1)).status(), FOXAPI_OK);
        CHECK(y.until([&] { return static_cast<int>(radio.seen().startBytes.size()) > startsBefore; }, 20000));
        CHECK(toneIn(y, 2));
        const Radio::Seen sn = radio.seen();
        CHECK(sn.moxFrames == 0 && sn.drive == 0 && !sn.nonZeroPayload && sn.badFrames == 0);
    }
    radio.stop();

    std::printf("L5: a Hermes asked for as a Hermes-Lite 2\n");
    {
        Radio hermes(0x01);
        CHECK(hermes.start());
        Rig r;
        const auto o = r.add(2, "open-hpsdr1:127.0.0.1:" + std::to_string(portOf(hermes)) + ",profile=hl2", true);
        std::printf("  RADIO_ADD -> %d\n", o.status());
        r.until([&] { return r.fact("radio.2.refusal") != "<absent>" || r.fact("radio.2.source") != "siggen"; }, 30000);
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        const std::string why = r.fact("radio.2.refusal");
        std::printf("  refusal: %s\n", why.c_str());
        CHECK(why.find("not a Hermes-Lite 2") != std::string::npos);
        hermes.stop();
    }
    return check::finish("test_engine_api_wireb_hl2");
}
