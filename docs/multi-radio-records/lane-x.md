# Lane X (wave 8): in-app help, the DSP speed test, three open rows of the merge, knob input, the web page on touch, the patch page's last rows

Branch `feat/lane-x` from `d4f855c5`, worktree `D:/foxsdr-wt/en-webmodels`, build `D:/foxsdr-build/en-webmodels`, scratch `D:/foxsdr-build/x-scratch/`. Nothing pushed. Closed at the last commit of the branch (see git log).

## Rows

| Row | Verdict | What and proof |
|---|---|---|
| G5-14 in-app help | BUILT | `lanex_help.{hpp,cpp}` (28 topics written in the program; no network, no file), `lanex_publish.cpp`, `ui/lane_x.slint` (one mount line in `main.slint`). App menu "Help topics" (id 610) and Ctrl+F1 open it at the topic of the section last opened; searchable list; Esc and Close shut it. Every sentence is either a control's own tooltip copied letter for letter (marked `T` in `lanex_help.cpp`) or a sentence checked against the code named beside it (`C`). Left out because not verifiable here: Radar, Target details, Maps, Transmit, the Scanner's priority and threshold fields (the manual says the scanner does not use them; another lane is changing the scanner), Updates, Serial ports, Language, the Settings and backup section. Tests: `test_slint_lanex_host` (table rules: unique ids and sections, no forum / chat / "beta" / "later" words, search, context), drive `lanex-help`. |
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

## Proof, in one place
| Row | Logic tests | Drive (hidden desktop `FoxSDRTests`, real engine, test generator) | Break-it |
|---|---|---|---|
| G5-14 help | `test_slint_lanex_host` 485 checks | `lanex-help` (opens at the topic of the open section, list, search by title / sentence / key word / nothing, a topic, Esc after a search, Ctrl+F1, Close) | the first run of the test went RED for the intended reason (`topicForSection(-1,-1)` matched the topics tied to no section); fixed, green |
| G5-19 speed test | `test_slint_lanex_host` (job: full run, cancel between items, second start refused, throwing item, zero items, destroy mid-run) | `lanex-bench` (21 items done, 0 failed; a second run cancelled at once; a third completes) | cancel check removed: `test_slint_lanex_host` RED (3 checks), restored green |
| P1 panes | the pane tests build with the new fields | `lanex-panes-airband`, `lanex-panes-gap` (read the PUBLISHED row) | NOT break-it checked (a drive-level break needs another full product build) |
| PF-1 | `test_slint_host_signal` | none (a drive cannot make a patch hold the saved radio without opening one) | rule forced off: RED at three checks, restored green |
| WG2-1492 recorder + Preview | `test_slint_tester_host` 27 checks | none (a drive would need a tester code and the preview row, which exists only while the tester feature is on) | browser mark removed: RED, restored green |
| G5-16 knobs | `test_slint_knob_host` 43 checks; the merged layer's five `test_input_*` (386 checks) | `lanex-knobs` (stand-in controllers: nothing open until the switch and the device; the jog is three steps of 10 kHz read back as `tunedHz`; button 4 is mute read back as the receiver's muted flag; switch off closes and the knob does nothing) | transmit refusal inside a command removed: RED, restored green |
| G5-17 touch | `test_web_touch` (headless browser at 390 x 844 and 1100 x 800, real `Input.dispatchTouchEvent`) | the same | the OLD page: RED (no digit control, divider and grip do not move, 12 small targets, divider 7 px); the new page green |

## Gates (build `D:/foxsdr-build/en-webmodels`, Release, hidden desktop, `-j3`)
* Headless `-L "headless|slint"` (1174 tests) at `438cd130`: `99% tests passed, 6 tests failed out of 1174`, `Total Test time (real) = 2377.07 sec`. Reds: `test_slint_house_rules` (MINE: an untranslated literal in lane_x.slint; fixed), `test_json_dump_policy` (MINE and the merged knob layer: three dumps without the error handler; fixed), `test_thread_inventory` (MINE and the merged knob layer: three thread sites; rows added), `test_health_app_stepped` (load flake: passed alone in 44.65 s and again in 27.41 s), `test_slint_tools` (NOT MINE: it renders `tools_specimen.slint` and the Markers sheet; none of tools_bank.slint, the specimens or the test is in this branch's diff; it fails alone on the sheet's geometry; UNVERIFIED on a base build), `telemetry_worker` (NOT MINE: the node test "the fixture directory matches its manifest"; `telemetry-worker/` is not in the branch's diff; UNVERIFIED on a base build).
* The reds that were mine, re-run after the fixes: `test_slint_house_rules`, `test_json_dump_policy`, `test_thread_inventory`, `test_health_app_stepped` pass.
* GUI `-L gui` (390 tests) at `438cd130`: `99% tests passed, 4 tests failed out of 390`, `Total Test time (real) = 2814.13 sec`. Reds: `slint_drive_lanex-help` (MINE: Esc and the focus; fixed in two commits, `Passed 31.72 sec` at the tip), `test_diagnostics` (the stamp: an uncommitted tree), `slint_drive_layout-section-overlap-1600` and `-624` (NOT MINE by inspection: the CW surround's last key sits 30 px lower than the test's number, `"Read the pitch now" bottom <= 2028 -> it reads 2058`; the SIGNAL bank's bodies are not in this branch's diff; they fail alone; UNVERIFIED on a base build).
* `test_error_paths` passes: the PENDING pin is 1126 before and 1126 after (14 ALLOW lines added, each with its reason; nothing new is PENDING).
* `test_i18n` passes (68 keys in 33 catalogues, 0 missing, 0 stale, 0 empty).
* Real `%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, unchanged. Nothing pushed, no download, no outside request, no radio and no MIDI device opened, nothing keyed, nothing on the visible desktop, `FOXSDR_ALLOW_*` never set. The browser was the installed Edge, headless, ended by its own PID.

## UNVERIFIED
* A real phone and a real touch screen: only emulated touch in a headless browser. No real ShuttleXpress or PowerMate was ever read; the button order and the report layout come from the knob lane's record (itself unverified on hardware).
* The base build of this tree was not completed: it was stopped while compiling the generated main file, because the first change to `main.slint` made that compile moot. The reds called "not mine" were not run on a base build.
* The Help topics' sentences: each checked against the code when written; none read by a second person. The 68 translations are machine-made and unchecked.
* Frame cost of the new overlays and the clip sweep: not measured. The tester Preview was not driven in the window (no tester code is set in a drive).

## OPEN ROWS FOR THE NEXT LANE
| Row | What is left | Kind of lane |
|---|---|---|
| G5-29, G5-30 | Pluto and rtl_tcp radios on the patch page and a gain slider per patch radio: engine work in `engine_patch_radios.cpp` (a node setting for the address, a per-radio gain through the patch graph and its commands), receive only | engine, then screen |
| G5-16 MIDI | a MIDI controller needs a learn step (which control is the knob) and a mapping page; the layer parses MIDI already | screen with an engine duty |
| G5-16 hardware | no real ShuttleXpress or PowerMate read: the report layout and the button order are unverified | mouse drive / owner |
| WG2-1492 | the tester sender, the retry queue and the journal in the Slint host (the radio-reports lane builds the queue, sender and journal; connect `tester_host.hpp` to them after that lane is merged) | engine, after radio-reports |
| G5-14 | topics left out because unverifiable here: Radar, Target details, Maps, Transmit, the Scanner's priority fields, Updates, Serial ports, Language, Settings and backup, the Patch page's own controls | screen |
| G5-14 | App menu "Help: the manual" still opens the GitHub page; the App menu's labels are not translated (as before) | screen |
| G5-17 | pinch-zoom on the page's spectrum and waterfall (it has none); a real phone | web + mouse drive |
| P1 | frame cost of the panes' new fields; a drive-level break-it | screen |
| G4-110, G5-02, G5-04, G5-11, G5-37 | not this lane's (carried unchanged); G5-18 web JavaScript plug-ins: nothing built, the questions are above | owner / Linux lane |
| four reds | `test_slint_tools`, `telemetry_worker`, `slint_drive_layout-section-overlap-1600` and `-624`: run them on `d4f855c5` to confirm they were red there | merge lane |
