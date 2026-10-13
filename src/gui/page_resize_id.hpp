// page_resize_id.hpp - the one place the application asks ImGui for the id of a
// window's resize corner, so that a window caught in the middle of being
// docked cannot take the process down.
//
// WHY THIS FILE EXISTS. Field crash DE3F72AAE6B69997 (0.99.75, fatal; the same
// frames in D94CCD4584724FAC on 0.99.58): 0xC0000005 in
// ImGui::GetWindowResizeCornerID+0x1b, called from AppWindow::beginPage+0xa3c,
// under drawPluginWindows. beginPage lights the page grip by comparing the
// hovered and active ids with that function's answer, and ImGui 1.92.8 answers
//
//     window->DockIsActive ? window->DockNode->HostWindow->ID : window->ID
//
// (third_party/imgui/imgui.cpp, GetWindowResizeCornerID). DockIsActive is set
// BEFORE the dock node has a host window: DockContextProcessDock marks the
// window it has just put in a new node (imgui.cpp, "target_window->DockIsActive
// = true", node still unhosted), and BeginDocked marks a window whose node has
// no HostWindow yet while the node is hidden for its windows resizing, or
// whose dockspace has just gone. ImGui's own code never reads the pointer in
// that state - it only asks for a resize id when `DockNodeAsHost ||
// !DockIsActive` (imgui.cpp, handle_borders_and_resize_grips) - but the page
// grip asked unconditionally, once in every frame of a window being dropped
// into, or pulled out of, a dock node. The reporting machine had opened a WEFAX
// window five minutes before and had filed a bug saying that inserting a
// window at the bottom with the mouse does not work.
//
// So: ask here, and get 0 - which no ImGui id ever is - in the frames in which
// there is no host to ask. A caller that compares an id with the answer must
// not take 0 == 0 for a match, which is what pageGripIdHot is for.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PAGE_RESIZE_ID_HPP
#define CASCADE_GUI_PAGE_RESIZE_ID_HPP

#include <imgui.h>
#include <imgui_internal.h>

namespace cascade::gui {

// The id of `w`'s lower-right resize corner, or 0 when it cannot be known this
// frame: `w` is null, or it is docked (DockIsActive) into a node that has no
// node or no host window yet. Never call ImGui::GetWindowResizeCornerID /
// GetWindowResizeBorderID from application code; tests/test_page_resize_id.cpp
// scans src/ for it.
inline ImGuiID pageResizeCornerId(ImGuiWindow* w) {
    if (w == nullptr) { return 0; }
    if (w->DockIsActive && (w->DockNode == nullptr || w->DockNode->HostWindow == nullptr)) {
        return 0;
    }
    return ImGui::GetWindowResizeCornerID(w, 0);
}

// Whether the resize corner whose id is `corner` is the thing under the pointer
// (`hovered` = ImGui::GetHoveredID()) or being dragged (`active` =
// ImGui::GetActiveID()). Both of those are 0 when nothing is, so an unknown
// corner (0) is not "lit".
inline bool pageGripIdHot(ImGuiID corner, ImGuiID hovered, ImGuiID active) {
    return corner != 0 && (hovered == corner || active == corner);
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PAGE_RESIZE_ID_HPP
