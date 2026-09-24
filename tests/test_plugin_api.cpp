// Tests for HOST API LEVEL 1 (plugin_abi.h, "HOST API LEVEL 1") - every
// function a plugin reaches through CascadeHostApi beyond the original four,
// and the host machinery behind them (core/plugin_api.hpp, the trampolines in
// core/plugin_ui.cpp, the processor chain in core/plugin_runner.cpp, the
// capability check in core/plugin_host.cpp, the patch runner, the config).
//
// THE PLUGINS HERE ARE FAKES WRITTEN TO THE REAL C ABI: a CascadeHostClientApi
// whose attach() keeps the table it is handed, exactly as a DLL would, and
// every call below goes THROUGH THAT TABLE - the same function pointers, the
// same ctx - so what is tested is what a plugin gets, not a C++ shortcut
// around it. The published plugin binaries themselves are loaded by
// tests/test_plugin_abi3_compat.cpp.
//
// THE FAILURE PATHS the task names, each with its own section:
//   [W] called on the wrong thread       - the settings store refuses the
//                                          real-time thread; everything else
//                                          is queued, never applied there
//   [V] invalid values                   - NaN, out of range, malformed
//                                          structs, bad keys, bad UTF-8
//   [U] plugin unloaded mid-call         - calls racing clear() and the
//                                          owner's destruction; DETACHED after
//   [O] an old plugin with no knowledge  - the four original members at their
//       of the table                       original offsets, and a new plugin
//                                          on an old host degrading
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_runner.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "gui/plugin_markers.hpp"
#include "gui/plugins_view.hpp"
#include "test_check.hpp"

using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginApiCore;
using cascade::core::PluginControl;
using cascade::core::PluginRejection;
using cascade::core::PluginRunner;
using cascade::core::PluginUi;
using cascade::core::ReceiverFacts;
using cascade::core::RealtimeThreadScope;

namespace fs = std::filesystem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// ---------------------------------------------------------------------------
// Fake plugins: up to four, each with its own attach() that keeps its table
// ---------------------------------------------------------------------------

struct Captured {
    const CascadeHostApi* api = nullptr;
    int attaches = 0;
    // What the plugin got when it asked from INSIDE attach() - the natural
    // place to read one's settings and the receiver.
    std::int32_t stateInAttach = 99;
    std::int32_t settingsInAttach = 99;
};
Captured g_cap[4];

template <int I>
void attachI(const CascadeHostApi* h) {
    Captured& c = g_cap[I];
    c.api = h;
    ++c.attaches;
    if (CASCADE_HOST_HAS(h, get_state)) {
        CascadeReceiverState s{};
        s.structSize = sizeof(s);
        c.stateInAttach = h->get_state(h->ctx, &s);
        char buf[16];
        c.settingsInAttach = h->settings_get(h->ctx, "seen", buf, sizeof(buf));
    }
}

const CascadeHostClientApi kHc[4] = {
    {sizeof(CascadeHostClientApi), &attachI<0>},
    {sizeof(CascadeHostClientApi), &attachI<1>},
    {sizeof(CascadeHostClientApi), &attachI<2>},
    {sizeof(CascadeHostClientApi), &attachI<3>},
};

LoadedPlugin plug(int i, const char* name) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.capabilities = CASCADE_CAP_HOST_CLIENT;
    p.hostClient = &kHc[i];
    return p;
}

std::string keyOf(const char* name) { return std::string(name) + ".dll"; }

const CascadeHostApi* H(int i) { return g_cap[i].api; }
void* C(int i) { return g_cap[i].api != nullptr ? g_cap[i].api->ctx : nullptr; }

void resetCaps() {
    for (Captured& c : g_cap) { c = Captured{}; }
}

struct ServiceLog {
    std::atomic<int> centre{0}, rate{0}, tune{0}, time{0};
};

HostServices services(ServiceLog* log) {
    HostServices s;
    s.centreHz = [log] {
        ++log->centre;
        return 145.0e6;
    };
    s.sampleRateHz = [log] {
        ++log->rate;
        return 2.4e6;
    };
    s.tune = [log](double) {
        ++log->tune;
        return static_cast<std::int32_t>(CASCADE_TUNE_OK);
    };
    s.unixTimeMs = [log] {
        ++log->time;
        return static_cast<std::int64_t>(1'700'000'000'000LL);
    };
    return s;
}

ReceiverFacts facts() {
    ReceiverFacts f;
    f.running = true;
    f.deviceOpen = true;
    f.agcSupported = true;
    f.centreHz = 145.0e6;
    f.vfoOffsetHz = 12500.0;
    f.sampleRateHz = 2.4e6;
    f.bandwidthHz = 12500.0;
    f.squelchDb = -60.0;
    f.volume = 0.5;
    f.signalDb = -30.0;
    f.demodMode = CASCADE_DEMOD_NFM;
    std::snprintf(f.deviceName, sizeof(f.deviceName), "%s", "RTL-SDR Blog V4");
    f.gainCount = 2;
    std::snprintf(f.gains[0].name, sizeof(f.gains[0].name), "%s", "LNA");
    f.gains[0].minDb = 0.0;
    f.gains[0].maxDb = 49.6;
    f.gains[0].stepDb = 0.1;
    f.gains[0].currentDb = 20.0;
    std::snprintf(f.gains[1].name, sizeof(f.gains[1].name), "%s", "VGA");
    f.gains[1].unit = CASCADE_GAIN_UNIT_STEPS;
    f.gains[1].minDb = 0.0;
    f.gains[1].maxDb = 15.0;
    f.gains[1].stepDb = 1.0;
    f.gains[1].currentDb = 7.0;
    f.rateCount = 3;
    f.rates[0] = 1.024e6;
    f.rates[1] = 2.048e6;
    f.rates[2] = 2.4e6;
    f.outputRateHz = 48000.0;
    f.outputFrames = 480000;
    return f;
}

CascadeReceiverState state(int i, std::int32_t* rc = nullptr) {
    CascadeReceiverState s{};
    s.structSize = sizeof(s);
    const std::int32_t r = H(i)->get_state(C(i), &s);
    if (rc != nullptr) { *rc = r; }
    return s;
}

// Runs `f` on a thread and gives up after `ms`: a deadlocked host must fail
// the test, not hang the suite. The thread is detached on a timeout (the
// process exits with a failure straight after), joined otherwise.
bool finishesWithin(int ms, const std::function<void()>& f) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        f();
        done.store(true);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done.load() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!done.load()) {
        std::printf("FAIL: did not finish within %d ms (deadlock?)\n", ms);
        std::fflush(stdout);
        std::_Exit(1);
    }
    t.join();
    return true;
}

// ===========================================================================
// [O] Discovery, and the two compatibility directions
// ===========================================================================
//
// THE LAYOUT AN OLD PLUGIN WAS BUILT AGAINST, spelled out as its own struct.
// This is literally the CascadeHostApi of every header before level 1; an old
// plugin reads the table the host hands it through THIS shape.
struct OldHostApi {
    uint32_t structSize;
    void* ctx;
    double (*centre_hz)(void* ctx);
    double (*sample_rate_hz)(void* ctx);
    int32_t (*request_tune)(void* ctx, double centreHz);
    int64_t (*unix_time_ms)(void* ctx);
};
static_assert(offsetof(CascadeHostApi, ctx) == offsetof(OldHostApi, ctx), "ctx moved");
static_assert(offsetof(CascadeHostApi, centre_hz) == offsetof(OldHostApi, centre_hz),
              "centre_hz moved");
static_assert(offsetof(CascadeHostApi, sample_rate_hz) == offsetof(OldHostApi, sample_rate_hz),
              "sample_rate_hz moved");
static_assert(offsetof(CascadeHostApi, request_tune) == offsetof(OldHostApi, request_tune),
              "request_tune moved");
static_assert(offsetof(CascadeHostApi, unix_time_ms) == offsetof(OldHostApi, unix_time_ms),
              "unix_time_ms moved");
// Level 1 starts exactly where the old table ended: nothing was inserted.
static_assert(offsetof(CascadeHostApi, apiLevel) == sizeof(OldHostApi),
              "level 1 must be APPENDED after unix_time_ms");

void testDiscovery() {
    std::printf("[O] discovery and compatibility\n");
    resetCaps();
    ServiceLog log;
    PluginUi ui;
    ui.setServices(services(&log));
    ui.rebuild({plug(0, "New")});
    const CascadeHostApi* h = H(0);
    CHECK(h != nullptr);
    if (h == nullptr) { return; }

    // The table IS its version: structSize is the host's own sizeof.
    CHECK(h->structSize == sizeof(CascadeHostApi));
    CHECK(h->apiLevel == CASCADE_HOST_API_LEVEL);
    CHECK(h->knownCapabilities == CASCADE_CAP_ALL_KNOWN);
    CHECK((h->knownCapabilities & CASCADE_CAP_AUDIO_PROCESSOR) != 0u);
    CHECK(h->hostName != nullptr && std::strcmp(h->hostName, "FoxSDR") == 0);
    CHECK(h->hostVersion != nullptr && h->hostVersion[0] != '\0');
    // Every level-1 function is provided (a level-1 host fills all it covers).
    CHECK(CASCADE_HOST_HAS(h, get_state));
    CHECK(CASCADE_HOST_HAS(h, get_gain));
    CHECK(CASCADE_HOST_HAS(h, get_sample_rates));
    CHECK(CASCADE_HOST_HAS(h, get_stream_info));
    CHECK(CASCADE_HOST_HAS(h, set_frequency));
    CHECK(CASCADE_HOST_HAS(h, set_vfo_offset));
    CHECK(CASCADE_HOST_HAS(h, set_mode));
    CHECK(CASCADE_HOST_HAS(h, set_bandwidth));
    CHECK(CASCADE_HOST_HAS(h, set_squelch));
    CHECK(CASCADE_HOST_HAS(h, set_sample_rate));
    CHECK(CASCADE_HOST_HAS(h, set_gain));
    CHECK(CASCADE_HOST_HAS(h, set_device_agc));
    CHECK(CASCADE_HOST_HAS(h, set_running));
    CHECK(CASCADE_HOST_HAS(h, set_volume));
    CHECK(CASCADE_HOST_HAS(h, set_muted));
    CHECK(CASCADE_HOST_HAS(h, set_marker));
    CHECK(CASCADE_HOST_HAS(h, remove_marker));
    CHECK(CASCADE_HOST_HAS(h, clear_markers));
    CHECK(CASCADE_HOST_HAS(h, settings_get));
    CHECK(CASCADE_HOST_HAS(h, settings_set));
    CHECK(CASCADE_HOST_HAS(h, log));
    CHECK(CASCADE_HOST_HAS(h, add_command));
    CHECK(CASCADE_HOST_HAS(h, remove_command));
    CHECK(CASCADE_HOST_HAS(h, poll_command));

    // [O1] AN OLD PLUGIN, reading the table through the only layout it knows,
    // gets the four functions it always had - pointing at the host, working.
    const auto* old = reinterpret_cast<const OldHostApi*>(h);
    CHECK(old->ctx == h->ctx);
    CHECK(old->centre_hz(old->ctx) == 145.0e6);
    CHECK(old->sample_rate_hz(old->ctx) == 2.4e6);
    CHECK(old->unix_time_ms(old->ctx) == 1'700'000'000'000LL);
    // The grant rule is exactly the old one: refused until granted.
    CHECK(old->request_tune(old->ctx, 146.0e6) == CASCADE_TUNE_DENIED);
    ui.setTuneAllowed(keyOf("New"), true);
    CHECK(old->request_tune(old->ctx, 146.0e6) == CASCADE_TUNE_OK);
    CHECK(log.tune.load() == 1);
    // ...and the structSize it may check is at least what it compiled.
    CHECK(old->structSize >= sizeof(OldHostApi));

    // [O2] A NEW PLUGIN ON AN OLD HOST: the host hands it a table whose
    // structSize ends at unix_time_ms. CASCADE_HOST_HAS must say "no" for
    // every level-1 member, so the plugin never calls through memory the old
    // host never filled - while the four originals still answer "yes".
    CascadeHostApi oldHost{};
    std::memcpy(&oldHost, h, sizeof(OldHostApi));
    oldHost.structSize = static_cast<uint32_t>(sizeof(OldHostApi));
    CHECK(CASCADE_HOST_HAS(&oldHost, unix_time_ms));
    CHECK(CASCADE_HOST_HAS(&oldHost, request_tune));
    CHECK(!CASCADE_HOST_COVERS(&oldHost, apiLevel));
    CHECK(!CASCADE_HOST_HAS(&oldHost, get_state));
    CHECK(!CASCADE_HOST_HAS(&oldHost, poll_command));
    // A NULL table is "no" too, never a crash.
    const CascadeHostApi* none = nullptr;
    CHECK(!CASCADE_HOST_HAS(none, get_state));

    // [O3] AND AN OLD DESCRIPTOR still validates exactly as before: a plugin
    // with no knowledge of level 1 declares none of its bits.
    static const CascadeDecoderApi dec = {sizeof(CascadeDecoderApi), 0u,
                                          [](uint32_t) -> void* { return &g_cap[3]; },
                                          [](void*, const float*, size_t) {},
                                          [](void*, char*, size_t) -> int32_t { return 0; },
                                          [](void*) {}};
    static const CascadeCapabilityEntry caps[] = {
        {CASCADE_CAP_DECODER, sizeof(CascadeDecoderApi), &dec},
        {CASCADE_CAP_HOST_CLIENT, sizeof(CascadeHostClientApi), &kHc[3]},
    };
    CascadePluginDesc d{};
    d.structSize = sizeof(CascadePluginDesc);
    d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    d.name = "Old";
    d.version = "1.0.0";
    d.author = "";
    d.licence = "MIT";
    d.capabilities = CASCADE_CAP_DECODER | CASCADE_CAP_HOST_CLIENT;
    d.capabilityCount = 2;
    d.capabilityTables = caps;
    CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::None);
}
// The ABI did not move: every one of the 27 published plugins says 3.
static_assert(CASCADE_PLUGIN_ABI_VERSION == 3, "level 1 is additive - no ABI bump");

// ===========================================================================
// Receiver state, gains, and the change counters
// ===========================================================================

void testState() {
    std::printf("state\n");
    resetCaps();
    ServiceLog log;
    PluginUi ui;
    ui.setServices(services(&log));
    ui.rebuild({plug(0, "Reader")});
    CHECK(H(0) != nullptr);
    if (H(0) == nullptr) { return; }

    // Before anything is published: OK, everything zero - a counter of zero is
    // documented as "never read", so a plugin can tell.
    std::int32_t rc = 0;
    CascadeReceiverState s = state(0, &rc);
    CHECK(rc == CASCADE_API_OK);
    CHECK(s.seq == 0u);

    ui.api().publish(facts());
    s = state(0, &rc);
    CHECK(rc == CASCADE_API_OK);
    CHECK(s.structSize == sizeof(CascadeReceiverState));
    CHECK(s.centreHz == 145.0e6);
    CHECK(s.vfoOffsetHz == 12500.0);
    CHECK(s.tunedHz == 145.0125e6);
    CHECK(s.sampleRateHz == 2.4e6);
    CHECK(s.bandwidthHz == 12500.0);
    CHECK(s.squelchDb == -60.0);
    CHECK(s.volume == 0.5);
    CHECK(s.signalDb == -30.0);
    CHECK_NEAR(s.sMeter, 0.75, 1e-9);  // -30 dB over the host's [-120, 0]
    CHECK(s.demodMode == CASCADE_DEMOD_NFM);
    CHECK(s.gainCount == 2u);
    CHECK(std::strcmp(s.deviceName, "RTL-SDR Blog V4") == 0);
    CHECK((s.flags & CASCADE_STATE_RUNNING) != 0u);
    CHECK((s.flags & CASCADE_STATE_DEVICE_OPEN) != 0u);
    CHECK((s.flags & CASCADE_STATE_AGC_SUPPORTED) != 0u);
    CHECK((s.flags & CASCADE_STATE_SQUELCH_OPEN) != 0u);  // -30 > -60
    CHECK((s.flags & CASCADE_STATE_TUNE_GRANTED) == 0u);
    CHECK((s.flags & CASCADE_STATE_SETTINGS_GRANTED) == 0u);
    CHECK(s.seq >= 1u && s.tuneSeq >= 1u && s.modeSeq >= 1u && s.deviceSeq >= 1u &&
          s.audioSeq >= 1u);

    // The grants are THIS plugin's, read lock-free on every call.
    ui.setTuneAllowed(keyOf("Reader"), true);
    ui.setSettingsAllowed(keyOf("Reader"), true);
    s = state(0);
    CHECK((s.flags & CASCADE_STATE_TUNE_GRANTED) != 0u);
    CHECK((s.flags & CASCADE_STATE_SETTINGS_GRANTED) != 0u);

    // CHANGE COUNTERS SAY WHICH GROUP MOVED, and a measurement moves none.
    const CascadeReceiverState before = s;
    ReceiverFacts f = facts();
    f.signalDb = -80.0;          // measurement only
    f.outputFrames = 999999;     // measurement only
    ui.api().publish(f);
    s = state(0);
    CHECK(s.seq == before.seq);
    CHECK(s.signalDb == -80.0);
    CHECK((s.flags & CASCADE_STATE_SQUELCH_OPEN) == 0u);  // -80 < -60
    f.centreHz = 146.0e6;        // tune group
    ui.api().publish(f);
    s = state(0);
    CHECK(s.tuneSeq == before.tuneSeq + 1u);
    CHECK(s.modeSeq == before.modeSeq);
    CHECK(s.deviceSeq == before.deviceSeq);
    CHECK(s.audioSeq == before.audioSeq);
    CHECK(s.seq == before.seq + 1u);
    f.demodMode = CASCADE_DEMOD_AM;  // mode group
    ui.api().publish(f);
    s = state(0);
    CHECK(s.modeSeq == before.modeSeq + 1u);
    CHECK(s.tuneSeq == before.tuneSeq + 1u);
    f.gains[0].currentDb = 30.0;  // device group, through a gain readback
    ui.api().publish(f);
    s = state(0);
    CHECK(s.deviceSeq == before.deviceSeq + 1u);
    f.muted = true;  // audio group
    ui.api().publish(f);
    s = state(0);
    CHECK(s.audioSeq == before.audioSeq + 1u);
    CHECK((s.flags & CASCADE_STATE_MUTED) != 0u);
    // An identical publish moves nothing at all.
    const CascadeReceiverState same = s;
    ui.api().publish(f);
    s = state(0);
    CHECK(s.seq == same.seq);

    // Gains, by index, with the readback and the unit.
    CascadeGainInfo g{};
    g.structSize = sizeof(g);
    CHECK(H(0)->get_gain(C(0), 0, &g) == CASCADE_API_OK);
    CHECK(std::strcmp(g.name, "LNA") == 0);
    CHECK(g.currentDb == 30.0);
    CHECK(g.unit == CASCADE_GAIN_UNIT_DB);
    CHECK(H(0)->get_gain(C(0), 1, &g) == CASCADE_API_OK);
    CHECK(std::strcmp(g.name, "VGA") == 0);
    CHECK(g.unit == CASCADE_GAIN_UNIT_STEPS);
    CHECK(H(0)->get_gain(C(0), 2, &g) == CASCADE_API_NOT_FOUND);

    // The radio's rates: the count is always the whole list, the copy is
    // bounded by cap, and cap 0 only asks.
    double rates[8] = {};
    CHECK(H(0)->get_sample_rates(C(0), nullptr, 0) == 3);
    CHECK(H(0)->get_sample_rates(C(0), rates, 8) == 3);
    CHECK(rates[0] == 1.024e6 && rates[1] == 2.048e6 && rates[2] == 2.4e6 && rates[3] == 0.0);
    double two[2] = {};
    CHECK(H(0)->get_sample_rates(C(0), two, 2) == 3);
    CHECK(two[1] == 2.048e6);
    CHECK(H(0)->get_sample_rates(C(0), nullptr, 4) == CASCADE_API_BAD_ARGUMENT);
    // A new list moves the device counter.
    const std::uint64_t devBefore = state(0).deviceSeq;
    f.rates[2] = 3.2e6;
    ui.api().publish(f);
    CHECK(state(0).deviceSeq == devBefore + 1u);
    // No radio, no rates.
    ReceiverFacts nodev = f;
    nodev.deviceOpen = false;
    ui.api().publish(nodev);
    CHECK(H(0)->get_sample_rates(C(0), rates, 8) == 0);
    ui.api().publish(f);

    // [V] Malformed arguments: refused, nothing written.
    CHECK(H(0)->get_state(C(0), nullptr) == CASCADE_API_BAD_ARGUMENT);
    CascadeReceiverState small{};
    small.structSize = 8;  // shorter than level 1's struct
    CHECK(H(0)->get_state(C(0), &small) == CASCADE_API_BAD_ARGUMENT);
    CHECK(small.seq == 0u);
    CHECK(H(0)->get_gain(C(0), 0, nullptr) == CASCADE_API_BAD_ARGUMENT);
    CascadeGainInfo gsmall{};
    gsmall.structSize = 4;
    CHECK(H(0)->get_gain(C(0), 0, &gsmall) == CASCADE_API_BAD_ARGUMENT);
    CascadeStreamInfo si{};
    si.structSize = 3;
    CHECK(H(0)->get_stream_info(C(0), &si) == CASCADE_API_BAD_ARGUMENT);
    CHECK(H(0)->get_stream_info(C(0), nullptr) == CASCADE_API_BAD_ARGUMENT);

    // A LARGER struct than the host knows (a plugin built against a newer
    // header): the host fills what it knows, says so in structSize, and does
    // not touch the plugin's trailing bytes.
    struct Bigger {
        CascadeReceiverState s;
        std::uint64_t future;
    } big{};
    big.s.structSize = sizeof(Bigger);
    big.future = 0xABCDEF;
    CHECK(H(0)->get_state(C(0), &big.s) == CASCADE_API_OK);
    CHECK(big.s.structSize == sizeof(CascadeReceiverState));
    CHECK(big.future == 0xABCDEFu);
    CHECK(big.s.centreHz == 146.0e6);
}

// ===========================================================================
// Receiver control: grants, validation, queueing
// ===========================================================================

std::vector<PluginControl> drain(PluginUi& ui) {
    std::vector<PluginControl> v;
    ui.api().takeControls(v);
    return v;
}

void testControls() {
    std::printf("controls\n");
    resetCaps();
    ServiceLog log;
    PluginUi ui;
    ui.setServices(services(&log));
    ui.rebuild({plug(0, "Ctl"), plug(1, "Other"), plug(2, "Quiet")});
    CHECK(H(0) != nullptr && H(1) != nullptr && H(2) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr || H(2) == nullptr) { return; }
    ui.api().publish(facts());
    const CascadeHostApi* h = H(0);
    void* c = C(0);

    // DENIED without a grant - and nothing queued.
    CHECK(h->set_frequency(c, 146.0e6) == CASCADE_API_DENIED);
    CHECK(h->set_mode(c, CASCADE_DEMOD_AM) == CASCADE_API_DENIED);
    CHECK(drain(ui).empty());
    // Asking is recorded for the SETTINGS key (offered only where it means
    // something), and only the plugin that asked.
    {
        const std::vector<std::string> askers = ui.api().settingsRequesters();
        CHECK(askers.size() == 1u && !askers.empty() && askers[0] == keyOf("Ctl"));
    }

    // THE TWO GRANTS ARE SEPARATE, which is the whole point of having two:
    // the tune grant does not permit settings, and vice versa.
    ui.setTuneAllowed(keyOf("Ctl"), true);
    CHECK(h->set_frequency(c, 146.0e6) == CASCADE_API_OK);
    CHECK(h->set_vfo_offset(c, -5000.0) == CASCADE_API_OK);
    CHECK(h->set_mode(c, CASCADE_DEMOD_AM) == CASCADE_API_DENIED);
    CHECK(h->set_volume(c, 0.2) == CASCADE_API_DENIED);
    ui.setTuneAllowed(keyOf("Ctl"), false);
    ui.setSettingsAllowed(keyOf("Ctl"), true);
    CHECK(h->set_frequency(c, 146.0e6) == CASCADE_API_DENIED);
    CHECK(h->set_mode(c, CASCADE_DEMOD_AM) == CASCADE_API_OK);
    CHECK(h->set_bandwidth(c, 8000.0) == CASCADE_API_OK);
    CHECK(h->set_squelch(c, -70.0) == CASCADE_API_OK);
    CHECK(h->set_sample_rate(c, 2.048e6) == CASCADE_API_OK);
    CHECK(h->set_gain(c, "LNA", 25.0) == CASCADE_API_OK);
    CHECK(h->set_device_agc(c, 1) == CASCADE_API_OK);
    CHECK(h->set_running(c, 0) == CASCADE_API_OK);
    CHECK(h->set_volume(c, 0.25) == CASCADE_API_OK);
    CHECK(h->set_muted(c, 1) == CASCADE_API_OK);
    // The other plugin's grant is its own.
    CHECK(H(1)->set_volume(C(1), 0.5) == CASCADE_API_DENIED);

    // QUEUED, not applied: in order, each carrying its client and its value.
    std::vector<PluginControl> q = drain(ui);
    CHECK(q.size() == 11u);
    using K = PluginControl::Kind;
    const K want[] = {K::Frequency, K::VfoOffset, K::Mode,    K::Bandwidth,
                      K::Squelch,   K::SampleRate, K::Gain,   K::DeviceAgc,
                      K::Running,   K::Volume,     K::Muted};
    for (std::size_t i = 0; i < q.size() && i < 11u; ++i) { CHECK(q[i].kind == want[i]); }
    if (q.size() == 11u) {
        CHECK(q[0].value == 146.0e6);
        CHECK(q[1].value == -5000.0);
        CHECK(q[2].mode == CASCADE_DEMOD_AM);
        CHECK(std::strcmp(q[6].gainName, "LNA") == 0 && q[6].value == 25.0);
        CHECK(q[7].flag == true);
        CHECK(q[8].flag == false);
        CHECK(q[9].value == 0.25);
        CHECK(ui.api().clientKey(q[0].client) == keyOf("Ctl"));
    }
    CHECK(drain(ui).empty());  // taking empties it

    // [V] INVALID VALUES - each answer names the fault, nothing is queued.
    ui.setTuneAllowed(keyOf("Ctl"), true);
    CHECK(h->set_frequency(c, kNaN) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_frequency(c, kInf) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_frequency(c, 0.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_frequency(c, -1.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_frequency(c, 2e12) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_vfo_offset(c, kNaN) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_vfo_offset(c, 2e8) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_mode(c, CASCADE_DEMOD_UNCHANGED) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_mode(c, CASCADE_DEMOD_RAW + 1u) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_bandwidth(c, 10.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_bandwidth(c, 5e7) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_bandwidth(c, kNaN) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_squelch(c, -500.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_squelch(c, 50.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_sample_rate(c, 1.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_sample_rate(c, 1e9) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_volume(c, -0.1) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_volume(c, 1.5) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_volume(c, kNaN) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_gain(c, nullptr, 10.0) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_gain(c, "", 10.0) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->set_gain(c, "NOPE", 10.0) == CASCADE_API_UNSUPPORTED);
    CHECK(h->set_gain(c, "LNA", 100.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_gain(c, "LNA", -3.0) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->set_gain(c, "LNA", kNaN) == CASCADE_API_BAD_ARGUMENT);
    // A name longer than any the host could publish is not a stage it has.
    CHECK(h->set_gain(c, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", 1.0) ==
          CASCADE_API_UNSUPPORTED);
    CHECK(drain(ui).empty());

    // WHAT NEEDS A RADIO says so without one; an AGC the radio lacks says so.
    ReceiverFacts nodev = facts();
    nodev.deviceOpen = false;
    nodev.gainCount = 0;
    ui.api().publish(nodev);
    CHECK(h->set_sample_rate(c, 2.4e6) == CASCADE_API_NO_DEVICE);
    CHECK(h->set_gain(c, "LNA", 10.0) == CASCADE_API_NO_DEVICE);
    CHECK(h->set_device_agc(c, 1) == CASCADE_API_NO_DEVICE);
    ReceiverFacts noagc = facts();
    noagc.agcSupported = false;
    ui.api().publish(noagc);
    CHECK(h->set_device_agc(c, 1) == CASCADE_API_UNSUPPORTED);
    ui.api().publish(facts());

    // BUSY WHEN THE QUEUE IS FULL, never growing: exactly kControlQueue fit.
    for (std::size_t i = 0; i < PluginApiCore::kControlQueue; ++i) {
        if (h->set_volume(c, 0.5) != CASCADE_API_OK) {
            CHECK(false);
            break;
        }
    }
    CHECK(h->set_volume(c, 0.5) == CASCADE_API_BUSY);
    q = drain(ui);
    CHECK(q.size() == PluginApiCore::kControlQueue);
    CHECK(h->set_volume(c, 0.5) == CASCADE_API_OK);  // room again
    q = drain(ui);

    // RE-CHECKED AT APPLY TIME: a request queued under a grant that is revoked
    // before the frame, or by a plugin stopped before it, must not act.
    CHECK(h->set_volume(c, 0.9) == CASCADE_API_OK);
    CHECK(h->set_frequency(c, 150.0e6) == CASCADE_API_OK);
    q = drain(ui);
    CHECK(q.size() == 2u);
    if (q.size() == 2u) {
        CHECK(ui.api().controlStillAllowed(q[0]));
        CHECK(ui.api().controlStillAllowed(q[1]));
        ui.setSettingsAllowed(keyOf("Ctl"), false);
        CHECK(!ui.api().controlStillAllowed(q[0]));  // volume: settings grant gone
        CHECK(ui.api().controlStillAllowed(q[1]));   // frequency: tune grant kept
        ui.setStopped({keyOf("Ctl")});
        CHECK(!ui.api().controlStillAllowed(q[1]));
    }
    // A stopped plugin is refused outright.
    CHECK(h->set_frequency(c, 150.0e6) == CASCADE_API_DENIED);
    CHECK((state(0).flags & CASCADE_STATE_STOPPED) != 0u);

    // A STOP REMOVES WHAT IT HAD QUEUED, so nothing it asked for lands after
    // the user switched it off.
    ui.setStopped({});
    ui.setSettingsAllowed(keyOf("Ctl"), true);
    ui.setSettingsAllowed(keyOf("Other"), true);
    CHECK(h->set_volume(c, 0.3) == CASCADE_API_OK);
    CHECK(H(1)->set_volume(C(1), 0.4) == CASCADE_API_OK);
    ui.setStopped({keyOf("Ctl")});
    q = drain(ui);
    CHECK(q.size() == 1u);
    if (!q.empty()) { CHECK(ui.api().clientKey(q[0].client) == keyOf("Other")); }

    // A STOPPED PLUGIN IS REFUSED BEFORE ANYTHING ELSE: its values are not
    // even looked at (DENIED, not BAD_ARGUMENT), and it is NOT recorded as
    // asking - the host must never invite the user to grant settings to a
    // plugin they switched off.
    ui.setStopped({keyOf("Quiet")});
    CHECK(H(2)->set_mode(C(2), CASCADE_DEMOD_AM) == CASCADE_API_DENIED);
    CHECK(H(2)->set_frequency(C(2), kNaN) == CASCADE_API_DENIED);
    {
        const std::vector<std::string> askers = ui.api().settingsRequesters();
        bool quietAsked = false;
        for (const std::string& k : askers) { quietAsked = quietAsked || k == keyOf("Quiet"); }
        CHECK(!quietAsked);
    }
    CHECK(drain(ui).empty());
}

// ===========================================================================
// [W] Threads: the real-time thread, workers, and nothing ever waiting
// ===========================================================================

void testThreads() {
    std::printf("[W] threads\n");
    resetCaps();
    ServiceLog log;
    PluginUi ui;
    ui.setServices(services(&log));
    ui.rebuild({plug(0, "Thr")});
    CHECK(H(0) != nullptr);
    if (H(0) == nullptr) { return; }
    const CascadeHostApi* h = H(0);
    void* c = C(0);
    ui.api().publish(facts());
    ui.setSettingsAllowed(keyOf("Thr"), true);

    // THE SETTINGS STORE REFUSES THE REAL-TIME THREAD - it allocates, which
    // the DSP thread must never do - and says so by name.
    std::int32_t onRtSet = 0, onRtGet = 0, onRtLog = 0, onRtMark = 0, onRtState = 0,
                 onRtCtl = 0;
    std::thread rt([&] {
        const RealtimeThreadScope realtime;
        onRtSet = h->settings_set(c, "k", "v");
        char buf[8];
        onRtGet = h->settings_get(c, "k", buf, sizeof(buf));
        onRtLog = h->log(c, CASCADE_LOG_INFO, "from the DSP thread");
        CascadeMarker m{};
        m.structSize = sizeof(m);
        m.freqHz = 145.0e6;
        onRtMark = h->set_marker(c, &m);
        CascadeReceiverState s{};
        s.structSize = sizeof(s);
        onRtState = h->get_state(c, &s);
        onRtCtl = h->set_volume(c, 0.1);
    });
    rt.join();
    CHECK(onRtSet == CASCADE_API_WRONG_THREAD);
    CHECK(onRtGet == CASCADE_API_WRONG_THREAD);
    // ...and nothing else refuses it: those are fixed-size copies.
    CHECK(onRtLog == CASCADE_API_OK);
    CHECK(onRtMark == CASCADE_API_OK);
    CHECK(onRtState == CASCADE_API_OK);
    CHECK(onRtCtl == CASCADE_API_OK);
    // The marker is thread-local: a plain worker (no scope) may use the store.
    std::int32_t onWorker = 0;
    std::thread w([&] { onWorker = h->settings_set(c, "k", "v"); });
    w.join();
    CHECK(onWorker == CASCADE_API_OK);
    CHECK(!cascade::core::onRealtimeThread());

    // A REQUEST FROM ANY THREAD IS QUEUED, NEVER APPLIED THERE: the core has
    // no path to the receiver at all, so the only effect of a call on a
    // worker is an entry that the GUI thread drains.
    std::vector<PluginControl> q;
    ui.api().takeControls(q);
    CHECK(q.size() == 1u);  // the one set_volume from the real-time thread

    // READS NEVER TEAR AND NEVER WAIT: a writer publishing as fast as it can
    // while a real-time reader reads. Every snapshot must be ONE publish -
    // centre and offset are written as a pair here, so a torn read shows up
    // as a pair that does not match.
    std::atomic<bool> stop{false};
    std::atomic<long> torn{0}, reads{0}, busy{0};
    std::thread writer([&] {
        ReceiverFacts f = facts();
        double n = 1.0;
        while (!stop.load()) {
            f.centreHz = 100.0e6 + n;
            f.vfoOffsetHz = n;
            ui.api().publish(f);
            n += 1.0;
        }
    });
    finishesWithin(20000, [&] {
        const RealtimeThreadScope realtime;
        for (int i = 0; i < 200000; ++i) {
            CascadeReceiverState s{};
            s.structSize = sizeof(s);
            const std::int32_t r = h->get_state(c, &s);
            if (r == CASCADE_API_BUSY) {
                ++busy;
                continue;
            }
            ++reads;
            if (s.centreHz - 100.0e6 != s.vfoOffsetHz && s.vfoOffsetHz != 12500.0) { ++torn; }
        }
    });
    stop.store(true);
    writer.join();
    std::printf("  concurrent reads: %ld consistent, %ld torn, %ld busy\n", reads.load(),
                torn.load(), busy.load());
    CHECK(torn.load() == 0);
    CHECK(reads.load() > 0);
}

// ===========================================================================
// Spectrum / waterfall marks
// ===========================================================================

CascadeMarker marker(std::uint32_t id, double hz, std::uint32_t kind = CASCADE_MARKER_POINT,
                     double width = 0.0, const char* label = "") {
    CascadeMarker m{};
    m.structSize = sizeof(m);
    m.id = id;
    m.kind = kind;
    m.freqHz = hz;
    m.widthHz = width;
    std::snprintf(m.label, sizeof(m.label), "%s", label);
    return m;
}

std::vector<cascade::core::HostMarker> marks(PluginUi& ui) {
    std::vector<cascade::core::HostMarker> v;
    ui.api().markers(v);
    return v;
}

void testMarkers() {
    std::printf("markers\n");
    resetCaps();
    ServiceLog log;
    PluginUi ui;
    ui.setServices(services(&log));
    ui.rebuild({plug(0, "Mk"), plug(1, "Mk2")});
    CHECK(H(0) != nullptr && H(1) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr) { return; }
    const CascadeHostApi* h = H(0);
    void* c = C(0);

    const std::uint64_t seq0 = ui.api().markersSeq();
    CascadeMarker m = marker(7, 145.5e6, CASCADE_MARKER_POINT, 0.0, "Repeater");
    CHECK(h->set_marker(c, &m) == CASCADE_API_OK);
    CHECK(ui.api().markersSeq() > seq0);
    // THE HOST COPIED IT: the plugin reusing its buffer changes nothing.
    std::snprintf(m.label, sizeof(m.label), "%s", "CHANGED");
    m.freqHz = 1.0;
    std::vector<cascade::core::HostMarker> v = marks(ui);
    CHECK(v.size() == 1u);
    if (!v.empty()) {
        CHECK(std::strcmp(v[0].m.label, "Repeater") == 0);
        CHECK(v[0].m.freqHz == 145.5e6);
        CHECK(v[0].key == keyOf("Mk"));
        CHECK(v[0].name == "Mk");
    }
    // SETTING AN ID REPLACES.
    CascadeMarker span = marker(7, 144.0e6, CASCADE_MARKER_SPAN, 2.0e6, "2 m band");
    CHECK(h->set_marker(c, &span) == CASCADE_API_OK);
    v = marks(ui);
    CHECK(v.size() == 1u);
    if (!v.empty()) {
        CHECK(v[0].m.kind == CASCADE_MARKER_SPAN);
        CHECK(v[0].m.widthHz == 2.0e6);
    }
    // Another plugin's marks are its own - same id, separate entry.
    CascadeMarker other = marker(7, 433.0e6);
    CHECK(H(1)->set_marker(C(1), &other) == CASCADE_API_OK);
    CHECK(marks(ui).size() == 2u);

    // [V] invalid marks, refused with the reason, nothing stored.
    CHECK(h->set_marker(c, nullptr) == CASCADE_API_BAD_ARGUMENT);
    CascadeMarker bad = marker(1, 145.0e6);
    bad.structSize = 8;
    CHECK(h->set_marker(c, &bad) == CASCADE_API_BAD_ARGUMENT);
    bad = marker(1, kNaN);
    CHECK(h->set_marker(c, &bad) == CASCADE_API_BAD_ARGUMENT);
    bad = marker(1, -5.0);
    CHECK(h->set_marker(c, &bad) == CASCADE_API_OUT_OF_RANGE);
    bad = marker(1, 145.0e6, 9u);
    CHECK(h->set_marker(c, &bad) == CASCADE_API_OUT_OF_RANGE);
    bad = marker(1, 145.0e6, CASCADE_MARKER_SPAN, 0.0);
    CHECK(h->set_marker(c, &bad) == CASCADE_API_OUT_OF_RANGE);
    bad = marker(1, 145.0e6, CASCADE_MARKER_SPAN, kInf);
    CHECK(h->set_marker(c, &bad) == CASCADE_API_BAD_ARGUMENT);
    CHECK(marks(ui).size() == 2u);

    // A LABEL FILLED TO THE LAST BYTE (no terminator) is cut, never over-read.
    CascadeMarker full = marker(2, 146.0e6);
    std::memset(full.label, 'X', sizeof(full.label));
    CHECK(h->set_marker(c, &full) == CASCADE_API_OK);
    v = marks(ui);
    bool sawFull = false;
    for (const auto& e : v) {
        if (e.m.id == 2u && e.key == keyOf("Mk")) {
            sawFull = true;
            CHECK(std::strlen(e.m.label) == sizeof(full.label) - 1u);
        }
    }
    CHECK(sawFull);
    // ...and one ending part-way through a UTF-8 character is cut back to it.
    CascadeMarker utf = marker(3, 146.5e6);
    std::memset(utf.label, 'a', sizeof(utf.label));
    utf.label[sizeof(utf.label) - 3] = static_cast<char>(0xE2);  // start of a 3-byte char
    utf.label[sizeof(utf.label) - 2] = static_cast<char>(0x82);
    utf.label[sizeof(utf.label) - 1] = static_cast<char>(0xAC);  // (no room for the NUL)
    CHECK(h->set_marker(c, &utf) == CASCADE_API_OK);
    v = marks(ui);
    for (const auto& e : v) {
        if (e.m.id == 3u) { CHECK(std::strlen(e.m.label) == sizeof(utf.label) - 3u); }
    }

    // THE QUOTA: CASCADE_MAX_MARKERS_PER_PLUGIN, then LIMIT - replacing still works.
    CHECK(h->clear_markers(c) == CASCADE_API_OK);
    for (std::uint32_t i = 0; i < CASCADE_MAX_MARKERS_PER_PLUGIN; ++i) {
        CascadeMarker k = marker(100u + i, 100.0e6 + i);
        if (h->set_marker(c, &k) != CASCADE_API_OK) {
            CHECK(false);
            break;
        }
    }
    CascadeMarker extra = marker(999, 101.0e6);
    CHECK(h->set_marker(c, &extra) == CASCADE_API_LIMIT);
    CascadeMarker again = marker(100, 102.0e6);
    CHECK(h->set_marker(c, &again) == CASCADE_API_OK);

    CHECK(h->remove_marker(c, 100) == CASCADE_API_OK);
    CHECK(h->remove_marker(c, 100) == CASCADE_API_NOT_FOUND);
    CHECK(h->clear_markers(c) == CASCADE_API_OK);
    CHECK(marks(ui).size() == 1u);  // only Mk2's is left

    // A STOPPED PLUGIN'S MARKS LEAVE THE SCREEN AT ONCE; a plugin that is no
    // longer attached after a rebuild loses them too.
    CascadeMarker back = marker(5, 145.0e6);
    CHECK(h->set_marker(c, &back) == CASCADE_API_OK);
    ui.setStopped({keyOf("Mk")});
    v = marks(ui);
    CHECK(v.size() == 1u);
    if (!v.empty()) { CHECK(v[0].key == keyOf("Mk2")); }
    ui.setStopped({});
    ui.rebuild({plug(0, "Mk")});  // Mk2 is gone
    CHECK(marks(ui).empty());
}

// --- where a mark is drawn (gui/plugin_markers.hpp), pure --------------------

void testMarkerGeometry() {
    std::printf("marker geometry\n");
    using cascade::gui::pluginMarkerGeometry;
    // View 100..101 MHz on a 1000 px panel at x = 50.
    CascadeMarker m = marker(1, 100.25e6);
    auto d = pluginMarkerGeometry(m, 100.0e6, 101.0e6, 50.0f, 1000.0f, false);
    CHECK(d.visible && !d.span);
    CHECK_NEAR(d.x0, 300.0, 1e-3);
    // Outside the view: not drawn.
    m.freqHz = 99.0e6;
    CHECK(!pluginMarkerGeometry(m, 100.0e6, 101.0e6, 50.0f, 1000.0f, false).visible);
    // A span partly in view is clipped to the panel.
    CascadeMarker s = marker(2, 99.5e6, CASCADE_MARKER_SPAN, 1.0e6);
    d = pluginMarkerGeometry(s, 100.0e6, 101.0e6, 50.0f, 1000.0f, false);
    CHECK(d.visible && d.span);
    CHECK_NEAR(d.x0, 50.0, 1e-3);
    CHECK_NEAR(d.x1, 550.0, 1e-3);
    // Wholly outside: not drawn.
    s.freqHz = 101.5e6;
    CHECK(!pluginMarkerGeometry(s, 100.0e6, 101.0e6, 50.0f, 1000.0f, false).visible);
    // SPECTRUM ONLY keeps it off the waterfall and on the spectrum.
    m = marker(3, 100.5e6);
    m.flags = CASCADE_MARKER_FLAG_SPECTRUM_ONLY;
    CHECK(pluginMarkerGeometry(m, 100.0e6, 101.0e6, 0.0f, 100.0f, false).visible);
    CHECK(!pluginMarkerGeometry(m, 100.0e6, 101.0e6, 0.0f, 100.0f, true).visible);
    // A degenerate view draws nothing rather than dividing by zero.
    CHECK(!pluginMarkerGeometry(m, 100.0e6, 100.0e6, 0.0f, 100.0f, false).visible);
    // Colour: 0 is the host's own; an invisible alpha is floored.
    CHECK(cascade::gui::pluginMarkerColour(0u) == cascade::gui::theme::kGold);
    const ImU32 red = cascade::gui::pluginMarkerColour(0xFF000000u);  // alpha 0
    CHECK(((red >> IM_COL32_A_SHIFT) & 0xFFu) == cascade::gui::kPluginMarkerMinAlpha);
    CHECK(((red >> IM_COL32_R_SHIFT) & 0xFFu) == 0xFFu);
}

// ===========================================================================
// The settings store
// ===========================================================================

void testSettings() {
    std::printf("settings\n");
    resetCaps();
    ServiceLog log;
    PluginUi ui;
    ui.setServices(services(&log));
    // Loaded BEFORE the rebuild, as AppWindow::applyConfig does - so the
    // plugin finds its setting from inside attach().
    cascade::core::PluginSettingsMap preload;
    preload["Store"]["seen"] = "yes";
    preload["Store"]["bad key!"] = "dropped by the sanitiser";
    ui.api().loadSettings(preload);
    CHECK(ui.api().settingsGeneration() == 0u);  // a load is not a change
    ui.rebuild({plug(0, "Store"), plug(1, "Neighbour")});
    CHECK(H(0) != nullptr && H(1) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr) { return; }
    // Read from inside attach(): live, and the value was there.
    CHECK(g_cap[0].stateInAttach == CASCADE_API_OK);
    CHECK(g_cap[0].settingsInAttach == 3);  // strlen("yes")
    CHECK(g_cap[1].settingsInAttach == CASCADE_API_NOT_FOUND);
    const CascadeHostApi* h = H(0);
    void* c = C(0);

    char buf[64];
    CHECK(h->settings_get(c, "seen", buf, sizeof(buf)) == 3);
    CHECK(std::strcmp(buf, "yes") == 0);
    CHECK(h->settings_get(c, "bad key!", buf, sizeof(buf)) == CASCADE_API_BAD_ARGUMENT);

    // Round trip, and the generation moves only on a real change.
    const std::uint64_t g0 = ui.api().settingsGeneration();
    CHECK(h->settings_set(c, "callsign", "G0ABC") == CASCADE_API_OK);
    const std::uint64_t g1 = ui.api().settingsGeneration();
    CHECK(g1 == g0 + 1u);
    CHECK(h->settings_set(c, "callsign", "G0ABC") == CASCADE_API_OK);
    CHECK(ui.api().settingsGeneration() == g1);
    CHECK(h->settings_get(c, "callsign", buf, sizeof(buf)) == 5);
    CHECK(std::strcmp(buf, "G0ABC") == 0);

    // A buffer too small: the full length is returned, the copy is cut and
    // terminated, and never split inside a character.
    CHECK(h->settings_set(c, "utf", "ab\xC3\xA9z") == CASCADE_API_OK);  // "abéz", 5 bytes
    char four[4];
    CHECK(h->settings_get(c, "utf", four, sizeof(four)) == 5);
    CHECK(std::strcmp(four, "ab") == 0);  // "ab" + half of é would be 3; cut to 2
    char one[1];
    CHECK(h->settings_get(c, "utf", one, sizeof(one)) == 5);
    CHECK(one[0] == '\0');
    CHECK(h->settings_get(c, "utf", nullptr, 0) == 5);  // measure only

    // A plugin sees only its own keys.
    CHECK(H(1)->settings_get(C(1), "callsign", buf, sizeof(buf)) == CASCADE_API_NOT_FOUND);
    CHECK(H(1)->settings_set(C(1), "callsign", "M0XYZ") == CASCADE_API_OK);
    CHECK(h->settings_get(c, "callsign", buf, sizeof(buf)) == 5);
    CHECK(std::strcmp(buf, "G0ABC") == 0);

    // Delete.
    CHECK(h->settings_set(c, "callsign", nullptr) == CASCADE_API_OK);
    CHECK(h->settings_get(c, "callsign", buf, sizeof(buf)) == CASCADE_API_NOT_FOUND);
    CHECK(h->settings_set(c, "callsign", nullptr) == CASCADE_API_NOT_FOUND);

    // [V] keys and values the store refuses.
    CHECK(h->settings_set(c, nullptr, "v") == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->settings_set(c, "", "v") == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->settings_set(c, "has space", "v") == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->settings_set(c, "slash/no", "v") == CASCADE_API_BAD_ARGUMENT);
    const std::string key63(63, 'k');
    const std::string key64(64, 'k');
    CHECK(h->settings_set(c, key63.c_str(), "v") == CASCADE_API_OK);
    CHECK(h->settings_set(c, key64.c_str(), "v") == CASCADE_API_BAD_ARGUMENT);
    const std::string big(CASCADE_SETTING_VALUE_BYTES - 1u, 'x');
    const std::string tooBig(CASCADE_SETTING_VALUE_BYTES, 'x');
    CHECK(h->settings_set(c, "big", big.c_str()) == CASCADE_API_OK);
    CHECK(h->settings_set(c, "big", tooBig.c_str()) == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->settings_set(c, "badutf", "\xC3\x28") == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->settings_set(c, "badutf", "\xFF") == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->settings_get(c, "k", nullptr, 4) == CASCADE_API_BAD_ARGUMENT);

    // THE QUOTA: CASCADE_MAX_SETTINGS_PER_PLUGIN keys, then LIMIT - but an
    // existing key can still be rewritten.
    for (std::uint32_t i = 0; i < CASCADE_MAX_SETTINGS_PER_PLUGIN; ++i) {
        char k[16];
        std::snprintf(k, sizeof(k), "q%u", i);
        (void)h->settings_set(c, k, "1");
    }
    CHECK(h->settings_set(c, "one.more", "1") == CASCADE_API_LIMIT);
    CHECK(h->settings_set(c, "q0", "2") == CASCADE_API_OK);

    // KEYED ON THE PLUGIN'S NAME, which survives an upgrade: a new build under
    // a new FILE name finds the same settings.
    const cascade::core::PluginSettingsMap snap = ui.api().settingsSnapshot();
    CHECK(snap.count("Store") == 1u);
    CHECK(snap.count("Neighbour") == 1u);
    ui.rebuild({});
    resetCaps();
    LoadedPlugin upgraded = plug(2, "Store");
    upgraded.path = "C:/plugins/Store-2.0.0.dll";
    ui.rebuild({upgraded});
    CHECK(H(2) != nullptr);
    if (H(2) != nullptr) {
        CHECK(H(2)->settings_get(C(2), "q0", buf, sizeof(buf)) == 1);
        CHECK(std::strcmp(buf, "2") == 0);
    }

    // The sanitiser: the same bounds, applied to a hand-edited config.
    cascade::core::PluginSettingsMap dirty;
    dirty["P"]["ok"] = "fine";
    dirty["P"]["no way"] = "bad key";
    dirty["P"]["long"] = std::string(CASCADE_SETTING_VALUE_BYTES, 'x');
    dirty["P"]["utf"] = "\xC3";
    dirty["P"][std::string("nul\0in", 6)] = "v";
    dirty[""]["k"] = "no plugin";
    const cascade::core::PluginSettingsMap clean = cascade::core::sanitisePluginSettings(dirty);
    CHECK(clean.size() == 1u);
    if (clean.count("P") != 0u) {
        CHECK(clean.at("P").size() == 1u);
        CHECK(clean.at("P").count("ok") == 1u);
    }
    cascade::core::PluginSettingsMap many;
    for (std::size_t i = 0; i < cascade::core::kMaxSettingsPlugins + 10u; ++i) {
        many["p" + std::to_string(i)]["k"] = "v";
    }
    CHECK(cascade::core::sanitisePluginSettings(many).size() == cascade::core::kMaxSettingsPlugins);
}

// --- the config file carries both new fields ---------------------------------

void testConfigRoundTrip() {
    std::printf("config round trip\n");
    const fs::path dir = fs::temp_directory_path() /
                         ("foxsdr_plugin_api_cfg_" + std::to_string(
                                                         std::chrono::steady_clock::now()
                                                             .time_since_epoch()
                                                             .count()));
    fs::create_directories(dir);
    const std::string path = (dir / "config.json").string();

    cascade::core::AppConfig out;
    out.pluginSettingsAllowed = {"sat-1.0.0.dll", "sat-1.0.0.dll", ""};
    out.pluginSettings["Satellites"]["station"] = "IO93";
    out.pluginSettings["Satellites"]["elevation.min"] = "10";
    out.pluginSettings["Pager"]["k"] = "caf\xC3\xA9";
    std::string err;
    CHECK(cascade::core::ConfigStore::save(path, out, err));
    cascade::core::AppConfig in;
    CHECK(cascade::core::ConfigStore::load(path, in, err));
    // Sanitised exactly like the tune grants: duplicates and empties dropped.
    CHECK(in.pluginSettingsAllowed.size() == 1u);
    if (!in.pluginSettingsAllowed.empty()) {
        CHECK(in.pluginSettingsAllowed[0] == "sat-1.0.0.dll");
    }
    CHECK(in.pluginSettings.size() == 2u);
    CHECK(in.pluginSettings["Satellites"]["station"] == "IO93");
    CHECK(in.pluginSettings["Satellites"]["elevation.min"] == "10");
    CHECK(in.pluginSettings["Pager"]["k"] == "caf\xC3\xA9");

    // A config from BEFORE this build has neither key: both load empty, and
    // every other setting is unaffected (the meaning of old settings is not
    // changed by the new ones existing).
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            std::fputs("{\"pluginTuneAllowed\":[\"sat-1.0.0.dll\"],\"volume\":0.3}", f);
            std::fclose(f);
        }
        cascade::core::AppConfig legacy;
        CHECK(cascade::core::ConfigStore::load(path, legacy, err));
        CHECK(legacy.pluginSettingsAllowed.empty());
        CHECK(legacy.pluginSettings.empty());
        CHECK(legacy.pluginTuneAllowed.size() == 1u);
    }
    // A hand-edited file with junk in the store: the junk is dropped.
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        if (f != nullptr) {
            std::fputs("{\"pluginSettings\":{\"A\":{\"ok\":\"1\",\"bad key\":\"x\",\"n\":5},"
                       "\"B\":7,\"C\":{}},\"pluginSettingsAllowed\":\"notalist\"}",
                       f);
            std::fclose(f);
        }
        cascade::core::AppConfig junk;
        CHECK(cascade::core::ConfigStore::load(path, junk, err));
        CHECK(junk.pluginSettings.size() == 1u);
        CHECK(junk.pluginSettings["A"].size() == 1u);
        CHECK(junk.pluginSettingsAllowed.empty());
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ===========================================================================
// Log lines and notices
// ===========================================================================

void testLog() {
    std::printf("log\n");
    resetCaps();
    ServiceLog slog;
    PluginUi ui;
    ui.setServices(services(&slog));
    ui.rebuild({plug(0, "Talker")});
    CHECK(H(0) != nullptr);
    if (H(0) == nullptr) { return; }
    const CascadeHostApi* h = H(0);
    void* c = C(0);

    CHECK(h->log(c, CASCADE_LOG_INFO, "hello") == CASCADE_API_OK);
    CHECK(h->log(c, CASCADE_LOG_WARN, "careful") == CASCADE_API_OK);
    CHECK(h->log(c, CASCADE_LOG_ERROR, "broken") == CASCADE_API_OK);
    CHECK(h->log(c, CASCADE_LOG_DEBUG, "dropped") == CASCADE_API_OK);  // accepted...
    CHECK(h->log(c, 4u, "no such level") == CASCADE_API_OUT_OF_RANGE);
    CHECK(h->log(c, CASCADE_LOG_INFO, nullptr) == CASCADE_API_BAD_ARGUMENT);
    std::vector<cascade::core::PluginLogLine> lines;
    ui.api().takeLog(lines);
    CHECK(lines.size() == 3u);  // ...and not shown
    if (lines.size() == 3u) {
        CHECK(lines[0].text == "hello" && lines[0].level == CASCADE_LOG_INFO);
        CHECK(lines[1].level == CASCADE_LOG_WARN);
        CHECK(lines[2].level == CASCADE_LOG_ERROR);
        CHECK(lines[0].name == "Talker" && lines[0].key == keyOf("Talker"));
    }

    // Cut at CASCADE_LOG_TEXT_BYTES - 1, on a character boundary.
    std::string longText(CASCADE_LOG_TEXT_BYTES + 100u, 'a');
    longText[CASCADE_LOG_TEXT_BYTES - 2u] = static_cast<char>(0xC3);  // a 2-byte char straddling
    longText[CASCADE_LOG_TEXT_BYTES - 1u] = static_cast<char>(0xA9);  // the cut
    CHECK(h->log(c, CASCADE_LOG_INFO, longText.c_str()) == CASCADE_API_OK);
    lines.clear();
    ui.api().takeLog(lines);
    CHECK(lines.size() == 1u);
    if (!lines.empty()) { CHECK(lines[0].text.size() == CASCADE_LOG_TEXT_BYTES - 2u); }

    // BUSY past the queue, counted; never growing.
    for (std::size_t i = 0; i < PluginApiCore::kLogQueue; ++i) {
        (void)h->log(c, CASCADE_LOG_INFO, "x");
    }
    CHECK(h->log(c, CASCADE_LOG_INFO, "one too many") == CASCADE_API_BUSY);
    CHECK(ui.api().logDropped() == 1u);
    lines.clear();
    ui.api().takeLog(lines);
    CHECK(lines.size() == PluginApiCore::kLogQueue);
}

// ===========================================================================
// Commands
// ===========================================================================

void testCommands() {
    std::printf("commands\n");
    resetCaps();
    ServiceLog slog;
    PluginUi ui;
    ui.setServices(services(&slog));
    ui.rebuild({plug(0, "Cmd")});
    CHECK(H(0) != nullptr);
    if (H(0) == nullptr) { return; }
    const CascadeHostApi* h = H(0);
    void* c = C(0);
    const std::string key = keyOf("Cmd");

    CHECK(h->add_command(c, 1, "Scan now") == CASCADE_API_OK);
    CHECK(h->add_command(c, 2, "Reset") == CASCADE_API_OK);
    CHECK(h->add_command(c, 1, "Scan") == CASCADE_API_OK);  // relabel, not a third
    std::vector<cascade::core::HostCommand> cmds = ui.api().commands(key);
    CHECK(cmds.size() == 2u);
    if (cmds.size() == 2u) {
        CHECK(cmds[0].id == 1u && cmds[0].label == "Scan");
        CHECK(cmds[1].id == 2u && cmds[1].label == "Reset");
    }
    // [V]
    CHECK(h->add_command(c, 0, "zero id") == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->add_command(c, 3, nullptr) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->add_command(c, 3, "") == CASCADE_API_BAD_ARGUMENT);
    std::uint32_t id = 0;
    CHECK(h->poll_command(c, nullptr) == CASCADE_API_BAD_ARGUMENT);
    CHECK(h->poll_command(c, &id) == 0);

    // A press is delivered once, in order, to the plugin that owns the key.
    CHECK(ui.api().pressCommand(key, 2));
    CHECK(ui.api().pressCommand(key, 1));
    CHECK(!ui.api().pressCommand(key, 99));        // not a key it has
    CHECK(!ui.api().pressCommand("nobody.dll", 1));
    CHECK(h->poll_command(c, &id) == 1 && id == 2u);
    CHECK(h->poll_command(c, &id) == 1 && id == 1u);
    CHECK(h->poll_command(c, &id) == 0);

    // The queue keeps the NEWEST presses: the oldest goes past kPressQueue.
    for (std::size_t i = 0; i < PluginApiCore::kPressQueue + 3u; ++i) {
        (void)ui.api().pressCommand(key, (i % 2u) == 0u ? 1u : 2u);
    }
    std::size_t delivered = 0;
    while (h->poll_command(c, &id) == 1) { ++delivered; }
    CHECK(delivered == PluginApiCore::kPressQueue);

    // Removing a command withdraws its pending presses.
    CHECK(ui.api().pressCommand(key, 1));
    CHECK(ui.api().pressCommand(key, 2));
    CHECK(h->remove_command(c, 1) == CASCADE_API_OK);
    CHECK(h->remove_command(c, 1) == CASCADE_API_NOT_FOUND);
    CHECK(h->poll_command(c, &id) == 1 && id == 2u);
    CHECK(h->poll_command(c, &id) == 0);

    // The quota.
    for (std::uint32_t i = 10; i < 10u + CASCADE_MAX_COMMANDS_PER_PLUGIN; ++i) {
        (void)h->add_command(c, i, "k");
    }
    CHECK(h->add_command(c, 500, "too many") == CASCADE_API_LIMIT);
    CHECK(ui.api().commands(key).size() == CASCADE_MAX_COMMANDS_PER_PLUGIN);

    // A long label is cut to CASCADE_COMMAND_LABEL_CHARS - 1.
    ui.rebuild({plug(0, "Cmd")});  // same plugin: stays live, keeps its keys
    CHECK(ui.api().commands(key).size() == CASCADE_MAX_COMMANDS_PER_PLUGIN);
    CHECK(h->remove_command(c, 10) == CASCADE_API_OK);
    const std::string longLabel(100, 'L');
    CHECK(h->add_command(c, 77, longLabel.c_str()) == CASCADE_API_OK);
    for (const auto& k : ui.api().commands(key)) {
        if (k.id == 77u) { CHECK(k.label.size() == CASCADE_COMMAND_LABEL_CHARS - 1u); }
    }
    // Stopped: its keys go, and a press is refused.
    ui.setStopped({key});
    CHECK(ui.api().commands(key).empty());
}

// ===========================================================================
// [U] Detach, unload and teardown - including calls racing them
// ===========================================================================

void testDetach() {
    std::printf("[U] detach and unload\n");
    resetCaps();
    ServiceLog slog;
    const CascadeHostApi* h = nullptr;
    void* c = nullptr;
    {
        PluginUi ui;
        ui.setServices(services(&slog));
        ui.rebuild({plug(0, "Gone")});
        h = H(0);
        c = C(0);
        CHECK(h != nullptr);
        if (h == nullptr) { return; }
        ui.setSettingsAllowed(keyOf("Gone"), true);
        CascadeMarker m = marker(1, 145.0e6);
        CHECK(h->set_marker(c, &m) == CASCADE_API_OK);
        CHECK(h->add_command(c, 1, "Go") == CASCADE_API_OK);

        // A PLUGIN THREAD HAMMERING THE TABLE WHILE THE HOST TEARS DOWN: the
        // plugin is unloaded mid-call. Every answer must be OK or DETACHED -
        // never a crash, never a hang - and after clear() only DETACHED.
        std::atomic<bool> stop{false};
        std::atomic<long> okN{0}, detN{0}, otherN{0};
        std::thread hammer([&] {
            char buf[32];
            while (!stop.load()) {
                CascadeReceiverState s{};
                s.structSize = sizeof(s);
                const std::int32_t rs[] = {
                    h->get_state(c, &s), h->log(c, CASCADE_LOG_INFO, "x"),
                    h->settings_set(c, "k", "v"), h->settings_get(c, "k", buf, sizeof(buf)),
                    h->set_marker(c, &m)};
                for (std::int32_t r : rs) {
                    if (r == CASCADE_API_OK || r > 0) {
                        ++okN;
                    } else if (r == CASCADE_API_DETACHED) {
                        ++detN;
                    } else if (r != CASCADE_API_BUSY) {
                        ++otherN;
                    }
                }
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ui.clear();  // what a rescan does before unloading the modules
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true);
        hammer.join();
        std::printf("  racing clear(): %ld ok, %ld detached, %ld other\n", okN.load(),
                    detN.load(), otherN.load());
        CHECK(okN.load() > 0);
        CHECK(detN.load() > 0);
        CHECK(otherN.load() == 0);

        // After clear(): everything DETACHED, marks and commands gone.
        CascadeReceiverState s{};
        s.structSize = sizeof(s);
        CHECK(h->get_state(c, &s) == CASCADE_API_DETACHED);
        CHECK(h->set_volume(c, 0.5) == CASCADE_API_DETACHED);
        CHECK(h->log(c, CASCADE_LOG_INFO, "x") == CASCADE_API_DETACHED);
        CHECK(h->set_marker(c, &m) == CASCADE_API_DETACHED);
        std::uint32_t id = 0;
        CHECK(h->poll_command(c, &id) == CASCADE_API_DETACHED);
        std::vector<cascade::core::HostMarker> v;
        ui.api().markers(v);
        CHECK(v.empty());
        CHECK(ui.api().commands(keyOf("Gone")).empty());

        // A rebuild that attaches it again makes it live again - same table.
        ui.rebuild({plug(0, "Gone")});
        CHECK(H(0) == h);
        CHECK(h->get_state(c, &s) == CASCADE_API_OK);
    }
    // THE OWNER IS GONE (process shutdown order): the table is still valid
    // memory - a plugin may have kept it - and answers DETACHED, and the four
    // original functions their documented "nothing".
    CascadeReceiverState s{};
    s.structSize = sizeof(s);
    CHECK(h->get_state(c, &s) == CASCADE_API_DETACHED);
    CHECK(h->settings_set(c, "k", "v") == CASCADE_API_DETACHED);
    CHECK(h->set_frequency(c, 145.0e6) == CASCADE_API_DETACHED);
    CHECK(h->centre_hz(c) == 0.0);
    CHECK(h->unix_time_ms(c) == 0);

    // A STOPPED plugin is not attached by the next rebuild: DETACHED too.
    resetCaps();
    PluginUi ui2;
    ui2.setServices(services(&slog));
    ui2.rebuild({plug(0, "Paused")});
    const CascadeHostApi* h2 = H(0);
    CHECK(h2 != nullptr);
    if (h2 == nullptr) { return; }
    ui2.setStopped({keyOf("Paused")});
    ui2.rebuild({plug(0, "Paused")});
    CHECK(h2->get_state(h2->ctx, &s) == CASCADE_API_DETACHED);
}

// ===========================================================================
// The stream clock - including a read from INSIDE process()
// ===========================================================================

struct StreamProbe {
    std::atomic<int> calls{0};
    std::int32_t lastRc = 99;
    CascadeStreamInfo last{};
};
StreamProbe g_probe;
const CascadeHostApi* g_probeHost = nullptr;

void* probeCreate(uint32_t) { return &g_probe; }
void probeProcess(void*, const float*, size_t) {
    // THE HOST IS HOLDING THE RUNNER'S LOCK HERE. A get_stream_info that took
    // that lock would deadlock on the first block; this is the test of that.
    if (g_probeHost != nullptr) {
        CascadeStreamInfo si{};
        si.structSize = sizeof(si);
        g_probe.lastRc = g_probeHost->get_stream_info(g_probeHost->ctx, &si);
        g_probe.last = si;
    }
    ++g_probe.calls;
}
int32_t probePoll(void*, char*, size_t) { return 0; }
void probeDestroy(void*) {}
const CascadeDecoderApi kProbeDecoder = {sizeof(CascadeDecoderApi), 0u, &probeCreate,
                                         &probeProcess, &probePoll, &probeDestroy};

void testStreamInfo() {
    std::printf("stream info\n");
    resetCaps();
    ServiceLog slog;
    PluginUi ui;
    ui.setServices(services(&slog));
    LoadedPlugin p = plug(0, "Clock");
    p.capabilities |= CASCADE_CAP_DECODER;
    p.decoder = &kProbeDecoder;
    ui.rebuild({p});
    g_probeHost = H(0);
    CHECK(g_probeHost != nullptr);
    if (g_probeHost == nullptr) { return; }
    ui.api().publish(facts());

    PluginRunner runner;
    runner.setStreamClock(ui.api().streamClock());
    runner.rebuild({p}, 48000.0, 2.4e6, 145.0e6);

    CascadeStreamInfo si{};
    si.structSize = sizeof(si);
    CHECK(g_probeHost->get_stream_info(g_probeHost->ctx, &si) == CASCADE_API_OK);
    const std::uint64_t epoch1 = si.epoch;
    CHECK(epoch1 >= 1u);
    CHECK(si.iqRateHz == 2.4e6);
    CHECK(si.audioRateHz == 48000.0);
    CHECK(si.iqFrames == 0u && si.audioFrames == 0u);
    CHECK(si.outputRateHz == 48000.0);
    CHECK(si.outputFrames == 480000u);
    CHECK(si.epochStartUnixMs > 1'600'000'000'000LL);

    std::vector<float> audio(480, 0.1f);
    std::vector<float> iq(2 * 1000, 0.1f);
    finishesWithin(10000, [&] {
        const RealtimeThreadScope realtime;
        for (int i = 0; i < 10; ++i) { runner.processAudio(audio.data(), audio.size()); }
        runner.processIq(iq.data(), 1000);
    });
    CHECK(g_probe.calls.load() == 10);
    CHECK(g_probe.lastRc == CASCADE_API_OK);  // read from inside process()
    // The tenth block's own frames are counted before process() is called.
    CHECK(g_probe.last.audioFrames == 10u * 480u);
    CHECK(g_probeHost->get_stream_info(g_probeHost->ctx, &si) == CASCADE_API_OK);
    CHECK(si.audioFrames == 4800u);
    CHECK(si.iqFrames == 1000u);

    // A rebuild is a new unbroken stream: the epoch moves and counts restart.
    runner.rebuild({p}, 48000.0, 1.024e6, 145.0e6);
    CHECK(g_probeHost->get_stream_info(g_probeHost->ctx, &si) == CASCADE_API_OK);
    CHECK(si.epoch == epoch1 + 1u);
    CHECK(si.iqRateHz == 1.024e6);
    CHECK(si.audioFrames == 0u);
    // clear() ends the stream: another epoch, no rates.
    runner.clear();
    CHECK(g_probeHost->get_stream_info(g_probeHost->ctx, &si) == CASCADE_API_OK);
    CHECK(si.epoch == epoch1 + 2u);
    CHECK(si.iqRateHz == 0.0 && si.audioRateHz == 0.0);
    g_probeHost = nullptr;
}

// ===========================================================================
// CASCADE_CAP_AUDIO_PROCESSOR: the loader, the runner, the pipeline
// ===========================================================================

struct Proc {
    float gain = 1.0f;
    bool makeNan = false;
    int creates = 0, destroys = 0;
    std::uint32_t rate = 0, channels = 0;
    std::atomic<long> frames{0};
    std::vector<int>* order = nullptr;
    int tag = 0;
    void reset() {
        gain = 1.0f;
        makeNan = false;
        creates = destroys = 0;
        rate = channels = 0;
        frames = 0;
        order = nullptr;
        tag = 0;
    }
};
Proc g_procA, g_procB;

template <Proc* P>
void* procCreate(uint32_t rateHz, uint32_t channels) {
    ++P->creates;
    P->rate = rateHz;
    P->channels = channels;
    return P;
}
template <Proc* P>
void procProcess(void* h, float* x, size_t frames) {
    Proc* p = static_cast<Proc*>(h);
    for (size_t i = 0; i < 2u * frames; ++i) { x[i] *= p->gain; }
    if (p->makeNan && frames > 0) { x[0] = std::numeric_limits<float>::quiet_NaN(); }
    if (p->order != nullptr) { p->order->push_back(p->tag); }
    p->frames += static_cast<long>(frames);
}
template <Proc* P>
void procDestroy(void*) {
    ++P->destroys;
}

CascadeAudioProcessorApi procApi(void* (*c)(uint32_t, uint32_t), void (*p)(void*, float*, size_t),
                                  void (*d)(void*), const char* title = "Gain") {
    CascadeAudioProcessorApi a{};
    a.structSize = sizeof(a);
    a.flags = 0;
    a.title = title;
    a.create = c;
    a.process = p;
    a.destroy = d;
    return a;
}

PluginRejection validateProc(const CascadeAudioProcessorApi* a, std::uint32_t tableSize) {
    const CascadeCapabilityEntry caps[] = {{CASCADE_CAP_AUDIO_PROCESSOR, tableSize, a}};
    CascadePluginDesc d{};
    d.structSize = sizeof(CascadePluginDesc);
    d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    d.name = "Proc";
    d.version = "1.0.0";
    d.author = "";
    d.licence = "MIT";
    d.capabilities = CASCADE_CAP_AUDIO_PROCESSOR;
    d.capabilityCount = 1;
    d.capabilityTables = caps;
    return cascade::core::validatePluginDesc(&d);
}

LoadedPlugin procPlugin(const char* name, const CascadeAudioProcessorApi* a) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.capabilities = CASCADE_CAP_AUDIO_PROCESSOR;
    p.audioProcessor = a;
    return p;
}

void testProcessorValidation() {
    std::printf("processor validation\n");
    CascadeAudioProcessorApi a =
        procApi(&procCreate<&g_procA>, &procProcess<&g_procA>, &procDestroy<&g_procA>);
    // A PROCESSOR-ONLY plugin is usable on its own - it does a job.
    CHECK(validateProc(&a, sizeof(a)) == PluginRejection::None);
    CHECK(validateProc(nullptr, sizeof(a)) == PluginRejection::MissingAudioProcessorApi);
    CHECK(validateProc(&a, sizeof(a) + 8u) == PluginRejection::None);  // tableSize is advisory
    CascadeAudioProcessorApi b = a;
    b.structSize = sizeof(a) - 8u;
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::AudioProcessorStructSizeMismatch);
    b = a;
    b.process = nullptr;
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::MissingAudioProcessorFunction);
    b = a;
    b.create = nullptr;
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::MissingAudioProcessorFunction);
    b = a;
    b.destroy = nullptr;
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::MissingAudioProcessorFunction);
    b = a;
    b.title = "";
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::MissingAudioProcessorFunction);
    b = a;
    b.title = nullptr;
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::MissingAudioProcessorFunction);
    b = a;
    b.flags = 1;
    CHECK(validateProc(&b, sizeof(a)) == PluginRejection::AudioProcessorReservedNotZero);
    // Every rejection has its own words.
    CHECK(std::strstr(cascade::core::pluginRejectionMessage(
                          PluginRejection::MissingAudioProcessorFunction),
                      "processor") != nullptr);
}

void testProcessorChain() {
    std::printf("processor chain\n");
    g_procA.reset();
    g_procB.reset();
    std::vector<int> order;
    g_procA.gain = 0.5f;
    g_procA.order = &order;
    g_procA.tag = 1;
    g_procB.gain = 3.0f;
    g_procB.order = &order;
    g_procB.tag = 2;
    const CascadeAudioProcessorApi a =
        procApi(&procCreate<&g_procA>, &procProcess<&g_procA>, &procDestroy<&g_procA>, "Half");
    const CascadeAudioProcessorApi b =
        procApi(&procCreate<&g_procB>, &procProcess<&g_procB>, &procDestroy<&g_procB>, "Triple");

    PluginRunner runner;
    std::vector<float> l(64, 1.0f), r(64, -1.0f);
    // No processor: the audio is not touched at all.
    runner.rebuild({}, 48000.0, 2.4e6, 0.0);
    runner.processAudioChain(l.data(), r.data(), l.size());
    CHECK(l[5] == 1.0f && r[5] == -1.0f);

    runner.rebuild({procPlugin("A", &a), procPlugin("B", &b)}, 48000.0, 2.4e6, 0.0);
    CHECK(g_procA.creates == 1 && g_procB.creates == 1);
    CHECK(g_procA.rate == 48000u && g_procA.channels == 2u);
    CHECK(runner.processorTitles().size() == 2u);
    CHECK(runner.isFeeding("A.dll"));  // a processor reads FED on its plate
    runner.processAudioChain(l.data(), r.data(), l.size());
    // IN PLACE, IN LOAD ORDER, each on the previous one's output: x0.5 then x3.
    CHECK_NEAR(l[5], 1.5, 1e-6);
    CHECK_NEAR(r[5], -1.5, 1e-6);
    CHECK(order.size() == 2u && order[0] == 1 && order[1] == 2);

    // A NON-FINITE SAMPLE NEVER REACHES THE NEXT STAGE, and is counted.
    g_procA.makeNan = true;
    std::fill(l.begin(), l.end(), 1.0f);
    std::fill(r.begin(), r.end(), 1.0f);
    runner.processAudioChain(l.data(), r.data(), l.size());
    CHECK(l[0] == 0.0f);          // the NaN, replaced by silence...
    CHECK(std::isfinite(l[0]));
    CHECK_NEAR(l[1], 1.5, 1e-6);  // ...and nothing else disturbed
    CHECK(runner.processorNonFinite() == 1u);
    g_procA.makeNan = false;

    // STOPPED IS BYPASSED: no instance, the chain is exactly what it was.
    runner.setStopped({"A.dll"});
    runner.rebuild({procPlugin("A", &a), procPlugin("B", &b)}, 48000.0, 2.4e6, 0.0);
    CHECK(g_procA.destroys == 1);   // the old one, on the rebuild
    CHECK(g_procA.creates == 1);    // and no new one
    std::fill(l.begin(), l.end(), 1.0f);
    runner.processAudioChain(l.data(), r.data(), l.size());
    CHECK_NEAR(l[3], 3.0, 1e-6);
    runner.clear();
    CHECK(g_procB.destroys == 2);   // once per instance, exactly
    runner.setStopped({});

    // create() failing leaves the chain untouched and says why.
    const CascadeAudioProcessorApi failing = procApi(
        [](uint32_t, uint32_t) -> void* { return nullptr; }, &procProcess<&g_procA>,
        &procDestroy<&g_procA>, "Broken");
    runner.rebuild({procPlugin("Broken", &failing)}, 48000.0, 2.4e6, 0.0);
    CHECK(runner.processorTitles().empty());
    bool said = false;
    for (const auto& s : runner.status()) {
        if (s.plugin == "Broken" && s.reason == cascade::core::DecoderIdleReason::CreateFailed) {
            said = true;
        }
    }
    CHECK(said);
    runner.clear();
}

// THE REAL CHAIN: a processor installed through the pipeline changes what the
// speakers (and the recorder, the web stream and the tap) get.
template <class Fn>
bool waitFor(Fn ready, int timeoutMs) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        if (ready()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready();
}

double rmsOf(const std::vector<float>& x) {
    double s = 0.0;
    for (float v : x) { s += static_cast<double>(v) * v; }
    return x.empty() ? 0.0 : std::sqrt(s / static_cast<double>(x.size()));
}

void testProcessorInPipeline() {
    std::printf("processor in the pipeline\n");
    g_procA.reset();
    g_procA.gain = 0.25f;
    const CascadeAudioProcessorApi a =
        procApi(&procCreate<&g_procA>, &procProcess<&g_procA>, &procDestroy<&g_procA>, "Quarter");

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;
    cascade::core::Pipeline p(cfg);
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setSquelchDb(-200.0f);
    p.sigGen().setTone(0, 3000.0, 0.0f);
    p.sigGen().setNoiseFloorDb(-300.0f);

    PluginRunner runner;
    runner.rebuild({}, cascade::core::Pipeline::kAudioRateHz, cfg.sampleRateHz, 0.0);
    p.setPluginRunner(&runner);
    p.start();
    constexpr std::size_t kWin = 4096;
    std::vector<float> left(kWin), right(kWin);
    CHECK(waitFor([&] { return p.audioSamplesProduced() > 4u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double plain = rmsOf(left);

    runner.rebuild({procPlugin("Quarter", &a)}, cascade::core::Pipeline::kAudioRateHz,
                   cfg.sampleRateHz, 0.0);
    const std::uint64_t mark = p.audioSamplesProduced();
    CHECK(waitFor([&] { return p.audioSamplesProduced() - mark > 4u * kWin; }, 30000));
    CHECK(p.audioTapStereo(left.data(), right.data(), kWin) == kWin);
    const double processed = rmsOf(left);
    std::printf("  tap rms: %.4f plain, %.4f through a x0.25 processor (%ld frames seen)\n",
                plain, processed, g_procA.frames.load());
    CHECK(plain > 0.01);
    CHECK(g_procA.frames.load() > 0);
    CHECK(processed > 0.2 * plain && processed < 0.3 * plain);

    p.stop();
    p.setPluginRunner(nullptr);
    runner.clear();
    CHECK(g_procA.destroys == 1);
}

// ===========================================================================
// THE PATCH PAGE runs a level-1 plugin's decoder - and it can call the host
// from inside process() there too
// ===========================================================================

struct PatchProbe {
    std::atomic<long> frames{0};
    std::atomic<int> calls{0}, logOk{0}, stateOk{0}, markOk{0}, settingsWrongThread{0}, lines{0};
};
PatchProbe g_patch;
const CascadeHostApi* g_patchHost = nullptr;

void* ppCreate(double, double) { return &g_patch; }
void ppProcess(void*, const float*, size_t frames) {
    g_patch.frames += static_cast<long>(frames);
    ++g_patch.calls;
    const CascadeHostApi* h = g_patchHost;
    if (h == nullptr) { return; }
    CascadeReceiverState s{};
    s.structSize = sizeof(s);
    if (h->get_state(h->ctx, &s) == CASCADE_API_OK) { ++g_patch.stateOk; }
    if (h->log(h->ctx, CASCADE_LOG_INFO, "patch block") == CASCADE_API_OK) { ++g_patch.logOk; }
    CascadeMarker m = marker(42, 145.0e6);
    if (h->set_marker(h->ctx, &m) == CASCADE_API_OK) { ++g_patch.markOk; }
    char buf[8];
    if (h->settings_get(h->ctx, "k", buf, sizeof(buf)) == CASCADE_API_WRONG_THREAD) {
        ++g_patch.settingsWrongThread;
    }
}
int32_t ppPoll(void*, char* buf, size_t cap) {
    if (g_patch.frames.load() == 0 || g_patch.lines.load() > 0 || cap < 16) { return 0; }
    ++g_patch.lines;
    std::memcpy(buf, "LEVEL1 OK\n", 10);
    return 10;
}
void ppDestroy(void*) {}

void testPatchPage() {
    std::printf("patch page\n");
    using namespace cascade::core::patch;
    resetCaps();
    ServiceLog slog;
    PluginUi ui;
    ui.setServices(services(&slog));
    ui.rebuild({plug(0, "Level1")});
    g_patchHost = H(0);
    CHECK(g_patchHost != nullptr);
    if (g_patchHost == nullptr) { return; }
    ui.api().publish(facts());

    CascadeIqDecoderApi iq{};
    iq.structSize = sizeof(iq);
    iq.create = &ppCreate;
    iq.process = &ppProcess;
    iq.poll_text = &ppPoll;
    iq.destroy = &ppDestroy;
    const std::vector<DecoderInfo> cat = {{"Level1.dll", "Level1", PortType::Iq, 0.0}};
    std::vector<PluginApis> apis(1);
    apis[0].iq = &iq;

    Graph g;
    const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
    const NodeId dec = g.addNode(NodeKind::Decoder, "Level1", PortType::Iq);
    const NodeId text = g.addNode(NodeKind::Sink, "Text", PortType::Text);
    g.mutableNode(dec)->plugin = "Level1.dll";
    CHECK(g.connect(radio, 0, dec, 0) == Connect::Ok);
    CHECK(g.connect(dec, 0, text, 0) == Connect::Ok);
    const double rate = 2.4e6;
    const Plan plan = compile(g, rate, 145.0e6, &cat);
    CHECK(plan.decoders.size() == 1u);
    Runner r;
    r.publish(buildStripSet(plan, g, rate, kNoNode, 48000.0, &cat, &apis));
    std::vector<std::complex<float>> block(24000, std::complex<float>(0.1f, 0.0f));
    std::vector<PatchLine> got;
    finishesWithin(20000, [&] {
        // The patch runs its decoders on a real-time thread in the app (the
        // pipeline's DSP thread or a patch radio's reader): so does this.
        const RealtimeThreadScope realtime;
        for (int i = 0; i < 5; ++i) { r.process(block.data(), block.size()); }
    });
    for (int i = 0; i < 50 && got.empty(); ++i) {
        for (PatchLine& pl : r.drainText()) { got.push_back(std::move(pl)); }
        if (got.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    std::printf("  patch: %ld frames, %d state reads, %d log lines, %d marks, %d settings "
                "refusals, %zu text lines\n",
                g_patch.frames.load(), g_patch.stateOk.load(), g_patch.logOk.load(),
                g_patch.markOk.load(), g_patch.settingsWrongThread.load(), got.size());
    CHECK(g_patch.frames.load() == 5 * 24000);
    // Every call - the patch slices a long block, so there are more calls than
    // blocks - and every one of them reached the host and was answered.
    const int calls = g_patch.calls.load();
    CHECK(calls >= 5);
    CHECK(g_patch.stateOk.load() == calls);
    CHECK(g_patch.logOk.load() == calls);
    CHECK(g_patch.markOk.load() == calls);
    CHECK(g_patch.settingsWrongThread.load() == calls);
    bool sawLine = false;
    for (const PatchLine& pl : got) {
        if (pl.text.find("LEVEL1 OK") != std::string::npos) { sawLine = true; }
    }
    CHECK(sawLine);
    // And what it did through the host arrived there.
    std::vector<cascade::core::HostMarker> v;
    ui.api().markers(v);
    CHECK(v.size() == 1u);
    std::vector<cascade::core::PluginLogLine> lines;
    ui.api().takeLog(lines);
    CHECK(static_cast<int>(lines.size()) == calls);
    r.flushNow();
    g_patchHost = nullptr;
}

// A PROCESSOR-ONLY module is being fed signal, and its plate must say so -
// not "takes no signal", which is what a basemap is.
void testFittedState() {
    std::printf("fitted state of a processor\n");
    cascade::gui::FittedModule m;
    m.loaded = true;
    m.capabilities = CASCADE_CAP_AUDIO_PROCESSOR;
    m.fed = true;
    CHECK(cascade::gui::fittedState(m, true) == cascade::gui::FittedState::Fed);
    m.fed = false;
    CHECK(cascade::gui::fittedState(m, true) == cascade::gui::FittedState::NotFed);
    m.capabilities = CASCADE_CAP_BASEMAP;
    CHECK(cascade::gui::fittedState(m, true) == cascade::gui::FittedState::NoSignal);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("plugin API (host API level 1) tests\n");
    testFittedState();
    testDiscovery();
    testState();
    testControls();
    testThreads();
    testMarkers();
    testMarkerGeometry();
    testSettings();
    testConfigRoundTrip();
    testLog();
    testCommands();
    testDetach();
    testStreamInfo();
    testProcessorValidation();
    testProcessorChain();
    testProcessorInPipeline();
    testPatchPage();
    return testSummary("test_plugin_api");
}
