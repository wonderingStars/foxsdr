# Lane X (wave 8): in-app help, the DSP speed test, three open rows of the merge, knob input, the web page on touch, the patch page's last rows

Branch `feat/lane-x` from `d4f855c5`, worktree `D:/foxsdr-wt/en-webmodels`, build `D:/foxsdr-build/en-webmodels`, scratch `D:/foxsdr-build/x-scratch/`. Nothing pushed. Written while the lane ran; the RESULTS section is filled at the end.

## Rows

| Row | Verdict | What and proof |
|---|---|---|
| G5-14 in-app help | BUILT | `lanex_help.{hpp,cpp}` (32 topics written in the program; no network, no file), `lanex_publish.cpp`, `ui/lane_x.slint` (one mount line in `main.slint`). App menu "Help topics" (id 610) and Ctrl+F1 open it at the topic of the section last opened; searchable list; Esc and Close shut it. Every sentence is either a control's own tooltip copied letter for letter (marked `T` in `lanex_help.cpp`) or a sentence checked against the code named beside it (`C`). Left out because not verifiable here: Radar, Target details, Maps, Transmit, the Scanner's priority and threshold fields (the manual says the scanner does not use them; another lane is changing the scanner), Updates, Serial ports, Language, the Settings and backup section. Tests: `test_slint_lanex_host` (table rules: unique ids and sections, no forum / chat / "beta" / "later" words, search, context), drive `lanex-help`. |
| G5-19 DSP speed test | BUILT | `core/dsp_benchmark.hpp` gained `benchItemCount` / `runBenchItem` (the list is unchanged; `runDspBenchmark` loops them). `lanex_bench.{hpp,cpp}`: one worker thread, progress, cancel between items, exceptions become failed items. Rows under SYSTEM > Performance (`LaneX.rows-bench`). The publisher has no way to send a command: it changes no setting. Tests: `test_slint_lanex_host` (job), drive `lanex-bench`. |
| P1 panes | BUILT | `radio_pane.slint` `RadioPaneData` carries `airband-channels`, `airband-block`, `airband-hearing`, `wf-gap-*`, `wf-rail-*`; `workspace_publish.cpp` fills them from each radio's own figures; the tuned band and the passband peak hide on the radio the monitor listens on. Drives `lanex-panes-airband`, `lanex-panes-gap` read the PUBLISHED row (`state pane.r<n>.*`). |
| PF-1 | BUILT | `signal_logic.cpp` `savedRadioHeldByPatch`; label "(held by the patch)" and one sentence. Tests in `test_slint_host_signal`. |
| WG2-1492 tester Preview | PART BUILT | `tester_host.{hpp,cpp}`: the tester usage recorder of the Slint host (same marks as the classic window) and the Preview box fed from it (`SystemPublisher::setTesterPreviewSource`). Nothing is sent. Test `test_slint_tester_host`. The sender, retry queue and journal are NOT built (the radio-reports lane's): OPEN ROW. |
| G5-16 knobs | BUILT for HID (ShuttleXpress, PowerMate) | branch `feat/knob-input` merged (`6908d6e3`, no conflicts). `knob_host.{hpp,cpp,win.cpp}` consumes it; rows under SYSTEM > Accessibility; the saved choice in `lanex.json`. MIDI controllers NOT built (need a learn step): OPEN ROW. Test `test_slint_knob_host`, drive `lanex-knobs` (stand-in controllers). |
| G5-17 web on touch | BUILT | see below. `test_web_touch` (headless browser, real touch events). A real phone: UNVERIFIED. |
| G5-29, G5-30 | NOT BUILT | engine surgery in `engine_patch_radios.cpp` (a node setting for the Pluto / rtl_tcp address and a per-radio gain through the patch graph and its commands) and cannot be proved without opening a radio or a network radio. OPEN ROW. |

## RULED OUT / NOT YOURS (carried unchanged)
G4-110 Pluto transmit controls (owner decision 54), G5-02 Android, G5-11 Linux updater, G5-04 AppImage, G5-37 arm64: not touched. G5-18 web JavaScript plug-ins: nothing built. What exists: the page is served with `default-src 'none'` and no inline script (`web_server.cpp`, tested). The questions: who may add a script; whether a script may call `/api/control` (it would then hold every control the session holds, including the transmit key route); how a script is signed and revoked; whether it may read the audio stream; what a script is allowed to send out (the page's CSP forbids it, a plug-in would need a hole in it).

## G5-17 what was found and changed (`src/net/web_server.cpp`)
Found by reading: the two dividers and the column grip listened to `mousedown`/`mousemove` only (a finger drag fires neither); the frequency digits tuned by the wheel only (no way to tune a digit on a phone); targets were mouse-sized. Changed: pointer events with capture for the dividers and the grip (and `touch-action:none`), tap (upper or lower half) and swipe on a digit (`attachDigitTouch`, its own string piece because the old piece hit MSVC's 16380 byte limit), `@media (pointer: coarse)` rules for 44 px targets and wider dividers. The spectrum already tuned by `click` (a tap sends it) and the map and volume knob already used pointer events.

## New engine commands
None.

## SHARED FILE EDITS
`src/slint/ui/main.slint` (import, export, three mounts); `src/slint/ui/radio_pane.slint` (P1 fields); `src/slint/main.cpp` (publisher, knob host, tester recorder, menu item 610, section memory, `lx.` and `pane.` readbacks); `src/slint/CMakeLists.txt`, `src/slint/tests/CMakeLists.txt`, `tests/CMakeLists.txt`; `src/slint/signal_logic.{hpp,cpp}` (PF-1 only); `src/slint/key_dispatch.{hpp,cpp}` (Ctrl+F1); `src/slint/host_logic.{hpp,cpp}` (`fmtNumber`); `src/slint/system_publish.{hpp,cpp}` (the tester preview source); `src/slint/workspace_publish.{hpp,cpp}` (P1, `publishedRow`); `src/core/dsp_benchmark.hpp`; `src/net/web_server.cpp`; the four pane tests and two measure programs (the new pane fields' defaults).

## New strings
68 keys in all 33 catalogues, machine-made by four Sonnet agents from the same key list and the existing catalogues' own vocabulary, placeholders and newline counts checked by script; unchecked by a speaker. Agents' own doubts: "noise blanker" and "Quit" have no catalogue term; some languages repeat one phrase for "Key bindings" and "Keyboard shortcuts"; Greek maps OFF and ON to words that read as closed and open; a plural of "million samples" is not handled in pl and ro (a number of 1 would read wrongly).
