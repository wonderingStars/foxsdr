/*
 * THE PER-FRAME WIN32 CALL THAT HAD NOTHING TO DO.
 *
 * THE REPORT (0.99.26, Windows 10.0.26200, nine reports from one session,
 * foxsdr.com crash store): the GUI thread was captured with its top frame
 * inside win32u.dll, called from USER32.dll, called from
 * _glfwSetWindowMousePassthroughWin32 <- ImGui_ImplGlfw_NewFrame <-
 * AppWindow::run - past the watchdog's 5 s threshold. Several OTHER threads in
 * the same capture were sitting inside dwmapi.dll and the Intel display
 * driver (ig9icd64.dll): the desktop compositor and GPU driver were
 * themselves congested, and this per-frame call is what the GUI thread
 * happened to be inside when a routine win32u syscall got slow.
 *
 * Dear ImGui's GLFW backend calls glfwSetWindowAttrib(GLFW_MOUSE_PASSTHROUGH)
 * unconditionally, for every platform viewport, on every single frame -
 * ImGui_ImplGlfw_UpdateMouseData, called from ImGui_ImplGlfw_NewFrame. On
 * Windows that is GetWindowLongW plus SetWindowLongW(GWL_EXSTYLE), and when
 * enabling, SetLayeredWindowAttributes too - real syscalls through win32u.dll
 * - regardless of whether the wanted value has changed since the frame
 * before. It practically never has: window_no_input is only ever true for a
 * viewport mid-drag under multi-viewport docking, so on the overwhelming
 * majority of frames this sets the window's existing style bits back to
 * themselves. A call with nothing to justify making it that frame is a call
 * that should not be able to stall anything, however busy the compositor is.
 *
 * THE FIX (mouse-passthrough-cache, third_party/imgui/FOXSDR-PATCHES.md):
 * skip the call once the wanted value for a window matches what was last set
 * for it. ImGui_ImplGlfw_MousePassthroughSyscallCountForTest() counts the
 * calls that actually happened; ImGui_ImplGlfw_ResetMousePassthroughForTest()
 * clears both that counter and the cache it is judged against, so this test
 * is not sensitive to what ran before it in the same process.
 *
 * THE BREAK-IT CHECK done while writing this: with the cache's
 * ShouldSetMousePassthrough forced to `return true;` unconditionally (the
 * shape of the bug this patch removes), the count after N frames reads N, not
 * 1 - this test fails exactly the way the old code behaves, which is the
 * proof it is testing the right thing.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>

#include "test_check.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include <GLFW/glfw3.h>

namespace {

// One un-docked window (the main viewport only), many frames, nothing ever
// asking for mouse passthrough: the wanted value is false on every single
// frame, so after the first frame records it, every later frame must skip
// the Win32 call outright.
void testSteadyStateFrameLoopMakesTheCallOnceNotEveryFrame() {
    ImGui_ImplGlfw_ResetMousePassthroughForTest();
    CHECK(ImGui_ImplGlfw_MousePassthroughSyscallCountForTest() == 0);

    const int kFrames = 50;
    for (int i = 0; i < kFrames; i++) {
        ImGui_ImplGlfw_NewFrame();
    }

    // The very first frame has no prior record for the window, so it must
    // still make the call once - this proves the cache does not simply
    // suppress the call outright, only the redundant repeats of it.
    const int count = ImGui_ImplGlfw_MousePassthroughSyscallCountForTest();
    CHECK(count == 1);
    if (count != 1) {
        std::printf("  got %d calls over %d frames (upstream's unfixed behaviour is %d)\n",
                    count, kFrames, kFrames);
    }
}

// A wanted value that keeps changing must still be applied every time it
// changes - the cache must never turn into "set it once and never again".
// NoInputs is normally driven by ImGui itself during a multi-viewport drag;
// this test flips it directly on the main viewport to reach the same
// decision (window_no_input) without staging a real drag.
void testAChangingValueIsStillAppliedEveryTime() {
    ImGui_ImplGlfw_ResetMousePassthroughForTest();

    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const ImGuiViewportFlags originalFlags = mainViewport->Flags;

    for (int i = 0; i < 6; i++) {
        if (i % 2 == 0) {
            mainViewport->Flags |= ImGuiViewportFlags_NoInputs;
        } else {
            mainViewport->Flags &= ~ImGuiViewportFlags_NoInputs;
        }
        ImGui_ImplGlfw_NewFrame();
    }

    mainViewport->Flags = originalFlags;

    // Six frames, alternating every time: every one of them must fire.
    const int count = ImGui_ImplGlfw_MousePassthroughSyscallCountForTest();
    CHECK(count == 6);
    if (count != 6) {
        std::printf("  got %d calls over 6 alternating frames, wanted 6\n", count);
    }
}

}  // namespace

int main() {
    if (!glfwInit()) {
        std::printf("FAIL glfwInit\n");
        ++g_checksFailed;
        ++g_checksRun;
        return testSummary("test_mouse_passthrough_cache");
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(320, 240, "test_mouse_passthrough_cache", nullptr, nullptr);
    CHECK(window != nullptr);
    if (window != nullptr) {
        glfwMakeContextCurrent(window);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(320.0f, 240.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        CHECK(ImGui_ImplGlfw_InitForOpenGL(window, false));

        testSteadyStateFrameLoopMakesTheCallOnceNotEveryFrame();
        testAChangingValueIsStillAppliedEveryTime();

        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
    }
    glfwTerminate();
    return testSummary("test_mouse_passthrough_cache");
}
