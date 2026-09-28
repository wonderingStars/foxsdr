// REVIEW SCRATCH (engine3bpre2 review) - NOT FOR COMMIT.
// Drives the real AppWindow::drawScannerSection through ImGui's input queue
// (mouse + keyboard) and reads back ONLY the Engine's scan fields, so the
// same file builds against 2e3bbe0 (fields bound directly) and 41e3853
// (window-side draft committed on deactivate-after-edit).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
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
    static void seed(AppWindow& a) { a.applyConfig(cascade::core::AppConfig{}); }
    static double start(AppWindow& a) { return a.engine_.scanStartMhz_; }
    static double stop(AppWindow& a) { return a.engine_.scanStopMhz_; }
    static double step(AppWindow& a) { return a.engine_.scanStepKhz_; }
    static double dwell(AppWindow& a) { return a.engine_.scanDwellMs_; }
    static double hold(AppWindow& a) { return a.engine_.scanHoldMs_; }
    static bool active(AppWindow& a) { return a.engine_.scanner_.active(); }
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

bool g_open = true;  // draw the section at all (false = the page was switched away)

void frame(AppWindow& a) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 900));
    ImGui::Begin("rev", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
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
// InputDoubles, then the Start scan key.
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

AppWindow* makeApp(bool seeded) {
    auto* a = new AppWindow();
    if (seeded) { A::seed(*a); }
    g_open = true;
    frames(*a, 3);
    return a;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_rev_scan_" + std::to_string(pid));
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

    // --- SCENARIO A: the web remote moves the range, then the desk edits Dwell.
    {
        std::printf("A: web remote sets 144-146 MHz, then the desk edits Dwell only\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        std::printf("   located %zu items\n", ids.size());
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            FoxCommand c = cascade::core::cmd::make(FOXAPP_OP_SCANNER_RANGE);
            c.ival[0] = 1 | 2 | 4;  // exactly net::controlRequestToCommands' shape
            c.num[0] = 144.0e6;
            c.num[1] = 146.0e6;
            c.num[2] = 12.5e3;
            (void)A::apply(*a, c);
            frames(*a, 2);
            std::printf("   after web: start=%.4f stop=%.4f step=%.2f\n", A::start(*a), A::stop(*a), A::step(*a));
            const float dwellY = ids[4].second;  // header, start, stop, step, DWELL
            click(*a, 60.0f, dwellY);
            typeOver(*a, "75");
            click(*a, 850.0f, 880.0f);  // somewhere empty: deactivates
            frames(*a, 3);
            std::printf("   after desk dwell edit: start=%.4f stop=%.4f step=%.2f dwell=%.1f\n", A::start(*a),
                        A::stop(*a), A::step(*a), A::dwell(*a));
            CHECK(A::dwell(*a) == 75.0);           // the desk edit landed
            CHECK(A::start(*a) == 144.0);          // the web remote's range survives it
            CHECK(A::stop(*a) == 146.0);
        }
        delete a;
    }

    // --- SCENARIO B: a launch whose config could not be read (applyConfig
    //     never runs), then the desk edits Dwell.
    {
        std::printf("B: no config applied (corrupt config.json path), desk edits Dwell only\n");
        AppWindow* a = makeApp(false);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            std::printf("   before: start=%.4f stop=%.4f hold=%.1f\n", A::start(*a), A::stop(*a), A::hold(*a));
            click(*a, 60.0f, ids[4].second);
            typeOver(*a, "75");
            click(*a, 850.0f, 880.0f);
            frames(*a, 3);
            std::printf("   after: start=%.4f stop=%.4f step=%.2f dwell=%.1f hold=%.1f\n", A::start(*a),
                        A::stop(*a), A::step(*a), A::dwell(*a), A::hold(*a));
            CHECK(A::dwell(*a) == 75.0);
            CHECK(A::start(*a) == 88.0);
            CHECK(A::stop(*a) == 108.0);
            CHECK(A::hold(*a) == 2000.0);
        }
        delete a;
    }

    // --- SCENARIO C: type a Start MHz and press Start scan straight away.
    {
        std::printf("C: type Start MHz 150, click Start scan with the field still active\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            click(*a, 60.0f, ids[1].second);
            typeOver(*a, "150");
            click(*a, 60.0f, ids[8].second);  // Start scan
            frames(*a, 3);
            std::printf("   active=%d start=%.4f\n", A::active(*a) ? 1 : 0, A::start(*a));
            CHECK(A::active(*a));
            CHECK(A::start(*a) == 150.0);
        }
        delete a;
    }

    // --- SCENARIO D: type a Start MHz and the page goes away before the
    //     field is deactivated (the section is not drawn again).
    {
        std::printf("D: type Start MHz 150, then the section stops being drawn\n");
        AppWindow* a = makeApp(true);
        const auto ids = locate(*a);
        CHECK(ids.size() >= 9);
        if (ids.size() >= 9) {
            click(*a, 60.0f, ids[1].second);
            typeOver(*a, "150");
            g_open = false;
            frames(*a, 5);
            std::printf("   start=%.4f\n", A::start(*a));
            CHECK(A::start(*a) == 150.0);
            // E: the section comes back (box still reads what was typed) and
            // Start scan is pressed: which range does the scan use?
            g_open = true;
            frames(*a, 3);
            click(*a, 60.0f, ids[8].second);
            frames(*a, 3);
            std::printf("E: after return + Start: active=%d engine start=%.4f\n", A::active(*a) ? 1 : 0,
                        A::start(*a));
            CHECK(A::start(*a) == 150.0);
        }
        delete a;
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_zz_review_scanner_draft");
}
