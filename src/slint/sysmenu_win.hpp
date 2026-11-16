// sysmenu_win.hpp - THE TITLE RAIL'S SYSTEM MENU (window gaps lane 1, ledger row 14): a right-click on the 36 px title rail opens Windows' own system menu of the main window
// (Restore, Move, Size, Minimize, Maximize, Close), as the classic window's rail does. The first phase's note said the native handle was not reachable; gaps_win.hpp's findMainWindow
// reaches it now, so the menu is the window's own (GetSystemMenu), and the command the person picks is posted back to the window as WM_SYSCOMMAND.
//
// Everything native is behind SystemMenu so the hand-over is tested with a stand-in: no test raises the real menu. Whether the real menu looks right on a frameless window is
// UNVERIFIED until the mouse drive; `itemCount` is the one thing a hidden-desktop run can read (the handle is reachable and the window has a system menu).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <memory>
#include <string>

namespace foxsdr::slintsysmenu {

class SystemMenu {
public:
    virtual ~SystemMenu() = default;
    // The window's handle (an HWND as a pointer); nullptr = none yet.
    virtual void* window() = 0;
    // The number of entries the window's system menu holds, or -1 when the window has none (GetSystemMenu answered nothing).
    virtual int itemCount(void* window) = 0;
    // Show the menu at the pointer and post the picked command back to the window. True when the menu was shown (whatever was picked).
    virtual bool show(void* window) = 0;
};

// The Windows one (GetSystemMenu + TrackPopupMenuEx); elsewhere a menu that is never there.
std::unique_ptr<SystemMenu> makeWindowsSystemMenu();

// What a right-click on the rail does: ask the menu to show; when it cannot, SAY why (the host's note) and do nothing else. Returns true when shown.
bool requestSystemMenu(SystemMenu& menu, std::string& note);

}  // namespace foxsdr::slintsysmenu
