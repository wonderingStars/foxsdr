// test_engine_host_threads.cpp - what the window answers the engine from a
// thread that is not the window's (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 4).
//
// THE FRAME CLOCK. The engine asks its host for the frame time (recordings'
// start times, the bookmark save's debounce, the scanner's dwell, the audio
// watchdog's cadence, the reopen hold-off). The window answered with
// ImGui::GetTime(), which reads ImGui's context - owned by the GUI thread and
// rewritten by it every frame. Once the engine pumps on a control thread that
// is a read of another thread's state. Now the window answers ON its own
// thread from ImGui, and caches the answer; from any other thread it answers
// the cache, and ImGui's context is never touched:
//   A  the GUI thread reads T1; ImGui's clock moves to T2 without being read;
//      a call from another thread gets T1, not T2
//   B  the same for frameClockRunning(): with the context gone, another
//      thread still gets the last answer the GUI thread gave
//
// THE PUBLISH-TIME FACTS. webListening, tunerDisplayStyle and basemapFacts
// were hooks the engine called while publishing; now the window HANDS them
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

#include "gui/app_window.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static cascade::engine::EngineHost& host(AppWindow& a) { return a; }
    static void handFacts(AppWindow& a, const cascade::engine::FrontEndFacts& f) { a.engine_.setFrontEndFacts(f); }
    static cascade::core::PublishedState published(AppWindow& a) {
        (void)a.engine_.publishReceiverState();
        cascade::core::PublishedState s;
        if (!a.engine_.receiverSnapshot_->read(s)) { s = a.engine_.receiverSnapshot_->readFull()->state; }
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
        AppWindow app;
        cascade::engine::EngineHost& host = A::host(app);

        // A: the frame clock.
        frame(1.0);
        const double t1 = host.frameTimeS();   // the GUI thread reads it
        frame(1.0);
        frame(1.0);
        const double t2 = ImGui::GetTime();    // ImGui has moved on, unread
        double fromOther = -1.0;
        std::thread([&] { fromOther = host.frameTimeS(); }).join();
        std::printf("A: GUI thread read %.1f s; ImGui is now at %.1f s; another thread got %.1f s\n", t1, t2,
                    fromOther);
        CHECK(t2 > t1);
        CHECK(fromOther == t1);

        // B: frameClockRunning, with ImGui's context gone.
        CHECK(host.frameClockRunning());   // the GUI thread's answer: a context exists
        ImGui::SetCurrentContext(nullptr);
        bool otherRunning = false;
        std::thread([&] { otherRunning = host.frameClockRunning(); }).join();
        ImGui::SetCurrentContext(ctx);
        std::printf("B: another thread, the context gone: frameClockRunning=%d (the GUI thread's last "
                    "answer: 1)\n",
                    otherRunning ? 1 : 0);
        CHECK(otherRunning);

        // C: the publish-time facts are the ones HANDED OVER, not asked of the
        // window (whose web server is not running and has no basemap here).
        cascade::engine::FrontEndFacts f;
        f.webListening = true;
        f.basemap.active = true;
        f.basemap.minZoom = 3;
        f.basemap.maxZoom = 11;
        f.basemap.tileSize = 256;
        A::handFacts(app, f);
        const cascade::core::PublishedState ps = A::published(app);
        const bool listening = (ps.rx.flags & FOXAPI_RX_WEB_LISTENING) != 0u;
        std::printf("C: handed over listening=1 basemap 3..11: published listening=%d basemap=%d %u..%u\n",
                    listening ? 1 : 0, ps.app.basemapActive ? 1 : 0, ps.app.basemapMinZoom,
                    ps.app.basemapMaxZoom);
        CHECK(listening);
        CHECK(ps.app.basemapActive);
        CHECK(ps.app.basemapMinZoom == 3u && ps.app.basemapMaxZoom == 11u && ps.app.basemapTileSize == 256u);
    }
    ImGui::DestroyContext(ctx);
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_engine_host_threads");
}
