# Slint window gaps: de-emphasis, digit grouping, frequency lock, Saving state, trail keys, counter menu, BROWSE FILES, file drop, SDRplay, help, high contrast (2026-11-11)

Branch `feat/slint-gaps`, cut from `mr/screen-s6` at 9d907b68. Worktree `C:\Users\steve\foxsdr-wt\feat-gaps`, build `D:/foxsdr-build/feat-gaps`. Nothing is pushed or merged. Records read first: `screen-s6.md` (commands, gates, drive language, ledger, strings) and the survey `D:/foxsdr-build/prep/second-feature-list-survey.md` (section 4, hypotheses; every file:line checked below).

COMMITS: 6297ef86 (items 1 to 10, code, strings, unit tests, first drive scripts), dd996356 (drive scripts for items 4 to 10, seams, the lock set widened), 1bf125b7 (high contrast, ledger rows, COMPONENTS.md, two more strings), then the layout and test repairs and this record (see git log).

## STEP 0: every item re-checked against the classic window

| # | Item | Classic (file:line) | Slint window BEFORE (file:line) | Status | What was built |
|---|---|---|---|---|---|
| 1 | FM de-emphasis | `app_window.cpp:272` (kDeemphLabels), `:6676-6684` (combo "De-emph", greyed outside NFM/WFM, SET_DEEMPHASIS) | only a state read for the drive harness (`main.cpp` state `deemphasis`), no control | ABSENT | De-emph `DropKey` in the Audio filters section (`signal_bank.slint`, `signal_logic.cpp` deemphModes/opDeemph) |
| 2 | Dotted digits | the classic counter has no dots; the Slint counter drew 1.090.000.000 (`digits.slint` dots after digits 1, 4, 7) | typing accepted one dot only (`host_logic.cpp parseFrequencyHz`) | PARTLY (style 0 present) | option in SYSTEM > Display, style 1 = MHz.kHz.Hz (1090.000.000), saved in the window half; typing reads both styles |
| 3 | Frequency lock | none in the classic (grep: no lock of tuning anywhere) | none | ABSENT (new feature) | padlock key by the counter, per radio, saved; one choke point (the API table the publishers hold) |
| 4 | "Saving" state | the classic's "Saving..." is on the PICTURE save (`app_window_disk_work.cpp:149-197`, a worker, key greyed) and on the bookmark/preset export (`app_window_airband.cpp:743`). There is NO such state on I/Q or audio recordings: `Recorder::stop` finalises synchronously and the engine's recording list has only ACTIVE / STALLED / FULL (`foxsdr_api.h` FOXAPI_LIST_RECORDINGS) | exports: present (`tools_publish.cpp:558`). Picture save: SYNCHRONOUS on the window's thread, no state (`plugin_kit.cpp savePicture`) | PICTURE SAVE ABSENT; recordings: nothing to show | picture save on a worker with the Saving... note and a greyed key; recordings: ENGINE HAND-OFF |
| 5 | Trail style controls | map page combo "Line / Ribbon" + width (`app_window.cpp:14861`, `:14873`) | SYSTEM > Display has "Trail style" and "Trail width" (`system_bank.slint:42,144`, docs 6.52, driven by `system-display-map.txt`); the MAP PAGE has none (`pages.slint` Trails + Altitude colours only) | PRESENT in Display; ABSENT on the map page | Line / Ribbon keys on the map page (same saved setting); the width stays in Display (decision below) |
| 6 | Counter right-click menu | `app_window.cpp:6990-7013` drawCounterMenu: Enlarge figures, Normal size, Show tuner switches, Counter face, Enlarge every reading. No copy / paste / step | the same menu exists (`main.cpp openMenu Counter`); three of the four size items only logged ("phase 2") | PARTLY | the three now save counterScale / readingsScale (the figures are not drawn larger: see decisions); Copy frequency and Paste frequency added (new) |
| 7 | BROWSE FILES | `plugin_settings_form.cpp:207-240` (file dialog, programs first for a .exe placeholder, the choice committed as typed) | two stubs that printed to stderr (`kit_publish.cpp` decode_entry_browse and page_browse_clicked) | STUB | both call `KitPublisher::browseSetting` -> `core::pickFile`; the PATH setting kind is the only one with a chooser (a file, never a folder) |
| 8 | File drop | `app_window.cpp:1108` (GLFW drop callback), `:3330 submitPendingDrop`: .wav -> the I/Q file; anything else -> the frequency-list import (which refuses non-lists in its own window) | no handler | ABSENT | Win32 drop target on the window (`gaps_win.cpp`); .wav -> I/Q file, .xml / .csv -> import, other -> refused with a note under the counter |
| 9a | SDRplay diagnostic dialog | `app_window_sdrplay.cpp:150-279`, `source/sdrplay_probe` (a child process that takes the radio for minutes, a bias-tee confirmation) | the key logs "not started" (`system_publish.cpp`) | STUB | NOT BUILT (see NOT DONE) |
| 9b | Copy diagnostics | `app_window.cpp:21466 copyDiagnosticsBundle` | the key logged "nothing copied" | STUB | `SystemPublisher::copyDiagnostics`: bundle to the clipboard and to `<reports>/diagnostics.txt`, the status line |
| 9c | RESTART SDRPLAY SERVICE | `app_window_sdrplay.cpp:48-148` | the model existed in `signal_logic.cpp` but no publisher set it and `SourceBody` drew no key | ABSENT in the UI | key + line in `SourceBody`, `SignalPublisher::pollSdrPlay / sdrPlayRestartPressed` (the classic's logic, on `SdrPlayServiceRestart`) |
| 10 | Help | the classic has NO help entry (only the privacy link and "Open foxsdr.com"). The manual is `docs/MANUAL.md` of the repository; it is not installed | none | ABSENT in both | App menu entry "Help: the manual" opens `https://github.com/wonderingStars/foxsdr/blob/master/docs/MANUAL.md` (HTTP 200 checked from this machine); owner decision below |
| 11 | High contrast | none in the classic | none | ABSENT | see the note and the result below |

## What was built, item by item

(See the commit messages and the files named in the table above; the drive scripts are `src/slint/tests/drive/gaps-*.txt` and `restart/gaps-*-2.txt`, the unit tests `test_slint_host_gaps`, `test_slint_gaps_prefs_host`, `test_slint_hc_palette`, the ledger rows 4600 to 4615 and `coverage-gaps.tsv`.)

1. De-emphasis: the engine's SET_DEEMPHASIS order (0 = 50 us, 1 = 75 us, 2 = off); the row is live in NFM and WFM and greyed elsewhere, as the classic's combo is.
2. Digit grouping: `FrequencyCounter.dot-style` (0 thousands, 1 MHz.kHz.Hz). The typed editor is seeded in the chosen style (style 0 keeps the old `%.6f` MHz seed) and `parseTypedFrequencyHz` reads both: two or more dots are grouping marks of a whole number of the unit, one dot is a decimal as ever. The seed always has at least two dots so it cannot be read as a decimal. Saved as `dg=` in the `workspace` entry of the window half (`workspace_prefs`), written only when not 0.
3. Frequency lock: `gaps_logic.hpp`. The table `api` the window's publishers hold is a copy (`lockedApi`) whose `submit` and `submit_text` refuse the tuning commands of a locked radio (`FOXAPI_DENIED`, ticket 0; a batch goes on without the refused ones); the made-up radios of a bounded run (`seam`) ask the same lock. The refused set: SET_CENTRE, SET_FREQUENCY, SET_VFO_OFFSET, STEP_TUNE, BOOKMARK_TUNE, PLUGIN_PRESET, a SCANNER_RUN that starts, and the app-level click-to-tune (FOXAPP_OP_VFO_TO_ABSOLUTE: FOUND BY THE DRIVE SCRIPT, the first run tuned through a locked radio), VFO slider (SET_VFO_OFFSET_FREE), USER_PRESET_APPLY, SCANNER_LIST start, AIRBAND listen / next block / tune one. The padlock key is lit while locked; a refusal puts "Frequency locked" under the counter for three seconds. Per radio, saved as `fl=` (bit n-1 = radio n).
4. Saving: `PluginKit::prepareSavePicture` copies the picture and the paths on the window's thread and hands back a job; `KitPublisher` runs it with `std::async`, the note says "Saving...", the key is greyed (`save-enabled` already existed in `output.slint`), and the answer ("Saved <path>" / the error) replaces the note. The name is taken atomically (`reserveUnique`): two saves in one second are two files (the old code overwrote). The folder is still `<settings folder>\pictures`, not the recordings folder the classic uses (decision below).
5. Trail style on the map page: two `SegKey`s Line / Ribbon beside Altitude colours (`pages.slint`), greyed with Trails; they save the same `mapTrailStyle` Display saves (`KitPublisher::setTrailStyleSink`). The survey was right that Display already has the control (driven by `system-display-map.txt`, row 364); the map page's keys are an addition so the page has what the classic page has.
6. Counter menu: ids 101, 102, 104 save `counterScale` and `readingsScale` (their checked marks read the saved values); 105 Copy frequency (the dotted text in force), 106 Paste frequency (either style, through the lock). Menu labels are plain English like every label of that menu today (not in the catalogues).
7. BROWSE FILES: `KitPublisher::browseSetting`: `core::pickFile` with the classic's request (programs first for a `.exe` placeholder, starts beside the box's text), a choice goes through `PluginKit::editSetting` as if typed; a cancel changes nothing. Called on the window's thread, exactly as the I/Q row's Browse key is (`signal_publish.cpp iqBrowse`): the Slint host has no hang watchdog to trip. FOUND: a settings edit stays PENDING in this window (the answer of FOXAPP_OP_PLUGIN_SETTING_EDIT is not seen as delivered within 150 ticks), so the box kept its old text; a path chosen with BROWSE FILES is therefore shown at once (`browsed_`), and a re-read of the lists after an answer was added (`rereadLists_`). The pending state itself is not changed (it is older than this lane).
8. File drop: `gaps_win.cpp` replaces the backend's drop target on the window (`RevokeDragDrop` + `RegisterDragDrop`) with one that takes the first file of a CF_HDROP; `handleDrop` classifies by extension (`classifyDrop`). `gaps.dropinstalled == 1` was read on the hidden desktop (the window exists and the target registered).
9. SDRplay: the RESTART key is the classic's: shown while the driver has trouble reaching the service, one restart at a time on a worker (`SdrPlayServiceRestart`), outcome sentences as the classic's, rescan after a good restart. Copy diagnostics as above (a bounded run leaves the desk's clipboard alone).
10. Help: menu id 605.
11. High contrast: see below.

## Item 11: how the skins are structured and what a high-contrast variant costs

A skin is ONE struct, `Skin` (`theme.slint`): 128 tokens (grounds, lines, text inks, accent and digit colours, lamp, chip, key, danger, slider, meter, trace, radio and waterfall colours, the two typefaces, six radii, `digit-face`, `lamp-glow`, `shadows`, `hairline`). `Workbench` and `NightWatch` are two globals holding a `Skin` value; `Theme` exposes every token as a property of `Theme.skin`, components read only `Theme.*` (the house-rules test forbids colour literals elsewhere), and the host swaps the whole skin with one assignment (`look_logic.hpp`, `main.cpp applyLook`). Metrics (key heights, panel widths, font sizes) are NOT part of the skin. A third skin therefore costs a palette and nothing else: one more global of 128 tokens, one more branch in the host's skin choice, an option. No component changed. RESULT: built as a palette (`HighContrast` in `theme.slint`): black grounds, white lines and text, one yellow accent, 2 px lines, no glow, no shadow; every reading pair is at least 7 : 1 (`test_slint_hc_palette` reads the .slint and computes WCAG ratios for 30 pairs, the disabled ink at 4.5 : 1). The option is a tick "High contrast" in SYSTEM > Display (saved as `hc=1` in the workspace entry); Windows' own high-contrast setting (`SystemParametersInfo(SPI_GETHIGHCONTRAST)`, polled once a second) forces the skin on. Not done: forced-colours mapping of Windows' own palette (the skin is ours, not Windows' system colours), and a visual review of every page in the new skin (UNVERIFIED, below).


## Results

Wall clock: 14:57 to about 18:30 (3 h 30 min), nearly 2 h of it waiting on three `main.slint` compiles (about 25 min each) and the gates.

Break-it checks (each mutation applied, the named test seen RED, restored; the build afterwards and the suite GREEN again):

| Mutation | Red |
|---|---|
| A10 `opDeemph` returns nothing | `slint_drive_gaps-deemph` (4 failed steps) |
| A1 the picture save's key not greyed while writing | `gaps-picture-save` |
| A2 BROWSE FILES never enters the chosen path | `gaps-browse-files` |
| A3 `.wav` not recognised as a recording | `gaps-drop` (6 failed steps) |
| A4 the SDRplay key press does not reach the launcher | `gaps-sdrplay` |
| A5 Copy diagnostics does nothing | `gaps-misc` |
| A6 menu entry 101 does not save | `gaps-counter-menu` |
| A9 the high-contrast skin is never chosen | `gaps-contrast` |
| B1 the API copy does not install the lock | `gaps-lock` (14 failed steps), `test_slint_host_gaps` |
| B2 click-to-tune left out of the lock's set | `test_slint_host_gaps` (the waterfall step of `gaps-lock` found it first, before the fix) |
| B3 the lock is not written to the settings | `test_slint_gaps_prefs_host` |
| B4 the Help entry opens nothing | `gaps-misc` (helpurl) |
| B5 the digit grouping style ignored | `test_slint_host_gaps` |

NOT break-it-checked: the `.slint` changes (the lock key, De-emph row, map keys, Source key, theme) were not mutated (25 minutes a compile); each is read by its drive script, so a removed control fails the script's `click` or `expect`. The dotted counter's plate is read through the editor seed, not by a screenshot.

Gates (Release, `D:/foxsdr-build/feat-gaps`):
- Headless and slint gate in three ranges (`-I 1,340`, `341,680`, `681,1300`), 763 tests. First pass: "98% tests passed, 6 tests failed out of 340", "99% tests passed, 3 tests failed out of 340", "100% tests passed out of 83". Known false: `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`, `test_diagnostics_free` (stale commit stamp). REAL, mine, and fixed: `test_slint_signal` (the De-emph row stands where the absent Blanking note was probed: the probe moved one row), `test_slint_system` (the two new Display rows shifted rows the test indexes by position: both moved to the END of the Display body), `test_thread_inventory` (the picture save's `std::async` needed a row in `tests/thread_inventory_rows.inc`). After the fixes and a rebuild: those three, all `test_slint_host_*`, `test_slint_hc_palette`, `test_slint_gaps_prefs_host`, `test_i18n` and `slint_drive_ledger`: "100% tests passed out of 31". The ranges were NOT re-run whole after the repairs.
- GUI gate on the hidden desktop (`-L gui -j3`, `FOXSDR_FRAME_CAP_HZ=60`, after the repairs): "99% tests passed, 2 tests failed out of 278", 2710 s. `slint_drive_add-kinds`: flake (passed alone, 66 s). `test_health_app`: fails alone, and ALSO fails on the S6 build (`D:/foxsdr-build/mr-screen1`, a tree without these changes): this machine's state, not caused here, not investigated.
- Ledger: `ledger: 2203 rows, 575 asserted, 94 driven, 1534 not drivable, 0 unclassified`.
- Config: `%APPDATA%\foxsdr\config.json` sha256 BEFORE the first run `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, AFTER the last run `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (every run used the harness's scratch profile).

## MERGE NOTES (files shared with the screen lane S1 and others)

- `src/slint/main.cpp`: additive edits: includes, the `rawApi`/`lockedApi` lines at the API table, the lock and digit-grouping callbacks and the drop handler after `on_edit_cancelled`, a block after `set_tune_hint` in the tick, counter menu entries 105 and 106 (101, 102, 104 now save), app menu entry 605, `gaps.*` state names after `view3d.`, bounded-run seams. Changed existing lines: the typed-frequency parse, the editor seed, the `submit` lambda's seam branch, `applyLook`'s skin choice.
- `src/slint/ui/main.slint`: three properties after `leading-zeros`, the lock `Key` before the RCVR lamp, `dot-style` on the counter, `HighContrast` in the import and export lists. `theme.slint`, `signal_bank.slint`, `system_bank.slint`, `pages.slint`, `digits.slint`: additive.
- `src/slint/workspace_prefs.*`: the `workspace` entry of the window half gains `dg=`, `fl=`, `hc=` (written only when not default); `view3d_prefs.cpp` (S1 edits it) is untouched.
- `src/slint/CMakeLists.txt` (two lines, plus `ole32 shell32 uuid` on the Windows link line) and `src/slint/tests/CMakeLists.txt` (two branches).
- `resources/lang/*.json` and `src/core/lang_assets.hpp`: eight new keys in each catalogue. After merging S1's keys run `python tools/i18n_keys.py --merge <code>` per catalogue and `python tools/embed-lang.py`, never merge by hand; my translations are in the commits (machine made, not reviewed).
- `docs/slint-port-tick.md` rows 4600 to 4615 and `coverage-gaps.tsv`; `docs/slint-drive-coverage.md` is generated: regenerate after merging both lanes' rows.
- `tests/thread_inventory_rows.inc`: one row.

## ENGINE HAND-OFF

- Recordings have no finalising state: `FOXAPI_LIST_RECORDINGS` carries only ACTIVE / STALLED / FULL and `Recorder::stop` finalises synchronously, so there is nothing for a window to show between Stop and "file closed". If wanted, the engine needs a "closing" state on the list item and in `FoxReceiverState`, set while the writer finalises.
- A frequency lock in the ENGINE: an op `RADIO_LOCK` (radio, 0/1) refusing the tuning ops listed in `gaps_logic.hpp`, a `fact.radio.<n>.locked` and the saved setting in the engine's half. The window's lock does NOT stop: plugins that tune (their own retune, DECODER_START applying a preset), the web page, CAT, a scanner or airband block already running when the lock is turned on, another client, and the classic window. It stops every command this window sends, including Link tuning's retunes of a locked follower.
- A settings-edit answer: the answer to `FOXAPP_OP_PLUGIN_SETTING_EDIT` did not clear the edit's pending state in this window within 150 ticks (IBOC Radio path field); cause not found (window side `SettingsEdits::deliver` or the engine's answer).

## UNVERIFIED

- The OS file drop: a real drag onto the window (the drop target registered, `gaps.dropinstalled == 1`; the handler is driven directly).
- RESTART SDRPLAY SERVICE on hardware (UAC prompt, elevated `net stop / net start`, the service's answer). The SDRplay diagnostic dialog is NOT BUILT (the probe child of the classic window was not ported; it needs the radio to prove).
- The system file dialog for BROWSE FILES (the seam answers); the real clipboard for Copy diagnostics and Copy / Paste frequency (a bounded run keeps its own); the real browser for Help.
- Windows' own high-contrast setting (stood in for); a visual review of every page in the high-contrast skin (only the palette's pairs are measured); the dotted counter's picture.
- Native speakers: the eight new strings are machine translations in 33 languages. The menu entries (Copy frequency, Paste frequency, Help: the manual) are English like every label of that menu.
- Band keys, a bookmark recall and a scanner start under the lock: proved at the command level (`test_slint_host_gaps` runs every tuning op through the table), not by a gesture script.

## Decisions needed from the owner

1. Help opens `https://github.com/wonderingStars/foxsdr/blob/master/docs/MANUAL.md` (HTTP 200 here). The classic has no help entry and the manual is not installed; a foxsdr.com page would be better if wanted.
2. "Enlarge figures" and "Enlarge every reading" are saved but the Slint counter and readings are not drawn larger (a component change across many files).
3. The picture save writes to `<settings folder>\pictures`, not the recordings folder the classic uses.
4. The trail width slider stays in SYSTEM > Display (only the style keys are on the map page).
5. Whether the engine-side lock and the recording "closing" state are wanted.
6. Windows' own high-contrast setting forces the skin on; the option can only add to it.
