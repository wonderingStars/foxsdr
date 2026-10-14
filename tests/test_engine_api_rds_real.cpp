// test_engine_api_rds_real.cpp - what the engine publishes of a REAL decode (API 0.3
// section 8.4, window port group 5).
//
// tests/test_engine_api_03.cpp (group R) holds the flags, the counters and the
// bytes against stand-ins for the DSP thread (test.rds, test.autoNotch,
// test.pilot), because a hermetic engine on the signal generator hears no RDS.
// A stand-in proves the engine's half; this proves the other: that the member
// the stand-in replaces - Pipeline::rdsSnapshot() and pilotLocked(), written by
// the DSP thread - reaches the state a session reads, through the same fill.
//
// The input is a real FM carrier modulated with a real broadcast multiplex
// (tests/fm_stereo_rds_source.hpp, the signal tests/test_pipeline_audio.cpp
// decodes): a 19 kHz pilot, a stereo difference channel and an RDS subcarrier
// carrying a programme identification, a service name, RadioText, TP and TA.
// The engine runs under the test's own passes, so what it publishes and what
// the session reads is not a race with a control thread.
//
//   1. the decode arrives: every RDS flag, the codes, the texts, PILOT_LOCKED
//      and STEREO_ACTIVE
//   2. rdsGroups climbs while rdsSeq stays still (a measurement moves nothing)
//   3. stereo forced off: the lock stays, the lamp goes - the third state the
//      rail letters ("ST (forced mono)")
//   4. a retune forgets it all (the pipeline resets its decoders): the flags,
//      the texts and the lock clear, rdsSeq moves
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "check.hpp"
#include "engine/engine.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "fm_stereo_rds_source.hpp"
#include "foxsdr_api.h"

namespace cascade::gui {
struct AppWindowTestAccess {
    // What the window does to switch to another source: install it, then follow
    // its rate (the chain's decimation, and so the channel the multiplex must fit).
    static void installSource(cascade::engine::Engine& e, std::unique_ptr<cascade::source::IqSource> src) {
        e.installSource(std::move(src));
        e.followInputRate();
    }
};
}  // namespace cascade::gui

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

// Passes until `done` holds (at most timeoutMs).
template <class F>
bool passUntil(cascade::engine::Engine& eng, F done, int timeoutMs) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// One command, passes until its result lands.
int32_t pumped(cascade::engine::Engine& eng, testing::Session& s, const FoxCommand& c) {
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    if (n < 0 || sr.status != FOXAPI_OK) { return n < 0 ? n : sr.status; }
    for (int pass = 0; pass < 20; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) { return r[i].status; }
        }
    }
    return -1000;  // never landed: not a status the engine has
}

}  // namespace

int main() {
    engine_api_test::hermetic("rdsreal");
    const std::string ps = "CASCADE!";
    const std::string rt = "STEREO RDS TEST ";  // whole 4-character segments
    const std::string rtShown = "STEREO RDS TEST";  // the decoder trims the trailing space
    constexpr std::uint16_t kPi = 0x2C41;
    constexpr unsigned kPty = 10;

    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    CHECK(s.ok());

    // The receiver listens to the transmitter: the transmitter at DC, the whole
    // multiplex inside the channel, nothing gating.
    cascade::gui::AppWindowTestAccess::installSource(
        eng, std::make_unique<fmrds::FmStereoSource>(
                 fmrds::biphaseLevels(fmrds::buildRdsBits(kPi, ps, rt, kPty, 200))));
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM)), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_BANDWIDTH, fmrds::kVfoBandwidthHz)), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 0.0)), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_RUN, 0.0, 1)), FOXAPI_OK);

    eng.controlPass();
    {
        const FoxReceiverState r = s.state();
        std::printf("receiver: \"%s\" at %.0f S/s, channel %.0f S/s, mode %u, bandwidth %.0f Hz, VFO %+.0f Hz, flags 0x%08x\n",
                    r.deviceName, r.sampleRateHz, r.channelRateHz, r.demodMode, r.bandwidthHz, r.vfoOffsetHz, r.flags);
    }

    // 1. THE DECODE ARRIVES.
    const bool decoded = passUntil(
        eng,
        [&] {
            const FoxReceiverState st = s.state();
            return (st.rdsFlags & FOXAPI_RDS_PS_VALID) != 0u && (st.rdsFlags & FOXAPI_RDS_PI_VALID) != 0u &&
                   (st.flags & FOXAPI_RX_PILOT_LOCKED) != 0u && std::string(st.rdsRadioText) == rtShown;
        },
        40000);
    FoxReceiverState st = s.state();
    std::printf("real decode: rdsFlags 0x%x PI %04X PTY %u PS \"%s\" RT \"%s\" groups %llu errors %llu, flags 0x%08x, pilot level %.2f\n",
                st.rdsFlags, st.rdsPi, st.rdsPty, st.rdsPs, st.rdsRadioText, static_cast<unsigned long long>(st.rdsGroups),
                static_cast<unsigned long long>(st.rdsErrors), st.flags, st.pilotLevel);
    CHECK(decoded);
    CHECK_EQ(st.rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_SYNCED | FOXAPI_RDS_PI_VALID | FOXAPI_RDS_PS_VALID |
                                                FOXAPI_RDS_TP | FOXAPI_RDS_TA | FOXAPI_RDS_RT_VALID));
    CHECK_EQ(st.rdsPi, static_cast<uint32_t>(kPi));
    CHECK_EQ(st.rdsPty, kPty);
    CHECK_EQ(std::string(st.rdsPs), ps);
    CHECK_EQ(std::string(st.rdsRadioText), rtShown);
    CHECK(st.rdsGroups > 0u);
    CHECK((st.flags & FOXAPI_RX_PILOT_LOCKED) != 0u);
    CHECK((st.flags & FOXAPI_RX_STEREO_ENABLED) != 0u);
    CHECK((st.flags & FOXAPI_RX_STEREO_ACTIVE) != 0u);  // enabled AND locked
    CHECK(st.pilotLevel > 0.5);
    CHECK(st.rdsSeq >= 2u);  // it moved when the fields arrived

    // 2. A MEASUREMENT MOVES NO COUNTER: the groups keep arriving, and with the
    // words unchanged rdsSeq stands still.
    const FoxReceiverState a = s.state();
    CHECK(passUntil(eng, [&] { return s.state().rdsGroups >= a.rdsGroups + 20u; }, 20000));
    const FoxReceiverState b = s.state();
    std::printf("groups %llu -> %llu, rdsSeq %llu -> %llu\n", static_cast<unsigned long long>(a.rdsGroups),
                static_cast<unsigned long long>(b.rdsGroups), static_cast<unsigned long long>(a.rdsSeq),
                static_cast<unsigned long long>(b.rdsSeq));
    CHECK(b.rdsGroups >= a.rdsGroups + 20u);
    CHECK_EQ(b.rdsSeq, a.rdsSeq);

    // 3. STEREO FORCED OFF: the lock is the decoder's, the lamp is its AND the
    // user's.
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_STEREO, 0.0, 0)), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return (s.state().flags & FOXAPI_RX_STEREO_ACTIVE) == 0u; }, 5000));
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_STEREO_ENABLED) == 0u);
    CHECK((st.flags & FOXAPI_RX_STEREO_ACTIVE) == 0u);
    CHECK((st.flags & FOXAPI_RX_PILOT_LOCKED) != 0u);
    CHECK((st.rdsFlags & FOXAPI_RDS_PS_VALID) != 0u);  // the same station, still heard
    std::printf("stereo forced off: PILOT_LOCKED set, STEREO_ACTIVE clear (flags 0x%08x)\n", st.flags);
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_STEREO, 0.0, 1)), FOXAPI_OK);

    // 4. A RETUNE FORGETS IT ALL: the pipeline resets its RDS and pilot decoders
    // (a lingering name from the station left would be a lie about the new one).
    const FoxReceiverState before = s.state();
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 100000.0)), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return (s.state().rdsFlags & FOXAPI_RDS_PS_VALID) == 0u; }, 10000));
    st = s.state();
    std::printf("retuned: rdsFlags 0x%x PS \"%s\" RT \"%s\" PILOT_LOCKED %d, rdsSeq %llu -> %llu\n", st.rdsFlags, st.rdsPs,
                st.rdsRadioText, (st.flags & FOXAPI_RX_PILOT_LOCKED) != 0u ? 1 : 0,
                static_cast<unsigned long long>(before.rdsSeq), static_cast<unsigned long long>(st.rdsSeq));
    CHECK_EQ(std::string(st.rdsPs), std::string());
    CHECK_EQ(std::string(st.rdsRadioText), std::string());
    CHECK((st.rdsFlags & FOXAPI_RDS_PI_VALID) == 0u);
    CHECK(st.rdsSeq > before.rdsSeq);
    CHECK(passUntil(eng, [&] { return (s.state().flags & FOXAPI_RX_PILOT_LOCKED) == 0u; }, 10000));

    return check::finish("test_engine_api_rds_real");
}
