# Window gaps, lane 2: the SIGNAL and SYSTEM pages and the display settings (2026-11-14)

Branch `feat/window-gaps-2`, cut from `07e717e6` in worktree `D:/foxsdr-wt/en-tuning` (it was clean; its old branch `feat/wire-a` is merged). Build folder `D:/foxsdr-build/en-tuning`
(configured here with the Slint window ON, it had been OFF), scratch `D:/foxsdr-build/wg2-scratch/`, hidden desktop `FoxSDRGaps2`. Nothing is pushed or merged. The real
`%APPDATA%/foxsdr/config.json` sha256 at the start and at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`.

The read-only audit (`not-built-audit.md`) was used as a hint only; every row was checked in the code first. The ledger rows were stale for about half of this lane.

## Rows

| Row | Control | State | Proof |
|---|---|---|---|
| 35, 36 | Enlarge figures, Normal size (counter menu) | BUILT | `Theme.counter-scale` (theme.slint) scales digit width and height, the figure and dot fonts and the strip height; main.cpp applies the saved `counterScale` once changed. `system-look.txt`: `gaps.counterscale == 2`, then `1` |
| 40, 158, 1396 | Enlarge every reading | BUILT | `Theme.reading-scale` scales the peak readout, the slider readouts, the tuned-frequency text and the level text; `system-look.txt`: `gaps.readingscale == 14`, then `10` |
| 156, 1394 | Counter figures | BUILT | the same scale; `system-look.txt` |
| 148 | Bench Classic XL | BUILT | Workbench palette, figures doubled, tuner switches put away (`showNudges` follows the saved `counterSwitches`), readings 1.4x; `system-look.txt` |
| 150, 151, 152 | Glass Cockpit, Daylight Lab, Field Radio | BUILT | three more `Skin` fillings in theme.slint (the classic presets' colours mapped onto the tokens the way Night Watch's were; the tokens with no classic role are mixes), `skinOfTheme` names them; `test_slint_themes_four` (every token present, reading pairs at 4.5:1), `system-look.txt` reads `look.bg` and `look.accent` of each; screenshots of Field Radio and Bench Classic XL looked at |
| 145 | Ctrl+= and Ctrl+- | BUILT | step the interface size through 100 to 250 % from the size in force (`stepUiSizePercent`, unit-tested), alongside the key table's zoom on the same chord as the classic does on the same press; `wg2-size-keys.txt` |
| 258, 269, 270 | Band plan palette, Size, Colour | BUILT | `bandplan_palette.cpp` quotes the classic's Vivid and Mono tables by service class; the ribbon takes each band's colour and the saved Size sets the ribbon tier (small 8, medium 13, large 20 px); `test_slint_host_bandpalette`, `wg2-bandplan.txt` reads `gaps.ribbonsize` 3, 2, 1 |
| 271, 274, 275 | Plan status text, On the spectrum, Stack close names | ALREADY BUILT | system_logic.cpp:294 and system_bank.slint; tools_bank.slint:1431 and :1440, tools_publish.cpp saveBookmarkOptions, main.cpp bookmarkMarkers / bookmarkNameRows; `tools-bookmarks-options.txt` |
| 50, 51, 52 to 55, 162 | Meter menu, hover hint, four styles, style per meter | BUILT | the two bench meters (SAMPLE RATE, VOLUME) now stand in SYSTEM > Display (`ui/meters_panel.slint`) with the classic's right-click menu and its hint; each style saved (`meterStyleRate`, `meterStyleVolume`); `wg2-meters.txt` (a pick through a seam: a popup's rows cannot be reached by label) |
| 1281 | LO quick-picks | ALREADY BUILT | keys read 125 / 100 / 2 MHz in the order of `kConverterQuickLoHz` (signal_bank.slint, signal_logic.cpp:1586); `signal-source-converter.txt` |
| 1292 to 1294 | RESTART SDRPLAY SERVICE, progress, outcome | ALREADY BUILT | signal_bank.slint sdrplay-key-shown, signal_publish.cpp sdrPlayRestartPressed; `gaps-sdrplay.txt` (stand-in) |
| 1317 | De-emph | ALREADY BUILT | signal_bank.slint deemph-modes, opDeemph; `gaps-deemph.txt` |
| 1318 | Signal strength | BUILT | the S-meter bar in the Radio section over -120 to 0 dB, "-61.3 dB"; `test_slint_host_signal`, `wg2-signal.txt` |
| 1369, 1374 | Saving (I/Q, audio) | BUILT | new engine fact `recorder.finishing`; the key reads "Saving IQ..." / "Saving audio..." and is dead until the file is closed; `test_slint_host_signal`. The classic window itself has no such state on those keys |
| 1383 to 1385, 1647, 1651 to 1653 | Import: drop zone, Browse..., reading line, Bookmarks drop | ALREADY BUILT (Browse hardened) | drop: gaps_win.cpp installFileDrop, main.cpp handleDrop, `gaps-drop.txt`; reading line: signal_logic.cpp:1359 into the dialog's note; Browse: a bounded run never raises the Windows file chooser (FOXSDR_FILE_PICK stands in), `wg2-import.txt` |
| 1457 | Pointer ledger tick | BUILT | one line across the top of the window, no clicks taken; `wg2-diagnostics.txt` |
| 1458 | Copy diagnostics | ALREADY BUILT, MOVED | `diag_copy.cpp` is the ONE function (see HAND-OFF); `gaps-misc.txt` |
| 1460, 1461, 1464 to 1470, 4614 | SDRplay diagnostic and its bias-tee question | BUILT | `sdrplaydiag_logic.cpp`, `ui/diag_overlays.slint`; `test_slint_host_sdrplaydiag` (4089 checks including a 4000-step random walk: the probe is never started with the bias tee on unless the last answer was yes), `wg2-diagnostics.txt` runs the dialog with a stand-in child |
| 1492 | Tester Preview | NOT DONE | the Slint host has no beta-tester usage recorder, sender or journal; the preview would need the recorder (marks for features, plugins, radios from the engine's facts), the sender and the journal in `HostDuties` (the classic: gui/app_window_tester.cpp, about 450 lines). Preview-only would show a record nothing ever sends |

REPLACED ON PURPOSE: none. NEEDS DATA: none.

Decisions made: Ctrl+= does both the table zoom and the size step, as the classic does; Field Radio's counter ground is the dark well, not the classic's light green LCD, because the same token grounds the text fields
(light ink on a light ground would have failed the 4.5:1 test); the volume meter reads the engine's audio level, not the classic's post-volume peak of the scope tap.

## New engine commands and facts
`fact.recorder.finishing` (engine_api_lists.cpp): two characters, I/Q then audio, `1` while that take's file is being closed (from `Engine::takeFinishing`). No new command.

## SHARED FILE EDITS
`ui/main.slint` (imports, exports, the two overlays, the three new skins in the import and export lists), `ui/signal_bank.slint` (five components and DropRow made `export`, the Signal strength row and its model fields, one import), `ui/system_bank.slint` (`MetersBlock {}` and its import), `ui/theme.slint` (three skins, `counter-scale`, `reading-scale`), `ui/sliders.slint` and main.slint (reading-scale on three readouts), `main.cpp` (look, scale, ribbon, key answer, state readers), `look_logic`, `system_publish`, `signal_logic` / `signal_publish`, `workspace_publish.cpp` and `radio_view.hpp` (the band colour), `input_script.cpp` (the scripted key `ctrl+=`, `ctrl+-`), `engine/engine_api_lists.cpp` (the fact), `resources/lang/*.json`, `src/core/lang_assets.hpp`, the CMake lists. No section height or spacing was changed; the Radio section gained one 24 px row.

## New strings (machine-made in 33 languages, UNCHECKED by a speaker)
"Saving IQ...", "Saving audio...", "Signal strength". Every other string (the SDRplay dialog, the meters, the ledger's help) was already in the catalogues.

## HAND-OFF
* Lane 1 (the "FoxSDR did not close normally" dialog): call `foxsdr::slintdiag::copyDiagnostics(DiagCopyInput)` from `src/slint/diag_copy.hpp`; it builds the bundle, puts it on the clipboard and saves `diagnostics.txt` beside the reports. Fill `reportsPath`, `lastRunUnclean`, `launches` and `crashes` as `SystemPublisher::copyDiagnostics` does. If both branches have a copy when they meet, keep this one and delete the other.
* Row 1492: see the table.
* The classic's "Saving" state for I/Q and audio keys does not exist; ours is new.

## Resumed on Monday 2026-11-16 (second agent)

* Tree at `2b867906` was clean; full rebuild at the tip exit 0 (no compile error: the `openDialog` rename was complete). `test_slint_host_sdrplaydiag` Passed, `test_plugin_containment` Passed (24.71 sec).
* `test_engine_api_source_kinds` and `test_engine_api_decode_radio` are NOT environment reds once their fixtures exist: the `.wav` files are gitignored and made by the repository's own `src/slint/tests/drive/fixtures/make_*.py` (standard library only; the drive runner `test_slint_drive_run` makes a missing one itself, the two engine tests do not). Ran `python -I make_<name>.py` for all the generators in this worktree's fixtures folder (25 `.wav` files; nothing downloaded; `git status` still clean because the output is gitignored). Both then Passed in the same run (source_kinds 18.31 sec, decode_radio 36.46 sec). Anyone who sees them red in a fresh worktree runs the generators first.
* Bench Classic XL at 1600 px: see the next section.

## Bench Classic XL at 1600 px: measured, a real defect, fixed (commit 60bf4e68)

Measured on the hidden desktop `FoxSDRGaps2` at 1600 x 900 with the `dump` verb (positions, logical px), Today's bench against Bench Classic XL:

| | Today's bench | XL before | XL after |
|---|---|---|---|
| counter | 397 x 84 | 776 x 136 | 413 x 86 (1.04 times) |
| Gain slider | 163 | 91 | 153 |
| Squelch slider | 163 | 91 | 153 |
| Volume slider ends at | 1435 | 1591 (past the 1584 padding) | 1435 |
| Antenna power (bias tee) key | at 1464 to 1584 | not in the window | at 1464 to 1584 |

Before the fix it was NOT only tighter: the three lanes had fallen to their 396 px minimum, the Gain and Squelch headers (caption, reading, AUTO key) overlapped ("N/A" over "AUTO", the SQUELCH caption gone) and the antenna key was clipped out of the window (the strip clips). At 2000 px it is the same (82 px short): the doubled figures need about 2200 px.
Fix: `foxsdr::slintlook::fitCounterScale` (look_logic.cpp): the scale DRAWN is the one asked (1 or 2) less what would leave the three lanes under 525 px (150 more per whole step of the readings scale); measured lane widths: width - 1000 px below 1760 px, width - 1307 px from it. The saved choice is untouched. `gaps.counterscale` still reads the asked value, new `gaps.counterfit` reads the drawn one in per cent. At 1600 px XL therefore draws 1.04 times; the full doubling appears from about 2200 px. After the fix the SQUELCH caption still elides to "S..." at 1600 because of the 1.4 times readings (a caption elision; nothing overlaps or leaves the window; seen in a screenshot).
Proof: `test_slint_host_look` 630 checks, 0 failed (new `counterFit`); new drive `wg2-xl-fit.txt` Passed (11.48 sec); BREAK-IT: `fitCounterScale` returning the asked value turned it RED (`counterfit` 200, Gain 90.67 px wide, the antenna key not in the window), restored, GREEN.
Owner decision it leaves: the product has XL at about normal size on a 1600 px window; if the doubled figures must show there, the strip needs another arrangement (the antenna key and a lane on a second row), which is a design call. UNVERIFIED: a strip with more than one radio (the radio block widens the strip from 1760 px), languages with longer words.

## Gates (resumed run, hidden desktop `FoxSDRGaps2`, `FOXSDR_FRAME_CAP_HZ=60`, machine shared)
* Headless + slint gate at `2b867906` (`ctest -L "headless|slint" -j3 --timeout 300`): `99% tests passed, 10 tests failed out of 1151`, 2491.79 sec. Reds, classified:
  * Known false failures: test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode.
  * Load, passed alone: test_slint_fitted (13.52 sec), test_engine_api_plugin_smoke_radio (109.14 sec; timed out at 119 s in the gate), test_plugin_unload_handshake (3.33 sec), test_pass_control (passed on the second re-run, 59.56 sec; the first re-run failed its 2 ms floor check at 1.881 ms under load).
  * test_health_app_stepped: failed in the gate and twice alone (`tests/test_health_app.cpp:1167 bindOrAbsentCount(onDisk, 1)`): the ledger it reads carried an extra `slow.startup.250ms=1` (the application's start took over 250 ms on this loaded machine), which the test's `withoutStagesOrMachine` does not strip. Neither the test nor `src/core/health*` is touched by this branch (`git diff 07e717e6 -- tests/test_health_app.cpp src/core/health*` is empty). Classified as a load flake; NOT proven by a quiet-machine pass: UNVERIFIED.
* After the XL fix (`main.cpp`, `look_logic`, `test_slint_host_look`, `wg2-xl-fit.txt`): `ctest -L slint -j3`: `100% tests passed out of 96`.
* GUI gate `ctest -L gui -j3 --timeout 400` at 60bf4e68: `99% tests passed, 3 tests failed out of 351`, 2992.78 sec. Reds:
  * `slint_drive_transmit-page`: MINE, FIXED (commit bec1fbb1). The Signal strength bar (24 px plus 6 spacing in the always-open Radio section) pushed the Transmit header, already at the very foot of the list since engine-merge-5, out of the tree (`click "Transmit" -> no control labelled "Transmit"`). The script now wheels the list (`scroll "Audio filters" 400`) before the click; no assertion changed. Alone: Passed (15.67 sec).
  * `slint_drive_system-look`: `look.width == 1600 -> FAILED: it is 800` at the line after "Interface size: Auto" (the first agent recorded the same line as a timing flake). With the SAME binary it passed 3 of 7 alone-runs (one of the failures had the wait lengthened to 90 frames), so it is a race, not a fixed wait: the window's size stays 800 after the pinned 200 % goes back to Auto although look.scale already reads 1.00. The script is left as committed. Cause NOT found and not shown to predate this branch (no baseline build was run): UNVERIFIED; listed below for a screen lane.
  * `test_diagnostics`: `tests/test_diagnostics.cpp:941 gitCommit() == expect`: the build stamp is older than the commits made after the build (the known stamp family); see the stamp line below.
* Drive coverage ledger (`slint_drive_ledger` Passed): the new script `wg2-xl-fit` is not a ledger row (rows 35 and 148 stay ASSERTED by system-look).
* Real `%APPDATA%/foxsdr/config.json` sha256 after the resumed run: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

## UNVERIFIED
* The real SDRplay probe on an RSP, the real service restart and its UAC prompt, the real file chooser behind Browse..., a real OS file drag, the real clipboard: stand-ins only, until the mouse drive.
* What the Vivid and Mono band colours look like on the screen (a bounded run has no band in view); the rule is unit-tested.
* The "Saving" label while a real take is being closed (too short to catch from a script); the label is unit-tested.
* The pointer ledger's OS cursor numbers on a real desktop (the hidden desktop will not give the cursor).
* Bench Classic XL at 1600 px: measured and fixed (see its section); a strip with several radios and longer-worded languages are not re-measured.
* All new translations; a speaker has not read them.

## OPEN ROWS FOR THE NEXT LANE

| Row | What is missing | Kind of lane |
|---|---|---|
| 1492 (tester Preview box) | NOT DONE on purpose: the Slint host has no beta-tester usage recorder, sender or journal. It needs the recorder (marks for features, plugins and radios from the engine's facts), the sender and the journal in `HostDuties` (classic: `gui/app_window_tester.cpp`, about 450 lines); a preview alone would show a record nothing ever sends | screen (with an engine/host duty) |
| 1292 to 1294, 1460 to 1470, 4614 (SDRplay diagnostic, RESTART SDRPLAY SERVICE) | the real probe on an RSP, the real service restart and its UAC prompt exist only behind stand-ins | mouse drive |
| 1383 to 1385, 1647, 1651 to 1653 (import) | the real Windows file chooser behind Browse... and a real OS file drag | mouse drive |
| 1457, 1458 (pointer ledger, Copy diagnostics) | OS cursor numbers on a real desktop; the real clipboard | mouse drive |
| 258, 269, 270 (band-plan Vivid and Mono) | what the colours look like on screen with a band in view | mouse drive |
| 1369, 1374 (Saving state) | the label while a real take is closed (too short to catch from a script) | mouse drive |
| 148, 35 (Bench Classic XL) | owner decision: XL draws about normal size on a 1600 px window (doubling needs about 2200 px); a second strip row for the antenna key and a lane would allow it. Not measured: a strip with several radios, longer-worded languages | owner decision |
| Transmit header at the foot of the SIGNAL list | still not visible without wheeling the list at 1600 x 900 (engine-merge-5's residual, now 30 px further down): Sinks closed by default or a shorter Source body | owner decision |
| `slint_drive_system-look`, the line `state look.width == 1600` | intermittent: the window's size stays at the pinned 200 % value after Interface size goes back to Auto; cause not found | screen |
| `test_health_app_stepped` | fails under load because `slow.startup.250ms` is counted in the ledger the test compares; the test should strip `slow.` counts or the threshold be loosened; not touched here | engine |
| New translations ("Saving IQ...", "Saving audio...", "Signal strength") | machine-made, unread by a speaker | owner (translation pass) |
| Lane 1 hand-off | `foxsdr::slintdiag::copyDiagnostics` must be the only copy when the branches meet (keep this one) | merge |
