// pointer_ledger.hpp - THE POINTER LEDGER (window-gaps-2, row 1457): "Show the pointer ledger across the top of the window". For the report "my clicks land beside the control", every number
// the pointer passes through on its way from the operating system to a control, in one line across the top of the window, so a screenshot of the miss carries the disagreeing pair.
// The classic's line (app_window.cpp, the input ledger) quotes ImGui's mouse and GLFW's numbers; this window has no such pair, so it quotes the operating system's (the cursor, the client
// area's origin and size, the window rectangle), the window's own position, size and scale as Slint reports them, and the difference of the first two: where a click should land.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace foxsdr::slintgaps {

struct LedgerInputs {
    void* window = nullptr;       // the HWND (gaps_win findMainWindow); null on a platform with none
    int slintX = 0, slintY = 0;   // the window's position as the toolkit reports it
    int slintW = 0, slintH = 0;   // and its size, in physical pixels
    float scale = 1.0f;           // and its scale factor
};

// The line; "" when the operating system cannot be asked (no window, or not Windows).
std::string pointerLedgerLine(const LedgerInputs& in);

// The same line from numbers already read, so a test can pin its words without a window.
std::string formatPointerLedger(int cursorX, int cursorY, int originX, int originY, int clientW, int clientH, int winX, int winY, int winW, int winH, const LedgerInputs& in);

}  // namespace foxsdr::slintgaps
