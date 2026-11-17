# Engine merge lane 8 (`engine-merge-8`): the second-merge branch and the security fixes into `engine/multi-radio` (2026-11-16 / 17)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `06345034` (clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, `FOXSDR_SLINT_CLIP_PROBE=OFF`, Release, `--parallel 6`), scratch `D:/foxsdr-build/m8-main-scratch/`. Every GUI-capable test ran through the hidden-desktop launcher on desktop `FoxSDRMerge8M`, `FOXSDR_FRAME_CAP_HZ=60`, `ctest -j3` (reruns `-j1`); `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set; no radio opened; nothing pushed, downloaded or deployed; no other worktree touched. The machine was quiet (no other lane building): a red that failed alone is treated as real.

## 1. The two merges (merge commits, no rebase, no squash)

Both tips were checked before merging: `integ/second-merge` = `ea290112`, `feat/security-fixes-1` = `b60611b1`, exactly as briefed. `feat/screen-s4b` was not merged.

| # | Branch | Merge commit | Conflicts and how each was settled |
|---|---|---|---|
| 1 | `integ/second-merge` (ea290112; screen lane S3, lane X, the S5b/S5c radios follow-on with the Fobos bridge, the radio-report calls) | 5b3c573f | ONE textual conflict: `tests/thread_inventory_rows.inc` (merge 7's own row for `src/slint/desk_logic.cpp` against the second merge's three rows for `hid_win.cpp`, `midi_win.cpp`, `lanex_bench.cpp`): both sides kept. Everything else merged clean because merge 7 changed only five files since `66072154` (its record, the open-rows file, `engine_wire_c.cpp`, `s4_logic.cpp`, the inventory), so merge 7's later gate fixes (the CMake test-family compile lines, the one `takeUnclaimedResults` loop, the wire-c version fix for S4's outputs, the health-path rows, the measured bounds) were never in conflict and stay. `lang_assets.hpp` regenerated with `tools/embed-lang.py`: byte-identical to the merged file (diff empty after line-ending normalisation); `tools/i18n_keys.py`: 33 catalogues, 4628 keys each, 0 empty, 0 missing, 0 stale |
| 2 | `feat/security-fixes-1` (b60611b1) | 2ef39bbd | none textual (`web_server.cpp`, `tests/CMakeLists.txt` and `errpath_allow.tsv` auto-merged). The one new allow line (companion_programs.cpp damaged record) kept; `kPendingPin` stays 459 |

Fixes of mine after the merges (each its own commit, never by weakening a test; section 3 has the evidence):

| Commit | What |
|---|---|
| da1db129 | three counts of lane S5c's new Fobos rows in other lanes' tests: wire-b source kinds 12 to 13, needed libraries 9 to 10 (and the last row `ftdi-d2xx` to `libfobos`), the `lanel-libs` drive `ll.libs` 9 to 10 |
| ea28e1ac | `test_converter_call_sites`: one allowed row for lane S3's read-only recorded-file facts (`engine_recplay.cpp:117`) |
| 1a63ccbc | `test_thread_inventory`: rows for `src/engine/cat_link.cpp` and `src/source/fobos_source.cpp` |
| 236d0229 | `docs/api/openapi.yaml`: `/api/session` carries `readOnly` (the security lane added it to the handler) |
| 15c94747 | `core/sys_bundle.cpp`: source kind `fobos` is a local radio on the import guard's list, with a test and a break-it |
| d5090c1a | `test_engine_publish_alloc` F3 bound 2400 to 2550, measured |
| 38bb07bc | `test_engine_api_03` and `test_patch_graph_draft_input`: the missing-recording case no longer uses drive Z: |

## 2. Checks the brief asked for

**Import guard (security lane M2/R13) against S3, X and the radios follow-on.** The guard imports only `config.json`, `bookmarks.json` and `markers.json` (`allowedBundleFileName`). `git diff 66072154 ea290112 -- src/core/config.cpp src/core/config.hpp` is empty: none of the three lanes added a `config.json` key, and no bookmark or marker field changed. Their settings (`cat.link.*` including the serial port name, `rec.roll_*`, `rec.clip_*`, `rec.replay_s`, and the facts) live in `wirec.json`, which a bundle cannot carry, and `cat.link.port` is validated to `COM1` to `COM256` on write. So no key needed adding to `importLeftOutKeys()`. One real gap found: lane S5c's new source kind `fobos` (a USB radio; its only path-like option `lib=` is not an allowed key and is dropped by the classifier) was not on `localSourceKind`, so a backup of a Fobos radio was left out on import. Added with `testFobosRadioComesBack` (a Fobos source choice and saved radio return, the `lib=` path never does). BREAK-IT: `fobos` removed from the list gave `test_sys_bundle: 7660 checks, 3 failed`; restored `7660 checks, 0 failed`. The older wire-b USB kinds (icr8600, funcube, g39ddc, g33ddc, sdriq) are also not on the list: open row `M8-import-usb-kinds`.

**The web page and the new Host/Origin/read-only checks.** Lane X's `test_web_touch` (a headless browser through the launcher) Passed unchanged in the full GUI gate, and every other test that talks to the web page Passed: `test_web_server` (1291 checks at the lane), `test_web_secure`, `test_web_tls`, `test_server_host`, `test_remote_key_release`, `test_stop_ends_recordings`, `test_engine_api_wire_c_web`. No request needed fixing; the guard was never touched.

**The two Slint drives the security lane edited and never ran** (`companions-website-browse.txt`, `companions-website-page.txt`): both Passed in the full GUI gate (17.66 s and 19.62 s), as did `companions-website-unavailable`. Nothing in the scripts was wrong.

**Dangling-reference search** (the `d3519455` pattern; `m7-scratch/dangle.py`): 19 bare blocks in `main()` now (17 at merge 7, plus the publishers of lane S3 and lane X), 0 candidates. Windows-only proof; the GCC behaviour is UNVERIFIED.

## 3. Gate results

Final code build after the last source change: `cmake --build` exit 0. Headless gate `ctest -L "headless|slint" -j3 --timeout 300` in three slices on the first merged build (`2ef39bbd`): 1 to 540 (545 run), 541 to 1090 (550 run), 1091 to 1700 (128 run) = 1223 runs (the slices overlap on the numbers the re-registration shifts); full GUI gate `ctest -L gui` (418 tests, one run, `Total Test time 3306 s`).

| Red | Class | Evidence |
|---|---|---|
| `test_engine_api_wireb_sources` | REAL, merge consequence, FIXED (da1db129) | `FAIL test_engine_api_wireb_sources.cpp:32 n == 12 (13 vs 12)`: lane S5c added the "fobos" row; the lane changed its own test, not this one |
| `test_converter_call_sites` | REAL (not the known false failure), FIXED (ea28e1ac) | `FAIL engine_recplay.cpp:117 bypasses the converter: const auto* file = dynamic_cast<const ...IqFileSource*>(&r.pipeline_.rawSource())`, `bad == 0` failed: lane S3's radio facts read the file source's header flag and length (no frequency read or told), the sibling file-transport sites are already allow-listed |
| `test_sdriq_d2xx` | REAL, merge consequence, FIXED (da1db129) | `needed::neededTable().size() == 9 && back().libraryId == "ftdi-d2xx"`: lane S5c added `libfobos` (10 rows, last) |
| `test_thread_inventory` | REAL, FIXED (1a63ccbc) | `UNACCOUNTED THREADS: src/engine/cat_link.cpp ...` and `src/source/fobos_source.cpp ...` |
| `packaging_openapi_routes` | REAL, security lane, FIXED (236d0229) | `GET /api/session: the schema SessionState has exactly the keys the handler writes [... 'readOnly' ...] vs [...]` |
| `slint_drive_lanel-libs` | REAL, merge consequence, FIXED (da1db129) | `state ll.libs == 9 -> FAILED: it is 10` |
| `slint_drive_look-stamps` | INTERMITTENT, cause NOT proven (section 5) | failed once in the `-j3` GUI slice and once in a `-j1` repeat (`until-fail:6`: pass, pass, fail); `wf.tick1` read `-` at `state wf.tick1 contains :` after choosing Clock time, local |
| `test_engine_api_03` | INTERMITTENT, cause FOUND, FIXED (38bb07bc) | not red in the gates; failed once in ten runs alone (96.56 s): section 5 |

All other tests of both gates passed, including every merged lane's own: `test_iq_file_s3`, `test_overview_s3`, `test_record_rollover_s3`, `test_slint_lanes3_host`, `test_slint_knob_host`, `test_slint_lanex_host`, `test_slint_tester_host`, `test_input_*`, `test_slint_s5b_host`, `test_engine_api_cat_link`, `test_engine_api_s5b`, `test_fobos_source`, `test_rtlsdr_ifbw`, `test_soapy_settings`, `test_web_touch`, `slint_drive_lanex-*` (knobs among them), the security lane's 27 suites (web guard, secure room, TLS, vault, bundle, companions, report scrubber, RDS log, TCI, package identity), `test_error_paths`, `errpath_gate_selftest`, `test_i18n` (Korean word-spacing check included), `telemetry_worker`, `test_health_app`, `test_slint_tools`, `slint_drive_layout-section-overlap-1600` / `-624`.

After the fixes: the batch `test_engine_api_wireb_sources, test_converter_call_sites, test_sdriq_d2xx, test_thread_inventory, packaging_openapi_routes, test_sys_bundle, test_sys_setup, test_slint_host_system, test_slint_system, slint_drive_lanel-*, test_error_paths, test_i18n, errpath_gate_selftest` (22 ctest entries): `100% tests passed out of 22`.

## 4. Pinned numbers and changed expectations (every one, both values)

| Where | Was | Now | Inputs and reason |
|---|---|---|---|
| `kPendingPin` (`test_error_paths.cpp`) | 459 on both sides | 459 | union of the line changes: 459 + 0 (second merge: its sites are ALLOW, the re-seed dropped 75 lines of sites that now report and added 58) + 0 (security lane: one ALLOW line); the tsv has 459 `PENDING:` lines. By group: reports-not-in-function 168, O-outputs 55, unsure 45, R-record 27, G3-control-refused 27, G10-gpu-fft 16, D-gps-serial 13, G2-stream-health 12, T-tx 11, F-files 11, unmatched 9, S2-window 7, S1b-headless 7, A-audio 7, S5-driver-install 5, S3-plugin-ui 5, P-store 5, P-plugin 5, S9-report-upload 4, S8-sdrplay-service 4, S6-scan 4, S7-ui-engine 3, S1-startup 3, S4-first-run-check 2, G1-device-dead 2, PL2-companion 1, G7-patch-radio 1 (the same groups and counts as engine merge 7) |
| `test_engine_publish_alloc.cpp` F3 bound | 2400 (merge 7; measured 2162 to 2165) | 2550 | MEASURED on the merged tree: the F3 rebuilds allocate 2309 to 2318 (max 2318): 2318 x 1.10 = 2549.8, rounded up to 2550; the settled passes stay 0. F12 keeps 2200 (measured 1946 to 1947, 13 percent below; row `M7-publish-alloc-f12`). The growth of about 153 comes from the second merge's recording, CAT link and Fobos facts and rows and the security fields (not split per lane) |
| `layout-section-overlap-1600` / `-624` | 2292 (merge 7), window 3200 high | unchanged | MEASURED again with probe copies of both scripts (impossible bounds, read the "it reads" values): `"Read the pitch now"` bottom 2292.00, `"Sinks"` top 2480.00, `"Stop bypassing the selection"` bottom 820, `"Source"` top 880. Lane S3 (rows folded into the SIGNAL list) and lane X (pane marks) added 0 px to the pinned bottom: both lanes' controls sit in other banks or inside existing rows. `Sinks` top 2480 was not recorded at merge 7, so its own delta is unmeasured |
| `test_engine_api_wireb_sources.cpp` W1 | `n == 12` | `n == 13` | the table has thirteen rows ("fobos", lane S5c) |
| `test_sdriq_d2xx.cpp` | `neededTable().size() == 9 && back() == "ftdi-d2xx"` | `size() == 10 && back() == "libfobos"` and `findNeeded("ftdi-d2xx") != nullptr` | lane S5c's tenth row, appended last |
| `src/slint/tests/drive/lanel-libs.txt` | `state ll.libs == 9` | `== 10` | the same row, shown in the Needed libraries section |
| `test_converter_call_sites.cpp` kAllowed | no row for `engine_recplay.cpp` | one row | read-only recorded-file facts, no frequency |
| `tests/thread_inventory_rows.inc` | no rows | `cat_link.cpp` 1 thread, `fobos_source.cpp` 1 thread (R_DRIVER) | reasons in the rows |
| `docs/api/openapi.yaml` SessionState | no `readOnly` | `readOnly: boolean` | the handler writes it |
| `lang_assets.hpp`, the 33 catalogues | 4628 keys | 4628 keys, 0 missing, 0 stale | regenerated, identical |
| `docs/slint-drive-coverage.md` | n/a | unchanged | `slint_drive_ledger` Passed in the gate |

Not changed, though other lanes pin them: `test_health_events` token count.

## 5. The two tests merge 7 saw fail, and the two intermittents found

**`test_web_server`**, alone, `-j1`, ten runs: 10 passed, 0 failed (26.0 to 26.8 s each). Merge 7's 120 s timeout was load.

**`test_engine_api_03`**, alone, `-j1`: ten runs gave 9 passed and 1 FAILED (96.56 s against 57 s); a further 20 runs passed. Cause FOUND and measured: the case "an I/Q file that is not there" opened `Z:/no/such/recording.wav` and waits up to 60 s for `fact.patch.runnable` to turn 0. On this PC Z: is a remembered network drive (`net use`: `Disconnected  Z:  \\192.168.0.8\PcDownloads`), and a path on it sometimes waits out Windows' own 21 s network timeout. Measured: `Test-Path 'Z:\no\such\recording.wav'` took 0.0 s, 0.0 s, 0.0 s and 21.0 s on four tries in a row (the test's own comment already warned of "a mapped share that is offline"). Fix (38bb07bc): the case names a folder that does not exist on the local temp volume; the wait, the checks and the expected problem codes are unchanged. `test_patch_graph_draft_input` had the same name for its failing-to-open recording and got the same change. After: `test_engine_api_03` and `test_patch_graph_draft_input`, six runs of each alone: all 12 passed. The failing run itself was not captured with its failing line (the log of that run was overwritten): the match with the 21 s stall is by timing (+39 s) and by the measured stall, not by the failing line.

**`slint_drive_look-stamps`** (lane B's drive, passed at merge 7): failed once in the `-j3` GUI slice and once alone (3rd of a `until-fail:6` repeat), at `state wf.tick1 contains :` after `choose "Time stamps" "Clock time, local"`: `wf.tick1` reads `-`, which is what `main.cpp:3653` prints when the waterfall has fewer than two ticks. Afterwards it passed 20 of 20 and 20 of 20 (with extra non-failing probes added once), and a diagnostic copy passed 18 of 18 and showed 4 to 5 ticks, "27 lines/s . 9 s of history", step 2 s. Cause NOT proven. Most probable: `clockTicks(history = height x secondsPerRow)` makes one tick when the measured line rate momentarily spikes (history shorter than the tick step). It does not depend on this merge's code (`spectrum_extras.cpp` and the drive are unchanged since lane B). Open row `M8-look-stamps`.

## 6. Final state, stamp tests, config hash

After the fixes, a full rebuild at `ca11ba34` (the commit that carries sections 1 to 5 and 7; exit 0), then the FULL gates again at that commit through the launcher on `FoxSDRMerge8M`: headless `ctest -L "headless|slint" -I 1,1700 -j3` in one run: `100% tests passed out of 1218` (2 skipped by their own rule: `test_plugin_abi3_compat`, `test_rtlsdr_live`), `Total Test time 1968.94 s`; this run contains the stamp tests (`test_diagnostics`, `test_diagnostics_free`, built at that commit: Passed). GUI `ctest -L gui`: `100% tests passed out of 418`, `Total Test time 3291.18 s`, `slint_drive_look-stamps`, `slint_drive_lanel-libs` and the two companions-website drives among them. Counting the first gates, every red of this merge is therefore classified: 7 real (all fixed), 2 intermittent (one cause found and fixed, `look-stamps` cause unproven; it passed in this final run). The commit that adds this paragraph is documentation only: a stamp test run on a build made before it reads the older commit. The real `%APPDATA%/foxsdr/config.json` sha256 after the last test run: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing is running: every build and test was run to its end in the foreground.

## 7. Open rows

`docs/multi-radio-records/open-rows-2026-11-17.md`, refreshed: the open-rows tables of `screen-s3-recplay`, `lane-x`, `screen-s5b`, `screen-s5c` and `security-fixes-1` gathered once (ids S3-, LX-, S5b-, S5c-, SF1-), the merge's own rows (M8-), the three old rows the merges touch updated, and the closed rows marked CLOSED with evidence. Counts by who takes them, before 277 rows (244 open, 33 closed): screen 47 open / 14 closed, engine 120 / 20, plugins 15 / 0, Worker/site 18 / 5, owner decision 45 / 0, mouse drive 40 / 1: **285 open, 40 closed, 325 in all**. Closed by these merges (with evidence in the file): `S3-layout`, `S3-tools`, `LX-four-reds`, `S5c-fobos-report-calls`, `SF1-drives`, `M8-api03`, `M7-load-reds` (and the old `RRA-closed-layout`, `S2-closed-reds` rows are unchanged).

## UNVERIFIED

- GCC behaviour of the merged tree (nothing built on Linux; the security lane's POSIX halves of L10 and L3 were never compiled).
- Every real-hardware, real-pointer, real-browser-on-a-phone, real-Store and real-folder-chooser item of the five merged records (their open rows): the Fobos bridge on a real radio, RTL-SDR IF filter, a real rig and OmniRig, ShuttleXpress / PowerMate, the read-only badge and the Allowed-web-origins text under a real browser; the Worker and site, none deployed.
- The machine translations of S3, X, S5b and S5c keys (unread by a speaker).
- `slint_drive_look-stamps`: cause unproven (section 5). `test_engine_api_03`: the 21 s Z: stall is proven on this PC, but the failing line of the one failing run was not captured.
- The per-lane share of the F3 allocation growth (153) and of the section-overlap pixels is not split.
