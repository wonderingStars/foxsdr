// sysmenu_win.cpp - see sysmenu_win.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sysmenu_win.hpp"

#include "gaps_win.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace foxsdr::slintsysmenu {

namespace {

#if defined(_WIN32)
class WindowsMenu final : public SystemMenu {
public:
    void* window() override { return foxsdr::slintgaps::findMainWindow(); }
    int itemCount(void* window) override {
        if (window == nullptr) { return -1; }
        HMENU m = ::GetSystemMenu(static_cast<HWND>(window), FALSE);   // a copy-free read: FALSE is "give me the window's own menu", not a reset
        return m == nullptr ? -1 : ::GetMenuItemCount(m);
    }
    bool show(void* window) override {
        if (window == nullptr) { return false; }
        const HWND hwnd = static_cast<HWND>(window);
        HMENU m = ::GetSystemMenu(hwnd, FALSE);
        if (m == nullptr) { return false; }
        POINT pt{};
        if (!::GetCursorPos(&pt)) { return false; }
        // The menu belongs to the foreground window or it never closes on a click elsewhere (Microsoft's TrackPopupMenu remarks).
        ::SetForegroundWindow(hwnd);
        const BOOL picked = ::TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN, pt.x, pt.y, hwnd, nullptr);
        if (picked != 0) { ::PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(picked), 0); }
        return true;
    }
};
#else
class NoMenu final : public SystemMenu {
public:
    void* window() override { return nullptr; }
    int itemCount(void*) override { return -1; }
    bool show(void*) override { return false; }
};
#endif

}  // namespace

std::unique_ptr<SystemMenu> makeWindowsSystemMenu() {
#if defined(_WIN32)
    return std::make_unique<WindowsMenu>();
#else
    return std::make_unique<NoMenu>();
#endif
}

bool requestSystemMenu(SystemMenu& menu, std::string& note) {
    note.clear();
    void* const w = menu.window();
    if (w == nullptr) { note = "the system menu: the window's handle is not there yet"; return false; }
    if (!menu.show(w)) { note = "the system menu: the window has none to show"; return false; }
    return true;
}

}  // namespace foxsdr::slintsysmenu
