# Error-path sweep A (wave 8)

Branch `feat/errpath-sweep-a` from `d4f855c5`, worktree `D:/foxsdr-wt/en-feedplan`, build `D:/foxsdr-build/ep` (moved from C: by the coordinator; the C: folder was removed). Nothing pushed.

## Result

698 sites in the two groups. Pin 1126 -> 607. `reports-not-in-function` 610 -> 145; `unsure` 88 -> 34.

| Verdict | reports-not-in-function | unsure | Total |
|---|---|---|---|
| REPORTS ELSEWHERE (ALLOW, path named) | 384 | 12 | 396 |
| INFORMATION, NOT A FAULT (ALLOW) | 81 | 42 | 123 |
| UNREPORTED FAULT, fixed here | (inside the R count: 3 functions, below) | | |
| left PENDING | 145 | 34 | 179 |

By directory (allowed / left): core 219/26, source 217/131, engine 28/12, gui 22/7, slint 27/1, sink 1/1, engine_main.cpp 5/0, src/main.cpp 0/1.

The reason on each ALLOW line carries the file:line path to the reporter (the reasons are the text in `tests/errpath_allow.tsv`, added column `sweep-a`). Families: plugin catalogue and transport -> `notePluginCatalogueFailed` (`fetchIndex` failed() lambda); install -> `install()` `notePluginInstallFailed`; update -> `noteUpdate*Failed`; kept catalogue -> `Recovered::CatalogueBad`; plugin faults -> `fault::totals()` -> RecoveryWatch; every radio open path (source `open()` reasons) -> `noteRadioFail` at engine.cpp:1641, :10171, engine_radio_saved.cpp:212, engine_patch_radios.cpp:329, engine_soundcard.cpp:201; refused window -> `breadcrumb::noteWindowFailed` -> sentinel; Soapy enumeration children -> `noteContainedChildDeath`.

Design decision relied on (not mine, stated in `plugin_repo.hpp` install() note, 0.99.74): a plugin SITE's failures are not counted, the counter is the official catalogue's. Sites that a site's failure reaches carry that in their reason. `test_errpath_sweep_a` pins that a site index failure is not counted.

## Reports added (all in `src/core/plugin_repo.cpp`, header `plugin_repo.hpp`)

| Site | Reporter | Test |
|---|---|---|
| `fetchRegionalIndex` (transport, parse, refused address) | `notePluginCatalogueFailed` via a failed() lambda, cancel excluded | `test_errpath_sweep_a` `testRegionalIndexIsCounted` |
| `fetchScreenshot` (now a counting wrapper over `fetchScreenshotImpl`) | `notePluginCatalogueFailed`, cancel excluded | `testScreenshotFailureIsCounted` |
| `recordInstall` (now a wrapper over `recordInstallImpl`) | `notePluginInstallFailed(Write)`, official entries only | `testRecordInstallFailureIsCounted` |

Break-it (all three reports disabled): `test_errpath_sweep_a: 39 checks, 12 failed`; restored: `39 checks, 0 failed`. Messages carry no frequency, file name, path, user name, text, position or address (tokens only). Judgement call to review: the picture failure shares the catalogue token, so a failing picture server adds one count per picture.

Allow-list keys of the sites inside the two renamed functions were re-keyed to `...fetchScreenshotImpl` / `...recordInstallImpl`.

## HANDED TO THE OWNING LANE

None: no unreported fault was found in the reserved files that has an existing reporter that fits. Faults seen there are in OPEN ROWS (left PENDING, no code edited in `src/source/`, `engine.cpp`, recorder, store, telemetry, `pluto_tx.cpp`).

## SHARED FILE EDITS

`tests/errpath_allow.tsv` (my 698 lines only; the seed tool rewrote the file with the same order, git diff is exactly 519 replaced lines), `kPendingPin` in `tests/test_error_paths.cpp` (1126 -> 607).

## Gates run

`test_error_paths` 22 checks 0 failed; `errpath_gate_selftest` passed; `test_errpath_sweep_a` 39/0; `test_plugin_cache`, `test_plugin_repo`, `test_plugin_repo_helper` built; all passed, as did `test_plugin_repo_helper`, `test_health_events`, `test_issue_report`. The full headless gate in slices and the GUI gate were NOT run (see UNVERIFIED).

Config hash not touched (no test of mine reads or writes `%APPDATA%/foxsdr/config.json`; hash not re-computed).

## OPEN ROWS FOR THE NEXT LANE

| Rows | What | Lane |
|---|---|---|
| 26 core sites left | local manifest parse/save/inventory (`parseManifest`, `saveManifest`, `loadInventory`, `sha256File`), kept-copy writes, `savePendingRemovals`, `removeCompanions`, remove-file failures, `plugin_marker` strike store: need a reporter word for "a local plugin file or store could not be read or written" | engine/health lane (new Recovered word) |
| 131 source sites | runtime sites (`start`, `transact`, stream restarts, vendor faults, SDRplay late-control, Soapy abandon paths, `closeDevice`), `iiod_client::connectTcp` (shared with rig CAT and TX), RF Explorer, `rig_cat`, `rf64_writer`, `hackrf_sweep` runtime, `pluto_tx` (transmit: verdicts only) | radio-reports lane (G1, G2, G3, G9) |
| engine.cpp 12 | rate refusal after open (3133, 1699), tune refusal (1834), `followInputRate`, plugin UI "not framing" (3360, 4337), control thread wait (11350), `restoreSavedRadios` note | radio-reports lane |
| gui 7, slint 1, main.cpp 1, pa_init 1, process_boot 1, wire_c_portable 1 | GLFW error callback, bundled typeface fallback, language font gap, picture decode failure, cjk font register, classic `service.start` failure, stream-list starve wait, stop-signal install, swallowed pending-system-actions | screen / start-up lane |
| Mouse drive | none of this lane's changes has a window effect | - |

## UNVERIFIED

- The headless gate in slices, the GUI gate and the stamp tests were not run on this tip (a first full build of the engine was not done in this build folder; only the targets above were built).
- `test_plugin_catalogue_app` was not built or run.
- Each R verdict was proved by reading the named lines, not by a test that makes every source's failure happen; the radio open family relies on the single funnel `engine.cpp:1902 -> :1641`.
- Sha256 helper sites are allowed on the ground that a failing crypto provider fails every install; the `loadInventory` re-hash path only adds a note.
