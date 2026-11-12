# Engine merge lane (`engine-merge`): the engine lane branches and the screen branches merged into engine/multi-radio

**MERGED TIP (code): `b33a578b3bcc7467c978612fea4933a85fe8206c`, `git describe` = `v0.99.108-525-gb33a578b`.** The commit that adds this file sits on top of it and
changes only this file; the branch head is that commit. Screen lanes J and K and the release lane start from the branch head. Version strings: none changed
(`project(cascade VERSION 0.99.108)` as before). Nothing pushed.

Date 2026-11-12 (start 07:55). Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release), start tip `d3630d78` (clean).
Brief: `D:/foxsdr-build/prep/queued-briefs/remaining/engine-merge.md` with its section ORCHESTRATOR ADDITIONS. `feat/engine-serial-a` is NOT merged (being built in
`mr-engine1`; it comes by a follow-up message).

## 1. The merge table (22 merge commits, one per branch, `git merge --no-ff`)

Every tip below equals the tip the orchestrator named. After each merge: build (twice where the merge added test files, because the VS generator only learns new
globbed test targets on the second build) and the tests the merge added, headless, plus `test_thread_inventory` / `test_health_paths` where a thread or health
table could change.

| # | Branch (tip) | Merge commit | Conflicts and how they were resolved | Tests run after (real results) |
|---|---|---|---|---|
| 1 | feat/engine-wire-2 (5a45640f) | 1b853840 | none (a straight descendant of d3630d78). Brings in W1 (2b2ede72) and what W2 merged: recording-models, core-formats, net-map-models, net-outputs, radio-sources, array-maths, unblocked-desk, platform, kiwi-client, web-bookmarks, slint-gaps (b9e22d3f, so it was not merged again), engine-wire (W1) and the PortAudio list lock | 30 W2 tests (`engine_api_(lock\|macros\|recname\|private\|markerlimit\|oirt\|merge\|source_kinds)`, plugin_events, pa_open_list_race, pipeline_oirt, thread_inventory, health_paths, plugin_misuse, upload_scrub, linux_update, radio_setup, winusb, driver_policy, tool_benchmark, process_boot_kind, freq_import): `100% tests passed out of 30` (`test_engine_api_merge` Passed 21.54 s) |
| 2 | feat/screen-s1 (b4bdcd08) | 5ae1aa92 | `src/slint/ui/main.slint` line 51-52, the exported-types list: HEAD's `HighContrast` and S1's `MarkTab, LayoutRow, LayoutsModel, ExportModel` all in the line. `src/core/lang_assets.hpp` (generated, 146 hunks): regenerated with `tools/embed-lang.py`. `docs/slint-port-tick.md`: both blocks kept. `docs/slint-drive-coverage.md`: regenerated: `ledger: 2226 rows, 595 asserted, 94 driven, 1537 not drivable, 0 unclassified` | 24 tests (slint_host_layouts/marks/export/persistence/input, waterfall, i18n, house_rules, constraints, config, ledger, scope_modes, system): `100% tests passed out of 24` |
| 3 | feat/slint-gaps (b9e22d3f) | (already inside W2, `merge-base --is-ancestor` yes) | - | - |
| 4 | feat/screen-c (e888ed27) | 6d8e2f0f | `src/slint/main.cpp` include block: HEAD's S1 includes and `lanec_publish.hpp` both kept. `main.slint` status bar: S1 had rebuilt the keys as the width-holding group `status-keys`; C's clock block (the `LaneC.clock-text` rectangle) was moved INTO `status-keys`, after the Report key and before the 8 px spacer, and C's old flat copies of the three keys dropped (the group has them). `lang_assets.hpp` (180 hunks) regenerated. tick doc: both kept. coverage regenerated: `2249 rows, 615 asserted, 96 driven, 1538 not drivable, 0 unclassified` | 48 tests (lanec, scanner, slint_host*, house rules, constraints, i18n, ledger, scope_modes, system, waterfall): `100% tests passed out of 48` |
| 5 | feat/screen-b (71b51f83) | cdc54cc8 | `main.slint` export list: `HighContrast` + `SpectrumEvents, WaterfallLook`. `src/slint/ui/system_bank.slint` Display section: HEAD's "Digit grouping" / "High contrast" lines first, then B's "LANE B" block. `resources/lang/*.json` (33 files, 4 hunks each): done by a three-way merge of each key table (base, ours, theirs) written back in the file's own format (json indent 1, sorted keys): **1683 keys added from screen B (51 per catalogue); no key had different text on both sides; none removed**; `tools/i18n_keys.py`: `2661 translated, 0 empty, 0 missing, 0 stale` in every catalogue. `lang_assets.hpp` (242 hunks) regenerated. coverage regenerated: `2269 rows, 635 asserted, 96 driven, 1538 not drivable, 0 unclassified`. B's adjusted `test_slint_scope_modes` / `test_slint_system` artboards kept (not reverted) | 114 tests (slint, i18n, ledger, waterfall, spectrum, colour, trace, history, measure, scanner, lanec): `100% tests passed out of 114` |
| 6 | feat/web-secure (2050fd99) | e88e50a4 | `src/core/thread_beats.hpp` kAllThreads: HEAD's Tci/UdpAudio lines kept, a new line `&kThreadWebTlsAccept, &kThreadWebTlsConn,` after them. `tests/CMakeLists.txt`: HEAD's `test_extio` branch kept and web-secure's three test names added to the `elseif(test_font_assets ...)` condition; the interface-fence header list gains `net/web_users.hpp net/web_tls.hpp` after HEAD's `net/udp_audio.hpp`. `tests/thread_inventory_rows.inc`: `src/net/web_tls.cpp` row placed before HEAD's `src/source/extio_client.cpp` row | 30 tests (`test_web_*`, thread_inventory, thread_beats, interface_fence web): `100% tests passed out of 30`, run on the hidden desktop (`test_web_tls` Passed 17.93 s, `test_web_secure` 18.78 s, `test_thread_beats` 89.46 s). See section 5 for why `test_web_tls` was allowed to run |
| 7 | feat/radio-families (05172612) | 67496535 | none | `test_ascp_source`, `test_hpsdr1_source`, `test_aaronia_source`, `test_radio_families_sinks` + inventory/health/source kinds: `100% tests passed out of 20` |
| 8 | feat/tuning (ed884c3b) | a4dd5bbe | none | tuning_bookmarks/dsp/operate/scan + thread_inventory + health_paths: `100% tests passed out of 6` |
| 9 | feat/recplay (0fec660e) | 018d52d8 | none | 9 added tests + inventory/health: `100% tests passed out of 12` |
| 10 | feat/sysmodels (21f38996) | 876ff226 | `tests/CMakeLists.txt` tail of the file: HEAD's recplay timeout property for `test_record_attachments` and sysmodels' `foxsdr_manifest_sign` block, both kept (HEAD's first) | 7 added tests + inventory/health: `100% tests passed out of 10` |
| 11 | feat/access (601d9640) | bbe35d1d | none | 5 added tests + inventory/health: `100% tests passed out of 8` |
| 12 | feat/sigmeas (3b6305f5) | ce7ab6da | `tests/CMakeLists.txt` tail: recplay, sysmodels blocks, then sigmeas' `sigmeas_realdata` executable (not a ctest) | 12 added tests + inventory/health: `100% tests passed out of 15` |
| 13 | feat/bitview (1b6ca098) | c5bf648f | none | 8 added tests + inventory/health: `100% tests passed out of 11` |
| 14 | feat/rfmon (4979566a) | 44db36e6 | none | 5 added tests + inventory/health: `100% tests passed out of 8` |
| 15 | feat/webmodels (a2f5c0fe) | 0acd84ba | `tests/CMakeLists.txt` interface-fence header list: HEAD's line ending in `net/web_tls.hpp`, then webmodels' seven headers (`web_viewport ... upnp_igd`) after a comment line, closing parenthesis moved to the end | 8 added tests + inventory/health: `100% tests passed out of 11` |
| 16 | feat/dspmodels (3f4012ad) | 66682ebe | none | 12 added tests + inventory/health: `100% tests passed out of 15` |
| 17 | feat/hamops (47e08ba4) | 82fb88ba | `tests/CMakeLists.txt` tail: the recplay, sysmodels and sigmeas blocks (HEAD) then hamops' `foreach` giving its source-scan tests `HAMOPS_SRC_DIR` | 13 added tests + inventory/health: `100% tests passed out of 16` |
| 18 | feat/sweepsrc (66f9b2f4) | 5bdac03b | none | 4 added tests + inventory/health: `100% tests passed out of 7` |
| 19 | feat/packaging (d64f474c) | 07a186ea | none | `test_package_identity` Passed; `packaging_openapi_routes` FAILED: caused by the merge, fixed in b33a578b (section 3). `packaging_manifests` is NOT registered on this build (CMake's `pwsh` is the WindowsApps stub), so it was not run: UNVERIFIED |
| 20 | feat/hpsdrduo (c853e0a6) | 928f6fdc | none | `test_hpsdr2`, `test_hpsdr_frames` + inventory/health: `100% tests passed out of 5` |
| 21 | feat/gpufft (30526ea6) | 8a7a452f | none | `test_gpu_fft`, `test_gpu_fft_bench` + inventory/health: `100% tests passed out of 5` |
| 22 | feat/feedplan (8fb813e1) | ba2a4679 | none | `test_engine_api_feed_check`, `test_feed_fixtures` + inventory/health: `100% tests passed out of 5` |
| fix | (this lane) | b33a578b | not a merge: section 3 | see section 3 |

Already inside W2, so not merged again: feat/engine-wire (W1 2b2ede72), feat/slint-gaps, feat/kiwi-client, feat/web-bookmarks, feat/unblocked-desk, feat/platform,
feat/recording-models, core-formats, net-map-models, net-outputs, radio-sources, array-maths.

## 2. Branches NOT merged, and why

| Branch | Why |
|---|---|
| feat/engine-serial-a | being built now in `mr-engine1` from 5a45640f; a follow-up message brings it. Not waited for. |
| feat/knob-input (c86bb870) | not inside W2 either (W2 record section 1 row 9: a host-layer input model; `plugin-or-host.md` row U11a places it as the plugin `controllers`). Not on the orchestrator's list; left as a branch. |
| feat/manual | not started, not on the list. |
| feat/android (33331d16) | NOT merged by order. Ten lines for the orchestrator's decision, read from its record `docs/feature-records/android.md`: (1) it is `origin/android` (49 commits since 0.98.1) merged forward into d3630d78, plus a follow-up, 52 commits; (2) on the desktop it changes the root `CMakeLists.txt` (OpenGL/GLFW/PortAudio/ImGui moved under `if(NOT CASCADE_ANDROID)`; `cascade_interface` link rule), `tests/CMakeLists.txt`, 27 `src/core` files, 42 `src/gui` files (mostly `gui::px()` scaling and a PlatformWindow refactor), sinks, usb, net - and it deletes two test files that exercised android GUI code (`test_plugin_store_deck`, ...); (3) it will conflict with every screen lane and with W2/serial lanes in `CMakeLists.txt` and `app_window.cpp`; (4) the Android app does not link: `AppWindow` was never ported onto `PlatformWindow` (a ~1,600-line change to a 24,866-line file), nothing Android was built (no SDK/NDK/JDK/Gradle on this PC); (5) its desktop gate was `98% tests passed, 16 failed out of 792`: the 5 known false failures + `test_window_failed_app` (configuration), `test_crash_upload` and `_free` (fail the same on feat-web), `test_software_gl_app` (softgl DLLs; the same on feat-web), `test_engine_api_source_kinds` and `_decode_radio` (git-ignored `scope.wav` fixtures), and five that passed on re-run (`test_link_request_poll`, `test_slow_frames_app`, `test_health_app_stepped`, `test_diagnostics`, `test_diagnostics_free`); (6) the record says no red is attributable to the merge; (7) its APK CI job is expected to fail at compile until the AppWindow port is done. |

## 3. What this lane fixed (commit b33a578b; everything else is a merge)

1. **`test_json_dump_policy` (caused by a merge)**: `src/core/bookmark_pack.hpp` (feat/tuning) `exportPack` called `j.dump(2)` without `error_handler_t::replace`; the policy test
   that holds every `dump()` in the tree to it read the new file: `scanned 1165 files, 62 dump() sites, 1 violation(s)`, `3 checks, 1 failed`. Fixed to
   `j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)`. After: Passed (0.30 s alone, 6.20 s in a batch). The failure is the red before the fix.
2. **`packaging_openapi_routes` (caused by a merge)**: `docs/api/openapi.yaml` was written against a web server without the web-secure routes and with W1's SSE/mute marked
   "to add at merge" (the packaging record's own merge note says to remove the markers). Before: `604 checks, 13 failed`. After: `711 checks, 0 failed`. Changes: markers removed
   (events, `muted` on the control request and the status), events description corrected (the stream cap is no longer 4), documented `GET /api/room`, `GET/POST /api/chat`,
   `GET /api/admin/state`, `POST /api/admin` and nine schemas, `SessionState` gains `role, listenersEnabled, secure, name`, a new `LoginOk` schema, statuses 403/503 on `/api/login`
   and 403 on `/api/control`, all read from `src/net/web_server.cpp` as merged. Two lines of `tests/packaging/test_openapi_routes.py`: a route that refuses with
   `who.role == Role::None` counts as needing a sign-in (the room routes ask `identify()`), and the login answer is compared with `LoginOk`. No check was removed. This is
   documentation of behaviour I read, not run: the live server was not started (UNVERIFIED).

## 4. Gates (Release, `D:/foxsdr-build/mr-dsp`)

**Headless + slint** (`ctest -L "headless|slint" -I a,b -j4 --timeout 300`, 1022 tests, slices 1-100, 101-350, 351-550, 551-750, 751-1022) at `ba2a4679` plus the uncommitted
form of the b33a578b fix for the last two slices:

| Slice | Result |
|---|---|
| 1-100 | `99% tests passed, 1 tests failed out of 100`: `test_slint_fitted` (Passed alone 11.67 s; then 3 more runs alone `Passed` 11.0-11.1 s: load flake) |
| 101-350 | `99% tests passed, 3 tests failed out of 250`: `test_airband_data`, `test_band_plan`, `test_converter_call_sites` (known false failures) |
| 351-550 | `99% tests passed, 3 tests failed out of 200`: `test_json_dump_policy` (merge; fixed, section 3), `test_page_resize_id`, `test_png_decode` (known false failures). The FIRST attempt at this slice was spoiled by the incident in section 6 (0xc0000142 on most tests) and was discarded; the numbers are from the clean second run (`Total Test time 389.28 sec`) |
| 551-750 | `100% tests passed out of 200` |
| 751-1022 | `99% tests passed, 1 tests failed out of 272`: `packaging_openapi_routes` (merge; fixed, section 3) |

All five known false failures were red as expected. `test_diagnostics_free` passed in the gate (fresh stamp), `test_crash_upload_free` passed (it was red in W2's record).
After the fix commit and a rebuild: `test_json_dump_policy`, `packaging_openapi_routes`, `test_tuning_bookmarks`, `test_i18n`, `test_i18n_glyphs`, `test_thread_inventory` Passed;
`test_diagnostics_free` failed once in that batch (15.67 s, while the clip sweep and a build ran) and Passed alone (12.67 s): load.

**GUI** (`-L gui -j3 --timeout 300`, hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`): `100% tests passed out of 319`, `Total Test time (real) = 2480.41 sec`. (Includes
`test_diagnostics`, `test_health_app`, `test_stop_ends_recordings`, `test_crash_upload`, every `slint_drive_*` script.)

**Re-run on the final executable** (hidden desktop): `slint_drive_scope-analysis` Passed 21.64 s, `scope-demod-inputs` 18.77 s, `scope-demod-open` 10.13 s, `scope-radar-open` 11.36 s,
`scope-radar-targets` 41.84 s, `test_diagnostics` 3.76 s. (`test_health_app` and `test_stop_ends_recordings` failed in that same batch: section 4a.)

**Clip sweep** (`tools/slint_clip_sweep.py` on a probe build `D:/foxsdr-build/mr-dsp-probe`, `-DFOXSDR_SLINT_CLIP_PROBE=ON`, hidden desktop, `--jobs 3`): `SWEEP: 1224 runs (34 languages x 36 views), 0 runs that did not finish`;
`18141 items (EDGE 2412, ELIDE 11581, OUT 4148); 14987 explained, 3154 UNEXPLAINED at 12 sites in 34 languages` (sites: chips.slint:34, keys.slint:58/201/285, main.slint:403,
palette.slint:49, sections.slint:187, sliders.slint:241, spectrum.slint:432/439, tooltip.slint:118, waterfall.slint:551). Per view kind: each of the seven `r4-*` kinds (card, decode,
export, layouts, marks, one, panes) has exactly **446** unexplained items, each of the eight bank views 4. S1's own final figure was "446 in each of the three kinds, the same sites in each", so the per-kind number
has not grown. **Against the brief's "738 unexplained of screen S1"**: that figure was S1's first commit's sweep of eight views (r4-one + r4-panes); the same two kinds read 2 x 446 = 892 here, which is
MORE than 738, but S1's later commits changed what the sweep counts (it reports 446 per kind for its three new kinds at its own tip), and no sweep was run at b4bdcd08, b9e22d3f or
the other tips, so a like-for-like comparison with 738 is UNVERIFIED. What is checked: the figure is the same in every kind (the pre-existing receiver strip, tab row, title bar and tooltips), not a new site. The
full per-item file is `D:/foxsdr-build/em-work/em-sweep.tsv` (not committed).

### 4a. Tests still to classify (each alone, three times, on the merge tip; baseline = a build whose engine code is identical to d3630d78)

There is no build of d3630d78 itself. The baseline used is `D:/foxsdr-build/en-feedplan` (feat/feedplan = d3630d78 plus new files only; `git diff d3630d78 feat/feedplan -- src/engine
src/core/pipeline.cpp src/core/pipeline.hpp tests/test_pass_control.cpp` is empty), run directly, interleaved with the tip so the load is shared. Its binaries are not mine and were only executed.

| Test | At the tip, alone | Baseline | Class |
|---|---|---|---|
| test_engine_api_transmit | Passed x3 (103.3, 103.0, 103.1 s) and in the headless gate | not run (green at the tip every time) | load flake / old (W2 saw it red only under load) |
| test_pass_control | Passed in the gate; alone through ctest: Passed, FAILED (`:891-895`, pass rate outside 50-70/s), then direct runs 3 of 3 Passed | 3 of 3 passed | **load flake** (pass-rate bounds; not a merge: same code on both) |
| test_stop_ends_recordings_free | Passed x3 (56.6-57.0 s) | - | not red |
| test_stop_ends_recordings (gui) | FAILED once in a batch of five (`:944 slowLine.empty()`), then Passed x3 alone (49.5-50.5 s) | - | load flake (a slow-frame line) |
| test_excuse_cap | Passed x3 (68.9 s) | - | not red at the tip |
| test_failure_files | Passed x3 (66.0 s) | - | not red at the tip |
| test_window_failed_app | Passed in the GUI gate (Slint ON build) | - | the "Slint off only" failure does not apply here |
| test_health_app_stepped | Passed x3 (48.6 s) | - | load flake (W2: the same) |
| test_plugin_unload_handshake | Passed x3 (3.9 s) | - | not red at the tip |
| test_sentinel | Passed x3 (6.8 s) | - | not red at the tip |
| test_plugin_store_window_free | FAILED once alone (74.5 s) after the incident of section 6, then Passed x4 (23.9-25.9 s) | - | load flake |
| test_health_app (gui) | alone on the tip: 1-2 of its 252-254 checks fail in each of three runs (`bindOrAbsentCount(... , 1)` at :1167/:1220/:1240/:1641: 1, 2 and 4 failures); on the baseline build in the same session: 1, 1 and 6 failures | fails the same way | **old failure** (intermittent; W2 and S1 say so) |
| test_slint_fitted | Passed x4 alone | - | load flake |
| test_json_dump_policy | red in the gate, alone: `1 violation(s)` | - | **caused by a merge**, fixed |
| packaging_openapi_routes | red in the gate, alone: `604 checks, 13 failed` | - | **caused by a merge**, fixed |
| test_diagnostics_free | Passed in the gate and alone; failed once in a loaded batch | - | stale-stamp family / load |

W2 record 8b's reds: `test_crash_upload_free` and `test_diagnostics_free` Passed in this gate; `test_engine_api_merge` Passed (21.54 s after the W2 merge); `test_engine_main_password_not_in_argv`, `test_plugin_misuse`,
`test_health_paths` Passed.

## 5. Web TLS test

`test_web_tls` was read before it was run: web-secure's tip carries the silent-flag rework (every NCrypt call with `NCRYPT_SILENT_FLAG`, thumbprint tests on an in-memory store through
`TlsOptions::searchStore`, the source-scan test `testTlsSetUpIsSilent`, which `grep` confirms is in the tree). `src/net/web_tls.cpp:598` does open the user's Personal store read-only, but only when a
thumbprint certificate is configured, which no test does. It then ran on the hidden desktop only (`test_web_tls` Passed 17.93 s, `test_web_secure` 18.78 s, `test_web_users`). In the later full
headless gate it ran through ctest on the VISIBLE desktop's session as part of slice 751-1022 (it is labelled headless): it passed and no dialog was reported; that is a deviation from "hidden desktop only" for
that one test and is listed in section 6.

## 6. Incidents and deviations (to be known)

* **Dialogs on the owner's visible desktop.** During my second headless slice (tests 351-550) a tool call was interrupted by the user; the ctest that call had started kept running detached and
  launched test executables that failed to start with **0xc0000142** ("The application was unable to start correctly"), each raising an Application Error box on the visible desktop (the user sent a
  screenshot: `test_panel_note_once.exe`; the boxes kept reappearing until they were dismissed). I killed that ctest (pid 188988, mine) and checked that no test process of mine remained. Cause: a detached
  process cut off from its launching call (UNVERIFIED as the exact mechanism; the same tests Passed when started again from a fresh call, and nothing changed in the binaries). This breaks "never raise a
  Windows prompt". From then on every GUI run used the hidden-desktop launcher, and long headless runs were not left to a call that could be interrupted.
* `test_radio_setup_app` (a gui-labelled test) was run once on the visible desktop in the first batch after W2 (`ctest -R` including `radio_setup`); it passed, no dialog was reported.
* The headless gate ran on the machine's normal session (not the hidden desktop), as the brief says.
* `-j4` for headless and `-j3` for GUI as ordered; builds used `--parallel 8` (`--parallel 4` while the sweep ran).

## 7. BUILT BUT NOT WIRED (each row exists as a standalone model with tests; no engine call, no screen control; from the lane records)

Hand-off sections are in each lane's `docs/feature-records/<lane>.md` (section named "Wiring and screen hand-off"); the earlier branches' item-by-item table is W2's record section 2 (`engine-wiring-2.md`).

| Lane | What exists (not wired) | File a wiring lane starts from (new files) and the hand-off |
|---|---|---|
| tuning | scanner trigger and ranges (`ScanTrigger`, `ScanPlan`, `ScanCursor`), whole-span `fftScan`, bookmark notes/tags, packs, favourites, history, band memory, RIT/XIT, channel plans, CSV overlay, key actions, marker import/export, CW surround, tone burst, companion launcher | `src/core/scan_trigger.hpp`, `scan_plan.hpp`, `fft_scan.hpp`, `bookmark_notes.hpp`, `bookmark_pack.hpp`, `favourite_state.hpp`, `band_memory.hpp`, `rit_xit.hpp`, `channel_plans.hpp`; `src/dsp/cw_surround.hpp`, `tone_burst.hpp`; hand-off section 4 of tuning.md (needs engine scanner tick, bookmark page, key bindings, CAT) |
| recplay | playback transport (pause/step/speed/A-B), lock while recording, daily subfolders, analyser project (`.fxproj`), box export, SigMF annotations, waterfall history (pause + scroll back), record attachments, WAV LO metadata (P16) | `src/source/playback_transport.hpp`, `src/core/record_lock.hpp`, `rec_daily.hpp`, `analyser_project.hpp`, `record_attachments.hpp`, `waterfall_history.hpp`, `src/dsp/box_export.hpp`, `src/source/sigmf_annotations.hpp`, `wav_meta.hpp`; each Part's "WIRING HAND-OFF" paragraph (`iq_file_source.*`, `recorder.cpp`, `fox_engine_api.cpp`) |
| sysmodels | setup wizard, portable mode, export/import bundle, reset, settings profiles, device-arrival and resume watchers, keep-awake, resource meter + effects policy, manifest signature check, ed25519 | `src/core/sys_portable.hpp` (needs `main.cpp`), `sys_device_watch.hpp`, `sys_resume_watch.hpp`, `sys_keep_awake.hpp`, `sys_resource_meter.hpp`, `sys_manifest_sig.hpp` (needs `updater.cpp`), `sys_bundle.hpp`, `sys_profiles.hpp`, `sys_reset.hpp`, `sys_first_run.hpp`; hand-off steps 1-7 of sysmodels.md; the signing tool `tools/manifest_sign.cpp` is built; code signing needs the owner's certificate |
| access | speech (SAPI), sonification, colour-blind-safe palettes, units, locale number format | `src/core/access_speech.hpp`, `access_sonify.hpp`, `access_palette.hpp`, `access_units.hpp`, `access_number_format.hpp`; access.md section 8 (settings keys, `track_metrics.hpp`, `map_view.cpp`, `host_logic.cpp` call sites) |
| sigmeas | symbol rate, vs-time, FSK/OOK, bursts, hopping/chirp, occupied bandwidth, FM/AM meters, audio measurements, frame log, histograms, eight cursors | `src/dsp/sigmeas_*.hpp` (12 files); sigmeas.md section 6 (one read op per block; panel for the selected box) |
| bitview | bit view, line codes, whitening, sync, field inference, CRC finder, message diff, user spec decoder, raw ring, binary editor | `src/core/bits/*.hpp` (11 files); bitview.md hand-off (engine slicer tap + a page) |
| rfmon | RBW/VBW + detectors, limit mask, field strength / antenna factor, pseudo-Doppler, Matrix overview model | `src/dsp/rbw_detector.hpp`, `pseudo_doppler.hpp`, `src/core/spectrum_mask.hpp`, `field_strength.hpp`, `radio_matrix_model.hpp`; rfmon.md hand-off |
| webmodels | viewport, bookmarks, clip, profiles, exclusive-control arbiter, CORS, UPnP IGD client, store sort | `src/net/web_viewport.*`, `web_bookmarks.*`, `web_clip.*`, `web_profiles.*`, `web_arbiter.*`, `web_cors.*`, `upnp_igd.*`, `src/core/store_sort.*`; `docs/webmodels-handoff.md` (needs `web_server.cpp`, now merged from web-secure) |
| dspmodels | display averaging, gradient, zoom-to-selection, mixer, IF filter, signal-generator and scope nodes, patch edit history, airband per-channel | `src/dsp/trace_average.hpp`, `audio_mixer.hpp`, `if_filter.hpp`, `siggen_model.hpp`, `scope_model.hpp`, `snr_squelch.hpp`, `src/core/zoom_select.hpp`, `gradient_model.hpp`, `graph_edit.hpp`, `command_stack.hpp`, `airband_channel.hpp`, `airband_writer.hpp`; dspmodels.md section 6, rows 1-9 |
| hamops | Cabrillo, contest macros/score, ADIF QSO + broadcast, band map, OmniRig client, RadioInfo UDP, Antenna Genius, X-Plane UDP, HID descriptor/map, transcript store | `src/core/cabrillo.hpp`, `adif_qso.hpp`, `contest_score.hpp`, `band_map.hpp`, `omnirig_client.hpp`, `antenna_genius.hpp`, `transcript_store.hpp`, `src/net/adif_broadcast.hpp`, `radioinfo_listener.hpp`, `xplane_listener.hpp`, `src/input_hid/*`; hamops.md section 7 (FLRig and HRD clients are blocked there) |
| sweepsrc | tinySA sweep protocol + Windows serial port, sweep line/history/stitcher/occupancy | `src/source/tinysa_protocol.hpp`, `serial_port_win.hpp`, `src/core/sweep_*.hpp`; sweepsrc.md (a new "sweep device" source kind in the registry and `engine.cpp`; RF Explorer and HackRF sweep blocked) |
| packaging | winget/Scoop/Chocolatey templates + generator, SDK template, OpenAPI file | `tools/make-package-manifests.ps1`, `packaging/`, `sdk/`, `docs/api/openapi.yaml`; release script step in packaging.md (publishing needs accounts: owner) |
| hpsdrduo | HPSDR protocol-2 decoder and source, HPSDR frame decoder (RSPduo rate table work) | `src/source/hpsdr2_protocol.hpp`, `hpsdr2_source.hpp`, `hpsdr_frame.hpp`; hpsdr-rspduo-followup.md (register as a source kind) |
| radio-families | ASCP (RFspace NetSDR/Afedri), HPSDR-1 (Red Pitaya, Metis/Hermes) and Aaronia sources, built against stand-ins, UNVERIFIED on hardware | `src/source/ascp_source.hpp`, `hpsdr1_source.hpp`, `aaronia_source.hpp` (registration pattern in W2 record section 2.5) |
| gpufft | Direct3D compute spectrum engine | `src/dsp/gpu_fft.hpp` (`GpuSpectrumEngine`); gpufft.md hand-off: `core/pipeline.cpp` next to `SpectrumEstimator`, setting `gpu_fft.mode` |
| feedplan | feed table + fixtures + headless feed check (no engine change by design) | `tests/test_engine_api_feed_check.cpp`, `test_feed_fixtures.cpp`; 23 "NO FEED YET" generators are a porting project (owner decision in feedplan.md) |
| screen S1 | engine hand-offs: a credit field on plugin marks, a per-radio level store for every open radio, a saved trace mode with persistence, a "Save as" dialog | screen-s1.md "ENGINE CHANGES WANTED" |
| screen C | scanner extras op (`FOXAPP_OP_SCANNER_EXTRAS`: its proposed number 0x860C collides with SCHEDULE_*/CAL_* in this tree; 0x860A/0x860B are free), bookmark-carried squelch/tone, tell a plugin the symbol rate, separate plugin mute | screen-c.md hand-offs 1-2; W2 record section 2.7 rows C1-C9 |
| earlier (W2 section 2) | CLI options, naming templates (done by W2), FLAC/RF64/SigMF writers, IF recording, recordings browser, seek/A-B loop, scan aids, VFO A/B, decode database/alert rules, metrics, webhooks, MCP server, network I/Q out, SBS out, coverage grid, track store, SoapyRemote, SDRconnect, low-IF, CAT follower, ExtIO helper, CAT emulation, Discord presence, combiner, DF | W2 record `engine-wiring-2.md` sections 2.1-2.8 and 6 (file names there) |

## 8. Owner decisions and UNVERIFIED

Decisions: (1) whether `feat/android` merges and when (section 2); (2) `feat/knob-input` stays a branch, or becomes the plugin; (3) the clip-sweep "738" bar (section 4) - a sweep per tip would be needed to compare like for like;
(4) merge keeps screen C's clock inside the width-holding key group (a visible placement choice); (5) the OpenAPI file now documents the room, chat and admin routes as merged.

UNVERIFIED: the 5 known false failures were not investigated; `packaging_manifests` not run (no registered `pwsh`); the OpenAPI additions against a live server; the sweep comparison with 738; the mechanism of the
0xc0000142 incident; test_web_tls ran once through the ordinary headless slice as well (section 5); nothing was built for Linux; the `kiwisdr`/`soapyremote` telemetry words of W2 section 5 still need the Worker first.

## 9. Configuration file

`%APPDATA%/foxsdr/config.json` sha256 before (start of the lane) `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`; after (checked at 13:55, before this commit)
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`. Byte-identical. No outside connection, no download, no account, nothing pushed.
