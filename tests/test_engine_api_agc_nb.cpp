// test_engine_api_agc_nb.cpp - THE AUDIO AGC CHOICE AND THE NOISE BLANKER THROUGH THE ENGINE API (multi-radio lane C), held against the real engine through the
// C table and the engine's own passes, with no window anywhere:
//
//   A  FOXAPI_OP_SET_AGC and FOXAPI_OP_SET_NB land in the radio's state (agcMode, agcFixedGainDb, nbEnabled, nbThresholdDb) and move modeSeq; the mode
//      first and the fixed gain after it (Off with a fixed gain leaves THAT gain in force, down to -20 dB);
//   B  every range refusal at the submit (a mode past Off, a fixed gain outside -20..+40, a threshold outside 6..30) changes nothing, and a number the op does
//      not carry (ival[1] = 0) is not range-checked;
//   C  the target: radio 1 (and 0, the focus) lands, radio 2 and the first number past the limit are refused NO_DEVICE and change nothing;
//   D  the live gain and the blanked share are MEASUREMENTS: with the radio running they do not move modeSeq.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static const cascade::engine::Receiver& rx(Engine& e) { return *e.receivers_[0]; }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kLocalGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_agc_nb_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

// A fact's value, or "<absent>" for a key the engine does not publish.
std::string fact(testing::Session& s, const std::string& key) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string("<absent>");
}

template <class F>
bool passUntil(Engine& eng, F done, int timeoutMs = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void passes(Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

// A command pumped through the engine's own passes; the outcome carries the submit's answer (a refusal at the submit never reaches the queue) and, when
// it was queued, the result.
testing::Outcome pumped(Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0) {
    FoxCommand c = testing::cmd(op, n0, i0);
    c.target = target;
    return c;
}

struct Direct {
    Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};


FoxCommand agcCmd(std::uint32_t target, long long mode, bool withGain = false, double gainDb = 0.0) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_AGC, gainDb, mode);
    c.ival[1] = withGain ? 1 : 0;
    c.target = target;
    return c;
}

FoxCommand nbCmd(std::uint32_t target, long long on, bool withThreshold = false, double thresholdDb = 0.0) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_NB, thresholdDb, on);
    c.ival[1] = withThreshold ? 1 : 0;
    c.target = target;
    return c;
}

void testAgcOp() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    FoxReceiverState st = s.state();
    CHECK_EQ(st.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_AUTO));  // the default is the AGC as it always was
    CHECK_EQ(st.agcFixedGainDb, 0.0);
    for (std::uint32_t m = FOXAPI_AGC_FAST; m <= FOXAPI_AGC_OFF; ++m) {
        const std::uint64_t before = s.state().modeSeq;
        CHECK_EQ(pumped(d.eng, s, agcCmd(0, m)).status(), FOXAPI_OK);
        passes(d.eng, 2);
        st = s.state();
        CHECK_EQ(st.agcMode, m);
        CHECK(st.modeSeq > before);  // the mode group moved
        CHECK_EQ(Access::rx(d.eng).agcMode_, static_cast<int>(m));
    }
    // The mode first, the fixed gain after it: Off with -12 dB leaves -12 dB in force, in the state and in the pipeline.
    CHECK_EQ(pumped(d.eng, s, agcCmd(1, FOXAPI_AGC_AUTO)).status(), FOXAPI_OK);
    testing::Outcome o = pumped(d.eng, s, agcCmd(1, FOXAPI_AGC_OFF, true, -12.0));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(o.result.applied[0], -12.0);
    passes(d.eng, 2);
    st = s.state();
    CHECK_EQ(st.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_OFF));
    CHECK_EQ(st.agcFixedGainDb, -12.0);
    CHECK_EQ(Access::rx(d.eng).pipeline_.agcFixedGainDb(), -12.0f);
    // The ends of the widened range, both landing exactly.
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_OFF, true, -20.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().agcFixedGainDb, -20.0);
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_OFF, true, 40.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().agcFixedGainDb, 40.0);
    // A mode alone leaves the fixed gain alone.
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_SLOW)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_SLOW));
    CHECK_EQ(s.state().agcFixedGainDb, 40.0);
}

void testNbOp() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    FoxReceiverState st = s.state();
    CHECK_EQ(st.nbEnabled, 0u);  // off by default
    CHECK_EQ(st.nbThresholdDb, 15.0);
    const std::uint64_t before = st.modeSeq;
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    st = s.state();
    CHECK_EQ(st.nbEnabled, 1u);
    CHECK_EQ(st.nbThresholdDb, 15.0);  // the switch alone leaves the threshold alone
    CHECK(st.modeSeq > before);
    testing::Outcome o = pumped(d.eng, s, nbCmd(1, 1, true, 22.5));
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK_EQ(o.result.applied[0], 22.5);
    passes(d.eng, 2);
    CHECK_EQ(s.state().nbThresholdDb, 22.5);
    CHECK_EQ(Access::rx(d.eng).pipeline_.noiseBlankerThresholdDb(), 22.5f);
    CHECK(Access::rx(d.eng).pipeline_.noiseBlankerEnabled());
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().nbEnabled, 0u);
    CHECK_EQ(s.state().nbThresholdDb, 22.5);
    // The ends of the range land exactly.
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 1, true, 6.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().nbThresholdDb, 6.0);
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 1, true, 30.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().nbThresholdDb, 30.0);
}

void testRefusals() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_MEDIUM, true, 5.0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 1, true, 18.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const FoxReceiverState before = s.state();
    CHECK_EQ(before.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_MEDIUM));
    // a mode past Off, a negative mode
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, 5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, -1)).status(), FOXAPI_OUT_OF_RANGE);
    // a fixed gain outside -20..+40
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_OFF, true, 40.5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_OFF, true, -20.5)).status(), FOXAPI_OUT_OF_RANGE);
    // a threshold outside 6..30
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 0, true, 5.9)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 0, true, 30.1)).status(), FOXAPI_OUT_OF_RANGE);
    passes(d.eng, 2);
    // nothing changed, not even the mode that came with the bad gain
    const FoxReceiverState after = s.state();
    CHECK_EQ(after.agcMode, before.agcMode);
    CHECK_EQ(after.agcFixedGainDb, before.agcFixedGainDb);
    CHECK_EQ(after.nbEnabled, before.nbEnabled);
    CHECK_EQ(after.nbThresholdDb, before.nbThresholdDb);
    CHECK_EQ(after.modeSeq, before.modeSeq);
    // a number the op does not carry (ival[1] = 0) is not range-checked
    FoxCommand c = agcCmd(0, FOXAPI_AGC_FAST);
    c.num[0] = 1.0e6;
    CHECK_EQ(pumped(d.eng, s, c).status(), FOXAPI_OK);
    FoxCommand n = nbCmd(0, 1);
    n.num[0] = -1.0e6;
    CHECK_EQ(pumped(d.eng, s, n).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().agcFixedGainDb, before.agcFixedGainDb);
    CHECK_EQ(s.state().nbThresholdDb, 18.0);
}

void testTheTarget() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const FoxReceiverState before = s.state();
    // A radio that is not there: refused at the submit, NO_DEVICE, and nothing changed.
    testing::Outcome r = pumped(d.eng, s, agcCmd(2, FOXAPI_AGC_OFF, true, 9.0));
    CHECK_EQ(r.status(), FOXAPI_NO_DEVICE);
    CHECK(r.landed);
    CHECK((r.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK_EQ(pumped(d.eng, s, nbCmd(2, 1, true, 9.0)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, agcCmd(static_cast<std::uint32_t>(cascade::engine::kMaxReceivers) + 1u, FOXAPI_AGC_FAST)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, nbCmd(0xFFFFFFFEu, 1)).status(), FOXAPI_NO_DEVICE);
    // ALL_RADIOS is for RUN alone.
    CHECK_EQ(pumped(d.eng, s, agcCmd(FOXAPI_TARGET_ALL_RADIOS, FOXAPI_AGC_FAST)).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(pumped(d.eng, s, nbCmd(FOXAPI_TARGET_ALL_RADIOS, 1)).status(), FOXAPI_BAD_ARGUMENT);
    passes(d.eng, 2);
    const FoxReceiverState after = s.state();
    CHECK_EQ(after.agcMode, before.agcMode);
    CHECK_EQ(after.agcFixedGainDb, before.agcFixedGainDb);
    CHECK_EQ(after.nbEnabled, before.nbEnabled);
    CHECK_EQ(after.nbThresholdDb, before.nbThresholdDb);
    CHECK_EQ(after.modeSeq, before.modeSeq);
    // The local command path (the window queue) is held to the same rule.
    CHECK_EQ(d.eng.applyCommand(agcCmd(2, FOXAPI_AGC_FAST)).status, FOXAPI_NO_DEVICE);
    CHECK_EQ(d.eng.applyCommand(nbCmd(2, 1)).status, FOXAPI_NO_DEVICE);
    CHECK_EQ(d.eng.applyCommand(agcCmd(1, FOXAPI_AGC_FAST)).status, FOXAPI_OK);
    CHECK_EQ(d.eng.applyCommand(nbCmd(1, 1)).status, FOXAPI_OK);
    CHECK_EQ(Access::rx(d.eng).agcMode_, static_cast<int>(FOXAPI_AGC_FAST));
    CHECK(Access::rx(d.eng).nbEnabled_);
}

void testMeasurementsDoNotMoveTheMode() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(pumped(d.eng, s, agcCmd(0, FOXAPI_AGC_FAST)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, nbCmd(0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RUNNING) != 0u; }));
    passes(d.eng, 3);
    const FoxReceiverState first = s.state();
    // Let the audio flow for a while; the live readouts may move, the mode group must not.
    double lastGain = first.agcGainDb;
    bool gainMoved = false;
    for (int i = 0; i < 150; ++i) {
        d.eng.controlPass();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        const FoxReceiverState now = s.state();
        CHECK_EQ(now.modeSeq, first.modeSeq);
        CHECK(now.nbBlankedPct >= 0.0 && now.nbBlankedPct <= 100.0);
        if (now.agcGainDb != lastGain) { gainMoved = true; }
        lastGain = now.agcGainDb;
    }
    // (whether the gain moved depends on the generator level against the loop target; it is reported, not required)
    std::printf("  live gain moved while running: %s, last %.2f dB\n", gainMoved ? "yes" : "no", lastGain);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
}

}  // namespace

int main() {
    std::printf("test_engine_api_agc_nb\n");
    isolate();
    testAgcOp();
    testNbOp();
    testRefusals();
    testTheTarget();
    testMeasurementsDoNotMoveTheMode();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_agc_nb");
}
