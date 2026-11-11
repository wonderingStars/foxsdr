// test_engine_api_saved_radios.cpp - THE SAVED SETTINGS OF RADIOS 2 TO 8, THE ENGINE'S SIDE (multi-radio engine, stage 5; docs/multi-radio-records/engine-stages-5-6.md).
//
// Held against the real engine through the C table and the engine's own passes, with test radios (the signal generator) and the fake RTL-SDR, so that no hardware is needed.
//
//   A  A ONE-RADIO USER SEES NO CHANGE: the old file loads, the one radio comes back with its settings, nothing else is created, the snapshot the engine would save
//      has no `radios` key;
//   B  ROUND TRIP, TWO TO EIGHT RADIOS: radios are set up through the API (centre, mode, bandwidth, squelch, volume, mute, AGC, blanker, notch, route, running), the
//      engine's snapshot is written through the file format and read back, and a second engine restored from it shows every radio as the first showed it;
//   C  A DEVICE RADIO comes back on its device (the fake RTL-SDR), and its saved entry names the device;
//   D  A MISSING DEVICE is reported by radio number and name (fact.radio.restore, fact.radio.N.refusal), the radio sits on the generator, the OTHER radios come
//      back, and the next save still names the missing device;
//   E  THE SAME DEVICE TWICE: the second entry is refused by the ledger, naming the holder; the radios after it are restored;
//   F  A DAMAGED ARRAY at the engine: radio 1 is whole, the engine starts, no extra radio exists;
//   G  THE USAGE REPORT'S radios_max counts the most receivers running at once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/patch_devices.hpp"
#include "core/telemetry.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;
using json = nlohmann::json;

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installFakeRadio() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static void removeFakeRadio() {
        Engine::testHooks_.makeDevice = nullptr;
        Engine::testHooks_.nativeScan = nullptr;
        Engine::testHooks_.soapyScan = nullptr;
    }
    static cascade::core::AppConfig snapshot(Engine& e) {
        e.publishConfig();
        return e.configSnapshot();
    }
    static void restore(Engine& e, const cascade::core::AppConfig& c) { e.applyConfig(c); }
    static std::uint32_t radiosMax(Engine& e) { return e.telemetryRadiosMax_; }
    static bool running(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.running(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kLocalGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;

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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_saved_radios_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

std::string fact(testing::Session& s, const std::string& key) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string("<absent>");
}

template <class F>
bool passUntil(Engine& eng, F done, int timeoutMs = 10000) {
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

testing::Outcome pumped(Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 30 && !o.landed; ++pass) {
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

FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
    FoxCommand c = testing::cmd(op, n0, i0, text);
    c.target = target;
    return c;
}

FoxReceiverState readRadio(testing::Session& s, std::uint32_t radio, std::int32_t* status = nullptr) {
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    const std::int32_t r = api()->read_radio_state(s.raw(), radio, &st);
    if (status != nullptr) { *status = r; }
    return st;
}

// An engine as the headless service builds it: API configured, initialised, THEN the file applied (EngineService::restore), then the first pass.
struct Restored {
    Engine eng;
    explicit Restored(const AppConfig* cfg = nullptr) {
        eng.configureApi(kHooks);
        eng.initialise();
        if (cfg != nullptr) { Access::restore(eng, cascade::core::startupState(*cfg)); }
        eng.controlPass();
    }
};

// What a file would hold: the engine's snapshot, serialised and read back (the format's own clamps included).
AppConfig throughTheFile(Engine& e) {
    AppConfig out;
    std::string error;
    const std::string text = ConfigStore::serialize(Access::snapshot(e));
    CHECK(ConfigStore::parse(text, "test", out, error));
    return out;
}

void addGenerator(Engine& e, testing::Session& s, long long number, double toneHz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_ADD, toneHz, number);
    CHECK_EQ(pumped(e, s, c).status(), FOXAPI_OK);
}

void sameState(const FoxReceiverState& a, const FoxReceiverState& b) {
    CHECK_EQ(a.centreHz, b.centreHz);
    CHECK_EQ(a.vfoOffsetHz, b.vfoOffsetHz);
    CHECK_EQ(a.bandwidthHz, b.bandwidthHz);
    CHECK_EQ(a.squelchDb, b.squelchDb);
    CHECK_EQ(a.volume, b.volume);
    CHECK_EQ(a.demodMode, b.demodMode);
    CHECK_EQ(a.deemphasis, b.deemphasis);
    CHECK_EQ(a.nrStrength, b.nrStrength);
    CHECK_EQ(a.notchHz, b.notchHz);
    CHECK_EQ(a.notchQ, b.notchQ);
    CHECK_EQ(a.agcMode, b.agcMode);
    CHECK_EQ(a.agcFixedGainDb, b.agcFixedGainDb);
    CHECK_EQ(a.nbEnabled, b.nbEnabled);
    CHECK_EQ(a.nbThresholdDb, b.nbThresholdDb);
    const std::uint32_t mask = FOXAPI_RX_RUNNING | FOXAPI_RX_MUTED | FOXAPI_RX_NR | FOXAPI_RX_NOTCH | FOXAPI_RX_AUTO_NOTCH | FOXAPI_RX_STEREO_ENABLED;
    CHECK_EQ(a.flags & mask, b.flags & mask);
}

FoxCommand audioCmd(long long radio, long long route) {
    FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_AUDIO, 0.0, radio);
    c.ival[1] = route;
    return c;
}

// Sets radio `n` up with values that tell the radios apart.
void setUp(Engine& e, testing::Session& s, std::uint32_t n, bool run) {
    const double base = 7.0e6 + 1.0e6 * n;
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_CENTRE, n, base)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_MODE, n, 0.0, n % 2 == 0 ? FOXAPI_DEMOD_USB : FOXAPI_DEMOD_NFM)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_BANDWIDTH, n, n % 2 == 0 ? 2800.0 : 12500.0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_VFO_OFFSET, n, 1000.0 * n)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_SQUELCH, n, -70.0 + n)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_VOLUME, n, 0.05 * n)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_MUTED, n, 0.0, n % 2)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_DEEMPHASIS, n, 0.0, n % 3)).status(), FOXAPI_OK);
    FoxCommand nr = aimed(FOXAPI_OP_SET_NR, n, 0.1 * n, n % 2);
    nr.ival[1] = 1;
    CHECK_EQ(pumped(e, s, nr).status(), FOXAPI_OK);
    FoxCommand agc = aimed(FOXAPI_OP_SET_AGC, n, 3.0 + n, n % 5);
    agc.ival[1] = 1;
    CHECK_EQ(pumped(e, s, agc).status(), FOXAPI_OK);
    FoxCommand nb = aimed(FOXAPI_OP_SET_NB, n, 7.0 + n, (n + 1) % 2);
    nb.ival[1] = 1;
    CHECK_EQ(pumped(e, s, nb).status(), FOXAPI_OK);
    FoxCommand notch = aimed(FOXAPI_OP_SET_NOTCH, n, 300.0 * n, n % 2);
    notch.ival[1] = 1;
    notch.num[1] = 5.0 + n;
    CHECK_EQ(pumped(e, s, notch).status(), FOXAPI_OK);
    CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_SET_AUTO_NOTCH, n, 0.0, (n + 1) % 2)).status(), FOXAPI_OK);
    {
        const int32_t st = pumped(e, s, audioCmd(n, n % 4)).status();
        CHECK(st == FOXAPI_OK || st == FOXAPI_NO_CHANGE);
    }
    if (run) { CHECK_EQ(pumped(e, s, aimed(FOXAPI_OP_RUN, n, 0.0, 1)).status(), FOXAPI_OK); }
}

// ---- A -------------------------------------------------------------------------------------------------------------------------------------------

const char* kOldFile = R"({
  "schemaVersion": 1, "sourceKind": "siggen", "centerHz": 98500000.0, "mode": "AM", "bandwidthHz": 9000.0, "squelchDb": -61.0, "volume": 0.35,
  "sampleRateHz": 2000000.0, "stereoEnabled": true, "deemphasisIndex": 1, "nrEnabled": true, "nrStrength": 0.8, "agcMode": 1, "vfoOffsetHz": 120000.0
})";

void testAOneRadioUserSeesNoChange() {
    AppConfig cfg;
    std::string error;
    CHECK(ConfigStore::parse(kOldFile, "old", cfg, error));
    CHECK(cfg.radios.empty());
    Restored r(&cfg);
    testing::Session s(api(), r.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(r.eng, 4);
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
    const FoxReceiverState st = readRadio(s, 1);
    CHECK_EQ(st.centreHz, 98500000.0);
    CHECK_EQ(st.demodMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_AM));
    CHECK_EQ(st.bandwidthHz, 9000.0);
    CHECK_EQ(st.squelchDb, -61.0);
    CHECK(std::fabs(st.volume - 0.35) < 1e-6);
    CHECK_EQ(st.deemphasis, 1u);
    CHECK_EQ(st.agcMode, 1u);
    CHECK_EQ(st.vfoOffsetHz, 120000.0);
    CHECK_EQ(fact(s, "radio.restore"), std::string("<absent>"));
    // the file this engine would save has neither new key
    const json saved = json::parse(ConfigStore::serialize(Access::snapshot(r.eng)));
    CHECK(!saved.contains("radios"));
    CHECK(!saved.contains("pluginRadio"));
    CHECK_EQ(saved["schemaVersion"].get<int>(), 1);
    CHECK_EQ(saved["centerHz"].get<double>(), 98500000.0);
    CHECK_EQ(saved["mode"].get<std::string>(), std::string("AM"));
}

// ---- B -------------------------------------------------------------------------------------------------------------------------------------------

void roundTrip(std::uint32_t radios) {
    AppConfig file;
    std::vector<FoxReceiverState> before(radios + 1);
    {
        Restored a;
        testing::Session s(api(), a.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_SET_CENTRE, 1, 145.8e6)).status(), FOXAPI_OK);
        CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 1, 0.6)).status(), FOXAPI_OK);
        for (std::uint32_t n = 2; n <= radios; ++n) { addGenerator(a.eng, s, n, 20000.0 * n); }
        for (std::uint32_t n = 2; n <= radios; ++n) { setUp(a.eng, s, n, n % 2 == 0); }
        passes(a.eng, 6);
        for (std::uint32_t n = 1; n <= radios; ++n) { before[n] = readRadio(s, n); }
        // the fact says what the engine will save
        file = throughTheFile(a.eng);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(radios - 1));
        for (std::size_t i = 0; i < file.radios.size(); ++i) { CHECK_EQ(file.radios[i].number, static_cast<int>(i) + 2); }
    }
    // A SECOND ENGINE from the file alone.
    Restored b(&file);
    testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(b.eng, 6);
    CHECK_EQ(fact(s, "radio.count"), std::to_string(radios));
    for (std::uint32_t n = 2; n <= radios; ++n) {
        std::int32_t status = 0;
        const FoxReceiverState after = readRadio(s, n, &status);
        CHECK_EQ(status, FOXAPI_OK);
        sameState(before[n], after);
        CHECK_EQ(fact(s, "radio." + std::to_string(n) + ".audio"), std::string(cascade::engine::radioRouteWord(static_cast<int>(n % 4))));
        CHECK_EQ(fact(s, "radio." + std::to_string(n) + ".state"), n % 2 == 0 ? std::string("running") : std::string("stopped"));
    }
    // radio 1 came back as the top-level keys say
    const FoxReceiverState one = readRadio(s, 1);
    CHECK_EQ(one.centreHz, 145.8e6);
    CHECK(std::fabs(one.volume - 0.6) < 1e-6);
    // and a save from the restored engine is the file it was restored from (nothing drifts)
    const AppConfig again = throughTheFile(b.eng);
    CHECK_EQ(again.radios.size(), file.radios.size());
    for (std::size_t i = 0; i < again.radios.size() && i < file.radios.size(); ++i) { CHECK(again.radios[i] == file.radios[i]); }
}

void testRoundTrips() {
    roundTrip(2);
    roundTrip(3);
    roundTrip(8);
}

// ---- C -------------------------------------------------------------------------------------------------------------------------------------------

void testADeviceRadioComesBackOnItsDevice() {
    Access::installFakeRadio();
    AppConfig file;
    {
        Restored a;
        testing::Session s(api(), a.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        CHECK_EQ(pumped(a.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
        CHECK(passUntil(a.eng, [&] {
            for (int32_t i = 0; i < api()->read_list(s.raw(), FOXAPI_LIST_DEVICES, 0, nullptr); ++i) {
                FoxListItem it{};
                it.structSize = sizeof(it);
                api()->read_list(s.raw(), FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i), &it);
                if (std::string(it.detail) == engine_api_test::kFakeRadioId) { return true; }
            }
            return false;
        }));
        FoxCommand add = testing::cmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, engine_api_test::kFakeRadioId);
        CHECK_EQ(pumped(a.eng, s, add).status(), FOXAPI_OK);
        CHECK(passUntil(a.eng, [&] { return (readRadio(s, 2).flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
        CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_SET_CENTRE, 2, 433.9e6)).status(), FOXAPI_OK);
        passes(a.eng, 5);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].sourceKind, std::string(engine_api_test::kFakeRadioDriver));
        CHECK_EQ(file.radios[0].args, std::string(engine_api_test::kFakeRadioArgs));
        CHECK_EQ(file.radios[0].centerHz, 433.9e6);
        CHECK(file.radios[0].sampleRateHz > 0.0);
    }
    Restored b(&file);
    testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(b.eng, 6);
    CHECK_EQ(fact(s, "radio.count"), std::string("2"));
    const FoxReceiverState st = readRadio(s, 2);
    CHECK((st.flags & FOXAPI_RX_DEVICE_OPEN) != 0u);
    CHECK_EQ(st.centreHz, 433.9e6);
    CHECK_EQ(fact(s, "radio.2.device"), pc::makeDeviceKey(engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs));
    CHECK_EQ(fact(s, "radio.restore"), std::string("<absent>"));
    // radio 1 is untouched by the device on radio 2
    CHECK((readRadio(s, 1).flags & FOXAPI_RX_DEVICE_OPEN) == 0u);
    Access::removeFakeRadio();
}

// ---- D -------------------------------------------------------------------------------------------------------------------------------------------

AppConfig fileWith(const std::string& radios) {
    AppConfig out;
    std::string error;
    CHECK(ConfigStore::parse(R"({"schemaVersion": 1, "centerHz": 100000000.0, "mode": "WFM", "radios": )" + radios + "}", "test", out, error));
    return out;
}

void testAMissingDeviceIsReportedAndTheOthersComeBack() {
    Access::installFakeRadio();
    const AppConfig file = fileWith(R"([
        {"number": 2, "sourceKind": "rtlsdr", "args": "serial=MISSING", "centerHz": 88500000.0, "mode": "WFM", "volume": 0.4},
        {"number": 3, "sourceKind": "siggen", "toneHz": 40000.0, "centerHz": 99900000.0, "mode": "AM", "volume": 0.2, "audioRoute": 2},
        {"number": 4, "sourceKind": "rtlsdr", "args": "serial=00000001", "centerHz": 433000000.0}])");
    Restored b(&file);
    testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(b.eng, 6);
    // all three radios exist; the first of them is on the generator, and says which radio it was and what it was
    CHECK_EQ(fact(s, "radio.count"), std::string("4"));
    CHECK((readRadio(s, 2).flags & FOXAPI_RX_DEVICE_OPEN) == 0u);
    const std::string refusal = fact(s, "radio.2.refusal");
    CHECK(refusal != "<absent>");
    CHECK(refusal.find("RTL-SDR") != std::string::npos);   // the radio's name, as the first radio's own sentence gives it
    CHECK(refusal.find("did not open") != std::string::npos);
    const std::string notes = fact(s, "radio.restore");
    CHECK(notes.find("Radio 2:") != std::string::npos);
    CHECK(notes.find("Radio 3") == std::string::npos);
    CHECK(notes.find("Radio 4") == std::string::npos);
    // the others came back with everything: the generator radio and the radio on the fake device
    const FoxReceiverState r3 = readRadio(s, 3);
    CHECK_EQ(r3.centreHz, 99.9e6);
    CHECK_EQ(r3.demodMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_AM));
    CHECK(std::fabs(r3.volume - 0.2) < 1e-6);
    CHECK_EQ(fact(s, "radio.3.audio"), std::string("right"));
    const FoxReceiverState r4 = readRadio(s, 4);
    CHECK((r4.flags & FOXAPI_RX_DEVICE_OPEN) != 0u);
    CHECK_EQ(r4.centreHz, 433.0e6);
    // radio 1 is whole
    CHECK_EQ(readRadio(s, 1).centreHz, 100.0e6);
    // the missing radio's settings were applied to the generator that stands in for it
    CHECK_EQ(readRadio(s, 2).centreHz, 88.5e6);
    // and the NEXT SAVE still names the missing device: one session without the dongle does not forget it
    passes(b.eng, 3);
    const AppConfig again = throughTheFile(b.eng);
    CHECK_EQ(again.radios.size(), static_cast<std::size_t>(3));
    CHECK_EQ(again.radios[0].number, 2);
    CHECK_EQ(again.radios[0].sourceKind, std::string("rtlsdr"));
    CHECK_EQ(again.radios[0].args, std::string("serial=MISSING"));
    CHECK_EQ(again.radios[0].centerHz, 88.5e6);
    CHECK_EQ(again.radios[1].sourceKind, std::string("siggen"));
    CHECK_EQ(again.radios[2].args, std::string("serial=00000001"));
    Access::removeFakeRadio();
}

// ---- E -------------------------------------------------------------------------------------------------------------------------------------------

void testTheSameDeviceTwice() {
    Access::installFakeRadio();
    const AppConfig file = fileWith(R"([
        {"number": 2, "sourceKind": "rtlsdr", "args": "serial=00000001"},
        {"number": 3, "sourceKind": "rtlsdr", "args": "serial=00000001"},
        {"number": 4, "sourceKind": "siggen", "toneHz": 70000.0, "centerHz": 50000000.0}])");
    Restored b(&file);
    testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(b.eng, 6);
    CHECK_EQ(fact(s, "radio.count"), std::string("3"));   // radios 1, 2 and 4: the second claim on the device made no radio 3
    CHECK((readRadio(s, 2).flags & FOXAPI_RX_DEVICE_OPEN) != 0u);
    std::int32_t status = 0;
    (void)readRadio(s, 3, &status);
    CHECK_EQ(status, FOXAPI_NO_DEVICE);
    const std::string notes = fact(s, "radio.restore");
    CHECK(notes.find("Radio 3: in use by Radio 2") != std::string::npos);
    CHECK_EQ(readRadio(s, 4).centreHz, 50.0e6);
    Access::removeFakeRadio();
}

// ---- F -------------------------------------------------------------------------------------------------------------------------------------------

void testADamagedArrayAtTheEngine() {
    for (const char* bad : {"\"nope\"", "[1, 2, [3]]", "{\"number\": 2}", "[{\"number\": \"x\", \"centerHz\": -5, \"volume\": 9, \"agcMode\": 77}]"}) {
        const AppConfig file = fileWith(bad);
        Restored b(&file);
        testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        passes(b.eng, 4);
        CHECK_EQ(readRadio(s, 1).centreHz, 100.0e6);
        CHECK_EQ(readRadio(s, 1).demodMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_WFM));
        const std::string count = fact(s, "radio.count");
        CHECK(count == "1" || count == "2");
        if (count == "2") {
            // the one clamped entry: a radio on the generator with every value inside its range
            const FoxReceiverState r = readRadio(s, 2);
            CHECK(r.volume <= 1.0 && r.volume >= 0.0);
            CHECK(r.agcMode <= 4u);
        }
    }
}

// ---- G -------------------------------------------------------------------------------------------------------------------------------------------

void testTheUsageReportCountsTheMostRadiosRunning() {
    // the report's field, clamped to 1..8
    cascade::core::TelemetryReport rep;
    CHECK_EQ(json::parse(rep.toJson())["radios_max"].get<int>(), 1);
    rep.session.radiosMax = 0;
    CHECK_EQ(json::parse(rep.toJson())["radios_max"].get<int>(), 1);
    rep.session.radiosMax = 5;
    CHECK_EQ(json::parse(rep.toJson())["radios_max"].get<int>(), 5);
    rep.session.radiosMax = 99;
    CHECK_EQ(json::parse(rep.toJson())["radios_max"].get<int>(), 8);
    // and the engine's counter: stopped radios do not count, running ones do, and it never goes down
    Restored a;
    testing::Session s(api(), a.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(a.eng, 3);
    CHECK_EQ(Access::radiosMax(a.eng), 1u);
    addGenerator(a.eng, s, 2, 20000.0);
    addGenerator(a.eng, s, 3, 30000.0);
    passes(a.eng, 3);
    CHECK_EQ(Access::radiosMax(a.eng), 1u);   // three radios open, none running
    CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_RUN, 2, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_RUN, 3, 0.0, 1)).status(), FOXAPI_OK);
    passes(a.eng, 3);
    CHECK_EQ(Access::radiosMax(a.eng), 2u);
    CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    passes(a.eng, 3);
    CHECK_EQ(Access::radiosMax(a.eng), 3u);
    CHECK_EQ(pumped(a.eng, s, aimed(FOXAPI_OP_RUN, 3, 0.0, 0)).status(), FOXAPI_OK);
    passes(a.eng, 3);
    CHECK_EQ(Access::radiosMax(a.eng), 3u);
}

}  // namespace

int main() {
    std::printf("test_engine_api_saved_radios\n");
    isolate();
    testAOneRadioUserSeesNoChange();
    testRoundTrips();
    testADeviceRadioComesBackOnItsDevice();
    testAMissingDeviceIsReportedAndTheOthersComeBack();
    testTheSameDeviceTwice();
    testADamagedArrayAtTheEngine();
    testTheUsageReportCountsTheMostRadiosRunning();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_saved_radios");
}
