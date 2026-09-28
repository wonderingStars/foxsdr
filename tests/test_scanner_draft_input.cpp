// test_scanner_draft_input.cpp - the Scanner section's form, driven through
// ImGui's own input queue (engine round 3 fix,
// docs/review-harness/engine-round3-review.md findings 1-3 and 5).
//
// WHY THROUGH THE INPUT QUEUE. Every other scanner test applies hand-built
// commands; none of them would notice a panel that sends the wrong command,
// or none, or sends it at the wrong moment. This one feeds real mouse and
// keyboard events into AppWindow::drawScannerSection and reads back ONLY the
// Engine's scan fields (and the running Scanner), so what it checks is what
// the operator's typing actually did to the receiver. It began as the
// round-3 reviewer's throwaway harness (docs/review-harness/
// test_zz_review_scanner_draft.cpp); scenarios A-E are that harness's, F-H
// were added with the fix.
//
//   A  the web remote moves the range, then the desk edits Dwell only: the
//      remote's range survives, and the form showed it before the edit
//   B  no config ever applied: the form holds real defaults, never zeros
//   C  type Start and press Start scan with the field still active
//
// (The harness typed 150; these type 95 so the typed start sits inside the
// 88-108 default range and the running scan's first frequency IS it, rather
// than Scanner::configure's swap of a reversed range.)
//   D  type Start and the section stops being drawn before it deactivates
//   E  ...then comes back, and Start scan uses what was typed
//   F  a RUNNING scan never takes a half-typed value; it takes the finished
//      one on deactivate, reconfigured from it
//   G  a running scan's edit left mid-field by a collapsed section is
//      committed by flushScannerDraft, as drawUi runs it
//   H  a running scan's timing edit reaches the engine (the timing command)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
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
#include "core/config.hpp"
#include "gui/app_window.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static void draw(AppWindow& a) { a.drawScannerSection(); }
    static void flush(AppWindow& a) { a.flushScannerDraft(); }
    static void seed(AppWindow& a) { a.applyConfig(cascade::core::AppConfig{}); }
    static double start(AppWindow& a) { return a.engine_.scanStartMhz_; }
    static double stop(AppWindow& a) { return a.engine_.scanStopMhz_; }
    static double step(AppWindow& a) { return a.engine_.scanStepKhz_; }
    static double dwell(AppWindow& a) { return a.engine_.scanDwellMs_; }
    static double hold(AppWindow& a) { return a.engine_.scanHoldMs_; }
    static double draft(AppWindow& a, int i) { return a.scanDraft_[i]; }
    static bool active(AppWindow& a) { return a.engine_.scanner_.active(); }
    static double scanningHz(AppWindow& a) { return a.engine_.scanner_.currentHz(); }
    static void drain(AppWindow& a) { a.engine_.drainLocalCommands(); }
    static FoxCommandResult apply(AppWindow& a, const FoxCommand& c) { return a.engine_.applyCommand(c); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

bool g_open = true;  // draw the section at all (false = its bank was switched away)

// One frame, in drawUi's order for what this test touches: the pending-edit
// flush near the top, then the section, then the next drain.
void frame(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    A::flush(a);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 900));
    ImGui::Begin("scanner", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (g_open) { A::draw(a); }
    ImGui::End();
    ImGui::Render();
    A::drain(a);
}

void frames(AppWindow& a, int n) {
    for (int i = 0; i < n; ++i) { frame(a); }
}

// The distinct hovered ids down the window's left edge, top to bottom, with
// the y each was first found at: the section header, then the seven
// InputDoubles, then the Start (or Stop) scan key.
std::vector<std::pair<ImGuiID, float>> locate(AppWindow& a) {
    std::vector<std::pair<ImGuiID, float>> out;
    ImGuiIO& io = ImGui::GetIO();
    for (float y = 2.0f; y < 700.0f; y += 3.0f) {
        io.AddMousePosEvent(40.0f, y);
        frame(a);
        frame(a);
        const ImGuiID h = GImGui->HoveredId;
        if (h != 0 && (out.empty() || out.back().first != h)) {
            bool seen = false;
            for (auto& p : out) { seen = seen || p.first == h; }
            if (!seen) { out.push_back({h, y}); }
        }
    }
    io.AddMousePosEvent(850.0f, 880.0f);
    frame(a);
    return out;
}

void click(AppWindow& a, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame(a);
    io.AddMouseButtonEvent(0, true);
    frame(a);
    io.AddMouseButtonEvent(0, false);
    frame(a);
}

void typeOver(AppWindow& a, const char* s) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame(a);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame(a);
    io.AddInputCharactersUTF8(s);
    frame(a);
}

void clickAway(AppWindow& a) { click(a, 850.0f, 880.0f); }  // somewhere empty: deactivates

AppWindow* makeApp(bool seeded) {
    auto* a = new AppWindow();
    if (seeded) { A::seed(*a); }
    g_open = true;
    frames(*a, 3);
    return a;
}

void startScan(AppWindow& a, double startHz, double stopHz, double stepHz) {
    FoxCommand run = cascade::core::cmd::makeInt(FOXAPI_OP_SCANNER_RUN, 1);
    run.num[0] = startHz;
    run.num[1] = stopHz;
    run.num[2] = stepHz;
    (void)A::apply(a, run);
}

// Header, start, stop, step, dwell, hold, resume, listen, then the key.
constexpr std::size_t kStart = 1;
constexpr std::size_t kDwell = 4;
constexpr std::size_t kKey = 8;

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_scanner_draft_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_OPEN_SECTIONS", "Scanner");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    // --- A: the web remote moves the range, then the desk edits Dwell only.
    {
        std::printf("A: web remote sets 144-146 MHz, then the desk edits Dwell only\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            FoxCommand c = cascade::core::cmd::make(FOXAPP_OP_SCANNER_RANGE);
            c.ival[0] = 1 | 2 | 4;  // exactly net::controlRequestToCommands' shape
            c.num[0] = 144.0e6;
            c.num[1] = 146.0e6;
            c.num[2] = 12.5e3;
            (void)A::apply(*a, c);
            frames(*a, 2);
            // The form shows the range Start would scan - before any edit.
            CHECK(A::draft(*a, 0) == 144.0);
            CHECK(A::draft(*a, 1) == 146.0);
            CHECK(A::draft(*a, 2) == 12.5);
            click(*a, 60.0f, ids[kDwell].second);
            typeOver(*a, "75");
            clickAway(*a);
            frames(*a, 3);
            std::printf("   start=%.4f stop=%.4f step=%.2f dwell=%.1f\n", A::start(*a), A::stop(*a),
                        A::step(*a), A::dwell(*a));
            CHECK(A::dwell(*a) == 75.0);  // the desk edit landed (the timing command was sent)
            CHECK(A::start(*a) == 144.0);  // the web remote's range survives it
            CHECK(A::stop(*a) == 146.0);
            CHECK(A::step(*a) == 12.5);
        }
        delete a;
    }

    // --- B: a launch whose config could not be read (applyConfig never
    //     runs), then the desk edits Dwell.
    {
        std::printf("B: no config applied, desk edits Dwell only\n");
        AppWindow* a = makeApp(false);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            CHECK(A::draft(*a, 0) == 88.0);  // shows real values, never 0.0000
            click(*a, 60.0f, ids[kDwell].second);
            typeOver(*a, "75");
            clickAway(*a);
            frames(*a, 3);
            std::printf("   start=%.4f stop=%.4f dwell=%.1f hold=%.1f\n", A::start(*a), A::stop(*a),
                        A::dwell(*a), A::hold(*a));
            CHECK(A::dwell(*a) == 75.0);
            CHECK(A::start(*a) == 88.0);
            CHECK(A::stop(*a) == 108.0);
            CHECK(A::hold(*a) == 2000.0);
        }
        delete a;
    }

    // --- C: type a Start MHz and press Start scan straight away.
    {
        std::printf("C: type Start MHz 95, click Start scan with the field still active\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            click(*a, 60.0f, ids[kStart].second);
            typeOver(*a, "95");
            click(*a, 60.0f, ids[kKey].second);  // Start scan
            frames(*a, 3);
            CHECK(A::active(*a));
            CHECK(A::start(*a) == 95.0);
            CHECK(A::scanningHz(*a) == 95.0e6);
        }
        delete a;
    }

    // --- D/E: type a Start MHz and the section goes away before the field
    //     is deactivated; then it comes back and Start scan is pressed.
    {
        std::printf("D: type Start MHz 95, then the section stops being drawn\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            click(*a, 60.0f, ids[kStart].second);
            typeOver(*a, "95");
            g_open = false;
            frames(*a, 5);
            CHECK(A::start(*a) == 95.0);
            std::printf("E: the section returns and Start scan is pressed\n");
            g_open = true;
            frames(*a, 3);
            CHECK(A::draft(*a, 0) == 95.0);  // the box shows what the engine holds
            click(*a, 60.0f, ids[kKey].second);
            frames(*a, 3);
            CHECK(A::active(*a));
            CHECK(A::start(*a) == 95.0);
            CHECK(A::scanningHz(*a) == 95.0e6);
        }
        delete a;
    }

    // --- F: a RUNNING scan takes a field only once it is finished with.
    {
        std::printf("F: running scan, type Start MHz 95, then deactivate\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            startScan(*a, 88.0e6, 108.0e6, 100.0e3);
            frames(*a, 2);
            CHECK(A::active(*a));
            click(*a, 60.0f, ids[kStart].second);
            typeOver(*a, "95");
            frames(*a, 2);
            // Still being typed: neither the stored range nor the running
            // scan has seen a digit of it.
            CHECK(A::start(*a) == 88.0);
            CHECK(A::scanningHz(*a) == 88.0e6);
            clickAway(*a);
            frames(*a, 2);
            CHECK(A::start(*a) == 95.0);
            CHECK(A::stop(*a) == 108.0);
            CHECK(A::active(*a));
            CHECK(A::scanningHz(*a) == 95.0e6);  // reconfigured from the finished value
        }
        delete a;
    }

    // --- G: a running scan's edit left mid-field when the section goes.
    {
        std::printf("G: running scan, type Start MHz 95, then the section stops being drawn\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            startScan(*a, 88.0e6, 108.0e6, 100.0e3);
            frames(*a, 2);
            click(*a, 60.0f, ids[kStart].second);
            typeOver(*a, "95");
            CHECK(A::start(*a) == 88.0);
            g_open = false;
            frames(*a, 3);
            CHECK(A::start(*a) == 95.0);
            CHECK(A::scanningHz(*a) == 95.0e6);
            g_open = true;
            frames(*a, 2);
            CHECK(A::draft(*a, 0) == 95.0);
        }
        delete a;
    }

    // --- H: a running scan's timing edit reaches the engine.
    {
        std::printf("H: running scan, edit Dwell\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            startScan(*a, 144.0e6, 146.0e6, 12.5e3);
            frames(*a, 2);
            click(*a, 60.0f, ids[kDwell].second);
            typeOver(*a, "250");
            CHECK(A::dwell(*a) == 50.0);  // not yet (Scanner::Params{} dwell)
            clickAway(*a);
            frames(*a, 2);
            CHECK(A::dwell(*a) == 250.0);
            CHECK(A::start(*a) == 144.0);  // the range the scan was started with, untouched
            CHECK(A::stop(*a) == 146.0);
            CHECK(A::active(*a));
        }
        delete a;
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_scanner_draft_input");
}
