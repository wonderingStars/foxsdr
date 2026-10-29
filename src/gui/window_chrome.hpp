// window_chrome.hpp - what the window's chrome code (app_window_chrome.cpp) and
// app_window.cpp share without either owning the other: the lettered brass key
// the fold keys are made of, the chevrons struck on them, the work area of the
// monitor a window is on, and the one function a theme change and an
// interface-size change both funnel through.
//
// WHY A HEADER OF ITS OWN. app_window.cpp is at the object-file limit (MSVC
// C1128, src/gui/app_window_disk_state.hpp), so every batch of the stub
// campaign puts its new window members in a translation unit of their own; these
// are the few helpers such a unit has to call back into app_window.cpp for (or
// that app_window.cpp has to call into it for), declared once here. Not part of
// AppWindow's interface and not included by app_window.hpp, which must stay free
// of imgui.h.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_WINDOW_CHROME_HPP
#define CASCADE_GUI_WINDOW_CHROME_HPP

#include <functional>

#include <imgui.h>

struct GLFWwindow;

namespace cascade::gui {

// The height of the smallest main window (client area), logical pixels: the
// height at which the bar and the head of the function rail still sit on screen
// together. The width is kDeckMinWindowW (core/tune_control.hpp); both are
// multiplied by the interface size wherever they are handed to GLFW.
inline constexpr int kWindowMinH = 400;

// A lettered brass key with a real ImGui item behind it (defined in
// app_window.cpp). `label` may be empty: a fold key is struck with chevrons
// instead (engraveFoldChevrons).
bool benchWordKey(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, const char* label,
                  bool enabled, const char* id);

// "<<" (pointLeft) or ">>" struck as two chevrons in the ink a key's word is cut
// in, with the same one-pixel drop while held. Used by the rail's and the status
// column's fold keys and, from the patch page's batch, the inspector's.
void engraveFoldChevrons(ImDrawList* dl, const ImVec2& tl, const ImVec2& br, bool pointLeft,
                         bool held);

// The work area of the monitor a (possibly still hidden) window is on; zeros
// when GLFW knows none.
void monitorWorkareaForWindow(GLFWwindow* window, int& areaX, int& areaY, int& areaW, int& areaH);

// A theme pick's full effect on the live ImGui style at the live interface
// size: `writeTheme` is what writes the theme (theme::setTheme + applyTheme +
// the typeface pair), and this restores the un-scaled baseline before it,
// re-baselines after it and re-composes at uiscale::factor(). The baseline is
// the one applyPendingUiScale rebuilds from, which is why both live together.
void applyThemeAtScale(const std::function<void()>& writeTheme);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_WINDOW_CHROME_HPP
