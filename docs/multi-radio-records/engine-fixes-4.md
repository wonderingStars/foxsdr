# Engine fixes 4 (`engine-fixes-4`): the flaky wire-a test, the silent ABI-3 test, the drive-desktop guard, ONE needed-library table, the folded plugin ids

Date 2026-11-13. Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, branch `engine/multi-radio`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release, `--parallel 4`), scratch `D:/foxsdr-build/ef4-scratch/`.
Start tip `bbdbb8ca` (clean, verified), config.json sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the start. Nothing pushed. No download, no outside request, no radio opened.
Commits (code): d523947c (item 1), f2cba476 (item 2), fabd80e8 (item 3), 239fd23a (item 4), 76c7eab8 (item 5); this record is the last commit (`git log --oneline bbdbb8ca..HEAD`).

## Item 1. `test_engine_api_wire_a_record`, L1 "lock while recording": FIXED, the fault was in the PRODUCT

* Reproduced: the old test failed in 1 of 24 runs alone (`ctest` loop), and in 1 of 5 in a second loop with output: `FAIL tests\test_engine_api_wire_a_record.cpp:162: o.status() == FOXAPI_DENIED  (0 vs -1)`, then `r.fact("tuned.1.centreHz") == "100000000"  ("101000000" vs "100000000")` (merge 3: the gate and 3 of about 30 runs).
* Cause, one sentence: an I/Q (or audio) take opens its file on a worker (`gui::RecordStart`), the WAV is on disk while the open is still out, and the engine arms the recorder (so `recording()` reads true) only on the pass that collects the open; `WireA::lockSync` looked at `recording()` only, so a retune in that window was allowed (a real user could retune during the first moments of a locked take).
* Fix: `lockSync` (`src/engine/wire_a_recording.cpp`) also counts `iqStart_.starting()` and `audioStart_.starting()` (open out, not withdrawn) as an open take; a Stop cancels the open so nothing stays locked for a withdrawn take.
* Test made deterministic: `Access::slowIqOpenOnce(engine, ms)` in `tests/wire_a_rig.hpp` wraps the I/Q recorder's opener so the next open returns 1500 ms after the file exists; L1 retunes in that window and again after the open is collected.
* RED with the new test and no fix: `FAIL ... test_engine_api_wire_a_record.cpp:165: o.status() == FOXAPI_DENIED  (0 vs -1)` ... `test_engine_api_wire_a_record: 102 checks passed, 9 failed` (reproducible every run). GREEN with the fix: `ctest --repeat until-fail:50` = 50 of 50 Passed, `Total Test time (real) = 725.20 sec`, exit 0. The red-then-green pair on the same test (fix absent, then present) is the break-it.
* In the final gate it passed (slice 281-560).

## Item 2. `test_plugin_abi3_compat`: the failing expectation was STALE; no published plugin is broken

* The test prints SKIP and `0 checks, 0 failed` without `FOXSDR_PUBLISHED_PLUGINS_DIR` (`tests/test_plugin_abi3_compat.cpp:87`); merge 3's Passed proved nothing, and so does its pass in every ordinary gate (it passed with 0 checks in this gate too).
* How it was run: the 43 Windows binaries of the plugins repository's `master` (`git show master:plugins/<file>` of each `*-win-x64.dll`, copied to `D:/foxsdr-build/ef4-scratch/published`; same ids and versions as the user's installed set), through the hidden-desktop launcher (desktop `FoxSDRTestsEM`), `set FOXSDR_PUBLISHED_PLUGINS_DIR=...` then the exe (`D:/foxsdr-build/ef4-scratch/abi3.bat`).
* Result before: `43 of 43 published binaries loaded; 16 take the host table`, `test_plugin_abi3_compat: 147 checks, 13 failed`. After: `test_plugin_abi3_compat: 147 checks, 0 failed`. (The earlier `269 checks, 134 failed` was a different tree and plugin folder: the count is not comparable.)
* Classes: ONE assertion (line 127, 13 times): `(p.capabilities & ~0x7FFu) == 0u`. The 13 binaries (adsb-decoder, dab, demod-analyzer, ft8-decoder, nrsc5-radio-dynamic, packet-tnc, pocsag-decoder, propagation-tools, radio-email, rtty-decoder, solar-system-view, wefax-decoder, wmbus-decoder) declare 0x1000 SETTINGS_UI, 0x2000 RECEIVER_LOCATOR, 0x4000 TX_AUDIO or 0x8000 IMAGE_INPUT: defined in `src/core/plugin_abi.h:264-267`, members of `CASCADE_CAP_ALL_KNOWN` (line 269). All 43 load, none declares the level-1 bit 0x800.
* Fix: the assertion now says every declared bit is one this host still knows (`ALL_KNOWN` plus `CASCADE_FLAG_PRIVATE_DATA`), the property the mask protected (header bits are only added). Old assertion = red, new = green.

## Item 3. The drive-desktop question: a `slint_drive_*` test uses the CALLER's desktop; a guard was added

* Answer from the code: `src/slint/CMakeLists.txt:210-216` registers `test_slint_drive_run` and one `slint_drive_<script>` per `tests/drive/*.txt` (`COMMAND test_slint_drive_run <script>`, labels `gui;drive`); `src/slint/tests/test_slint_drive_run.cpp:67` starts `foxsdr-slint` with `_popen` (no `CreateProcess`, no `lpDesktop`), so the window is created on whatever desktop the runner is on; the file's own header (line 13) says it "runs on the hidden desktop under the gui label (it opens a window)": it relies on being started there. No test creates or switches a desktop (grep for `CreateDesktop`, `OpenDesktop`, `SetThreadDesktop`, `GetUserObjectInformation` over src, tests, tools found nothing) and no windowless backend is chosen. No guard existed.
* Guard: `tests/gui_desktop_guard.{hpp,cpp}` (pure decision plus a program reading the thread desktop's name with `GetUserObjectInformation(UOI_NAME)`; "Default" or an unreadable name is refused with ONE line, exit 1; override `FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1`); the CTest test `gui_desktop_guard` is `FIXTURES_SETUP gui_desktop`; every gui-labelled test has `FIXTURES_REQUIRED gui_desktop` (`tests/CMakeLists.txt` loop and the four registrations in `src/slint/CMakeLists.txt`). A refusal makes ctest report every gui test "Not Run" (counted as failed): no window, no dialog.
* Proof: `test_gui_desktop_guard: 22 checks, 0 failed` (both branches; from the normal session the real program answered `Default: exit 1`, and `FoxSDRTestsEM: exit 0`); `ctest --show-only=json-v1`: 332 gui tests (331 + the guard), 0 without the fixture; a scratch CMake project (`ef4-scratch/fixt`) shows under `-R`, `-L gui` and `-I 2,3 -j4` that with a failing setup test the dependents are "Not Run" and their marker files are never created. Break-it: `judge()` made to allow "Default": `22 checks, 11 failed`; restored.
* The GUI gate below ran with the guard in place on `FoxSDRTestsEM` (the guard passed there, so it did not refuse).

## Item 4. ONE needed-library table: DONE (the decisions (a) to (e) as given)

* `needed::Info` (`src/source/needed_library.{hpp,cpp}`) gains a `Kind` enum (library file, Soapy module, running program, software on another machine, user's file), `sourceKind`, `pageStatus`, `folderNote`, `fetchNote` and a per-row check function; `State` gains the SEVENTH word `cannot-check`; `Check` gains `depth` (`headers` / `loaded` / "").
* ONE table of nine rows (wire-b's eight, then `ftdi-d2xx` from `ftd2xxNeededInfo()` calling `checkFtd2xx`), library ids unchanged; `NEEDED_LIB_CHECK 0x8930` and the fact `needed.libs` report all nine; `wireb::NeededLibrary` and its functions are gone (`src/engine/wire_b_needed_libs.{hpp,cpp}` only write JSON).
* (a) a check loads vendor code only for `ftdi-d2xx`; every other file row reads headers only (proved by the loader counter staying still). DECISION TO READ: a header-only success is reported as `working` with `depth` "headers" (the old words `found` / `unusable` became `working` / `not-a-library`); `working` for those rows means "found, 64-bit, well formed", and the sentence says it is loaded only when the radio is opened. `ftdi-d2xx`'s `page_status` is "verified" (the FTDI site address that FTDI's own programmer's guide names; no download page is claimed); every other row stays "unverified" with no address shown.
* (d) New `tests/test_engine_api_wireb_ftdi.cpp`: `{"id":"ftdi-d2xx"}` reports the row with the six states against the stand-in `ftd2xx.dll` of `test_sdriq_d2xx` (+ check again without restart, the PATH, the loader counter): `test_engine_api_wireb_ftdi: 43 checks passed, 0 failed`. (e) `test_engine_api_wireb_neededlibs: 84 checks passed, 0 failed`, `test_sdriq_d2xx: 292 checks, 0 failed`, adapted not deleted.
* Break-it (three mutants in one build, each caught by its own lines): the ftdi row's check answering `cannot-check` gave `test_engine_api_wireb_ftdi: 22 checks passed, 21 failed`, neededlibs 3 failed, `test_sdriq_d2xx: 292 checks, 1 failed`; the header-only check made to load the library failed the loader-counter checks (`silentLoadCount() == loads0  (1 vs 0)`) in two tests; the third mutant was the item 3 guard. Restored, rebuilt, green (built twice, exit 0 both times).
* `docs/multi-radio-records/screen-handoff-L.md` section C describes the one shape; `wire-b.md` has a note that its NEEDED LIBRARIES text describes what the lane built.

## Item 5. The 18 folded plugin ids: DONE (manual, release notes, feed list)

Changed: `docs/MANUAL.md` (12 proof-table rows that named a removed id now read "<survivor>, <mode> (the former <id>; how the mode is picked)", the two shared rows split, the new-plugins rows for HF modem identifier / PACTOR monitor, EnOcean, KG-STV and Z-Wave, the CW skimmer bullet), `docs/release-notes-0.99.109.md` (PACTOR, KG-STV, Z-Wave say where they live), `tests/feed/feed-cases-new.tsv` (module prefixes of the PSK31 and EnOcean cases).
Left, with reasons: the drive scripts and their ledger rows that assert `"FT8 - PSK Reporter"` / `"Weather sensors"` (they pin the PUBLISHED binaries `ft8-decoder-1.1.1` / `weather-decoder-1.0.0` and `kit.plugins == 43`); lane records (history); the fixture file names and generator functions; ADIF mode names in `qso_log.hpp`; helper-program rows of the manual. Full list in the commit message of 76c7eab8.
Evidence that the feed rows point at real survivors (plugins repository's built binaries, read only, headless, hidden desktop): PSK31 against `hf-keyboard-modes-1.2.0`: `PASS psk31-decoder-gen 15.2s ... 1500 Hz: CQ CQ DE FOXSDR FIXTURE K`. EnOcean against `short-range-frames-0.4.1`: FAIL (`unmet: EnOcean RORG 0xF6 data 30 && ...`; the survivor prints `EnOcean RORG 0xF6 (RPS) data 30 ...`). The removed `enocean-monitor-0.2.0` gives the identical FAIL, so the stale expectation predates the fold; left unchanged (not weakened): its owner decides the new wording. `D:/foxsdr-build/prep/DRIVE-PLAN.md` was not touched.

## Gates (code tip 76c7eab8; binary stamp `d523947c61ee-dirty`, see below)

Headless + slint, `ctest -C Release -L "headless|slint" -I a,b -j4 --timeout 300`, 1122 tests in four slices through `ef4-gate.bat` started by the hidden-desktop launcher (desktop `FoxSDRTestsEM`), foreground:

| Slice | Result |
|---|---|
| 1-280 | `99% tests passed, 3 tests failed out of 280` (304.91 s): test_airband_data, test_band_plan, test_converter_call_sites |
| 281-560 | `99% tests passed, 1 tests failed out of 280` (608.72 s): test_page_resize_id |
| 561-840 | `99% tests passed, 1 tests failed out of 280` (321.92 s): test_png_decode |
| 841-1122 | `99% tests passed, 2 tests failed out of 282` (331.61 s): test_health_app_stepped, test_diagnostics_free |

1122 run, 1115 passed, 7 red. The five known false failures (test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode) are red as in every earlier record. `test_health_app_stepped`: load flake, `Passed 22.78 sec` alone (hidden desktop). `test_diagnostics_free`: stale build stamp, `git HEAD now: 76c7eab8f9e6, binary says: d523947c61ee-dirty` (the binary was configured at the item 1 commit and built with the later items uncommitted); it needs a rebuild at the commit that carries this record, and any later commit makes the stamp stale again, so its result after that rebuild is in the final report of the lane, not in this file.
GUI: `ctest -L gui -j3 --timeout 300` through the launcher on `FoxSDRTestsEM` with `FOXSDR_FRAME_CAP_HZ=60` (`ef4-gui.bat`), 332 tests (the 331 of merge 3 and `gui_desktop_guard`): `99% tests passed, 1 tests failed out of 332`, `Total Test time (real) = 2474.93 sec`. The one red is `test_diagnostics` (the same stale build stamp as `test_diagnostics_free`, not a defect). `gui_desktop_guard` was Test 1/332 and Passed on the hidden desktop; every other gui test passed, including all the `slint_drive_*` scripts (255 tests labelled drive).

## Config hash

`%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the start; at the end (after the gates): c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3, unchanged (`sha256sum` of the real file; no test opened the real file).

## UNVERIFIED

* Nothing for Linux was built. No real FTDI / WiNRADiO / SoapySDR vendor library, no radio and no web page was touched.
* Item 1's fix covers the I/Q and audio takes (`starting()`); a take queued behind a finishing one (`iqStartQueued_`) is not treated as open until it starts.
* `test_plugin_abi3_compat` proves 43 published Windows binaries load on this host; it passes with `0 checks` in every ordinary gate unless the variable is set.
* The header-only reading of `working` for the file rows (item 4) is a decision the owner may want to change (an eighth state or the old words).
* The EnOcean feed expectation (item 5) is stale against the plugin's own output and was not rewritten; the A5 line of its output was not seen.
* The 12 vs 13 count: the commit message of 76c7eab8 says "13 rows"; twelve rows and two shared rows were edited.

## Item 6 (follow-up). A take queued behind a finishing one counts as open: FIXED

* Reachable from the public operations: stop a take (its file closes on a worker), press `RECORD_IQ 1` before the close ends: `Engine::iqStartQueued_` (audio: `audioStartQueued_`) is set and the take starts when the close ends (`engine.cpp` `startIqRecording`, `pollRecordFinishes`).
* Reproduced deterministically: new section L2 of `test_engine_api_wire_a_record` makes the close slow (`Recorder::setFinishHookForTest`, 2500 ms), stops a locked take, presses Record behind it and retunes. RED without the fix: `FAIL ...test_engine_api_wire_a_record.cpp:257: o.status() == FOXAPI_DENIED  (0 vs -1)`, `("102000000" vs "100000000")`, `test_engine_api_wire_a_record: 123 checks passed, 3 failed`.
* Fix: `WireA::lockSync` also counts `iqStartQueued_` and `audioStartQueued_` as an open take (commit 90ba734d). GREEN: Passed; the red-then-green pair is the break-it. Loop alone: `ctest --repeat until-fail:20` = 20 of 20 Passed, `Total Test time (real) = 438.68 sec`, exit 0.

## Item 7 (follow-up). The EnOcean feed case: the EXPECTATION was stale (fixture and plugin are right)

* Fixture: `tests/feed/feedsignals.hpp` `enocean()` builds three ERP1 telegrams (RPS F6 data 30, 1BS D5 data 09, 4BS A5 data 00 00 55 08, three sub-telegrams each; CRC-8 poly 0x107; 8-to-12 line code), consistent with what the plugin decodes (it printed the F6 and D5 telegrams with `CRC-8 ok`).
* Plugin: `mode_enocean.h:157` prints the RORG type name since enocean-monitor 0.2.0, and the plugin's own tests pin it (`test_enocean.cpp:583-591`: `RORG 0xA5 (4BS) data`, `0xF6 (RPS) data`, `0xD5 (1BS) data`; read only in plug-merge). No plugin fault, no hand-off.
* Fix (commit 103034f7): the expectation in `feed-cases-new.tsv` now carries `(RPS)`, `(1BS)`, `(4BS)`; data bytes unchanged. Run against the built `short-range-frames-0.4.1` (headless, hidden desktop): `PASS enocean-monitor-gen 1.6s`, `feed check: 2 PASS, 0 FAIL, 0 SKIP`.
* Item 4 note: `screen-handoff-L.md` section C has a new row: a `working` with `depth` "headers" is shown as "Found (not test-loaded)", never "Working" (only `depth` "loaded", the `ftdi-d2xx` row, may be "Found and working").

### Gate for items 6 and 7 (code tip 103034f7)

Headless + slint, 1122 tests, four slices through the launcher: `99% tests passed, 3 tests failed out of 280` (308.62 s: the known test_airband_data, test_band_plan, test_converter_call_sites); `99% tests passed, 1 tests failed out of 280` (608.47 s: test_page_resize_id); `99% tests passed, 2 tests failed out of 280` (320.41 s: test_png_decode and test_tci_engine); `100% tests passed out of 282` (329.13 s). 1116 passed, 6 red (3 + 1 + 2 + 0): the five known false failures plus `test_tci_engine` (corrected in item 8: this line first said 1117 passed, 5 red). `test_tci_engine` failed alone (`tests/test_tci_engine.cpp:474  strtoull(sentFact) >= got.size()`, `fact.udp.sent = "151", datagrams received 152`), then passed twice and failed once in a repeat of 6: a pre-existing INTERMITTENT race in the TCI UDP counter (wire-c's test; this lane touches nothing of it: `git diff bbdbb8ca HEAD` over `src/engine/tci*` and the test is empty); it passed in the first gate of this lane. Not fixed here. No GUI gate: no gui-labelled test and no `.slint` file changed. Stamp tests: their result after the rebuild at the final commit is in the lane's final report. config.json sha256 at the end: c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3, unchanged.

## Item 8 (follow-up). `test_tci_engine` intermittent: the TEST read the counter too early; FIXED in the test

* Measured alone (plain ctest, it opens no window): 10 of 30 failed with `tests/test_tci_engine.cpp:474  strtoull(sentFact) >= got.size()`, `fact.udp.sent = "151", datagrams received 152`; 20 passed.
* Cause: `UdpAudioSender` counts a packet after `sendto` returns (`src/net/udp_audio.cpp:165-168`) and `fact.udp.sent` reads that atomic live (`fox_engine_api.cpp:1022`); the test read it while the stream still ran, right after its last `recv`, so the datagram taken last could be delivered a few microseconds before the counter moved. Counting after the send is inherent; a settled-state reader never sees it behind, so the product is unchanged.
* Fix (commit 1798f2cb): the test polls the fact (10 ms steps, bounded 2 s) until it has caught up with the datagrams received; the assertion `sent >= received` is unchanged. After: 30 of 30 Passed alone. The 10-of-30 red before is the break-it (assertion unchanged, wait absent).
* Correction to section 7: its slice list is 3 + 1 + 2 + 0 = 6 red, so 1116 passed (the five known false failures plus `test_tci_engine`), not "1117 passed, 5 red"; the line is corrected in place.
* Gate for this item (tci tests, slice 561-840 and the stamp tests after the rebuild at the final commit): in the lane's final report.
