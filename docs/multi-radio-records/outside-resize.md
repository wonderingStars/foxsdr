# Outside resize: SetWindowPos from another program killed FoxSDR's window

Branch `fix/outside-resize-2`, cut from `17e9a61a`, worktree `D:/foxsdr-wt/en-screen-k`, build `D:/foxsdr-build/en-screen-k` (Release), hidden desktop `FoxSDROutsideResize`, scratch `D:/foxsdr-build/or2-scratch/`. Nothing pushed.

## What the six dumps prove (all six unwound by hand; `or2-scratch/unw.py`, `msg2.py`)

Every dump has the same stack. Fault thread = the UI thread. Outermost frames: `foxsdr-slint.exe` main -> `slint_cpp` event loop -> `user32!PeekMessageW` -> `win32u!NtUserPeekMessage` -> kernel callback -> window procedure.

| Fact | Evidence |
|---|---|
| The UI thread was NOT inside a frame or a presentation call. It was idle in the event loop's `PeekMessageW`. | unwinding from the saved `PeekMessageW` return address gives main -> run loop -> `PeekMessageW` and nothing else; no draw or present frame is on the stack |
| The foreign `SetWindowPos` arrives as a SENT message pair delivered inside that `PeekMessage` (kernel callback), on the UI thread. | frame chain `NtUserPeekMessage` -> `KiUserCallbackDispatch` -> `DispatchMessageW` -> window procedure |
| One window, the main one. | the HWND in the frames is the same in each dump; no second top-level window is involved |
| Sequence: `WM_WINDOWPOSCHANGED` (0x47) -> `DefWindowProc` -> `SendMessage` `WM_SIZE` (5) -> toolkit resize -> panic in a function that panics with "Surface::configure". | message numbers read from the saved argument slots of the two window-procedure frames |
| THE CAUSE: the size in those messages. In all six dumps `WINDOWPOS.cx = 1920`, `WINDOWPOS.cy = 65535`, and `WM_SIZE` lparam = 1920 x 65535. | WINDOWPOS read from the stack; `flags` 0x4 or 0x1006 |

The "frame in progress" reading of the earlier lane is NOT supported: nothing was in progress. The size was impossible.

Why 65535: `tools/window-size.ps1` assigns `$h = $p.MainWindowHandle`. PowerShell variable names are case-insensitive, so `$h` IS the typed `[int]$H` parameter (the height): the height asked for became the window handle's number (millions), which the window system cuts to 65535. Shown in a PowerShell snippet here (`param([int]$H=1080); $h=[IntPtr]5899640` leaves `$H` = 5899640, Int32); the dump agrees (cx matches `-W 1920`; cy and x, y do not match `-H 1080 -X 0 -Y 0`). The real script was not run. A fixed copy (variable renamed) is `D:/foxsdr-build/or2-scratch/window-size-fixed.ps1`, not run. Fix for the drive's script is the orchestrator's (rename the variable); the application fix below does not depend on it, because any program, or any typo, can ask for such a size.

## REPRODUCED on the hidden desktop (the release candidate exe of 16:14, before the fix)

`or2-scratch/log-a.txt`, harness `rz-harness.ps1` (copied from the earlier lane), real `SetWindowPos` from another process:

| Size asked | Result |
|---|---|
| 1920 x 65535 | DIED, `0xC0000409`, `slint_cpp.dll+0xB344C5` (the same offset as the real display kills) |
| 1920 x 16385 | DIED, the same offset |
| 1920 x 1080 | survived |

(The earlier lane's hidden-desktop runs stopped at 5120 x 4400 and never reached the limit.) So the toolkit panics when a side of the new size is larger than the largest texture side its surface allows; 16385 is over, and the 16384 limit is measured here on the hidden desktop's renderer. UNVERIFIED: the real display's graphics driver may have a different limit (a larger one cannot matter: 65535 is over every driver's; a smaller one than 16384 is possible but not measured).

## The fix (the brief's expected shape was replaced: the evidence shows no "frame in progress")

New files `src/slint/window_guard.hpp`, `src/slint/window_guard_win.cpp`. Every top-level window of the process that belongs to the UI thread is subclassed (`SetWindowSubclass`, comctl32). `WM_WINDOWPOSCHANGING`: a requested width or height over `kMaxWindowExtent` (16384) is lowered to it before the window system applies it and before the toolkit hears of it. `WM_GETMINMAXINFO`: the maximum sizes are lowered the same way (never raised). Installed by the host's 33 ms tick (`main.cpp`, ticks 1-3 and every 15th tick, so windows made later join). `state window.guard.clamps` reads how many requests were lowered.

Deliberately NOT done, with the reason: no minimum clamp (the toolkit already holds 1024 x 640; hidden-desktop runs of 0 x 0, 1 x 1 and 100 x 100 survived), no clamp to the virtual screen (19 committed drive scripts use windows 4400 to 8000 px tall, and a window across three monitors can be wider than one screen), no deferral (nothing was mid-frame).

## Tests

| Test | Result |
|---|---|
| End to end, hidden desktop, the NEW exe, real cross-process SetWindowPos (`or2-scratch/log-b.txt`) | 1920x65535, 1920x16385, 1920x16384, 1920x1080 all SURVIVED (rects 1920x16384, 1920x16384, 1920x16384, 1920x1080); 15 mixed calls (1080, 65535, 16385, 65535 wide, 1600x900, 100000) all alive, window held at the limit or the asked size |
| `test_slint_window_guard` (new; a real never-shown popup window on the hidden desktop, the same `SetWindowPos` from this thread and from another thread, the sizes the window's own procedure is handed read back) | RED with the clamp removed: `40 checks, 15 failed` (`or2-scratch/t-red.txt`); GREEN: `40 checks, 0 failed` |

Existing tests at -j1 on the hidden desktop after the final code change (`or2-scratch/ct-out.txt`): `18/18 passed`: drives window-min-strip, window-strip-1024/1280/1366/1440/1480/1920, layout-section-overlap-1600/624, bar-min-624, bar-small-1280, main-strip; test_slint_host_mustfixa, test_slint_main_screen, test_slint_lanec_host, test_slint_window_guard, test_error_paths (pin 410 untouched).

Changed expectations: none.

NOT RUN: the full headless and GUI gates, the other layout drives, the clip sweep, anything on the real display. Real config.json sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` after.

## UNVERIFIED / not mine
The real-display run (the orchestrator's): see the hand-back message. The tall-window fault of window-followups item 10 (4400 px and taller, with a picture and the clip recorder) is NOT shown to be the same: its heights are under 16384; this fix does not claim it.
