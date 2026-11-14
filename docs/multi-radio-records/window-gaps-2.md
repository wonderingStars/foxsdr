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

## Gates
(filled after the runs, below)

## UNVERIFIED
* The real SDRplay probe on an RSP, the real service restart and its UAC prompt, the real file chooser behind Browse..., a real OS file drag, the real clipboard: stand-ins only, until the mouse drive.
* What the Vivid and Mono band colours look like on the screen (a bounded run has no band in view); the rule is unit-tested.
* The "Saving" label while a real take is being closed (too short to catch from a script); the label is unit-tested.
* The pointer ledger's OS cursor numbers on a real desktop (the hidden desktop will not give the cursor).
* Bench Classic XL at the default 1600 px window: the doubled counter squeezes the gain and squelch lanes of the strip (seen in a screenshot); not judged by an owner.
* All new translations; a speaker has not read them.
