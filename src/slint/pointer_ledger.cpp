// pointer_ledger.cpp - see pointer_ledger.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pointer_ledger.hpp"

#include <cstdio>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace foxsdr::slintgaps {

std::string formatPointerLedger(int cursorX, int cursorY, int originX, int originY, int clientW, int clientH, int winX, int winY, int winW, int winH, const LedgerInputs& in) {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "os cursor %d,%d | client origin %d,%d client %dx%d | window %d,%d %dx%d | toolkit window %d,%d %dx%d scale %.2f | cursor in client %d,%d",
                  cursorX, cursorY, originX, originY, clientW, clientH, winX, winY, winW, winH, in.slintX, in.slintY, in.slintW, in.slintH, static_cast<double>(in.scale),
                  cursorX - originX, cursorY - originY);
    return line;
}

std::string pointerLedgerLine(const LedgerInputs& in) {
#if defined(_WIN32)
    HWND hw = static_cast<HWND>(in.window);
    if (hw == nullptr) { return {}; }
    POINT os{0, 0};
    POINT origin{0, 0};
    RECT cr{0, 0, 0, 0};
    RECT wr{0, 0, 0, 0};
    if (!GetCursorPos(&os)) { os.x = -1; os.y = -1; }   // a desktop that is not the input desktop (a bounded run) will not say: the line still carries the rest
    ClientToScreen(hw, &origin);
    GetClientRect(hw, &cr);
    GetWindowRect(hw, &wr);
    return formatPointerLedger(os.x, os.y, origin.x, origin.y, cr.right, cr.bottom, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, in);
#else
    (void)in;
    return {};
#endif
}

}  // namespace foxsdr::slintgaps
