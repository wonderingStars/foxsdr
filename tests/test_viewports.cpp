/*
 * A DISPLAY THAT WILL NOT GIVE A SECOND OPENGL CONTEXT MUST NOT KILL THE APP.
 *
 * THE REPORT (0.95.0, Windows 10.0.22621, 144 dpi). A tester dragged a page
 * out of the main window and the log said:
 *
 *     GLFW error 65543: WGL: Failed to create OpenGL context
 *     ...access violation in glfwGetWin32Window
 *       <- ImGui_ImplGlfw_CreateWindow <- ImGui::UpdatePlatformWindows
 *       <- AppWindow::run
 *
 * Their driver refused a second context sharing the first. glfwCreateWindow
 * answered nullptr and the backend's very next line handed that nullptr to
 * glfwGetWin32Window. This file holds both halves of the fix:
 *
 *   THE DECISION, which is pure - the env switch, the startup probe and the
 *   runtime refusal count, in gui/viewport_policy.hpp.
 *
 *   THE BACKEND GUARD, which is not: every platform callback in
 *   imgui_impl_glfw.cpp is called on a viewport whose window is null, which is
 *   exactly the state a refused creation leaves behind and exactly what ImGui
 *   does with it before the application notices. There is no way to make this
 *   machine's driver refuse, so the backend carries a test seam that makes the
 *   next creation fail - a guard nothing has ever exercised is a guard nobody
 *   knows works.
 *
 * THE BREAK-IT CHECKS, both run while writing this (0.96.1).
 *
 *   The backend guard: with the null check removed from
 *   ImGui_ImplGlfw_ShowWindow, this test prints no failure at all - it dies at
 *   0xC0000005 inside GLFW, which is the same signature the field report
 *   carried. That is the only shape this particular red can have, and it is
 *   why the checks around driveEveryPlatformCallback are written as "reaching
 *   the next line is the result".
 *
 *   The decision: with the probe and the failure count ignored in
 *   viewportDecision, nine checks go red, starting
 *   "FAIL tests/test_viewports.cpp:92  d == ViewportDecision::ProbeFailed".
 *
 * AND ONE THING THIS FILE HAD TO BE FIXED FOR FIRST. It read the log line as
 * `const char*` and passed it straight to std::string - so the very run that
 * had a failure to report crashed instead of reporting it, because
 * viewportDecisionLine answers null when there is nothing to say. lineOf()
 * below is the fix; a CHECK that records and carries on must never be followed
 * by a dereference of the thing it just checked.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <string>

#include "gui/viewport_policy.hpp"
#include "test_check.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include <GLFW/glfw3.h>

namespace {

using cascade::gui::ViewportDecision;
using cascade::gui::viewportDecision;
using cascade::gui::viewportDecisionLine;
using cascade::gui::viewportsEnabled;

// CHECK records and carries on, so a plain `CHECK(line != nullptr)` followed by
// std::string(line) crashes in exactly the run that has something to report -
// the failure would kill the process instead of naming the expectation. Every
// read of the line goes through this.
std::string lineOf(ViewportDecision d) {
    const char* s = viewportDecisionLine(d);
    return (s != nullptr) ? std::string(s) : std::string();
}

// --- 1. the decision, which needs no display ------------------------------

void testTheHealthyMachineGetsViewports() {
    const ViewportDecision d = viewportDecision(nullptr, true, 0);
    CHECK(d == ViewportDecision::Enabled);
    CHECK(viewportsEnabled(d));
    // Nothing to explain, so nothing is logged: a line here would appear on
    // every healthy run and mean nothing.
    CHECK(viewportDecisionLine(d) == nullptr);
}

void testTheDeveloperSwitchWinsOverEverything() {
    // Set, and set to something that is not "0" or empty. The self-capture
    // depends on this behaving identically on every machine, so the switch is
    // read before the probe and before the failure count.
    CHECK(viewportDecision("1", true, 0) == ViewportDecision::SingleEnvVar);
    CHECK(viewportDecision("1", false, 7) == ViewportDecision::SingleEnvVar);
    CHECK(viewportDecision("yes", true, 0) == ViewportDecision::SingleEnvVar);
    CHECK(!viewportsEnabled(viewportDecision("1", true, 0)));

    // ...and the three spellings of "not set", which is the reading the
    // application has always used. `set FOXSDR_SINGLE_VIEWPORT=0` must not
    // quietly disable the feature it looks like it is enabling.
    CHECK(viewportDecision(nullptr, true, 0) == ViewportDecision::Enabled);
    CHECK(viewportDecision("", true, 0) == ViewportDecision::Enabled);
    CHECK(viewportDecision("0", true, 0) == ViewportDecision::Enabled);
}

void testAFailedProbeTurnsViewportsOffBeforeTheFirstPageIsDrawn() {
    const ViewportDecision d = viewportDecision(nullptr, false, 0);
    CHECK(d == ViewportDecision::ProbeFailed);
    CHECK(!viewportsEnabled(d));
    const std::string line = lineOf(d);
    // The GLFW error number is quoted deliberately: the log two lines above
    // already carries it, and a reader matching the two is why this exists.
    CHECK(line.find("65543") != std::string::npos);
    CHECK(line.find("stays inside the main window") != std::string::npos);
    if (line.empty()) { std::printf("     no line for a failed probe\n"); }
}

void testARefusalWhileRunningTurnsThemOffForTheSession() {
    const ViewportDecision d = viewportDecision(nullptr, true, 1);
    CHECK(d == ViewportDecision::RuntimeFailure);
    CHECK(!viewportsEnabled(d));
    const std::string line = lineOf(d);
    CHECK(line.find("65543") != std::string::npos);
    CHECK(line.find("for this session") != std::string::npos);
    if (line.empty()) { std::printf("     no line for a runtime refusal\n"); }

    // A probe that failed still reports the probe, not the count: with
    // viewports never enabled the count could not have risen, so reporting it
    // would name the wrong cause.
    CHECK(viewportDecision(nullptr, false, 3) == ViewportDecision::ProbeFailed);
}

// --- 2. the backend guard, which needs a window ---------------------------

// Every platform callback the GLFW backend installs, called on `vp`. Each of
// these used to dereference the viewport's GLFWwindow* without asking whether
// there was one.
void driveEveryPlatformCallback(ImGuiViewport* vp) {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Platform_ShowWindow) { pio.Platform_ShowWindow(vp); }
    if (pio.Platform_SetWindowPos) { pio.Platform_SetWindowPos(vp, ImVec2(11.0f, 22.0f)); }
    if (pio.Platform_SetWindowSize) { pio.Platform_SetWindowSize(vp, ImVec2(33.0f, 44.0f)); }
    if (pio.Platform_SetWindowTitle) { pio.Platform_SetWindowTitle(vp, "no window"); }
    if (pio.Platform_SetWindowFocus) { pio.Platform_SetWindowFocus(vp); }
    if (pio.Platform_SetWindowAlpha) { pio.Platform_SetWindowAlpha(vp, 0.5f); }
    if (pio.Platform_GetWindowFramebufferScale) { pio.Platform_GetWindowFramebufferScale(vp); }
    if (pio.Platform_RenderWindow) { pio.Platform_RenderWindow(vp, nullptr); }
    if (pio.Platform_SwapBuffers) { pio.Platform_SwapBuffers(vp, nullptr); }
}

void testARefusedViewportWindowSurvivesEveryPlatformCallback(GLFWwindow* main) {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    CHECK(pio.Platform_CreateWindow != nullptr);
    if (pio.Platform_CreateWindow == nullptr) { return; }

    const int before = ImGui_ImplGlfw_ViewportWindowCreationFailures();

    ImGuiViewport vp;
    vp.Pos = ImVec2(100.0f, 200.0f);
    vp.Size = ImVec2(320.0f, 240.0f);

    ImGui_ImplGlfw_FailNextViewportWindowForTest();
    pio.Platform_CreateWindow(&vp);

    // The refusal was counted rather than used, and nothing was published for
    // ImGui to hand to the renderer.
    CHECK(ImGui_ImplGlfw_ViewportWindowCreationFailures() == before + 1);
    CHECK(vp.PlatformHandle == nullptr);
    CHECK(vp.PlatformHandleRaw == nullptr);
    CHECK(vp.PlatformUserData != nullptr);  // the backend still owns its record

    // THIS IS THE CRASH, DRIVEN ON PURPOSE. ImGui goes on calling these until
    // the application turns viewports off, so every one of them has to survive
    // a viewport with no window. Reaching the line after this call IS the
    // check: the unguarded backend takes an access violation inside GLFW.
    driveEveryPlatformCallback(&vp);

    // The readers answer from what ImGui already believes rather than from a
    // window that does not exist.
    if (pio.Platform_GetWindowPos) {
        const ImVec2 p = pio.Platform_GetWindowPos(&vp);
        CHECK(p.x == 100.0f && p.y == 200.0f);
    }
    if (pio.Platform_GetWindowSize) {
        const ImVec2 s = pio.Platform_GetWindowSize(&vp);
        CHECK(s.x == 320.0f && s.y == 240.0f);
    }
    if (pio.Platform_GetWindowFocus) { CHECK(pio.Platform_GetWindowFocus(&vp) == false); }
    // Minimized, because there is nothing to draw into - which is what stops
    // ImGui trying to render it before the application notices.
    if (pio.Platform_GetWindowMinimized) { CHECK(pio.Platform_GetWindowMinimized(&vp) == true); }

    // And it can be destroyed: WindowOwned is false, so no null window ever
    // reaches glfwDestroyWindow.
    pio.Platform_DestroyWindow(&vp);
    CHECK(vp.PlatformUserData == nullptr);

    (void)main;
}

// THE 0.99.0 CRASH REPORT (2026-09-18, a HackRF on Windows 10.0.22631): the log
// says "GLFW error 65543: WGL: Failed to create OpenGL context", then the
// 0.96.1 fallback's own line "every page stays inside the main window for this
// session" - and nine seconds later an access violation in
// _glfwWindowFocusedWin32 <- ImGui_ImplGlfw_NewFrame <- AppWindow::run.
//
// 0.96.1 made every platform CALLBACK survive a viewport with no window. It
// missed the two loops the backend runs by itself at the top of every frame,
// ImGui_ImplGlfw_UpdateMouseData and ImGui_ImplGlfw_UpdateMouseCursor: both
// walk platform_io.Viewports and hand each PlatformHandle to GLFW without
// asking whether there is one, and a viewport whose window was refused stays
// in that list until ImGui gets round to dropping it.
//
// Driven exactly as the field had it: a refused viewport sitting in the list
// when NewFrame runs. As with the callbacks above, reaching the next line IS
// the check - the unguarded loops die at 0xC0000005 inside GLFW, which is the
// report's own signature.
void testANewFrameSurvivesARefusedViewportStillInTheList() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    CHECK(pio.Platform_CreateWindow != nullptr);
    if (pio.Platform_CreateWindow == nullptr) { return; }

    ImGuiViewport vp;
    vp.ID = 0x0F0C5D12u;
    vp.Pos = ImVec2(100.0f, 200.0f);
    vp.Size = ImVec2(320.0f, 240.0f);
    ImGui_ImplGlfw_FailNextViewportWindowForTest();
    pio.Platform_CreateWindow(&vp);
    CHECK(vp.PlatformHandle == nullptr);

    const int listed = pio.Viewports.Size;
    pio.Viewports.push_back(&vp);

    // Both cursor branches: the ordinary arrow, and "ImGui draws the cursor",
    // which takes the other arm of UpdateMouseCursor's loop.
    ImGuiIO& io = ImGui::GetIO();
    const bool drawCursorWas = io.MouseDrawCursor;
    io.MouseDrawCursor = false;
    ImGui_ImplGlfw_NewFrame();
    io.MouseDrawCursor = true;
    ImGui_ImplGlfw_NewFrame();
    io.MouseDrawCursor = drawCursorWas;
    CHECK(true);  // reaching here is the result

    // Taken back out before anything else looks at the list: it is a stack
    // object and ImGui did not put it there.
    pio.Viewports.pop_back();
    CHECK(pio.Viewports.Size == listed);
    pio.Platform_DestroyWindow(&vp);
    CHECK(vp.PlatformUserData == nullptr);
}

void testAViewportWithNoBackendRecordAtAllIsAlsoSafe() {
    // The other null: PlatformUserData never set. ImGui does not normally
    // produce this, but every callback reads that pointer first and a crash
    // here would be the same crash for a different reason.
    ImGuiViewport vp;
    vp.Pos = ImVec2(1.0f, 2.0f);
    vp.Size = ImVec2(3.0f, 4.0f);
    vp.PlatformUserData = nullptr;
    driveEveryPlatformCallback(&vp);
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    if (pio.Platform_GetWindowFocus) { CHECK(pio.Platform_GetWindowFocus(&vp) == false); }
    if (pio.Platform_DestroyWindow) { pio.Platform_DestroyWindow(&vp); }
    CHECK(true);  // reaching here is the result
}

}  // namespace

int main() {
    testTheHealthyMachineGetsViewports();
    testTheDeveloperSwitchWinsOverEverything();
    testAFailedProbeTurnsViewportsOffBeforeTheFirstPageIsDrawn();
    testARefusalWhileRunningTurnsThemOffForTheSession();

    // The backend half needs a real GL window, exactly as app_smoke does. A
    // failure to get one is reported as a failure rather than skipped: this
    // suite already requires a display, and a check that cannot fail is
    // decoration.
    if (!glfwInit()) {
        std::printf("FAIL glfwInit\n");
        ++g_checksFailed;
        ++g_checksRun;
        return testSummary("test_viewports");
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(320, 240, "test_viewports", nullptr, nullptr);
    CHECK(window != nullptr);
    if (window != nullptr) {
        glfwMakeContextCurrent(window);
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(320.0f, 240.0f);
        io.DeltaTime = 1.0f / 60.0f;
        // 1.92 lets the backend own texture uploads; saying so is what makes a
        // context with no renderer behind it legal.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        CHECK(ImGui_ImplGlfw_InitForOpenGL(window, false));

        testARefusedViewportWindowSurvivesEveryPlatformCallback(window);
        testANewFrameSurvivesARefusedViewportStillInTheList();
        testAViewportWithNoBackendRecordAtAllIsAlsoSafe();

        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
    }
    glfwTerminate();
    return testSummary("test_viewports");
}
