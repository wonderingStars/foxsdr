// THE WINDOW GUARD (docs/multi-radio-records/outside-resize.md). A size asked of FoxSDR's window by ANOTHER program (a window manager, a tiling tool, a script,
// a remote-desktop client: SetWindowPos from outside) is not limited by anything the toolkit knows about. The toolkit reconfigures its drawing surface to the new
// client size and PANICS (and with it the whole application ends, exit 0xC0000409) when a side is larger than the graphics driver's largest texture. Measured on
// the 0.99.109 release candidate: height 16384 survives, height 16385 kills it, every time, at the same instruction. The guard subclasses each top-level window
// of the process and keeps every requested width and height at or under kMaxWindowExtent, before the toolkit hears of it; nothing the user can do with the
// pointer or the keys reaches the limit (an 8K screen is 7680 x 4320).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace foxsdr::slintguard {

// The largest width or height, in physical pixels, the window is ever asked to have. 16384 is the largest texture side every Direct3D 11 class driver
// offers, and the largest side the release candidate was seen to survive.
constexpr int kMaxWindowExtent = 16384;

// Pure rule: returns the extent to use for a requested one (a request of zero or less is passed through: the window system's own rules for
// empty windows stay in charge). `clamped` (if given) is set when the request was lowered.
int clampExtent(int requested, bool* clamped = nullptr);

// Windows only (window_guard_win.cpp). Subclasses every top-level window of this process that belongs to the calling thread and is not subclassed yet;
// safe to call again and again (windows that already carry the guard are skipped). Returns how many windows carry the guard after the call.
int installWindowGuards();

// Subclasses one window (the calling thread must own it). Returns true when it carries the guard afterwards.
bool installWindowGuard(void* hwnd);

// How many requests the guard has lowered since the process began (`state window.guard.clamps`).
long guardClampCount();

}  // namespace foxsdr::slintguard
