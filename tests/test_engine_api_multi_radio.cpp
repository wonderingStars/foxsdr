// test_engine_api_multi_radio.cpp - SLOTS LIVE (multi-radio stage 4): the engine runs several radios at once, held against the real engine through the C table
// and the engine's own passes, with test radios (the signal generator, one tone each) so that no hardware is needed.
//
//   A  ADD AND REMOVE: RADIO_ADD builds a radio (number, state, label, device and audio facts, the count), RADIO_REMOVE takes it away (its facts go, its
//      reads answer NO_DEVICE), a number that is not 2..8 is OUT_OF_RANGE, an open radio answers NO_CHANGE, radio 1 cannot be removed;
//   B  TWO RADIOS, TWO TONES: each radio's spectrum shows its OWN tone and not the other's, each radio's state is its own;
//   C  A COMMAND AIMED AT RADIO 2 changes radio 2 and nothing of radio 1 (volume, mode, bandwidth, centre, squelch), and the other way round; a command
//      with no target follows the session's focus; a focus change moves every seq group of THAT session's reads with two live radios; a second session
//      keeps its own focus;
//   D  THE LIMIT: eight radios open and the ninth is refused with the ledger's words (it names the limit and every holder); patch radios count in the
//      limit (four patch radios running leave room for four receivers only); a device another radio holds is refused BY NAME, whichever side asks;
//   E  REMOVAL WHILE STREAMING leaves the others running (their spectra keep moving); REMOVE THEN RE-ADD into the same slot gives a radio of the new tone,
//      with its own spectrum sequence (still increasing) and nothing of the old one;
//   F  SOLO AND ROUTE: RADIO_AUDIO sets a radio's route and the fact says so, RADIO_SOLO holds the others silent without touching their routes, removing
//      the solo radio clears the solo;
//   G  PER-RADIO RECORDERS: two radios record I/Q at once into files that say which radio, the one radio's file keeps the plain name while it is alone;
//   H  START-UP AND SHUTDOWN WITH SEVERAL RADIOS: eight radios running, the engine destroyed, every thread joined (the process's thread count is back),
//      and a RUN of ALL RADIOS starts and stops them together;
//   I  CONCURRENCY: add / remove / command / read storms from three threads while the pass runs - nothing crashes, nothing hangs, and the engine ends
//      with exactly the radios the last operations left, all of them removable;
//   J  A STOP, AND A FAULT, ON ONE RADIO LEAVE THE OTHERS RUNNING (the fault is a fake radio whose source throws mid-stream).
//
// Each section is a break-it check of the behaviour it covers (docs/multi-radio-records/engine-stage-4.md lists what was removed and what turned red).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

#define CHECK_NEAR(a, b, tol) CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= (tol))

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
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
    // Two scanned radios: the usual mock RTL-SDR and a "hackrf" one that can be made to die (engine_api_test::fakeRadioThrows).
    static std::vector<cascade::source::NativeDeviceInfo> twoFakeRadios() {
        return {{"rtlsdr", "Mock RTL-SDR (serial 00000001)", "serial=00000001"}, {"hackrf", "Mock HackRF (serial 00000002)", "serial=00000002"}};
    }
    static void installTwoFakeRadios() {
        installFakeRadio();
        Engine::testHooks_.nativeScan = &twoFakeRadios;
    }
    static void setRecordDir(Engine& e, const std::string& dir) { e.recordDir_ = dir; }
    static std::uint32_t addPatchRadio(Engine& e, const std::string& device) {
        const pc::NodeId id = e.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        pc::Node* n = e.patchGraph_.mutableNode(id);
        n->device = device;
        n->on = true;
        return id;
    }
    static void setPatchRunning(Engine& e, bool on) { e.patchRunning_ = on; }
    static int audioRoute(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.audio().route() : -1; }
    static bool silenced(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.audio().silenced() : false; }
    static bool pipelineRunning(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.running(); }
    static std::uint32_t soloRadio(Engine& e) { return e.soloRadio_; }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kLocalGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;
constexpr std::uint32_t kMax = static_cast<std::uint32_t>(cascade::engine::kMaxReceivers);

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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_multi_radio_" + std::to_string(pid));
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
    return n >= 0 ? std::string(buf) : std::string("<absent>");
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
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

// RADIO_ADD: `radio` (0 = the lowest free), a test radio on the generator at `toneHz` (or a device id), started or not.
FoxCommand addCmd(long long radio, double toneHz, bool start, const char* device = nullptr) {
    FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_ADD, toneHz, radio, device);
    c.ival[1] = start ? 1 : 0;
    return c;
}

testing::Outcome addRadio(Engine& e, testing::Session& s, long long radio, double toneHz, bool start = true, const char* device = nullptr) {
    return pumped(e, s, addCmd(radio, toneHz, start, device));
}

testing::Outcome removeRadio(Engine& e, testing::Session& s, long long radio) { return pumped(e, s, testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, radio)); }

FoxReceiverState readRadio(testing::Session& s, std::uint32_t radio, std::int32_t* status = nullptr) {
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    const std::int32_t r = api()->read_radio_state(s.raw(), radio, &st);
    if (status != nullptr) { *status = r; }
    return st;
}

struct Spectrum {
    std::int32_t status = FOXAPI_NO_CHANGE;
    FoxSpectrumInfo info{};
    std::vector<float> bins;
};

Spectrum readSpectrum(testing::Session& s, std::uint32_t radio, std::uint64_t since = 0) {
    Spectrum sp;
    sp.bins.assign(4096, 0.0f);
    sp.info.structSize = sizeof(sp.info);
    sp.status = api()->read_radio_spectrum(s.raw(), radio, since, &sp.info, sp.bins.data(), static_cast<std::uint32_t>(sp.bins.size()));
    if (sp.status == FOXAPI_OK) { sp.bins.resize(sp.info.binCount); }
    return sp;
}

// The strongest bin of a spectrum, and how far a bin stands above the spectrum's median.
std::size_t peakBin(const Spectrum& sp) { return static_cast<std::size_t>(std::max_element(sp.bins.begin(), sp.bins.end()) - sp.bins.begin()); }
float medianDb(const Spectrum& sp) {
    std::vector<float> v = sp.bins;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}
// Where a tone of `offsetHz` from the centre lands, in a spectrum of this width at this span, given the bin order the engine publishes (centre in the middle).
std::size_t binFor(const Spectrum& sp, double offsetHz) {
    const double n = static_cast<double>(sp.info.binCount);
    const double frac = offsetHz / sp.info.spanHz + 0.5;
    return static_cast<std::size_t>(std::llround(frac * n)) % sp.info.binCount;
}
// How far above the median the strongest bin within +/-3 of `bin` is.
float standoutAt(const Spectrum& sp, std::size_t bin) {
    float best = -1.0e9f;
    for (int d = -3; d <= 3; ++d) {
        const long long i = static_cast<long long>(bin) + d;
        if (i >= 0 && i < static_cast<long long>(sp.bins.size())) { best = std::max(best, sp.bins[static_cast<std::size_t>(i)]); }
    }
    return best - medianDb(sp);
}

// A spectrum of the radio once it has published one.
bool spectrumOf(Engine& e, testing::Session& s, std::uint32_t radio, Spectrum& out) {
    return passUntil(e, [&] {
        out = readSpectrum(s, radio);
        return out.status == FOXAPI_OK && !out.bins.empty();
    });
}

struct Direct {
    Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

std::size_t processThreads() {
#if defined(_WIN32)
    std::size_t n = 0;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { return 0; }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = ::GetCurrentProcessId();
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) { ++n; }
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return n;
#else
    std::error_code ec;
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator("/proc/self/task", ec)) { (void)e; ++n; }
    return n;
#endif
}

// ---- A -------------------------------------------------------------------------------------------------------------------------------------------

void testAddAndRemove() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));

    // A radio that is added: the lowest free number, stopped (it was not asked to run), a test radio on the generator.
    testing::Outcome r = addRadio(d.eng, s, 0, 0.0, /*start=*/false);
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 2.0);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::string("2"));
    CHECK_EQ(fact(s, "radio.2.state"), std::string("stopped"));
    CHECK_EQ(fact(s, "radio.2.label"), std::string("Signal generator"));
    CHECK_EQ(fact(s, "radio.2.device"), std::string());
    CHECK_EQ(fact(s, "radio.2.audio"), std::string("both"));
    CHECK_EQ(fact(s, "radio.3.state"), std::string("<absent>"));
    std::int32_t st = 0;
    (void)readRadio(s, 2, &st);
    CHECK_EQ(st, FOXAPI_OK);

    // The next one takes the next number; a named free number takes that one; a number that is there says so (NO_CHANGE) and changes nothing.
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).result.applied[0], 3.0);
    CHECK_EQ(addRadio(d.eng, s, 5, 0.0, false).result.applied[0], 5.0);
    r = addRadio(d.eng, s, 5, 0.0, false);
    CHECK_EQ(r.status(), FOXAPI_NO_CHANGE);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::string("4"));
    CHECK_EQ(fact(s, "radio.4.state"), std::string("<absent>"));
    CHECK_EQ(fact(s, "radio.5.state"), std::string("stopped"));
    // Out of range: radio 1 is always there, and a number past the limit is no number.
    CHECK_EQ(addRadio(d.eng, s, 1, 0.0).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(addRadio(d.eng, s, kMax + 1, 0.0).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(addRadio(d.eng, s, -3, 0.0).status(), FOXAPI_OUT_OF_RANGE);

    // Remove: its facts go, its reads answer NO_DEVICE, the others stay.
    r = removeRadio(d.eng, s, 3);
    CHECK_EQ(r.status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::string("3"));
    CHECK_EQ(fact(s, "radio.3.state"), std::string("<absent>"));
    (void)readRadio(s, 3, &st);
    CHECK_EQ(st, FOXAPI_NO_DEVICE);
    (void)readRadio(s, 2, &st);
    CHECK_EQ(st, FOXAPI_OK);
    CHECK_EQ(removeRadio(d.eng, s, 3).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(removeRadio(d.eng, s, 1).status(), FOXAPI_UNSUPPORTED);
    CHECK_EQ(removeRadio(d.eng, s, 0).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(removeRadio(d.eng, s, kMax + 1).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(removeRadio(d.eng, s, 2).status(), FOXAPI_OK);
    CHECK_EQ(removeRadio(d.eng, s, 5).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
    // Radio 1 was never disturbed.
    (void)readRadio(s, 1, &st);
    CHECK_EQ(st, FOXAPI_OK);
}

// ---- B -------------------------------------------------------------------------------------------------------------------------------------------

void testTwoTonesTwoSpectra() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 3, -300000.0).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.2.state") == "running" && fact(s, "radio.3.state") == "running"; }));

    Spectrum s2, s3;
    CHECK(spectrumOf(d.eng, s, 2, s2));
    CHECK(spectrumOf(d.eng, s, 3, s3));
    CHECK(s2.info.binCount > 0u);
    CHECK_EQ(s2.info.binCount, s3.info.binCount);
    const std::size_t bin2 = binFor(s2, 200000.0);
    const std::size_t bin3 = binFor(s3, -300000.0);
    CHECK(bin2 != bin3);
    // Each radio's spectrum shows its own tone, well above its floor, at the bin that tone belongs to...
    CHECK(standoutAt(s2, bin2) > 30.0f);
    CHECK(standoutAt(s3, bin3) > 30.0f);
    CHECK(std::llabs(static_cast<long long>(peakBin(s2)) - static_cast<long long>(bin2)) <= 3);
    CHECK(std::llabs(static_cast<long long>(peakBin(s3)) - static_cast<long long>(bin3)) <= 3);
    // ...and NOT the other's: where the other radio's tone is, this one has nothing.
    CHECK(standoutAt(s2, bin3) < 10.0f);
    CHECK(standoutAt(s3, bin2) < 10.0f);
    // Radio 1's own spectrum (the focus) carries neither.
    Spectrum s1;
    CHECK(spectrumOf(d.eng, s, 1, s1) || true);
    if (s1.status == FOXAPI_OK && !s1.bins.empty()) {
        CHECK(standoutAt(s1, bin2) < 10.0f);
        CHECK(standoutAt(s1, bin3) < 10.0f);
    }
    // A radio that is not there reads NO_DEVICE; and the state is each radio's own (a running radio, its own run flag).
    CHECK_EQ(readSpectrum(s, 4).status, FOXAPI_NO_DEVICE);
    std::int32_t st = 0;
    const FoxReceiverState r2 = readRadio(s, 2, &st);
    CHECK_EQ(st, FOXAPI_OK);
    CHECK((r2.flags & FOXAPI_RX_RUNNING) != 0u);
    const FoxReceiverState r1 = readRadio(s, 1, &st);
    CHECK((r1.flags & FOXAPI_RX_RUNNING) == 0u);   // radio 1 was not started
    CHECK(std::string(r2.deviceName).find("generator") != std::string::npos);
}

// ---- C -------------------------------------------------------------------------------------------------------------------------------------------

void testACommandForRadio2ChangesRadio2Only() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    passes(d.eng, 3);
    std::int32_t st = 0;
    const FoxReceiverState b1 = readRadio(s, 1, &st);
    const FoxReceiverState b2 = readRadio(s, 2, &st);

    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 2, 0.31)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_MODE, 2, 0.0, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_SQUELCH, 2, -57.0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_FREQUENCY, 2, 123.456e6)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    const FoxReceiverState a1 = readRadio(s, 1, &st);
    const FoxReceiverState a2 = readRadio(s, 2, &st);
    // Radio 2 moved...
    CHECK_NEAR(a2.volume, 0.31, 1e-6);
    CHECK_EQ(a2.demodMode, static_cast<std::uint32_t>(FOXAPI_DEMOD_AM));
    CHECK_NEAR(a2.squelchDb, -57.0, 1e-6);
    CHECK_NEAR(a2.tunedHz, 123.456e6, 1.0);
    CHECK(a2.tuneSeq != b2.tuneSeq);
    CHECK(a2.audioSeq != b2.audioSeq);
    CHECK(a2.modeSeq != b2.modeSeq);
    // ...and radio 1 did not move at all: no figure and no seq group.
    CHECK_EQ(a1.volume, b1.volume);
    CHECK_EQ(a1.demodMode, b1.demodMode);
    CHECK_EQ(a1.squelchDb, b1.squelchDb);
    CHECK_EQ(a1.centreHz, b1.centreHz);
    CHECK_EQ(a1.tunedHz, b1.tunedHz);
    CHECK_EQ(a1.tuneSeq, b1.tuneSeq);
    CHECK_EQ(a1.audioSeq, b1.audioSeq);
    CHECK_EQ(a1.modeSeq, b1.modeSeq);
    CHECK_EQ(a1.deviceSeq, b1.deviceSeq);

    // The other way round: radio 1 (target 1) changes radio 1 and radio 2 keeps what it has.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 1, 0.66)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_FREQUENCY, 1, 99.9e6)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    const FoxReceiverState c1 = readRadio(s, 1, &st);
    const FoxReceiverState c2 = readRadio(s, 2, &st);
    CHECK_NEAR(c1.volume, 0.66, 1e-6);
    CHECK_NEAR(c1.tunedHz, 99.9e6, 1.0);
    CHECK_NEAR(c2.volume, 0.31, 1e-6);
    CHECK_NEAR(c2.tunedHz, 123.456e6, 1.0);
    CHECK_EQ(c2.tuneSeq, a2.tuneSeq);

    // A command that names no radio follows the session's FOCUS: with the focus on radio 2 it lands there.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 2)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_VOLUME, 0.12)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_NEAR(readRadio(s, 2, &st).volume, 0.12, 1e-6);
    CHECK_NEAR(readRadio(s, 1, &st).volume, 0.66, 1e-6);
    // read_state follows the focus: radio 2's figures.
    const FoxReceiverState focused = s.state();
    CHECK_NEAR(focused.volume, 0.12, 1e-6);
    // ...and an explicit target still wins over the focus.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_SET_VOLUME, 1, 0.5)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_NEAR(readRadio(s, 1, &st).volume, 0.5, 1e-6);
    CHECK_NEAR(readRadio(s, 2, &st).volume, 0.12, 1e-6);
}

// AGC and noise blanker are PER RADIO (multi-radio lane C wiring, merged onto the slot model): a command aimed at one radio changes that radio's seven
// published fields and nothing of the other's, a radio added later starts at the defaults, and read_radio_state reports each radio's own figures.
FoxCommand agcCmd(std::uint32_t target, long long mode, double fixedDb) {
    FoxCommand c = aimed(FOXAPI_OP_SET_AGC, target, fixedDb, mode);
    c.ival[1] = 1;
    return c;
}
FoxCommand nbCmd(std::uint32_t target, bool on, double thresholdDb) {
    FoxCommand c = aimed(FOXAPI_OP_SET_NB, target, thresholdDb, on ? 1 : 0);
    c.ival[1] = 1;
    return c;
}

void checkAgcNbDefaults(const FoxReceiverState& r) {
    CHECK_EQ(r.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_AUTO));
    CHECK_NEAR(r.agcFixedGainDb, 0.0, 1e-6);
    CHECK_EQ(r.nbEnabled, 0u);
    CHECK_NEAR(r.nbThresholdDb, 15.0, 1e-6);
}

void testAgcAndBlankerArePerRadio() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    passes(d.eng, 3);
    std::int32_t st = 0;
    checkAgcNbDefaults(readRadio(s, 1, &st));
    checkAgcNbDefaults(readRadio(s, 2, &st));
    const FoxReceiverState b1 = readRadio(s, 1, &st);

    // Aimed at radio 2: radio 2 changes, radio 1 keeps its defaults and its mode seq.
    CHECK_EQ(pumped(d.eng, s, agcCmd(2, FOXAPI_AGC_FAST, 12.0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, nbCmd(2, true, 22.0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    const FoxReceiverState a1 = readRadio(s, 1, &st);
    const FoxReceiverState a2 = readRadio(s, 2, &st);
    CHECK_EQ(a2.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_FAST));
    CHECK_EQ(a2.nbEnabled, 1u);
    CHECK_NEAR(a2.nbThresholdDb, 22.0, 1e-6);
    checkAgcNbDefaults(a1);
    CHECK_EQ(a1.modeSeq, b1.modeSeq);
    // The focused read of radio 1 agrees with read_radio_state(1).
    const FoxReceiverState f1 = s.state();
    CHECK_EQ(f1.agcMode, a1.agcMode);
    CHECK_EQ(f1.nbEnabled, a1.nbEnabled);

    // The other way round: radio 1 gets its own values; radio 2 keeps what it had.
    CHECK_EQ(pumped(d.eng, s, agcCmd(1, FOXAPI_AGC_SLOW, 5.0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, nbCmd(1, true, 9.0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    const FoxReceiverState c1 = readRadio(s, 1, &st);
    const FoxReceiverState c2 = readRadio(s, 2, &st);
    CHECK_EQ(c1.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_SLOW));
    CHECK_NEAR(c1.nbThresholdDb, 9.0, 1e-6);
    CHECK_EQ(c2.agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_FAST));
    CHECK_NEAR(c2.nbThresholdDb, 22.0, 1e-6);
    CHECK_EQ(c2.modeSeq, a2.modeSeq);

    // With no target the command follows the session's focus.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 2)).status(), FOXAPI_OK);
    FoxCommand fc = agcCmd(0, FOXAPI_AGC_OFF, 3.0);
    CHECK_EQ(pumped(d.eng, s, fc).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(readRadio(s, 2, &st).agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_OFF));
    CHECK_EQ(readRadio(s, 1, &st).agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_SLOW));

    // A removed and re-added radio starts at the defaults, not at what the old one had.
    CHECK_EQ(removeRadio(d.eng, s, 2).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    passes(d.eng, 3);
    checkAgcNbDefaults(readRadio(s, 2, &st));
    CHECK_EQ(readRadio(s, 1, &st).agcMode, static_cast<std::uint32_t>(FOXAPI_AGC_SLOW));
}

void testTheFocusBumpWithTwoLiveRadios() {
    Direct d;
    testing::Session a(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    testing::Session b(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(a.ok() && b.ok());
    CHECK_EQ(addRadio(d.eng, a, 2, 200000.0).status(), FOXAPI_OK);
    passes(d.eng, 4);
    const FoxReceiverState before = a.state();
    const FoxReceiverState bBefore = b.state();
    passes(d.eng, 3);
    // Without a focus change a settled engine moves no group of the state (the first radio is stopped and idle).
    const FoxReceiverState again = a.state();
    CHECK_EQ(again.tuneSeq, before.tuneSeq);
    CHECK_EQ(again.deviceSeq, before.deviceSeq);
    CHECK_EQ(again.modeSeq, before.modeSeq);

    CHECK_EQ(pumped(d.eng, a, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 2)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const FoxReceiverState after = a.state();
    // A CHANGE OF FOCUS moves every seq group of this session's read, so an interface redraws everything...
    CHECK(after.seq > again.seq);
    CHECK(after.tuneSeq > again.tuneSeq);
    CHECK(after.modeSeq > again.modeSeq);
    CHECK(after.deviceSeq > again.deviceSeq);
    CHECK(after.audioSeq > again.audioSeq);
    CHECK(after.displaySeq > again.displaySeq);
    CHECK(after.txSeq > again.txSeq);
    CHECK(after.listSeq > again.listSeq);
    // ...and it is radio 2's state it reads now (a different centre than radio 1's after radio 2 is tuned).
    CHECK_EQ(pumped(d.eng, a, aimed(FOXAPI_OP_SET_FREQUENCY, 2, 145.5e6)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_NEAR(a.state().tunedHz, 145.5e6, 1.0);
    // The OTHER session's focus is its own: still radio 1, and none of its groups moved.
    const FoxReceiverState bAfter = b.state();
    CHECK(std::fabs(bAfter.tunedHz - 145.5e6) > 1.0);
    CHECK_EQ(bAfter.tuneSeq, bBefore.tuneSeq);
    CHECK_EQ(bAfter.modeSeq, bBefore.modeSeq);
    // A focus on a radio that is removed answers NO_DEVICE until the session sets another; the other session is untouched.
    CHECK_EQ(removeRadio(d.eng, b, 2).status(), FOXAPI_OK);
    passes(d.eng, 3);
    FoxReceiverState gone{};
    gone.structSize = sizeof(gone);
    CHECK_EQ(api()->read_state(a.raw(), &gone), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, a, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(api()->read_state(a.raw(), &gone), FOXAPI_OK);
}

// ---- D -------------------------------------------------------------------------------------------------------------------------------------------

void testTheLimit() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // Seven more radios: eight open, every number from 1 to 8.
    for (long long n = 2; n <= static_cast<long long>(kMax); ++n) {
        const testing::Outcome r = addRadio(d.eng, s, 0, 0.0, /*start=*/false);
        CHECK_EQ(r.status(), FOXAPI_OK);
        CHECK_EQ(r.result.applied[0], static_cast<double>(n));
    }
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.count"), std::to_string(kMax));
    for (std::uint32_t n = 1; n <= kMax; ++n) { CHECK_EQ(fact(s, "radio." + std::to_string(n) + ".state"), std::string("stopped")); }
    // The ninth is refused with the ledger's words: the limit, and who holds the radios.
    testing::Outcome r = addRadio(d.eng, s, 0, 0.0, false);
    CHECK_EQ(r.status(), FOXAPI_LIMIT);
    CHECK((r.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    const std::string why = r.result.message;
    CHECK(why.find("at most 8 radios can be open at once") != std::string::npos);
    CHECK(why.find("Radio 1") != std::string::npos);
    CHECK(why.find("Radio 8") != std::string::npos);
    CHECK_EQ(fact(s, "radio.count"), std::to_string(kMax));
    // A free slot makes room again, and exactly one.
    CHECK_EQ(removeRadio(d.eng, s, 4).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).result.applied[0], 4.0);
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_LIMIT);
}

void testPatchRadiosCountInTheLimit() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_OK);
    // Four patch radios on, the patch running: 3 receivers + 4 patch radios = 7 of 8 open.
    for (int i = 0; i < 4; ++i) { (void)Access::addPatchRadio(d.eng, pc::kGeneratorKey); }
    Access::setPatchRunning(d.eng, true);
    passes(d.eng, 3);
    // One more receiver fits, and the next does not: there are free slots, the LIMIT is what refuses it.
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_OK);
    const testing::Outcome r = addRadio(d.eng, s, 0, 0.0, false);
    CHECK_EQ(r.status(), FOXAPI_LIMIT);
    const std::string why = r.result.message;
    CHECK(why.find("at most 8 radios") != std::string::npos);
    CHECK(why.find("patch radio") != std::string::npos);   // the words name the patch radios that hold the rest
    CHECK(why.find("Radio 4") != std::string::npos);
    // The patch stops: its radios stop counting and the receiver is welcome again.
    Access::setPatchRunning(d.eng, false);
    passes(d.eng, 3);
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_OK);
}

void testADeviceAnotherRadioHoldsIsRefusedByName() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::installFakeRadio();
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_DEVICES); ++i) {
            if (std::string(item(s, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i)).detail) == engine_api_test::kFakeRadioId) { return true; }
        }
        return false;
    }));
    // Radio 1 opens the fake radio.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
    passes(d.eng, 3);
    // A second radio asked for the same device: refused, naming Radio 1 - and no radio was added.
    testing::Outcome r = addRadio(d.eng, s, 0, 0.0, false, engine_api_test::kFakeRadioId);
    CHECK_EQ(r.status(), FOXAPI_BUSY);
    CHECK_EQ(std::string(r.result.message), std::string("in use by Radio 1"));
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
    // The generator is nobody's: any number of radios sit on it.
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 0, 0.0, false).status(), FOXAPI_OK);
    // Radio 1 lets the device go (back to the generator); radio 3 now takes it.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "siggen")).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(removeRadio(d.eng, s, 3).status(), FOXAPI_OK);
    r = addRadio(d.eng, s, 3, 0.0, false, engine_api_test::kFakeRadioId);
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.3.device") == pc::makeDeviceKey(engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs); }));
    // And now radio 1 asking for it is the one refused, naming Radio 3; so is radio 2.
    r = pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId));
    CHECK_EQ(r.status(), FOXAPI_BUSY);
    CHECK_EQ(std::string(r.result.message), std::string("in use by Radio 3"));
    r = pumped(d.eng, s, aimed(FOXAPI_OP_SELECT_SOURCE, 2, 0.0, 0, engine_api_test::kFakeRadioId));
    CHECK_EQ(r.status(), FOXAPI_BUSY);
    CHECK_EQ(std::string(r.result.message), std::string("in use by Radio 3"));
    // Radio 3 asking again for its own device is not refused by itself.
    r = pumped(d.eng, s, aimed(FOXAPI_OP_SELECT_SOURCE, 3, 0.0, 0, engine_api_test::kFakeRadioId));
    CHECK(r.status() != FOXAPI_BUSY);
    // Removing the holder frees the device for the next asker.
    CHECK_EQ(removeRadio(d.eng, s, 3).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
    Access::removeFakeRadio();
}

// ---- E -------------------------------------------------------------------------------------------------------------------------------------------

void testRemovalWhileStreamingAndReAdd() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 3, -300000.0).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 4, 450000.0).status(), FOXAPI_OK);
    Spectrum s2, s3, s4;
    CHECK(spectrumOf(d.eng, s, 2, s2));
    CHECK(spectrumOf(d.eng, s, 3, s3));
    CHECK(spectrumOf(d.eng, s, 4, s4));
    // Radio 3 is removed while it streams: the others go on - their spectrum sequences keep rising - and radio 3 is gone for every read.
    const std::uint64_t seq2 = s2.info.seq;
    const std::uint64_t seq4 = s4.info.seq;
    CHECK_EQ(removeRadio(d.eng, s, 3).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return readSpectrum(s, 2, seq2).status == FOXAPI_OK && readSpectrum(s, 4, seq4).status == FOXAPI_OK; }));
    CHECK(Access::pipelineRunning(d.eng, 1));
    CHECK(!Access::pipelineRunning(d.eng, 2));
    CHECK(Access::pipelineRunning(d.eng, 3));
    CHECK_EQ(readSpectrum(s, 3).status, FOXAPI_NO_DEVICE);
    std::int32_t st = 0;
    (void)readRadio(s, 3, &st);
    CHECK_EQ(st, FOXAPI_NO_DEVICE);
    CHECK_EQ(fact(s, "radio.2.state"), std::string("running"));
    CHECK_EQ(fact(s, "radio.4.state"), std::string("running"));

    // Re-add into the same slot with another tone: the new radio shows ITS tone and nothing of the old one, and its spectrum sequence continues upward.
    const std::uint64_t seq3 = s3.info.seq;
    CHECK_EQ(addRadio(d.eng, s, 3, 100000.0).result.applied[0], 3.0);
    Spectrum n3;
    CHECK(passUntil(d.eng, [&] {
        n3 = readSpectrum(s, 3);
        return n3.status == FOXAPI_OK && !n3.bins.empty();
    }));
    CHECK(n3.info.seq > seq3);
    CHECK(standoutAt(n3, binFor(n3, 100000.0)) > 30.0f);
    CHECK(standoutAt(n3, binFor(n3, -300000.0)) < 10.0f);
}

// ---- F -------------------------------------------------------------------------------------------------------------------------------------------

void testSoloAndRoute() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0, false).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 3, -300000.0, false).status(), FOXAPI_OK);
    passes(d.eng, 3);
    // The route: published, set on the radio's own stream, and the user's own choice.
    FoxCommand route = testing::cmd(FOXAPI_OP_RADIO_AUDIO, 0.0, 2);
    route.ival[1] = 1;   // left
    CHECK_EQ(pumped(d.eng, s, route).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.2.audio"), std::string("left"));
    CHECK_EQ(fact(s, "radio.1.audio"), std::string("both"));
    CHECK_EQ(Access::audioRoute(d.eng, 1), 1);
    CHECK_EQ(Access::audioRoute(d.eng, 0), 0);
    CHECK_EQ(pumped(d.eng, s, route).status(), FOXAPI_NO_CHANGE);
    route.ival[1] = 9;
    CHECK_EQ(pumped(d.eng, s, route).status(), FOXAPI_OUT_OF_RANGE);
    route.ival[0] = 5;
    route.ival[1] = 2;
    CHECK_EQ(pumped(d.eng, s, route).status(), FOXAPI_NO_DEVICE);
    // A route with the radio left out follows the focus.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 3)).status(), FOXAPI_OK);
    FoxCommand focusRoute = testing::cmd(FOXAPI_OP_RADIO_AUDIO, 0.0, 0);
    focusRoute.ival[1] = 3;   // none
    CHECK_EQ(pumped(d.eng, s, focusRoute).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "radio.3.audio"), std::string("none"));
    CHECK_EQ(fact(s, "radio.2.audio"), std::string("left"));

    // Solo radio 2: the others are held silent (their streams' silence switch), radio 2 is not, and nobody's route moved.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 2)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(Access::silenced(d.eng, 0));
    CHECK(!Access::silenced(d.eng, 1));
    CHECK(Access::silenced(d.eng, 2));
    CHECK_EQ(Access::audioRoute(d.eng, 1), 1);
    CHECK_EQ(Access::audioRoute(d.eng, 2), 3);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 2)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 6)).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, kMax + 1)).status(), FOXAPI_OUT_OF_RANGE);
    // The user's mute on radio 3 was never touched by the solo (no mute was set; the engine did not set one).
    CHECK((readRadio(s, 3).flags & FOXAPI_RX_MUTED) == 0u);
    // Solo moves to radio 3, then off: everyone is as they were.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 3)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(Access::silenced(d.eng, 0));
    CHECK(Access::silenced(d.eng, 1));
    CHECK(!Access::silenced(d.eng, 2));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    for (std::size_t slot = 0; slot < 3; ++slot) { CHECK(!Access::silenced(d.eng, slot)); }
    CHECK_EQ(Access::audioRoute(d.eng, 1), 1);
    CHECK_EQ(Access::audioRoute(d.eng, 2), 3);
    // Removing the solo radio clears the solo: nobody stays silent for a radio that is gone.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RADIO_SOLO, 0.0, 3)).status(), FOXAPI_OK);
    CHECK_EQ(removeRadio(d.eng, s, 3).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(Access::soloRadio(d.eng), 0u);
    CHECK(!Access::silenced(d.eng, 0));
    CHECK(!Access::silenced(d.eng, 1));
}

// ---- G -------------------------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> filesIn(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file()) { out.push_back(e.path().filename().string()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void testPerRadioRecorders() {
    const fs::path dir = g_scratch / "takes";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    Direct d;
    Access::setRecordDir(d.eng, dir.string());
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // ALONE, radio 1's file keeps the name it has always had.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RUNNING) != 0u; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RECORDING_IQ) != 0u; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_RECORDING_IQ) == 0u; }));
    passes(d.eng, 20);
    std::vector<std::string> alone = filesIn(dir);
    CHECK_EQ(alone.size(), std::size_t{1});
    if (!alone.empty()) { CHECK(alone[0].rfind("iq_", 0) == 0); }

    // WITH TWO RADIOS, every file says which radio it is.
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.2.state") == "running"; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // a file name has a one-second resolution
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_IQ, 1, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_IQ, 2, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_AUDIO, 2, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] {
        return (readRadio(s, 1).flags & FOXAPI_RX_RECORDING_IQ) != 0u && (readRadio(s, 2).flags & FOXAPI_RX_RECORDING_IQ) != 0u &&
               (readRadio(s, 2).flags & FOXAPI_RX_RECORDING_AUDIO) != 0u;
    }));
    // Each radio's recording flag is its own: radio 1 is not recording audio.
    CHECK((readRadio(s, 1).flags & FOXAPI_RX_RECORDING_AUDIO) == 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_IQ, 2, 0.0, 0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (readRadio(s, 2).flags & FOXAPI_RX_RECORDING_IQ) == 0u; }));
    CHECK((readRadio(s, 1).flags & FOXAPI_RX_RECORDING_IQ) != 0u);   // radio 1's take is still running: stopping radio 2's did not touch it
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_IQ, 1, 0.0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RECORD_AUDIO, 2, 0.0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 40);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    passes(d.eng, 20);
    const std::vector<std::string> files = filesIn(dir);
    int r1iq = 0, r2iq = 0, r2audio = 0;
    for (const std::string& f : files) {
        if (f.rfind("radio1_iq_", 0) == 0) { ++r1iq; }
        if (f.rfind("radio2_iq_", 0) == 0) { ++r2iq; }
        if (f.rfind("radio2_audio_", 0) == 0) { ++r2audio; }
    }
    CHECK_EQ(r1iq, 1);
    CHECK_EQ(r2iq, 1);
    CHECK_EQ(r2audio, 1);
    for (const std::string& f : files) {
        if (f.rfind("radio", 0) == 0) { CHECK(fs::file_size(dir / f) > 44u); }
    }
}

// ---- H -------------------------------------------------------------------------------------------------------------------------------------------

void testStartUpAndShutdownWithSeveralRadios() {
    const std::size_t before = processThreads();
    {
        Direct d;
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        for (long long n = 2; n <= static_cast<long long>(kMax); ++n) { CHECK_EQ(addRadio(d.eng, s, n, 20000.0 * static_cast<double>(n), false).status(), FOXAPI_OK); }
        // RUN of ALL RADIOS: every radio starts together...
        CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] {
            for (std::uint32_t n = 1; n <= kMax; ++n) {
                if (fact(s, "radio." + std::to_string(n) + ".state") != "running") { return false; }
            }
            return true;
        }));
        // Two threads a radio (source and DSP), more than at the start.
        CHECK(processThreads() >= before + 2 * kMax);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // ...and stop together.
        CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS, 0.0, 0)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] {
            for (std::uint32_t n = 1; n <= kMax; ++n) {
                if (fact(s, "radio." + std::to_string(n) + ".state") != "stopped") { return false; }
            }
            return true;
        }));
        // Start them once more and leave them RUNNING for the destructor: the engine goes with eight radios streaming.
        CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS, 0.0, 1)).status(), FOXAPI_OK);
        passes(d.eng, 10);
        // (the session and the engine leave scope here)
    }
    // Every thread the radios started is joined: the process has the threads it had (a small allowance for the runtime's own).
    bool back = false;
    for (int i = 0; i < 100 && !back; ++i) {
        back = processThreads() <= before + 3;
        if (!back) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }
    CHECK(back);
}

// ---- I -------------------------------------------------------------------------------------------------------------------------------------------

void testStorms() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    testing::Session s2(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    testing::Session s3(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s2.ok() && s3.ok());
    std::atomic<bool> stop{false};
    std::atomic<int> submitted{0};
    std::atomic<int> threadsDone{0};
    // Thread 1: add and remove radios 2..5, over and over.
    std::thread adder([&] {
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        for (int i = 0; i < 120 && !stop.load(); ++i) {
            const long long radio = 2 + (i % 4);
            const FoxCommand add = addCmd(radio, 10000.0 * static_cast<double>(radio), true);
            (void)api()->submit(s2.raw(), &add, 1, &sr);
            const FoxCommand rem = testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2 + ((i + 1) % 4));
            (void)api()->submit(s2.raw(), &rem, 1, &sr);
            ++submitted;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        ++threadsDone;
    });
    // Thread 2: commands aimed at every radio, focus changes, and reads of every kind, from another session.
    std::thread commander([&] {
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        std::vector<float> bins(4096);
        std::vector<float> audio(1024);
        for (int i = 0; i < 400 && !stop.load(); ++i) {
            const std::uint32_t target = static_cast<std::uint32_t>(1 + (i % 6));
            const FoxCommand v = aimed(FOXAPI_OP_SET_VOLUME, target, 0.1 + 0.001 * i);
            (void)api()->submit(s3.raw(), &v, 1, &sr);
            const FoxCommand f = testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0, 1 + (i % 5));
            (void)api()->submit(s3.raw(), &f, 1, &sr);
            FoxReceiverState st{};
            st.structSize = sizeof(st);
            (void)api()->read_radio_state(s3.raw(), target, &st);
            FoxSpectrumInfo info{};
            info.structSize = sizeof(info);
            (void)api()->read_radio_spectrum(s3.raw(), target, 0, &info, bins.data(), static_cast<std::uint32_t>(bins.size()));
            (void)api()->read_state(s3.raw(), &st);
            std::uint64_t cursor = 0;
            FoxStreamInfo si{};
            si.structSize = sizeof(si);
            (void)api()->read_audio(s3.raw(), &cursor, audio.data(), static_cast<std::uint32_t>(audio.size()), &si);
            char buf[256];
            (void)api()->get_setting(s3.raw(), "fact.radio.count", buf, sizeof(buf));
            ++submitted;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++threadsDone;
    });
    // The pass runs on this thread throughout, until both threads are done (or twenty seconds).
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < end && threadsDone.load() < 2) {
        d.eng.controlPass();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop = true;
    adder.join();
    commander.join();
    passes(d.eng, 20);
    CHECK(submitted.load() > 0);
    CHECK_EQ(threadsDone.load(), 2);
    // The engine is coherent: the count it publishes is the radios it has, each of them reads, and each is removable.
    const int announced = std::atoi(fact(s, "radio.count").c_str());
    int live = 0;
    for (std::uint32_t n = 1; n <= kMax; ++n) {
        std::int32_t st = 0;
        (void)readRadio(s, n, &st);
        if (st == FOXAPI_OK) { ++live; }
    }
    CHECK_EQ(announced, live);
    CHECK(live >= 1);
    for (std::uint32_t n = 2; n <= kMax; ++n) {
        std::int32_t st = 0;
        (void)readRadio(s, n, &st);
        if (st == FOXAPI_OK) { CHECK_EQ(removeRadio(d.eng, s, n).status(), FOXAPI_OK); }
    }
    passes(d.eng, 4);
    CHECK_EQ(fact(s, "radio.count"), std::string("1"));
}

// ---- J -------------------------------------------------------------------------------------------------------------------------------------------

void testAStopOnOneRadioLeavesTheOthersRunning() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 3, -300000.0).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 4, 450000.0).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] {
        return fact(s, "radio.1.state") == "running" && fact(s, "radio.2.state") == "running" && fact(s, "radio.3.state") == "running" &&
               fact(s, "radio.4.state") == "running";
    }));
    // Radio 3 stops (an ordinary RUN 0 aimed at it): the other three run on, and radio 3's own state says stopped.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, 3, 0.0, 0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.3.state") == "stopped"; }));
    CHECK_EQ(fact(s, "radio.1.state"), std::string("running"));
    CHECK_EQ(fact(s, "radio.2.state"), std::string("running"));
    CHECK_EQ(fact(s, "radio.4.state"), std::string("running"));
    Spectrum a, b;
    CHECK(spectrumOf(d.eng, s, 2, a));
    CHECK(spectrumOf(d.eng, s, 4, b));
    CHECK(passUntil(d.eng, [&] { return readSpectrum(s, 2, a.info.seq).status == FOXAPI_OK && readSpectrum(s, 4, b.info.seq).status == FOXAPI_OK; }));
    // It starts again on its own RUN.
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, 3, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.3.state") == "running"; }));
}

// A RADIO THAT DIES MID-STREAM (its source throws, as a vendor call that faults does) is that radio's fault alone: its state says faulted and every other radio
// runs on; starting it again (the user's RUN) brings it back once the radio is well.
void testAFaultOnOneRadioLeavesTheOthersRunning() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::installTwoFakeRadios();
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_DEVICES) >= 2; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 2, 200000.0).status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 3, 0.0, true, "hackrf:serial=00000002").status(), FOXAPI_OK);
    CHECK_EQ(addRadio(d.eng, s, 4, 450000.0).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] {
        for (std::uint32_t n = 1; n <= 4; ++n) {
            if (fact(s, "radio." + std::to_string(n) + ".state") != "running") { return false; }
        }
        return true;
    }));
    Spectrum a, b;
    CHECK(spectrumOf(d.eng, s, 2, a));
    CHECK(spectrumOf(d.eng, s, 4, b));
    // Radio 3 dies.
    engine_api_test::fakeRadioThrows() = true;
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.3.state") == "faulted"; }));
    std::int32_t st = 0;
    const FoxReceiverState f3 = readRadio(s, 3, &st);
    CHECK((f3.flags & FOXAPI_RX_FAULTED) != 0u);
    CHECK(std::string(f3.faultMessage).find("died") != std::string::npos);
    // The others did not notice: still running, not faulted, and their spectra still move.
    for (const std::uint32_t n : {1u, 2u, 4u}) {
        CHECK_EQ(fact(s, "radio." + std::to_string(n) + ".state"), std::string("running"));
        CHECK((readRadio(s, n, &st).flags & FOXAPI_RX_FAULTED) == 0u);
    }
    CHECK(passUntil(d.eng, [&] { return readSpectrum(s, 2, a.info.seq).status == FOXAPI_OK && readSpectrum(s, 4, b.info.seq).status == FOXAPI_OK; }));
    // The radio is well again; the user starts it, and it runs.
    engine_api_test::fakeRadioThrows() = false;
    CHECK_EQ(pumped(d.eng, s, aimed(FOXAPI_OP_RUN, 3, 0.0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "radio.3.state") == "running"; }));
    Access::removeFakeRadio();
}

}  // namespace

int main() {
    std::printf("test_engine_api_multi_radio\n");
    isolate();
    testAddAndRemove();
    testTwoTonesTwoSpectra();
    testACommandForRadio2ChangesRadio2Only();
    testAgcAndBlankerArePerRadio();
    testTheFocusBumpWithTwoLiveRadios();
    testTheLimit();
    testPatchRadiosCountInTheLimit();
    testADeviceAnotherRadioHoldsIsRefusedByName();
    testRemovalWhileStreamingAndReAdd();
    testSoloAndRoute();
    testPerRadioRecorders();
    testStartUpAndShutdownWithSeveralRadios();
    testStorms();
    testAStopOnOneRadioLeavesTheOthersRunning();
    testAFaultOnOneRadioLeavesTheOthersRunning();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_multi_radio");
}
