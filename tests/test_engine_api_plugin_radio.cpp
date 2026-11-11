// test_engine_api_plugin_radio.cpp - PLUGINS PER RADIO (multi-radio engine, stage 6; docs/multi-radio-records/engine-stages-5-6.md).
//
// Held against the real engine through the C table and its own passes, with test radios (the signal generator, one tone each) and a REAL plugin module
// (tests/fixtures/radio_probe_plugin.cpp, copied under several names: each copy is a plugin of its own) that says where it runs by what it is fed.
//
//   A  A PLUGIN IS ON RADIO 1 until it is put elsewhere: fact.plugin.<file>.radio, fact.radio.<n>.plugins, and what it is fed (the first radio's tones, the centre it was told);
//   B  PLUGIN_RADIO MOVES IT: each radio's plugin is fed its own radio's I/Q (the tone of THAT radio, that radio's centre), its host get_state answers from that radio, the
//      other runners hold nothing of it; the op's answers (NO_CHANGE, NOT_FOUND, NO_DEVICE, OUT_OF_RANGE);
//   C  A PLUGIN'S set_frequency MOVES ITS OWN RADIO ONLY;
//   D  MOVING A RUNNING PLUGIN IN A LOOP, with a reader thread beside the pass: no crash, no deadlock, no instance left behind, no thread left;
//   E  REMOVING A PLUGIN'S RADIO stops it cleanly: back on radio 1, stopped, nothing fed, the other radios untouched;
//   F  THE CHOICE SURVIVES A RESTART (the settings file's `pluginRadio`), and falls back to radio 1 when its radio is not open;
//   G  A PLUGIN THAT FAULTS ON RADIO 2 does not disturb radio 1's decoders or audio path (nor radio 3's), and neither does one that HANGS there (a call that does not return for
//      1.5 s on radio 2's DSP thread): radio 1's decoder is fed throughout;
//   H  THE SCANNER stays on the radio it was started on and says so (fact.scanner.radio); the TRANSMITTER follows radio 1 unless a Pluto is another radio (the rule is a pure function,
//      tested on its own), and fact.transmit.radio says which.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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
#include "core/config.hpp"
#include "core/plugin_fault.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/transmit_radio.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "fault_probe_names.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace cc = cascade::core;
using cascade::engine::Engine;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

fs::path g_scratch;
fs::path g_plugins;
fs::path g_logs;
std::string g_fixture;

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
std::string pluginDirText() { return g_plugins.string(); }

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirText;
    }
    static bool runnerHas(Engine& e, std::size_t slot, const std::string& file) { return e.runnerOf(slot).hasInstances(file); }
    static bool hasRunner(Engine& e, std::size_t slot) { return slot == 0 || e.radioRunners_[slot] != nullptr; }
    static cascade::core::AppConfig snapshot(Engine& e) {
        e.publishConfig();
        return e.configSnapshot();
    }
    static void restore(Engine& e, const cascade::core::AppConfig& c) { e.applyConfig(c); }
    static std::size_t transmitSlot(const Engine& e) { return e.transmitRadioSlot(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

int g_scenario = 0;

std::string leafOf(const std::string& id) { return id + CASCADE_PROBE_SUFFIX; }

// A parsed log line: key=value pairs.
using Line = std::map<std::string, std::string>;

Line lastLine(const std::string& file) {
    std::ifstream in(g_logs / (file + ".log"), std::ios::binary);
    std::string line, last;
    while (std::getline(in, line)) {
        if (!line.empty()) { last = line; }
    }
    Line out;
    std::istringstream ss(last);
    std::string tok;
    while (ss >> tok) {
        const std::size_t eq = tok.find('=');
        if (eq != std::string::npos) { out[tok.substr(0, eq)] = tok.substr(eq + 1); }
    }
    return out;
}
std::size_t lineCount(const std::string& file) {
    std::ifstream in(g_logs / (file + ".log"), std::ios::binary);
    std::string line;
    std::size_t n = 0;
    while (std::getline(in, line)) { n += line.empty() ? 0u : 1u; }
    return n;
}
double num(const Line& l, const char* k) {
    const auto it = l.find(k);
    return it == l.end() ? -1.0e18 : std::atof(it->second.c_str());
}

// One engine with the fixture copied under `ids` into a plugin folder of its own, and a log folder of its own.
struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;
    std::vector<std::string> files;

    explicit Rig(const std::vector<std::string>& ids, const AppConfig* cfg = nullptr) {
        ++g_scenario;
        std::error_code ec;
        g_plugins = g_scratch / ("plugins" + std::to_string(g_scenario));
        g_logs = g_scratch / ("logs" + std::to_string(g_scenario));
        fs::create_directories(g_plugins, ec);
        fs::create_directories(g_logs, ec);
        engine_api_test::setEnv("RADIO_PROBE_DIR", g_logs.string());
        for (const std::string& id : ids) {
            const std::string leaf = leafOf(id + std::to_string(g_scenario));
            fs::copy_file(g_fixture, g_plugins / leaf, fs::copy_options::overwrite_existing, ec);
            files.push_back(leaf);
        }
        eng.configureApi(kHooks);
        eng.initialise();
        if (cfg != nullptr) { Access::restore(eng, cascade::core::startupState(*cfg)); }
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(s->ok());
        pump(3);
    }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    template <class F>
    bool until(F done, int timeoutMs = 10000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return false;
    }
    testing::Outcome send(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxReceiverState radio(std::uint32_t n, std::int32_t* status = nullptr) {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        const std::int32_t r = api()->read_radio_state(s->raw(), n, &st);
        if (status != nullptr) { *status = r; }
        return st;
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
    void addRadio(long long number, double tone, bool run = true) {
        FoxCommand c = testing::cmd(FOXAPI_OP_RADIO_ADD, tone, number);
        c.ival[1] = run ? 1 : 0;
        CHECK_EQ(send(c).status(), FOXAPI_OK);
    }
    testing::Outcome place(const std::string& file, long long radio, bool start = false) {
        FoxCommand c = testing::cmd(FOXAPI_OP_PLUGIN_RADIO, 0.0, radio, file.c_str());
        c.ival[1] = start ? 1 : 0;
        return send(c);
    }
    std::string pluginsOn(std::uint32_t radioNumber) { return fact("radio." + std::to_string(radioNumber) + ".plugins"); }
    // The PLUGINS row's run word (value[3]): bits 0-1 how it runs (0 auto, 1 always, 2 stopped).
    int runWord(const std::string& file) {
        const int32_t count = api()->read_list(s->raw(), FOXAPI_LIST_PLUGINS, 0, nullptr);
        for (int32_t i = 0; i < count; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            api()->read_list(s->raw(), FOXAPI_LIST_PLUGINS, static_cast<std::uint32_t>(i), &it);
            if (file == it.detail) { return static_cast<int>(it.value[3]); }
        }
        return -1;
    }
    std::size_t instancesOf(const std::string& file) {
        std::size_t n = 0;
        for (std::size_t slot = 0; slot < cascade::engine::kMaxReceivers; ++slot) {
            if (Access::hasRunner(eng, slot) && Access::runnerHas(eng, slot, file)) { ++n; }
        }
        return n;
    }
    // Waits until the plugin's log has a line newer than `before` lines, and returns it.
    Line fresh(const std::string& file, std::size_t before, int timeoutMs = 10000) {
        (void)until([&] { return lineCount(file) > before; }, timeoutMs);
        return lastLine(file);
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

// Three radios: 1 (centre 14.1 MHz, the engine's default two tones), 2 (21 MHz, a 90 kHz tone), 3 (28 MHz, a -150 kHz tone), all running.
void threeRadios(Rig& r) {
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 14.1e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    r.addRadio(2, 90000.0);
    r.addRadio(3, -150000.0);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 21.0e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 3, 28.0e6)).status(), FOXAPI_OK);
    r.pump(3);
}

// ---- A -------------------------------------------------------------------------------------------------------------------------------------------

void testDefaultIsRadioOne() {
    std::printf("  A: a plugin is on radio 1 until it is put elsewhere\n");
    Rig r({"rp-a"});
    const std::string a = r.files[0];
    threeRadios(r);
    CHECK(r.until([&] { return lineCount(a) > 0; }));
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("1"));
    CHECK(r.pluginsOn(1).find(a) != std::string::npos);
    CHECK_EQ(r.pluginsOn(2), std::string());
    CHECK_EQ(r.pluginsOn(3), std::string());
    CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(1));
    CHECK(Access::runnerHas(r.eng, 0, a));
    const Line l = r.fresh(a, lineCount(a));
    // the first radio's two generator tones: the stronger one (+300 kHz) leads the estimate, the other pulls it a few kHz
    CHECK(std::fabs(num(l, "tone_hz") - 300000.0) < 25000.0);
    CHECK_EQ(num(l, "centre_now"), 14.1e6);
    CHECK_EQ(num(l, "state_centre"), 14.1e6);
    CHECK_EQ(num(l, "state_running"), 1.0);
}

// ---- B -------------------------------------------------------------------------------------------------------------------------------------------

void testMoveFeedsTheRadioItIsOn() {
    std::printf("  B: PLUGIN_RADIO moves it; it is fed its own radio and nothing else\n");
    Rig r({"rp-a"});
    const std::string a = r.files[0];
    threeRadios(r);
    CHECK(r.until([&] { return lineCount(a) > 0; }));
    // the answers of the op
    CHECK_EQ(r.place(a, 1).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.place(a, 9).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.place(a, 0).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.place(a, 5).status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(r.place("no-such-1.2.3-abi3-win-x64.dll", 2).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("1"));
    CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(1));

    // to radio 2: its tone, its centre, and nothing left in the first runner
    const std::size_t n0 = lineCount(a);
    const testing::Outcome moved = r.place(a, 2);
    CHECK_EQ(moved.status(), FOXAPI_OK);
    CHECK_EQ(moved.result.applied[0], 2.0);
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("2"));
    CHECK(r.until([&] { return r.pluginsOn(2).find(a) != std::string::npos; }));
    CHECK_EQ(r.pluginsOn(1), std::string());
    CHECK(!Access::runnerHas(r.eng, 0, a) && Access::runnerHas(r.eng, 1, a) && !Access::runnerHas(r.eng, 2, a));
    CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(1));
    Line l = r.fresh(a, n0);
    CHECK(std::fabs(num(l, "tone_hz") - 90000.0) < 2000.0);
    CHECK_EQ(num(l, "centre_create"), 21.0e6);
    CHECK_EQ(num(l, "state_centre"), 21.0e6);   // get_state answers from the plugin's radio
    // a retune of radio 2 reaches the plugin; a retune of radio 1 does not
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 22.5e6)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 10.0e6)).status(), FOXAPI_OK);
    r.pump(3);
    l = r.fresh(a, lineCount(a));
    CHECK_EQ(num(l, "centre_now"), 22.5e6);
    CHECK_EQ(num(l, "state_centre"), 22.5e6);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 1, 14.1e6)).status(), FOXAPI_OK);

    // to radio 3
    const std::size_t n1 = lineCount(a);
    CHECK_EQ(r.place(a, 3).status(), FOXAPI_OK);
    l = r.fresh(a, n1);
    CHECK(std::fabs(num(l, "tone_hz") - (-150000.0)) < 2000.0);
    CHECK_EQ(num(l, "state_centre"), 28.0e6);
    CHECK(Access::runnerHas(r.eng, 2, a) && !Access::runnerHas(r.eng, 1, a));
    CHECK(r.pluginsOn(3).find(a) != std::string::npos);

    // and back to radio 1
    const std::size_t n2 = lineCount(a);
    CHECK_EQ(r.place(a, 1).status(), FOXAPI_OK);
    l = r.fresh(a, n2);
    CHECK(std::fabs(num(l, "tone_hz") - 300000.0) < 25000.0);
    CHECK_EQ(num(l, "state_centre"), 14.1e6);
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("1"));
    CHECK(Access::runnerHas(r.eng, 0, a) && r.instancesOf(a) == 1u);

    // THE DECODERS LIST OF EACH RADIO says where the plugin runs: ACTIVE and FED on its own radio only, value[0] its radio everywhere
    const auto find = [&](std::uint32_t radioNo, FoxListItem& out) {
        const int32_t count = api()->read_radio_list(r.s->raw(), radioNo, FOXAPI_LIST_DECODERS, 0, nullptr);
        for (int32_t i = 0; i < count; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            api()->read_radio_list(r.s->raw(), radioNo, FOXAPI_LIST_DECODERS, static_cast<std::uint32_t>(i), &it);
            if (std::string(it.name).find(a.substr(0, a.find('-', 3))) != std::string::npos) {
                out = it;
                return true;
            }
        }
        return false;
    };
    CHECK_EQ(r.place(a, 2).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.pluginsOn(2).find(a) != std::string::npos; }));
    for (std::uint32_t radioNo = 1; radioNo <= 3; ++radioNo) {
        FoxListItem it{};
        CHECK(find(radioNo, it));
        CHECK_EQ(static_cast<int>(it.value[0]), 2);                                      // the radio it runs on, on every radio's list
        CHECK_EQ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u, radioNo == 2u);                  // running ON THIS radio
    }
    FoxListItem on2{};
    CHECK(find(2, on2));
    CHECK((on2.flags & FOXAPI_ITEM_FED) != 0u);
    FoxListItem on1{};
    CHECK(find(1, on1));
    CHECK((on1.flags & FOXAPI_ITEM_FED) == 0u);
    // read_list follows the focus: with the focus on radio 2 it is radio 2's
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 2)).status(), FOXAPI_OK);
    FoxListItem focused{};
    bool seen = false;
    const int32_t n = api()->read_list(r.s->raw(), FOXAPI_LIST_DECODERS, 0, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        api()->read_list(r.s->raw(), FOXAPI_LIST_DECODERS, static_cast<std::uint32_t>(i), &it);
        if (std::string(it.name).find(a.substr(0, a.find('-', 3))) != std::string::npos) {
            focused = it;
            seen = true;
        }
    }
    CHECK(seen);
    CHECK((focused.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_FOCUS, 0.0, 1)).status(), FOXAPI_OK);
}

// ---- C -------------------------------------------------------------------------------------------------------------------------------------------

void testASetFrequencyMovesItsOwnRadio() {
    std::printf("  C: a plugin's set_frequency moves its own radio only\n");
    Rig r({"rp-a"});
    const std::string a = r.files[0];
    threeRadios(r);
    CHECK(r.until([&] { return lineCount(a) > 0; }));
    FoxCommand grant = testing::cmd(FOXAPI_OP_PLUGIN_GRANT, 0.0, 1, a.c_str());
    grant.ival[1] = 1;
    CHECK_EQ(r.send(grant).status(), FOXAPI_OK);
    CHECK_EQ(r.place(a, 2).status(), FOXAPI_OK);
    r.pump(5);
    const double t1 = r.radio(1).tunedHz, t3 = r.radio(3).tunedHz;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_PLUGIN_COMMAND, 0.0, 1, a.c_str())).status(), FOXAPI_OK);
    CHECK(r.until([&] { return std::fabs(r.radio(2).tunedHz - 7.2e6) < 1.0; }));
    CHECK_EQ(r.radio(2).tunedHz, 7.2e6);
    CHECK_EQ(r.radio(1).tunedHz, t1);
    CHECK_EQ(r.radio(3).tunedHz, t3);
    const Line l = r.fresh(a, lineCount(a));
    CHECK_EQ(num(l, "asked"), 0.0);   // CASCADE_API_OK: accepted and queued
    // the same op from radio 3's plugin moves radio 3 (and the focus of the session was radio 1 all along)
    CHECK_EQ(r.place(a, 3).status(), FOXAPI_OK);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 3, 28.0e6)).status(), FOXAPI_OK);
    r.pump(3);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_PLUGIN_COMMAND, 0.0, 1, a.c_str())).status(), FOXAPI_OK);
    CHECK(r.until([&] { return std::fabs(r.radio(3).tunedHz - 7.2e6) < 1.0; }));
    CHECK_EQ(r.radio(1).tunedHz, t1);
}

// ---- D -------------------------------------------------------------------------------------------------------------------------------------------

void testMovingInALoop() {
    std::printf("  D: moving a running plugin in a loop\n");
    const std::size_t before = processThreads();
    {
        Rig r({"rp-a", "rp-b"});
        const std::string a = r.files[0], b = r.files[1];
        threeRadios(r);
        CHECK(r.until([&] { return lineCount(a) > 0 && lineCount(b) > 0; }));
        std::atomic<bool> stop{false};
        std::atomic<long> reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                for (std::uint32_t n = 1; n <= 3; ++n) {
                    FoxReceiverState st{};
                    st.structSize = sizeof(st);
                    (void)api()->read_radio_state(r.s->raw(), n, &st);
                }
                const int32_t count = api()->read_list(r.s->raw(), FOXAPI_LIST_PLUGINS, 0, nullptr);
                for (int32_t i = 0; i < count; ++i) {
                    FoxListItem it{};
                    it.structSize = sizeof(it);
                    (void)api()->read_list(r.s->raw(), FOXAPI_LIST_PLUGINS, static_cast<std::uint32_t>(i), &it);
                }
                char buf[1024];
                (void)api()->get_setting(r.s->raw(), ("fact.plugin." + a + ".radio").c_str(), buf, sizeof(buf));
                ++reads;
            }
        });
        const auto start = std::chrono::steady_clock::now();
        int failed = 0;
        for (int i = 0; i < 160; ++i) {
            const long long radio = 1 + (i * 7 + i / 3) % 3;
            const long long other = 1 + (i * 5 + 1) % 3;
            const int32_t sa = r.place(a, radio).status();
            const int32_t sb = r.place(b, other).status();
            if (sa != FOXAPI_OK && sa != FOXAPI_NO_CHANGE) { ++failed; }
            if (sb != FOXAPI_OK && sb != FOXAPI_NO_CHANGE) { ++failed; }
            if (i % 8 == 0) { r.pump(2); }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stop.store(true);
        reader.join();
        CHECK_EQ(failed, 0);
        CHECK(seconds < 120.0);
        CHECK(reads.load() > 0);
        r.pump(5);
        // nothing leaked, nothing duplicated: each plugin has exactly one instance, on the radio it says
        CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(1));
        CHECK_EQ(r.instancesOf(b), static_cast<std::size_t>(1));
        for (const std::string& f : {a, b}) {
            const int radioNo = std::atoi(r.fact("plugin." + f + ".radio").c_str());
            CHECK(radioNo >= 1 && radioNo <= 3);
            if (radioNo >= 1 && radioNo <= 3) { CHECK(Access::runnerHas(r.eng, static_cast<std::size_t>(radioNo) - 1u, f)); }
        }
        // still fed after the storm
        const std::size_t na = lineCount(a), nb = lineCount(b);
        CHECK(r.until([&] { return lineCount(a) > na && lineCount(b) > nb; }));
    }
    bool back = false;
    for (int i = 0; i < 100 && !back; ++i) {
        back = processThreads() <= before + 3;
        if (!back) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    }
    CHECK(back);
}

// ---- E -------------------------------------------------------------------------------------------------------------------------------------------

void testRemovingTheRadioStopsThePlugin() {
    std::printf("  E: removing a plugin's radio stops it cleanly\n");
    Rig r({"rp-a", "rp-b"});
    const std::string a = r.files[0], b = r.files[1];
    threeRadios(r);
    CHECK_EQ(r.place(a, 3).status(), FOXAPI_OK);
    CHECK_EQ(r.place(b, 2).status(), FOXAPI_OK);
    CHECK(r.until([&] { return lineCount(a) > 0 && lineCount(b) > 0; }));
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("3"));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 3)).status(), FOXAPI_OK);
    r.pump(4);
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("1"));   // back on radio 1...
    CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(0));        // ...with nothing running...
    CHECK_EQ(r.runWord(a) & 3, 2);                                  // ...and STOPPED
    CHECK(!Access::hasRunner(r.eng, 2));
    const std::size_t frozen = lineCount(a);
    // radio 2's plugin is untouched and keeps being fed; radio 1 runs
    const std::size_t nb = lineCount(b);
    CHECK(r.until([&] { return lineCount(b) > nb + 1; }));
    CHECK_EQ(r.instancesOf(b), static_cast<std::size_t>(1));
    CHECK_EQ(r.radio(1).flags & FOXAPI_RX_RUNNING, FOXAPI_RX_RUNNING);
    CHECK_EQ(lineCount(a), frozen);
    // a radio added again in the same place does not bring the plugin back
    r.addRadio(3, 150000.0);
    r.pump(5);
    CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(0));
    CHECK_EQ(r.pluginsOn(3), std::string());
    CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("1"));
    // and the user can start it again anywhere
    CHECK_EQ(r.place(a, 3, /*start=*/true).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.instancesOf(a) == 1u && r.pluginsOn(3).find(a) != std::string::npos; }));
}

// ---- F -------------------------------------------------------------------------------------------------------------------------------------------

void testTheChoiceSurvivesARestart() {
    std::printf("  F: the choice is saved (and a plugin on radio 1 is the absence of an entry)\n");
    Rig r({"rp-a"});
    const std::string plugin = r.files[0];
    threeRadios(r);
    CHECK_EQ(r.place(plugin, 2).status(), FOXAPI_OK);
    r.pump(3);
    AppConfig file;
    std::string error;
    CHECK(ConfigStore::parse(ConfigStore::serialize(Access::snapshot(r.eng)), "test", file, error));
    CHECK_EQ(file.radios.size(), static_cast<std::size_t>(2));
    CHECK_EQ(file.pluginRadio.size(), static_cast<std::size_t>(1));
    CHECK_EQ(file.pluginRadio.begin()->first, cc::pluginRunId(plugin));
    CHECK_EQ(file.pluginRadio.begin()->second, 2);
    CHECK_EQ(r.place(plugin, 1).status(), FOXAPI_OK);
    r.pump(2);
    CHECK(Access::snapshot(r.eng).pluginRadio.empty());
}

void testRestoreByIdAndFallback() {
    std::printf("  F2: restore by plugin id; a radio that is not open reads as radio 1\n");
    // The Rig names its copies "<id><scenario>" and the saved choice is by plugin id, so the test writes the id of the copy it is about to create.
    const int next = g_scenario + 1;
    AppConfig cfg;
    std::string error;
    CHECK(ConfigStore::parse(R"({"radios": [{"number": 2, "sourceKind": "siggen", "toneHz": 90000, "centerHz": 21000000, "running": true}], "pluginRadio": {")" + cc::pluginRunId(leafOf("rp-a" + std::to_string(next))) +
                                 R"(": 2}})",
                             "test", cfg, error));
    CHECK_EQ(cfg.pluginRadio.size(), static_cast<std::size_t>(1));
    {
        Rig r({"rp-a"}, &cfg);
        const std::string a = r.files[0];
        CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("2"));
        CHECK(r.until([&] { return lineCount(a) > 0; }));
        const Line l = r.fresh(a, 0);
        CHECK(std::fabs(num(l, "tone_hz") - 90000.0) < 2000.0);
        CHECK_EQ(num(l, "centre_create"), 21.0e6);
        CHECK(Access::runnerHas(r.eng, 1, a) && !Access::runnerHas(r.eng, 0, a));
    }
    {
        // radio 5 is in the file's plugin map but not in its radios: the plugin runs on radio 1
        const int next2 = g_scenario + 1;
        AppConfig cfg2;
        CHECK(ConfigStore::parse(R"({"radios": [{"number": 2, "sourceKind": "siggen", "toneHz": 90000}], "pluginRadio": {")" + cc::pluginRunId(leafOf("rp-a" + std::to_string(next2))) +
                                     R"(": 5}})",
                                 "test", cfg2, error));
        Rig r({"rp-a"}, &cfg2);
        const std::string a = r.files[0];
        CHECK_EQ(r.fact("plugin." + a + ".radio"), std::string("1"));
        CHECK(Access::runnerHas(r.eng, 0, a));
        CHECK(Access::snapshot(r.eng).pluginRadio.empty());
    }
}

// ---- G -------------------------------------------------------------------------------------------------------------------------------------------

void testAFaultOrAHangOnRadioTwoLeavesTheOthers() {
    std::printf("  G: a fault, and a hang, on radio 2 leave radio 1's decoder and audio path running\n");
#if !defined(_WIN32)
    std::printf("    SKIPPED: the guard's absorbing needs Windows' structured exceptions\n");
    return;
#else
    cc::fault::resetForTest();
    {
        Rig r({"rp-a", "rp-b", "rp-c"});
        const std::string a = r.files[0], b = r.files[1], c = r.files[2];   // a: the abuser, on radio 2; b: radio 1; c: radio 3
        threeRadios(r);
        CHECK_EQ(r.place(a, 2).status(), FOXAPI_OK);
        CHECK_EQ(r.place(c, 3).status(), FOXAPI_OK);
        CHECK(r.until([&] { return lineCount(a) > 0 && lineCount(b) > 0 && lineCount(c) > 0; }));
        const std::size_t nb = lineCount(b), nc = lineCount(c);
        // a faults in poll_text() on radio 2's DSP thread (command key 2)
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_PLUGIN_COMMAND, 0.0, 2, a.c_str())).status(), FOXAPI_OK);
        CHECK(r.until([&] { return cc::fault::isQuarantinedKey(a); }, 15000));
        r.pump(4);
        CHECK(cc::fault::isQuarantinedKey(a));
        CHECK(!cc::fault::isQuarantinedKey(b) && !cc::fault::isQuarantinedKey(c));
        CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(0));
        // the neighbours on the other radios kept being fed; every radio is running and none is faulted
        CHECK(r.until([&] { return lineCount(b) > nb + 2 && lineCount(c) > nc + 2; }));
        for (std::uint32_t n = 1; n <= 3; ++n) {
            const FoxReceiverState st = r.radio(n);
            CHECK((st.flags & FOXAPI_RX_RUNNING) != 0u);
            CHECK((st.flags & FOXAPI_RX_FAULTED) == 0u);
        }
        CHECK_EQ(r.instancesOf(b), static_cast<std::size_t>(1));
        CHECK(Access::runnerHas(r.eng, 0, b) && Access::runnerHas(r.eng, 2, c));
        // radio 2's spectrum goes on (its DSP thread survived the plugin)
        FoxSpectrumInfo info{};
        info.structSize = sizeof(info);
        std::vector<float> bins(4096);
        std::uint64_t seq = 0;
        CHECK(r.until([&] {
            const std::int32_t rc = api()->read_radio_spectrum(r.s->raw(), 2, seq, &info, bins.data(), static_cast<std::uint32_t>(bins.size()));
            if (rc == FOXAPI_OK) { seq = info.seq; }
            return rc == FOXAPI_OK && info.seq > 0 && seq > 0;
        }));
        const std::uint64_t first = seq;
        CHECK(r.until([&] {
            const std::int32_t rc = api()->read_radio_spectrum(r.s->raw(), 2, seq, &info, bins.data(), static_cast<std::uint32_t>(bins.size()));
            if (rc == FOXAPI_OK) { seq = info.seq; }
            return seq > first + 3;
        }));
    }
    cc::fault::resetForTest();
    {
        // A HANG: key 3 makes the plugin's next poll_text() run for 1.5 s on radio 2's DSP thread. The budget is shortened so the guard calls it a wedge.
        Rig r({"rp-a", "rp-b"});
        const std::string a = r.files[0], b = r.files[1];
        threeRadios(r);
        CHECK_EQ(r.place(a, 2).status(), FOXAPI_OK);
        CHECK(r.until([&] { return lineCount(a) > 0 && lineCount(b) > 0; }));
        cc::fault::overrideBudgetForTest(cc::fault::Entry::PollText, 150);
        const std::size_t nb = lineCount(b);
        // (the command is read by the plugin's own next poll: the key is pressed through the op)
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_PLUGIN_COMMAND, 0.0, 3, a.c_str())).status(), FOXAPI_OK);
        // while radio 2's decoder is inside the call, radio 1's decoder keeps being fed: wait for lines of b with the pass left alone (a pass touching radio 2's runner waits for
        // the call, as the pass always waits for the first radio's runner; the DSP threads do not wait for each other)
        const auto start = std::chrono::steady_clock::now();
        bool fedDuring = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1300)) {
            if (lineCount(b) > nb + 3) {
                fedDuring = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(fedDuring);
        CHECK(r.until([&] { return cc::fault::isQuarantinedKey(a); }, 15000));
        cc::fault::dropBudgetOverridesForTest();
        r.pump(4);
        CHECK(!cc::fault::isQuarantinedKey(b));
        CHECK_EQ(r.instancesOf(a), static_cast<std::size_t>(0));
        CHECK_EQ(r.instancesOf(b), static_cast<std::size_t>(1));
        CHECK((r.radio(1).flags & FOXAPI_RX_FAULTED) == 0u && (r.radio(2).flags & FOXAPI_RX_FAULTED) == 0u);
    }
    cc::fault::resetForTest();
#endif
}

// ---- H -------------------------------------------------------------------------------------------------------------------------------------------

void testTheScannerAndTheTransmitter() {
    std::printf("  H: the scanner stays on its radio; the transmitter follows radio 1 unless a Pluto is another radio\n");
    {
        Rig r({});
        threeRadios(r);
        CHECK_EQ(r.fact("scanner.radio"), std::string("0"));
        CHECK_EQ(r.fact("transmit.radio"), std::string("1"));
        FoxCommand run = r.aimed(FOXAPI_OP_SCANNER_RUN, 2, 7.0e6, 1);
        run.num[1] = 7.1e6;
        run.num[2] = 5000.0;
        CHECK_EQ(r.send(run).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(r.fact("scanner.radio"), std::string("2"));
        CHECK((r.radio(2).flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
        CHECK((r.radio(1).flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u && (r.radio(3).flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
        // a command to radio 1 does not move it; the scan goes on
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_VOLUME, 1, 0.3)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(r.fact("scanner.radio"), std::string("2"));
        // starting it on radio 3 moves it there
        FoxCommand run3 = r.aimed(FOXAPI_OP_SCANNER_RUN, 3, 28.0e6, 1);
        run3.num[1] = 28.1e6;
        run3.num[2] = 5000.0;
        CHECK_EQ(r.send(run3).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(r.fact("scanner.radio"), std::string("3"));
        CHECK((r.radio(2).flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u && (r.radio(3).flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
        // removing its radio stops it
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 3)).status(), FOXAPI_OK);
        r.pump(3);
        CHECK_EQ(r.fact("scanner.radio"), std::string("0"));
        CHECK((r.radio(1).flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
    }
    {
        // THE RULE, as a pure function: the radio whose device is the transmitter's own (the Pluto), else the first
        using cascade::engine::transmitRadioFor;
        using cascade::engine::TransmitCandidate;
        const std::vector<TransmitCandidate> none = {{"siggen", "", false}, {"siggen", "", false}};
        CHECK_EQ(transmitRadioFor(true, "uri=ip:192.168.2.1", none), static_cast<std::size_t>(0));
        const std::vector<TransmitCandidate> pluto2 = {{"rtlsdr", "serial=1", true}, {"pluto", "uri=ip:192.168.2.1", true}, {"pluto", "uri=ip:10.0.0.9", true}};
        CHECK_EQ(transmitRadioFor(true, "uri=ip:192.168.2.1", pluto2), static_cast<std::size_t>(1));
        CHECK_EQ(transmitRadioFor(true, "uri=ip:10.0.0.9", pluto2), static_cast<std::size_t>(2));
        CHECK_EQ(transmitRadioFor(true, "uri=ip:172.16.0.1", pluto2), static_cast<std::size_t>(0));   // not any radio's device
        CHECK_EQ(transmitRadioFor(false, "uri=ip:192.168.2.1", pluto2), static_cast<std::size_t>(0));  // no transmitter open
        CHECK_EQ(transmitRadioFor(true, "", pluto2), static_cast<std::size_t>(0));
        const std::vector<TransmitCandidate> notOpen = {{"siggen", "", false}, {"pluto", "uri=ip:192.168.2.1", false}};
        CHECK_EQ(transmitRadioFor(true, "uri=ip:192.168.2.1", notOpen), static_cast<std::size_t>(0));   // a Pluto that did not open is not a radio to follow
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_plugin_radio\n");
    if (argc < 2) {
        std::printf("usage: test_engine_api_plugin_radio <radio probe module>\n");
        return 2;
    }
    g_fixture = argv[1];
    engine_api_test::hermetic("plugin_radio");
    engine_api_test::setEnv("FOXSDR_DORMANT_AFTER_MS", "400");
    g_scratch = fs::temp_directory_path() / ("foxsdr_plugin_radio_" + std::to_string(
#if defined(_WIN32)
                                                                                    _getpid()
#else
                                                                                    getpid()
#endif
                                                                                        ));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    Access::installHooks();

    testDefaultIsRadioOne();
    testMoveFeedsTheRadioItIsOn();
    testASetFrequencyMovesItsOwnRadio();
    testMovingInALoop();
    testRemovingTheRadioStopsThePlugin();
    testTheChoiceSurvivesARestart();
    testRestoreByIdAndFallback();
    testAFaultOrAHangOnRadioTwoLeavesTheOthers();
    testTheScannerAndTheTransmitter();

    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_plugin_radio");
}
