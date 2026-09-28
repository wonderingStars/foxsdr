// test_patch_add_radio_part.cpp - a Radio PART dropped from the patch page's
// bin starts on a free native radio (0.99.40), not the signal generator
// (engine/stage3b-pre B1, 2026-09-28).
//
// THE REGRESSION. drawPatchView's press handler queued the native scan
// (engine_.submitCommand) and read patchDefaultDeviceKey() on the very next
// line - which still saw the PREVIOUS (empty) native list, because a queued
// command has not run yet. Every "add a Radio" click therefore saved the
// generator, however many real radios were on the desk. Reproduced against
// the built application with an RTL-SDR attached: master (f7d1cfc) saved
// "rtlsdr|serial=00000001", this branch saved "siggen".
//
// THIS TEST exercises AppWindow::patchAddRadioPart directly - the extracted
// method drawPatchView's Radio-part branch now calls - with the native scan
// hook standing in for the RTL-SDR (Engine::testHooks_.nativeScan), so no
// real USB enumeration ever runs. Hermetic like test_converter_app_paths:
// scratch per-user directories, telemetry at a black hole, no ImGui frame
// ever drawn (patchAddRadioPart calls no ImGui function).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;

namespace {

// The reviewer's exact reproduction device: an RTL-SDR with a serial, so the
// resulting key is checkable against master's own "rtlsdr|serial=00000001".
std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"rtlsdr", "RTL-SDR", "serial=00000001"}};
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
                ("foxsdr_patch_add_radio_part_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for its tests (see AppWindow::testHooks_ /
// Engine::testHooks_).
struct AppWindowTestAccess {
    static void installHooks() { cascade::engine::Engine::testHooks_.nativeScan = &fakeScan; }
    // The exact call drawPatchView's Radio-part branch makes.
    static pc::NodeId addRadioPart(AppWindow& a, float x, float y) {
        return a.patchAddRadioPart("Radio", pc::PortType::Iq, x, y);
    }
    static std::size_t nativeCount(AppWindow& a) { return a.engine_.nativeDevices_.size(); }
    static const pc::Node* find(AppWindow& a, pc::NodeId id) { return a.engine_.patchGraph_.find(id); }
    // The buggy pre-fix call, kept here ONLY so this test can show the
    // regression it replaces still fails the way the reviewer's
    // addradio.py found it - a queued scan has not run by the time the
    // very next line reads the native list.
    static pc::NodeId addRadioPartQueued(AppWindow& a, float x, float y) {
        a.engine_.submitCommand(cascade::core::cmd::make(FOXAPP_OP_SCAN_NATIVE_ONLY));
        const std::string dev = a.patchDefaultDeviceKey();
        const pc::NodeId made = a.engine_.patchGraph_.addNode(pc::NodeKind::Radio, "Radio",
                                                              pc::PortType::Iq, x, y);
        if (pc::Node* n = a.engine_.patchGraph_.mutableNode(made)) { n->device = dev; }
        return made;
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main() {
    std::printf("test_patch_add_radio_part\n");
    isolate();
    Access::installHooks();

    // --- RED, named here so the mechanism this test pins is explicit: the
    //     QUEUED command (the branch's bug) saves the generator even with a
    //     real radio on the desk, because the scan has not run yet when
    //     patchDefaultDeviceKey() is read on the next line. ---------------
    {
        cascade::gui::AppWindow a;
        CHECK(Access::nativeCount(a) == 0);  // nothing scanned yet
        const pc::NodeId made = Access::addRadioPartQueued(a, 40.0f, 40.0f);
        const pc::Node* n = Access::find(a, made);
        CHECK(n != nullptr);
        if (n != nullptr) {
            std::printf("  queued (buggy) path saved: %s\n", n->device.c_str());
            CHECK(n->device == pc::kGeneratorKey);  // "siggen" - the bug
        }
    }

    // --- GREEN: the fixed method, applied at once ------------------------
    {
        cascade::gui::AppWindow a;
        CHECK(Access::nativeCount(a) == 0);
        const pc::NodeId made = Access::addRadioPart(a, 40.0f, 40.0f);
        CHECK(Access::nativeCount(a) == 1);  // the scan ran, synchronously
        const pc::Node* n = Access::find(a, made);
        CHECK(n != nullptr);
        if (n != nullptr) {
            std::printf("  applied-at-once (fixed) path saved: %s\n", n->device.c_str());
            CHECK(n->device == "rtlsdr|serial=00000001");
            CHECK(n->device != pc::kGeneratorKey);
        }
    }

    // --- a device another Radio already has is never handed to a second
    //     one - patchDefaultDeviceKey's existing rule, exercised through the
    //     same fixed path.
    {
        cascade::gui::AppWindow a;
        const pc::NodeId first = Access::addRadioPart(a, 40.0f, 40.0f);
        const pc::NodeId second = Access::addRadioPart(a, 80.0f, 80.0f);
        const pc::Node* n1 = Access::find(a, first);
        const pc::Node* n2 = Access::find(a, second);
        CHECK(n1 != nullptr && n2 != nullptr);
        if (n1 != nullptr && n2 != nullptr) {
            CHECK(n1->device == "rtlsdr|serial=00000001");
            CHECK(n2->device == pc::kGeneratorKey);  // the only radio is taken
        }
    }

    const int rc = testSummary("test_patch_add_radio_part");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(g_scratch, ec);
    }
    return rc;
}
