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
