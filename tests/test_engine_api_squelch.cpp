// test_engine_api_squelch.cpp - AUTOMATIC SQUELCH in the engine (stub campaign B12, RAIL-B; 0.99.74), held against the real engine through the C table and the
// engine's own passes, with no window anywhere. The AUTO key under the top bar's SQUELCH dial was the window's in master (autoSquelchFrame, setSquelchAuto);
// it is the engine's now, so a headless engine, a web page and a CAT client honour it too, and these are the rules it carries. The radio is a fake of this
// file's own that puts out white noise at a level the test sets (the real binary's test_deck_knobs_app does the same through the window).
//
//   A1  THE OP: FOXAPP_OP_SET_SQUELCH_AUTO (0x842A) switches it, says what it is after, changes nothing for what it already is, refuses a value that is not 0
//       or 1, and is a LOCAL session's with the SETTINGS grant (not on the web profile); fact.squelch.auto follows it, and the config carries it;
//   A2  A HAND ON ANY THRESHOLD IS A MANUAL CHOICE: an accepted SET_SQUELCH (the dial, the Radio section's slider, the keys, a web page) and an accepted
//       AIRBAND squelch or above-the-noise turn it off, whoever sent them; a refused one does not; a threshold restored from the config does not;
//   A3  A PRESS OF "ABOVE THE NOISE" IS A HAND EVEN WHEN NO CHANNEL HAS REPORTED A LEVEL YET (the key is refused, AUTOMATIC is off all the same);
//   B1  THE RULE, on the receiver's channel: with it on, the threshold follows the measured noise floor + 8 dB - noise 20 dB louder puts it about 20 dB higher
//       - and fact.squelch.floorDb says what was measured;
//   B2  A CARRIER NEVER TEACHES THE FLOOR: a stretch of loud signal that holds the gate open for longer than the tracker's ten-second window leaves the
//       threshold where the quiet noise put it;
//   B3  A RETUNE, A STOPPED RECEIVER AND A SWITCH TURNED ON ARE EACH ANOTHER MEASUREMENT: the floor of the channel before is dropped;
//   C1  WHILE THE AIRBAND MONITOR LISTENS the rule is the monitor's "above the noise" applied to the monitor's own squelch, and the receiver's threshold is left
//       where it was; when the monitor stops the rule goes back to the receiver.
//
// Each was shown red against a stub of engine_squelch_auto.cpp (the op answering UNSUPPORTED, no rule) and then against a mutant of the code that holds it
// (docs/engine-stage4.md 6.22, B12).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "source/device_source.hpp"

namespace fs = std::filesystem;
namespace cc = cascade::core;

namespace {

// The level of the white noise the radio below puts out (total power, dB), set by the test between passes.
std::atomic<double> g_noiseDb{-50.0};

// engine_api_test::FakeRadio's figures, with a read() that returns white noise at g_noiseDb instead of silence (paced at the sample rate, so the DSP sees a
// radio and not a firehose). The test that listens to the generator needs none of this.
class NoiseRadio final : public cascade::source::DeviceSource {
public:
    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(1.0e6 * static_cast<double>(n) / rate_)));
        if (abort_) { return 0; }
        const float sigma = static_cast<float>(std::sqrt(std::pow(10.0, g_noiseDb.load() / 10.0) * 0.5));
        std::normal_distribution<float> dist(0.0f, sigma);
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {dist(rng_), dist(rng_)}; }
        return n;
    }
    const char* name() const override { return "Noise radio"; }
    const char* lastError() const override { return ""; }
    const char* driverKey() const override { return engine_api_test::kFakeRadioDriver; }
    bool open(const std::string&) override {
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return {}; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    std::mt19937 rng_{12345u};
};

std::unique_ptr<cascade::source::DeviceSource> makeNoiseRadio(const std::string&) { return std::make_unique<NoiseRadio>(); }

}  // namespace

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installNoiseRadio() {
        Engine::testHooks_.makeDevice = &makeNoiseRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static void removeFakeRadio() {
        Engine::testHooks_.makeDevice = nullptr;
        Engine::testHooks_.nativeScan = nullptr;
        Engine::testHooks_.soapyScan = nullptr;
    }
    static void seed(Engine& e, std::vector<cascade::core::Bookmark> rows) { e.freqMgr_.addMany(std::move(rows)); }
    // The pass's clock, moved on by `s` seconds: the tracker's ten-second window is crossed in a second of the test's own time.
    static void advanceClock(Engine& e, double s) {
        e.clockOrigin_ -= std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
    }
    static float squelchDb(Engine& e) { return e.squelchDb_; }
    static float airbandSquelchDb(Engine& e) { return e.airbandSquelchDb_; }
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_squelch_" + std::to_string(pid));
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

std::string fact(testing::Session& s, const std::string& key) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
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

// Passes for `ms` of the test's own time (the radio delivers in real time; the tracker reads the pass clock).
void passesFor(Engine& eng, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void passes(Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

FoxCommand op(std::uint32_t code, long long i0 = 0, long long i1 = 0, double n0 = 0.0, const char* text = nullptr) {
    FoxCommand c = testing::cmd(code, n0, i0, text);
    c.ival[1] = i1;
    return c;
}

testing::Outcome pumpedAs(Engine& eng, FoxSession* session, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(session, &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(session, r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}
testing::Outcome pumped(Engine& eng, testing::Session& s, const FoxCommand& c) { return pumpedAs(eng, s.raw(), c); }

struct Direct {
    Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

FoxCommand autoOp(int on) { return op(FOXAPP_OP_SET_SQUELCH_AUTO, on); }
FoxCommand squelchOp(double db) { return testing::cmd(FOXAPI_OP_SET_SQUELCH, db); }

bool autoOn(testing::Session& s) { return fact(s, "squelch.auto") == "1"; }
double squelchOf(testing::Session& s) { return s.state().squelchDb; }
// fact.squelch.floorDb as a number: NaN for "" (no measurement yet).
double floorOf(testing::Session& s) {
    const std::string f = fact(s, "squelch.floorDb");
    return f.empty() ? std::nan("") : std::atof(f.c_str());
}

// The noise radio open and the receiver running on it, the monitor's patch view out of the way.
bool startOnNoiseRadio(Direct& d, testing::Session& s, double noiseDb) {
    g_noiseDb = noiseDb;
    Access::installNoiseRadio();
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    const auto fakeId = [&] {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_DEVICES); ++i) {
            if (std::string(item(s, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i)).detail) == engine_api_test::kFakeRadioId) { return true; }
        }
        return false;
    };
    if (!passUntil(d.eng, fakeId)) { return false; }
    if (pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status() != FOXAPI_OK) { return false; }
    if (!passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; })) { return false; }
    if (pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status() != FOXAPI_OK) { return false; }
    return passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RUNNING) != 0u; });
}

// Passes until the rule has had its say: the threshold has left `from` (and is steady, which a floor that is still being measured is not).
bool thresholdMovedFrom(Direct& d, testing::Session& s, double from, int timeoutMs = 8000) {
    return passUntil(d.eng, [&] { return std::fabs(squelchOf(s) - from) > 3.0; }, timeoutMs);
}

// ---- A1: the op --------------------------------------------------------------------------------------------------------------------------

void testOp() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "squelch.auto"), std::string("0"));
    CHECK_EQ(fact(s, "squelch.floorDb"), std::string(""));

    testing::Outcome r = pumped(d.eng, s, autoOp(1));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 1.0);
    passes(d.eng, 2);
    CHECK(autoOn(s));
    // ASKED FOR WHAT IT ALREADY IS: nothing changes, and it says so.
    r = pumped(d.eng, s, autoOp(1));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 1.0);
    CHECK(autoOn(s));
    r = pumped(d.eng, s, autoOp(0));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 0.0);
    passes(d.eng, 2);
    CHECK(!autoOn(s));
    r = pumped(d.eng, s, autoOp(0));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 0.0);
    // A value that is neither.
    CHECK_EQ(pumped(d.eng, s, autoOp(2)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, autoOp(-1)).status(), FOXAPI_OUT_OF_RANGE);
    passes(d.eng, 2);
    CHECK(!autoOn(s));

    // THE CONFIG CARRIES IT, both ways: what the engine saves, and what a saved config brings back (quietly: no threshold is moved by it).
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    cc::AppConfig saved;
    d.eng.fillConfig(saved);
    CHECK(saved.squelchAuto);
    CHECK_EQ(pumped(d.eng, s, autoOp(0)).status(), FOXAPI_OK);
    d.eng.fillConfig(saved);
    CHECK(!saved.squelchAuto);
    cc::AppConfig restored;
    restored.squelchAuto = true;
    restored.squelchDb = -61.0f;
    d.eng.applyConfig(restored);
    passes(d.eng, 2);
    CHECK(autoOn(s));
    CHECK_EQ(squelchOf(s), -61.0);
    restored.squelchAuto = false;
    d.eng.applyConfig(restored);
    passes(d.eng, 2);
    CHECK(!autoOn(s));

    // ONLY A LOCAL SESSION WITH THE SETTINGS GRANT: a REMOTE one, one with no SETTINGS, and the web profile's own session are refused, and nothing changes.
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, kLocalGrants, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(pumped(d.eng, remote, autoOp(1)).status(), FOXAPI_DENIED);
    testing::Session noSettings(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE);
    CHECK(noSettings.ok());
    CHECK_EQ(pumped(d.eng, noSettings, autoOp(1)).status(), FOXAPI_DENIED);
    std::int32_t st = 0;
    FoxSession* web = d.eng.api().openInternalSession(cascade::engine::InternalKind::Web, "the web server", &st);
    CHECK_EQ(st, FOXAPI_OK);
    if (web != nullptr) {
        CHECK_EQ(pumpedAs(d.eng, web, autoOp(1)).status(), FOXAPI_DENIED);
        api()->close_session(web);
    }
    passes(d.eng, 2);
    CHECK(!autoOn(s));
    std::printf("A1: switched on and off, repeated (nothing changes), a value that is neither refused, the config both ways, a REMOTE session, one with no SETTINGS and the web profile refused\n");
}

// ---- A2: a hand on any threshold is a manual choice --------------------------------------------------------------------------------------------

void testManualChoice() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 2);

    // The dial, the slider, the keys and a web page all send this one op.
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return autoOn(s); }));
    CHECK_EQ(pumped(d.eng, s, squelchOp(-40.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!autoOn(s));
    CHECK_EQ(squelchOf(s), -40.0);
    // ...and a web session's own SET_SQUELCH is the same hand.
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    std::int32_t st = 0;
    FoxSession* web = d.eng.api().openInternalSession(cascade::engine::InternalKind::Web, "the web server", &st);
    CHECK_EQ(st, FOXAPI_OK);
    if (web != nullptr) {
        CHECK_EQ(pumpedAs(d.eng, web, squelchOp(-45.0)).status(), FOXAPI_OK);
        api()->close_session(web);
    }
    passes(d.eng, 2);
    CHECK(!autoOn(s));
    CHECK_EQ(squelchOf(s), -45.0);

    // A REFUSED THRESHOLD CHOSE NOTHING: out of the table's range, the switch stays where it was.
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, squelchOp(500.0)).status(), FOXAPI_OUT_OF_RANGE);
    passes(d.eng, 2);
    CHECK(autoOn(s));

    // THE MONITOR'S OWN SQUELCH (action 4) is a threshold too, listening or not; its above-the-noise key (action 3) is one only when it did something - with
    // nothing listening it is refused and AUTO stays.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 3)).status(), FOXAPI_NO_CHANGE);
    passes(d.eng, 2);
    CHECK(autoOn(s));
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 4, 0, -70.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!autoOn(s));
    CHECK_EQ(Access::airbandSquelchDb(d.eng), -70.0f);
    // The hold (action 5) is not a threshold.
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 5, 0, 3.0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(autoOn(s));
    std::printf("A2: SET_SQUELCH (local and the web profile's) and the monitor's squelch turn it off, a refused one, the hold and a refused above-the-noise do not\n");
}

// ---- B1: the rule ---------------------------------------------------------------------------------------------------------------------------

// The threshold the rule settles on for noise at `noiseDb`, from a start of -100 (a config that comes up with AUTO lit).
struct Settled {
    double threshold = 0.0;
    double floor = 0.0;
};
Settled settleOn(double noiseDb) {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Settled out;
    cc::AppConfig cfg;
    cfg.squelchDb = -100.0f;
    cfg.squelchAuto = true;
    d.eng.applyConfig(cfg);
    CHECK(startOnNoiseRadio(d, s, noiseDb));
    CHECK(thresholdMovedFrom(d, s, -100.0));
    passesFor(d.eng, 1500);
    out.threshold = squelchOf(s);
    out.floor = floorOf(s);
    CHECK(autoOn(s));
    return out;
}

void testRule() {
    const Settled lo = settleOn(-50.0);
    const Settled hi = settleOn(-30.0);
    std::printf("B1: threshold with the noise at -50 dB: %.1f (floor %.0f), at -30 dB: %.1f (floor %.0f)\n", lo.threshold, lo.floor, hi.threshold, hi.floor);
    CHECK(lo.threshold > -100.0 + 5.0);
    CHECK(lo.threshold < 0.0);
    // 20 dB more noise, about 20 dB more threshold.
    CHECK(std::fabs((hi.threshold - lo.threshold) - 20.0) < 4.0);
    // THE MARGIN IS 8 dB OVER WHAT WAS MEASURED, and fact.squelch.floorDb is what was measured (whole decibels).
    CHECK(!std::isnan(lo.floor) && !std::isnan(hi.floor));
    CHECK(std::fabs(lo.threshold - (lo.floor + 8.0)) < 2.0);
    CHECK(std::fabs(hi.threshold - (hi.floor + 8.0)) < 2.0);
}

// ---- B2: a carrier never teaches the floor ----------------------------------------------------------------------------------------------------

void testCarrier() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    cc::AppConfig cfg;
    cfg.squelchDb = -100.0f;
    cfg.squelchAuto = true;
    d.eng.applyConfig(cfg);
    CHECK(startOnNoiseRadio(d, s, -50.0));
    CHECK(thresholdMovedFrom(d, s, -100.0));
    passesFor(d.eng, 1500);
    const double quiet = squelchOf(s);
    const double quietFloor = floorOf(s);
    // A carrier 30 dB over the noise: over the threshold, so the gate opens and stays open. It holds the channel for fourteen seconds of the pass clock
    // (the tracker's window is ten), a second of that clock at a time with the radio delivering the carrier throughout.
    g_noiseDb = -20.0;
    for (int i = 0; i < 14; ++i) {
        Access::advanceClock(d.eng, 1.0);
        passesFor(d.eng, 150);
    }
    const double during = squelchOf(s);
    std::printf("B2: threshold from the quiet noise %.1f (floor %.0f), after a carrier held for 14 s %.1f (floor %.0f)\n", quiet, quietFloor, during, floorOf(s));
    CHECK(std::fabs(during - quiet) < 4.0);
    CHECK(during < quiet + 10.0);
    CHECK(autoOn(s));
}

// ---- B3: another channel, a stopped receiver, a fresh switch -----------------------------------------------------------------------------------

void testNewMeasurement() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    cc::AppConfig cfg;
    cfg.squelchDb = -100.0f;
    cfg.squelchAuto = true;
    d.eng.applyConfig(cfg);
    CHECK(startOnNoiseRadio(d, s, -50.0));
    CHECK(passUntil(d.eng, [&] { return !std::isnan(floorOf(s)); }));
    CHECK(thresholdMovedFrom(d, s, -100.0));

    // A RETUNE is another channel and another floor: the one before is dropped at once (the threshold it set stays until the new one has its own).
    const double before = squelchOf(s);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 400000.0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(std::isnan(floorOf(s)));
    CHECK_EQ(squelchOf(s), before);
    CHECK(autoOn(s));
    CHECK(passUntil(d.eng, [&] { return !std::isnan(floorOf(s)); }));

    // A new bandwidth, likewise.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 50000.0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(std::isnan(floorOf(s)));
    CHECK(passUntil(d.eng, [&] { return !std::isnan(floorOf(s)); }));

    // A STOPPED RECEIVER measures nothing, and what it held is not the channel it will start on.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(std::isnan(floorOf(s)));
    CHECK(autoOn(s));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);

    // A SWITCH TURNED ON starts afresh: off, on again, and the floor of before is not there.
    CHECK(passUntil(d.eng, [&] { return !std::isnan(floorOf(s)); }));
    CHECK_EQ(pumped(d.eng, s, autoOp(0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(std::isnan(floorOf(s)));
    std::printf("B3: a retune, another bandwidth, a stopped receiver and a switch turned on each dropped the floor\n");
}

// ---- C1: the Airband monitor listening --------------------------------------------------------------------------------------------------------

cc::Bookmark bm(const char* name, double hz, const char* mode, double bw, bool tick) {
    cc::Bookmark b;
    b.name = name;
    b.freqHz = hz;
    b.mode = mode;
    b.bandwidthHz = bw;
    b.scan = tick;
    return b;
}

void testAirbandRule() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // The built-in generator, as test_engine_api_airband listens to it: two ticked AM rows 600 kHz apart that share one centre.
    Access::seed(d.eng, {bm("Low", 119.7e6, "AM", 10000.0, true), bm("High", 120.3e6, "AM", 10000.0, true)});
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    cc::AppConfig cfg;
    cfg.squelchDb = -100.0f;
    cfg.squelchAuto = true;
    d.eng.applyConfig(cfg);
    passes(d.eng, 2);
    FoxCommand listen = op(FOXAPP_OP_AIRBAND, 1, 0, 0.0, "");
    CHECK_EQ(pumped(d.eng, s, listen).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "airband.listening") == "1"; }));
    CHECK_EQ(Access::airbandSquelchDb(d.eng), -55.0f);

    // WHILE IT LISTENS THE RULE IS THE MONITOR'S: its squelch leaves the default and the receiver's threshold is not moved.
    CHECK(passUntil(d.eng, [&] { return std::fabs(Access::airbandSquelchDb(d.eng) - (-55.0f)) > 1.0f; }, 10000));
    passesFor(d.eng, 600);
    const double applied = Access::airbandSquelchDb(d.eng);
    CHECK(autoOn(s));
    CHECK_EQ(squelchOf(s), -100.0);
    CHECK(std::isnan(floorOf(s)));
    // It is what "above the noise" gives from the channels' own levels (the key's rule, the one place it is written): press it by hand (which is a hand: AUTO
    // goes off) and the two agree to the half decibel the rule moves by.
    const testing::Outcome key = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 3));
    CHECK_EQ(key.status(), FOXAPI_OK);
    std::printf("C1: while listening AUTO put the monitor's squelch at %.1f dB, the key's rule gives %.1f; the receiver's stayed at %.1f\n", applied, key.result.applied[1],
                squelchOf(s));
    CHECK(std::fabs(applied - key.result.applied[1]) < 1.5);
    passes(d.eng, 2);
    CHECK(!autoOn(s));

    // AUTO ON AGAIN, then STOP: the rule goes back to the receiver, which starts a floor of its own.
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 0)).status(), FOXAPI_OK);
    const double stoppedAt = Access::airbandSquelchDb(d.eng);
    CHECK(passUntil(d.eng, [&] { return !std::isnan(floorOf(s)); }, 10000));
    CHECK(passUntil(d.eng, [&] { return std::fabs(squelchOf(s) - (-100.0)) > 3.0; }, 10000));
    CHECK_EQ(Access::airbandSquelchDb(d.eng), static_cast<float>(stoppedAt));   // the monitor's own is where it was left
    CHECK(autoOn(s));
}

// A PRESS OF "ABOVE THE NOISE" IS A HAND EVEN WHEN IT FINDS NOTHING TO WORK FROM: just after the monitor starts (or after AUTO rebuilt its block: every channel's level is
// forgotten until its next report) no channel has reported a level, the key is refused (NO_CHANGE) - and AUTOMATIC, which master turned off on the press, is off.
void testAboveTheNoiseBeforeALevel() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::seed(d.eng, {bm("Low", 119.7e6, "AM", 10000.0, true), bm("High", 120.3e6, "AM", 10000.0, true)});
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(pumped(d.eng, s, autoOp(1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 1, 0, 0.0, "")).status(), FOXAPI_OK);
    // The very next command: the monitor's channels have not reported yet.
    const testing::Outcome key = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 3));
    std::printf("A3: \"Above the noise\" the pass after LISTEN answered %d (%s)\n", key.status(), key.status() == FOXAPI_OK ? "a level was already in" : "no level yet");
    CHECK(key.status() == FOXAPI_OK || key.status() == FOXAPI_NO_CHANGE);
    passes(d.eng, 2);
    CHECK(!autoOn(s));
}

}  // namespace

int main() {
    std::printf("test_engine_api_squelch\n");
    isolate();
    testOp();
    testManualChoice();
    testRule();
    testCarrier();
    testNewMeasurement();
    testAirbandRule();
    testAboveTheNoiseBeforeALevel();
    Access::removeFakeRadio();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_squelch");
}
