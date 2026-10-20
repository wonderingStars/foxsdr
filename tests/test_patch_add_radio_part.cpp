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
// WINDOW PORT GROUP 13 made the scan and the read TWO PASSES: the scan goes
// through the window's session, so the press cannot apply it at once. The node
// is created with no device (the press shows), and the device is chosen when
// the scan's answer arrives. The assertions are unchanged; each is made after
// the LOOK of the frames that take (look(): the pass, the publish, the
// window's reads and the session's answers - the same order as drawUi's).
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
#include "pass_seam.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;

namespace {

// The reviewer's exact reproduction device: an RTL-SDR with a serial, so the
// resulting key is checkable against master's own "rtlsdr|serial=00000001".
// How many radios the desk has (1: the reviewer's RTL-SDR; 3: that one, an Airspy and a HackRF).
int g_radios = 1;
std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::vector<cascade::source::NativeDeviceInfo> out{{"rtlsdr", "RTL-SDR", "serial=00000001"}};
    if (g_radios >= 3) {
        out.push_back({"airspy", "Airspy", "serial=A1"});
        out.push_back({"hackrf", "HackRF", "serial=H1"});
    }
    return out;
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
    // One frame's engine part, in drawUi's order (see the header comment), and the page's own steps that follow it.
    static void look(AppWindow& a) {
        passseam::pass(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            a.engine_.drainLocalCommands();
            a.commitPatchDraft();
            a.engine_.pumpInputs();
            a.engine_.pumpPlugins();
            a.engine_.pumpPatch();
            (void)a.engine_.pumpPublish();
        }, [&] { a.commitPatchDraft(); });
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
        a.pumpTransmitKey();
    }
    static void sync(AppWindow& a) { a.syncPatchDraft(); }
    static void finish(AppWindow& a) { a.finishPatchRadioParts(); }
    static void commit(AppWindow& a) { a.commitPatchDraft(); }
    // A frame of the page that has no Radio part pressed in it.
    static void frame(AppWindow& a) {
        look(a);
        sync(a);
        commit(a);
    }
    static const pc::Node* draftNode(AppWindow& a, pc::NodeId id) { return a.patchDraft_.find(id); }
    // The exact call drawPatchView's Radio-part branch makes.
    static pc::NodeId addRadioPart(AppWindow& a, float x, float y) {
        return a.patchAddRadioPart("Radio", pc::PortType::Iq, x, y);
    }
    static std::size_t nativeCount(AppWindow& a) { return a.engine_.nativeDevices_.size(); }
    // Group 13 part b: the table's device list as the window reads it, the recording the engine has listed and the radio the
    // receiver has lent to the patch (put there behind the engine's back).
    static std::vector<std::string> deviceKeys(AppWindow& a) {
        std::vector<std::string> keys;
        for (const auto& d : a.patchLists().devices) { keys.push_back(d.key); }
        return keys;
    }
    static std::string receiverRadio(AppWindow& a) { return a.patchLists().receiverRadio; }
    static void listRecording(AppWindow& a, const std::string& path, double rateHz) {
        a.engine_.patchRecordings_.push_back({path, "rec.wav", pc::makeIqFileKey(path), rateHz});
    }
    static void lend(AppWindow& a, const std::string& kind, const std::string& args) {
        a.engine_.patchMainKeep_.valid = true;
        a.engine_.patchMainKeep_.kind = kind;
        a.engine_.patchMainKeep_.args = args;
        a.engine_.patchMainKeep_.label = "Lent radio";
    }
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

    // --- GREEN: the fixed method, the scan and the read as two passes -----
    {
        cascade::gui::AppWindow a;
        CHECK(Access::nativeCount(a) == 0);
        Access::frame(a);  // the page is up: the first publish is read and the draft starts from it
        const pc::NodeId made = Access::addRadioPart(a, 40.0f, 40.0f);
        // THE PRESS SHOWS AT ONCE, with no device: the scan has not run and the list is not there to read.
        CHECK(Access::draftNode(a, made) != nullptr && Access::draftNode(a, made)->device.empty());
        CHECK(Access::nativeCount(a) == 0);
        // A page step before the scan has been answered (a second press, the draft's follow) must not choose from the list
        // as it is - the scan's answer is what the device waits for.
        Access::finish(a);
        CHECK(Access::draftNode(a, made) != nullptr && Access::draftNode(a, made)->device.empty());
        Access::frame(a);  // the pass runs the scan; its answer reaches the page; the device is chosen from the list
        CHECK(Access::nativeCount(a) == 1);  // the scan ran
        Access::frame(a);  // and the pass takes the device
        const pc::Node* n = Access::find(a, made);
        CHECK(n != nullptr);
        if (n != nullptr) {
            std::printf("  two-pass (fixed) path saved: %s\n", n->device.c_str());
            CHECK(n->device == "rtlsdr|serial=00000001");
            CHECK(n->device != pc::kGeneratorKey);
        }
    }

    // --- a device another Radio already has is never handed to a second
    //     one - patchDefaultDeviceKey's existing rule, exercised through the
    //     same fixed path.
    {
        cascade::gui::AppWindow a;
        Access::frame(a);
        // BOTH presses before the scan is answered: the second must not choose the radio the first is about to take.
        const pc::NodeId first = Access::addRadioPart(a, 40.0f, 40.0f);
        const pc::NodeId second = Access::addRadioPart(a, 80.0f, 80.0f);
        Access::frame(a);
        Access::frame(a);
        Access::frame(a);
        const pc::Node* n1 = Access::find(a, first);
        const pc::Node* n2 = Access::find(a, second);
        CHECK(n1 != nullptr && n2 != nullptr);
        if (n1 != nullptr && n2 != nullptr) {
            CHECK(n1->device == "rtlsdr|serial=00000001");
            CHECK(n2->device == pc::kGeneratorKey);  // the only radio is taken
        }
    }

    // --- THE DEVICE COMES FROM THE TABLE'S LIST, IN ITS ORDER (window port group 13, part b): PATCH_DEVICES is the engine's
    //     composition (generator, recordings, native radios, ...), and a new Radio takes the first RADIO of it that no other
    //     Radio has - never a recording, however many are listed - falling back to the generator when none is free.
    {
        g_radios = 3;
        cascade::gui::AppWindow a;
        Access::frame(a);
        Access::listRecording(a, "C:/rec/sample.wav", 96000.0);
        const pc::NodeId n1 = Access::addRadioPart(a, 40.0f, 40.0f);
        const pc::NodeId n2 = Access::addRadioPart(a, 80.0f, 80.0f);
        const pc::NodeId n3 = Access::addRadioPart(a, 120.0f, 120.0f);
        const pc::NodeId n4 = Access::addRadioPart(a, 160.0f, 160.0f);
        for (int i = 0; i < 5; ++i) { Access::frame(a); }
        const std::vector<std::string> keys = Access::deviceKeys(a);
        std::printf("  the table lists %zu devices:", keys.size());
        for (const std::string& k : keys) { std::printf(" %s", k.c_str()); }
        std::printf("\n");
        CHECK(keys.size() == 5u);   // the generator, the recording, and the three radios
        CHECK(keys.size() >= 5u && keys[0] == pc::kGeneratorKey && pc::isIqFileKey(keys[1]));
        const pc::Node* d1 = Access::find(a, n1);
        const pc::Node* d2 = Access::find(a, n2);
        const pc::Node* d3 = Access::find(a, n3);
        const pc::Node* d4 = Access::find(a, n4);
        CHECK(d1 != nullptr && d2 != nullptr && d3 != nullptr && d4 != nullptr);
        if (d1 != nullptr && d2 != nullptr && d3 != nullptr && d4 != nullptr) {
            CHECK(d1->device == "rtlsdr|serial=00000001");   // the list's order: the press order is the radios' order
            CHECK(d2->device == "airspy|serial=A1");
            CHECK(d3->device == "hackrf|serial=H1");
            CHECK(d4->device == pc::kGeneratorKey);          // all taken; the recording is never chosen for a new Radio
        }
        g_radios = 1;
    }

    // --- THE RECEIVER'S OWN RADIO FIRST: a radio lent to the patch (fact.patch.receiverRadio) is the starter's device, ahead of
    //     the list, even when the list does not hold it (a SoapySDR radio is only listed after a scan).
    {
        g_radios = 1;
        cascade::gui::AppWindow a;
        Access::lend(a, "airspy", "serial=LENT");
        Access::frame(a);
        Access::frame(a);
        CHECK(Access::receiverRadio(a) == pc::makeDeviceKey("airspy", "serial=LENT"));
        const pc::NodeId first = Access::addRadioPart(a, 40.0f, 40.0f);
        const pc::NodeId second = Access::addRadioPart(a, 80.0f, 80.0f);
        for (int i = 0; i < 5; ++i) { Access::frame(a); }
        const pc::Node* d1 = Access::find(a, first);
        const pc::Node* d2 = Access::find(a, second);
        CHECK(d1 != nullptr && d2 != nullptr);
        if (d1 != nullptr && d2 != nullptr) {
            std::printf("  with a radio lent: %s then %s\n", d1->device.c_str(), d2->device.c_str());
            CHECK(d1->device == pc::makeDeviceKey("airspy", "serial=LENT"));   // the receiver's radio, first
            CHECK(d2->device == "rtlsdr|serial=00000001");                     // then the list's first free radio
        }
    }

    const int rc = testSummary("test_patch_add_radio_part");
    if (rc == 0) {
        std::error_code ec;
        std::filesystem::remove_all(g_scratch, ec);
    }
    return rc;
}
