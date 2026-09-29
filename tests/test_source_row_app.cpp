// test_source_row_app.cpp - what the REAL AppWindow's Source row says about
// radios (0.99.49 beta feedback): a tester whose patch was running a NooElec
// NESDR SMArt v5 perfectly was told "No radio hardware found", and the Source
// row said only "Signal gen".
//
//   - "No radio hardware found" was gated on the SoapySDR list alone, so a
//     dongle the NATIVE driver found - listed in the combo right above the
//     warning - did not count. It must be shown only when no radio of any kind
//     was found (and the ADALM-Pluto row, which is always there and is an
//     address box rather than a radio found, does not count as one).
//   - While a patch runs it has the radios and the receiver is on the
//     generator; the combo and the chip must say the patch has the radio.
//
// The rules themselves are pinned in tests/test_source_row_status.cpp; this
// proves AppWindow asks them, with no window, no device and no ImGui frame -
// the native list is the test's own (TestHooks::nativeScan) and the patch
// radio is a generator standing in for the dongle, never started.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/patch_radio.hpp"
#include "gui/app_window.hpp"
#include "source/siggen_source.hpp"
#include "test_check.hpp"

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// ISOLATED FROM THE REAL CONFIG, the rule every *_app test follows.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_source_row_app_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    const std::string s = scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

// The desk this test pretends to have. Switched by the test between scans.
bool g_withDongle = true;

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::vector<cascade::source::NativeDeviceInfo> out;
    if (g_withDongle) {
        cascade::source::NativeDeviceInfo d;
        d.driver = "rtlsdr";
        d.label = "NESDR SMArt v5 (serial 00000001)";
        d.args = "serial=00000001";
        out.push_back(d);
    }
    // The Pluto row scanNative always adds is an address box, not a radio.
    cascade::source::NativeDeviceInfo pluto;
    pluto.driver = "pluto";
    pluto.label = "ADALM-Pluto (network)";
    pluto.args = "uri=ip:192.168.2.1";
    out.push_back(pluto);
    return out;
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.nativeScan = &fakeScan; }
    // A completed, whole SoapySDR scan that found nothing - the tester's case:
    // no SoapySDR module for the dongle.
    static void soapyScanFoundNothing(AppWindow& a) {
        a.scanNative();
        a.soapyDevices_.clear();
        a.soapyScanned_ = true;
        a.soapyScanPartial_ = false;
        a.soapyScanPending_ = false;
        a.deviceOpenPending_ = false;
    }
    static bool warningShown(const AppWindow& a) { return a.noRadioHardwareShown(); }
    // The patch starting with the dongle on a Radio node: what patchReconcile
    // leaves behind once the open has landed. The source is a generator
    // standing in for the dongle and is never started.
    static void patchTakesDongle(AppWindow& a) {
        a.patchRunning_ = true;
        const cascade::core::patch::NodeId id = 7;
        a.patchRadios_[id] = std::make_unique<cascade::core::patch::PatchRadio>(
            id, std::make_unique<cascade::source::SigGenSource>(2.0e6),
            "NESDR SMArt v5 (serial 00000001)");
        a.patchRadioOpenedAs_[id] = "rtlsdr|serial=00000001@2000000";
    }
    // ...and a second Radio node on the generator, which is no radio at all.
    static void patchAddsGenerator(AppWindow& a) {
        const cascade::core::patch::NodeId id = 9;
        a.patchRadios_[id] = std::make_unique<cascade::core::patch::PatchRadio>(
            id, std::make_unique<cascade::source::SigGenSource>(2.0e6), "Signal generator");
        a.patchRadioOpenedAs_[id] = "siggen@2000000";
    }
    static void patchStops(AppWindow& a) {
        a.patchRadios_.clear();
        a.patchRadioOpenedAs_.clear();
        a.patchRunning_ = false;
    }
    static std::string preview(AppWindow& a) { return a.sourceComboPreview(); }
    static std::string chip(AppWindow& a) { return a.sourceRowChip(); }
    static std::vector<std::string> held(const AppWindow& a) { return a.patchHeldRadioNames(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_source_row_app\n");
    isolate();
    Access::installHooks();

    cascade::gui::AppWindow app;

    // --- 1. A NATIVE DONGLE IS HARDWARE FOUND ------------------------------
    // The tester's desk: SoapySDR found nothing, the native RTL-SDR driver
    // found the NESDR. The warning must not be shown.
    g_withDongle = true;
    Access::soapyScanFoundNothing(app);
    CHECK(!Access::warningShown(app));

    // Nothing plugged in at all: the warning is still shown - and the Pluto
    // row, which is always in the list, does not hide it.
    g_withDongle = false;
    Access::soapyScanFoundNothing(app);
    CHECK(Access::warningShown(app));
    g_withDongle = true;
    Access::soapyScanFoundNothing(app);

    // --- 2. THE SOURCE ROW SAYS THE PATCH HAS THE RADIO --------------------
    // Before the patch: the receiver on the generator says just that.
    CHECK(Access::preview(app) == "Signal generator");
    CHECK(Access::chip(app) == "Signal gen");
    CHECK(Access::held(app).empty());

    Access::patchTakesDongle(app);
    Access::patchAddsGenerator(app);
    const std::vector<std::string> held = Access::held(app);
    CHECK(held.size() == 1u);  // the generator node is no radio
    CHECK(!held.empty() && held[0] == "NESDR SMArt v5");  // serial left off
    CHECK(Access::preview(app) == "Signal generator - the patch has NESDR SMArt v5");
    CHECK(Access::chip(app) == "ON PATCH");

    // STOP gives the radio back, and the row says only what it is on again.
    Access::patchStops(app);
    CHECK(Access::preview(app) == "Signal generator");
    CHECK(Access::chip(app) == "Signal gen");

    return testSummary("test_source_row_app");
}
