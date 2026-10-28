// test_engine_host_threads.cpp - what the window answers the engine from a
// thread that is not the window's (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 4).
//
// THE CLOCK (changed by the pump move's step P4, adopted decision 5). The engine used to ask its host for the frame time (recordings'
// start times, the bookmark save's debounce, the scanner's dwell, the audio watchdog's cadence, the reopen hold-off), which the
// window answered from ImGui::GetTime() - a clock owned by the GUI thread that stands still when the window does (a drag of the
// title bar, a modal loop) and that a pass on a thread of its own could only read from a cache refreshed once a frame. The engine
// has its OWN clock now, Engine::clockS(): seconds on a steady clock, readable from any thread, always current:
//   A  it runs with no frame at all: ImGui's clock has not moved (no frame was drawn) and the engine's has
//   B  from another thread it answers the same clock - monotone, between the reader's own bounds - and ImGui's context is never
//      touched (the context is gone for the read)
// and the host class no longer has frameTimeS or frameClockRunning (tests/test_engine_shell_guard.cpp holds the hook list).
//
// THE PUBLISH-TIME FACTS. tunerDisplayStyle and basemapFacts (and webListening, until stage 5b made that the engine's own: the web server
// is its host's) were hooks the engine called while publishing; now the window HANDS them
// over (Engine::setFrontEndFacts) and the publish reads what was handed:
//   C  facts handed over reach the published web block, and a later change
//      of the window's own state does not until it is handed over again
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static cascade::engine::ShellHooks& host(AppWindow& a) { return a; }
    static double clockS(AppWindow& a) { return th::engine(a).clockS(); }
    static void handFacts(AppWindow& a, const cascade::engine::FrontEndFacts& f) { th::engine(a).setFrontEndFacts(f); }
    static cascade::core::PublishedState published(AppWindow& a) {
        (void)th::engine(a).publishReceiverState();
        cascade::core::PublishedState s;
        if (!th::engine(a).receiverSnapshot_->read(s)) { s = th::engine(a).receiverSnapshot_->readFull()->state; }
        return s;
    }
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

void frame(double dt) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = static_cast<float>(dt);
    ImGui::NewFrame();
    ImGui::Render();
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_host_threads\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_host_threads_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    {
        th::TestWindow app;
        cascade::engine::ShellHooks& host = A::host(app);

        // A: the engine's clock moves with no frame; ImGui's does not.
        frame(1.0);
        const double imgui1 = ImGui::GetTime();
        const double e1 = A::clockS(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const double e2 = A::clockS(app);
        const double imgui2 = ImGui::GetTime();   // no frame was drawn in between
        std::printf("A: no frame for 120 ms: the engine's clock moved %.3f s, ImGui's %.3f s\n", e2 - e1, imgui2 - imgui1);
        CHECK(imgui2 == imgui1);
        CHECK(e2 - e1 >= 0.100 && e2 - e1 < 5.0);

        // B: from another thread, with ImGui's context gone: the same clock, between the caller's own bounds.
        ImGui::SetCurrentContext(nullptr);
        double lo = 0.0, mid = 0.0, hi = 0.0;
        std::thread([&] {
            lo = A::clockS(app);
            mid = A::clockS(app);
            hi = A::clockS(app);
        }).join();
        ImGui::SetCurrentContext(ctx);
        std::printf("B: another thread, no ImGui context: %.6f <= %.6f <= %.6f (monotone), %.3f s after the GUI thread's read\n",
                    lo, mid, hi, lo - e2);
        CHECK(lo >= e2);
        CHECK(mid >= lo && hi >= mid);

        // C: the publish-time facts are the ones HANDED OVER, not asked of the
        // window (which has no basemap here). Whether the web server listens is not one of them since stage 5b: the server is the engine's,
        // and the bit is its host's answer - NOT listening here, whatever the window hands over.
        cascade::engine::FrontEndFacts f;
        f.basemap.active = true;
        f.basemap.minZoom = 3;
        f.basemap.maxZoom = 11;
        f.basemap.tileSize = 256;
        A::handFacts(app, f);
        const cascade::core::PublishedState ps = A::published(app);
        const bool listening = (ps.rx.flags & FOXAPI_RX_WEB_LISTENING) != 0u;
        std::printf("C: handed over basemap 3..11: published listening=%d (the engine's own server: none) basemap=%d %u..%u\n",
                    listening ? 1 : 0, ps.app.basemapActive ? 1 : 0, ps.app.basemapMinZoom,
                    ps.app.basemapMaxZoom);
        CHECK(!listening);
        CHECK(ps.app.basemapActive);
        CHECK(ps.app.basemapMinZoom == 3u && ps.app.basemapMaxZoom == 11u && ps.app.basemapTileSize == 256u);
    }
    ImGui::DestroyContext(ctx);
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_engine_host_threads");
}
