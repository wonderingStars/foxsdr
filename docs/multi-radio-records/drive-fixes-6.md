# Drive fixes 6

Branch `fix/drive-6`, cut from `20be194e` (drive fixes 5), worktree `D:/foxsdr-wt/en-screen-j`, build `D:/foxsdr-build/en-screen-j` (Release, `--parallel 4`, `ctest -j1`), hidden desktop `FoxSDRDriveFix6`, scratch `D:/foxsdr-build/df6-scratch/`. Nothing pushed. Source: `D:/foxsdr-build/drive-final/handover-recheck.txt` (A1, A7, A18).

An item 5 (mode-only presets) was sent during the work and withdrawn by the orchestrator before it was committed; its uncommitted edits were restored to the last commit and the new header deleted. Nothing of it is in this branch.

| # | Reproduced | Cause (file) | Verdict | RED before |
|---|---|---|---|---|
| 1 Tab order | YES, in process: a bank field ("Bookmark name") has the keyboard, the search box is pressed, Escape, Tab: `focus.place` stayed empty for nine Tabs and never reached the figures (`RED-tab-base.log`: 3 steps FAILED). The group 2 drive starts with no focus, so it passed | `ui/palette.slint` CommandField: the press did not take the keyboard, so the sheet (a popup) gave it back to the previous control when it closed, and Tab walked on from there (side panel rows, spectrum toolbar) | FIXED: the press calls `field-focus.focus()` before the sheet opens; Tab then goes search, Layouts, bell, minimise, maximise, close, STOP, lock, the figures, the mode (a picker below 1700 px, eight keys above), squelch slider, squelch AUTO, VOLUME, then the bank. GAIN and BIAS TEE are disabled on the generator and are out of the ring by design. The window keys sit between the search box and STOP because the tree order puts them there; not changed | drives `drive6-tab-chain-1024/1366/1920`: 5 checks, 0 failed each after; RED on the base build as above |
| 2 Height at 150 % | YES by the numbers of the test: a window centred on the screen keeps its top-left while it grows (1625 x 975 at y 130 on a 1040 high work area, bottom 1105) | `look_logic.cpp fitWindowToScreen` bounded only the SIZE; nothing moved the window | FIXED: `placeInWorkArea` and `windowWorkRect` (`gaps_win.cpp`), applied in `main.cpp` after the grow | `test_slint_host_look` with `placeInWorkArea` a no-op: 21 "off the screen" lines and FAILs at line 257 |
| 3 Legacy Decoder output wakes decoders | YES by reading and test: `app_window_plugin_use.cpp` sent bit 8 for every text or I/Q decoder | the old block | FIXED. What stops working: opening the Legacy Decoder output window (or a preset press on a text decoder with no window of its own, which opens it) no longer starts idle decoders; START, KEEP RUNNING or the plugin's own window do | `test_plugin_window` lines 627, 628, 636 with the old block restored |
| 4 No key to remove a radio | NOT REPRODUCED as "no key": in the 2 x 2 workspace with four radios the pane headers of R2, R3 and R4 each end with a 24 x 24 px X key, accessible label and tooltip "Remove Radio n" (dump: at 1568,205; 945,543; 1568,543 on a 1600 x 900 window; picture `rm-2x2.png` of the probe run). R1 has none (RADIO_REMOVE refuses it). It exists ONLY there: no menu on a radio selector key or a status bar chip (their right-click only offers STOP Rn), no keyboard chord, and nothing in the One layout's header of the other radios (they have no pane there) | `ui/radio_pane.slint:297` (`removable`, `workspace_publish.cpp:135`) | REPORTED, not redesigned: it is a small icon-only ghost key at the far right of a pane header, easy to miss; a running or recording radio asks first ("Remove Radio n?") | n/a |

## Changed expectations
- `test_plugin_window` testUseReport: with the Decoder output window open the table was g13 + g14 + the text decoder with bit 8; now g13 + g14 only. The radar and map-page line loses its text-decoder row the same way.
- `test_slint_host_look`: the window-size test ran for one screen (1920 x 1040); it now runs for five work areas and also checks the window's edges. No old value changed.

## Strings
None new, none changed: `test_i18n` and `test_cjk_wrap` pass (no catalogue touched).

## Tests run (final tip, hidden desktop, `-j1`)
`test_slint_host_look`, `test_cjk_wrap`, `test_decoders_mute_grants`, `test_i18n`, `test_plugin_window`, `test_error_paths` all Passed (error-path pin 410, not touched). Drives `drive6-tab-chain-1024`, `-1366`, `-1920`: 5 checks, 0 failed each. NOT RUN: the full headless and GUI gates, the older drives (`drive2-tab-1366` was not re-run after the probe change; the probe only writes a property), the Legacy window's drives.

## UNVERIFIED
A real keyboard on the real desktop (the drive uses the window's own key path); the fix of item 2 against a real second monitor and a real taskbar (stand-in work areas); item 4 in the One layout.
