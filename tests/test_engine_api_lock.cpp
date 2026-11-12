// test_engine_api_lock.cpp - the frequency lock in the ENGINE (FOXAPP_OP_RADIO_LOCK; engine wiring W2, the engine half of slint-gaps item 3), held against
// the real engine through the C table and its own passes. The window's lock (slint/gaps_logic.cpp) stops what the window sends; this one stops what ANY
// session sends and every other way a retune reaches the source.
//
//   L1  the op: lock answers OK, fact.radio.<n>.locked reads 1; locking again is NO_CHANGE; unlock answers OK and reads 0; unlocking again is NO_CHANGE
//   L2  a locked radio refuses every tuning command with DENIED (SET_FREQUENCY, SET_CENTRE, SET_VFO_OFFSET, STEP_TUNE, VFO_TO_ABSOLUTE, SET_VFO_OFFSET_FREE,
//       SCANNER_RUN that starts) and its frequency does not move; mode, volume and squelch are not tuning and still work
//   L3  unlocking lets the same commands through
//   L4  the lock is PER RADIO: radio 1 locked leaves radio 2 free to tune, and the other way round
//   L5  THE CHOKE POINT: a retune that does not come as a command (the scanner's step, a plugin's request_tune) is a no-op on a locked radio and works again
//       after the unlock
//   L6  a REMOTE session cannot send the op ("the application's own commands are for sessions on this machine"), so it cannot unlock what the operator locked
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    // A retune that is not a command: what the scanner's next step and a plugin's legacy request_tune call, on radio `slot` (0 = radio 1).
    static void retune(Engine& e, std::size_t slot, double centreHz) {
        Engine::RadioWork scope(e, slot);
        e.retuneSourceHz(centreHz);
    }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

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
    double tunedHz(std::uint32_t radio) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        api()->read_radio_state(local->raw(), radio, &st);
        return st.tunedHz;
    }
    double centreHz(std::uint32_t radio) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        api()->read_radio_state(local->raw(), radio, &st);
        return st.centreHz;
    }
    void pass(int n = 3) {
        for (int i = 0; i < n; ++i) { eng.controlPass(); }
    }
};

void testOp() {
    std::printf("L1: the op and its fact\n");
    Rig r;
    CHECK_EQ(r.fact("radio.1.locked"), std::string("0"));
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.locked"), std::string("1"));
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 1)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.locked"), std::string("0"));
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 5, 0, 1)).status(), FOXAPI_NO_DEVICE);   // there is no radio 5
}

void testRefusals() {
    std::printf("L2/L3: a locked radio refuses tuning and still does the rest\n");
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 100.1e6)).status(), FOXAPI_OK);
    const double before = r.tunedHz(1);
    CHECK(std::fabs(before - 100.1e6) < 1.0);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 1)).status(), FOXAPI_OK);
    const FoxCommand tuning[] = {r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 98.0e6),     r.aimed(FOXAPI_OP_SET_CENTRE, 1, 98.0e6),
                                 r.aimed(FOXAPI_OP_SET_VFO_OFFSET, 1, 20000.0),   r.aimed(FOXAPI_OP_STEP_TUNE, 1, 12500.0, 1),
                                 r.aimed(FOXAPP_OP_VFO_TO_ABSOLUTE, 1, 99.0e6),   r.aimed(FOXAPP_OP_SET_VFO_OFFSET_FREE, 1, 30000.0),
                                 r.aimed(FOXAPI_OP_SCANNER_RUN, 1, 0.0, 1)};
    for (const FoxCommand& c : tuning) {
        const testing::Outcome o = r.send(c);
        if (o.status() != FOXAPI_DENIED) { std::printf("  op 0x%04x answered %d\n", c.op, static_cast<int>(o.status())); }
        CHECK_EQ(o.status(), FOXAPI_DENIED);
    }
    CHECK(std::fabs(r.tunedHz(1) - before) < 1.0);   // nothing moved
    // not tuning: mode, volume, squelch, mute still work on the locked radio, and so does stopping a scan
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_MODE, 1, 0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VOLUME, 1, 0.5)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_SQUELCH, 1, -60.0)).status(), FOXAPI_OK);
    CHECK(r.send(r.aimed(FOXAPI_OP_SCANNER_RUN, 1, 0.0, 0)).status() != FOXAPI_DENIED);   // a stop is not a start
    // L3: unlocked, the same command goes through
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 98.0e6)).status(), FOXAPI_OK);
    CHECK(std::fabs(r.tunedHz(1) - 98.0e6) < 1.0);
}

void testPerRadio() {
    std::printf("L4: the lock is per radio\n");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, "")).status(), FOXAPI_OK);
    r.pass(10);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 2, 101.0e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.2.locked"), std::string("0"));
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 2, 102.0e6)).status(), FOXAPI_OK);   // radio 2 is free
    CHECK(std::fabs(r.tunedHz(2) - 102.0e6) < 1.0);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 97.0e6)).status(), FOXAPI_DENIED);   // radio 1 is not
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 2, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 2, 103.0e6)).status(), FOXAPI_DENIED);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 97.0e6)).status(), FOXAPI_OK);   // and the other way round
    CHECK(std::fabs(r.tunedHz(2) - 102.0e6) < 1.0);
}

void testChokePoint() {
    std::printf("L5: a retune that is not a command is a no-op on a locked radio\n");
    Rig r;
    // (a retune that is not a command does not mark the published state dirty by itself: a harmless command after it makes the pass publish, as the real callers' passes do)
    const auto publish = [&r] { r.send(r.aimed(FOXAPI_OP_SET_VOLUME, 1, 0.4)); };
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 100.0e6)).status(), FOXAPI_OK);
    Access::retune(r.eng, 0, 95.0e6);
    publish();
    CHECK(std::fabs(r.centreHz(1) - 95.0e6) < 1.0);    // the call works on an unlocked radio
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 1)).status(), FOXAPI_OK);
    Access::retune(r.eng, 0, 90.0e6);
    publish();
    CHECK(std::fabs(r.centreHz(1) - 95.0e6) < 1.0);    // the scanner's step or a plugin's request_tune did nothing
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 0)).status(), FOXAPI_OK);
    Access::retune(r.eng, 0, 90.0e6);
    publish();
    CHECK(std::fabs(r.centreHz(1) - 90.0e6) < 1.0);    // the same call works once unlocked
}

void testRemote() {
    std::printf("L6: a REMOTE session cannot send the op\n");
    Rig r;
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPP_OP_RADIO_LOCK, 1, 0, 0), &remote).status(), FOXAPI_DENIED);
    CHECK_EQ(r.fact("radio.1.locked"), std::string("1"));   // still locked
    // and a remote session's tune is refused by the lock itself
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, 1, 95.0e6), &remote).status(), FOXAPI_DENIED);
}

}  // namespace

int main() {
    engine_api_test::hermetic("lock");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testOp();
    testRefusals();
    testPerRadio();
    testChokePoint();
    testRemote();
    return check::finish("test_engine_api_lock");
}
