# Engine merge lane 6 (`engine-merge-6`): eight branches into `engine/multi-radio` (2026-11-16)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `544aeca3` (clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, `FOXSDR_SLINT_CLIP_PROBE=OFF`, Release, `--parallel 4`), scratch `D:/foxsdr-build/m6-scratch/`. Every GUI-capable test ran through the hidden-desktop launcher, `FOXSDR_FRAME_CAP_HZ=60`; `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set; no radio opened; nothing pushed, downloaded or deployed. The owner's `foxsdr-slint.exe` under `D:/foxsdr-build/show/` was not touched. Real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

## 1. The merges (merge commits, no rebase, no squash)

| # | Branch (merged commit) | Merge commit | Conflicts and how each was resolved | After-merge check |
|---|---|---|---|---|
| 1 | `feat/patch-faces` (04ffba5e) | da50c44c | `src/core/lang_assets.hpp` only: regenerated with `tools/embed-lang.py` (33 catalogues) | build exit 0; `patch|i18n|house_rules|thread_inventory|ledger` + the patch-faces drives and host tests: 65 of 65 |
| 2 | `feat/window-gaps-2` (a998c67e) | fe436e74 | `src/slint/ui/main.slint` export list: kept both (lane's `SdrPlayDialogState/Model`, `PointerLedger`, `MeterPanel`, themes `GlassCockpit/DaylightLab/FieldRadio` AND the engine side's `Handover`); `lang_assets.hpp` regenerated; `docs/slint-drive-coverage.md` regenerated with `tools/slint_drive_ledger.py` (753 asserted / 117 driven / 1509 not drivable / 0 unclassified of 2379) | build exit 0; lane + subset 137: 134 pass; `test_patch_dest_async` (load flake), `slint_drive_system-language` passed alone; `slint_drive_system-look` red alone (see 4) |
| 3 | `feat/radio-reports` (tip 7082a68d, as the orchestrator moved it from 6ae6072b) | 8de0a922 | none textual (`telemetry-worker/worker.js` auto-merged with engine fixes 7 and 8). Two merge consequences fixed, see 2 | `node --test` in `telemetry-worker/`: 107 of 107; ctest `health_events|vocab|worker|telemetry|i18n|house_rules|thread_inventory|ledger`: 11 of 11 |
| 4 | `feat/window-gaps-1` (77af293d) | b5a71b7c | `main.slint`: both sheets kept (the engine fixes 7 `Handover` question and the lane's diagnostics-offer and link sheets, each closed properly); `test_slint_drive_run.cpp`: both header keys kept (`patchdoc=` and `rootfile=`/`appfile=`); `lang_assets.hpp` and the coverage ledger regenerated (790 / 116 / 1473 / 0) | build exit 0; 141 of 141 (gaps1 drives and tests, `duties`, `wg2`, `system`, `signal`, `patch`, i18n, house rules, thread inventory, ledger) |
| 5 | `feat/window-gaps-3` (4ec3e364) | 05bff845 | only the two generated files, regenerated (797 / 116 / 1466 / 0) | build exit 0; 105 of 105 (`wg3`, `extend`, `kit`, `fitted`, `store`, `patch`, i18n, house rules, thread inventory, ledger) |
| 6 | `feat/window-gaps-4` (b9c23f0e) | d40cf54f | `src/slint/CMakeLists.txt`: both source lists kept; `main.slint`: the `spectrum.slint` import takes the lane's `AirbandChannel` and keeps lane 1's `Gaps1` import, the export list is the engine side's plus `AirbandChannel, MapExtras, MapIcons`; generated files regenerated (806 / 118 / 1455 / 0). The patch-faces lane reuses spectrum/waterfall/map pieces only in `patch_page.slint` / `patch_faces.cpp`, which this lane did not change | build exit 0; this lane's six drives (`wf-gap`, `airband-spectrum`, `map-bar`, `airband-dial`, `globe-satellites`, `survey-3d`), the three patch-faces drives and host tests, kit, spectrum, waterfall, map, scene3d, survey3d, i18n, house rules, thread inventory, ledger: 100 of 100 |
| 7 | `feat/linux-port` (2edb12c7) | adf7bbab | none textual; see 3 for the two test consequences | build exit 0; the 27 registered tests the branch touched pass; `test_thread_inventory` (CAT row 3 to 5) passes; the five formerly "false" tests pass alone |
| 8 | `feat/errpath-gate` (6fd68f56) | d4f855c5 | none textual; allow-list re-seeded, see 5 | `test_error_paths` and `errpath_gate_selftest` pass |

Duplicates: none found. The one hand-off the brief names is closed: the "did not close normally" dialog's Copy key goes `Gaps1.diag-copy` -> `env.copyDiagnostics` -> `SystemEvents.diag-copy` -> `SystemPublisher::copyDiagnostics` -> `foxsdr::slintdiag::copyDiagnostics` (`diag_copy.hpp`): one effect, no second copy of it existed; `gaps1-diag-offer` asserts `gaps.diagcopies` 0 to 1 after the key (the clipboard of a bounded run is a stand-in that takes nothing).
Window gaps 3's last pass had moved `wg3-store-page` to read the SECOND picture's caption. The first caption was asserted only in `test_slint_host_extend` (`screenshots[0].caption == "The map view"`), in no drive; `wg3-store-page` now also asserts `Picture number 1 of the page` while the strip is at frame 0 (it passes).

## 2. Worker vocabulary and the radio-reports consequences (merge 3)

The merged health vocabulary in `telemetry-worker/worker.js` holds 18 families, 171 qualifier words, 387 tokens: the reason word `control` of engine fixes 8 (in `radio_fail`) and the four companion fault words (`companioncrash`, `companionstall`, `companionbreak`, `companiongaveup`, in `recovered`) are both there. 387 equals the `7 + 234 + 48 + 54 + 44` `test_health_events` pins: no expectation changed.
Two faults the merge exposed, fixed: (1) the contract's reason classes (`radio-journal-spec.js`, its generated `radio-journal-spec.json`, `docs/radio-reports-contract.md` line 98) lacked `control`, so the Worker suite's contract-versus-vocabulary test failed and a radio-open report with that reason would have been refused: `control` added before `other`, fixtures regenerated with `make-radio-fixtures.mjs`; (2) `radio-reports.test.mjs` split `MANIFEST.sha256` on LF only and failed on a CRLF checkout (autocrlf): it splits on CRLF or LF now, its hash comparison unchanged.

## 3. Linux merge: the converter findings and the five "false" tests (merge 7)

`test_airband_data`, `test_converter_call_sites`, `test_page_resize_id` and `test_png_decode` now run with the source root as working directory and find the tree. Re-run alone on the merged tree: `test_airband_data` Passed, `test_converter_call_sites` Passed, `test_page_resize_id` Passed, `test_png_decode` Passed. `test_band_plan` (the fifth of the old list) has the same walk-up-from-the-working-directory fault and the Linux lane had not fixed it: it failed alone (`test_band_plan.cpp:208 !shipped.empty()`); it joins the same `WORKING_DIRECTORY` loop in `tests/CMakeLists.txt` and Passed. No test of the old list is still red.
The four converter findings (`engine.cpp` status record about 4952, FILE_SEEK about 7415, FILE_LOOP about 7427, `wire_a_transport.cpp:16`) were each read: they call `rawSource()` only to downcast to `IqFileSource` and read or move the recorded file's play position, loop marks or speed. None reads or tells the radio a frequency, so none is a wrong-frequency defect (nothing to fix). They are in the test's own allow list (`kAllowed`, with a reason each), and the scan's `rawSource` branch now consults that list. Break-it: with the `wire_a_transport.cpp` entry renamed the test printed `FAIL wire_a_transport.cpp:16 bypasses the converter ... 1 bypass the converter` and `bad == 0` failed; restored: `16 allowed ... 0 bypass`, `5 checks, 0 failed`.

## 4. Gate results

Final build: full `cmake --build` exit 0 at `3b60af46`, and again at `961e76be` (the record's first commit; exit 0). The later commits are drive scripts and records only.

Headless gate, `ctest -L "headless|slint" -j3 --timeout 300`, hidden desktop, three slices (1 to 400, 401 to 800, 801 to 1200): 405 + 400 + 366 = 1171 tests; 12 red, each re-run alone:

| Red in the gate | Class | Evidence |
|---|---|---|
| `test_slint_tools` | REAL, merge consequence, FIXED (01a132b5) | alone `Failed` at lines 883, 899, 900 (7 checks): window gaps 1 widened the Markers sheet (620 to `min(700, stage - 24)` = 636 px in the 660 px stage) and the Noted column (76 to 150 px, `MarkerCols.dropped-w`) and did not run the test. Pinned geometry recomputed from those two inputs (sheet width, note field x +74, Tune/Bookmark keys x +16, remove and close click points); every assertion kept; Passed, 371 checks |
| `test_engine_api_schedule`, `test_engine_api_wireb_sigmeas`, `test_engine_api_zoom`, `test_engine_main_config_roundtrip`, `test_discord_presence`, `test_failure_files`, `test_pass_control`, `test_plugin_unload_handshake`, `test_snr_tap`, `test_plugin_store_window_stepped`, `test_slint_digits` | load flakes | each `Passed` alone in one `-j1` run (3.15 to 66.2 s); open row MG6-load-reds |

None of the five old "known false failures" was red. `test_i18n` Passed in the gate and again at the end.

GUI gate, `ctest -L gui -j3`, three slices (1 to 130, 131 to 260, 261 to 390): 131 + 131 + 124 = 386 tests; 9 red (a tenth, `slint_drive_system-language`, found red under load in the last subset runs, is the next paragraph):

| Red | Class | Evidence |
|---|---|---|
| `slint_drive_layout-section-overlap-1600` and `-624` | REAL, merge consequence, FIXED (55adafe9) | `expect "Read the pitch now" bottom <= "2028" -> FAILED: it reads "2058.00"`. Window gaps 2's Signal strength bar (24 px + 6 px spacing) sits in the always-open Radio section: everything below moved down 30 px. Both bounds recomputed to 2058 (and `Sinks top >= 2058`, which still proves the next header starts at or below the key); Passed |
| `test_health_app` | REAL, FIXED (55adafe9) | failed alone: `health: the real application left reached.launched=1,...,slow.startup.250ms=1` and `test_health_app.cpp:1167, 1220, 1623, 1635, 1641`. On a busy PC the binary's start takes over 250 ms and the application counts `slow.startup.250ms`; the helper that sets stages and the machine's first-run findings aside now sets `slow.startup.*` aside too (held by `test_health_events` and `test_health_paths` with a controlled clock) |
| `slint_drive_tools-airband` | REAL timing race, FIXED (b631ee4f) | found in the last subset run, failed alone 2 of 3: `state fact.airband.squelchDb < -55 -> FAILED: it is -55.0`. The scroll is eased over time; 3 frames after it the Squelch slider read y=641 and came to rest at y=583, so the drag missed. The dump now comes 30 frames after the scroll; alone after: 5 of 5 Passed |
| `test_diagnostics` | stamp (binary older than HEAD) | see the stamp line |
| `slint_drive_system-language` (red in the earlier merge-2 run and in later subset runs, not in the three gate slices) | REAL crash, test race FIXED in the drive script (this commit) | exit -1073741819 (access violation) after `click "Not set"`; crash report `diag/crashes`: `slint_cpp.dll+0x53D03E`, signature BE59150DB2C51A2B; the stack symbolised through the PDB (DIA) reads `LineInput_root_939::update_timers` timer lambda -> `Callback::set_handler` lambda (`slint_timer.h:83`, `slint_callbacks.h:82`): the 1 ms focus-grab timer of the popup field (`system_bank.slint` LineInput, documented in docs 6.47) firing while the popup is closed by the next click. The script clicked "Not set" one frame after opening the list. Alone with `--repeat until-fail`: failed on the 3rd run (twice, same signature); the click now waits 6 frames as the first open does: 15 of 15 Passed. Product caveat in the open rows (MG6-popup-timer) |
| `slint_drive_lanel-system`, `test_decoder_settings_fold_app`, `test_slow_frames_app` | load flakes | `Passed` alone (50.2 s, 19.8 s, 34.9 s) |
| `test_diag_hang` | load (Timeout 300 s in the gate) | `Passed` alone in 104.35 s: open row MG6-diag-hang |

THE TWO "INTERMITTENT" TESTS (the brief asked for five alone-runs each and a cause if either fails alone on a quiet tree; the clip probe was OFF in this build folder, `FOXSDR_SLINT_CLIP_PROBE:BOOL=OFF`):
* `slint_drive_system-look` (also red once more inside a loaded batch of 15 at `961e76be`, 69 s, text not captured; alone 5 of 5 Passed on that build): 4 of 5 alone-runs FAILED on the merged tree before the fix (`look.width == 1600 -> FAILED: it is 800`), so it is a code fault and not the probe. Cause: when the interface size changes and the window is not maximised, `main.cpp` asked the window for "the logical size stays", the physical size times new scale over old. At 200 % that asks for 3200 x 1800, which the system clamps to the 1600 x 900 the window already fills; going back to Auto the request is 800 x 450, which IS honoured, so the window ended at half its size (a user with a screen-filling window would see the same). The hidden desktop's window reports `is_maximized` 0 (a temporary log line showed `window was 1600x900 px at 2.00, maximised 0, now 1600x900`, then `window was 800x450 px at 1.00` at the next step); whether the late 800 x 450 landed before the check made it pass. Fix (d98dd046): `ScaleFollower::sizeAfter` remembers the size before the first grow and asks for it again on the way back when the window is no bigger than the grow asked for; unit test `clampedGrowRoundTrip` in `test_slint_host_look` (640 checks), break-it (memory switched off): 4 checks fail, restored 0 failed. Five alone-runs after the fix: 5 Passed (20.0 s each); before: 4 Failed, 1 Passed.
* `test_health_app_stepped` (and `test_health_app`): see the `test_health_app` row; five alone-runs of both after the fix: 10 of 10 Passed, no `slow.startup` in any output.

Stamp line: `test_diagnostics`, `test_diagnostics_free` and the `issue_report` family were run in the last subset (`diagnostics|issue_report|stamp|system|look|layout|health|tools|error_path|errpath|i18n|ledger|thread_inventory|converter_call|band_plan|airband_data|page_resize|png_decode|wg3|patch`, 144 tests) on the build made at `3b60af46`: 143 passed, the one red was `slint_drive_tools-airband` (fixed after, above). After the rebuild at `961e76be` the stamp tests were run again: `test_diagnostics` Passed (63.52 s), `test_diagnostics_free` Passed (106.33 s), `test_issue_report` Passed, `slint_drive_system-diagnostics` and `slint_drive_wg2-diagnostics` Passed. The commits after that (the language drive script and the final record) change no source; a stamp test run on that build before another rebuild reads the older commit, as engine fixes 8 recorded.

## 5. The error-path re-seed (merge 8)

Scanner on the merged tree: 3252 sites, 69 report, 2057 allowed, 1126 PENDING. The pinned PENDING count was raised ONCE with `--allow-raise`, 1124 to 1126.
New sites brought in by the merged lanes, eleven:

| Lane that added it | Site | Verdict |
|---|---|---|
| window gaps 1 | `gaps1_logic.cpp LinkFlow::resolved` (two error texts) | ALLOW, ui-text (the link dialog shows it) |
| window gaps 1 | `gaps1_publish.cpp Publisher::Impl::publishLink` | ALLOW, plumbing |
| window gaps 3 and 1 | `host_duties.cpp storeCardFor`, `HostDuties::pollStore` | ALLOW, ui-text / plumbing |
| window gaps 1 | `main.cpp` stderr summary lines #7 and #8 | ALLOW, developer output (the old "system menu needs the native handle" line is gone with its code) |
| window gaps 2 | `sdrplaydiag_logic.cpp SdrPlayDiag::run` and `::poll` | ALLOW, user-action (the probe the person started, shown in the dialog they opened) |
| engine fixes 8 (D3) | `airspyhf_source.cpp readCalibrationLocked`, two warnings (stored crystal trim, stored front-end options refused; carrying on) | PENDING:G3-control-refused (the 2 that raised the pin) |

The four older `main.cpp` stderr lines (#3 to #6) stay PENDING:unsure as seeded. New PENDING sites by lane: engine fixes 8: 2; every other lane: 0. After the scale fix the gate was run again (a log line I had extended changed an allow key and turned it red: the line is back to its old text) and Passed.

## 6. Pinned numbers and expectations changed (every one)

| Where | Was | Now | Inputs |
|---|---|---|---|
| `docs/slint-drive-coverage.md` (generated) | 2379 rows, 716 asserted at engine merge 5's tip | 806 asserted / 118 driven / 1455 not drivable / 0 unclassified | regenerated after each merge: 753 / 117 / 1509 (after window gaps 2), 790 / 116 / 1473 (gaps 1), 797 / 116 / 1466 (gaps 3), 806 / 118 / 1455 (gaps 4) |
| `test_error_paths` `kPendingPin` | 1124 | 1126 | the two airspyhf warnings of engine fixes 8 (section 5) |
| `test_slint_tools` Markers geometry | sheet 620, note x 262, keys x 452, remove x 594-18, close x 620-17 | sheet 636, note x +74, keys x +16, click points moved with them | window gaps 1: `min(700, 660 - 24)` and `MarkerCols.dropped-w` 76 to 150 |
| `layout-section-overlap-1600/-624` drives | 2028 | 2058 | window gaps 2: Signal strength bar 24 + 6 px |
| `withoutStagesOrMachine` (test_health_app) | set aside stages and `setup.*` | also `slow.startup.*` | not a number: a timing count (section 4) |
| `test_thread_inventory` CAT row | 3 | 5 | made by the Linux lane (both copies of the TCP service); passes |
| `updatePlanFor(false, false, true, true)` / plan-line count | `StoreDeferred` / 4 | `StoreAsk` / 5 | made by window gaps 1, accepted by the orchestrator |
| `test_sigmeas_bandwidth` 3 dB width bound | 3 % | 8 % | made by the Linux lane (40-seed measurement in the comment), accepted by the orchestrator; open row LX-sigmeas |
| `test_health_events` token total | 7 + 234 + 48 + 54 + 44 = 387 | unchanged | the merged vocabulary counts 387 |

## 7. Open rows

`docs/multi-radio-records/open-rows-2026-11-16.md`: every merged lane's table plus engine-fixes-8's (engine-fixes-7 has no table of its own; its rows are in engine-fixes-8's), grouped by the kind of lane: screen 20 open / 6 closed, engine 57 / 9, plugins 11 / 0, Worker/site 9 / 3, owner decision 13 / 0, mouse drive 22 / 0; 132 open, 18 closed with evidence, 150 in all. Row P1 (window gaps 4: Airband marks, gap break and range rail on no pane in a multi-radio layout) is marked NOTICEABLE at the top.

## UNVERIFIED
The machine translations of all new strings; every real-hardware, real-pointer and real-Store row in the open-rows file; whether the clip-probe build also passes `slint_drive_system-look` after the scale fix (only the probe-OFF folder was run); whether the full-screen clamp also happens on a physical desktop (the logic is unit-tested, the clamp seen only on the hidden desktop); the Worker and site, none deployed.
