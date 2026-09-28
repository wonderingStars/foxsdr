// test_patch_scan_wanted.cpp - the patch page's DEFERRED SoapySDR scan wish
// (engine/stage3b-pre B2, 2026-09-28).
//
// THE REGRESSION. Master's drawPatchRadioInspector, before the engine
// extraction, set patchScanWanted_ the FIRST time a Radio's device list
// opened (never immediately) when the scan was unscanned or partial, and
// patchReconcile ran the scan once the scan PLAN stopped deferring (a radio
// still opening defers it - see engine/device_scan_plan.hpp). This branch's
// merge reused FOXAPP_OP_SCAN_DEVICES_ON_OPEN for the same call site, which
// scans IMMEDIATELY and never retries - so patchScanWanted_ was set nowhere
// at all (dead code, read and reset at engine_patch_radios.cpp:580-582 but
// never written).
//
// THIS TEST drives Engine::patchReconcile directly (headless, no window, no
// ImGui): a radio "opening" forces the scan plan to Defer
// (device_scan_plan.hpp: opening implies Defer regardless of what else is
// open); once it clears, the wish must fire exactly once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

namespace {

namespace cmd = cascade::core::cmd;
using cascade::engine::Engine;

std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> oneSoapyDevice() {
    return {{"SoapySDR B200", "driver=uhd,serial=123"}};
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() /
                ("foxsdr_patch_scan_wanted_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

bool ok(const FoxCommandResult& r) {
    return r.status == FOXAPI_OK && (r.flags & FOXAPI_RESULT_REFUSED) == 0u;
}

bool waitFor(const std::function<bool()>& done, int ms = 5000) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (done()) { return true; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

namespace cascade::gui {

// The friend Engine names for its tests (Engine::testHooks_, and direct
// access to the private fields this test drives and reads).
struct AppWindowTestAccess {
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &oneSoapyDevice;
    }
    static void setDeviceOpenPending(Engine& e, bool v) { e.deviceOpenPending_ = v; }
    static void reconcile(Engine& e) { e.patchReconcile(); }
    static bool scanWanted(const Engine& e) { return e.patchScanWanted_; }
    static bool listsWanted(const Engine& e) { return e.patchListsWanted_; }
    static bool soapyPending(const Engine& e) { return e.soapyScanPending_; }
    static std::size_t soapyCount(const Engine& e) { return e.soapyDevices_.size(); }
    static bool waitSoapy(Engine& e) {
        return waitFor([&e] {
            e.pollSourceAsync();
            return !e.soapyScanPending_;
        });
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_patch_scan_wanted\n");
    isolate();
    Access::installHooks();

    Engine e;
    e.initialise();

    // --- a Radio's device list opens while a radio is still opening --------
    // (soapyScanPlan's "opening" bit - see device_scan_plan.hpp - defers the
    // whole scan regardless of what family is open).
    Access::setDeviceOpenPending(e, true);
    CHECK(ok(e.applyCommand(cmd::make(FOXAPP_OP_PATCH_RADIO_LIST_OPENED))));
    CHECK(Access::listsWanted(e));
    CHECK(Access::scanWanted(e));   // the wish: unscanned, so it is owed
    CHECK(!Access::soapyPending(e));  // NOT fired here - only a wish so far

    // --- patchReconcile, radio STILL opening: the wish must not fire -------
    Access::reconcile(e);
    CHECK(Access::scanWanted(e));      // still owed
    CHECK(!Access::soapyPending(e));   // deferred, exactly as master's plan
    CHECK(Access::soapyCount(e) == 0u);

    // A second reconcile pass while still deferring changes nothing either -
    // this is the property engine_patch_radios.cpp:580 names "run once the
    // scan plan stops deferring", not "run on the first pass regardless".
    Access::reconcile(e);
    CHECK(Access::scanWanted(e));
    CHECK(Access::soapyCount(e) == 0u);

    // --- the radio finishes opening: the plan clears, the scan runs ONCE ---
    Access::setDeviceOpenPending(e, false);
    Access::reconcile(e);
    CHECK(!Access::scanWanted(e));          // consumed
    CHECK(Access::waitSoapy(e));
    CHECK(Access::soapyCount(e) == 1u);

    // --- one more reconcile: the wish is spent, so no second scan ---------
    Access::reconcile(e);
    CHECK(!Access::soapyPending(e));
    CHECK(Access::soapyCount(e) == 1u);   // unchanged - not scanned twice

    const int rc = testSummary("test_patch_scan_wanted");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(g_scratch, ec);
    }
    return rc;
}
