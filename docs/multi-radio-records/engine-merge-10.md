# Engine merge 10 (`engine-merge-10`): the three must-fix branches (B, E, A) into `engine/multi-radio` (2026-11-17)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `903af9ca` (engine merge 9, clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, `FOXSDR_SLINT_CLIP_PROBE=OFF`, Release, `--parallel 6`), scratch `D:/foxsdr-build/m10-scratch/`. Every test ran through the hidden-desktop launcher on desktop `FoxSDRMerge10`, `FOXSDR_FRAME_CAP_HZ=60`, `ctest -j3` (reruns `-j1`); `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set; no radio opened, nothing transmitted (the transmit file was not edited), nothing pushed, downloaded or deployed, no outside request; no other worktree touched; the two live-send switches (`kRadioReportsLiveSend`, `kCompanionFaultReportsOn`) are still false; the approved privacy wording is unchanged (section 7). The real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing is running: every build and test ran to its end in the foreground.

## 1. The merges

Tips checked before merging, exactly as briefed: `feat/mustfix-e` `5bfaa899` (it contains `feat/mustfix-b` `3dc4637d`, so B and E are ONE merge: the brief names two branches, three in its heading), `feat/mustfix-a` `14e6434a`. All cut from `054b5a99`.

| Merge commit | Branch | Conflicts | Settled |
|---|---|---|---|
| `afcce1d3` | `feat/mustfix-e` (with B) | none; git merged `tests/errpath_allow.tsv` and `kPendingPin` (459 to 410) without a marker | the allow list re-seeded after the second merge (section 4) |
| `9f423e63` | `feat/mustfix-a` | the 33 `resources/lang/*.json` and `src/core/lang_assets.hpp` only (`main.cpp`, `main.slint`, `errpath_allow.tsv` merged clean) | catalogues by key against the common ancestor `054b5a99` with `m10-scratch/lang_union.py` (merge 9's script, plus one rule: a key lane A REMOVED and the merged side had not changed is removed here too; no key changed on both sides, output format identical to the files); the 17 removed keys are the stale wording (Forum, Discord, GLOBE, "Off. Nothing is transmitted.", the "yet" sentences). `lang_assets.hpp` regenerated with `tools/embed-lang.py`, not hand-merged. `tools/i18n_keys.py`: 33 catalogues, **4767 keys each** (4772 at merge 9 + 12 new strings of lane A - 17 removed), 0 empty, 0 missing, 0 stale; the Korean word-spacing check is in `test_i18n`: Passed |

Shared-file edits of the lanes (`main.cpp`, `main.slint`, `src/slint/CMakeLists.txt`, `signal_publish.cpp`, `engine.cpp`) merged without a marker; the full build (exit 0) is the proof that every call site compiles against lane E's helper signatures. Nothing refers to `test_slint_stubs`, `ui/stubs.slint` or `lanek-community` any more except lane A's own deletion note (`COMPONENTS.md`) and the test that holds the file gone (`test_slint_host_mustfixa`).

## 2. Fixes after the merge (each its own commit; nothing weakened)

| Commit | What |
|---|---|
| `4a4d9c28` | error-path allow list re-seeded (section 4); `telemetry_worker`: the manifest test split `MANIFEST.sha256` on `"\n"`, so on this CRLF working copy every name ended in `\r` (`ENOENT ... drop-synthetic-beat.json\r`). Lane E's record lists the change to `/\r?\n/` but its tip does not hold it. Now `split(/\r?\n/)`; the fixture count, the hash of each and each verdict are checked exactly as before. 116 of 117 then 117 of 117 |
| `860fbc65` | the two intermittents at their causes (section 8) |
| `bb15a43a` | **`test_driver_policy`**: 4 checks failed alone (`test_driver_policy.cpp:35`, "every known radio family has exactly one reviewed row"). Lane B added HydraSDR, AOR, the Icom IC-R8600 and the Mirics family to the first-run check's table of known devices and did not run this test. Four `NotAllowed` rows with `evidenceVerified` false added to `core/driver_policy.cpp` (the source says the ids come from the driver's own table and no vendor Windows driver page was opened). Behaviour unchanged: `decide()` already answered `NotAllowed` for a family with no row. RED (350 checks, 4 failed), GREEN, test unchanged |
| `6b64b502` | **`layout-section-overlap-1600` / `-624`**: `expect "Source"#1 top >= "868" -> FAILED: it reads "867.00"`. Lane A shortened the Patch note (SIGNAL bank): "...Pick parts by number here: a click on the canvas does not select in the engine yet." became "...Pick parts by number here." It wraps to two lines (top 828, bottom 855, measured with a probe script) instead of three (868). Source header 867 = 855 + 12; everything below moved up one wrapped line (13 px): "Read the pitch now" bottom 2279 (2292), Sinks header 2467. The scripts assert "the next header starts at or below the last control above it" with the measured bottoms of the old layout; re-pinned to the new measured bottoms (section 4), arithmetic in the scripts |
| `6198f300` | `test_engine_publish_alloc`: the measurements written beside the bounds (comment only, section 4) |

## 3. What the must-fix rules found on the outputs pages (the outputs follow-on meets lane A's rules for the first time)

Lane A's search words (not built, not yet, later, coming, phase, beta, soon, planned, yet) were run over EVERY `tr()`, `@tr()`, `FOX_TR_NOOP()` and `T()` string of `src/slint` on the merged tree (`m10-scratch/wordscan.py`; lane A's own test only holds a narrower banned list). **Nothing on the outputs pages (`s4_*`, `s4b_*`) matches** except "...and play it back later" on the macro recorder, which is an ordinary use (play a take later), not a missing feature. The rest are the ordinary empty-state words lane A left on purpose ("No markers yet", "Not checked yet") and the update channel's real name ("Every release (beta)").

The two items lane A left for the outputs follow-on are whole:
- **Range-scan lockout control.** The Tools scanner's "Lock out 10 min", "Lock out for good" and "Clear lockouts" (`tools_bank.slint` 615 to 632) send `FOXAPP_OP_SCANNER_EXTRAS` 1 for a range or plan scan (`lanec_publish.cpp:184`); the SCANNER page's "Locked-out frequencies" block (`s4b_publish.cpp` `buildScan`) reads the engine's `scan.lockouts` fact (every locked frequency, "and N more") and "Always skip these frequencies" sends `SCANNER_EXTRAS` 5. Drives `s4b-lockout` and `s4-scan` read it back from the engine and Passed in the full GUI gate and alone.
- **Notification choice.** "Show each alert as a desktop notification" (`s4b_publish.cpp` 421 to 429) sets the wirec key `alerts.notify`, shows "Notifications handed to Windows" and the engine's `alerts.notify_problem`; its tip's "turn them on above" is true (the "Raise an alert..." tick is the Keyword alerts tick of `s4_publish.cpp` in the same Outputs bank, above it). Drive `s4b-outputs` Passed.
- Every key of the S4b panels (rotator turn, stop, park; listening log add, export, clear; macro record, stop, save, play, delete, halt; MCP token) has a handler that sends an engine command or a wirec setting (read in `s4b_publish.cpp`; no empty handler); the drives read the effects back.

Not done: no pass by eye of the Outputs rows (UNVERIFIED, mouse drive).

## 4. Pinned numbers and changed expectations (every one, both values)

| Where | Was | Now | Inputs and reason |
|---|---|---|---|
| `kPendingPin` (`test_error_paths.cpp`) | 459 (merge 9); 410 on lane E's branch; 459 on lane A's | **410** | union key by key against `054b5a99` (git merged the three sides without a marker), then `tools/errpath_seed.py` on the merged scanner: 3473 sites, 3214 allow lines kept, 0 re-keyed, 0 dropped, 0 sites without a survey row; the only change to the file is the position of one line. Arithmetic by group, merge 9 to now: G3-control-refused 27 to 3 (-24), G2-stream-health 12 to 0 (-12), S5-driver-install 5 to 0 (-5), S8-sdrplay-service 4 to 0 (-4), S4-first-run-check 2 to 0 (-2), G7-patch-radio 1 to 0 (-1), reports-not-in-function 168 to 167 (-1) = -49: 459 - 49 = 410. Groups now: reports-not-in-function 167, O-outputs 55, unsure 45, R-record 27, G10-gpu-fft 16, D-gps-serial 13, F-files 11, T-tx 11, unmatched 9, S2-window 7, S1b-headless 7, A-audio 7, P-plugin 5, P-store 5, S3-plugin-ui 5, S6-scan 4, S9-report-upload 4, S7-ui-engine 3, S1-startup 3, G3-control-refused 3, G1-device-dead 2, PL2-companion 1 = 410. **None of the three lanes added a PENDING site** (lane A: one ALLOW line; the outputs follow-on: none) |
| `test_engine_publish_alloc.cpp` F3 bound | 2800 (merge 9) | **2800** | MEASURED on the merged tree: rebuilds 2528 to 2537 (max 2537; 2535 at merge 9), settled passes 0; 2537 x 1.10 = 2790.7, so 2800 stands, set once |
| same, F12 bound | 2400 | **2400** | MEASURED 2165 to 2166 (2163 to 2164 at merge 9); 2166 x 1.10 = 2382.6, so 2400 stands, set once |
| `layout-section-overlap-1600` / `-624`, `Source#1 top >=` | 868 | **855** | the Patch note's measured bottom, section 2 (868 - 13) |
| same, `"Read the pitch now" bottom <=` and `Sinks#1 top >=` | 2292 | **2279** | 2292 - 13, measured; the Sinks header reads 2467 |
| `telemetry-worker/radio-reports.test.mjs` manifest split | `split('\n')` | `split(/\r?\n/)` | CRLF working copy, section 2 |
| lanes' own listed changes (not mine) | | | lane E (`radio-reports.test.mjs` class list, kinds 9 to 12, site tests), lane A (`test_slint_host_*` typed comma, themes six to five, parts bin 11 to 9, usage Off sentence, drive `mapj-layers`, `system-look`, `wg2-xl-fit`): each is in that lane's record with both values; none was changed by this merge, and every one passed in the merged tree |

No expectation of a base test was weakened. The pin is at or below 410 plus nothing.

## 5. Report words: the Worker's own validator over the merged tree

`FOXSDR_DUMP_APP_MESSAGES=m10-scratch/app-messages.jsonl` on `test_radio_events`, `test_radio_reporter` and `test_radio_reports_engine` (hidden desktop), then `node tests/radio_reports_validate.mjs` over the whole file: **146 messages, 146 accepted (`OK`), 0 refused** (22 from `test_radio_events`, 68 from `test_radio_reporter`, 56 from the engine test; the tests' own lines: "validator saw 68 messages: 0 refused", "56 messages the engine made: 0 refused"). Kinds in it: first_session 7, radio_close 10, radio_fail 37, radio_found 10, radio_open 22, radio_recovery 7, radio_settings 21, radio_stream 7, session_end 21, session_env 3, session_fault 1. `test_mustfix_b`, `test_fobos_source` and `test_radio_reports_never_sent` do not call the validator (they pass through the same reporter). Every call site compiles against lane E's helper signatures (full build exit 0, including the outputs follow-on's and the radios follow-on's calls). The Worker's own node suite (`telemetry_worker`): 117 of 117.

## 6. Help topics, catalogue and privacy wording

- **In-app help** (28 topics, `lanex_help.cpp`): no topic mentions the hidden theme (Bench Classic XL), the three hidden map switches, the two hidden patch parts, Stellarium, Forum, Discord or the Community links. The Source topic says the chip reads GENERATOR while no radio is open: `main.cpp` still prints that chip. One finding, NOT changed: the `sys-usage` topic says the Send anonymous usage reports switch "turns the usage reports on or off", while the approved wording says a radio that fails to open is still reported with it off. It is a sentence beside the privacy statement (33 catalogues); row `M10-help-usage`.
- **Catalogues**: section 1; 0 missing, 0 stale, Korean check green.
- **Approved privacy wording**: `git diff --word-diff 14e6434a HEAD -- PRIVACY.md docs/DIAGNOSTICS.md` is EMPTY (0 bytes), no "DRAFT, awaiting" marker remains in either, and the in-app sentences are untouched by this merge. Nobody edited those words.

## 7. Gates

Final code state: commit `6198f300` (full `cmake --build` exit 0 after it). Commits after it are documentation only, so a stamp test run on this build reads the older commit (as at merge 9).

**Full headless gate** (`ctest -L "headless|slint" -j3 --timeout 300`, 1236 tests; two slices of the filtered index, 565 and 676 slots, 5 tests in both; 2 skipped by their own rule, `test_plugin_abi3_compat` and `test_rtlsdr_live`), run at the build of `860fbc65` (the driver-policy, drive and comment commits came after, see below):

| Slice | Real time | Reds | Class |
|---|---|---|---|
| 1 to 560 (565 run) | 1017 s | `test_driver_policy` | REAL, lane B's fault: fixed (`bb15a43a`), Passed alone and in the final slice |
| | | `test_engine_api_wireb_sigmeas` | load (known flake of the standing rules): Passed alone, 14.81 s |
| 561 to 1236 (676 run) | 1143 s | `test_rtl_tcp_source` | load: Passed alone, 23.94 s |
| | | `test_sdrplay_source` | load: Passed alone, 26.46 s |
| | | `test_diagnostics_free` | STAMP: the binary said `860fbc65`, HEAD was `bb15a43a` ("git HEAD now: bb15a43ab7cc, binary says: 860fbc65b52b"); Passed at the final build (3.61 s) |

**Full GUI gate** (`ctest -L gui -j3`, 419 tests, 211 + 209 slots, 1 in both), run at the build of `bb15a43a`:

| Slice | Real time | Reds | Class |
|---|---|---|---|
| 1 to 210 | 1773 s | `slint_drive_layout-section-overlap-1600` and `-624` | REAL (lane A shortened a note): re-pinned (`6b64b502`), Passed alone |
| | | `slint_drive_lanel-system` | load: Passed alone, 37.94 s; and again in the final slice |
| 211 to 419 | 1817 s | `test_diagnostics` | STAMP (binary older than HEAD): Passed at the final build |

In the GUI gate: `slint_drive_look-stamps` Passed (21.93 s), `s4b-outputs`, `s4b-lockout`, `s4-outputs`, `s4-scan`, `system-language`, `system-usage`, `mapj-layers`, `wg2-xl-fit`, `system-look` and `system-display` (the drives lane A changed) all Passed.

**After the last code commit** (build of `6198f300`): a 21-test slice (`test_diagnostics`, `test_diagnostics_free`, `test_error_paths`, `errpath_gate_selftest`, `test_i18n`, `test_driver_policy`, `test_engine_publish_alloc`, `telemetry_worker`, `test_mustfix_b`, `test_radio_events`, `test_radio_reporter`, `test_radio_reports_engine`, `test_slint_host_mustfixa`, both overlap drives, `system-language`, `lanel-system`, `s4b-outputs`, `s4b-lockout`, `test_engine_api_multi_radio`): `100% tests passed out of 21` (the stamp tests Passed); and a 24-test slice of everything that links the driver policy, the radio setup, the health words and the reports (`driver|winusb|radio_setup|first_run|install|zadig|health|radio_report|radio_event` and three window hosts): `100% tests passed out of 24`. The full headless gate was NOT re-run at `6198f300`: the three later source changes are `driver_policy.cpp` (four data rows, covered by the 24-test slice), two drive scripts and a comment.

Lane tests in the first slice (build `afcce1d3`/`9f423e63`, 70 tests): `test_error_paths`, `errpath_gate_selftest`, `test_i18n`, `test_mustfix_b`, `test_radio_*`, `test_fobos_source`, `test_health_events`, `test_installer_report`, all `test_slint_host_*`, `test_typed_number`, `test_usage_diag_window`, `test_window_handover`, `telemetry_worker`: all passed except the three that this merge then fixed or explained (`test_diagnostics*` stamp, `telemetry_worker` CRLF).

## 8. The two intermittents, cause and runs

**`slint_drive_look-stamps`** (3 of 30 alone at merge 9: `state wf.tick1` read `-` right after choosing a Time stamps mode). Cause, found by reading `paneTick` (not by instrumentation): `rv.fig` is rebuilt EMPTY on every tick (`fig = PaneFigures{}`) and the ticks are filled only on a tick with a NEW spectrum frame; the window publishes them only then (`publishPicture`: `if (fig.newFrame)`), so the picture is never empty. The drive's read-back (`wf.ticks`, `wf.tick1`, `wf.tick1frac`) read `rv.fig` itself, and on a tick with no new frame that is an empty list, which prints `-` ("fewer than two ticks"). **The product never showed an empty first stamp; the read-back read a figure a frame-less tick had just emptied.** Fix: the read-back reads `rv.shownAgeTicks`, a copy kept when the ticks are made (what the window shows). Nothing the window draws changed. Proof, 60 runs alone through the launcher: first batch at the fix build **59 passed, 1 failed**; the failure is a DIFFERENT signature (run 57: `wf.tick1 contains " s" -> it is 2 min` at script line 5, in a run that took 32.7 s against about 21 s, with `gui_desktop_guard` at 0.72 s against 0.03 s: the machine stalled; a gap of seconds in the frame times makes the frame-rate average, and so the row spacing, large for a moment, and the age scale honestly reads minutes); second batch at the final build **60 of 60 passed**. 119 of 120; the signature that was counted (`-`) 0 of 120. Row `M10-look-startup` keeps the stall case.

**`test_engine_api_multi_radio` line 869** (1 of 50 alone: a recording file of the two-radio take still 44 bytes). Cause: a stop hands the take's file to the record finisher's own worker (`core/record_finish.hpp`: the tail is written, the header patched, the file closed THERE; until then the samples sit in the stdio buffer and the file is the 44-byte header the opener flushed), and the test looked after a fixed 200 ms. **The recorder does not leave a header-only file; the test checked before the finisher had run.** Fix: the test waits with `RecordFinisher::drain` (the call the quit makes) before it reads any size. Proof: 131 passes and 0 failures at line 869 (10 by `ctest --repeat until-fail:100` before it stopped, 80 and 40 by direct runs alone, 1 in a slice). **A different failure was found on the way**: run 11 of the ctest repeat ended `SEGFAULT` 13 s in; the Windows Application event log (event 1000, 07:47:38) names the faulting module **`wdmaud.drv`** (Windows' own WinMM audio driver), exception `0xc0000005`, offset `0x4068`: 1 crash in 132 runs, inside the OS audio driver, with no product frame in the record. The test opens the real default audio output (no null-audio hook exists for an engine in this test). This is the "aborted once" of row LX-9; row `M10-multi-radio-crash` carries it with the next step (a null audio output for the test or a post-mortem loop). UNVERIFIED which audio call (open or close) was running.

## 9. Open rows and the config hash

`docs/multi-radio-records/open-rows-2026-11-17.md` refreshed in place. Before (merge 9) 290 open, 50 closed, 340 in all; after **309 open, 54 closed, 363 in all**: screen 53 open / 16 closed, engine 123 / 31, plugins 16 / 0, Worker/site 19 / 6, owner decision 51 / 0, mouse drive 47 / 1. Closed with evidence: `MG6-popup-timer` (lane A), `M8-look-stamps`, `LX-9` (the file-size check; the crash is its own row), `RRA-session-end` (lane E); narrowed: `S4-O-outputs`, `RR1-serial`, `E1-installer-report`, `RRA-live-send`. New rows: `MA-community-row`, `MA-typed-comma`, `MA-needs-data`, `MA-elad-rspduo`, `MA-full-span`, `MA-startmenu`, `MA-erase-sentence`, `ME-privacy-old`, `ME-vocab-copy`, `ME-privacy-l10n`, `ME-output-fail`, `MB-pluto-tx`, `MB-firstrun-launch`, `MB-firstrun-radios`, `MB-cat-radio`, `MB-plugin-start`, `MB-sdrplay-service`, `M10-help-usage`, `M10-driver-policy`, `M10-multi-radio-crash`, `M10-lane-claims`, `M10-look-startup`, `M10-nothing-pushed`.

The real `%APPDATA%/foxsdr/config.json` sha256 after the last test run: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing is pushed.

## UNVERIFIED

- Everything lanes B, E and A mark UNVERIFIED in their own records (no real radio, no real Worker runtime, no installer compile, "Full span" in vi/ru/et/el, the Start-menu entries, the translations, a Slint other than 1.18.1).
- The Outputs rows, the Usage page lines and the Patch note by eye (the drives read values, not looks); the website repository's lane (`site-lane-4`), the Worker and the installer script were not part of this merge.
- GCC behaviour of the merged tree (nothing built on Linux); the d3519455 dangling-reference search was not re-run (the brief did not ask; lane A added no `main.cpp` block of that kind).
- Which audio call was running when `wdmaud.drv` faulted; whether the age scale at start-up should ignore a whole-machine stall.
- The full headless gate was run at the build of `860fbc65` and the full GUI gate at `bb15a43a`; the three source changes after them are covered by the slices of section 7 only.
