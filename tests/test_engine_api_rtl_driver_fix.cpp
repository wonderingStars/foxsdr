// test_engine_api_rtl_driver_fix.cpp - THE ENGINE SAYS "AN RTL-SDR IS ON THE WRONG WINDOWS DRIVER" AS DATA, for a saved radio at start-up and for the person's own pick (docs 0.99.114).
//
// The new window offers INSTALL DRIVER beside a failed-open sentence only while the engine's `source.fix` fact says "rtl-driver" (core/rtl_open_case.hpp). Held against the
// real engine through the C table, with the fake radio failing its open with each of the three "no RTL-SDR" sentences, so no hardware is needed:
//   F1  THE START-UP RESTORE of a saved RTL-SDR that fails with the wrong-driver sentence: fact.source.fix == "rtl-driver", and the error line is the stand-in sentence that
//       quotes it;
//   F2  THE PERSON'S PICK of a radio that fails the same way: the same fact;
//   F3  THE OTHER TWO SENTENCES (not plugged in; cannot tell) and every other failure ("no such device"): the fact is empty - no offer;
//   F4  A SUCCESSFUL OPEN afterwards clears it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
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

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/rtl_open_case.hpp"
#include "core/source_fallback.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace rtlopen = cascade::core::rtlopen;

namespace {
std::vector<cascade::source::NativeDeviceInfo> errTextScan() {
    return {{engine_api_test::kFakeRadioDriver, "Mock RTL-SDR (serial 00000001)", engine_api_test::kFakeRadioArgs},
            {engine_api_test::kFakeRadioDriver, "RTL-SDR (serial ERRTEXT)", "serial=ERRTEXT"}};
}
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &errTextScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static void remove() {
        Engine::testHooks_.makeDevice = nullptr;
        Engine::testHooks_.nativeScan = nullptr;
        Engine::testHooks_.soapyScan = nullptr;
    }
    static void restore(Engine& e, const cascade::core::AppConfig& c) { e.applyConfig(c); }
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_rtl_fix_fact_" + std::to_string(pid));
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

void passes(Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
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

AppConfig savedRtl(const std::string& args) {
    AppConfig out;
    std::string error;
    CHECK(ConfigStore::parse(R"({"schemaVersion": 1, "centerHz": 100000000.0, "mode": "WFM", "sourceKind": "rtlsdr", "nativeArgs": ")" + args + R"("})", "test", out, error));
    return out;
}

struct Restored {
    Engine eng;
    explicit Restored(const AppConfig* cfg) {
        eng.configureApi(kHooks);
        eng.initialise();
        if (cfg != nullptr) { Access::restore(eng, cascade::core::startupState(*cfg)); }
        eng.controlPass();
    }
};

// ---- F1, F3: the start-up restore ---------------------------------------------------------------------------------------------------------------

void testTheRestore() {
    struct Row { const char* what; std::string said; const char* wantFix; };
    const std::vector<Row> rows = {
        {"wrong driver", rtlopen::sentence(rtlopen::Case::WrongDriver), "rtl-driver"},
        {"not plugged in", rtlopen::sentence(rtlopen::Case::NotPlugged), ""},
        {"cannot tell", rtlopen::sentence(rtlopen::Case::CannotTell), ""},
        {"a different failure", "no such device", ""},
        {"the busy sentence", "the radio is already in use by another program", ""},
    };
    for (const Row& r : rows) {
        engine_api_test::fakeRadioOpenError() = r.said;
        Access::install();
        const AppConfig file = savedRtl("serial=ERRTEXT");
        Restored b(&file);
        testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        passes(b.eng, 6);
        const std::string line = fact(s, "status.sourceError");
        const std::string fix = fact(s, "source.fix");
        std::printf("  restore, %s: fix=\"%s\" (want \"%s\"), standin=%s\n", r.what, fix.c_str(), r.wantFix, fact(s, "source.standin").c_str());
        CHECK_EQ(fix, std::string(r.wantFix).empty() ? std::string() : std::string(r.wantFix));
        CHECK(line.find("did not open") != std::string::npos);   // the stand-in sentence quotes the open's own words
        CHECK(line.find(r.said) != std::string::npos);
        CHECK_EQ(fact(s, "source.standin"), std::string("1"));
        Access::remove();
    }
}

// ---- F2, F4: the person's pick ---------------------------------------------------------------------------------------------------------------

void testThePickAndAGoodOpenAfterIt() {
    engine_api_test::fakeRadioOpenError() = rtlopen::sentence(rtlopen::Case::WrongDriver);
    Access::install();
    Restored b(nullptr);
    testing::Session s(api(), b.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(b.eng, 3);
    CHECK_EQ(fact(s, "source.fix"), std::string());   // nothing failed yet: no offer
    CHECK_EQ(pumped(b.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);   // the row exists only after a scan (the hooked scan lists "serial=ERRTEXT")
    const testing::Outcome picked = pumped(b.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "rtlsdr:serial=ERRTEXT"));
    std::printf("  pick: submit status %d, landed %d\n", picked.status(), picked.landed ? 1 : 0);
    CHECK_EQ(picked.status(), FOXAPI_OK);
    CHECK(passUntil(b.eng, [&] { return fact(s, "source.fix") == "rtl-driver"; }));
    std::printf("  pick, wrong driver: fix=\"%s\"\n", fact(s, "source.fix").c_str());
    CHECK(fact(s, "status.sourceError").find(engine_api_test::fakeRadioOpenError()) != std::string::npos);
    // a pick of a radio that opens (the scan lists the fake RTL-SDR too) clears the line, and so the offer (F4)
    CHECK_EQ(pumped(b.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
    CHECK(passUntil(b.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
    std::printf("  a good open afterwards: fix=\"%s\", error line \"%s\"\n", fact(s, "source.fix").c_str(), fact(s, "status.sourceError").c_str());
    CHECK_EQ(fact(s, "source.fix"), std::string());
    // a pick that fails for another reason raises no offer
    engine_api_test::fakeRadioOpenError() = rtlopen::sentence(rtlopen::Case::NotPlugged);
    CHECK_EQ(pumped(b.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "rtlsdr:serial=ERRTEXT")).status(), FOXAPI_OK);
    CHECK(passUntil(b.eng, [&] { return fact(s, "status.sourceError").find(rtlopen::sentence(rtlopen::Case::NotPlugged)) != std::string::npos; }));
    CHECK_EQ(fact(s, "source.fix"), std::string());
    Access::remove();
}

}  // namespace

int main() {
    std::printf("test_engine_api_rtl_driver_fix\n");
    isolate();
    testTheRestore();
    testThePickAndAGoodOpenAfterIt();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_rtl_driver_fix");
}
