# Engine merge lane 9 (`engine-merge-9`): screen lane S4b (the outputs follow-on) and its security fixes into `engine/multi-radio` (2026-11-17)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `054b5a99` (clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, `FOXSDR_SLINT_CLIP_PROBE=OFF`, Release, `--parallel 6`), scratch `D:/foxsdr-build/m9-scratch/`. Every test ran through the hidden-desktop launcher on desktop `FoxSDRMerge9`, `FOXSDR_FRAME_CAP_HZ=60`, `ctest -j3` (reruns `-j1`); `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set; no radio opened, nothing transmitted, nothing pushed, downloaded or deployed; no other worktree touched. Two other lanes were building in theirs, so the machine was loaded (section 3 says what that did).

## 1. The merge

Tip checked before merging: `feat/screen-s4b-sec` = `8f7efef6`, exactly as briefed. The branch was cut from lane S4's tip `1d127941` (before engine merges 7 and 8); its 16 own commits are lane S4b (`27bc994e` and below) plus five security commits.

Merge commit **`08f8a39b`** (`git merge --no-ff`). Conflicts, 42 files, all settled by keeping both sides:

| File | How settled |
|---|---|
| `resources/lang/*.json` (33) | union of keys key by key against `1d127941` (`m9-scratch/lang_union.py`: no key changed on both sides, output format identical to the old files); no hand merge |
| `src/core/lang_assets.hpp` | regenerated with `tools/embed-lang.py`; `tools/i18n_keys.py`: 33 catalogues, 4772 keys each (4628 at merge 8), 0 empty, 0 missing, 0 stale; the Korean word-spacing check is in `test_i18n`: Passed |
| `src/engine/wire_c.cpp` | lane S3's `rec.*` keys kept, lane S4b's `srv.*`, `rot.*`, `swl.*`, `listing.*` keys added beside them |
| `src/slint/CMakeLists.txt`, `src/slint/ui/main.slint` | the S2/S3/S5/X lines kept; the S4b source line and the `LaneS4b` export added |
| `src/slint/tests/CMakeLists.txt` | git had interleaved the `s4b_host` family with the shared tail of the `s5_host` block; the s5, S3 and lane X families kept and `s4b_host` rebuilt as its own block |
| `src/slint/main.cpp` (7 hunks) | includes, member, answer and state-name lines both kept; git had interleaved the S4b publisher block with the S5 block: rebuilt as its own block after lane X's |
| `tests/errpath_allow.tsv` | key by key against `1d127941` with merge 7's `merge_tsv.py` (no key changed on both sides), then re-seeded (section 4) |
| `tests/thread_inventory_rows.inc` | S4b rows added; `server_host.cpp` keeps the later engine row (3, 4, 4) |
| `tests/test_engine_publish_alloc.cpp` | both lanes' bounds replaced by the measured ones (section 4) |

Merge 7's and 8's later fixes won where they and the branch touched the same line: the CMake test-family compile lines, the `takeUnclaimedResults` loop, the web checks and the wire-c version rules (see fix 3 below, the one place the branch lacked a fix of merge 7).

**SHARED FILE EDITS of both records, grepped after the merge** (all present): `main.slint` import, export and the two mount lines (`system-extra-base + 11` has the S4 and S4b panels beside lane L's); `main.cpp` (3 includes, the `s4bp` member, the `recordingApi(api)` line, the notifier line under `!bounded`, the block, tick, answer, state names); `src/slint/CMakeLists.txt`; `src/slint/tests/CMakeLists.txt`; `wire_c.cpp/.hpp` (keys, state fields `listingVersion`, `serversSettingsVersion`, `serversSig`, `serversHooked`); `engine_wire_c.cpp` (ops, facts, hooks, the settings pass); `engine_wire_b.cpp` (the network I/Q refusal while the rtl_tcp server holds a radio); `engine.hpp` (`setDesktopNotifier`); `app_commands.hpp/.cpp` (the three ops, `FOXAPP_OP_ROTATOR` 0x8A20, `SWL` 0x8A21, `MACRO_EDIT` 0x8A22); `macros.hpp` (denied prefixes `APP_ROTATOR`, `APP_SWL`, `APP_WIREC_SETTING`); `secret_vault.cpp` (`srv.mcp.token`); `net_sock.hpp/.cpp` (`peerOf`); `tests/wire_c_rig.hpp` (the 16-line helper patch applies in reverse); `thread_inventory_rows.inc` (`api_server.cpp` 2 threads); catalogues. The root and `tests/CMakeLists.txt` were not edited by the branch (the tests register by the existing lists).

## 2. Fixes after the merge (each its own commit; no test or check weakened)

| Commit | What |
|---|---|
| `938f8025` | the error-path pin re-seeded, the two publish-allocation bounds measured (section 4) |
| `89576848` | `test_sys_bundle`: `testServerSettingsNeverArrive` (section 5) |
| `c1840306` | **`test_apply_command` (and `_stepped`, `_free`)**: `FAIL: no case for APP_ROTATOR / APP_SWL / APP_MACRO_EDIT`. The test requires a case for every op of the table; lane S4b added three ops and its own gate was a 66-test selection that did not include this test. Three cases added (`covering(op)` and a `FOXAPI_BAD_ARGUMENT` refusal of a text that is not JSON): 2353 checks, 3 failed, to 2359 checks, 0 failed |
| `dad3ac0a` | **`test_json_dump_policy`**: five `dump()` sites of the branch without `error_handler_t::replace` (`s4b_swl.cpp:184`, `s4b_logic.cpp:193`, `s4b_publish.cpp:271,283,364`): the base's policy test (crash 6477BA87) post-dates the lane. The five calls now pass the handler; compact output of a valid text is byte-for-byte what it was |
| `bfa3a862` | **`test_engine_api_03`**: `FAIL test_engine_api_03.cpp:2647: s.state().listSeq == list0 (2 vs 1)` alone (38.9 s). The servers block of `applyWireC` raised `wireCVersion_` on its very first pass in three places (the first configure, the first listing build, the first signature) with nothing changed: the same fault engine merge 7 fixed for lane S4's outputs block. Fixed in that block's style: the first pass records the unconfigured signature as the baseline, does not count the first configure, and counts the first listing build only when it produced a preview or a problem; a first pass that really changes something (a saved switch that starts a server, a listing turned on) still moves the version. `test_engine_api_03` Passed (34.1 s) after |

## 3. Gate results

Final code state for the gates: full `cmake --build` after the last source change (`bfa3a862`), exit 0. Commits after it are documentation only.

**Full headless gate** (`ctest -L "headless|slint" -j3 --timeout 300`, 1235 tests; 2 skipped by their own rule, `test_plugin_abi3_compat` and `test_rtlsdr_live`), run twice:

| Run | Result | Reds |
|---|---|---|
| first, at the merge commit's build (`Total Test time 2137.71 s`) | 7 failed | `test_apply_command` x3, `test_engine_api_03`, `test_json_dump_policy`: REAL, merge/branch faults, FIXED (section 2). `test_diagnostics_free`: STAMP test, I committed the first fix during the run so the binary's stamp was one commit behind (failing line not captured); Passed after the rebuild at `bfa3a862`. `test_patch_dest_async` (30.2 s): known load flake of the standing rules; Passed alone 5 of 5 (25.1 s) |
| second, at `bfa3a862`'s build (`Total Test time 2142.12 s`) | 4 failed | `test_discord_presence` (Timeout 120 s), `test_engine_api_wireb_savedkinds` (64.7 s), `test_sdrplay_lowif`, `test_snr_tap`: LOAD (two other lanes were building; none of the four failed in the first run). Each Passed alone, `--repeat until-fail:5 -j1`: `discord_presence` 5.2 s, `savedkinds` 6.5 s, `sdrplay_lowif` 5.7 s, `snr_tap` 0.1 s |

The stamp tests `test_diagnostics` and `test_diagnostics_free` Passed at `bfa3a862` (in the second gate and alone, 5.06 s and 2.60 s). The commit that adds this record is documentation only: a stamp test run on a build made before it reads the older commit.

**Full GUI gate** (`ctest -L gui -j3`, once, at `bfa3a862`'s build): `100% tests passed out of 420`, `Total Test time 3349.83 s`. In it: `slint_drive_s4b-outputs` (38.1 s), `slint_drive_s4b-lockout` (24.2 s), `s4-outputs`, `s4-scan`, `slint_drive_lanel-libs`, the three `companions-website` drives, `layout-section-overlap-1600/-624`, `test_web_touch`, `slint_drive_look-stamps` (Passed this time, 21.1 s).

**The two drives the security lane left unrun** (`s4b-outputs`, `s4b-lockout`): both Passed UNCHANGED, before the fixes (`a4`, 37.9 s and 24.2 s) and in the final GUI gate. The new allow-list rows appear only when "every address" is ticked, which no drive does (open row `S4bS-drive-allow`); nothing in the scripts was wrong and no product guard was touched.

Selected runs on the merged tree: the 27 S4b and security tests (`test_s4b_sec_*`, `test_engine_api_s4b_*`, `test_api_server`, `test_rtl_tcp_server`, `test_tcp_audio`, `test_tcp_fanout`, `test_ws_frame`, `test_mcp_server`, `test_macros`, `test_s4_settings`, `test_wire_c_portable`, `test_slint_s4b_host`, `test_slint_s4_host`, `test_s4_outputs`): `100% tests passed out of 27`; the 60 tests of both security lanes together (section 5): `100% tests passed out of 60`.

**`slint_drive_look-stamps`, 30 runs alone** (`look.bat`, one `ctest -R` per run on the hidden desktop, nothing else of mine running, the other lanes still building): **27 passed, 3 FAILED** (runs 3, 16 and 30). The failing output, each time: `state wf.tick1 contains  s -> FAILED: it is -` (runs 3 and 16, script line 17, right after `choose "Time stamps" "Age (seconds ago)"`) and `state wf.tick1 contains : -> FAILED: it is -` (run 30, line 14, right after choosing Clock time, local); the other 14 steps of every failing run passed, 589 to 590 frames were painted and the host tick time was normal (mean 0.84 ms). `-` is what `main.cpp` prints when the waterfall has fewer than two ticks. All three failures are at the FIRST read after a mode switch (three of about ninety such reads), which narrows it to the figure built for the pass that follows the switch. Code reading, NOT proven (no instrumented build was made): `ageTicks` returns the single tick "now" when the history (`wfH * secondsPerRow`, `main.cpp` ~2727) is 0 or shorter than the first step. Row `M8-look-stamps` carries this and the next step (a diagnostic build printing history, height and row spacing of the pass that made a one-tick figure). The rate on this machine (10 percent) is higher than at merge 8 (about 3 percent); it passed in both full GUI gates of this merge.

## 4. Pinned numbers and changed expectations (every one, both values)

| Where | Was | Now | Inputs and reason |
|---|---|---|---|
| `kPendingPin` (`test_error_paths.cpp`) | 459 (merge 8; 1125 on the S4b lane's old base) | 459 | union key by key against `1d127941`, then `tools/errpath_seed.py` on the merged scanner: 3471 sites, 3228 allow lines kept, 0 re-keyed, 0 dropped, 0 sites without a survey row. Arithmetic: 459 (base) + 0 (lane S4b brought 34 ALLOW lines, no PENDING line) + 0 (security lane: no allow line, none raised) = 459. The PENDING key set is IDENTICAL to merge 8's (`diff` of the 459 `key + group` pairs is empty) and the groups are merge 8's: reports-not-in-function 168, O-outputs 55, unsure 45, R-record 27, G3-control-refused 27, G10-gpu-fft 16, D-gps-serial 13, G2-stream-health 12, T-tx 11, F-files 11, unmatched 9, S2-window 7, S1b-headless 7, A-audio 7, S5-driver-install 5, S3-plugin-ui 5, P-store 5, P-plugin 5, S9-report-upload 4, S8-sdrplay-service 4, S6-scan 4, S7-ui-engine 3, S1-startup 3, S4-first-run-check 2, G1-device-dead 2, PL2-companion 1, G7-patch-radio 1. The lane's output-failure sites were already inside group O-outputs (55), so no PENDING line is "its own". The 34 new ALLOW lines: `tcp_audio.cpp` 4, `rtl_tcp_server.cpp` 6, `api_server.cpp` 6, `s4b_mcp.cpp` 5, `s4b_notify_win.cpp` 3, `s4b_rotator.cpp` 5, `s4b_swl.cpp` 2 and others, each a validation or UI sentence answered to the person or program that asked |
| `test_engine_publish_alloc.cpp` F3 bound | 2550 (merge 8; the lane's own 2500) | **2800** | MEASURED on the merged tree (the test's own "the rebuild" lines): 2526 to 2535, max 2535; 2535 x 1.10 = 2788.5, rounded up to the next 50. The settled passes stay 0. Set once |
| same, F12 bound | 2200 (merge 8; the security lane's own 2010) | **2400** | MEASURED 2163 to 2164 (1946 to 1947 at merge 8); 2164 x 1.10 = 2380.4, rounded up to 2400. Set once. The growth of about 217 in each is lane S4b's facts of the servers, rotator, listing and listening log and the security lane's two settings (not split per lane) |
| `test_apply_command.cpp` | 2353 checks, no case for the three ops | 2359 checks, a case for each | section 2 |
| `test_sys_bundle.cpp` | 7660 checks | 7691 checks | the new test, section 5 |
| `lang_assets.hpp`, the 33 catalogues | 4628 keys | 4772 keys, 0 missing, 0 stale | regenerated |
| `layout-section-overlap-1600` / `-624` | 2292 (and `Sinks` top 2480 unmeasured) | unchanged, Passed | lane S4b's rows sit in the Scan and Outputs banks; the drives pinned at 2292 still pass |
| test inputs of the branch | `Host: x` in `test_api_server` (15 requests) and `test_engine_api_s4b_control` (1) | `Host: 127.0.0.1` | the lane's own change (the server now judges Host); no expected status changed |

No expectation of a base test was changed to make it pass.

## 5. The checks the brief asked for

**The two security lanes together.** `feat/security-fixes-1` (in the base): Host and Origin guard on the built-in web page (`web_guard`), a password-less page is read-only, `importLeftOutKeys`. `feat/screen-s4b-sec`: the program interface's own Host rule, an auth worker, a bound per address, open streams ended when the web account changes, a typed client list for rtl_tcp and audio over TCP. Checked by reading both and running both:
- The 60 tests of both lanes ran in one invocation and `100% tests passed out of 60`: `test_web_*` (server, guard, secure, tls, touch, policy and the rest), `test_server_host`, `test_remote_key_release`, `test_stop_ends_recordings`, `test_engine_api_wire_c_web`, `test_s4b_sec_api/_servers/_peers/_swl`, `test_engine_api_s4b_*`, `test_api_server`, `test_sys_bundle`, `test_sys_setup`, and the secret vault, TCI, RDS log, radio report and companion suites.
- No contradiction: the interface's Host rule (localhost, 127.x, `[::1]`, the bound address) is STRICTER than the page's (any IP literal, `localhost`, the computer's name, `name.local`, the "Allowed web origins" hosts). Beyond this computer ("every address") the interface asks for the web page's OWNER account (`engine_wire_c.cpp` `h.webAccount` = `servers_.config().web` username and password record; the listener role is not accepted) and, with the web password removed, lets nobody in (R8d, tested). On this computer only, a password-less interface is open to local programs (Host checked, any Origin refused); the page in the same state is read-only. That is a difference of design, not a conflict: row `M9-api-host-rule` asks whether the two Host rules should be one.
- Removing or changing the web password ends the interface's open streams: proved by `test_s4b_sec_servers` A and B (change seen within 0.5 s, streams closed, old login refused) against a STAND-IN account hook. The real hook was read; no test drives it end to end through the real `SERVER_CONFIG` (row `M9-api-account-e2e`, UNVERIFIED end to end).

**Import guard.** `git diff 054b5a99 HEAD -- src/core/config.cpp src/core/config.hpp src/core/sys_bundle.cpp` is empty: lane S4b added no `config.json` key. Every `srv.*`, `rot.*`, `swl.*` and `listing.*` switch lives in `wirec.json` (`engine/wire_c.hpp`), the listening log and the macros in files of their own, and a bundle carries `config.json`, `bookmarks.json` and `markers.json` only (`allowedBundleFileName`). So nothing joins `importLeftOutKeys()`. Nothing proved that part, so `testServerSettingsNeverArrive` (commit `89576848`): `wirec.json`, the listening log and its `.damaged` copy, the macro files and others are refused as names on parse AND on export, and a `config.json` carrying the server keys imports without them reaching the guarded list. BREAK-IT: `"wirec.json"` added to `allowedBundleFileName`: `test_sys_bundle: 7691 checks, 3 failed` (first at `test_sys_bundle.cpp:397`, the export refusal); restored: `7691 checks, 0 failed`.

**Dangling-reference search** (the `d3519455` pattern, `m7-scratch/dangle.py`): 20 bare blocks in `main()` (19 at merge 8; the S4b block is the 20th), 0 candidates. Windows-only proof.

## 6. New engine surface and behaviour a user will notice

Ops `FOXAPP_OP_ROTATOR` 0x8A20, `FOXAPP_OP_SWL` 0x8A21, `FOXAPP_OP_MACRO_EDIT` 0x8A22 (local sessions only); wirec keys `srv.tcpaudio.*`, `srv.control.radio`, `srv.rtltcp.*`, `srv.api.*`, `srv.mcp.*`, `rot.*`, `swl.*`, `listing.*`, with `srv.tcpaudio.allow` and `srv.rtltcp.allow` from the security lane. Every server is OFF by default and listens on this computer only. For the release notes (from the two lane records): with "Let other computers connect" ticked, a field "Computers allowed to connect ..." appears and until it holds an address or network the server does not start beyond this computer; an rtl_tcp connection that does not begin with an rtl_tcp command is dropped; the program interface refuses a Host that is not this computer's (403) and ends its open connections when the web password changes; a damaged `swl-log.json` no longer stops the program starting (kept as `swl-log.json.damaged`).

## 7. Open rows and the config hash

`docs/multi-radio-records/open-rows-2026-11-17.md` refreshed in place. Counts by who takes them, before 285 open, 40 closed, 325 in all; after **290 open, 50 closed, 340 in all**: screen 47 open / 15 closed, engine 119 / 29, plugins 15 / 0, Worker/site 18 / 5, owner decision 46 / 0, mouse drive 45 / 1. Closed with evidence: `S4-G2-43`, `S4-G3-22`, `S4-G3-24`, `S4-G3-30`, `S4-G3-31-32`, `S4-G3-34`, `S4-G3-37b` (the code), and the merge's own `M9-s4b-lane-gate`, `M9-load-reds`, `M9-import-wirec`; narrowed in place: `S4-G3-20-21`, `S4-G3-49-50`, `S4-O-outputs`, `M7-publish-alloc-f12`, `M8-look-stamps`, `M8-gcc`. New rows: `S4bS-tci-udp`, `S4bS-notifier`, `S4bS-hub-bucket`, `M9-api-host-rule`, `M9-api-account-e2e`, `S4bS-drive-allow`, `S4b-l10n`, `S4b-MD-rows`, `-clients`, `-notify`, `-rotator`, `-listing`.

The real `%APPDATA%/foxsdr/config.json` sha256 after the last test run: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged). Nothing is running: every build and test was run to its end in the foreground; nothing was pushed.

## UNVERIFIED

- GCC behaviour of the merged tree (nothing built on Linux; the POSIX halves of the S4b servers and the security lane were never compiled).
- The ending of the program interface's streams on a web-password change, end to end through the real server configuration (stand-in account only; `M9-api-account-e2e`).
- The new Outputs rows and the two allow-list fields by eye (no drive ticks "every address"); every real-client, real-notification, real-rotator and real-directory item of the S4b records (`S4b-MD-*` rows); the 147 machine translations of the two lanes.
- The cause of `slint_drive_look-stamps` (3 of 30 alone; code reading only).
- The first `test_diagnostics_free` red (the failing line was not captured; the stamp explanation fits the commit made during that run and it passed after the rebuild).
- The share of the F3 and F12 allocation growth that belongs to each lane.
