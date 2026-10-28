// test_interface_half_app.cpp - THE WINDOW'S HALF OF THE SETTINGS FILE, SENT AND READ THROUGH THE TABLE (shell retirement R5;
// docs/api-0.3-additions.md 9.16, docs/engine-stage4.md 6.25).
//
// The window owns part of the settings file (core/config_halves.hpp). Since R5 it READS the part it starts from from the engine (the LOCAL fact
// fact.config.interface, served at the call) and SENDS the part it is showing whenever that changes (FOXAPP_OP_INTERFACE_CONFIG, long text). The
// window still owns its keeper in R5 (the engine's snapshot duplicates its half harmlessly); what is held here is the traffic:
//
//   A  READS FROM THE TABLE: the window restored from a settings file holds that file's interface half, the engine holds the same, and the merge the
//      window reads with (withInterfaceHalfFromTable) takes the TABLE's half over the file's it is handed - the engine's own fields stay the file's
//   B  NOTHING TO SAY, NOTHING SENT: a hundred looks at a window whose half has not moved send no document
//   C  A CHANGE IS SENT ONCE, and reaches the engine: one document, which the engine's next pass puts into its snapshot
//   D  A BURST IS COALESCED: a field that changes every look (a window drag) sends at most one document per kInterfaceSendGapS, and the last state
//      still gets there after the burst - the session's queue is bounded, so "every look" would be BUSY within 256 frames
//   E  THE SELF-MOVING FIELDS DO NOT MAKE A DOCUMENT: the crash-upload limiter's memory moving by itself sends nothing; it rides along in the document
//      the next real change sends
//   F  THE DIAGNOSTICS SWITCH IS NOT MADE TO WAIT: it is sent inside the gap ("off means off")
//   G  A FULL SESSION IS NOT A LOST HALF: with the session's queue full (BUSY) nothing is marked sent, nothing is said to the log beyond the engine's
//      own count, and the half goes when the queue has room
//
// Break-it (docs/engine-stage4.md 6.25): sendInterfaceHalf without its compare (a document every look) -> B and D go RED; without the gap -> D RED;
// without the urgent rule -> F RED; marking the half sent when the session was BUSY -> G RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/config_halves.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "imgui.h"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace cascade::gui {
struct AppWindowTestAccess {
    static double gap() { return AppWindow::kInterfaceSendGapS; }
    static std::uint64_t sends(AppWindow& a) { return a.interfaceSends_; }
    static bool send(AppWindow& a, double nowS) { return a.sendInterfaceHalf(nowS); }
    static cascade::core::AppConfig withTable(AppWindow& a, const cascade::core::AppConfig& file) { return a.withInterfaceHalfFromTable(file); }
    static int& railBank(AppWindow& a) { return a.railBank_; }
    static float& splitRatio(AppWindow& a) { return a.splitRatio_; }
    static bool& diagnosticsEnabled(AppWindow& a) { return a.diagnosticsEnabled_; }
    static std::string& language(AppWindow& a) { return a.languageSetting_; }
    static std::string& updateChannel(AppWindow& a) { return a.updateChannel_; }
    static cascade::core::UploadPolicyState& crashState(AppWindow& a) { return a.crashUploadState_; }
    static void note(AppWindow& a, const std::string& name) { a.uiSubmit(cascade::core::cmd::makeText(FOXAPP_OP_NOTE_PANEL, name)); }
    static std::uint64_t busyDrops(AppWindow& a) { return a.uiBusyDrops_; }
    // The window's own once-a-frame reading of the answers the session holds (a command's result counts against the session until it is read).
    static void readAnswers(AppWindow& a) { a.pollUiAnswers(); }
    // One pass of the engine, as the engine's own frame runs it (the seam's: inline on this thread, or on the control thread).
    static void pump(AppWindow& a) { passseam::whole(th::engine(a), [&] { th::engine(a).pump(); }); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::core::AppConfig;

namespace {
void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_interface_half_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_interface_half_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    ImGui::CreateContext();

    const std::string path = (scratch / "config.json").string();
    {
        AppConfig seed;
        seed.railBank = 3;                // the window's
        seed.language = "de";
        seed.updateChannel = "beta";
        seed.diagnosticsEnabled = false;
        seed.volume = 0.3f;               // the engine's
        std::string err;
        CHECK(cascade::core::ConfigStore::save(path, seed, err));
    }
    {
        th::TestWindow app(path);
        const double gap = A::gap();

        // A. the window restored what the file said, from the table
        CHECK_EQ(A::railBank(app), 3);
        CHECK_EQ(A::language(app), std::string("de"));
        CHECK_EQ(A::updateChannel(app), std::string("beta"));
        CHECK(!A::diagnosticsEnabled(app));
        {
            AppConfig file;                // a file's config that disagrees with the table about the window's half and has its own engine fields
            file.railBank = 9;
            file.language = "xx";
            file.volume = 0.9f;
            const AppConfig merged = A::withTable(app, file);
            CHECK_EQ(merged.railBank, 3);                          // the table's
            CHECK_EQ(merged.language, std::string("de"));
            CHECK(!merged.diagnosticsEnabled);
            CHECK_EQ(merged.volume, 0.9f);                         // the file's: it is not in the interface's half
            AppConfig held;
            std::string error;
            CHECK(cascade::core::parseInterfaceHalf(th::engine(app).interfaceConfigDocument(), held, error));
            CHECK_EQ(held.railBank, 3);                            // ...and the engine holds what the window restored from
        }

        // B. nothing moved, nothing sent
        for (int i = 0; i < 100; ++i) { CHECK(!A::send(app, 10.0 + 0.01 * i)); }
        CHECK_EQ(A::sends(app), std::uint64_t{0});

        // C. a change is sent once, and the engine's next pass puts it into its snapshot
        A::railBank(app) = 2;
        double t = 20.0;
        CHECK(A::send(app, t));
        CHECK_EQ(A::sends(app), std::uint64_t{1});
        CHECK(!A::send(app, t + 0.01));                           // the same state again: nothing
        CHECK(!A::send(app, t + gap + 1.0));
        CHECK_EQ(A::sends(app), std::uint64_t{1});
        A::pump(app);
        CHECK_EQ(th::engine(app).configSnapshot().railBank, 2);
        AppConfig held;
        std::string error;
        CHECK(cascade::core::parseInterfaceHalf(th::engine(app).interfaceConfigDocument(), held, error));
        CHECK_EQ(held.railBank, 2);

        // D. a burst: a field that changes every look sends at most one document per gap, and the last state still gets there
        const std::uint64_t before = A::sends(app);
        t = 30.0;
        for (int i = 0; i < 100; ++i) {
            A::splitRatio(app) = 0.2f + 0.001f * static_cast<float>(i);
            (void)A::send(app, t + 0.01 * i);                     // a second of looks, 100 Hz
        }
        const std::uint64_t burst = A::sends(app) - before;
        std::printf("D: %llu documents for 100 changes in one second (gap %.2f s)\n", static_cast<unsigned long long>(burst), gap);
        CHECK(burst >= 2u);                                       // not stuck
        CHECK(burst <= static_cast<std::uint64_t>(1.0 / gap) + 2u);   // not one a look
        CHECK(A::send(app, t + 1.0 + gap));                       // the last state, after the burst
        A::pump(app);
        CHECK(std::fabs(th::engine(app).configSnapshot().splitRatio - (0.2f + 0.099f)) < 1e-5);
        CHECK(!A::send(app, t + 3.0));

        // E. the self-moving fields: the crash-upload limiter's memory moving by itself sends nothing ...
        t = 40.0;
        const std::uint64_t quiet = A::sends(app);
        A::crashState(app).windowCount += 5;
        A::crashState(app).windowStart = 1700000000;
        CHECK(!A::send(app, t));
        CHECK(!A::send(app, t + 5.0));
        CHECK_EQ(A::sends(app), quiet);
        // ... and rides along in the document a real change sends
        A::railBank(app) = 4;
        CHECK(A::send(app, t + 10.0));
        A::pump(app);
        CHECK_EQ(th::engine(app).configSnapshot().railBank, 4);
        CHECK_EQ(th::engine(app).configSnapshot().crashUploadWindowCount, std::uint32_t{5});

        // F. the Diagnostics switch is sent inside the gap
        t = 60.0;
        A::railBank(app) = 1;
        CHECK(A::send(app, t));
        A::diagnosticsEnabled(app) = !A::diagnosticsEnabled(app);
        A::railBank(app) = 0;                                      // (a change that would wait)
        CHECK(A::send(app, t + 0.01));                             // the switch does not
        A::pump(app);
        CHECK_EQ(th::engine(app).configSnapshot().diagnosticsEnabled, A::diagnosticsEnabled(app));
        // ... and a change that is not the switch, inside the gap, waits
        A::railBank(app) = 2;
        CHECK(!A::send(app, t + 0.02));
        CHECK(A::send(app, t + 0.01 + gap + 0.01));

        // G. a full session: the half is not lost and not marked sent. (Not in the free-running mode: a pass of the engine's own drains the session
        // while the test fills it, so "full" is not a state the test can hold there.)
        if (passseam::mode() == passseam::Mode::Free) {
            std::printf("G: skipped - free-running pass (the session cannot be held full)\n");
        } else {
        t = 80.0;
        A::pump(app);
        int filled = 0;
        while (A::busyDrops(app) == 0 && filled < 2000) { A::note(app, "panel-" + std::to_string(filled++)); }
        CHECK(A::busyDrops(app) > 0u);                             // the queue is full: the window's own count says so
        A::railBank(app) = 3;
        const std::uint64_t full = A::sends(app);
        CHECK(!A::send(app, t));
        CHECK_EQ(A::sends(app), full);                             // not marked sent
        A::pump(app);                                              // the pass answers them ...
        A::readAnswers(app);                                       // ... and the window reads the answers, as it does once a frame
        CHECK(A::send(app, t + gap + 0.01));                       // the same half, now that there is room
        CHECK_EQ(A::sends(app), full + 1u);
        A::pump(app);
        CHECK_EQ(th::engine(app).configSnapshot().railBank, 3);
        }
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_interface_half_app");
}
