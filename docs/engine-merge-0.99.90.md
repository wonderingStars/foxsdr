# Merging master (0.99.90) into the engine extraction (engine/stage4)

`origin/master` 04cba53a ("CI: stage the software renderer for the windows job's MSIX"), merged into `engine/stage4` at a72a12b (stage 7g: the
headless-binary milestone). The merge base was f28d40bc ("0.99.84: the reload-phase release"), the commit docs/engine-merge-0.99.84.md merged;
what is merged here is master's 29 commits since: releases 0.99.85 to 0.99.90, 140 files. House discipline as in the 0.99.84 ledger
(section 1 there): every commit classified BEFORE the merge, the code that moved re-expressed where it lives now, the same semantics, no silent stub.

CLASSES: **carried** = merged (or applied by patch to the same function) with no change of place; **re-expressed** = its code moved on this
branch and master's behaviour was put where the machinery lives now (the destination is named); **n/a** = release notes, release hashes, a
README line. **Stubbed: none new.** Two commits have a half that lands in the window's RADIO SETUP page, which has been a stub since the
0.99.84 merge (ledger id RADIO-SETUP, `src/gui_pending/app_window_radio_setup.cpp`); the master text of that half merged into the pending
file and is listed in section 3 as the page's worklist. Nothing was stubbed to make this merge, and nothing the window shows today was removed.

## 1. The ledger: one row per commit

| # | Commit | What | Class | Where it lives now |
|---|--------|------|-------|--------------------|
| 1 | 8820bab4 | SDRplay: samples the service itself lost, counted from its sample numbering | carried | source/sdrplay_source.* (test_sdrplay_source) |
| 2 | 9ceeb4f4 | 0.99.84 release hashes | n/a | docs |
| 3 | 80f21271 | SDRplay: a stranded Link survives static destructors | carried | source/sdrplay_source.* (test_sdrplay_strand_exit, new) |
| 4 | 2fc502bd | key chords stay out of read-only text; every rate change logged; service-gap count whole | carried | core/pipeline, core/diag_log, source/sdrplay_source, gui/key_bindings.* (pure); the window's one-line `dispatchKeyBindings` change applied by patch to the same function (test_key_bindings, test_rate_follow, test_upload_scrub) |
| 5 | a32f5a32 | a USRP UHD cannot identify for want of firmware images is named | carried + re-expressed | core/radio_setup, source/soapy_enum_proc, soapy_log_bridge, engine/device_scan_plan.hpp (git followed the rename): carried. The scan: `Engine::scanSoapy` / `pollSourceAsync` now call `enumerateIsolated` and keep `uhdImagesMissing` and "a USRP was on the bus" (`soapyUhdNoImages_`, `soapyUsrpOnUsb_`); published as `fact.source.uhdNoImages` / `fact.source.usrpOnUsb`; the Source section draws `uhdImagesMissingHint` from them. The radio-setup page's images finding: into the RADIO-SETUP stub (section 3) |
| 6 | 4047a085 | MSIX manifest declares every catalogue language | carried | installer/msix |
| 7 | c8031b6e | 0.99.85 bump and notes | carried | CMakeLists.txt, README.md, docs/release-notes-0.99.85.md |
| 8 | 4fcb9500 | 0.99.85 hashes | n/a | docs |
| 9 | 7700656c | pipeline ring drops: a log line once a minute, the total in the bundle | re-expressed | `Engine::pollDspMinute` / `closeDspMinute` (called in `pumpAudio` after `pollAudioHealth`, own clock); the bundle's two counters: the pass publishes them (`core::setRingCounters`, diag_report.*) and the window's `currentDiagnosticsBundle` reads them (`core::ringCounters`); core/pipeline, core/diag_report carried. test_dsp_drop_minute_app converted (section 4) |
| 10 | c301ed05 | 0.99.86 bump | carried | |
| 11 | 59c381bb | 0.99.86 hashes | n/a | docs |
| 12 | e4fae21d | a window Windows calls Not Responding is a freeze, reported with its stacks | carried | core/hang_watchdog, core/sentinel (test_hung_window_excuse, new; test_sentinel) |
| 13 | 0f5666df | 0.99.87 bump | carried | |
| 14 | 03ba0382 | 0.99.87 hashes | n/a | docs |
| 15 | ac812ddf | README links and plugin count | carried | README.md |
| 16 | 1fe38eef | usage report: a native radio's model is its family | carried + re-expressed | core/telemetry, telemetry-worker: carried. `Engine::nativeLabelFor` falls back to `nativeFamilyModel(kind)`, never the args; `Engine::currentDiagContext` writes `scrubModel(deviceModel_)` (master did both in AppWindow, which no longer holds them) |
| 17 | 942e8c27 | RADIO SETUP installs the RTL-SDR's driver itself (`--install-winusb`) | carried + re-expressed | core/winusb_install.*, core/radio_setup, CMake `newdev` (now on `cascade_engine`): carried. The helper mode that main() dispatched: `runHelperModes` in engine/process_boot.cpp, between the sentinel and the enumeration helper where main() had it, kind-blind like the other helpers. The page half (INSTALL DRIVER key, `DriverInstallJob`): into the RADIO-SETUP stub (section 3) |
| 18 | 2d5b20b6 | updates: stable or every release (beta) | carried | core/config, core/updater carried; the window's Updates section, `updateChannel_`, `startUpdateCheck`, the config write-back (`diagnosticsJournal`, beside `updateCheckEnabled`) applied to the same functions. The headless engine round-trips the field through its `loadedCfg_`: test_engine_main_config_roundtrip seed gained `updateChannel` (section 4) |
| 19 | a230394a | 0.99.88 bump | carried | |
| 20 | d890f94a | 0.99.88 hashes | n/a | docs |
| 21 | 970f16e1 | SDRplay: a START after a device failure streams again | carried | source/sdrplay_source.* (test_sdrplay_source, test_sdrplay_stall) |
| 22 | b694899c | sentinel: a start-up that had already hung is reported when the session was closing | carried | core/sentinel (test_sentinel); the sentinel is still armed LAST in `bootProcess` (step 13), untouched |
| 23 | 953fb856 | radio setup: the probe's tables outlive exit | carried | core/radio_setup, source/rtlsdr_source (test_radio_setup_exit, new) |
| 24 | 2de59ff1 | 0.99.89 bump | carried | |
| 25 | 0d9ede5b | 0.99.89 hashes | n/a | docs |
| 26 | 9a2dcb6a | a software OpenGL renderer; relaunch with FOXSDR_GL=software | re-expressed | see section 2 |
| 27 | 7973eb65 | 0.99.90 bump | carried | |
| 28 | bd9936bf | 0.99.90 hashes | n/a | docs |
| 29 | 04cba53a | CI: stage the software renderer for the MSIX | carried | .github/workflows/build.yml (merged beside 7g's engine-tarball steps; no overlap) |

Totals (by the table's column): **carried 17** (the 6 version bumps and 11 fixes), **carried + a re-expressed half 3** (5, 16, 17), **re-expressed 2**
(9, 26), **n/a 7** (the release-hash notes) = 29. **Stubbed: 0.** (Commit 18 is carried: the window's own Updates section is edited in place.)

## 2. Commit 9a2dcb6a (software renderer): what is where

Master's change had four parts. The boot-side parts are the point of the brief; the GL position relative to crash arming is kept.

  * **The mark a relaunched process takes** (main(): `takeRelaunchMarker`, then `primaryInstance = claim || relaunched`, log line "started again by
    process N"): in `claimInstance` (process_boot.cpp step 4, Host only, after `armDiagnostics` and before `parseFlags`, exactly master's place in main()).
    The engine library may not include gui/, so the one function boot needs moved to `core/relaunch_marker.{hpp,cpp}`; gui/software_gl.hpp keeps
    master's names (`kRelaunchEnv`, `takeRelaunchMarker`) as forwards, so master's tests compile unchanged.
  * **The relaunch itself** (after `app.run()`, the application object destroyed first, the watcher switched off, `relaunchAndWait`, the exit code
    passed through): main.cpp, the window application's own entry point, in the same shape. One addition: the settings-file lock (7c) is released
    before the second process starts, because the second process is the window application and takes it for its own run; held across the wait it would
    have found it "held by another FoxSDR program" and carried on without.
  * **The window's side** (`FOXSDR_GL`, `FOXSDR_TEST_WINDOW_FAIL`, the loader, the version check, the hand-over, the backend-failure reports): AppWindow::run
    and its helpers, applied to the branch's `run()` (the window is still the window's).
  * **Build**: `/DELAYLOAD:opengl32.dll` and the pinned Mesa staging are on the `cascade` target inside `if(CASCADE_BUILD_INTERFACE)`. The engine
    binary imports no OpenGL (test_engine_main_links_headless) and has no softgl\ directory. configure downloads the pinned archive once and checks its sha256.
  * **THE ENGINE KIND GAINS NOTHING**: the mark is read after the Host-only guard of the claim step; no file of src/engine/ and not engine_main.cpp names
    the renderer, the relaunch, FOXSDR_GL or the failure seam. tests/test_process_boot_softgl.cpp (new) holds this from the text and by running the boot
    as each kind: an Engine given the mark neither takes it nor becomes the primary instance; a Host given it is the primary instance although the name is
    held, and the variable is cleared. Break-it: the mark moved above the Host guard, an engine file including software_gl.hpp, and `|| softglRelaunched`
    deleted each turned it RED (md5 of every restored file checked equal).

## 3. What landed in the RADIO-SETUP stub (the page's worklist for its re-expression)

The page is `src/gui_pending/app_window_radio_setup.cpp` and is not built. Master's edits to it (commits 5 and 17) merged into that file cleanly and are
the worklist: the INSTALL DRIVER key and `radioSetupInstallDriver` / `radioSetupPollDriverInstall` / `DriverInstallJob`, the images finding
(`inv.uhdImagesMissing = soapyUhdNoImages_`, now `fact.source.uhdNoImages`), and these AppWindow members that master's header gained for it and this
branch's did not (declare them when the page is re-expressed): `std::shared_ptr<DriverInstallJob> driverInstall_`, `std::string driverInstallStatus_`,
`radioSetupInstallDriver(const std::string&)`, `radioSetupPollDriverInstall()`. The core halves (core/winusb_install, core/radio_setup and their tests)
are live and pass. Visible consequence today: a person on this branch has no INSTALL DRIVER key and no radio-setup images finding; they already had no
RADIO SETUP page. The Source section's own images line (commit 5) IS live.

UPDATE 2026-10-26, stub batch B8 (docs/engine-stage4.md 6.22): the worklist is done. The page is src/gui/app_window_radio_setup.cpp (git mv), with the INSTALL DRIVER key (`radioSetupInstallDriver`, `radioSetupPollDriverInstall`, `DriverInstallJob`, the two members `driverInstall_` and `driverInstallStatus_`) and the images finding (`fact.source.uhdNoImages` into the page's inventory, gui/radio_setup_inventory.hpp). test_radio_setup_app had no scenario for the key; one was written and holds it (FOXSDR_FAKE_DRIVER_INSTALL, a bounded-run seam), and `bootProcess` was not touched.

## 4. API, tests, and the traps

  * **Facts added** (public facts of the table, not in the 0.3 header): `fact.source.uhdNoImages`, `fact.source.usrpOnUsb` (hashed into the publish
    fingerprint from the two members). No op, no header change. (docs/api-0.3-additions.md section 9.2 and the 8.10 table list them.)
  * **Process-wide values**: `core::setRingCounters` / `ringCounters` (diag_report.*), the engine's pass writing, the window's bundle reading, drop count first.
  * **Tests converted, never weakened**: test_dsp_drop_minute_app (the access goes through the pass seam; section 5 reads engine.cpp where master read
    app_window.cpp; section 4 runs the poll first because the bundle shows what the pass published; master's expectations are untouched).
    test_engine_main_config_roundtrip: its seed must leave no config field at its default (a saturation check) and found the new `updateChannel`;
    the seed gained `"beta"` and the engine preserved it, which is the property. test_process_boot_order: the helper list gained `winusb::kHelperFlag` in
    position. test_engine_main_helpers: the new helper's refusals and its "only as argv[1]" rule, through foxsdr-engine. tests/tsan.cmake: master's one addition
    (test_dsp_drop_minute_app) beside test_audio_lead_app, edited as bytes (CR CR LF).
  * **Trap found by the suite, not by reading**: after the merge test_scope_window and test_patch_graph_draft_input (x3 each, 6 reds) failed because the
    tests find a function's end as "\n}\n" and the merge's checkout wrote src/gui/app_window.cpp/.hpp with CRLF where the worktree had held them as LF.
    Not a code change: the two files were put back to LF (the index is LF either way) and the six passed. Any scan test that reads a window file will fail
    on a CRLF working copy; the next merge should check `git ls-files --eol src/gui/app_window.cpp` before running the suite.
  * **Conflicts** (8 files): CMakeLists.txt (2 hunks), tests/CMakeLists.txt (1), tests/tsan.cmake (whole-file, CR trap), third_party/THIRD_PARTY.md (1),
    src/core/lang_assets.hpp (generated: rebuilt with tools/embed-lang.py from the merged catalogues), src/main.cpp (the branch's 55-line main taken;
    master's three edits re-expressed per section 2), src/gui/app_window.cpp and .hpp (the branch's taken whole; master's 23 + 7 hunks re-applied by `git apply --reject`:
    6 + 2 applied, the other 17 + 5 placed by hand - section 1 names where). The 33 catalogue JSONs merged without conflict.
  * **i18n, three-way**: `tools/i18n_keys.py --list`: base f28d40bc 1994 keys, branch a72a12b 1837, master 2010, merged 1853. Master adds 16 keys
    to its base (the channel radios and sentence, the UHD images line and finding, the INSTALL DRIVER page); the merged list is the branch's list plus
    exactly those 16 (`comm` of the two difference sets: identical) and loses nothing. The per-catalogue report (`tools/i18n_keys.py`, no flag) for the branch
    and for the merge differs only in "N translated" (+16 in each of the 33); the 2 missing and 162 stale keys are the same keys in both. test_i18n is red
    for exactly the reason it was red.

## 5. Status at the end of this pass

See docs/engine-stage4.md section 6.21 for the build, the suite and the checks; the red set is 54 merge_pending placeholders and test_i18n, as before.
