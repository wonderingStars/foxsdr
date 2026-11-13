# Engine merge lane 5 (`engine-merge-5`): screen lane M merged, and the Transmit-page red settled (2026-11-13)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release, `--parallel 3`), scratch `D:/foxsdr-build/m5-scratch/`. Hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`; `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set. Nothing pushed. No download, no outside request, no radio opened.
Start state found: the worktree was checked out on `feat/screen-m` (1766c164), not on `engine/multi-radio` (d1e54f28, clean, nothing else checked it out); I switched to `engine/multi-radio` after the first run.

## 1. The Transmit-page red (item 1)

`ctest -R "slint_drive_transmit-page|gui_desktop_guard"` through the launcher, each on a clean build of the tree named:

| Tree | Result |
|---|---|
| `feat/screen-m` 1766c164 | `slint_drive_transmit-page (Failed)`, `67% tests passed, 1 tests failed out of 3`; log: `input script line 8: click "Transmit" -> FAILED: no control labelled "Transmit"` (31 following steps fail) |
| `engine/multi-radio` d1e54f28 | `slint_drive_transmit-page ... Passed 15.18 sec`, `100% tests passed out of 3` |

The lane caused it, and it is a real defect. A `dump` at the default window height on the merged tree showed the SIGNAL list ending at "Symbol rate" with no Audio filters, Sinks or Transmit row in the tree: the Radio section is open at every start and the lane's clarifier block (heading, note, read-out, tick, key) plus the symbol rate field made its body about 170 px taller, so the Transmit row (SIGNAL row 5) sat below the window. (The lane's own "scroll" attempt could not work: the row was not in the tree to scroll to.)

Fix (commit `e1eddf91`; test not changed to hide it): the whole clarifier and symbol rate panel now sits behind the existing "More clarifier controls" key (folded at every start), and empty LaneM panels no longer take a layout gap (`LaneM.rows-x.length > 0` on each `KPanel` in `main.slint`). `lanem-signal.txt` was adapted to the folded default (rows.radio < 3 before the key, > 5 after; nothing else changed).
Proof, merged tree at the default 1600x900: the dump now lists "Transmit" at y=827 (it was absent), `slint_drive_transmit-page Passed 15.85 sec` (final build), and a mouse-wheel `scroll` over the list moves it to y=720, fully visible (`D:/foxsdr-build/m5-scratch/shots/m5-signal-default-height-scrolled-transmit-visible.png`).
Residual (not fully solved): at the default height the Transmit header row is at the very foot of the panel (y=827 to 863, the panel ends about 828); by estimate the base had it about 30 px higher (not measured on the base). It is reachable by scrolling, not visible without it. Closing the Sinks section by default, or a shorter Source body, would show it; that is a layout call for the owner.

## 2. The merge (item 2)

`git merge --no-ff 1766c164` -> `792157bb`, no conflicts (the tree equals feat/screen-m; 64 files). Built twice, exit 0. Lane tests after the fix, on the hidden desktop: `100% tests passed out of 15` (`test_slint_lanem_host`, `test_slint_forms`, `test_slint_patch`, `slint_drive_lanem-file/-signal/-tools/-system`, `lanec-lockout`, `tools-airband` (+ nearest, import), `slint_drive_transmit-page`, `test_i18n`, `test_i18n_glyphs`, `gui_desktop_guard`), `Total Test time (real) = 168.02 sec`.

## 3. The overlap (item 3)

Reproduced and REAL, not an animation: screenshot at frame 3 and at frame 360 are the same (`D:/foxsdr-build/m5-scratch/shots/m5-overlap-audio-sinks-and-patch-source.png`, 1600x2400, sections 0,2,3,6 open). Two places: the last key of the Audio filters panel ("Read the pitch now") is drawn over the Sinks header, and the last two lines of the Patch panel's closing note are drawn over the Source header. Dump numbers for the Patch case: the note is at y=828, height 40 (ends 868), the Source row is at y=854: the section is about one wrapped line shorter than its content (14 px). Both panels end in or carry a wrapped note. Cause not proven (a wrapped `Note` in a `KPanel` row counted one line short by the section's `body.preferred-height`); not a one-line fix I could verify without more build cycles, so not changed. Hand-off: `Section.body-height` / `KPanel` note measurement in `sections.slint` / `system_bank.slint`.

## 4. Gates (final tree `e1eddf91`; the records commit changes only `docs/`)

Headless + slint, `-L "headless|slint"` in four slices, `-j4 --timeout 300`: 1147 tests: slice 1-290 `99% tests passed, 3 tests failed out of 290` (420.74 s), 291-580 `99% ... 3 failed out of 290` (497.02 s), 581-870 `99% ... 1 failed out of 290` (339.18 s), 871-1147 `99% ... 1 failed out of 277` (329.28 s).
GUI gate, `-L gui -j3`: `99% tests passed, 2 tests failed out of 344`, `Total Test time (real) = 2618.03 sec` (`slint_drive_transmit-page` and the four `lanem-*` scripts passed).

| Red | Class | Evidence |
|---|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | the five known false failures | not investigated |
| test_failure_files | load flake | `Passed 66.19 sec` alone |
| test_pipeline_afc | load flake | `Passed 25.22 sec` alone |
| slint_drive_gaps-picture-save (GUI) | load flake | `Passed 30.97 sec` alone |
| test_diagnostics, test_diagnostics_free | stale git stamp (the merge and fix commits were newer than the build) | `test_diagnostics.cpp:941 gitCommit() == expect`; after a rebuild at `e1eddf91` both `Passed` (2.42 s, 2.49 s) |
| test_i18n | green throughout | `Passed 14.76 sec` and `11.34 sec` |

Real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

## 5. Owner decisions screen-m raises (its record lists five) and its hand-offs not built

Decisions: (1) the stitched sweep and history are a second panel in lane L's SYSTEM Sweep section (no Sweep page exists); (2) Diagnostics panel read only, the GPU mode stays lane L's one control; (3) a settings field's options drawn as one key per option, not a pop-up; (4) "Clear every channel's settings" and "Clear the history" ask for a second press; (5) the level store's size applies when the store is turned on. New from this lane: (6) the clarifier key is now named "More clarifier controls" but also hides the symbol rate (a better label needs a new string in 33 catalogues); (7) whether to show the Transmit row without scrolling at 900 px (section 1).
Hand-offs not built: patch chords Ctrl+Z/Y/C/V and a bypass key (key-table rows); inspector forms for the IF filter and Mixer settings text; the patch canvas's click-selection is never sent as `PATCH_SELECT`; a "Save as" picker for the sweep history (a typed full path only).

## UNVERIFIED
Base position of the Transmit row (estimated, base not re-measured); the cause of the overlap; frame cost and the clip sweep (the lane's own); all 33 translations unchecked; real device, real pointer, screen reader.
