# Multi-radio, wave 3 (screen): `mr/screen-s5` merged into `engine/multi-radio` (2026-11-11)

Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, branch `engine/multi-radio`, build folder `D:/foxsdr-build/mr-dsp`. The branch already held engine stages 5 and 6 (tip c442f100, `wave-3-engine-merge.md`); this wave
merges the screen stages S3 to S5 (`mr/screen-s5`, tip 943ae681; the records are `screen-s3.md`, `screen-s4.md`, `screen-s5.md`) onto it. Nothing is pushed.

## Commits

| Commit | What |
|---|---|
| e9f8d9b2 | `git merge --no-ff mr/screen-s5`: no textual conflict (89 files). `tools/embed-lang.py` regenerates `src/core/lang_assets.hpp` to the merged text unchanged, so the generated file was not touched by hand. |
| ddb8ae45 | Test support for the joins below: the drive language's second start (`restart=`) and `drag` verb, and two drive scripts (`restart-multi-radio`, `remove-selected-focus`). The application is unchanged. |
| (this record) | docs |

## The three joins (S5 was written against the stage 4 engine)

(a) THE FOCUS AFTER A REMOVAL. The engine now moves a session's focus to radio 1 itself when its radio is removed (`ApiHost::radioClosed`). The host's own handling (`refreshRadios`: the selection goes to the
lowest radio left and `RADIO_FOCUS` is sent for it; a failed read of the focus with NO_DEVICE does the same) is still right and needed no change: it sends the same focus the engine already chose, and it also covers a
removal by another client. Proof: `slint_drive_remove-selected-focus` (new): radio 3 stopped and selected, so the strip reads radio 3 (`state running == 0`); radio 3 removed: selection 1, the strip reads radio 1
(`state running == 1`), cells `1,2,0,0`; a removal of a radio that is NOT selected leaves the selection and the strip's reads alone; radio 2 removed last: selection 1. Passes.

(b) THE SAVED LAYOUT TOGETHER WITH THE ENGINE'S RESTORED RADIOS. S5 restored the saved selection only within the first ten seconds, from radios that the test added after start; the engine now saves radios 2 to 8 and restores
them at start. Proof end to end, on a redirected profile: `slint_drive_restart-multi-radio` (new; `restart=` in the script header runs the SAME scratch profile twice). First start: three generator radios added, 2 x 2, radio 3
selected with Ctrl+3, Link tuning on, radio 2's splitter DRAGGED with the pointer (`drag "Resize Radio 2" 0 -40`, new verb; `workspace.split.2` moved from 45), the run ends (the application saves). Second start,
a script with no `radio add` at all: `workspace.radios == 4` (the engine's saved radios 2 to 4), layout 2 x 2 (`workspace.layout == 2`, `workspace.shown == 2`), `workspace.selected == 3`, Link on (the key checked),
radio 2's splitter where it was left (`!= 45`, `> 10`), `fact.radio.count == 4`. Passes on the first run, 52 s. No code change was needed: the screen's restore window (the first 300 ticks) is longer than the engine's
restore, which happens before the window's first tick.

(c) `read_list` OF THE FOUR LISTS FOLLOWS THE SESSION'S FOCUS. The host selects a radio by sending `RADIO_FOCUS` (so the focused read is the selected radio's) and re-reads the lists when `listSeq` moves; that was
right before and after. What the follow-up lane (`screen-s6.md`, B1) does is to stop depending on it: the host asks for the selected radio's lists by number (`read_radio_list`).

## The two test-support additions

- `restart=<file>` in a drive script's header (`test_slint_drive_run`): after the run the same scratch profile (the settings file the first run wrote, the engine's own saved radios) is started again with
  `tests/drive/restart/<file>`; both starts must finish with 0 failed, 0 not reached. The second scripts live in `drive/restart/` so the CMake glob does not make tests of them.
- `drag "<label>" <dx> <dy>` (`input_script`, `input_target`): the pointer goes down at the control's centre, moves in four steps and comes up at the offset (a Splitter's DragArea reports each move). A target that
  cannot drag answers with a sentence, so the fake targets of the host-input test are unchanged. Its parse and its calls are tested in S6's `test_slint_host_input`.

## Gates (Release, `D:/foxsdr-build/mr-dsp`, one after the other, another lane loading the machine)

- Build: `cmake --build D:/foxsdr-build/mr-dsp --config Release --parallel`, exit 0 (the generated code of `main.slint` took about 25 minutes; a second build for the runner fix, seconds).
- Headless gate: `ctest -L "headless|slint" -j4 --timeout 300`, run in three ranges (1-340, 341-680, 681-1100) because one foreground command is capped at ten minutes: 759 tests. Failures, all named:
  `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode` (the five known false failures of a D: build: each run alone from the worktree root passes, 115, 5174, 5, 22
  and 49 checks, 0 failed); `test_diagnostics_free` (the known stale commit stamp: it passed after the build was refreshed past the last commit); `test_usage_diag_window_free` and `test_tx_audio` (load flakes;
  both passed in the re-run). Nothing else failed.
- Window gate on the hidden desktop (`FoxSDRTests`, `FOXSDR_FRAME_CAP_HZ=60`, no window on the visible desktop): `ctest -L gui -j3 --timeout 300`: **259 of 259 passed**, 2127 s (the 257 of S5 and the two new scripts).
- Ledger: `ledger: 2176 rows, 550 asserted, 92 driven, 1534 not drivable, 0 unclassified` (S5's figure, unchanged by the merge).

## Not covered

No real radio was opened (another lane is using them); nothing was listened to; the engine's `read_list` focus rule (c) is shown by S6's test, not here.
