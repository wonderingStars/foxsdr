// test_engine_api_wireb_funcube.cpp - THE FUNcube DONGLE PRO+ AS A RADIO OF THE ENGINE, SUCCESS PATH (wire-b second pass, item 34): RADIO_ADD "open-funcube:" through the wire-b source table.
// The "dongle" is the model test's STAND-IN (tests/test_funcube.cpp's Dongle behind a fake HID link, and its FakeAudio: a 12 kHz tone at 192 kS/s on a fake sound input named like the dongle's),
// handed to the engine's own device-open worker through Engine::testHooks_.makeDevice. No HID device and no sound card is touched.
//
//   F1  the radio opens through the engine and its 12 kHz tone is in the radio's spectrum (the engine runs the dongle at its fixed 192 kS/s)
//   F2  RECEIVE ONLY, measured at the peer: the HID commands sent are the documented query / tune / gain / bias ones; never the reset-to-bootloader command
//   F3  a retune reaches the dongle (the stand-in decodes the frequency report); removing the radio closes the HID link
//   F4  the radio is SAVED as kind funcube with the arguments the open normalised to (index=0, the source's own default), and a restart from the file alone opens it again
//   F5  no dongle plugged in: refused through the engine with the source's sentence, the radio stays on the generator
//
// UNVERIFIED on a real FUNcube Dongle: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_funcube_model_main
#define Rig FuncubeModelRig
#include "test_funcube.cpp"
#undef Rig
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

namespace {

std::unique_ptr<FuncubeModelRig> g_rig;

std::unique_ptr<cascade::source::DeviceSource> makeStandIn(const std::string& kind) {
    if (kind != "funcube" || !g_rig) { return nullptr; }
    return std::move(g_rig->src);
}

bool toneIn(Rig& r, std::size_t radio) {
    for (int i = 0; i < 80; ++i) {
        r.pump(10);
        if (peakAt(r.spectrum(static_cast<std::uint32_t>(radio)), 12000.0, 8)) { return true; }
    }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_funcube\n");
    engine_api_test::hermetic("wirebfuncube");
    Access::installHooks();
    Access::setMakeDevice(&makeStandIn);
    g_rig = std::make_unique<FuncubeModelRig>(true);
    std::shared_ptr<Dongle> dongle = g_rig->dongle;
    AppConfig file;

    std::printf("F1: it opens and its tone is in the spectrum\n");
    {
        Rig r;
        CHECK_EQ(r.add(2, "open-funcube:", true).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.source") == "funcube"; }, 30000));
        CHECK(r.until([&] { return r.radio(2).sampleRateHz > 0.0; }, 10000));
        std::printf("  the radio runs at %.0f S/s\n", r.radio(2).sampleRateHz);
        CHECK_EQ(r.radio(2).sampleRateHz, 192000.0);
        CHECK(toneIn(r, 2));

        std::printf("F2: receive only, measured at the peer\n");
        {
            std::lock_guard<std::mutex> lk(dongle->mu);
            CHECK(!dongle->log.empty());
            CHECK(std::count(dongle->log.begin(), dongle->log.end(), fc::cmd::ResetToBootloader) == 0);
            std::set<int> allowed = {fc::cmd::Query, fc::cmd::SetFrequencyHz, fc::cmd::GetFrequencyHz, fc::cmd::SetLna, fc::cmd::SetMixer, fc::cmd::SetBiasTee, fc::cmd::GetLna, fc::cmd::GetMixer,
                                     fc::cmd::GetBiasTee};
            for (int i = 0; i < 6; ++i) { allowed.insert(fc::cmd::SetIfStage[i]); allowed.insert(fc::cmd::GetIfStage[i]); }
            for (const std::uint8_t c : dongle->log) { CHECK(allowed.count(c) == 1); }
        }

        std::printf("F3: a retune reaches the dongle; remove closes the HID link\n");
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 14.074e6)).status(), FOXAPI_OK);
        CHECK(r.until([&] { std::lock_guard<std::mutex> lk(dongle->mu); return dongle->freq == 14074000u; }, 10000));
        r.pump(20);
        file = throughTheFile(r.eng);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        CHECK(r.until([&] { std::lock_guard<std::mutex> lk(dongle->mu); return dongle->closed; }, 10000));
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    }

    std::printf("F4: saved, and comes back\n");
    CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
    if (file.radios.size() == 1) {
        CHECK_EQ(file.radios[0].sourceKind, std::string("funcube"));
        CHECK_EQ(file.radios[0].args, std::string("index=0"));
        std::printf("  saved: kind %s, args %s\n", file.radios[0].sourceKind.c_str(), file.radios[0].args.c_str());
    }
    {
        g_rig = std::make_unique<FuncubeModelRig>(true);   // (a fresh stand-in: the first one was consumed by the first open)
        dongle = g_rig->dongle;
        Rig y(&file);
        CHECK(y.until([&] { return y.fact("radio.2.source") == "funcube"; }, 30000));
        CHECK_EQ(y.fact("radio.restore"), std::string("<absent>"));
        CHECK_EQ(y.send(y.aimed(FOXAPI_OP_RUN, 2, 0, 1)).status(), FOXAPI_OK);
        CHECK(toneIn(y, 2));
    }

    std::printf("F5: no dongle plugged in\n");
    {
        g_rig = std::make_unique<FuncubeModelRig>(true, false);   // (no sound input named like the dongle)
        Rig r;
        (void)r.add(2, "open-funcube:", true);
        r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000);
        const std::string why = r.fact("radio.2.refusal");
        std::printf("  refusal: %s\n", why.c_str());
        CHECK(!why.empty() && why != "<absent>");
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
    }
    Access::setMakeDevice(nullptr);
    return check::finish("test_engine_api_wireb_funcube");
}
