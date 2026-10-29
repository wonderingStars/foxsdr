/*
 * A TOOLTIP OR POPUP NEVER GETS AN OPERATING SYSTEM WINDOW OF ITS OWN (the popups-stay-inside
 * patch, third_party/imgui/FOXSDR-PATCHES.md, 0.99.103).
 *
 * THE REPORT (2026-10-29, an Intel UHD 630 laptop): 81 minutes of 430-520 ms frames in the
 * `render` scope while the ADS-B map page had a window of its own, and later the driver refused a
 * shared GL context with ERROR_NO_SYSTEM_RESOURCES. The one mechanism in this tree that fits:
 * upstream ImGui lets a tooltip extend over the whole monitor, and Begin() gives a tooltip that
 * does not fit its host viewport a viewport - an OS window and a shared GL context - of its own,
 * created in UpdatePlatformWindows (inside `render`) and destroyed a frame after the tooltip goes.
 * An aircraft's hover detail near the map page's edge blinks such a context every time a target
 * drifts under the pointer.
 *
 * WHAT IS TESTED, with a real GL window and the real GLFW backend, the way test_viewports does it:
 *
 *   1. A page with a viewport of its own (ImGuiViewportFlags_NoAutoMerge, as the map pages are)
 *      gets exactly one platform window.
 *   2. A tooltip opened over that page at its right edge, which would protrude past it: its
 *      viewport stays the PAGE's, it is placed inside the page's window, and ImGui's
 *      PlatformWindowsCreatedCount does not move - over a hundred blinks (the field cadence).
 *      A tooltip wider than the whole page cannot be placed inside and is clamped to the
 *      page's left edge instead - and still has no window of its own.
 *   3. The same for a tooltip at the main window's edge: no window of its own.
 *
 * BREAK-IT CHECK, run while writing this: with the patch line restored to upstream
 * (`window->ViewportAllowPlatformMonitorExtend = window->Viewport->PlatformMonitor`) the count read
 * 101 after the hundred blinks and 121 after the twenty wide ones - one operating system window
 * and GL context per blink - and the tooltip's viewport was its own, not the page's: nine checks
 * red. With the patch, 1 throughout.
 *
 * This test puts a window on the desktop (the page's own, for about a second) and is in
 * CASCADE_GUI_TESTS. It never takes the pointer: every mouse position is an ImGui input event.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cfloat>
#include <cstdio>
#include <cstring>

#include "test_check.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include <GLFW/glfw3.h>

namespace {

constexpr float kMainX = 100.0f, kMainY = 100.0f, kMainW = 640.0f, kMainH = 480.0f;
constexpr float kPageX = 200.0f, kPageY = 200.0f, kPageW = 300.0f, kPageH = 200.0f;

struct Frame {
    // What this frame draws and where the pointer is, in screen coordinates.
    bool tooltipOnPage = false;
    // A tooltip wider than the page itself: it cannot be placed inside, and must
    // still not get a window of its own.
    bool tooltipWiderThanPage = false;
    bool tooltipOnMain = false;
    float mouseX = -FLT_MAX;
    float mouseY = -FLT_MAX;
    ImGuiID mouseViewport = 0;
};

// One frame: the backend's new frame, the pointer as an input event (after the backend's own
// events, so this one wins), the page and the tooltips, then the platform windows.
void frame(GLFWwindow* window, const Frame& f) {
    glfwPollEvents();
    ImGui_ImplGlfw_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(f.mouseX, f.mouseY);
    if (f.mouseViewport != 0) { io.AddMouseViewportEvent(f.mouseViewport); }
    ImGui::NewFrame();

    ImGuiWindowClass pageClass;
    pageClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&pageClass);
    ImGui::SetNextWindowPos(ImVec2(kPageX, kPageY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kPageW, kPageH), ImGuiCond_Always);
    ImGui::Begin("Page###page", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("a page with a window of its own, like the map pages");
    if (f.tooltipOnPage) {
        ImGui::BeginTooltip();
        // About 200 points wide: it fits the 300-point page once moved left of the
        // pointer, which sits 10 points from the page's right edge.
        ImGui::TextUnformatted("an aircraft's hover detail here");
        if (f.tooltipWiderThanPage) {
            ImGui::TextUnformatted(
                "a line wider than the whole page, which cannot be placed inside it at all");
        }
        ImGui::EndTooltip();
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(kMainX + 20.0f, kMainY + 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200.0f, 100.0f), ImGuiCond_Always);
    ImGui::Begin("Inside###inside", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    if (f.tooltipOnMain) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "a tooltip at the main window's right edge, wide enough to protrude past it");
        ImGui::EndTooltip();
    }
    ImGui::End();

    ImGui::Render();
    GLFWwindow* const restore = glfwGetCurrentContext();
    ImGui::UpdatePlatformWindows();
    glfwMakeContextCurrent(restore);
    (void)window;
}

bool inside(const ImRect& inner, const ImRect& outer) {
    return inner.Min.x >= outer.Min.x - 0.5f && inner.Min.y >= outer.Min.y - 0.5f &&
           inner.Max.x <= outer.Max.x + 0.5f && inner.Max.y <= outer.Max.y + 0.5f;
}

}  // namespace

int main() {
    std::printf("test_popup_stays_inside\n");
    if (!glfwInit()) {
        std::printf("FAIL glfwInit\n");
        ++g_checksFailed;
        ++g_checksRun;
        return testSummary("test_popup_stays_inside");
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(static_cast<int>(kMainW), static_cast<int>(kMainH),
                                          "test_popup_stays_inside", nullptr, nullptr);
    CHECK(window != nullptr);
    if (window == nullptr) {
        glfwTerminate();
        return testSummary("test_popup_stays_inside");
    }
    glfwSetWindowPos(window, static_cast<int>(kMainX), static_cast<int>(kMainY));
    glfwMakeContextCurrent(window);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(kMainW, kMainH);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // No renderer backend is initialised (nothing is drawn), but ImGui only keeps
    // viewports enabled when BOTH backends say they support them: without this
    // flag it silently drops ImGuiConfigFlags_ViewportsEnable at NewFrame, the page
    // never gets a window of its own, and every count below reads 0 - which is
    // exactly how the first run of this test went.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    CHECK(ImGui_ImplGlfw_InitForOpenGL(window, false));
    ImGuiContext* const ctx = ImGui::GetCurrentContext();

    // --- 1. the page gets its own window, once ------------------------------------------
    for (int i = 0; i < 6; ++i) { frame(window, Frame{}); }
    ImGuiWindow* page = ImGui::FindWindowByName("Page###page");
    CHECK(page != nullptr);
    CHECK(page != nullptr && page->ViewportOwned);
    CHECK(page != nullptr && page->Viewport != ImGui::GetMainViewport());
    const int afterPage = ctx->PlatformWindowsCreatedCount;
    std::printf("    platform windows after the page: %d\n", afterPage);
    CHECK(afterPage == 1);
    if (page == nullptr) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return testSummary("test_popup_stays_inside");
    }
    const ImGuiID pageViewport = page->ViewportId;

    // --- 2. a tooltip at the page's right edge stays the page's --------------------------
    // The pointer ten points from the page's right edge, over the page's viewport; the
    // tooltip's text is some 500 points wide, so with a monitor's extent it protrudes.
    Frame onPage;
    onPage.tooltipOnPage = true;
    onPage.mouseX = kPageX + kPageW - 10.0f;
    onPage.mouseY = kPageY + 60.0f;
    onPage.mouseViewport = pageViewport;
    for (int i = 0; i < 4; ++i) { frame(window, onPage); }
    ImGuiWindow* tip = ImGui::FindWindowByName("##Tooltip_00");
    CHECK(tip != nullptr);
    if (tip != nullptr) {
        page = ImGui::FindWindowByName("Page###page");
        CHECK(tip->Viewport == page->Viewport);
        CHECK(tip->ViewportAllowPlatformMonitorExtend == -1);
        CHECK(!tip->ViewportOwned);
        const ImRect pageRect = page->Viewport->GetMainRect();
        std::printf("    tooltip %.0f,%.0f %.0fx%.0f inside the page's window %.0f,%.0f %.0fx%.0f\n",
                    tip->Pos.x, tip->Pos.y, tip->Size.x, tip->Size.y, pageRect.Min.x, pageRect.Min.y,
                    pageRect.GetWidth(), pageRect.GetHeight());
        CHECK(inside(tip->Rect(), pageRect));
        // The tooltip is not a sliver: it was placed, not clipped to nothing.
        CHECK(tip->Size.x > 100.0f && tip->Size.y > 10.0f);
    }
    CHECK(ctx->PlatformWindowsCreatedCount == afterPage);

    // A hundred blinks - on for two frames, off for two - at the field's cadence: not one
    // window more. Upstream makes one per blink.
    for (int blink = 0; blink < 100; ++blink) {
        frame(window, onPage);
        frame(window, onPage);
        Frame off = onPage;
        off.tooltipOnPage = false;
        frame(window, off);
        frame(window, off);
    }
    std::printf("    platform windows after 100 blinks over the page: %d\n",
                ctx->PlatformWindowsCreatedCount);
    CHECK(ctx->PlatformWindowsCreatedCount == afterPage);

    // A tooltip wider than the page: it cannot be placed inside, so it is clamped to
    // the page's left edge and clipped at the right - and it STILL has no window of
    // its own. (Upstream: this is the case that always made one.)
    Frame wide = onPage;
    wide.tooltipWiderThanPage = true;
    for (int blink = 0; blink < 20; ++blink) {
        frame(window, wide);
        frame(window, wide);
        if (blink == 0) {
            tip = ImGui::FindWindowByName("##Tooltip_00");
            page = ImGui::FindWindowByName("Page###page");
            CHECK(tip != nullptr && page != nullptr);
            if (tip != nullptr && page != nullptr) {
                std::printf("    wide tooltip %.0f,%.0f %.0fx%.0f on a %.0f-point page\n", tip->Pos.x,
                            tip->Pos.y, tip->Size.x, tip->Size.y, kPageW);
                CHECK(tip->Size.x > kPageW);
                CHECK(tip->Viewport == page->Viewport);
                CHECK(!tip->ViewportOwned);
                CHECK(tip->Pos.x >= kPageX - 0.5f && tip->Pos.x < kPageX + 20.0f);
            }
        }
        Frame off = onPage;
        off.tooltipOnPage = false;
        frame(window, off);
        frame(window, off);
    }
    std::printf("    platform windows after 20 blinks of the wide tooltip: %d\n",
                ctx->PlatformWindowsCreatedCount);
    CHECK(ctx->PlatformWindowsCreatedCount == afterPage);
    // ...and the page's own window is still there.
    page = ImGui::FindWindowByName("Page###page");
    CHECK(page != nullptr && page->ViewportOwned && page->Viewport->PlatformWindowCreated);

    // --- 3. a tooltip at the main window's edge stays in the main window ---------------
    Frame onMain;
    onMain.tooltipOnMain = true;
    onMain.mouseX = kMainX + kMainW - 10.0f;
    onMain.mouseY = kMainY + 60.0f;
    onMain.mouseViewport = ImGui::GetMainViewport()->ID;
    for (int blink = 0; blink < 20; ++blink) {
        frame(window, onMain);
        frame(window, onMain);
        tip = ImGui::FindWindowByName("##Tooltip_00");
        if (blink == 0) {
            CHECK(tip != nullptr);
            if (tip != nullptr) {
                CHECK(tip->Viewport == ImGui::GetMainViewport());
                CHECK(tip->ViewportAllowPlatformMonitorExtend == -1);
                CHECK(inside(tip->Rect(), static_cast<ImGuiViewportP*>(ImGui::GetMainViewport())->GetMainRect()));
            }
        }
        Frame off = onMain;
        off.tooltipOnMain = false;
        frame(window, off);
        frame(window, off);
    }
    std::printf("    platform windows after 20 blinks over the main window: %d\n",
                ctx->PlatformWindowsCreatedCount);
    CHECK(ctx->PlatformWindowsCreatedCount == afterPage);

    ImGui::DestroyPlatformWindows();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return testSummary("test_popup_stays_inside");
}
