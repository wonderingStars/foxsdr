# Engine merge lane 7 (`engine-merge-7`): ten wave-8 branches into `engine/multi-radio` (2026-11-16 / 17)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `9a19dc19` (clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, `FOXSDR_SLINT_CLIP_PROBE=OFF`, Release, `--parallel 6`), scratch `D:/foxsdr-build/m7-scratch/`. Every GUI-capable test ran through the hidden-desktop launcher on desktop `FoxSDRMerge7`, `FOXSDR_FRAME_CAP_HZ=60`, `ctest -j3`; `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set; no radio opened; nothing pushed, downloaded or deployed. The four lanes still building (`en-recplay`, `en-webmodels`, `en-dspmodels`, `en-rfmon`) are not in this merge and their worktrees were not touched. The real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). The machine was shared with the four lanes the whole time (single tests took 2 to 3 times their quiet time).

## 1. The ten merges (merge commits, no rebase, no squash)

Every branch tip was checked against the brief before its merge: all ten were exactly as listed.

| # | Branch (merged commit) | Merge commit | Conflicts and how each was settled |
|---|---|---|---|
| 1 | `feat/engine-e1` (52b51a9f) | 1768aa18 | none textual |
| 2 | `feat/radio-reports-app` (312e7b02) | a5b797a2 | `test_error_paths.cpp` pin (1125 vs 1116: union of the line changes from base 1126, -1 and -10 = 1115); `test_health_app.cpp` `withoutStagesOrMachine`: merge 6 added `slow.startup.*`, the lane added `scan_none`: both keys in the one function, both comments kept |
| 3 | `feat/radio-reports-app-2` (b6aae7e2) | 66762003 | pin only (928 from part 1's 1116 = -188; 1115 - 188 = 927) |
| 4 | `feat/errpath-sweep-a` (d2989291) | 0dec2b4a | `errpath_allow.tsv` (25 hunks) merged KEY BY KEY against `d4f855c5` with `m7-scratch/merge_tsv.py` (a line changed on one side takes it; a line the earlier merges removed because the site now reports, 7 of them the sweep had turned to ALLOW, stays removed); pin re-seeded 648 -> 456 (section 4) |
| 5 | `feat/screen-s1-signal` (1539b0f2) | 681efb35 | the 33 catalogues merged key by key against the base (`merge_lang.py`: S1's 181 keys in each, 0 keys changed differently on both sides), `lang_assets.hpp` regenerated |
| 6 | `feat/screen-s2` (c334c722) | 63dd42cb | `src/slint/CMakeLists.txt`, `src/slint/tests/CMakeLists.txt`, `main.slint` (import and exports of both lanes kept), `main.cpp` (8 hunks: includes, members, tick, answer, state names kept; the two Env construction blocks put back as two separate blocks), allow-list by key, catalogues (S2's 91 keys; "Lines written" added by both E1 and S2 with different machine text in 5 catalogues: the tree's existing text kept) |
| 7 | `feat/screen-s4` (1d127941) | 89b51024 | the same four files plus `test_engine_publish_alloc.cpp` (both lanes raised the bound 2000 -> 2200: taken once, measured at the end, section 4), `thread_inventory_rows.inc` (S4's `s4_outputs.cpp` row kept, `server_host.cpp` row is E1's 1/3/4/4); catalogues (S4's 85 keys) |
| 8 | `feat/screen-s5` (57520bd8) | bdf87d7a | `engine_api_lists.cpp` (S1's `passband` / `amdet` facts and S5's `directSampling` both kept), CMake, `main.slint`, `main.cpp` (a fifth Env block), the two overlap drive scripts, allow-list, catalogues (S5's 61 keys) |
| 9 | `feat/companions-shelf` (d0f02a79) | 1c91ef76 | allow-list and catalogues only (21 keys, 22 allow lines) |
| 10 | `feat/linux-slint-2` (361ea671) | 955a698d | none textual |

Merge faults of mine found and repaired (each in its own commit, never by weakening a test):

| Fault | Found by | Commit |
|---|---|---|
| keep-both of the CMake test families (S1, S2, S4 each add an `elseif(family ...)` whose first lines differ and whose last nine are identical) left `s1_host` and `lanes2_host` without their compile, include and test lines: `error C1083 foxsdr_api.h / nlohmann/json.hpp not found` | the first full build of merges 5 to 7 | 9befeddf |
| keep-both of the one-line `for (... kit.takeUnclaimedResults())` loops of S1 and S2 (merge 6) would have drained the list in the first loop and left S2 without the answers the kit's own poll took first: now ONE loop that serves patchBank, transmitBank, deckOnAnswer, laneLp, laneMp, laneS1p, laneS2p, s4p and s5p | reading the S4 merge hunk | in 89b51024 and bdf87d7a |

## 2. The dangling-reference search (the Linux lane's `d3519455` pattern)

`d3519455` moved six local lambdas (`stateOf`, `anyRunning`, `readDeviceRows`, `addFromCard`, `viewOf`, `precheck`) out of bare `{ }` blocks of `main()` because handlers registered in the block called them after it had ended (undefined behaviour that MSVC's fixed stack slots hid and GCC turned into a crash).
Method: `m7-scratch/dangle.py` finds every top-level bare block of `main()` (17 in the merged file), the names declared at depth 1 in each, and every `[&]` lambda in the block that names one of them and is a registration (`on_*`, `e.xxx =`, `std::function`, `set_handler`). VALIDATED first on the pre-fix `main.cpp` of `d4f855c5` (13 blocks): it reports exactly the six names of the Linux commit (`precheck` x2, `viewOf` x2, `anyRunning`, `stateOf` x2, `readDeviceRows` x2, `addFromCard` x2: 11 uses). On the merged file (17 blocks): 0 reports. Read by hand as well: the S1, S2, S4 and S5 Env blocks capture only `main()`-scope names (`api`, `session`, `workspace`, `service`, `selected`, `aimAt` (a free function), `configPath`, `autosaveJob`, `lastScannerState`, `utcNowMs`); the only 8-space-indent lambdas in the file are `add`, `matches`, `sink` (used inside the same tick or function) and `paneHoverAt`, which is handed to `wm.on_*_hover` BY VALUE (a copy of the closure whose captures are `main()`-scope). List of fixes found in the new blocks: NONE. (Windows-only proof; the GCC behaviour is UNVERIFIED on the merged tree.)

## 3. Gate results

Final code build: full `cmake --build` exit 0 (after the last source change, `97344f5e`; `445d26af` is a test table row, its target rebuilt and passed).

Headless gate, `ctest -L "headless|slint" -j3 --timeout 300`, hidden desktop, three slices (1 to 540: 545 run; 541 to 1090: 550 run; 1091 to 1610: 106 run): 545 + 550 + 106 = 1201 runs (the slices overlap by 5 numbers shifted by the re-registration; 1196 tests exist); GUI gate `ctest -L gui` (405 tests in one run, all numbered below 540): 405 run.

| Red in the gate | Class | Evidence |
|---|---|---|
| `test_json_dump_policy` | REAL, lane S4's own file, FIXED (5c363c27) | `VIOLATION (dump() without error_handler_t::replace ...): src/slint/s4_logic.cpp:1371 (line 44)`; `oneMember` built a text with a plain `j.dump()`; now `dump(-1, ' ', false, error_handler_t::replace)`; alone Passed |
| `test_engine_api_03` | REAL, lane S4's outputs block, FIXED (97344f5e) | failed in the slice AND alone, 3 runs of 3 at `-j1` too: `test_engine_api_03.cpp:2647 s.state().listSeq == list0 (2 vs 1)` and `:2657`; on the list of load flakes in four records, but a flake does not fail alone every time. Cause: `pumpWireC` of S4: `outputsSettingsVersion` starts at 0 and `outputsStatSig` at "", so an engine's first control pass incremented `wireCVersion_` twice, which is in the lists fingerprint, so the first pass moved listSeq after a client had read it. Narrowed by experiment (compiling out S1's fingerprint inputs: still red; suppressing the two first-pass increments: green). Fix: the first configure and the first signature reading set the state without the increment. Break-it: the red above is the break-it (unchanged code, 3 of 3 red); after: `100% tests passed out of 1` |
| `test_thread_inventory` | REAL, Linux lane's file, FIXED (445d26af) | `UNACCOUNTED THREADS: src/slint/desk_logic.cpp creates 2 std::thread` (the POSIX half's two `waitpid` reapers; the Windows build does not compile it but the inventory counts the text): one row with the reason; alone Passed |
| `test_health_paths` | REAL, radio reports part 2's own tip, FIXED (7bdf681f) | `Recovered::IqFileGone is used in src/engine/engine_disk_work.cpp (and engine_patch_radios.cpp), which the table does not list` (4 checks): the two uses were added without the table rows; the table lists them now |
| `slint_drive_layout-section-overlap-1600`, `-624` | REAL, measured, FIXED (66072154) | `expect "Read the pitch now" bottom <= "2058" -> FAILED: it reads "2292.00"`, `"Sinks": no control labelled`; section 4 |
| `test_diagnostics` | stamp (binary older than HEAD) | section 6 |
| `test_web_server` (Timeout 120 s), `test_pump_live_traffic`, `test_tx_audio` | load | each `Passed` alone, one run (`100% tests passed out of 3`) |

All other tests of both gates passed, including every lane's own: `test_i18n` (twice; the Korean word-spacing check of S1's branch is in it), `telemetry_worker`, `test_error_paths`, `errpath_gate_selftest`, `test_errpath_sweep_a`, `test_health_app` (both variants), `test_radio_reports_engine` (both variants), `test_slint_s1_host`, `test_slint_lanes2_host`, `test_slint_s4_host`, `test_slint_s5_host`, `test_engine_api_s5`, `test_engine_api_s4_outputs`, `test_s4_outputs`, `test_s4_settings`, `slint_drive_s1-*` (4), `slint_drive_s2-*` (5), `slint_drive_s4-*` (2), `slint_drive_s5-*`, `slint_drive_gaps-drop*`, `test_slint_tools`, `slint_drive_system-look`.
Not run: the two "intermittent" tests of engine merge 6 five times alone (the brief of this merge did not ask).

## 4. Pinned numbers and expectations changed (every one, both values)

| Where | Was | Now | Inputs and reason |
|---|---|---|---|
| `kPendingPin` (`test_error_paths.cpp`) | 1126 at the base | 459 | E1 -1, radio reports part 1 -10, part 2 -188, error-path sweep A -478 (1126 -> 648): naive sum 449. Merged tree after the sweep merge: 456, which is 7 above the sum because 7 sites were counted as fixed by both the sweep (ALLOW) and radio reports part 2 (a reporter now encloses them): a site fixed twice leaves the pin once. The coordinator's second review of the sweep (review-2.md) then returned 6 lines to PENDING (+6: `normalisePluginSiteUrl` x5, `kit_publish.cpp` browse files) and settled the four `app_window.cpp` window-start lines as ALLOW (-3): 459. Merges 5 to 10 changed nothing (only ALLOW lines). The pin never exceeds what the lanes imply plus the overlap and the review's net 3 |
| PENDING by group (459) | | | reports-not-in-function 168, O-outputs 55, unsure 45, R-record 27, G3-control-refused 27, G10-gpu-fft 16, D-gps-serial 13, G2-stream-health 12, F-files 11, T-tx 11, unmatched 9, S2-window 7, S1b-headless 7, A-audio 7, P-plugin 5, S5-driver-install 5, P-store 5, S3-plugin-ui 5, S6-scan 4, S9-report-upload 4, S8-sdrplay-service 4, S7-ui-engine 3, S1-startup 3, S4-first-run-check 2, G1-device-dead 2, PL2-companion 1, G7-patch-radio 1; gone: G8-sdrplay-lifecycle, G6-route-fallback, G5-rtl-pll, G9-vendor-fault |
| `test_engine_publish_alloc.cpp` F3 bound | 2000 (base); 2200 in E1 and in S4 | 2400 | measured on the merged tree: the F3 rebuilds allocate 2162 to 2165 (max 2165); about ten percent above. The F12 check keeps E1's 2200 (measured 1793 to 1794) |
| `layout-section-overlap-1600` / `-624` | bottom and Sinks top 2058 (merge 6); S5 said 2028 -> 2058 | 2292 (both numbers), window 2400 -> 3200 high | MEASURED. Engine fixes 6 2028; window gaps 2 Signal strength bar +30 (24 px + 6 px; in engine merge 6 already); screen lane S1's rows in the SIGNAL bank +234; lanes S2 and S4 mount only in the TOOLS and SYSTEM banks (+0). Lane S5's claimed +30 is NOT in the number: its "More source controls" row draws nothing for the signal generator, and the value read 2292 both before and after S5 was merged; S5 took the Signal strength bar (already in its base `d4f855c5`, where the script still said 2028) for its own row. With the longer SIGNAL list the Sinks header lies below 2400 px, so the window is 3200 (the same rule the scripts guard: the next section starts at or below the last control above it) |
| `tests/thread_inventory_rows.inc` | no row | `src/slint/desk_logic.cpp` 2 threads, reason | the Linux lane's POSIX reapers |
| `test_health_paths.cpp` table | `IqFileGone` in engine.cpp and engine_radio_saved.cpp | also engine_disk_work.cpp and engine_patch_radios.cpp | radio reports part 2's two uses |
| `docs/slint-drive-coverage.md` | 2379 rows, 806 / 118 / 1455 / 0 | unchanged | no merged branch changed it; `tools/slint_drive_ledger.py --check` on the merged tree prints the same 2379 / 806 / 118 / 1455 / 0 and `slint_drive_ledger` Passed |
| the 33 catalogues | 3907 keys after merge 1 (E1) | 4346 keys, 0 missing, 0 stale in every catalogue (`tools/i18n_keys.py`) | E1 +54 (-4 stale), S1 +181, S2 +91, S4 +85, S5 +61, companions +21 less duplicates added by two lanes |

Facts brought by merged lanes that other tests pin and that were NOT changed: `test_health_events` token count; E1's `test_thread_inventory` `server_host.cpp` row 1/3/4/4.

## 5. The second review of sweep A (the coordinator's addition, review-2.md)

One commit of its own (af0d375f). Returned to PENDING in their original groups: `normalisePluginSiteUrl` x5 (`reports-not-in-function`; `config.cpp:202` `sanitisePluginSites` drops a bad SAVED entry with `continue` and discards the sentence), `kit_publish.cpp` "browse files %" (`unsure`; `pickFile` returns false for a cancel and for a dialog failure). The `rtlsdr_source.cpp` `bringUpLocked` / `open` rows are NOT returned: the native-to-SoapySDR fallback reaches a reporter on the merged tree: `engine.cpp:1976` (asynchronous open) and `:10147` (saved radio) call `radioreport::noteRecovery(..., "routefallback", <native error>)` (added by radio reports part 2, a name in `tests/errpath_reporters.txt`) and `:1985` / `:10155` file the outcome. The fallback itself is not changed; the open-rows file carries it as `SA-fallback` (engine, REAL UNREPORTED RADIO FAILURE in the owner's sense that every radio failure is its own report: a journal recovery event exists, a report of its own does not). Reason texts corrected on 30 lines (the `parseHelperFields` rows now name `fetchSiteIndex` (`engine_store.cpp:349`) and `loadCatalogueCache`; the `Txn::stage` rows say `install()` skips the count for a site-sourced entry and for "cancelled"). The four `app_window.cpp` window-start lines settled as ALLOW: `glfwInit` (`:925-931`), `glfwCreateWindow` (`:961-974`), the ImGui GLFW backend (`:1241-1246`) and the OpenGL 3 backend (`:1252-1257`) each call `breadcrumb::noteWindowFailed()` before `tellUserWindowFailed`, and the sentinel files it at exit (`sentinel.cpp:398-413`, signature `:window-failed`): all four reach the same reporter.

## 6. Stamp tests, config hash

See the last section, written after the rebuild at the commit that carries this record.

## 7. Open rows

`docs/multi-radio-records/open-rows-2026-11-17.md`: the `OPEN ROWS FOR THE NEXT LANE` table of each of the ten merged records plus the rows still open from `open-rows-2026-11-16.md` (every old row kept; 151 old rows + 126 new rows = 277): screen 41 open / 11 closed, engine 101 / 17, plugins 15 / 0, Worker/site 16 / 5, owner decision 36 / 0, mouse drive 35 / 0: 244 open, 33 closed with evidence, 277 in all.

## UNVERIFIED

- GCC behaviour of the merged `main.cpp` (the dangling-reference search is a text analysis validated against the six known cases; nothing was built on Linux).
- The 33 catalogues' machine translations (E1 54, S1 181, S2 91, S4 85, S5 61, companions 21 keys), unread by a speaker.
- Every real-hardware, real-pointer, real-Store, real-certificate and real-folder-chooser item of the ten records (the lanes' open rows); the Worker and site, none deployed; the installer script (not compiled).
- The five load-red tests passed alone once each, not five times; `test_web_server` timed out in the loaded slice at 120 s, its cause was not looked for.
- Lane S5's real Source-row height (it draws nothing for the signal generator): unmeasured.
- The `m7-scratch` keep-both merge scripts reproduce the textual merges only for the files they were used on; the catalogues' and allow-list's order is the tools' (sorted, CRLF kept).
