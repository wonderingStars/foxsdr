# Engine merge 11, PHASE 1 (`engine-merge-11`): security lane 2, the hardware findings and the window follow-ups into `engine/multi-radio` (2026-11-17)

Worktree `C:/Users/steve/foxsdr-wt/mr-dsp`, branch `engine/multi-radio`, start tip `4229dbd3` (merge 10, clean), build `D:/foxsdr-build/mr-dsp` (Slint ON, clip probe OFF, Release, `--parallel 6`), scratch `D:/foxsdr-build/m11-scratch/`, hidden desktop `FoxSDRMerge11`. Every test and drive ran through the hidden-desktop launcher (`FOXSDR_FRAME_CAP_HZ=60`), `ctest -j3` (re-runs `-j1`). No radio opened, nothing transmitted, nothing pushed, downloaded or deployed, no outside request, no other worktree touched, `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` and `FOXSDR_ALLOW_REAL_HARDWARE_TESTS` never set. Real `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (see section 9).

**THIS IS PHASE 1. THE FULL GUI GATE HAS NOT RUN and the stamp tests have not been run on a final build: phase 2 brings two or three more branches and runs both once.**

## 1. The merges (tips checked first: `3c079831`, `614a31ca`, `253a26a5`, exactly as briefed)

| Merge commit | Branch | Conflicts | Settled |
|---|---|---|---|
| `1007544c` | `feat/security-fixes-2` | none | git merged `tests/errpath_allow.tsv` clean |
| `229aa80c` | `feat/hw-findings` | `tests/errpath_allow.tsv`, one hunk, both sides appended at the end | both kept (11 `sec2` lines and 1 `hw-findings` line); re-seeded after the third merge |
| `017005e6` | `feat/window-followups` | the 33 `resources/lang/*.json`, `src/core/lang_assets.hpp`, `layout-section-overlap-1600.txt` and `-624.txt` | catalogues by key against the common ancestor `860fbc65` with `m11-scratch/lang_union.py` (merge 10's script plus one rule: a key the already merged side removed and this lane left unchanged stays removed; without it the TCI "has no password" sentence, which lane 1 replaced, came back as a stale key in all 33); `lang_assets.hpp` regenerated with `tools/embed-lang.py`. `tools/i18n_keys.py`: **4806 keys in each of the 33**, 0 empty, 0 missing, 0 stale; Korean word-spacing check is inside `test_i18n`: Passed. The two drive scripts: the merge-10 pin (855) kept, see section 4 |

Later commits: `cdc1d783` (the CAT test and the re-seeded allow list), `36f41f16` (three red tests fixed), `da0bbe24` (allocation-bound comments).

## 2. The particular points

### CAT: what a saved "reachable" value does now
Read: `ServerHost::reconcileCat` (`engine/server_host.cpp`) and `CatServer::start` (`net/cat_server.cpp` line 142). On the merged tree a saved `catBindAll: true` with `catEnabled: true` and NO `catAllow` list makes the CAT server **not start at all** (no socket, not even loopback): `reconcileCat` parses the list first and, when it is missing or unusable, puts "to let other computers connect to the CAT interface, list the computers that may: ..." into the CAT listener's `refusal` (shown by the window as the CAT fault); `CatServer::start` refuses a wide bind with no list a second time. With a list it listens on all interfaces for the LISTED computers only (an unlisted peer is closed before a byte is read). The engine therefore did NOT still bind everything after the merge; lane 3's remark described its own base. What was missing was a test through the saved value (the existing test used the SERVER_CONFIG operation). Added `testSavedCatReachableNeedsTheList` in `test_tci_engine`: parse a settings file with `catEnabled` and `catBindAll` and no list, `Engine::applyConfig`, wait idle: server not running, listener not running, refusal says "list the computers that may"; an unreadable list (`0.0.0.0/0`) is the same. BREAK-IT: `reconcileCat`'s list check disabled and the start made on loopback (a break that removed both refusals would have bound every interface, so it was made loopback-only): `test_tci_engine` 359 checks, 5 failed (lines 666, 667, 701, 702, 703); restored, 0 failed. NOT in the engine's reach and not changed: the window's CAT page, with the tick hidden, shows "CAT is available on this computer only." and, for a saved true, the engine's refusal as the CAT fault and the "no password" warning; the user cannot type the list in the window (`SF2-cat-ui`, open).

### Signing key
One behaviour, in one place (`core/sys_manifest_sig.cpp`): `kOwnerKeyLiteral` is checked by a build-time `static_assert` (empty, or exactly 64 hexadecimal characters); `ownerKeyProblem` and `trustedKeysFromHex` throw `std::logic_error` with the plain sentence for a string that gets past (only tests call it with one); `ownerTrustedKeys()` is `trustedKeysFromHex(kOwnerKeyHex)`. Nothing else in the tree checks the key's length (`updater.cpp` and `companion_programs.cpp` only call `ownerTrustedKeys()`), so there are no second checks to disagree. `kOwnerKeyHex` is still EMPTY: the tree builds (exit 0) and `test_sys_ed25519` passes in the headless gate.

### Hidden controls and reason lines
Searched the string literals ADDED by lane 1 and lane 2 in `src/` (everything except tests and the generated catalogue) for "yet", unverified, lane, todo, stub, not built, phase, coming, registry, placeholder, later, soon, beta: none. The shown-words rule in `test_slint_host_mustfixa` (which scans `src/slint`, where lane 1's two TCI strings sit) passed in the gate. Lane 2's sentences are engine words and say what happened in plain words ("Sample rate coerced: this radio cannot run at ...", "FoxSDR's radio search program (cascade.exe) is missing from this install ... Reinstall FoxSDR."). No sentence needed fixing; the test was not touched.

### Minimum window
`kMinWindowW/H` 1480 x 560 merged as lane 3 made them; not touched.

### Approved privacy wording
`git diff --word-diff 4229dbd3 HEAD -- PRIVACY.md docs/DIAGNOSTICS.md` is EMPTY (0 bytes). The in-app sentences: no line of `src` changed by the three branches mentions usage reports, anonymous or the radio-that-did-not-open sentence (the only "did not open" lines are lane 2's stand-in text), and no catalogue value whose key mentions usage report, did not open, anonymous, privacy or diagnostic changed in any of the 33 files. Nobody edited those words. **Help topic `sys-usage` (28-topic help, `lanex_help.cpp` line 132) is not consistent with the approved text**: it says the Send anonymous usage reports switch "turns the usage reports on or off", while PRIVACY.md says a radio that does not open is reported whatever is switched. Reported, NOT changed (same finding as merge 10's `M10-help-usage`).

## 3. Error-path pin

`kPendingPin` was 410 on the base and on each of the three branches. Union key by key against the common ancestor `860fbc65`/`4a4d9c28`: lane 1 added 11 ALLOW lines, lane 2 added 1 ALLOW line and renamed 2 (same verdicts under new keys), lane 3 added 1 ALLOW line and renamed 2; none added a PENDING site. Re-seed (`tools/errpath_seed.py` on the merged scanner): 3486 sites, 3226 allow lines kept, 0 re-keyed, 0 dropped, 0 sites without a survey row; only the order of the appended lines changed. **Pin 410 before, 410 after: 410 + 0 + 0 + 0.** Groups (identical to merge 10): reports-not-in-function 167, O-outputs 55, unsure 45, R-record 27, G10-gpu-fft 16, D-gps-serial 13, F-files 11, T-tx 11, unmatched 9, S2-window 7, S1b-headless 7, A-audio 7, P-plugin 5, P-store 5, S3-plugin-ui 5, S6-scan 4, S9-report-upload 4, S7-ui-engine 3, S1-startup 3, G3-control-refused 3, G1-device-dead 2, PL2-companion 1 = 410. `test_error_paths` and `errpath_gate_selftest` Passed.

## 4. Pinned numbers and changed expectations (every one, both values)

| Where | Was | Now | Why |
|---|---|---|---|
| `test_engine_publish_alloc` F3 bound | 2800 | **2800** | MEASURED: rebuilds 2536 to 2545 (max 2545; 2537 at merge 10), settled passes 0; 2545 x 1.10 = 2799.5. Comment only |
| same, F12 bound | 2400 | **2400** | MEASURED 2173 to 2174 (2166 at merge 10); 2174 x 1.10 = 2391.4. Comment only |
| drives `layout-section-overlap-1600` / `-624`, `"Source"#1 top >=` | merge 10: 855; lane 3: 867 (from 868) | **855** (merge 10's, kept) | the two lanes' values are the same fact: MEASURED on the merged window (probe scripts, real run): Source header 867.00, "Read the pitch now" bottom 2279.00, Sinks header 2467.00 at both 1600 and 624. Lane 3's "867" is the header's reading; merge 10's "855" is the bound it pins (note bottom 855 + 12 = 867). Both hold; the pins `<= 2279` and `>= 2279` are met exactly |
| `test_plugin_store_view` line 1101 | `"hobbyist.example: not read yet"` | `"hobbyist.example: not read"` | lane 3 reworded the Legacy sentence (its list omitted this test) |
| `test_sdriq_d2xx` line 513 | `contains(info.licence, "unverified")` | `contains(info.licence, "FTDI's own licence")` | lane 3 replaced the developer word in the sentence the user reads; the other two conditions on the line are unchanged |
| `tests/CMakeLists.txt` | no stack option | `/STACK:8388608` for `test_audio_lead_app` | see section 6 |
| lane 3's drive expectations (its record's list): `lanel-libs` "Not checked yet" to "Not checked"; `extend-cat` the tick line removed; `patch-faces`, `system-updates-withdrawn-*`, `wg3-store-plain` reworded; layout pins | | | each re-read against the merged window: all of `lanel`, `extend`, `patch`, `system`, `wg3`, `s5`, `s4`, `s4b`, `layout-section`, `window-min`, `setup-auto` drives passed in the GUI slice (section 7), so every one still holds |
| lane 2's: `test_slint_s5_host` line 151 `!= nullptr` to `== nullptr`; `s5-sources` drive no click; `test_health_paths` ScanLimit sites 4 to 5; `test_slint_s5b_host` IF rows block | | | each passed in the merged tree (headless gate; drive in the GUI slice) |
| lane 1's: `importLeftOutKeys().size()` 9 to 10 to 11; `test_tci_server` bind-all case now lists its address; `test_engine_main_config_roundtrip` lists `tciAllow`, `catAllow` | | | each passed in the merged tree |

## 5. Hidden desktop tests of the three lanes' own work
All of each lane's tests are in the headless gate (section 7); the GUI drives each lane touched ran in the GUI slice. Lane 2's real-radio checks and lane 1's TLS and Linux items stay UNVERIFIED as their records say.

## 6. Red tests the merge produced (each its own commit, nothing weakened)

| Test | Alone | Cause | Fix (`36f41f16`) |
|---|---|---|---|
| `test_audio_lead_app`, `test_audio_lead_app_stepped` | SEGFAULT, exit 0xC00000FD (stack overflow), before the first output line; Windows event 1000 names ntdll / ucrtbase | the test's `th::applyConfig` frame is 0xF42E0 = 1,000,160 bytes (`__chkstk` size from `dumpbin /disasm` of the object) of the 1 MB default stack; lane 1 added two `std::string` members to `AppConfig` (`tciAllow`, `catAllow`) | `/STACK:8388608` on the test, the same remedy and reason as `test_config` and `test_engine_service_options` in the same file. RED seen alone in both variants, GREEN after |
| `test_plugin_store_view` | line 1101 | lane 3 reworded the sentence | expectation follows the sentence |
| `test_sdriq_d2xx` | line 513 | lane 3 removed the developer word | expectation follows the sentence |

## 7. Gates (build of `36f41f16` plus the comment commit; full `cmake --build` exit 0)

**Full headless gate** (`ctest -L "headless|slint" -j3 --timeout 300`, 1241 distinct tests, two slices of the filtered index, 625 and 621 slots, 5 in both; 2 skipped by their own rule, `test_plugin_abi3_compat` and `test_rtlsdr_live`), run at the build of `017005e6`/`cdc1d783`:

| Slice | Real time | Reds | Class |
|---|---|---|---|
| 1 to 620 (625 run) | 1430 s | `test_audio_lead_app` | REAL (section 6), fixed |
| | | `test_iq_open_async`, `test_engine_control_thread`, `test_engine_api_03` | load; each Passed alone (12.87 s, 8.59 s, 43.86 s) |
| 621 to 1241 (621 run) | about 1500 s | `test_audio_lead_app_stepped`, `test_plugin_store_view`, `test_sdriq_d2xx` | REAL (section 6), fixed |
| | | `test_record_finish_async_stepped`, `test_plugin_hostile` | load; Passed alone (28.47 s, 26.39 s; the hostile plugins' own access violations in the Windows log are the test's fixtures) |
| | | `test_diagnostics_free` | STAMP: "git HEAD now: cdc1d7834949, binary says: 017005e6a9e1-dirty"; a stamp test, to be run at the final build in phase 2 |

After the fixes, alone: `test_audio_lead_app`, `_stepped`, `test_plugin_store_view`, `test_sdriq_d2xx` 4 of 4 Passed. A final slice at the last build is in section 8.

**GUI slice** (drives the three lanes touched: `slint_drive_(setup-auto|window-min|lanel|lanek|lanec|system|tools|extend|patch|s4b|s4-|s5|signal|wg3|mapj|escape|bar-min|layout-section)`, `gui_desktop_guard`, `test_health_app(_stepped)`, the allocation test and the two audio-lead tests, `-j3`): **98 of 98 Passed**, including the new `setup-auto-check`, `setup-auto-check-bounded-default` and `window-min-strip`, both layout drives, `extend-cat`, `s5-sources`, `s4-outputs`. **The full GUI gate (419+ tests) has NOT run.**

## 8. Final slice at the last build
`ctest -R` over 18 tests (`test_audio_lead_app`, `_stepped`, `test_plugin_store_view`, `test_sdriq_d2xx`, `test_tci_engine`, `test_engine_publish_alloc`, `test_error_paths`, `errpath_gate_selftest`, `test_i18n`, `test_sys_ed25519`, `telemetry_worker`, `test_slint_host_mustfixa`, `test_engine_api_03`, `test_iq_open_async`, `test_engine_control_thread`, `test_diagnostics`, `test_diagnostics_free`), `-j2 --repeat until-fail:3`: the 16 non-stamp tests passed all 3 times (48 passes, 0 failures). `test_diagnostics` and `test_diagnostics_free` are red ONLY as stamp tests: "git HEAD now: da0bbe24cdf9, binary says: 36f41f16ed71-dirty" (the binary was built before the two documentation and comment commits); they are run at the final build in phase 2.

## 9. Config hash, UNVERIFIED, open rows

UNVERIFIED: the full GUI gate; the stamp tests on a final build; lane 1's HTTPS-relay total, the Linux `desk_logic.cpp` pipes and the installer's resolver time; lane 2's items on a second tuner model, the `library` field against the Worker's validator on a real Soapy failure, the ITU-R BS.450 tolerance, the B200 start crash (not reproduced); lane 3's real USB probe at start, the minimum window on a real small screen, a window maximised below 1480 x 560; every machine-made translation (122 + 2 strings in 33 languages) by a speaker; the CAT page's wording for a saved "reachable" value by eye; the Worker validator over the merged tree's messages (not re-run in phase 1: no report words changed in these three branches except lane 2's `library` field and `absent` class).

OPEN ROWS added or carried: `SF2-cat-ui` (a field for the CAT list in both windows; with the tick hidden a saved true leaves the CAT page showing a refusal the user cannot fix there), `M10-help-usage` (the `sys-usage` sentence), `SF2-L9-pipes`, `SF2-L12`, `SF2-tls-relay-total`, `SF2-translations`, lane 2's six rows (IF bandwidth, automatic ppm, B200 in-process discovery, hand-written Soapy row, other Soapy kinds, tuner gain count), lane 3's rows (real pointer pass, window snap, 19 tall-picture scripts, U3 to U10, M6 "foxsdr.com" twice, the SYSTEM page chips). Not yet folded into `open-rows-2026-11-17.md`: done at the end of phase 2.

---

# PHASE 2a (same day): send-on, errpath-third and linux-merged

Start tip `7e05adff`. Tips checked as briefed: `feat/send-on` `dbd7f85e`, `feat/errpath-third` `f593d0dc`, `feat/linux-merged` `e7fab527`. Nothing pushed; no outside connection made by me (stand-ins on 127.0.0.1 only); no version string changed. **The full GUI gate and the stamp tests at a final build have NOT run: they wait for phase 2b** (the headless gate's stamp tests did pass at this build, see 2a.10).

## 2a.1 The merges

| Commit | Branch | Conflicts | Settled |
|---|---|---|---|
| `f3651418` | `feat/send-on` | none | |
| `f413b431` | `feat/errpath-third` | `rate_follow_status.hpp` includes; `tests/errpath_allow.tsv` | includes: this lane added `core/radio_events.hpp`, hw-findings added `source/device_source.hpp`; both kept. Allow list key by key against the common ancestor `4229dbd3` (`m11-scratch/tsv3.py`): 206 lines take this lane's rewritten reason, 1 line removed (the SDRplay low-IF fall-back, now reported in its own block), 0 added; every phase-1 line kept |
| `b012e287` | `feat/linux-merged` | `desk_logic.cpp`, `test_slint_host_desk.cpp` | security-fixes-2 and this lane made the SAME relative-PATH fix (L9): the security lane's version kept in the source; both lanes' tests kept (they probe the same rule with different files) |

## 2a.2 The send guard and a real user (`2dfbe181`)

Read `core/report_send_guard.cpp`. FOUND: the last rule (the profile folder must be the one the system names) SILENCED a real user in the first case below and could in the others. Fixed; one test per case (`test_radio_reports_send_on`, the situations given as inputs to the new `outsideSendRefusalWithRoots`):
- **The system's profile-folder call fails, or answers something odd** (empty, relative, blanks): it used to answer "redirected" and nothing left. Now the profile rule refuses only when the system has named at least one USABLE absolute folder and the run's folder is under none; with no usable name the report is SENT. Tested: no names; odd names; an odd name beside a good one.
- **APPDATA redirected by the user's own policy** (roaming profile, folder redirection to a share): the known folder the system returns is the share and APPDATA is the same share, so those matched; a mapped drive against its share, or another spelling, would not have. Both sides are now resolved with `GetFinalPathNameByHandle` (a mapped drive becomes the share it stands for) before the comparison. Tested: the share, another spelling of it, a trailing slash.
- **The profile on another drive**: a persistent `APPDATA` in the user's own environment (`HKCU\Environment`) is now one of the names. Tested: the folder on D: is sent from.
- What it still stops: a scratch folder, a sibling folder that only starts with a real name, an empty queue folder, and every marker. The whole profile folder is NOT a name (a test's scratch under `%TEMP%` is inside it: found by a red run of the existing test, then removed).
BREAK-IT: the old rule restored fails lines 215 and 217; restored, green. `test_radio_reports_send_on` Passed.

**Markers in THIS tree.** `ctest --show-only=json-v1`: 1665 tests, 1660 carry `FOXSDR_NO_OUTSIDE_REPORTS=1` (set by `cmake/NoOutsideReports.cmake`, called as the last line of the `tests/`, `src/slint/` and `src/slint/tests/` CMakeLists); the 5 that do not are `test_input_event`, `test_input_hid_parse`, `test_input_mapping`, `test_input_midi`, `test_input_win` (no application, no network). The hidden-desktop launcher itself (`~/.claude/scripts/run_on_hidden_desktop.ps1`, outside the tree) sets NO marker; the drives get it from their own harnesses, which set it in the child's environment: `test_slint_drive_run`, `drive_handover_run`, `drive_retune_run`, `test_slint_config_roundtrip`, `test_slint_lists_engine`, `engine_api_test_env.hpp`, `engine_main_proc.hpp`, `pump3_support.hpp`. A harness that forgot all of it is still stopped by the profile rule (its redirected profile is under none of the system's names).
**Outside connection during the gate: how checked, and how weakly.** (a) the guard's own tests with the post observer (a veto before any socket) for every marker, the redirected profile, the whole reporter and the heartbeat; (b) every test carries the marker (above); (c) the Windows DNS client cache read before the full headless gate (no foxsdr, telemetry or workers.dev entry) and after it (none; the only entry of ours was hedgerowlabs.com, which is not the service). (c) is a WEAK signal: a cache entry expires within minutes, so it cannot prove a negative over an hour; (a) and (b) are the real evidence. UNVERIFIED: a packet-level capture of a whole gate.

## 2a.3 CAT (row SF2-cat-ui) (`38250ebb`)
While the tick is hidden a saved `catBindAll: true` with no typed list is read as this computer only: `core/cat_scope.hpp` `catReachesOtherComputers(saved, list)` = saved && (offered || list typed). `ServerHost::reconcileCat` starts CAT on LOOPBACK and shows no fault; the Slint CAT page reads the same function (no "no password" warning, `127.0.0.1`). A list typed on purpose (SERVER_CONFIG, or the headless `--cat-allow`) still gives the listener beyond this computer for those computers only; an unreadable list is still refused with nothing bound. Tests: `test_tci_engine` (SERVER_CONFIG case and saved-settings case; the case that sets a list now sets bindAll false so no test makes a wide bind), `test_slint_host_extend`. BREAK-IT: the function returning the saved value alone fails `test_tci_engine` 669 to 671 and 706 to 709; restored, green. Expectation changed, both values: saved true + no list: reachable, warning, "Listening on 0.0.0.0:4600" became not reachable, no warning, "Listening on 127.0.0.1:4600"; the old reading is kept for the same facts WITH a list.

## 2a.4 Help topic (row M10-help-usage) (`31fe9fe7`)
`sys-usage` gets the first sentence of the approved "Radio failure reports and the radio journal" in its own words ("ONE" lower-cased, the dash dropped): "When a radio does not open, FoxSDR sends one report about that failure, every time it happens, whether or not Diagnostics is on and whether or not Usage reporting is on." Added to all 33 catalogues (machine-made, each using that catalogue's own words for Diagnostics and Usage reporting), `lang_assets.hpp` regenerated: 4807 keys in each, 0 missing, 0 stale, Korean check green (`test_i18n`). `git diff --word-diff dbd7f85e HEAD -- PRIVACY.md` is EMPTY (0 bytes); `docs/DIAGNOSTICS.md` untouched.

## 2a.5 Release notes and deploy order (`ad3300f3`)
`docs/release-notes-0.99.109.md` body replaced by the checked candidate (its check log read). Privacy section re-checked against the merged `PRIVACY.md`: both quoted blocks (3 and 86 lines) are contiguous word-for-word text of the file. Hidden-controls list re-checked against the merged tree (its CAT sentence is now exactly true). Shape for `tools/release.ps1`: title `# FoxSDR 0.99.109` (a five-file release: no em dash Windows), text above the first `---` for the site entry, five `**SHA-256 of ...:**` blocks of `TBD` at the end; the `[WINDOW-SIZE: ...]` line is left for 2b. **OPEN for the release lane:** the candidate says the Linux download is an x86-64 tarball only, but a five-file release names two AppImages and an arm64 tarball as well (the Windows-only title would drop all Linux files). `telemetry-worker/README.md` and `docs/radio-reports-contract.md` section 11: Worker first for the tester period WITHOUT `SITE_INTAKE_URL` (503, every copy keeps its messages), then on release day the site, the Worker with the site address, one real message proved, the public release. Documents only.

## 2a.6 Error-path pin
410 on the base and on each of the three branches; union key by key: send-on added no PENDING line, errpath-third rewrote 206 reasons and removed 1 line without touching a PENDING one, linux-merged none. Re-seed on the merged scanner: 3486 sites, 3225 allow lines kept (3226 at phase 1 minus the removed one), 0 re-keyed, 0 dropped, 0 sites without a survey row, 284 sites pass by a reporter (282 at phase 1: errpath-third's). **410 + 0 + 0 + 0 = 410**; the re-seeded file differed from the committed one only in line endings. `test_error_paths` and `errpath_gate_selftest` Passed.

## 2a.7 The wobbles, alone and together
`test_sdrplay_lowif`, `test_patch_iqfile`, `test_patch_radio`, `test_patch_runner` and `test_engine_api_wireb_funcube` (which segfaulted once under load in the gate): 30 runs each alone (`--repeat until-fail:30`): **150 of 150 passed**; the same patch and sdrplay tests plus `test_patch_radio_abandon` together at `-j3`, 10 repeats: **50 of 50 passed**. NOT reproduced alone or together, and no shared state found (scratch folders are per process id or random; the patch radio and runner tests use none): the red in the combined run on lane 2 was load on a busy machine. That is the likeliest reading, not a proof; the cause is UNVERIFIED.
`test_driver_policy`: Passed in the full gate (phase 1's four rows are in this tree; the Linux lane's base predates them).

## 2a.8 Worker validator
`FOXSDR_DUMP_APP_MESSAGES` over `test_radio_events`, `test_radio_reporter`, `test_radio_reports_engine` (all Passed), then `node tests/radio_reports_validate.mjs`: **145 messages, 145 accepted, 0 refused** (146 at merge 10). `telemetry_worker` (the Worker's node suite) Passed in the gate.

## 2a.9 Publish-allocation bounds
`test_engine_publish_alloc`: F3 rebuilds 2536 to 2545 (max 2545), F12 2173 to 2174: identical to phase 1, so the bounds 2800 and 2400 stand (2545 x 1.10 = 2799.5; 2174 x 1.10 = 2391.4); nothing changed.

## 2a.10 Gates (build of `ad3300f3`)
**Full headless gate** (`ctest -L "headless|slint" -j3 --timeout 300`, 1243 tests in two slices, 625 and 623 slots; 2 skipped by their own rule): slice 1 (1430 s): `test_cat_server`, `test_flac_writer` (timeout), `test_slint_host_scope`, `test_iq_file_seek`, `test_discord_presence`, `test_engine_api_wireb_funcube` (SegFault), `test_engine_api_transmit` (191 s) red; every one **Passed alone** (12.10 s, 5.21 s, 2.51 s, 2.03 s, 5.19 s, 1.20 s, 105 s), classified LOAD. Slice 2: **623 of 623**, including `test_diagnostics`/`test_diagnostics_free` style stamp tests at this build. The stamp tests are still to be run on the final build in phase 2b.
Expectations changed (both values) in this phase: `test_slint_host_extend` and `test_tci_engine` (section 2a.3); `test_radio_reports_send_on` has new cases only. The lanes' own listed changes (send-on: `kRadioReportsLiveSend` true, `kBeatCarriesRadios` false; errpath-third; linux-merged) each passed in the merged tree.

## 2a.11 Config hash, UNVERIFIED
`%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (checked at the end). UNVERIFIED: the full GUI gate and the stamp tests at the final build; a packet-level proof of "no outside connection" for a whole gate; the guard on a real roaming-profile or folder-redirected machine (the cases are tested as inputs); the Linux half of the guard (the `HOME` rule; no Linux build here); the new help sentence's translations by a speaker; the cause of the wobbles.

---

# PHASE 2b and the final part (same day): look-fixes, window-strip, the version, the gates

Start tip `20428e92`. Nothing pushed; no outside connection; real `config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (checked at the end). Because the stamp tests compare the binary's commit with HEAD, THIS RECORD IS THE LAST COMMIT BEFORE THE FINAL REBUILD: the results of the runs at the final build (stamp tests, the repeat runs of the fixed tests, the validator, the allocation bounds, the pin) are in the hand-back message and in `D:/foxsdr-build/m11-scratch/` (`s*.log`), not here.

## 2b.1 Merges and conflicts

| Commit | Branch | Conflicts | Settled |
|---|---|---|---|
| `6f323336` | `feat/look-fixes` `2d663570` | `src/core/lang_assets.hpp` only (generated; the 33 catalogues merged without a marker) | regenerated with `tools/embed-lang.py`: 4888 keys in each of the 33 (4807 + 81), 0 empty, 0 missing, 0 stale; the help sentence of phase 2a is in all 33 |
| `7474da2d` | `feat/window-strip` `650512f3` | none | |

The look-fixes expectations, re-read against the merged window: airband `squelch_text` "squelch -55 dB" to "-55 dB", `hold_text` "hold 2.0 s" to "2.0" (and -80 dB / 7.5), `actual_rate` "actual 2.4 MS/s" to "2.4 MS/s": all passed in the merged tree (`test_slint_host_tools`, `test_slint_host_signal` in both headless slices). The two Legacy-only keys the Slint tools test dropped (`"squelch %.0f dB"`, `"hold %.1f s"`) are still used by `src/gui/app_window_airband.cpp` and are still in all 33 catalogues (checked in `de.json`; `i18n_keys.py` 0 stale). The window-strip expectations: `test_slint_host_mustfixa` 1480 / 560 to 1024 / 640 passed; `test_slint_main_screen` (1440 x 900 sheet: splitter row 608, ribbon rows 204..212, narrow-1100 page-key box y 288..326) passed on the merged tree, so each of the re-measured values holds (the box is not displaced by the look-fixes lane's taller slider rows: they are in the SIGNAL/TOOLS panels, not in that box).

## 2b.2 The sentence-class check (look-fixes reworded the raw I/Q link faults)
`src/source/raw_iq_net_source.cpp`: "the I/Q stream stopped: no samples arrived for 3.0 s" became "No samples arrived for 3 seconds: the sender may have stopped, or its address or port may be wrong", and "the I/Q stream connection failed: <err>" became "The connection to the I/Q stream failed: <err>" (the engine's own fact keeps its "source thread: " prefix; only the card strips it). No matcher in `src` or `tests` read the old words except `test_raw_iq_net_source` (found red, fixed, below). The failure-class sorter (`health::classifyRadioOpen`) reads the joined "headline: detail": old and new silence sentences sort as `other`, old and new connection sentences as `vendor` (the "failed:" rule); `test_health_events` now holds all four (commit `6a4c170e`). The developer-words rule (`test_slint_host_mustfixa`) scans `src/slint` and passed over the 81 new sentences; a scan of the 81 new catalogue keys and of the string literals added in `src` for yet, unverified, lane, todo, stub, not built, phase, coming, registry, placeholder, later, soon, beta, "source thread", snake_case found none (the one "source thread: " literal is the prefix the card strips).

## 2b.3 The version (`32ce8fe9`), one commit
Changed to 0.99.109: `CMakeLists.txt` line 22; `README.md` line 11; `docs/api/openapi.yaml` line 4; `docs/silent-install-switches.md` lines 54 and 59; `installer/generated-version.iss` (CMake writes it at configure from the project version; it is tracked and `tools/release.ps1` preflight checks it). Left alone, each history or fixture: `docs/feature-records/manual.md`, `packaging.md`; `docs/release-notes-0.99.109.md` line 22; `installer/cascade.iss` line 303; `src/slint/cjk_fonts.hpp`; `tests/CMakeLists.txt` line 1151; `tools/make-package-manifests.ps1`; fixture strings in `test_slint_host_export`, `test_slint_host_system`, `test_slint_lanek_host`, `test_linux_update`, `test_update_old_prompt` (they pass the number in and read it back; none compares it with the program's own version). No test pinned the old version, so no expectation changed.

## 2b.4 The release notes' shape (`0c1986a8`)
The `[WINDOW-SIZE]` line reads exactly "The smallest window is now 1024 x 640 pixels." `tools/release.ps1` preflight accepts two shapes only: a title ending in an em dash and "Windows" with ONE hash block (installer; Linux skipped), or a plain title with five (installer, two AppImages, two tarballs). The release is the installer plus ONE Linux x86-64 tarball, which matches neither; the notes use the Windows shape: title `# FoxSDR 0.99.109 <em dash> Windows`, one block for `foxsdr-setup-0.99.109.exe` (`TBD`), and a plain sentence under it naming `foxsdr-0.99.109-linux-x64.tar.gz` (NOT a `SHA-256 of` block: that would fail the names check); its checksum is written by hand next to it on the release page. Row `REL-hash-shape`.

## 2b.5 Layout pins, measured once on the merged window, both values
| Where | Was | Now | Measure |
|---|---|---|---|
| `layout-section-overlap-624`, "Stop bypassing the selection" bottom <= | 828 | **902** | +74: the look-fixes lane shows lane-panel slider names whole (they wrap), one line taller each, at this width only |
| same, "Source"#1 top >= (the Patch note's bottom) | 855 | **937** | +82; the Source header reads 949 = 937 + 12 |
| same, "Read the pitch now" bottom <= and "Sinks"#1 top >= | 2279 | **2361** | +82; the Sinks header reads 2549 = 2361 + 188 |
| `layout-section-overlap-1600` | 828 / 855 / 2279 | unchanged | measured 820 / 855 / 2279 and the headers 867 / 2467: the wider panel does not wrap |
| `test_slint_main_screen` 1440 sheet | splitter 608, ribbon 204..212, box 288..326 | unchanged | re-measured: holds |
| `test_slint_tools` (radar page) | lat and lon: two 124 px fields side by side (x0a and x0a+130); Set RX here at fy + 30 | one field per row after a 64 px label and 6 px: box24(x0a+70, fy, kBodyW-70) and box24(x0a+70, fy+30, kBodyW-70); Set RX here at fy + 60; known rows +30 (Set T4+50 to T4+80, scope key T4+82 to T4+112); the ground probe at x0a+74 | look-fixes labelled and split the two fields; its record did not list this test |
| drive `patch-faces-map` | size 1500 x 900 | 1500 x 932 | the toolbar folds to a second row at 1500 (+32): the Map node's own keys fell below the window |
| drive `export-wf` | 1280 x 720 | 1280 x 800 | the strip folds to two rows at 1280 |
| drive `bar-min-624` | 624 x 400 | 624 x 470 | two strip rows at 624 |
| drive `gaps1-markers-damaged` (and its `coverage-gaps1.tsv` line) | contains "No markers yet." | contains "No markers." | window-followups reworded the sentence; the full GUI gate is the first run since |
| `test_raw_iq_net_source` line 327 | headline contains "I/Q stream" | contains "No samples arrived for" | look-fixes reworded the headline |

## 2b.6 Tests the merge turned red, each fixed at its cause (its own commit)
`test_slint_tools` (23 checks), `test_raw_iq_net_source`, `test_cjk_wrap` (two new look-fixes sentences, "aaronia=host[:port]" and "soapyremote=host[:port]", could end a line on the bracket in ja, zh-CN and zh-TW; the test stops reporting at the first failing language, so it took two commits; the three catalogues now write host:port and say :port may be left out), and four drives (2b.5). Nothing weakened: each expectation follows a changed layout or sentence and says so.

## 2b.7 Gates (build of `07497f83`/`976228fb`, then the final build)
**Full headless gate** (1243 tests, slices 625 and 623): slice 1 red: `test_cjk_wrap`, `test_slint_tools` (both REAL, above). Slice 2 red: `test_raw_iq_net_source` (REAL), `test_diagnostics_free` (STAMP: binary older than HEAD); 2 skipped by their own rule. **Full GUI gate** (`ctest -L gui`, 428 tests, slices 216 and 213 slots, `-j3`, hidden desktop): slice 2 213 of 213; slice 1 four reds, each REAL and fixed (the four drives). The results after the fixes (the fixed tests alone, the stamp tests at the final build, the validator, the allocation bounds, the pin) are in the hand-back message.

## 2b.8 Open rows
`open-rows-2026-11-17.md` refreshed in place (engine merge 11): **339 open, 58 closed, 397 in all** (screen 63 / 17, engine 133 / 34, plugins 16 / 0, Worker/site 20 / 6, owner decision 57 / 0, mouse drive 50 / 1), with the strip lane's rows (GAIN and SQUELCH captions gone at 1024, the title-bar model line under the search box, English words left in a translated strip, the snap below the minimum, row two at 1366), the look-fixes and window-followups rows, the security, hardware, send-on, errpath-third and linux rows, and the release-notes shape. Closed with evidence: `SF2-cat-ui`, `SF2-cat-bindall`, `SF2-api-multi-host`, `M10-help-usage`.
