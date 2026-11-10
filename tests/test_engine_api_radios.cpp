// test_engine_api_radios.cpp - ADDRESSING THE RADIOS (multi-radio stage 3), held against the real engine through the C table and the engine's own passes,
// with no window anywhere. There is still ONE live receiver; what is proven is the vocabulary a client speaks to many, and that it answers for one:
//
//   A  THE FACTS: fact.radio.count is 1 and fact.radio.max is the engine's limit (kMaxReceivers, eight - a client never hard-codes it); radio 1 has a
//      state, a device, a label, an audio route and its plugins; radio 2 has none; the state follows the receiver (stopped, then running);
//   B  A TARGET THAT NAMES NO RADIO IS REFUSED NO_DEVICE (answered at once, as a refused result), for an op that addresses one radio - target 2, and the first number past the limit -
//      and nothing changes; target 0 (the focus) and target 1 land exactly as before; ALL_RADIOS is accepted for RUN and refused (BAD_ARGUMENT) for any
//      other; an op that is not about a radio ignores the target;
//   C  THE SAME FOR A COMMAND THAT DOES NOT COME FROM A SESSION (Engine::applyCommand: the window's local queue): target 2 is NO_DEVICE there too;
//   D  RADIO_FOCUS: a number the engine does not number is OUT_OF_RANGE, a radio it does not have is NO_DEVICE, the focus it has already is NO_CHANGE (and a
//      refused result carries FOXAPI_RESULT_REFUSED); the grant needed is VIEW (a session that was given no other grant may set its focus);
//   E  THE PER-RADIO READS, behind FOXAPI_HAS: read_radio_state(1) and (0) are read_state; radio 2 is NO_DEVICE; read_radio_spectrum(1) and (0) read the
//      spectrum frame read_spectrum reads, radio 2 is NO_DEVICE; a read_state through the focus is the same figures.
//
// B is the break-it check of the stage: with the target handling removed (the engine and the host answering OK for every target) B and C turn red.
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
    static float volume(Engine& e) { return e.volume_; }
    static double squelch(Engine& e) { return e.squelchDb_; }
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_radios_" + std::to_string(pid));
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

void testTheFacts() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
    // THE LIMIT IS A FACT: the engine's constant, not a number a client knows.
    CHECK_EQ(fact(s, "radio.max"), std::to_string(cascade::engine::kMaxReceivers));
    CHECK_EQ(fact(s, "radio.max"), std::string("8"));
    CHECK_EQ(fact(s, "radio.1.state"), std::string("stopped"));
    CHECK_EQ(fact(s, "radio.1.audio"), std::string("both"));
    CHECK_EQ(fact(s, "radio.1.device"), std::string());
    CHECK_EQ(fact(s, "radio.1.label"), std::string("Signal generator"));
    CHECK_EQ(fact(s, "radio.1.plugins"), std::string());
    // A radio that is not there has no facts at all.
    CHECK_EQ(fact(s, "radio.2.state"), std::string("<absent>"));
    CHECK_EQ(fact(s, "radio.2.device"), std::string("<absent>"));
    CHECK_EQ(fact(s, "radio.8.state"), std::string("<absent>"));
    // The state follows the receiver.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.1.state") == "running"; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.1.state") == "stopped"; }));
}

void testTheTarget() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 2);
    const float before = Access::volume(d.eng);
    CHECK(before != 0.25f);

    // A radio that is not there: refused at the submit, NO_DEVICE, and nothing changed.
    testing::Outcome r = pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 2, 0.25));
    CHECK_EQ(r.status(), FOXAPI_NO_DEVICE);
    CHECK(r.landed);
    CHECK((r.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK(std::string(r.result.message).find("no radio with that number") != std::string::npos);
    passes(d.eng, 2);
    CHECK_EQ(Access::volume(d.eng), before);
    // ...the first number past the engine's limit too, and the largest number a target holds below ALL.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, static_cast<std::uint32_t>(cascade::engine::kMaxReceivers) + 1u, 0.25)).status(),
             FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 0xFFFFFFFEu, 0.25)).status(), FOXAPI_NO_DEVICE);
    // The same through other ops that address a radio: a device control, the source, a recording, a tune.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_FREQUENCY, 3, 100.0e6)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_SAMPLE_RATE, 2, 2.4e6)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_AUDIO, 2, 0.0, 1)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SELECT_SOURCE, 2)).status(), FOXAPI_NO_DEVICE);

    // Target 0 (the focus, radio 1) and target 1 land exactly as a command with no target always has.
    r = pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 0, 0.25));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(Access::volume(d.eng), 0.25f);
    r = pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 1, 0.75));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(Access::volume(d.eng), 0.75f);
    r = pumped(d.eng, s, aimed(FOXAPI_OP_SET_SQUELCH, 1, -61.0));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(Access::squelch(d.eng), -61.0);

    // ALL_RADIOS: RUN alone.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RUNNING) != 0u; }));
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS, 0.0, 0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RUNNING) == 0u; }));
    r = pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, FOXAPI_TARGET_ALL_RADIOS, 0.1));
    CHECK_EQ(r.status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(Access::volume(d.eng), 0.75f);

    // An op that is not about a radio ignores the target, whatever it says.
    for (const std::uint32_t target : {2u, 9u, FOXAPI_TARGET_ALL_RADIOS}) {
        FoxCommand c = testing::cmd(FOXAPI_OP_BOOKMARK_ADD, 0.0, 0, "from a target");
        c.target = target;
        CHECK_EQ(pumped(d.eng, s, c).status(), FOXAPI_OK);
    }
}

void testTheLocalCommandPath() {
    // A command that does not come from a session (the window's own queue) is held to the same rule where it is applied.
    Direct d;
    const FoxCommandResult refused = d.eng.applyCommand(aimed(FOXAPI_OP_SET_VOLUME, 2, 0.4));
    CHECK_EQ(refused.status, FOXAPI_NO_DEVICE);
    CHECK((refused.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK(Access::volume(d.eng) != 0.4f);
    const FoxCommandResult all = d.eng.applyCommand(aimed(FOXAPI_OP_SET_VOLUME, FOXAPI_TARGET_ALL_RADIOS, 0.4));
    CHECK_EQ(all.status, FOXAPI_BAD_ARGUMENT);
    const FoxCommandResult ok = d.eng.applyCommand(aimed(FOXAPI_OP_SET_VOLUME, 1, 0.4));
    CHECK_EQ(ok.status, FOXAPI_OK);
    CHECK_EQ(Access::volume(d.eng), 0.4f);
    const FoxCommandResult none = d.eng.applyCommand(aimed(FOXAPI_OP_SET_VOLUME, 0, 0.45));
    CHECK_EQ(none.status, FOXAPI_OK);
}

void testTheFocus() {
    Direct d;
    // A session given the VIEW grant and nothing else.
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    CHECK(s.ok());
    passes(d.eng, 2);
    // Out of range: a number the engine does not number.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 0)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, static_cast<long long>(cascade::engine::kMaxReceivers) + 1)).status(),
             FOXAPI_OUT_OF_RANGE);
    // A radio the engine does not have.
    testing::Outcome r = pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 2));
    CHECK_EQ(r.status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, static_cast<long long>(cascade::engine::kMaxReceivers))).status(), FOXAPI_NO_DEVICE);
    // The focus it has already: nothing changes, it says so, and a refused result is marked as one.
    r = pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 1));
    CHECK(r.landed);
    CHECK_EQ(r.result.status, FOXAPI_NO_CHANGE);
    CHECK_EQ(r.result.applied[0], 1.0);
    // The state a session reads is the same before and after: no focus change happened, so no seq group moved by it.
    FoxReceiverState a = s.state();
    passes(d.eng, 3);
    FoxReceiverState b = s.state();
    CHECK_EQ(a.tuneSeq, b.tuneSeq);
    CHECK_EQ(a.deviceSeq, b.deviceSeq);
}

void testThePerRadioReads() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK(FOXAPI_HAS(api(), read_radio_state));
    CHECK(FOXAPI_HAS(api(), read_radio_spectrum));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RUNNING) != 0u; }));
    passes(d.eng, 3);

    const FoxReceiverState base = s.state();
    FoxReceiverState one{};
    one.structSize = sizeof(one);
    FoxReceiverState zero{};
    zero.structSize = sizeof(zero);
    FoxReceiverState two{};
    two.structSize = sizeof(two);
    CHECK_EQ(api()->read_radio_state(s.raw(), 1, &one), FOXAPI_OK);
    CHECK_EQ(api()->read_radio_state(s.raw(), 0, &zero), FOXAPI_OK);
    CHECK_EQ(api()->read_radio_state(s.raw(), 2, &two), FOXAPI_NO_DEVICE);
    CHECK_EQ(api()->read_radio_state(s.raw(), static_cast<std::uint32_t>(cascade::engine::kMaxReceivers) + 1u, &two), FOXAPI_NO_DEVICE);
    CHECK_EQ(api()->read_radio_state(s.raw(), 1, nullptr), FOXAPI_BAD_ARGUMENT);
    // Radio 1, the focus (0) and read_state are one radio: the same frequency, mode, run state and grants.
    CHECK_EQ(one.centreHz, base.centreHz);
    CHECK_EQ(zero.centreHz, base.centreHz);
    CHECK_EQ(one.demodMode, base.demodMode);
    CHECK_EQ(one.flags & FOXAPI_RX_RUNNING, base.flags & FOXAPI_RX_RUNNING);
    CHECK_EQ(one.grants, base.grants);
    CHECK_EQ(one.listSeq, zero.listSeq);

    // The spectrum: radio 1 and the focus read the frame read_spectrum reads; radio 2 is NO_DEVICE.
    std::vector<float> bins(4096);
    FoxSpectrumInfo i1{};
    i1.structSize = sizeof(i1);
    FoxSpectrumInfo i0{};
    i0.structSize = sizeof(i0);
    FoxSpectrumInfo ig{};
    ig.structSize = sizeof(ig);
    FoxSpectrumInfo i2{};
    i2.structSize = sizeof(i2);
    CHECK(passUntil(d.eng, [&] { return api()->read_radio_spectrum(s.raw(), 1, 0, &i1, bins.data(), static_cast<std::uint32_t>(bins.size())) == FOXAPI_OK; }));
    CHECK(i1.binCount > 0u);
    CHECK(i1.copied > 0u);
    CHECK_EQ(api()->read_radio_spectrum(s.raw(), 0, 0, &i0, bins.data(), static_cast<std::uint32_t>(bins.size())), FOXAPI_OK);
    CHECK_EQ(api()->read_spectrum(s.raw(), 0, &ig, bins.data(), static_cast<std::uint32_t>(bins.size())), FOXAPI_OK);
    CHECK_EQ(i0.binCount, ig.binCount);
    CHECK_EQ(i0.centreHz, ig.centreHz);
    CHECK_EQ(api()->read_radio_spectrum(s.raw(), 2, 0, &i2, bins.data(), static_cast<std::uint32_t>(bins.size())), FOXAPI_NO_DEVICE);
    CHECK_EQ(api()->read_radio_spectrum(s.raw(), static_cast<std::uint32_t>(cascade::engine::kMaxReceivers) + 1u, 0, &i2, bins.data(),
                                        static_cast<std::uint32_t>(bins.size())),
             FOXAPI_NO_DEVICE);
    CHECK_EQ(api()->read_radio_spectrum(s.raw(), 1, 0, nullptr, bins.data(), static_cast<std::uint32_t>(bins.size())), FOXAPI_BAD_ARGUMENT);
}

}  // namespace

int main() {
    std::printf("test_engine_api_radios\n");
    isolate();
    testTheFacts();
    testTheTarget();
    testTheLocalCommandPath();
    testTheFocus();
    testThePerRadioReads();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_radios");
}
