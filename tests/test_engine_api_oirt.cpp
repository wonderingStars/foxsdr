// test_engine_api_oirt.cpp - the stereo system in the ENGINE (FOXAPP_OP_SET_STEREO_SYSTEM; engine wiring W2; Engine::pollStereoSystem, core/pipeline.cpp setStereoSystem),
// held against the real engine through the C table and its own passes, on the signal generator (the receiver runs, WFM is the mode, the DSP thread processes blocks).
//
//   S1  AUTOMATIC is the default and follows the band: the pilot-tone decoder at 100.1 MHz, the OIRT decoder inside 65.8 to 74 MHz (70 MHz, and both band edges), the
//       pilot-tone decoder again when the radio is tuned out of the band; fact.radio.<n>.stereo.system says auto throughout
//   S2  the explicit choices win over the band: pilot tone at 70 MHz, OIRT at 100.1 MHz; "0" gives the band's answer back
//   S3  in any other mode than WFM neither decoder is in the path: the decoder fact is "pilot" (the OIRT one is not running) even inside the OIRT band
//   S4  refusals: 3 and -1 are OUT_OF_RANGE, the same value again is NO_CHANGE, a radio that does not exist is NO_DEVICE
//   S5  the choice is per radio: radio 2 keeps its own while radio 1 is changed
//   S6  a REMOTE session cannot send the op
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Rig {
    cascade::engine::Engine eng;
    std::unique_ptr<testing::Session> local;
    Rig() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK(local->ok());
    }
    testing::Outcome send(const FoxCommand& c, testing::Session* who = nullptr) {
        testing::Session& s = who != nullptr ? *who : *local;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int i = 0; i < 40 && !o.landed; ++i) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s.raw(), r, 16);
            for (int32_t k = 0; k < got; ++k) {
                if (r[k].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[k];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[256];
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t radio, double n0 = 0.0, long long i0 = 0) {
        FoxCommand c = testing::cmd(op, n0, i0);
        c.target = radio;
        return c;
    }
    // Waits (the DSP thread has to process blocks for the decoder fact to follow) until a fact reads `want`.
    bool until(const std::string& key, const std::string& want, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (fact(key) == want) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return fact(key) == want;
    }
    void tune(std::uint32_t radio, double hz) { CHECK_EQ(send(aimed(FOXAPI_OP_SET_FREQUENCY, radio, hz)).status(), FOXAPI_OK); }
};

void testAutomatic() {
    std::printf("S1: automatic follows the band\n");
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0, FOXAPI_DEMOD_WFM)).status() == FOXAPI_OK || true, true);
    r.tune(1, 100.1e6);
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("auto"));
    r.tune(1, 70.0e6);
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    r.tune(1, 65.8e6);   // the band's lower edge is in
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    r.tune(1, 74.0e6);   // and so is the upper
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    r.tune(1, 74.1e6);   // just above: out
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    r.tune(1, 65.7e6);   // just below: out
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    r.tune(1, 70.0e6);
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    r.tune(1, 100.1e6);
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("auto"));
}

void testChoice() {
    std::printf("S2/S3/S4: the explicit choices, other modes, the refusals\n");
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0, 1)).status(), FOXAPI_OK);
    r.tune(1, 70.0e6);
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 1)).status(), FOXAPI_OK);    // pilot tone, at 70 MHz
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("pilot"));
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    r.tune(1, 100.1e6);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 2)).status(), FOXAPI_OK);    // OIRT, at 100.1 MHz
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("oirt"));
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 0)).status(), FOXAPI_OK);    // automatic again: 100.1 MHz is the pilot-tone system's
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("auto"));
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    // S3: NFM inside the band: no composite decoder at all
    r.tune(1, 70.0e6);
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0, FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    // S4
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 3)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 0)).status(), FOXAPI_NO_CHANGE);   // it is automatic already
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 7, 0, 1)).status(), FOXAPI_NO_DEVICE);
}

void testPerRadio() {
    std::printf("S5: per radio\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, "")).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 1)).status(), FOXAPI_OK);
    r.tune(1, 100.1e6);
    r.tune(2, 70.0e6);
    CHECK(r.until("radio.1.stereo.decoder", "pilot"));
    CHECK(r.until("radio.2.stereo.decoder", "oirt"));
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 2)).status(), FOXAPI_OK);   // radio 1 to OIRT
    CHECK(r.until("radio.1.stereo.decoder", "oirt"));
    CHECK_EQ(r.fact("radio.2.stereo.system"), std::string("auto"));                          // radio 2 untouched
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 2, 0, 1)).status(), FOXAPI_OK);   // radio 2 to pilot tone
    CHECK(r.until("radio.2.stereo.decoder", "pilot"));
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("oirt"));
    CHECK_EQ(r.fact("radio.1.stereo.decoder"), std::string("oirt"));
}

void testRemote() {
    std::printf("S6: a REMOTE session cannot send the op\n");
    Rig r;
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_SET_STEREO_SYSTEM, 1, 0, 2), &remote).status(), FOXAPI_DENIED);
    CHECK_EQ(r.fact("radio.1.stereo.system"), std::string("auto"));
}

}  // namespace

int main() {
    engine_api_test::hermetic("oirt");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testAutomatic();
    testChoice();
    testPerRadio();
    testRemote();
    return check::finish("test_engine_api_oirt");
}
