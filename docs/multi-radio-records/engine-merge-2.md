# Engine merge lane 2 (`engine-merge-2`): serial-a and the three OSS lanes merged into engine/multi-radio

Date 2026-11-12. Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release, `--parallel 4`). Start tip `090e7039` (tree clean, as ordered).
Method: the first merge lane's record `engine-merge.md` (same build folder, same gate commands, GUI gate on the hidden desktop `FoxSDRTestsEM` with `FOXSDR_FRAME_CAP_HZ=60`).
Nothing pushed. No download, no outside connection. The code tip is `e162aa1a`; the commit that adds this file sits on top and changes only this file.

## 1. The merges (`git merge --no-ff`, in the ordered sequence)

| # | Branch (tip, verified equal to the commit ordered) | Merge commit | Conflicts and resolution | Tests run after (real) |
|---|---|---|---|---|
| 1 | feat/engine-serial-a (25ba3bea; 181 commits behind the tip) | f16c0e65 | `tests/thread_inventory_rows.inc`, one hunk: HEAD had the `sys_keep_awake.cpp` and `sys_resume_watch.cpp` rows (from sysmodels), the serial-a side only the `sys_device_watch.cpp` row, which HEAD already had byte for byte (serial-a took `sys_device_watch.*` from sysmodels). Kept every row of both sides; checked no file name appears twice. `src/core/config.cpp` and `tests/CMakeLists.txt` auto-merged. The `test_config` 8 MB stack fix and the uninitialised VFO member fix (F6) both came in unchanged. | 42 tests (every test file serial-a added or changed, plus `test_thread_inventory`, `test_health_paths`): 41 passed; `test_pipeline_afc` Failed once in that batch (28.06 s), then Passed 4 times alone (27.5, 27.2 x3) and in a second parallel batch (27.33 s). See 3b. |
| 2 | feat/oss-radios (659a7973) | 0f19d680 | none (clean) | 21 tests (`sdrplay_rate_map`, `ascp_maker_spec`, `ascp_source`, `sdriq_source`, `icr8600_source`, `funcube`, `hackrf_sweep`, `hl2`, `usb_*`, `thread_inventory`, `health_paths`, ...): `100% tests passed out of 21` |
| 3 | feat/oss-net (8574a600) | 93562882 | add/add `src/source/THIRD-PARTY-NOTICES.md` (oss-radios and oss-net both create it): both texts kept in full, oss-radios text first, one blank line between, CRLF kept (454 lines; no marker left). Follow-ups in the same merge commit: `docs/api/openapi.yaml` path `/status.json` (section 2) and a fix to the OpenAPI test's heuristic (section 2). | 15 tests (`spyserver_source`, `aaronia_source`, `freq_import*`, `receiver_listing`, `oss_net_sinks`, `web_server`, `web_secure`, `packaging_openapi_routes`, `thread_inventory`, `health_paths`, ...): `100% tests passed out of 15` |
| 4 | feat/oss-libs (13595681) | dd1117b5 | none. Collision check: its one thread-inventory row (`src/source/vendor_apartment.cpp`) repeats no file in the table (a duplicate-file-name scan of the table after the merge printed nothing); its `Recovered::ReaderLeak` row in `test_health_paths` is for `vendor_apartment.cpp` and serial-a added no row there. | 8 tests (`vendor_lib`, `g39ddc_source`, `g33ddc_source`, `sdrconnect_source`, `aaronia_source`, `thread_inventory`, `health_paths`, ...): `100% tests passed out of 8` |
| fix | (this lane) | e162aa1a | not a merge: section 3 | `test_engine_shell_guard` Passed (19.01 s) |

Each merge was followed by a build (exit 0 each time, a second build pass to pick up the globbed new test targets, exit 0). Merge 2 was built and tested before merge 3 was started.
Note on process: after the merge commit of step 2 I started step 3 before building step 2, noticed, ran `git merge --abort` (tree clean), built and tested step 2, and only then repeated step 3.

## 2. `/status.json` in the OpenAPI file, and the test heuristic

`tests/packaging/test_openapi_routes.py` demands every `svr.Get` route be documented. feat/oss-net adds `GET /status.json`.
* `docs/api/openapi.yaml` gains the path `/status.json` (tag `page`, `security: []`, responses 200 and 404). Written from the handler (`src/net/web_server.cpp`: `listingDocument()` empty gives 404, else the document as `application/json`; the request is also not counted as usage) and from `listingJson()` in `src/net/receiver_listing.hpp`, read in full: `receiver{name, location, gps{lat, lon} only when both were typed}`, `max_clients` (1..1000), `version` (at most 32 printable ASCII), `sdrs` (always one entry: `name` = the receiver name, `type` = "FoxSDR", `profiles[]` of `name`, `center_freq` and `sample_rate` as rounded integers, at most 32). The 404 is "off (the default) or never set". The live server was not started: the schema is documentation of code I read, not of a response I saw (UNVERIFIED against a running server; `test_receiver_listing` Passed and tests the same builder).
* The first run of the OpenAPI test with the new path gave `FAIL  GET /status.json: documented status 200 can really be answered  handler answers: 404`: `handler_statuses()` decides an implicit 200 only when `res.status` does not occur in the three lines before `res.set_content`, and this handler sets 404 inside an `if` block that returns, then answers 200 below. Fixed in the test: a `res.status` that is followed by a closing brace before the `set_content` no longer hides the 200. No check removed or loosened toward passing; the other routes read as before. Result: `725 checks, 0 failed` (it was 724 with one failure; the extra check is the new route's 200).

## 3. The one test red caused by a merge, and its fix (commit e162aa1a)

`test_engine_shell_guard` FAILED in the headless gate: `FAIL: src/net/THIRD-PARTY-NOTICES.md is not a .cpp, .hpp or .h file directly in the directory - the fence's stubs (cmake/interface_fence.cmake: *.hpp and *.h) cannot shadow it` (`tests/test_engine_shell_guard.cpp:700 strangers == 0`, `279 checks, 1 failed`). feat/oss-net added a 6-line notice (item 4, "nothing was ported") in `src/net`, a directory the test holds to `.cpp/.hpp/.h` so the interface fence can deny it whole. Fix: `git mv src/net/THIRD-PARTY-NOTICES.md docs/third-party-notices-net.md` (content unchanged, first line names the new place) and the one line of `docs/feature-records/oss-net.md` that named the old path. The test is not changed. After: Passed.
`test_json_dump_policy` PASSED in the gate (and alone, 0.34 s): confirmed fixed by the base commit 090e7039.

## 3b. Gates

**Headless + slint** at `dd1117b5` (before the e162aa1a move), `ctest -C Release -L "headless|slint" -I a,b -j4 --timeout 300`, 1071 tests in slices of 280 (slice 5, 1121-1400, found no test; 280 + 280 + 280 + 231 = 1071):

| Slice | Result |
|---|---|
| 1-280 | `99% tests passed, 3 tests failed out of 280` (362 s): `test_airband_data`, `test_band_plan`, `test_converter_call_sites` |
| 281-560 | `99% tests passed, 4 tests failed out of 280` (764 s): `test_engine_shell_guard`, `test_page_resize_id`, `test_pipeline_afc`, `test_plugin_store_window` |
| 561-840 | `99% tests passed, 1 tests failed out of 280` (350 s): `test_png_decode` |
| 841-1120 | `99% tests passed, 1 tests failed out of 231` (466 s): `test_health_app_stepped` |

Total 1071 run, 1062 passed, 9 red. Classification (re-runs alone, through ctest, one at a time):

| Red | Class | Evidence |
|---|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | the five known false failures | red as in every earlier record; not investigated |
| test_engine_shell_guard | **caused by the merge**, fixed | section 3; Passed after |
| test_plugin_store_window | load flake | Passed alone (25.07 s) |
| test_health_app_stepped | load flake (earlier records: the same) | Passed alone (55.35 s) |
| test_pipeline_afc (serial-a's) | load flake, failure text NOT captured | red in two loaded batches (28.06 s and about 28 s); Passed alone 4 times plus once more in an 11-test parallel batch with `-j6`, 27.2-27.5 s each. The test sleeps wall-clock intervals (4 s drift, 1.5 s holds) and judges the tracker against them, so a loaded machine can move a result; I could not reproduce the red, so the cause is unproven. |

Known from serial-a and not red here: `test_diagnostics_free`, `test_engine_main_password_not_in_argv` both passed in this gate. No `--repeat until-pass` was used.

**GUI** (`ctest -L gui -j3 --timeout 300`, hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`) on the dd1117b5 build: `99% tests passed, 3 tests failed out of 319`, `Total Test time (real) = 3120.28 sec` (the PC was at 100% CPU with other lanes' builds; the first record took 2480 s). Reds, re-run alone on the hidden desktop:

| Red | Class | Evidence |
|---|---|---|
| test_slow_frames_app | load flake (in the shared "Load flakes" list) | failed in the gate at 38.33 s; Passed alone (29.83 s) |
| test_stop_ends_recordings | load flake (the first record: the same, `slowLine.empty()`) | Passed alone (50.52 s) |
| test_diagnostics | stale build stamp, not a defect: `git HEAD now: e162aa1ac5f3, binary says: dd1117b5e129` (the binary was built before the e162aa1a commit moved HEAD) | after the final commit the app is rebuilt and the test re-run, section 7 |

## 4. ABI, configuration, tables

* `src/core/plugin_abi.h` (the plugin ABI header in this tree; there is no `include/plugin_abi.h`): `git diff --quiet 090e7039 HEAD -- src/core/plugin_abi.h` exit 0, unchanged. `third_party/foxsdr_api/foxsdr_api.h` (the engine C API) DID change: serial-a appends ops 0x011C to 0x0122 (49 lines, appended; nothing existing edited).
* config.json: `%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` before the lane and after the gates: unchanged. The real file was never opened by a lane test (tests redirect the profile).
* Engine operation numbers: every `#define FOXAPP_OP_* / FOXAPI_OP_*` with a hex value in `src/core/app_commands.hpp` and `third_party/foxsdr_api/foxsdr_api.h` (126 FOXAPP ops in the first), compared by value across both files: no value is defined twice. Serial-a's numbers: 0x011C-0x0123 (`SET_SPECTRUM`, `SET_IQ_CORRECT`, `SET_AFC`, `SET_CW_PEAK`, `SET_STEREO_PLACE`, `SET_RECORD_FORMAT`, `SET_CTUN`, `SET_FILTER_SHAPE`), 0x861A-0x8625 (`FILE_SEEK`, `FILE_LOOP`, `VFO_SELECT`, `VFO_SWAP`, `VFO_STORE`, `SET_FM_WIDTH`, `CHANNEL_CLAIM`, `CHANNEL_RELEASE`, `MPX_RECORD`, `IF_RECORD`, `PPM_MEASURE`, `SET_IF_ZOOM`). The three OSS branches added no op.
* Settings keys: the OSS branches touch neither `config.cpp` nor `config.hpp`; serial-a's keys (`spectrumFftSize` ... `ctun`, `filterShape`, `recordIqFormat`, `recordAudioFormat`, `vfoActive`, ...) are the only additions. A scan of `src/core/config.cpp` for a top-level `j["key"] =` line written twice found none (the `e[...]` duplicates it reports are separate writer functions that existed before).
* Registries: `kKinds` in `sanitiseSavedRadios` gained `"rawiq"` (serial-a) beside the earlier kinds; thread-inventory and ReaderLeak rows in section 1.

## 5. Not run, not verified

* `packaging_manifests` (CMake's `pwsh` is the WindowsApps stub on this PC, so it is not registered, as in the first record); the OpenAPI addition against a live server (section 2); nothing built for Linux; the sweep / clip checks of the first record were not repeated; no radio, no outside service, no real FTDI, Icom, FUNcube, WiNRADiO, SpyServer or directory contact (every OSS lane is proven against stand-ins written from the same documents).
* The cause of the `test_pipeline_afc` reds (section 3b).

## 6. Incidents

None. No ctest was left running: every gate ran to its end and its output was read (headless through a background script that wrote a done-file; GUI through the hidden-desktop launcher; both waited on). No dialog was reported.

## 7. Final-tip checks

`test_diagnostics` compares the binary's embedded commit with `git HEAD`, so it can only pass on a build made at the exact commit under test and goes stale with every commit (including the one that adds this file). It was re-run once after the commit of this file, on a rebuilt tree: the result is in the lane's final report, not here (a number written here would itself change HEAD).

## 8. BUILT BUT NOT WIRED (only what these four branches add; each exists with tests, but has no engine entry, no screen control, or both)

| Branch | Item | State | Where a wiring lane starts |
|---|---|---|---|
| serial-a | Screen half of every op: spectrum FFT size / window / frame rate, I/Q correction, AFC, CW peak, stereo placement, VFO A/B, record format, seek bar and A-B loop, raw I/Q row, centre tuning, FM width presets, channel claim, direct sampling, filter shape, MPX take, IF take, ppm from the pilot, IF zoom | engine ops and settings built and tested; no Slint control, string or drive script | `engine-serial-a.md` section 4 (labels, ops, ranges, defaults) |
| serial-a | IF zoom frame on the wire | `Engine::zoomFrame` works in process only; the C API serves one spectrum frame per radio, a second ring is ABI-visible | `engine-serial-a.md` section 3 row 2 |
| serial-a | T7 several VFOs in the main window, F10 AM co-channel canceller, whole-file waterfall (P15), live virtual-cable output for the multiplex (D4) | not built | `engine-serial-a.md` section 3 rows 3-6 |
| serial-a | Mock engine (`third_party/foxsdr_api/reference/mock_engine`) not extended for the new ops | not done | `engine-serial-a.md` section 3 row 10 |
| oss-radios | `sdriq` (SDR-IQ), `icr8600` (Icom IC-R8600), `funcube` (FUNcube Pro/Pro+) sources | built; no registration in the engine driver table or the add-radio screen. SDR-IQ cannot run for a user: its opener returns no link until a D2XX (FTDI) run-time link exists | `docs/feature-records/oss-radios.md` |
| oss-radios | Hermes-Lite 2 profile for the HPSDR-1 source (`profile=hl2`, `watchdog=`) | built; no screen control; every bit position unverified (facts from an unlicensed wiki) | same record |
| oss-radios | `HackRfSweeper` (linear sweep, produces `SweepLine`) | built; not a source kind and not polled by anything | same record |
| oss-radios | RSPduo rate map (`sdrplay_rate_map`) | built; no rate menu uses it; unverified on a real RSPduo | same record |
| oss-radios | shared `UsbDevice::readBulkOnce` (edits `usb_device.hpp`, `winusb_device.cpp`) | the WinUSB path is compiled only, no stand-in exercises it | same record |
| oss-net | SpyServer client (key `spyserver`) | built against a loopback stand-in; not in the engine driver table; no screen control | `docs/feature-records/oss-net.md` |
| oss-net | Aaronia `format=int16`, `control=remoteconfig`, `demod`, `spectran`, `spectranoffset` | built; no control; default stays `control=capture` | same record |
| oss-net | Receiver listing (`GET /status.json`, `ReceiverListing`, `WebServer::setReceiverListing`) | built, off by default, documented in OpenAPI; nothing calls `setReceiverListing`, so the route always answers 404; no Settings page | same record |
| oss-net | one-line file-kind label for the SDRuno `.s1b` import (`file_kind.cpp`) | the import works through `importFrequencyList`; the label line was not added | same record |
| oss-libs | `g39ddc`, `g33ddc` (WiNRADiO run-time bridges), SDRconnect `iqrate=` | built against stand-in DLLs; no engine registration, no screen control; real DLL bitness unknown (the loader refuses 32-bit) | `docs/feature-records/oss-libs.md` |
| oss-libs | `vendor_lib` (PE check and loader), `vendor_apartment` (one-thread apartment) | shared helpers with no other user yet | same record |
| oss-libs | radio-setup text has no BladeRF / Pluto / AntSDR family | not added | same record |
