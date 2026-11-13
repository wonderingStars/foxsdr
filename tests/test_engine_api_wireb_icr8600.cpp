// test_engine_api_wireb_icr8600.cpp - THE ICOM IC-R8600 [I/Q OUT] AS A RADIO OF THE ENGINE, SUCCESS PATH (wire-b second pass, item 34): RADIO_ADD "open-icr8600:" through the wire-b source table.
// The "radio" is the model test's STAND-IN IC-R8600 behind a fake USB device (tests/test_icr8600_source.cpp's World and FakeUsb, included here under a renamed main): the streaming personality
// 0C26:0023 is present, it answers CI-V and sends a 20 kHz tone in the radio's own I/Q framing. The engine's own device-open worker builds the source through Engine::testHooks_.makeDevice, which
// hands it the stand-in transport; no USB bus is touched and no Icom firmware is read or needed.
//
//   I1  the radio opens through the engine and its 20 kHz tone is in the radio's spectrum
//   I2  RECEIVE ONLY, measured at the peer: every CI-V command the radio was sent is one of the documented receive-side ones (frequency 05, attenuator 11, antenna 12, gain 14, pre-amp 16, the
//       I/Q-out prepare and switch 1A 13); none is a transmit, PTT or memory write
//   I3  a retune reaches the radio (the stand-in decodes the CI-V frequency); stopping the radio switches the I/Q output OFF; removing it closes the device
//   I4  the radio is SAVED as kind icr8600 with the arguments the open normalised to (timeout=1500, the source's own default: an empty list would be the generator to the file), and a restart
//       from the file alone opens it again
//   I5  no radio on the bus: the open is refused through the engine with the source's sentence and the radio stays on the generator
//
// UNVERIFIED on a real IC-R8600 and with Icom's real firmware: everything above is against the stand-in.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_icr8600_model_main
#include "test_icr8600_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "wire1_rig.hpp"

namespace {

std::shared_ptr<World> g_world;

std::unique_ptr<cascade::source::DeviceSource> makeStandIn(const std::string& kind) {
    if (kind != "icr8600") { return nullptr; }
    auto s = std::make_unique<Icr8600Source>();
    wire(*s, g_world);
    return s;
}

bool toneIn(Rig& r, std::size_t radio) {
    for (int i = 0; i < 80; ++i) {
        r.pump(10);
        if (peakAt(r.spectrum(static_cast<std::uint32_t>(radio)), 20000.0, 8)) { return true; }
    }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_icr8600\n");
    engine_api_test::hermetic("wirebicr8600");
    Access::installHooks();
    Access::setMakeDevice(&makeStandIn);
    g_world = streamWorld();
    AppConfig file;

    std::printf("I1: it opens and its tone is in the spectrum\n");
    {
        Rig r;
        CHECK_EQ(r.add(2, "open-icr8600:", true).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.source") == "icr8600"; }, 30000));
        CHECK(r.until([&] { std::lock_guard<std::mutex> lk(g_world->mu); return g_world->ivOn; }, 20000));
        CHECK(r.until([&] { return r.radio(2).sampleRateHz > 0.0; }, 10000));
        std::printf("  the radio runs at %.0f S/s\n", r.radio(2).sampleRateHz);
        CHECK(toneIn(r, 2));

        std::printf("I2: receive only, measured at the peer\n");
        {
            std::lock_guard<std::mutex> lk(g_world->mu);
            CHECK(!g_world->civLog.empty());
            const std::set<int> allowed = {0x05, 0x11, 0x12, 0x14, 0x16, 0x1A};
            for (const Bytes& c : g_world->civLog) {
                CHECK(!c.empty());
                if (!c.empty()) { CHECK(allowed.count(c[0]) == 1); }
                if (c.size() >= 2 && c[0] == 0x1A) { CHECK(c[1] == 0x13); }   // only the I/Q-out settings, never a memory or a menu write
            }
        }

        std::printf("I3: a retune reaches the radio; stop switches the output off; remove closes the device\n");
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.1e6)).status(), FOXAPI_OK);
        CHECK(r.until([&] { std::lock_guard<std::mutex> lk(g_world->mu); return g_world->freq == 7100000ull; }, 10000));
        r.pump(20);
        file = throughTheFile(r.eng);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { std::lock_guard<std::mutex> lk(g_world->mu); return !g_world->ivOn; }, 10000));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        CHECK(r.until([&] { std::lock_guard<std::mutex> lk(g_world->mu); return g_world->streamEnded; }, 10000));
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
    }

    std::printf("I4: saved, and comes back\n");
    CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
    if (file.radios.size() == 1) {
        CHECK_EQ(file.radios[0].sourceKind, std::string("icr8600"));
        CHECK_EQ(file.radios[0].args, std::string("timeout=1500"));
        std::printf("  saved: kind %s, args %s\n", file.radios[0].sourceKind.c_str(), file.radios[0].args.c_str());
    }
    {
        {
            std::lock_guard<std::mutex> lk(g_world->mu);
            g_world->streamEnded = false;
            g_world->ivOn = false;
        }
        Rig y(&file);
        CHECK(y.until([&] { return y.fact("radio.2.source") == "icr8600"; }, 30000));
        CHECK_EQ(y.fact("radio.restore"), std::string("<absent>"));
        CHECK_EQ(y.send(y.aimed(FOXAPI_OP_RUN, 2, 0, 1)).status(), FOXAPI_OK);
        CHECK(y.until([&] { std::lock_guard<std::mutex> lk(g_world->mu); return g_world->ivOn; }, 20000));
        CHECK(toneIn(y, 2));
    }

    std::printf("I5: no radio on the bus\n");
    {
        g_world = std::make_shared<World>();
        g_world->loaderPresent = false;
        g_world->streamPresent = false;
        Rig r;
        (void)r.add(2, "open-icr8600:", true);
        r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000);
        const std::string why = r.fact("radio.2.refusal");
        std::printf("  refusal: %s\n", why.c_str());
        CHECK(!why.empty() && why != "<absent>");
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
    }
    Access::setMakeDevice(nullptr);
    return check::finish("test_engine_api_wireb_icr8600");
}
