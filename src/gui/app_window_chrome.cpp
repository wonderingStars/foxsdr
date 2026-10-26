// app_window_chrome.cpp - the window's own chrome: the two fold keys and the
// strips they fold to (0.99.49 and 0.99.74), and the interface size (0.99.44) -
// the style it rebuilds, the keys that step it, the minimum window it moves.
// AppWindow members kept out of app_window.cpp because they are one subject and
// that file is at the object-file limit (window_chrome.hpp).
//
// Re-expressed from master's app_window.cpp (stub campaign, batch B1). Nothing
// here touches the engine: all of it is the window's own state - the choice is
// carried to the config by currentConfig/applyConfig, the way the theme is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

#include <imgui.h>

#include <GLFW/glfw3.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "engine/tune_control.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"
#include "gui/ui_scale.hpp"
#include "gui/ui_style_compose.hpp"
#include "gui/window_chrome.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

// --- A FOLD KEY'S CHEVRONS (0.99.49) -----------------------------------------
//
// "<<" and ">>" cut at the size a key's word is lettered are two specks, so a
// fold key (the rail's, the status column's, the patch inspector's) is drawn by
// benchWordKey with no word and struck with two chevrons instead, in the same
// ink and with the same one-pixel drop while held. `pointLeft` is "<<".
void engraveFoldChevrons(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool pointLeft,
                         bool held) {
    if (dl == nullptr) { return; }
    const ImU32 ink = cascade::gui::theme::toneHex(
        0x2A251C, 255, held ? cascade::gui::theme::ink::ActiveText : cascade::gui::theme::ink::CtrlText);
    const float h = br.y - tl.y;
    const float cx = (tl.x + br.x) * 0.5f;
    const float cy = (tl.y + br.y) * 0.5f + (held ? 1.0f : 0.0f);
    const float arm = h * 0.22f;             // half the chevron's height
    const float depth = arm * 0.9f;          // how far its point stands out
    const float step = arm * 0.95f;          // between the two chevrons
    const float th = std::max(1.5f, cascade::gui::uiscale::px(1.6f));
    const float dir = pointLeft ? -1.0f : 1.0f;
    for (int k = 0; k < 2; ++k) {
        const float tipX = cx + dir * (depth * 0.5f + step * (k == 0 ? 0.5f : -0.5f));
        const float backX = tipX - dir * depth;
        const ImVec2 pts[3] = {ImVec2(backX, cy - arm), ImVec2(tipX, cy), ImVec2(backX, cy + arm)};
        dl->AddPolyline(pts, 3, ink, ImDrawFlags_None, th);
    }
}

// THE WORK AREA OF THE MONITOR A (POSSIBLY STILL HIDDEN) WINDOW IS ON. GLFW has
// no direct "which monitor is this windowed, non-fullscreen window on" query -
// only glfwGetWindowContentScale, which answers the DPI question but not the
// work-area one. Found the same way GLFW's own Win32 backend would: by which
// monitor's rect contains the window's position. Falls back to the primary
// monitor if none matches (should not happen - every window is on some monitor
// - but a monitor unplugged between enumeration and this call is not worth
// crashing over). Used at start-up (sizing the very first window) and every time
// the interface size changes afterwards (so the MINIMUM size this size demands
// never exceeds the screen it has to fit on).
void monitorWorkareaForWindow(GLFWwindow* window, int& areaX, int& areaY, int& areaW, int& areaH) {
    areaX = areaY = areaW = areaH = 0;
    if (window == nullptr) { return; }
    int wx = 0, wy = 0;
    glfwGetWindowPos(window, &wx, &wy);
    GLFWmonitor* target = glfwGetPrimaryMonitor();
    int monCount = 0;
    GLFWmonitor** mons = glfwGetMonitors(&monCount);
    for (int m = 0; m < monCount; ++m) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        glfwGetMonitorWorkarea(mons[m], &mx, &my, &mw, &mh);
        if (wx >= mx && wx < mx + mw && wy >= my && wy < my + mh) {
            target = mons[m];
            break;
        }
    }
    if (target != nullptr) { glfwGetMonitorWorkarea(target, &areaX, &areaY, &areaW, &areaH); }
}

namespace {
// THE UN-SCALED STYLE BASELINE, shared by the theme and the interface-size
// mechanisms below (app_window.hpp must not need imgui.h, so this cannot be a
// class member - see the note beside interfaceScale_ there). "Un-scaled" means:
// whatever theme::applyTheme() last wrote, before ScaleAllSizes/FontScaleMain
// touch it. It is refreshed right after EVERY theme apply and the factor
// immediately re-imposed on it (applyThemeAtScale), which is what makes
// theme-then-scale and scale-then-theme compose to the same style either way
// (tests/test_ui_scale.cpp, testThemeAndScaleComposeRegardlessOfOrder).
ImGuiStyle g_uiStyleBase;
bool g_uiStyleBaseCaptured = false;

void refreshUiStyleBase() {
    g_uiStyleBase = ImGui::GetStyle();
    g_uiStyleBaseCaptured = true;
}

// Rebuilds the live style from the un-scaled baseline at the CURRENT interface
// factor, through the one shared, independently-tested function
// (gui/ui_style_compose.hpp) both a theme apply and a scale change funnel
// through. Idempotent.
void applyUiScaleFromBase() {
    ImGui::GetStyle() = cascade::gui::uiscale::composeStyle(g_uiStyleBase, cascade::gui::uiscale::factor());
}
}  // namespace

void applyThemeAtScale(const std::function<void()>& writeTheme) {
    // Everything AROUND the theme's own write - restoring the live style to the
    // last unscaled baseline FIRST, then re-baselining on what the theme just
    // wrote, then re-composing at the current interface factor - is
    // uiscale::applyThemeComposed, the SAME function tests/test_ui_scale.cpp
    // drives against the real theme::applyTheme().
    cascade::gui::uiscale::applyThemeComposed(writeTheme, g_uiStyleBase, g_uiStyleBaseCaptured,
                                              cascade::gui::uiscale::factor());
}

// --- THE FOLD KEYS -----------------------------------------------------------

void AppWindow::drawRailFoldKey(float colX, float colY, float colW) {
    // The bench's lettered key (benchWordKey), in the plate's title row on the
    // open rail and at the top of the strip when it is folded. Pressing it is
    // the whole of folding: the next frame lays the column out at its new
    // width, and currentConfig carries the choice to the debounced save.
    const cascade::gui::RailFoldKeyRect r = cascade::gui::railFoldKeyRect(
        colX, colY, colW, railCollapsed_, cascade::gui::uiscale::factor());
    const ImVec2 tl(r.x0, r.y0);
    const ImVec2 br(r.x1, r.y1);
    const bool pressed = benchWordKey(ImGui::GetWindowDrawList(), tl, br, "", true,
                                      railCollapsed_ ? "railunfold" : "railfold");
    engraveFoldChevrons(ImGui::GetWindowDrawList(), tl, br, !railCollapsed_, ImGui::IsItemActive());
    cascade::gui::census::rect(railCollapsed_ ? "railkey:unfold" : "railkey:fold", tl.x, tl.y,
                               br.x, br.y);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", railCollapsed_
                                    ? tr("Show the Function Select panel")
                                    : tr("Fold the Function Select panel away to give the main "
                                         "view its width"));
    }
    if (pressed) {
        railCollapsed_ = !railCollapsed_;
        cascade::core::diagLogf("rail: %s", railCollapsed_ ? "folded" : "opened");
    }
}

void AppWindow::drawRailStrip() {
    // THE FOLDED RAIL: the plate's ground and bevel, the width of a key, and the
    // one key that opens it. Nothing else fits a strip, and nothing else is
    // wanted there - the point of folding it is the width.
    const ImVec2 tl = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    cascade::gui::addBenchPlate(ImGui::GetWindowDrawList(), tl,
                                ImVec2(tl.x + size.x, tl.y + size.y), nullptr);
    drawRailFoldKey(tl.x, tl.y, size.x);
}

void AppWindow::toggleStatusFold() {
    statusCollapsed_ = !statusCollapsed_;
    cascade::core::diagLogf("status: %s", statusCollapsed_ ? "folded" : "opened");
}

void AppWindow::drawStatusFoldKey(float colX, float colY, float colW) {
    // The rail's key (drawRailFoldKey), mirrored for the right-hand panel
    // (0.99.74, the tester: "Just like you added the << and >> ... can you do
    // this for the right side Status panel?"). At the LEFT end of the title row
    // when the column is open - the end nearest the spectrum, as the rail's sits
    // at its right end - and centred on the strip when it is folded. The
    // chevrons point the other way round: ">>" on the open column pushes it away
    // to the right, "<<" on the strip brings it back toward the spectrum.
    // Pressing it is the whole of folding: the next frame lays the column out at
    // its new width, and currentConfig carries the choice to the save.
    const cascade::gui::RailFoldKeyRect r = cascade::gui::statusFoldKeyRect(
        colX, colY, colW, statusCollapsed_, cascade::gui::uiscale::factor());
    const ImVec2 tl(r.x0, r.y0);
    const ImVec2 br(r.x1, r.y1);
    const bool pressed = benchWordKey(ImGui::GetWindowDrawList(), tl, br, "", true,
                                      statusCollapsed_ ? "statusunfold" : "statusfold");
    engraveFoldChevrons(ImGui::GetWindowDrawList(), tl, br, statusCollapsed_, ImGui::IsItemActive());
    cascade::gui::census::rect(statusCollapsed_ ? "statuskey:unfold" : "statuskey:fold", tl.x,
                               tl.y, br.x, br.y);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", statusCollapsed_
                                    ? tr("Show the Status panel")
                                    : tr("Fold the Status panel away to give the spectrum its "
                                         "width"));
    }
    if (pressed) { toggleStatusFold(); }
}

void AppWindow::drawStatusStrip() {
    // THE FOLDED STATUS COLUMN: the plate's ground and bevel, the width of a key,
    // and the one key that opens it. As narrow as the rail's strip and for the
    // same reason - the point of folding it is the width - and it bypasses
    // drawStatusColumn's 40-px guard, which would draw nothing here.
    const ImVec2 tl = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    cascade::gui::addBenchPlate(ImGui::GetWindowDrawList(), tl,
                                ImVec2(tl.x + size.x, tl.y + size.y), nullptr);
    drawStatusFoldKey(tl.x, tl.y, size.x);
}

// --- THE INTERFACE SIZE (gui/ui_scale.hpp) -----------------------------------
//
// EVERY FONT SIZE IS LIVE ALREADY: fonts::uiPx() and its three siblings read
// uiscale::factor() on every call, so the moment factor() changes, the very next
// PushFont/CalcTextSizeA at any of those call sites draws at the new size - no
// atlas rebuild, because Dear ImGui 1.92 rasterises a loaded face at whatever
// size it is asked for. What is NOT automatically live is the ImGuiStyle's own
// SIZES (WindowPadding, ItemSpacing, FramePadding, ScrollbarSize and the rest):
// the one supported way to scale a whole style is ScaleAllSizes(S) applied ONCE,
// from an UN-SCALED snapshot - calling it every frame, or twice from the same
// starting style, would compound the scale each time and the window would
// visibly grow on every change. The snapshot (g_uiStyleBase) is captured on this
// function's first call in the frame loop, but from then on is kept FRESH by
// every theme apply (applyThemeAtScale), so a scale change afterwards rescales
// the CURRENT theme's numbers rather than whichever theme happened to be live
// the first time this ran.
void AppWindow::applyPendingUiScale() {
    if (!g_uiStyleBaseCaptured) { refreshUiStyleBase(); }
    if (!cascade::gui::uiscale::consumeChanged()) { return; }
    applyUiScaleFromBase();
    // THE MINIMUM WINDOW SIZE MOVES WITH S TOO: set once at start-up (the
    // minimum times the STARTING factor) and never again, a later Ctrl+=/Display
    // pick, or a drag to a higher-DPI monitor, would leave the OS enforcing a
    // minimum sized for whatever S the window opened at - stale in either
    // direction. Recomputed here every real change, and clamped to the CURRENT
    // monitor's work area so a 200% saved on a 4K desk does not demand a
    // 1248x800 minimum on a 1366x768 laptop the next time this profile opens
    // there: a minimum past the work area would make the window impossible to
    // shrink to fit its own screen.
    if (mainWindow_ != nullptr) {
        int areaW = 0, areaH = 0, unusedX = 0, unusedY = 0;
        monitorWorkareaForWindow(mainWindow_, unusedX, unusedY, areaW, areaH);
        int minW = static_cast<int>(
            cascade::gui::uiscale::px(static_cast<float>(cascade::gui::kDeckMinWindowW)));
        int minH = static_cast<int>(
            cascade::gui::uiscale::px(static_cast<float>(cascade::gui::kWindowMinH)));
        if (areaW > 0) { minW = std::min(minW, areaW); }
        if (areaH > 0) { minH = std::min(minH, areaH); }
        glfwSetWindowSizeLimits(mainWindow_, minW, minH, GLFW_DONT_CARE, GLFW_DONT_CARE);
    }
    cascade::core::diagLogf("interface size: %s -> %.0f%% (monitor reports %u dpi)",
                            cascade::gui::uiscale::choice().c_str(),
                            static_cast<double>(cascade::gui::uiscale::factor()) * 100.0,
                            cascade::gui::uiscale::monitorDpi());
}

void AppWindow::handleInterfaceScaleKeys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || !(io.KeyCtrl)) { return; }
    // Repeat suppressed the way every other key-driven setting in this
    // application is: one step per press, held or not.
    if (ImGui::IsKeyPressed(ImGuiKey_0, false)) {
        interfaceScale_ = "auto";
    } else if (ImGui::IsKeyPressed(ImGuiKey_Equal, false)) {
        const int now = cascade::gui::uiscale::dpiToPercent(
            static_cast<unsigned>(std::lround(96.0 * static_cast<double>(cascade::gui::uiscale::factor()))));
        int i = 0;
        while (i < cascade::gui::uiscale::kStepCount && cascade::gui::uiscale::kSteps[i] <= now) { ++i; }
        if (i < cascade::gui::uiscale::kStepCount) {
            interfaceScale_ = std::to_string(cascade::gui::uiscale::kSteps[i]);
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Minus, false)) {
        const int now = cascade::gui::uiscale::dpiToPercent(
            static_cast<unsigned>(std::lround(96.0 * static_cast<double>(cascade::gui::uiscale::factor()))));
        int i = cascade::gui::uiscale::kStepCount - 1;
        while (i >= 0 && cascade::gui::uiscale::kSteps[i] >= now) { --i; }
        if (i >= 0) { interfaceScale_ = std::to_string(cascade::gui::uiscale::kSteps[i]); }
    } else {
        return;
    }
    cascade::gui::uiscale::setChoice(interfaceScale_);
}

}  // namespace cascade::gui
