// test_source_row_app.cpp - what the REAL AppWindow's Source row says about
// radios (0.99.49 beta feedback): a tester whose patch was running a NooElec
// NESDR SMArt v5 perfectly was told "No radio hardware found", and the Source
// row said only "Signal gen".
//
//   - "No radio hardware found" was gated on the SoapySDR list alone, so a
//     dongle the NATIVE driver found - listed in the combo right above the
//     warning - did not count. It must be shown only when no radio of any kind
//     was found (and the ADALM-Pluto row, which is always there and is an
//     address box rather than a radio found, does not count as one - nor does
//     the rtl_tcp server's row, which is the same kind of row).
//   - While a patch runs it has the radios and the receiver is on the
//     generator; the combo and the chip must say the patch has the radio.
//
// The rules themselves are pinned in tests/test_source_row_status.cpp; this
// proves AppWindow asks them, with no window, no device and no ImGui frame -
// the native list is the test's own (Engine::testHooks_.nativeScan) and the
// patch radio is a generator standing in for the dongle, never started.
//
// CONVERTED FOR THE ENGINE'S TABLE (stub campaign B18; the 0.99.84 house pattern). Master's test set the window's own members (the scan, the
// patch's radios) and read the window's own answers; here the scan and the patch are the ENGINE's (th::engine, written on the pass thread
// through tests/pass_seam.hpp) and the window answers from what the engine published - the DEVICES list, the scan's flags, PATCH_RADIOS and
// the patch graph (a Radio node's device is what says that a radio is hardware and the generator is not). So between a change to the engine
// and the question put to the window there is the engine's publish and the window's look (look()): the same two steps a frame takes. What
// each scenario asserts is unchanged, with ONE addition: the rtl_tcp server's row (0.99.70) is held not to count as a radio found - master's
// fakeScan listed it, and the "warning is shown with no dongle" check only proved it by accident of listing it as well as the Pluto.
//
// Hermetic: the per-user directories point at a scratch folder, no USB enumeration runs and no real driver is ever constructed (the patch
// radios are installed directly, a generator standing in for the dongle, never started; the engine's driver factory is a fake in case a
// pass should look at them). No ImGui frame is rendered.
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
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
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
    // ...and so is the rtl_tcp server row (0.99.70), which follows it: an
    // address to type, never a radio that was found. With no dongle plugged
    // in, the "No radio hardware found" warning must still show beside BOTH.
    cascade::source::NativeDeviceInfo rtlTcp;
    rtlTcp.driver = "rtltcp";
    rtlTcp.label = "rtl_tcp server (network)";
    rtlTcp.args = "rtltcp=127.0.0.1:1234";
    out.push_back(rtlTcp);
    return out;
}

}  // namespace

namespace pc = cascade::core::patch;

namespace cascade::gui {

struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;

    static void installHooks() {
        Engine::testHooks_.nativeScan = &fakeScan;
        // (A pass that looked at the patch's radios would open them through this, never through a real driver.)
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
    }
    // One frame's engine part, in drawUi's order: the session's queue taken, the publish, the status copy, the window's read of the state. The
    // phases of the publish alone (a CUSTOM subset: a whole pass would reconcile the patch the test is about to stand up by hand).
    static void look(AppWindow& a) {
        passseam::custom(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            th::engine(a).pumpInputs();
            th::engine(a).pumpPlugins();
            (void)th::engine(a).pumpPublish();
        });
        a.statusLines_ = th::engine(a).statusText();
        a.refreshUiState();
    }
    // A completed, whole SoapySDR scan that found nothing - the tester's case:
    // no SoapySDR module for the dongle.
    static void soapyScanFoundNothing(AppWindow& a) {
        Engine& e = th::engine(a);
        passseam::on(e, [&] {
            e.scanNative();
            e.soapyDevices_.clear();
            e.soapyScanned_ = true;
            e.soapyScanPartial_ = false;
            e.soapyScanPending_ = false;
            e.deviceOpenPending_ = false;
        });
        look(a);
        look(a);
    }
    static bool warningShown(AppWindow& a) {
        look(a);
        return a.noRadioHardwareShown();
    }
    // A Radio node of the patch's graph, set to `device`, and the radio it runs: what the engine leaves behind once the open has landed. The
    // source is a generator standing in for the dongle and is never started; the node's rate is the one the radio was opened at, so a pass
    // that looked at the patch would find nothing to do.
    static void addRadio(AppWindow& a, pc::NodeId id, const std::string& device, const std::string& label, const std::string& openedAs) {
        Engine& e = th::engine(a);
        passseam::on(e, [&] {
            e.patchRunning_ = true;
            if (e.patchGraph_.addNodeAs(id, pc::NodeKind::Radio, "Radio") != pc::kNoNode) {
                if (pc::Node* n = e.patchGraph_.mutableNode(id)) {
                    n->device = device;
                    n->rateHz = 2.0e6;
                }
            }
            ++e.patchGraphVersion_;
            e.patchRadios_[id] = std::make_unique<pc::PatchRadio>(id, std::make_unique<cascade::source::SigGenSource>(2.0e6), label);
            e.patchRadioOpenedAs_[id] = openedAs;
        });
        look(a);
        look(a);
    }
    // The patch starting with the dongle on a Radio node.
    static void patchTakesDongle(AppWindow& a) { addRadio(a, 7, "rtlsdr|serial=00000001", "NESDR SMArt v5 (serial 00000001)", "rtlsdr|serial=00000001@2000000"); }
    // ...and a second Radio node on the generator, which is no radio at all.
    static void patchAddsGenerator(AppWindow& a) { addRadio(a, 9, "siggen", "Signal generator", "siggen@2000000"); }
    static void patchStops(AppWindow& a) {
        Engine& e = th::engine(a);
        passseam::on(e, [&] {
            e.patchRadios_.clear();
            e.patchRadioOpenedAs_.clear();
            e.patchGraph_ = pc::Graph{};
            ++e.patchGraphVersion_;
            e.patchRunning_ = false;
        });
        look(a);
        look(a);
    }
    static std::string preview(AppWindow& a) {
        look(a);
        return a.sourceComboPreview();
    }
    static std::string chip(AppWindow& a) {
        look(a);
        return a.sourceRowChip();
    }
    static std::vector<std::string> held(AppWindow& a) {
        look(a);
        return a.patchHeldRadioNames();
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_source_row_app\n");
    isolate();
    Access::installHooks();

    th::TestWindow app;

    // --- 1. A NATIVE DONGLE IS HARDWARE FOUND ------------------------------
    // The tester's desk: SoapySDR found nothing, the native RTL-SDR driver
    // found the NESDR. The warning must not be shown.
    g_withDongle = true;
    Access::soapyScanFoundNothing(app);
    CHECK(!Access::warningShown(app));

    // Nothing plugged in at all: the warning is still shown - and the Pluto
    // row and the rtl_tcp server's, which are always in the list, do not hide it.
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
