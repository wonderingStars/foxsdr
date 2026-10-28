// test_panel_note_once.cpp - the pages name themselves to the usage report ONCE
// a run, through the window's engine API session (engine stage 4, window port
// group 3, docs/engine-stage4.md 6.5).
//
// WHY THIS TEST. Fourteen pages (and seven more the port moved) call
// AppWindow::uiNotePanel("<name>") EVERY FRAME they are open. The call used
// to be the engine's own telemetryNotePanel, which de-duplicates; it is now a
// submit of FOXAPP_OP_NOTE_PANEL on the window's session, and a submit per
// frame would take a ticket per frame (the session holds 256), build a command
// per frame, and fill the queue within four seconds of a page being open. The
// window's once-per-name set is what prevents it. This drives the real pages
// through ImGui frames and counts what the SESSION is asked to carry.
//
//   A  nothing drawn: nothing noted
//   B  the Radar section drawn for 5 frames: ONE note op reaches the engine,
//      and the engine's list holds {radar}
//   C  the Cat section opened beside it for 5 frames: ONE more (cat), and the
//      list reads back {radar, cat}; 5 further frames of both: none
//   E  end to end: the usage record the engine writes (the same
//      config-snapshot record the next launch sends) lists "radar" and "cat"
//   D  the set's cost: a name already sent allocates NOTHING on the calling
//      thread (a name longer than the small-string size, so a temporary
//      std::string would show), and a first sighting does allocate
//
// Hermetic: every per-user folder is a scratch directory, telemetry points at
// a closed port, and no radio is opened (the window is only constructed).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

// --- allocations on THIS thread (the window and the test share it) ------------
namespace {
thread_local long t_allocs = 0;
}
void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace cascade::gui {
struct AppWindowTestAccess {
    static void drawRadar(AppWindow& a) { a.drawRadarSection(); }
    static void drawCat(AppWindow& a) { a.drawCatSection(); }
    static void notePanel(AppWindow& a, const char* name) { a.uiNotePanel(name); }
    // The engine's pass as far as the session's queue needs it - applied and
    // published (the same two calls the other window tests make).
    static void pass(AppWindow& a) {
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            (void)th::engine(a).pumpPublish();
        });
    }
    // The engine's list, COPIED on the pass's thread with the pass out of the way (passseam::read).
    static std::vector<std::string> panels(AppWindow& a) {
        return passseam::read<std::vector<std::string>>(th::engine(a), [&] { return th::engine(a).telemetryPanels_; });
    }
    // The usage record the engine writes for the next launch to send: reporting
    // switched on the way the checkbox does (the desktop's own op), then the
    // config snapshot the save reads, whose telemetryPending is that record.
    // (The consent and the publish are the pass's, made through passseam::on; configSnapshot() is the published copy, any thread.)
    static std::string usageRecord(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            (void)th::engine(a).applyCommand(cascade::core::cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, 1));
            th::engine(a).publishConfig();
        });
        return th::engine(a).configSnapshot().telemetryPending;
    }
    static std::size_t notedByWindow(AppWindow& a) { return a.uiNotedPanels_.size(); }
    // How many FOXAPP_OP_NOTE_PANEL results the window's session holds (one
    // per ticket the window took for it), and how many of them were refused.
    static int noteResults(AppWindow& a, int* refused) {
        int n = 0;
        *refused = 0;
        if (a.uiSession_ == nullptr) { return -1; }
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].op != FOXAPP_OP_NOTE_PANEL) { continue; }
                ++n;
                if ((r[i].flags & FOXAPI_RESULT_REFUSED) != 0u) { ++*refused; }
            }
        }
        return n;
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

// One frame: the pages that are open, then the engine's pass.
void frame(AppWindow& a, bool radar, bool cat) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(900, 900));
    ImGui::Begin("rail", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    if (radar) { A::drawRadar(a); }
    if (cat) { A::drawCat(a); }
    ImGui::End();
    ImGui::Render();
    A::pass(a);
}

void frames(AppWindow& a, int n, bool radar, bool cat) {
    for (int i = 0; i < n; ++i) { frame(a, radar, cat); }
}

using Names = std::vector<std::string>;

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_panel_note_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // The two sections open the first time they are drawn (verification hook).
    setEnv("FOXSDR_OPEN_SECTIONS", "Radar,CAT");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    {
        th::TestWindow* a = new th::TestWindow();
        int refused = 0;

        // A: nothing drawn, nothing said.
        frames(*a, 3, false, false);
        CHECK_EQ(A::noteResults(*a, &refused), 0);
        CHECK(A::panels(*a).empty());

        // B: the Radar section, 5 frames: one op, one name.
        frames(*a, 5, true, false);
        const int radarOps = A::noteResults(*a, &refused);
        std::printf("B: Radar section drawn for 5 frames: %d note op(s) submitted, %d refused; engine list holds %zu\n",
                    radarOps, refused, A::panels(*a).size());
        CHECK_EQ(radarOps, 1);
        CHECK_EQ(refused, 0);
        CHECK(A::panels(*a) == (Names{"radar"}));
        CHECK_EQ(A::notedByWindow(*a), 1u);

        // C: Cat opens beside it: one more, then nothing however long both stay open.
        frames(*a, 5, true, true);
        const int catOps = A::noteResults(*a, &refused);
        std::printf("C: Cat section opened too, 5 more frames: %d note op(s) submitted; engine list: %s, %s\n",
                    catOps, A::panels(*a).size() > 0 ? A::panels(*a)[0].c_str() : "-",
                    A::panels(*a).size() > 1 ? A::panels(*a)[1].c_str() : "-");
        CHECK_EQ(catOps, 1);
        CHECK_EQ(refused, 0);
        CHECK(A::panels(*a) == (Names{"radar", "cat"}));
        frames(*a, 5, true, true);
        CHECK_EQ(A::noteResults(*a, &refused), 0);
        CHECK(A::panels(*a) == (Names{"radar", "cat"}));
        CHECK_EQ(A::notedByWindow(*a), 2u);

        // E: end to end - the names are in the usage record the engine writes.
        const std::string record = A::usageRecord(*a);
        const std::size_t at = record.find("\"panels\":");
        std::printf("E: usage record %s\n", at == std::string::npos ? "(none)" : record.substr(at, 30).c_str());
        CHECK(at != std::string::npos);
        CHECK(record.find("\"panels\":[\"radar\",\"cat\"]") != std::string::npos);
        delete a;
    }

    // D: what the set costs once a name is in it.
    {
        th::TestWindow* a = new th::TestWindow();
        // 17 characters: longer than a std::string keeps inline, so a
        // temporary built per call would be an allocation we can see.
        const char* name = "diagnostics offer";
        const long before = t_allocs;
        A::notePanel(*a, name);
        const long first = t_allocs - before;
        const long mid = t_allocs;
        for (int i = 0; i < 1000; ++i) { A::notePanel(*a, name); }
        const long repeats = t_allocs - mid;
        std::printf("D: first sighting of \"%s\": %ld allocation(s); 1000 repeats: %ld allocation(s)\n", name, first,
                    repeats);
        CHECK(first > 0);
        CHECK_EQ(repeats, 0L);
        CHECK_EQ(A::notedByWindow(*a), 1u);
        delete a;
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_panel_note_once");
}
