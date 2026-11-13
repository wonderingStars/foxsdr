// test_engine_api_wireb_hackrfsweep.cpp - A HACKRF IN SWEEP MODE AS THE SWEEP DEVICE OF THE ENGINE (wire-b; FOXAPP_OP_SWEEP_OPEN kind "hackrf", 0x8920, with SWEEP_READ / SWEEP_CLOSE; engine/wire_b_sweep.cpp over
// source/hackrf_sweep.hpp). The device is the model test's STAND-IN HackRF (tests/test_hackrf_sweep.cpp's `Radio` and `FakeUsb`, included under a renamed main): a 2437.5 MHz tone over a noise floor on a fake
// USB transport. No real USB device is opened.
//
//   H1  SWEEP_OPEN hackrf 2400-2500 MHz: the stand-in is programmed (an init-sweep request with the range), sweeps arrive, the heatmap fills; the newest line peaks at the tone (2437.5 MHz within 6 MHz)
//   H2  RECEIVE ONLY: the stand-in saw no transmit request and no transmit mode (its control log holds none)
//   H3  close: the stand-in's streaming ends and the sweep mode is switched off; a second open is BUSY while one is open
//   H4  refusals: a port on a HackRF, a range that is not whole MHz / over 6000 MHz / reversed, an fft that is not a power of two, a serial on a tinySA; a device that is not there is FAILED with a sentence
//
// UNVERIFIED on a real HackRF (every USB request is the model's: see the oss-radios record).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_hackrfsweep_model_main
#define Rig HackRfModelRig   // (the model test has its own Rig; the engine rig below keeps the name)
#include "test_hackrf_sweep.cpp"
#undef Rig
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

testing::Outcome sendText(Rig& r, std::uint32_t op, const std::string& text, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(sess.raw(), &c, 1, &sr) : api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) { if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; } }
    }
    return o;
}

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

json state(Rig& r) {
    const json j = json::parse(bigFact(r, "sweep.state"), nullptr, false);
    return j.is_discarded() ? json() : j;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_hackrfsweep\n");
    engine_api_test::hermetic("wirebhackrfsweep");
    Access::installHooks();
    Rig rig;
    cascade::engine::WireBState& w = rig.eng.wireBStateForTest();
    auto radio = std::make_shared<Radio>();
    bool present = true;
    w.hackrfFactory = [&](const std::string& args, std::string& why) -> std::unique_ptr<hs::HackRfSweeper> {
        (void)args;
        if (!present) { why = "no HackRF found (is it plugged in?)"; return nullptr; }
        auto sw = std::make_unique<hs::HackRfSweeper>();
        std::vector<cascade::usb::UsbDeviceInfo> devs;
        devs.push_back({hr::kUsbVid, hr::kPidHackRfOne, "fake", "0000000000000000A", "HackRF One"});
        auto r = radio;
        sw->setTransportForTest(devs, [r](const std::string&, std::string&) { return std::make_unique<FakeUsb>(r); });
        if (!sw->open("")) { why = sw->lastError(); return nullptr; }
        return sw;
    };

    std::printf("H4a: a device that is not there\n");
    present = false;
    {
        const auto o = sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2400,\"stop_mhz\":2500}");
        CHECK_EQ(o.status(), FOXAPI_FAILED);
        CHECK(std::string(o.result.message).find("plugged in") != std::string::npos);
    }
    present = true;

    std::printf("H1: open and sweep\n");
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2400,\"stop_mhz\":2500,\"step_mhz\":10,\"lna_db\":16,\"vga_db\":16,\"fft_size\":1024}").status(), FOXAPI_OK);
    CHECK(rig.until([&] { const json j = state(rig); return j.is_object() && j["sweeps"].get<std::uint64_t>() >= 3; }, 30000));
    {
        const json j = state(rig);
        std::printf("  sweeps %llu, heatmap %zu rows\n", static_cast<unsigned long long>(j["sweeps"].get<std::uint64_t>()), j["heat_rows"].get<std::size_t>());
        CHECK(j["open"].get<bool>());
        CHECK_EQ(j["kind"].get<std::string>(), std::string("hackrf"));
        CHECK(j["heat_rows"].get<std::size_t>() >= 1);
        CHECK(std::fabs(j["start_hz"].get<double>() - 2400.0e6) < 1.0 && std::fabs(j["stop_hz"].get<double>() - 2500.0e6) < 1.0);
        CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_READ, "{\"max_points\":200}").status(), FOXAPI_OK);
        const json l = json::parse(bigFact(rig, "sweep.read"), nullptr, false);
        CHECK(!l.is_discarded());
        if (!l.is_discarded() && l["db"].size() >= 20) {
            std::size_t best = 0;
            for (std::size_t i = 0; i < l["db"].size(); ++i) { if (l["db"][i].is_number() && (l["db"][best].is_null() || l["db"][i].get<double>() > l["db"][best].get<double>())) { best = i; } }
            const double hz = l["start_hz"].get<double>() + (l["stop_hz"].get<double>() - l["start_hz"].get<double>()) * static_cast<double>(best) / static_cast<double>(l["db"].size() - 1);
            std::printf("  peak at %.1f MHz (%.1f dB)\n", hz / 1e6, l["db"][best].get<double>());
            CHECK(std::fabs(hz - 2437.5e6) < 6.0e6);
        }
    }

    std::printf("H2: receive only\n");
    {
        std::lock_guard<std::mutex> lk(radio->mu);
        std::size_t initSweeps = 0;
        for (const Radio::Ctl& c : radio->log) {
            if (c.request == hs::kRequestInitSweep) { ++initSweeps; }
            CHECK(c.request != hr::requestByte(hr::VendorRequest::SetTransceiverMode) || c.value != static_cast<std::uint16_t>(hr::TransceiverMode::Transmit));   // transmit mode: never sent
        }
        CHECK(initSweeps >= 1);
        CHECK(radio->sweeping);
    }

    std::printf("H3: busy, close\n");
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2400,\"stop_mhz\":2500}").status(), FOXAPI_BUSY);
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_OK);
    CHECK(rig.until([&] { return !state(rig)["open"].get<bool>(); }, 15000));
    {
        std::lock_guard<std::mutex> lk(radio->mu);
        CHECK(!radio->sweeping);   // the sweep mode is switched off
    }
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_NO_CHANGE);

    std::printf("H4: refusals\n");
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"port\":\"COM3\",\"start_mhz\":2400,\"stop_mhz\":2500}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2400.5,\"stop_mhz\":2500}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":5000,\"stop_mhz\":7000}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2500,\"stop_mhz\":2400}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2400,\"stop_mhz\":2500,\"fft_size\":1000}").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM3\",\"serial\":\"x\",\"start_mhz\":1,\"stop_mhz\":2}").status(), FOXAPI_BAD_ARGUMENT);
    testing::Session remote(api(), rig.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(sendText(rig, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"hackrf\",\"start_mhz\":2400,\"stop_mhz\":2500}", &remote).status(), FOXAPI_DENIED);
    return check::finish("test_engine_api_wireb_hackrfsweep");
}
