# Engine merge lane 3 (`engine-merge-3`): manual, links Worker, screen J and K, the three wiring lanes and radio-bridges merged into engine/multi-radio

Date 2026-11-12 to 2026-11-13. Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release, `--parallel 4`), scratch `D:/foxsdr-build/m3-scratch/`.
Start tip `e7e25fc9`, branch `engine/multi-radio`, `git status --short` empty (both verified before the first merge). Method: `engine-merge.md` and `engine-merge-2.md` (same gate commands, hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`).
Nothing pushed. No download, no outside request, no radio opened. The code tip is the commit that fixes the stale drive-script expectation (section 5, GUI); the commit that adds the three record files sits on top and changes only `docs/`. Commits of this lane after e7e25fc9 (first-parent): the eight merge commits, eb52816c (manual sentence), 857807ce and ede57fa7 (wire-b drop and repair), f9348683 (registration), the drive-script fix, the records.

Every branch tip was verified equal to the commit ordered: feat/manual 7541ac6d, feat/links-worker a7b33903, feat/screen-j 423f4854, feat/screen-k cc2f75d8, feat/wire-a a93b7568, feat/wire-b b5d79f4a, feat/wire-c 1d07d036, feat/radio-bridges ef409552 (added by the orchestrator's message of 23:35), claude/iq-file-open-paste-112c87 b74cc386.

## 1. The merges (`git merge --no-ff`, in the ordered sequence)

| # | Branch (tip) | Merge commit | Conflicts and resolution | Tests run after (real result lines) |
|---|---|---|---|---|
| 1 | feat/manual (7541ac6d) | 3bf926e9, then eb52816c | none. The manual record names four "pending-merge" blocks (libraries, helpers, new-plugins, plugin-proof): none can be dropped or promoted, because only part of the libraries block's branches are in this tree (oss-libs and oss-radios arrived in merge 2 as built code without engine registration or screen control; oss-userlibs, oss-voice, the helper host, the new plugins and the plugin-proof branch are not merged). Done: the libraries block's status sentence now says "pending merge (partly)" and names what is in (eb52816c, a one-sentence edit of `docs/MANUAL.md`). The other three blocks stay marked PENDING MERGE. | docs only; no build needed |
| 2 | feat/links-worker (a7b33903) | b4cd62ed | none (a new top-level folder `links-worker/`; nothing deployed, no `npm install`) | `node --test "test/*.test.mjs"`: `tests 79`, `pass 79`, `fail 0` |
| 3 | feat/screen-j (423f4854) | cf4640c4 | none | build exit 0 twice; `test_slint_host_mapj`, `test_thread_inventory`, `test_health_paths`: `100% tests passed out of 3` |
| 4 | feat/screen-k (cc2f75d8) | a5346d82 | `src/slint/CMakeLists.txt` (J's and K's source lines, both kept); `src/slint/main.cpp` (App menu entries and cases: J's Sky positions 650 and Antenna calculator 651, K's Forum 608 and Discord 609, both kept); `src/slint/ui/main.slint` (J's mapj.slint import and export list, K's `export { KRow, LaneK, KTile, LaneKMatrix }`; no exported name occurs twice); 33 `resources/lang/*.json` (a three-way merge of each nested key table, written in the file's own format: 8217 keys added in total, none removed; two keys added on both sides with different text, `Automatic` in `el.json` and `sv.json`: the engine side's text was kept, K's Greek "Αυτόματα" and Swedish "Automatiskt" dropped; `tools/i18n_keys.py`: `3033 translated, 0 empty, 0 missing, 0 stale` in every catalogue); `src/core/lang_assets.hpp` regenerated with `tools/embed-lang.py` (33 catalogues, 9448935 bytes); `docs/slint-drive-coverage.md` regenerated with `tools/slint_drive_ledger.py` (`2321 rows, 673 asserted, 107 driven, 1541 not drivable, 0 unclassified`; `--check` exit 0); `docs/slint-port-tick.md` (J's section and K's rows kept, each under its own heading). | build exit 0 twice; `test_slint_host_numbers`, `test_slint_lanek_host`, `test_slint_lanek_spectrum_host`, `test_slint_host_mapj`, the eight `slint_drive_lanek-*` scripts, `slint_drive_ledger`: `100% tests passed out of 17` (see section 6, incident 1: this run was started from the normal session) |
| 5 | feat/wire-a (a93b7568) | 52c60679 | none | build exit 0 twice; the seven `test_engine_api_wire_a_*`, `test_apply_command`, `test_thread_inventory`, `test_health_paths`: `100% tests passed out of 12` |
| 6 | feat/wire-b (b5d79f4a) | 0c667bab, then 857807ce (the drop, section 2), ede57fa7 (repair of my own over-deletion, section 6 incident 2) | `src/core/app_commands.cpp` (the kOps table: wire-a's `WIRE_A_KOPS` block and wire-b's block 0x8900-0x8930, both kept); `src/engine/engine_api_lists.cpp` (the fact-block stamp takes wire-a's version and wire-b's version and GPU stamp; the local fact list calls wire-a's and wire-b's facts). `tests/CMakeLists.txt`, `tests/thread_inventory_rows.inc`, `app_commands.hpp`, `test_apply_command.cpp`, `engine.hpp` merged without conflict. No operation number is defined twice (the one repeat, `FOXAPP_OP_WIRE_A_FIRST` = `FOXAPP_OP_HISTORY_STEP` = 0x8800, is wire-a's own alias). | build exit 0 twice after ede57fa7 (the first build failed to link: section 6, incident 2); 31 tests (the 22 `test_engine_api_wireb_*`, `test_apply_command` x3, `test_radio_scope`, `test_thread_inventory`, `test_json_dump_policy`, `test_file_kind`, `test_radio_setup`, `test_engine_shell_guard`): `100% tests passed out of 31` |
| 7 | feat/wire-c (1d07d036) | 5fab9f50 | `src/core/app_commands.cpp` (wire-c's block 0x8A00 and up after wire-a's and wire-b's); `src/engine/engine.cpp` (the include list: `engine/wire_a.hpp` and `core/plugin_repo_helper.hpp`; the control pass: `pumpWireB()` then `pumpWireC()`); `src/engine/engine_api_lists.cpp` (the local facts: wire-a, wire-b and wire-c). `tests/CMakeLists.txt`, `tests/thread_inventory_rows.inc`, `test_apply_command.cpp` and the settings registry auto-merged. Operation numbers: no value defined twice across `app_commands.hpp`, `wire_a_ops.hpp` and `third_party/foxsdr_api/foxsdr_api.h`. wire-a 0x8800-0x8825, wire-b 0x8900-0x8930, wire-c 0x8A00-0x8A0F are each in the `kOps` table and in the coverage of `test_apply_command` (wire-a through `wire_a_apply_cover.inc`, wire-b through `wire_b_apply_cases.inc`). | build exit 0 twice (the `plugin_abi.h` change recompiles most of the tree); the `wire_c` tests, `test_plugin_events`, `test_plugin_recorder`, `test_plugin_repo_helper`, `test_plugin_schedule_hint`, `test_apply_command`, `test_thread_inventory`, `test_health_paths`, wire-a and wire-b lane tests: `100% tests passed out of 48` |
| 7b | feat/radio-bridges (ef409552, added by the orchestrator) | e13ea9ff, then f9348683 (registration) | none (17 files, all new or the SDR-IQ source). Registration done as the record's REGISTRATION HAND-OFF says, in wire-b's tables: section 3. | build exit 0 twice; `test_sdriq_d2xx`, `test_sdriq_source`, `test_rfexplorer_source`, `test_engine_api_wireb_sources`, `test_engine_api_wireb_sweep`, `test_engine_api_wireb_rfexplorer` (new), `test_thread_inventory`, `test_apply_command`, `test_vendor_*`, `test_engine_shell_guard`: all Passed (`test_engine_api_wireb_rfexplorer: 58 checks passed, 0 failed`) |
| 8 | claude/iq-file-open-paste-112c87 (b74cc386), EVALUATE ONLY | not taken | see section 4 | - |

Not merged, as ordered: any commit of wire-a or wire-c after the tips above, `feat/android`, `feat/knob-input`, every `old/*` branch.

## 2. What was dropped

The orchestrator's decision D2 for wire-b ("no engine copies of the screen lane K traces"): wire-b's commit f2e06dc1 (items S9 and S10) was kept for its frame log and stripped of its eight-cursor part, in commit 857807ce (plus ede57fa7, section 6). Removed exactly:
* the three operations `CURSOR_SET 0x8919`, `CURSOR_READ 0x891A`, `CURSOR_MARKS 0x891B`: their `#define`s and comment block in `app_commands.hpp`, their rows in the `kOps` table, their three `case` labels in `radio_scope.hpp` (`isRadioScopedOp`) and in `Engine::applyWireB`, the cases in `tests/wire_b_apply_cases.inc` and the list in `tests/test_radio_scope.cpp`; the numbers stay unused (a test, `test_engine_api_wireb_framelog` section D, checks they are not operations);
* `Engine::cursorOp` (about 45 lines in `engine_wire_b.cpp`, declaration in `engine.hpp`), the per-radio `std::array<CursorSet8, 8> cursors` in `WireBState`, the facts `radio.<n>.sigmeas.cursors` and `radio.<n>.sigmeas.marks`;
* the cursor and mark functions of `src/engine/wire_b_cursors.hpp` (`cursorRun`, `CursorOp`, `cursordetail`); the file was renamed `src/engine/wire_b_framelog.hpp` and keeps only `frameLogJson`;
* `tests/test_engine_api_wireb_cursors.cpp` (71 checks): replaced by `tests/test_engine_api_wireb_framelog.cpp`, which carries the frame-log half unchanged in substance (F1, G) and adds a check that the three dropped numbers are no longer operations.
Kept: `FRAMELOG_CONFIG 0x891C`, `FRAMELOG_READ 0x891D`, the fact `framelog`, `WireBState::frameLog`, `wireBStateForTest()`. `src/dsp/sigmeas_cursors.hpp` is untouched (screen lane K uses it). A note was appended to `wire-b.md` saying its cursor text describes what the lane built, not what is in the tree. The orchestrator's other D2 half (the RBW trace) was never built by wire-b.
Nothing else was dropped. No test was weakened or deleted other than as stated here.

## 3. Registration of the radio-bridges sources (commit f9348683)

* I/Q table (`src/engine/wire_b_sources.cpp`): one include and one row `{"sdriq", "RFspace SDR-IQ (USB; needs FTDI's D2XX driver)", false, 196078.0, &makeOne<src::SdriqSource>}`. The table now has 12 rows; `test_engine_api_wireb_sources` asserts 12, that every row's key is its source's own `driverKey()`, and the sdriq row (no address, rate, empty arguments, `sdriq=AB12,timeout=900`, `open-sdriq:` is an open id).
* Sweep host: a fourth kind `rfexplorer`. `SweepHost::startRfe` (new worker `loopRfe`: connect, configure, one complete sweep after another, `hold` at stop), `RfeLinkFactory` with a default that opens the COM port by a validated name and baud (`RfeSerialLink`), `WireBState::rfeFactory`. `SWEEP_OPEN` takes `port`, `baud` (default 500000, only the maker's list), `start_mhz` and `stop_mhz` (rounded to whole kHz), `top_dbm` (-10), `bottom_dbm` (-120), and refuses the keys of other kinds both ways. The port name is in no fact and no sentence. `tests/thread_inventory_rows.inc`: `wire_b_sweep.cpp` 2 -> 3 workers (the test named the mismatch before the row was changed).
* New test `tests/test_engine_api_wireb_rfexplorer.cpp` against a stand-in device written from the maker's UART layout (no COM port is opened: the engine's factory is replaced): `test_engine_api_wireb_rfexplorer: 58 checks passed, 0 failed`. Break-it: removing the hold command at stop gave `57 checks passed, 1 failed` (`dev->saw("CH")`), restored, green.
* `test_engine_api_wireb_sweep` had a line asserting that kind `rfexplorer` is BAD_ARGUMENT (the kind did not exist). It now refuses a key of another kind (`points`) instead, and that test's RF Explorer factory refuses so no test can reach a real port. Documented in the commit message.
* The English radio-setup texts and status sentences are in `screen-handoff-L.md` (nothing was added to `core/radio_setup.cpp`; no translation).
* Both radio-bridges sources are OFF until a person sends the id; a REMOTE session is denied both.

### NEEDED-LIBRARY ALIGNMENT (NOT DONE: left in place, as the brief allows)
The orchestrator asked for ONE needed-library type. Two shapes are in the tree: wire-b's `wireb::NeededLibrary` (`src/engine/wire_b_needed_libs.{hpp,cpp}`, eight rows, operation `NEEDED_LIB_CHECK 0x8930`, fact `needed.libs`, header-only file check with the states `found / not-found / wrong-32-bit / unusable / cannot-check`) and radio-bridges' `needed::Info` / `needed::Check` (`src/source/needed_library.{hpp,cpp}`, one row `ftdi-d2xx`, six states `working / not-found / too-old / wrong-32-bit / would-not-load / not-a-library`, loads and unloads the library silently, reads the version resource). I did not convert, because the conversion is not clean:
1. Five of wire-b's eight rows are not files at all (a Soapy module found by a name fragment in an install, a running program, software on another machine, a user's firmware file); the six states of the radio-bridges shape have no place for "cannot check", so either its enum (the owner's rule names six) or those rows would have to change.
2. wire-b's check reads file headers only and never runs vendor code; the radio-bridges check loads the library. Expressing wire-b's rows in the radio-bridges shape would make the check for the WiNRADiO libraries load vendor code at "Check again", or would need a second check path under the same type.
3. `Info` has no `sourceKind`, `kind`, `pageStatus` or `fetchAllowed` string as wire-b's rows carry them (it has `fetchAllowed` as a bool); the registry address is empty for every wire-b row and unverified for `ftdi-d2xx`.
What remains, exactly: (a) decide whether the check loads vendor code; (b) one struct carrying both field sets with a kind enum and a seventh state; (c) put the nine rows (wire-b's eight plus `ftdi-d2xx`, calling `checkFtd2xx`) in the one table; (d) the test the orchestrator asked for: the operation `NEEDED_LIB_CHECK` with `{"id":"ftdi-d2xx"}` reports the SDR-IQ row with the six-state result; (e) keep `test_engine_api_wireb_neededlibs` and `test_sdriq_d2xx` passing (adapt, do not delete). The two shapes are both described in `screen-handoff-L.md` section C so a screen lane is not blocked on this.

## 4. The evaluate-only branch: claude/iq-file-open-paste-112c87 (b74cc386) is NOT taken

Decision: not taken, because the behaviour is already in this tree. Evidence (read with `git grep` and `ls` at the merged tip): the same commit message (subject "The I/Q-file row can choose a file and take a pasted one: Open over an empty box is the file dialog, a Browse key beside it, a copied recording pastes as its path ...") is commit 50c68615 in the history of this branch; `typedIqPath` is in `src/core/iq_file_box.hpp` and used by `src/gui/app_window_disk_work.cpp:88` and by the Slint window's `src/slint/signal_logic.cpp:1460`; `browseIqFile` and the "Browse..." key are in `src/gui/app_window.cpp:8144` and `app_window.hpp:4039`; the clipboard file-list paste (`CF_HDROP`) is `src/core/clipboard_files.cpp` (its header says it moved to `core/` in docs 6.63); `src/gui/paste_files.{hpp,cpp}`, `tests/test_iq_file_box.cpp`, `test_iq_file_box_app.cpp` and `test_iq_open_choice.cpp` exist. `git cherry HEAD b74cc386` prints `+` only because the earlier landing moved files and changed lines; the old commit would also rewrite all 33 catalogues and `lang_assets.hpp` (51 files, 211521 insertions) for strings that are already in them.
What it would need if it were wanted anyway (three lines): a rebase of one commit across the move of `iq_file_box.hpp` to `core/`, the catalogue merge by key (not by file), and a re-run of `tools/embed-lang.py`. Nothing is recommended: the feature is present.

## 5. Gates

Headless + slint, `ctest -C Release -L "headless|slint" -I a,b -j4 --timeout 300`, 1120 tests in four slices of 280, through `D:/foxsdr-build/m3-scratch/m3-gate.bat` started by the hidden-desktop launcher (desktop `FoxSDRTestsEM`) and waited on through a done-file, at the code tip f9348683 (binary stamp `e13ea9ffe11c-dirty`: section 6):

| Slice | Result |
|---|---|
| 1-280 | `99% tests passed, 3 tests failed out of 280` (291.60 s): `test_airband_data`, `test_band_plan`, `test_converter_call_sites` |
| 281-560 | `99% tests passed, 2 tests failed out of 280` (601.00 s): `test_engine_api_wire_a_record`, `test_page_resize_id` |
| 561-840 | `99% tests passed, 1 tests failed out of 280` (321.40 s): `test_png_decode` |
| 841-1120 | `99% tests passed, 4 tests failed out of 280` (349.23 s): `test_health_app_stepped`, `test_usage_diag_window_stepped`, `test_usage_diag_window_free`, `test_diagnostics_free` |

Total 1120 run, 1110 passed, 10 red. Classification (re-runs alone, one at a time, through ctest on the hidden desktop):

| Red | Class | Evidence |
|---|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | the five known false failures | red as in every earlier record; not investigated |
| test_health_app_stepped | load flake | Passed alone (23.40 s) |
| test_usage_diag_window_stepped | load flake | Passed alone (0.74 s) |
| test_diagnostics_free | stale build stamp, not a defect | alone: `git HEAD now: f934868343a1, binary says: e13ea9ffe11c-dirty` (`tests/test_diagnostics.cpp:941`); re-run on the rebuilt final tip: in the lane's final report |
| test_usage_diag_window_free | NOT classified as a flake: failed in the gate AND alone (0.74 s), `tests/test_usage_diag_window.cpp:421 has(bundle, engines)` (the diagnostics bundle must contain the engine's context block); its `_stepped` twin passes. It sits in the stale-stamp family (the block carries the build's commit stamp) but this is UNPROVEN; re-run on the rebuilt final tip: in the lane's final report | cause open |
| test_engine_api_wire_a_record | wire-a's own intermittent test, NOT caused by the merge as far as I can tell: it failed in the gate and in 3 of about 30 runs alone (the other runs Passed, 10.8-10.9 s each, 8 of 8 in one batch, 18 of 20 in another). The failing section is `L1: lock while recording` (`tests/test_engine_api_wire_a_record.cpp:162-167`: after an I/Q take has started, `SET_CENTRE` was ACCEPTED instead of DENIED with "centre frequency is locked while an I/Q recording is open"; centre became 101000000). Probable cause (UNVERIFIED): the test waits for the take's WAV to exist and sends the retune before the engine has marked the take open. Not repaired (the lane's own defect, not a one-line fix without reading the engine's record state machine). | next wiring lane |
| test_plugin_abi3_compat | not in the red list. Run alone with `-V`: `test_plugin_abi3_compat: 0 checks, 0 failed`, Passed (0.01 s). The brief says it showed `269 checks, 134 failed` before these merges; here it ran no checks. UNVERIFIED why (a skipped or gated body); not chased, as the brief says | - |

No `--repeat until-pass` was used. All other lane tests passed in the gate: all the `test_engine_api_wire_a_*` except the one above, `test_engine_api_wire_b*`, `test_engine_api_wire_c*`, `test_engine_api_wireb_rfexplorer`, `test_sdriq_*`, `test_rfexplorer_source`, `test_thread_inventory`, `test_apply_command` (x3), `test_engine_shell_guard`, `test_engine_api_source_kinds`, `test_engine_api_decode_radio`.
The two tests that need the gitignored WAV fixtures (`test_engine_api_decode_radio`, `test_engine_api_source_kinds`) found `scope.wav` and the other fixtures in `src/slint/tests/drive/fixtures/` of this worktree (present before the gate; no `make_*.py` had to be run) and passed in the gate.

**GUI** (`ctest -L gui -j3 --timeout 300`, hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`, `m3-gui.bat`), 331 tests:

`99% tests passed, 2 tests failed out of 331`, `Total Test time (real) = 2476.40 sec`. Reds:

| Red | Class | Evidence |
|---|---|---|
| slint_drive_lanec-scanner | **caused by the merge**, fixed (a test expectation that the merge made stale, not a defect) | failed in the gate (24.26 s) and twice alone: `foxsdr-slint: input script line 13: state lanec.enginenote == 1 -> FAILED: it is 0`. Cause: screen lane C shows the note "The priority list and the scan thresholds are saved; this scanner does not use them yet." while the engine fact `scanner.extras` is not "1" (`src/slint/lanec_publish.cpp:376`); wire-a (merged before) publishes `scanner.extras` = "1" always (`src/engine/wire_a_scan.cpp:365`) because it now reads those settings, so the note is no longer shown and the drive script's line 13 must expect 0. Changed in `src/slint/tests/drive/lanec-scanner.txt` (the script's own comment says why); `tools/slint_drive_ledger.py --check`: `2321 rows ... 0 unclassified`. After: Passed twice alone (24.29 s, 24.86 s). The behaviour the test pinned is the screen's "engine does not read it" note, which is correctly gone; the rest of the script (47 other steps) is unchanged and passed. |
| test_diagnostics | stale build stamp, not a defect | `git HEAD` moved after the build (the registration commit, the test fix and this record); re-run on the rebuilt final tip: in the lane's final report |

Every other gui test passed, including all eight `slint_drive_lanek-*`, the `slint_drive_mapj-*` scripts of screen lane J, and every drive script that existed before the merges.

## 6. Incidents and deviations

1. **Drive scripts started from the normal session.** After merge 4, the screen-K check `ctest -R "lanek|..."` included the eight `slint_drive_lanek-*` tests (label `gui`) and was started from my normal shell, not through the hidden-desktop launcher: 8 gui-labelled tests ran for 350 s of process time. Whether the drive harness (`test_slint_drive_run`) keeps its windows on a desktop of its own is not something I verified here. I have no sight of the desktop (the owner is asleep) and no dialog was reported to me; this is a deviation from "nothing may appear on the visible desktop". From then on every run that could show a window (the headless gate, the re-runs, the GUI gate) went through the launcher. All the per-merge lane test runs of section 1 (headless-labelled tests, as in merge 2) were likewise started from the normal session; none of them opens a window by design, and none was reported to show one.
2. **My own over-deletion in wire-b (repaired).** The first attempt at the drop (857807ce) cut `engine_wire_b.cpp` from the start of `Engine::cursorOp` to the start of `Engine::applyWireB`, but the lane's later commits had put `tracksOp`, `neededLibOp`, `coverageOp`, `pumpTracks`, `sbsOp`, `pumpSbs`, `followOp`, `pumpFollow`, `iqOutOp` and `sweepOp` in between. The build did not link (`unresolved external symbol ... Engine::sweepOp / iqOutOp / followOp / sbsOp / pumpSbs`). ede57fa7 takes the lane's file and removes exactly `cursorOp` and its three `case` labels (50 lines); `git diff feat/wire-b -- <the files touched>` now shows only cursor removals. After it the build was exit 0 and the 31 wire-b-related tests passed.
3. **Scripted edits and backslashes.** Three of my edit scripts first failed on escaped quotes (a tool-side quoting issue, no file was damaged); each was re-run from a script file written with the Write tool.
4. **Stamp.** `test_diagnostics` and `test_diagnostics_free` compare the binary's embedded commit with `git HEAD`: the gate ran on a build made before the registration commit and before this record's commit. They are re-run on a rebuilt final tip (final report).
5. No ctest was left running; every gate ran to its end and its output was read. No process of mine was killed; no Python was killed.

## 7. ABI, configuration, tables

* `src/core/plugin_abi.h` CHANGED (wire-c): two members appended at the END of `CascadeHostApi` and a label-length constant; `apiLevel` unchanged (the struct size is the version). `docs/PLUGIN-API.md` has a "Recorder" section; `tests/test_plugin_events.cpp` and `tests/test_plugin_schedule_hint.cpp` (which assert that `events_seq` is the last member) were updated by wire-c to expect the two new members after it. See PLUGIN HEADER HAND-OFF.
* `third_party/foxsdr_api/foxsdr_api.h`: not changed by these merges (every wire operation is a FOXAPP op in `app_commands.hpp`).
* Engine operation numbers added: wire-a 0x8800-0x8825 (`core/wire_a_ops.hpp`, 38 operations), wire-b 0x8900-0x8930 less the dropped 0x8919-0x891B (46 operations), wire-c 0x8A00-0x8A0F (16). No value is defined twice (checked by a script over `app_commands.hpp`, `wire_a_ops.hpp` and `foxsdr_api.h`).
* Settings: wire-c keys live in `wirec.json` beside `config.json` (config.json is not touched); wire-a's files in the per-user folder, all absent by default.
* `%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the start, after the headless gate and after the GUI gate: unchanged (`sha256sum` of the real file printed the same value each time). No test opened the real file (the tests redirect the profile).

## PLUGIN HEADER HAND-OFF (for the plugins repository; this lane does not own it)

`git diff e7e25fc9 HEAD -- src/core/plugin_abi.h` (wire-c, 18 added lines, nothing existing edited; the two members go at the END of `CascadeHostApi`, after `events_seq`):

```diff
@@ -1753,6 +1753,14 @@ typedef struct CascadeScheduleHint {
     char decoderName[CASCADE_SCHEDULE_LABEL_CHARS]; /* the plugin the user would want started with it, "" none (informational: the host does not start it) */
 } CascadeScheduleHint;
 
+/* ---- Recorder (appended after host API level 2; wiring lane C): check CASCADE_HOST_HAS(host, recorder_start). ----------------------------------
+ * A plugin asks the host to start and stop an AUDIO take of the radio it runs on - the demodulated audio, as the user's own Record key makes - under a LABEL
+ * the plugin names ("tg-1234", "ch16"), for example one take per talkgroup. It is a request, like set_mode: it needs the SETTINGS grant the user gives the
+ * plugin (CASCADE_API_DENIED without it), it is queued and applied by the host's next pass (CASCADE_API_OK means ACCEPTED, not started), and the host
+ * refuses a start while the radio is already recording (a take the user started is never taken over, and a plugin can stop only a take it started). The file
+ * goes to the recordings folder named with the label first. A plugin that is stopped, unloaded or detached has its take ended by the host. */
+#define CASCADE_RECORDER_LABEL_CHARS 33 /* the longest label is 32 bytes of [A-Za-z0-9._-], plus the terminator */
+
 /* ---- Decode events (appended after host API level 2; engine wiring W2, brief item 13 "AD3"): check CASCADE_HOST_HAS(host, post_event). ----------
  * A plugin POSTS a structured event (a decoded message, an alert, a position) and READS the events other plugins posted - and, as events of kind "text",
  * every decoder line the host drained. The host keeps the newest CASCADE_EVENT_RING of them in one ring in memory (never written to disk by the host);
@@ -1990,6 +1998,16 @@ typedef struct CascadeHostApi {
     int32_t (*read_events)(void *ctx, uint64_t afterSeq, CascadeEvent *out, uint32_t cap);
     /* The newest sequence number the host has assigned (0 = none yet): cheap to poll, so a plugin calls read_events only when it moved. */
     uint64_t (*events_seq)(void *ctx);
+
+    /* ---- Recorder (appended after host API level 2; wiring lane C: check CASCADE_HOST_HAS(host, recorder_start)). SETTINGS grant. ---------------- */
+
+    /* Asks for an audio take under `label` (1 to 32 bytes of [A-Za-z0-9._-]). CASCADE_API_OK: accepted and queued; CASCADE_API_DENIED (no SETTINGS grant, or the
+     * plugin is stopped); CASCADE_API_BAD_ARGUMENT (NULL, empty or an unusable label); CASCADE_API_OUT_OF_RANGE (a label over 32 bytes); CASCADE_API_BUSY (the
+     * plugin's small request queue is full: ask again next block); CASCADE_API_DETACHED. Whether the take really began is not answered here: the host
+     * refuses it while the radio is already recording or a file is being finished, and says so in its own log. */
+    int32_t (*recorder_start)(void *ctx, const char *label);
+    /* Ends the take THIS plugin started (no effect on any other). Same codes; a stop with no take of this plugin's running is accepted and does nothing. */
+    int32_t (*recorder_stop)(void *ctx);
 } CascadeHostApi;
 
 /*
```

The plugins repository must append the same constant and the same two members at the same place in its copy of the header (the struct size is the version; `CASCADE_HOST_HAS(host, recorder_start)` is the check a plugin makes). The lane's own wire-a ABI requests are in `not-wired-after-merge-3.md` item 13.

## OWNER DECISIONS raised by the lane records (one list, de-duplicated; none was decided here)

Wiring and engine
1. Record lock default (wire-a 13c): OFF today (a source change ends an I/Q take, as before); ON would refuse the change instead.
2. Companion launcher (wire-a row 15): stays unwired (it starts programs the user names). CAT maximum RIT / XIT (10c); scan aids (49); band stacking and mode memory (50).
3. Whether the Slint host calls the pure analyser-project header (wire-a row 20); how a same-process host reads the waterfall lines (`WireA::waterfallCopy`) and whether the API gets a read call; the level store's memory size (row 39); use of the `FREQ` chunk for WAV centres (row 25).
4. Spans for measurements from the scope tap (answered YES by the orchestrator: 1.31 s at 200 kHz) versus a raw-band ring; playback ownership (answered: wire-a).
5. Aaronia authentication (nothing stored); on-disk retention of track and coverage positions (memory only); `/status.json` listing privacy rules (off, nothing calls the setter, always 404); the interleaved HackRF sweep (GPL code); the owner's wider-bind rule for the network outputs (loopback only now).
6. Manifest signature: who holds the Ed25519 seed and when `kOwnerKeyHex` is filled (wire-c S6 is wired and inert until then); code-signing certificate (S12); HTTPS alone or not; the links Worker: hostname (proposed `links.foxsdr.com`), whether it is the same key as the update manifest's, the Cloudflare plan, who reads the admin list.
7. Helper programs: code signing of helper executables (SmartScreen); where helpers are hosted; a store shelf for them; the separate consent tick is built, default required.
8. Metrics page has no scraper token (no credential storage): a Prometheus scraper cannot sign in with a cookie. CORS credentials need HTTPS (`web.cors_credentials` accepts only "off"). UPnP left unwired. Decode database on disk, webhook cleartext URLs, CAT emulation exposure (`bind_all`).
9. Passwords at rest: not decided; plugin PASSWORD settings and the web password hash are written to `config.json` (and its dated copies); protection at rest is unverified (wire-c).
10. The QSO log (`qso-log.json`, callsigns the person typed) is on disk: confirm a user-typed log file is acceptable. S3 resume: whether a healthy open radio should be closed and reopened after a sleep.
11. Number-format default: screen lane K defaults to "follow the Windows regional settings" (a comma locale sees commas in four readouts); wire-c kept "always a dot" until its units / number-format wiring exists. Pick one.
12. First-run wizard: the usage-reports switch (screen K defaults it ON as the sysmodels record decided; wire-c changes it only if the person chooses it): say whether the wizard asks.
13. Backup, import, reset, profiles exist twice (screen lane K in the host, wire-c as engine operations with a different pending mechanism): choose one mechanism (DUPLICATE rows in `screen-handoff-L.md`).
Radio bridges
14. G31DDC (32-bit only): a 32-bit helper process, or point owners at WiNRADiO's ExtIO driver; may a lane read the public headers (`fobos.h`, `perseus-sdk.h`, `htraapi.h`) of a library the user installed; RFNM waits for a stable interface; the SDR-IQ driver page, FTDI licence and folder (PATH only, or a driver-folder setting).
15. The English radio-setup text for the SDR-IQ and the RF Explorer (in `screen-handoff-L.md`) for the radio-setup owner to add and translate.
Screens
16. Screen K: the order of the SYSTEM bank sections (About first); import / reset / profile switch / undo take effect at the next start; the Forum and Discord addresses (QUESTION: what are the two addresses?); the wizard's telemetry switch defaulted ON; averaging, detector, mask and cursors act on the displayed trace only.
17. Screen J: coverage persistence (session only now); track retention for the replay; which of the four map sources (Met Office / RainViewer, OpenAIP, Receiverbook, RepeaterBook) may be built after their terms are read; the Stellarium link; defaults of the APRS paths and Airspaces switches; the Airspaces layer's faint wash of closed footprints.
Manual
18. The manual's open questions (11, in `docs/feature-records/manual.md`): which window ships, config-file-only features (TCI, UDP audio), how a KiwiSDR is opened, where helpers and the new plugins' config files live, the ADS-B decoder version (oss-misc and oss-tables both set 1.12.0), bitness of the mbelib, Codec 2 and FFmpeg files, Help opening the GitHub copy of the manual or a foxsdr.com page, whether the HTTPS and listeners section stays, and the release notes' blank version, date and gate results.

## UNVERIFIED

* Nothing built for Linux; `packaging_manifests` is registered or not as `pwsh` allows on this PC (not examined); no radio, no outside service, no real RF Explorer, SDR-IQ, FTDI library, COM port or WiNRADiO library touched (every bridge is proved against a stand-in).
* The OpenAPI file was not touched; no `/status.json` response was seen from a live server.
* The two edits to the manual (one sentence); the screen-K and screen-J pages were not looked at (the clip sweep of the first merge was not repeated); machine-translated catalogues are as the lanes left them (no speaker has checked them).
* The wire-a intermittent test (section 5), `test_usage_diag_window_free` and `test_plugin_abi3_compat` (section 5).
* Whether the drive scripts of incident 1 kept to their own desktop.
