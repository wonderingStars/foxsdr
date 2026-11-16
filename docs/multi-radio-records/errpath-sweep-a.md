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

---

# RE-DO after the independent review (review-1.md), as new commits on 96f10873

Result: 41 of the 519 ALLOW lines returned to PENDING in their original group; 186 ALLOW lines kept with a corrected reason; 478 ALLOW remain. Pin 607 -> **648** (base 1126; the pin may rise here, never above the base). PENDING by group now: reports-not-in-function 173 (was 145), unsure 47 (was 34), the other 28 groups unchanged (G3-control-refused 140, O-outputs 55, G2 29, R-record 27, G1 26, G10 16, D-gps 13, F-files 11, T-tx 11, S2 10, G9 10, unmatched 9, A-audio 7, S1b 7, then the small ones).

## Returned to PENDING

| Cluster | Lines | Why |
|---|---|---|
| 1, shared helper with a caller that does not reach the reporter | 24 | `Sha256::init/update/finishHex` 8 (`sha256File` is also called by `loadInventory`, which only adds a note, and `linux_update_host.cpp:119`); `sanitiseFileName` 7 (`parseManifest` at plugin_repo.cpp:1891 pushes a note and skips the entry); `parsePlutoUri` 3 (also `pluto_tx.cpp:290`, whose failure `engine.cpp:5769` only diagWarnf's); `PortAudioSoundCardBackend::open` 4 and `SoundCardSource::openLocked` 2 (`setSampleRateHz` soundcard_source.cpp:982/991 reopens through it; a refusal reaches only the rate message) |
| 2, blanket echo / user-action / headless reasons | 17 | the `say()` echoes: `LiveWorld::say` (host_duties.cpp:965; carries 369/377), `ToolsPublisher::say` (clipboard :280), patch/system/transmit/extend `say`, kit_publish.cpp:1452 notices, slint/main.cpp 1817/2192/2259/3800; engine.cpp `selectSourceById` :6390 (device gone from the last scan) and `applyCommand` :8200 (radio refused the rate); `engine_patch_radios.cpp:334` (I/Q recording would not open); `engine_main.cpp:256` ("settings file damaged, defaults in use"); app_window.cpp:2003 (--patch-file unreadable) and :2439 (frame log not written) |

## Kept with a corrected reason

- `rawiq::parseSpec` (10): now INFORMATION, validation of a typed address, with its callers named: raw_iq_net_source.cpp:287 (open, counted through the open funnel), engine_radios.cpp:130 (FOXAPI_BAD_ARGUMENT to the sender), engine_rawiq.cpp:22 (shown under the source). No false route.
- `discoverHpsdr2` (2): said in those words, dead code in the product (no caller in `src/`, only tests/test_hpsdr2.cpp).
- Open-path family (about 150): the reason now lists every caller of `open()`: engine.cpp:1901 (counted :1641 unless the same busy incident repeats, or the person already chose another source, :1625), engine.cpp:3121 (-> :10171, engine_radio_saved.cpp:212), engine_patch_radios.cpp:593 (-> :329, files excluded), engine_soundcard.cpp:201, and the developer tools tool_modes.cpp:241/:433 and rtlsdr_check.cpp:74 (console output is their whole result). `closeDevice` "keep" lines now say that the kept error may be a run-time one and where it is reported.
- plugin_host `loadOne` reason now cites the gate refusals counted at engine.cpp:3693.

## Caller tables (shared helpers whose reason claims a route)

| Helper (sites) | Every caller in src/ | Reaches the reporter? |
|---|---|---|
| `wantString`, `wantUint`, `parseIndex` (plugin_repo.cpp) | `fetchIndex` :2667 (counts), `fetchRegionalIndex` (counts, this lane), `fetchSiteIndex` (site: by the 0.99.74 design), `loadKeptCopy` (CatalogueBad, engine_store.cpp:134-144/:190-193), `saveKeptCopy` (guard on text just parsed), `engine_store.cpp:87` readLocalCatalogue (test seam for a local index file) | yes, or by design / seam |
| transport (`crackHttpsUrl` ... `httpsGet`) | `fetchText` (only updater.cpp:436), `fetchVerifiedFile` (updater.cpp:505, plugin_repo_helper.cpp:327), `fetchIndex`, `fetchRegionalIndex`, `fetchSiteIndex`, `fetchScreenshot`, `installImpl` | yes; site fetches by design |
| `fetchVerifiedFile` | updater.cpp:505-512 (download / verify counts), plugin_repo_helper.cpp:327 -> install() | yes (the Refused class is not counted by the updater and is unreachable: parseUpdateManifest validated url and hash) |
| `Txn::stage` / `commit`, `looksLikeProgram` | installImpl only | yes (install() :3118-3120) |
| `parseHelperFields` | parseIndex :1708 only | yes |
| `installImpl` | `install()` only (engine_store.cpp:507, `applyUpdate`) | yes |
| `recordInstall` | engine_store.cpp:512, `applyUpdate` | yes (wrapper, this lane) |
| `fetchScreenshot` | engine_store.cpp:771 | yes (wrapper, this lane) |
| `unwrapSignedManifest`, `parseUpdateManifest` | updater.cpp `checkForUpdate` only | yes (:414-441) |
| `checkInApartment` | `checkStoreForUpdates` only | yes (:365) |
| `safeQuery`, `lastOsError`, `openModule` | `loadOne` only | yes (:331-378) |
| `quarantineSlot` / `countFault` / `countOverrun` / `countWedge` / `noteUnloadTimeout` | plugin_fault.cpp internal, hang watchdog | yes, counters -> engine.cpp:9633-9642 -> RecoveryWatch (polled engine.cpp:9599) |
| `resolveDevice` (airspy, airspyhf, hackrf, mirisdr, rx888, aor), `loadFirmwareLocked`, `loadFirmware`, `uploadFirmwareLocked`, `awaitStreamer`, `bringUpLocked`, `handshakeLocked`, `parseRtlTcpAddress`, `selectByArgsLocked`, `openFcdHid`, `fx2::loadImage`, `soapyMiricsRefusal`, `failOpen` | one caller each, inside the same class's `open()` (grep over all of src/) | yes, via the `open()` callers above |
| `parseHostPort` | the open() of aaronia, ascp, hpsdr, hpsdr1, hpsdr2, sdrconnect, spyserver and soapy_remote's address helper | yes, via open() |
| `discoverHpsdr` | hpsdr_source.cpp:295, hpsdr1_source.cpp:201, both in open() | yes |
| `Hpsdr2Source::Net::bind` | hpsdr2 open() and the dead `discoverHpsdr2` | yes / dead |
| `enumerateIsolated`, `sweepEachDriver` | engine.cpp:1402, soapy_source.cpp:698/706, soapy_enum_proc.cpp internal | yes, noteContainedChildDeath at :1213/:1242/:1246/:1413 |
| `markSessionLost`, `enumerateSdrPlayWith` | sdrplay_source.cpp internal | yes, but only on a change of state (`pollSdrPlayTrouble` engine.cpp:11085-11098) |

## Tests (re-do)

`test_errpath_sweep_a`: 49 checks, 0 failed. Added: the cancel FLAG path for the regional list (`cancel()` before the fetch) and for a picture (cancel raised from inside the transfer), and a REAL write failure for `recordInstall` (a valid entry recorded into a "plugins folder" that is a plain file: counted write class; the same for a site's entry not counted). `test_error_paths` 22/0 with pin 648.

Screenshot token decision: kept on `plug_cat`. The existing reporter words are the only vocabulary allowed (no new message kind); none names a picture, and `plug_cat` is the only token for "something the store needed from foxsdr.com could not be had". The cost: one count per failed picture, so a bad network inflates it and a reader of the count cannot tell a catalogue from a picture. A picture-only word is an OPEN ROW for the health lane.

## FAULTS FOUND THAT NOTHING REPORTS (confirmed by this re-do; all left PENDING, no code edited)

| # | Fault | File:line | Owning lane |
|---|---|---|---|
| 1 | a sound card's rate-change reopen refused; only the rate message shows | source/soundcard_source.cpp:982, :991 (via `setSampleRateHz` :967) | S5 / radio-reports (src/source) |
| 2 | an I/Q recording that would not open (patch radio and main path) | engine_patch_radios.cpp:334 (excluded at :328); engine.cpp:10008 | radio-reports (engine.cpp) |
| 3 | a rate the radio refused (API and open) | engine.cpp:8200, :3133, :1699 | radio-reports |
| 4 | a source that is gone from the last scan | engine.cpp:6390 | radio-reports |
| 5 | the clipboard could not be opened | slint/tools_publish.cpp:280 (`say` :231) | screen lane (tools) |
| 6 | the interface half of the settings not served or unreadable | slint/host_duties.cpp:369, :377 (`say` :965) | screen lane (host) |
| 7 | an open result dropped by `asyncOpenStillWanted`; the error is discarded on purpose | engine.cpp:1625 | radio-reports (decide whether a drop after a real failure should count) |
| 8 | the PlutoTx open fails, diagWarnf only | engine.cpp:5769, source/pluto_tx.cpp:290 | transmit owner (verdict only) |
| 9 | an installed plugin file that cannot be hashed, or a manifest that cannot be read, written or parsed | plugin_repo.cpp `sha256File`, `parseManifest`, `saveManifest`, `loadInventory` | health lane (needs a word) |
| 10 | a screenshot that could not be written, the system menu refused | slint/main.cpp:2192, :2259 | screen lane |
| 11 | the settings file damaged at the headless engine's start (status line only) | engine_main.cpp:256 | engine start-up lane |
| 12 | --patch-file unreadable, frame log not written | gui/app_window.cpp:2003, :2439 | classic window (developer options) |
| 13 | a tune refusal after open | engine.cpp:1834 | radio-reports |
| 14 | the plugin strike store cannot be read or written | core/plugin_marker.cpp:804, :887 | health lane |

Count: 14 faults. Counters behind the R verdicts are anonymous ledger tokens, sent only if usage reporting is on; `pollSdrPlayTrouble` counts only a change of state; `countFault` skips the shared slot (plugin_fault.cpp:176); rtl2832u.cpp:164 counts the first recovery per open; the sentinel start failure counts once per run.

## UNVERIFIED (re-do)

The headless and GUI gate slices and the stamp tests are still not run. The individual transport sites inside `httpsGetNetwork` and Linux-only paths were checked by caller table, not line by line. The open-path `open()` bodies rely on the caller proof, not on reading every error assignment.
