# Lane `sysmodels` - setup, settings, install, reliability models

Branch `feat/sysmodels` (from engine/multi-radio d3630d78). A new-file model lane: models, OS wrappers and tests in new files; no forbidden file edited.
Every model is standalone and tested through an injected stand-in; nothing is wired into the application (wiring hand-offs below).

## Step 0 - the table (reading only, written before any code)

Classes: `new file model` (built here, standalone), `needs a forbidden file` (the WIRING needs one: model built, exact hand-off written), `already built`, `blocked`.

| # | Row | Class | What was opened and found |
|---|-----|-------|---------------------------|
| 1 | First-run wizard and starter presets | new file model + needs a forbidden file (the pages are `.slint`) | `src/core/radio_setup.hpp` is a one-card check plus `radioSetupDontShow` (config.hpp:243). No wizard, no starter data anywhere. |
| 2 | Portable mode | new file model + needs a forbidden file (`config.cpp` / `ConfigStore::defaultPath` read `%APPDATA%` directly, and `diag_log.cpp`, `plugin_host.cpp` read `%LOCALAPPDATA%`) | The resolver can hand back an environment override pair (APPDATA, LOCALAPPDATA), so the wiring is two `_putenv_s` calls at the top of `main()`; `main.cpp` is not on the forbidden list but is another lane's neighbour, so it is a hand-off. |
| 3 | One-file settings export / import / backup | new file model + UI hand-off | Only the automatic `<file>.<stamp>.bak` copies exist (`config.cpp:1862`). No `export` / `import` / `backup` in `src/slint`, `src/engine`, or the API header. |
| 4 | Whole-app reset | new file model + UI hand-off | Only key-binding reset exists. |
| 5 | Settings profiles per radio / use; multi-PC sync | profiles: new file model. Sync: folder-based planner is new file model; a cloud/service sync is **blocked on an owner decision** (it needs an account and a server, both forbidden to this lane) | Nothing exists. |
| 6 | Telemetry on/off switch in the UI | **already built** (the switch: `src/slint/system_logic.cpp:922` reads `fact.telemetry.enabled`; engine op at `engine.cpp:8442-8446`). The brief still asks for a standalone model and the "off sends nothing" test, so a small model is built. | `PRIVACY.md:234` text about a hand edit is stale; listed. |
| 7 | Signature on the update manifest | new file model (Ed25519 verifier from RFC 8032 + envelope + signing tool) + needs a forbidden file (`updater.cpp`) | `updater.cpp:235-262` checks version, sha256 field, host allow-list. Whether HTTPS alone is enough is the owner's decision. |
| 8 | Code-signed setup | new file model (dormant `installer/sign-setup.ps1`, never run) + needs a forbidden file (`cascade.iss` `SignTool=`); buying a certificate is an owner decision | `docs/MANUAL.md:1862` says "not code-signed yet". |
| 9 | USB re-plug recovery | new file model (device-arrival watcher) + needs a forbidden file (engine). FEATURE-GAPS R10 belongs to the engine serial lane A: standalone watcher built, not wired. | No `WM_DEVICECHANGE` / `RegisterDeviceNotification` anywhere in `src`. The interface classes the engine lists (`src/usb/winusb_device.cpp`): `GUID_DEVINTERFACE_USB_DEVICE` and `GUID_DEVINTERFACE_WINUSB`. |
| 10 | Sleep / resume handling | new file model + needs a forbidden file (engine) | No `PBT_APM*` / `RegisterSuspendResumeNotification` in `src`. |
| 11 | Keep the PC awake while recording / scheduled capture | new file model + needs a forbidden file (recorder, schedule runner) | No `SetThreadExecutionState` in `src`. |
| 12 | CPU / GPU / memory readout; low-end / reduced-effects mode | part **already built** (memory: `process_resources.cpp` reads GDI/USER/handles/private bytes and DXGI video memory for one log line; `engine_measure.cpp:149` reads working set inside a bounded measurement run only). New file model for the live meter (process CPU %, working set, system memory, GPU engine utilisation by counters) and the reduced-effects policy. | No reduced-effects / low-end switch anywhere in `src` (grep). The UI half is `.slint`: hand-off. |

Nothing in the table is `blocked` outright; row 5's cloud sync and the three owner decisions (HTTPS-vs-signature, signing the manifest, buying a certificate) are listed under "Owner decisions" at the end.

## What was built (all new files; nothing wired)

No forbidden file was edited. The only existing file touched is `tests/CMakeLists.txt` (seven appended lines at its END: the `foxsdr_manifest_sign` executable and its dependency).
Every model is standalone, takes its inputs and its folders as arguments, and has an injected or fake path so no test touches a radio, a power setting, a key store or the real settings.

| Row | Files | What it is |
|-----|-------|------------|
| (shared) | `src/core/sys_hash.{hpp,cpp}`, `sys_sha_consts.inc`, `tools/gen_sha_constants.py` | SHA-256 and SHA-512 with no OS provider; the round constants are generated from their FIPS 180-4 definition with exact integers. |
| Manifest signature | `sys_ed25519.{hpp,cpp}`, `sys_manifest_sig.{hpp,cpp}`, `tools/manifest_sign.cpp` (target `foxsdr_manifest_sign`) | Ed25519 verify and sign written from RFC 8032; a versioned envelope (format, version, alg, keyId, base64 payload, base64 signature) over a domain-separated message; a trusted-key LIST (rotation); the owner key is an EMPTY constant (`kOwnerKeyHex`), so `verifyEnvelope` answers `NoTrustedKey`, never a pass; a signing tool that reads a seed FILE the owner holds and generates nothing. |
| Code-signed setup | `installer/sign-setup.ps1` | DORMANT, never run. Refuses (exit 2) with no certificate named by `FOXSDR_SIGN_CERT_SHA1` / `FOXSDR_SIGN_PFX` (+ password FILE), no https `FOXSDR_SIGN_TIMESTAMP_URL`, or no `.exe`, all before it looks for signtool; `-Check` prints what would run. Parse-checked only. |
| USB re-plug | `sys_device_watch.{hpp,cpp}` | Message-only window on its own thread; `RegisterDeviceNotification` for `GUID_DEVINTERFACE_USB_DEVICE` and `GUID_DEVINTERFACE_WINUSB` (the two classes `src/usb/winusb_device.cpp` lists); the interface path parsed to vid/pid/class with the serial dropped; a coalescer (a re-plug burst is one action after 1.5 s of quiet, at most every 10 s). Opens, enumerates and resets nothing. R10 is the engine serial lane A's: this is its standalone half. |
| Sleep/resume | `sys_resume_watch.{hpp,cpp}` | `ResumeGate` (pure, clock injected): one action 4 s after the first resume of a burst, a suspend cancels, at least 30 s between actions (deferred, never dropped). `ResumeWatcher` registers `RegisterSuspendResumeNotification` by callback (no window) and runs the action on its own thread. |
| Keep awake | `sys_keep_awake.{hpp,cpp}` | Reference-counted `SetThreadExecutionState` request. The OS state is per thread, so a holder thread owns it while any lease is held; leases release in their destructor on every exit path; the OS call is an injectable function table. |
| CPU/GPU/memory + low-end mode | `sys_resource_meter.{hpp,cpp}`, `sys_effects_policy.hpp` | Process CPU %, working set, system memory (load, total, available), GPU engine utilisation from the "GPU Engine" performance counter (pdh.dll loaded by name, English counter names), each with its own available flag; a struct of six reduced-effects switches with hysteresis (5 high samples in, 30 calm out, a dead band), a user override (On/Off/Auto) and a one-time low-end start. |
| Portable mode | `sys_portable.{hpp,cpp}` | `foxsdr.portable` marker beside the exe -> `<exe>/<name>/{settings,data,logs}`; `portableEnvOverrides` returns APPDATA and LOCALAPPDATA, which is the whole wiring (config.cpp, diag_log.cpp, plugin_host.cpp already read them). Never writes outside the exe folder (links followed). |
| Export / import / backup | `sys_bundle.{hpp,cpp}` | One-file versioned bundle (config.json, bookmarks.json, markers.json; a name allow-list, sha256 per file, stored not compressed); secrets and install identity never exported; import validates all-or-nothing, forces every network listener switch OFF, drops secrets again, plans against the destination, needs a confirm token, keeps `<name>.before-import-<stamp>`, rolls back on failure; backup names and pruning. |
| Whole-app reset | `sys_reset.{hpp,cpp}` | Dry-run plan (allow-listed files only), confirm token, folder-named-`foxsdr` guard, execution by moving into `reset-<stamp>` (nothing deleted), rollback, `undoReset`. |
| Profiles, per-radio, multi-PC | `sys_profiles.{hpp,cpp}` | Named secret-free profiles; `planSwitch` merges the profile with THIS machine's own secrets, identity, usage consent and network switches; radio-model-to-profile map (serial-looking keys refused); a three-way folder-sync planner (push / pull / conflict, never a delete). |
| First-run wizard + starter presets | `sys_first_run.{hpp,cpp}` | Steps Welcome/Radio/Preset/Privacy/Done with gating and skip; eight starter presets as `cascade::core::Bookmark` offered only where every frequency fits the radio's window; marker file `first-run-done` (no config.hpp edit). |
| Telemetry switch | `sys_telemetry_switch.hpp` | The on/off model and the rule "OFF SENDS NOTHING, OFF DELETES THE ID"; cross-checked against the real `ConfigStore::load`. |

Also: `tools/mutation_batch_sysmodels.sh` + `tools/sysmodels_mutate.py` (the break-it batch), `tests/sys_ed25519_vectors.hpp` (generated from the RFC text).

## Tests (real result lines; Release, `D:/foxsdr-build/en-sysmodels`)

| Test | Result line | Covers |
|------|-------------|--------|
| `test_sys_ed25519` | `test_sys_ed25519: 8914 checks, 0 failed` | published SHA digests incl. the million-'a' message, streaming == one-shot at every length to 300; all five RFC 8032 section 7.1 vectors (public key, signature, verify); every single-bit change of signature (512), key (256) and message refused; S+L, S=L, all-ones S; the identity key with R = B, S = 1 (verifies) and its y = p + 1 and sign-bit aliases (refused); base64 (RFC 4648 vectors, canonical-padding refusals); every envelope error branch; 4000-mutant envelope fuzz (16 stayed valid, all with the original payload). |
| `test_sys_manifest_tool` | `test_sys_manifest_tool: 89 checks, 0 failed` | the built tool on scratch files with the RFC seed: pubkey equals the RFC public key, sign then verify, wrong key / tampered envelope / short seed / missing input / empty manifest / unknown command / no arguments all exit 2 and write nothing; source scans (no key provider, store, random source or signing utility named in any lane file; the dormant script refuses before it looks for a tool and nothing references it). |
| `test_sys_power` | `test_sys_power: 333 checks, 0 failed` | keep-awake counting, exit paths, OS refusal, empty table, 8 threads x 300 acquire/release with strict set/clear alternation; the one real read-back (`set -> previous 0x80000000, repeat read 0x80000001, at clear 0x80000001, after clear 0x80000000`); resume gate scenarios; both real OS registrations succeed and are released; a real `WM_DEVICECHANGE` sent to the watcher's window produces one coalesced action with the right counts; parse boundaries and a 4000-mutant fuzz; 4-thread injection; source scans. |
| `test_sys_resources` | `test_sys_resources: 212 checks, 0 failed` | CPU arithmetic boundaries, GPU instance names and the busiest-engine rule, the meter with supplied numbers (each source failing alone), the policy's hysteresis scenarios; one real read: `cpus 32, working set 9 MB, memory 65% of 63.7 GB, process cpu 2.0%, gpu 2.7% (process available)`. |
| `test_sys_portable` | `test_sys_portable: 85 checks, 0 failed` | marker rules, folder-name rules, nothing outside the scratch stick changes, a file or a junction at the data folder's name is refused (the junction case RAN), and `ConfigStore::defaultPath/save/load` resolve inside the stick once APPDATA points at it. |
| `test_sys_bundle` | `test_sys_bundle: 7650 checks, 0 failed` | a REAL config from `ConfigStore::save` exported (no secret value anywhere in the text) and read back by `ConfigStore::load`; every export and import error branch; declared-size bombs refused with no data behind them; listeners imported off; plan / commit / stale token / rollback with a real read-only target; backup pruning; 6000-mutant fuzz (91 accepted, all with every validated field intact); 6-thread parse. |
| `test_sys_setup` | `test_sys_setup: 27342 checks, 0 failed` | telemetry model over every combination, 20000 random operations and against the real loader; reset plan / token / stale / execute / undo / occupied / rollback (a real open handle makes the last move fail); profiles, switch, radio map with a 3000-mutant fuzz, sync grid; starter catalogue fit rule and round trip through `FreqManager`; wizard flow and skip from every step; first-run marker. |

Concurrency: the three watchers and the keep-awake guard are tested from several threads; the bundle parser from six. The resource meter, policy, resolver and planners are single-owner objects by design (not shared across threads), so no concurrency test applies.

## Break-it table (one mutation per behaviour; the batch is `tools/mutation_batch_sysmodels.sh`)

Each mutation was applied exactly once (the helper aborts otherwise), the guarding test rebuilt and run, the file restored with git, and at the end every lane test rebuilt and run green again (`restored test_sys_ed25519 | exit 0 | ... 0 failed` for all seven).

| Behaviour removed | Test | Result |
|-------------------|------|--------|
| SHA-256 round function changed | test_sys_ed25519 | RED, 4 FAIL |
| Ed25519 S < L rule | test_sys_ed25519 | RED, 1 FAIL |
| Ed25519 canonical-y rule | test_sys_ed25519 | RED, 2 FAIL |
| Ed25519 x = 0 sign-bit rule | test_sys_ed25519 | RED, 1 FAIL |
| manifest sig: no-trusted-key refusal | test_sys_ed25519 | RED, 1 FAIL |
| manifest sig: domain-separation prefix | test_sys_ed25519 | RED, 1 FAIL |
| manifest tool: verify verdict | test_sys_manifest_tool | RED, 2 FAIL |
| dormant script: refusal text | test_sys_manifest_tool | RED, 1 FAIL |
| keep-awake: clear on release | test_sys_power | RED, 12 FAIL (first run of this row died with exit 127 because the test used `.at()`; the test was fixed to report the broken expectation, then rerun) |
| resume: rate limit | test_sys_power | RED, 3 FAIL |
| resume: suspend cancels a pending action | test_sys_power | RED, 2 FAIL |
| device watch: radio-class filter | test_sys_power | RED, 5 FAIL |
| resource meter: bad GPU values dropped | test_sys_resources | RED, 1 FAIL |
| effects policy: hysteresis | test_sys_resources | RED, 31 FAIL |
| portable: containment with links followed | test_sys_portable | RED, 2 FAIL |
| bundle: sha256 check | test_sys_bundle | RED, 2 FAIL |
| bundle: listeners imported off | test_sys_bundle | RED, 365 FAIL |
| bundle: confirm token | test_sys_bundle | RED, 5 FAIL |
| bundle: name allow-list | test_sys_bundle | RED, 13 FAIL |
| reset: confirm token | test_sys_setup | RED, 8 FAIL |
| reset: folder-name guard | test_sys_setup | RED, 4 FAIL |
| profiles: machine keys carried | test_sys_setup | RED, 6 FAIL |
| profiles: sync conflict | test_sys_setup | RED, 2 FAIL |
| first run: preset fit rule | test_sys_setup | RED, 26 FAIL |
| telemetry: off clears pending | test_sys_setup | RED, 1405 FAIL |
| telemetry: off sends nothing | test_sys_setup | RED, 2372 FAIL |

Not break-it-able, said plainly: the Ed25519 "point is not on the curve" refusal (no test supplies a signature that would otherwise verify under an off-curve key, so removing the check is invisible to the vectors; it is exercised only by the envelope fuzz and the y = 2 case, which fail for any reason); the OS-registration paths of the two watchers (their tests show registration succeeds on this machine, not what the OS does on a real resume or plug).

## Gates (once, at the end)

1. **Own tests** (ctest, Release): `test_sys_ed25519`, `test_sys_manifest_tool`, `test_sys_power`, `test_sys_resources`, `test_sys_portable`, `test_sys_bundle`, `test_sys_setup`, plus `test_json_dump_policy` and `test_thread_inventory`: all Passed (`100% tests passed out of 10`, together with `test_engine_control_thread`).
2. **Full build**: `cmake --build D:/foxsdr-build/en-sysmodels --config Release --parallel 3` -> `BUILDEXIT 0`, 0 compile errors, 570 test executables.
3. **Headless set, 710 tests, in nine slices under 10 minutes** (`ctest -L headless -I a,b -j 3..4 --timeout 150`). First pass on this loaded machine: 51 tests red. Reruns, one slice at a time and at `-j 1/2`, sorted them:
   - **One real finding, fixed.** `test_thread_inventory` refused the three new threads (`sys_device_watch.cpp`, `sys_keep_awake.cpp`, `sys_resume_watch.cpp`). Fix: three exemption rows with reasons in `tests/thread_inventory_rows.inc` (commit f1fdae92). This is the ONE edit outside the lane's own files besides `tests/CMakeLists.txt`; it is alphabetical and three rows. Rerun: Passed. (`test_json_dump_policy` had also been satisfied by giving `sys_manifest_sig.cpp`'s `dump()` the `error_handler_t::replace` argument.)
   - **Known false failures** (STANDARD-RULES): `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`, `test_window_failed_app`.
   - **Load flakes that pass on a serial rerun in this build**: `test_engine_api_03`, `_conformance` (it had segfaulted under load), `_multi_radio`, `_merge`, `_schedule`, `_plugin_smoke_radio`, `test_engine_main_smoke`, `test_engine_shell_guard`, `test_soapy_source`, `test_bounded_hooks`, `test_engine_publish_alloc`, `test_iq_open_async`, `test_pa_stream_list_guard`, `test_plugin_hostile`, `test_plugin_unload_handshake`, `test_sdrplay_probe`, `test_soapy_enum_proc`, `test_health_app_stepped`, `test_tci_engine`, `test_udp_audio`, `test_web_pictures`, `test_thread_beats`, `test_plugin_window_stepped/_free`, `test_record_finish_async_stepped`, `test_scope_window(_stepped)`, `test_shutdown_budget_free`, `test_tx_audio`, `test_window_threaded_look`, `release_tarball_check_selftest`, `engine_only_configure`, `telemetry_worker`. `test_crash_upload_free` fails about one run in three in BOTH trees (3 runs each: 1 red, 2 green).
   - **Red in this tree AND in the base commit d3630d78** (built for the comparison in a temporary worktree of my own, run under the same load, then removed): `test_engine_api_decode_radio` (timeout), `test_engine_api_source_kinds`, `test_excuse_cap`, `test_failure_files`, `test_plugin_api`, `test_rtl_tcp_source`, `test_rtlsdr_live`, `test_usb_bounded_walk` (segfault). Not caused by this lane: none of them can reach a symbol this lane added. Their cause was not investigated.
   - `test_pass_control` (known load flake) was not rerun.
   - **Not run**: the `gui`-labelled tests (not part of the headless set; this build has no Slint window).
   - Caveat on `test_rtlsdr_live`: its name says it uses a live radio; it was in the headless set and was run as part of the gate, red here and in the base. I did not read it to see whether it opens a radio when one is present.
4. **Real settings**: `%APPDATA%\foxsdr\config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, identical to the value in the rules, read-only hash taken after everything above.


## UNVERIFIED (nothing here was run against the thing it describes)

- **Real resume and real re-plug.** The OS notifications were registered and released, and synthetic `WM_DEVICECHANGE` / power events were fed through the same code; no machine was put to sleep and no radio was plugged or pulled (no radio may be opened, no power setting changed).
- **Keep-awake on a real recording.** Only the read-back of a few milliseconds of `ES_SYSTEM_REQUIRED` on a throwaway thread was done (released before the test ended; no power plan or setting was changed). Whether the display or the machine actually stayed awake for a long recording is not shown.
- **Portable mode through the real program.** `ConfigStore` was shown to follow APPDATA; the program was not started from a stick, and any code path that finds its folders another way (a Store package, an installer, `SHGetKnownFolderPath`) was not searched beyond a grep of `getenv("APPDATA")` / `getenv("LOCALAPPDATA")` in `src/core` (config.cpp, freq_manager.cpp, diag_log.cpp, plugin_host.cpp, problem_report.cpp, sentinel_host_win.cpp).
- **GPU readout on other machines.** One machine, one driver: the counter set was present and gave numbers; where it is absent the reading is "not available" (tested with supplied inputs only).
- **Starter-preset frequencies and RTL-SDR / SDRplay / Pluto windows.** Typed from general knowledge of standard allocations (FM broadcast, 121.500 MHz, marine channel 16 at 156.800 MHz, PMR446 channel 1, 2 m 145.500 and APRS 144.800, WWV 5 and 10 MHz, the 49 m band, 40 m and FT8) and NOT re-verified against a primary document in this session. The 88-470 MHz window for families with no range constant in the source is conservative, not a datasheet value. The owner reviews the list.
- **`.slint` binding names** (see the hand-off) are proposals; none exists.
- **The signing script** was parse-checked, never run (rule). Its `signtool` flags (`/fd sha256 /td sha256 /tr <url> /sha1 <thumb> | /f /p`) are written from memory of the tool's documented options and were NOT checked against a signtool help page or a run.
- **Sources not opened**: FIPS 180-4 (the SHA constants are generated from its definition and the digests are published, widely known test values: both agree, which is the only check), the Microsoft documentation for `SetThreadExecutionState`, `RegisterSuspendResumeNotification`, `RegisterDeviceNotification`, the performance counters. Their behaviour was established by the read-backs and registrations above.
- **The shipping skill text** carries no literal "off sends nothing" sentence (opened); the rule modelled is the one in `telemetry.hpp`, `config.cpp` (bad id switches reporting off) and the engine's off branch.

## Wiring and screen hand-off

Everything below needs a file this lane may not edit (`engine.cpp`, `pipeline.cpp`, `fox_engine_api.cpp`, the recorder, `config.*`, `updater.cpp`, `cascade.iss`, any `.slint`) or another lane's neighbour (`main.cpp`). English strings are listed; none is translated.

### Engine / process start (no screen)

1. **Portable mode - `main.cpp`, before anything resolves a path.**
   `auto p = cascade::core::sys::resolvePortable(cascade::core::sys::currentExeDir());`
   If `p.markerFound && !p.portable`: show `p.error` and stop (do NOT fall back to %APPDATA%). If `p.portable`: `ensurePortableRoots(exeDir, p, err)` (stop on failure), then `_putenv_s` each pair of `portableEnvOverrides(p)`. Strings: "This copy of FoxSDR is set up to run from a stick, but its data folder cannot be used: %s".
2. **Device arrival - engine (serial lane A).** Own a `DeviceArrivalWatcher({1500, 10000}, onSettled)`; `start(true)` at engine start, `stop()` at shutdown. `onSettled(summary)` posts the same command the manual Rescan button posts; when a radio that was open is among the removals, the engine's own close path runs first. The watcher never opens a device.
3. **Resume - engine.** Own a `ResumeWatcher({4000, 30000}, action)`; `action` = close and re-open the current radio exactly as a saved-radio open does; also re-open the audio device. `start(true)`.
4. **Keep awake - engine, at the recorder and the schedule runner.** One `KeepAwake` object; `auto lease = keepAwake.acquire();` when a recording starts or a scheduled capture begins, `lease.reset()` when it ends (the lease may be stored in the recording object; it releases in its destructor). Display not kept on. If `!keepAwake.active()` after acquire, log "could not keep the PC awake" once.
5. **Resource meter and effects policy - engine, 1 Hz.** `ResourceMeter meter(realResourceSources()); EffectsPolicy policy;` each second `auto s = meter.sample(); const auto& sw = policy.update(s);`, publish as facts. Proposed fact names (none exist): `fact.perf.cpu_percent`, `fact.perf.memory_mb`, `fact.perf.system_memory_percent`, `fact.perf.gpu_percent` (each "" when not available, so the UI shows "n/a", never 0), `fact.perf.reduced` ("1"/"0"). Op `perf.mode` value "auto" | "on" | "off" -> `policy.setMode`. Call `policy.startLowEnd(first sample)` once at start.
6. **Manifest signature - `updater.cpp`.** After the manifest bytes arrive and BEFORE parsing: `auto keys = sys::ownerTrustedKeys(); if (!keys.empty()) { std::string payload; if (sys::verifyEnvelope(body, keys, payload) != SigResult::Ok) refuse("the update information is not signed by FoxSDR"); body = payload; }` and parse `body` exactly as today. With no key configured the behaviour is today's. The update service (Worker/site) must then serve the envelope (`foxsdr_manifest_sign sign --seed-file <owner seed> --in manifest.json --out envelope.json`); the `sha256` inside the signed manifest must be of the SIGNED setup file.
7. **Code signing - `cascade.iss` and `tools/release.ps1`.** Once a certificate exists: call `installer/sign-setup.ps1 -SetupPath <setup.exe>` after the installer is built and BEFORE its sha256 goes into the manifest (a signature changes the file); add `SignTool=` / `SignedUninstaller=yes` to `cascade.iss` only if the owner wants Inno to sign too. Never run it without the three environment variables.

### Screens (every control: English label, op or fact, range, default)

| Screen | Control | English label | Op / fact | Range | Default |
|--------|---------|---------------|-----------|-------|---------|
| System / Settings | button | "Export settings..." | host picks a file; `exportBundle(files, {stripIdentity=true, appVersion, createdUtc})` | - | - |
| | note under it | "Passwords, tokens and your anonymous id are never exported. %d item(s) were left out." | `ExportReport::stripped.size()` | - | - |
| | button | "Import settings..." | `parseBundle` -> show `ImportResult::notes` and the plan (New / Replace / Identical per file) -> `commitImport` with the plan's token | - | - |
| | confirm | "Replace %d file(s)? Your current ones are kept as .before-import copies." + button "Import" | `ImportPlan::confirmToken` | - | - |
| | button | "Back up settings now" | `backupFileName(stamp)` into the user's backup folder, then `pruneBackups(dir, keep)` | keep 1..50 | 10 |
| | toggle | "Back up automatically when FoxSDR closes" | new setting (needs `config.hpp`) | on/off | off |
| | button | "Reset everything to defaults..." | `planReset` -> list, then `executeReset` with the token, then restart | - | - |
| | list | "These files will be set aside: ..." / "%d other file(s) in the folder are left alone." | `ResetPlan::items`, `leftAlone` | - | - |
| | checkboxes | "Also set aside installed plugins" / "Also set aside log files" | `ResetOptions::includePlugins/includeLogs` | on/off | off, off |
| | button | "Undo the reset" | `undoReset(stamp)` | - | - |
| | dropdown | "Profile" | `listProfiles`, `planSwitch` / `applySwitch` | up to 64 names | none |
| | buttons | "Save as profile...", "Delete profile" | `saveProfile`, `deleteProfile` | name 1-40 chars | - |
| | dropdown | "Use this profile with: <radio model>" | `RadioProfileMap::set` | up to 64 | none |
| | folder box | "Shared folder for syncing profiles" | `planSync` (host copies) | - | empty (off) |
| | toggle | "Usage reports" | already built (`fact.telemetry.enabled`) | on/off | on |
| | dropdown | "Reduced effects" | `perf.mode` | Auto / On / Off | Auto |
| | read-out | "CPU %d%%   Memory %d MB   GPU %d%%" ("n/a" when unavailable) | `fact.perf.*` | - | - |
| First run | page 1 | "Welcome to FoxSDR" + "Next" / "Skip setup" | `FirstRunWizard` | - | - |
| | page 2 | "Which radio do you have?" + the labels from `radioChoices()` | `chooseRadio` | 11 choices | none (must pick) |
| | page 3 | "Add some starting stations?" + the titles from `startersFor` + "None" | `choosePreset` | 0..8 | None |
| | page 4 | "Anonymous usage reports" with the switch shown and explained | `chooseTelemetry` | on/off | on (the owner's decision; see below) |
| | finish | "Done" | `outcome()` -> set source kind, `FreqManager::addMany`, usage switch, then `markFirstRunDone` | - | - |
| Portable | (none) | - | marker file | - | off |

First-run labels (English, from `radioChoices()`): "I have no radio yet (try the built-in test signal)", "RTL-SDR dongle", "Airspy R2 / Mini", "Airspy HF+", "SDRplay RSP", "HackRF", "RX888 mk2", "Mirics MSi2500", "ADALM-Pluto", "A recorded I/Q file", "A sound card input". Preset titles: "FM broadcast band", "Airband emergency channel", "Marine VHF channel 16", "PMR446 channel 1", "2 m amateur FM", "Time signals (WWV)", "Shortwave broadcast, 49 m", "Amateur HF".

## Sources opened

| URL | Licence / status | Used for |
|-----|------------------|----------|
| https://www.rfc-editor.org/rfc/rfc8032.txt | IETF Trust text of the standard (read-only reading) | Ed25519 definition, section 7.1 test vectors (extracted by script into `tests/sys_ed25519_vectors.hpp`) |
| (repository files only otherwise) | | `src/usb/winusb_device.cpp` (the two interface classes), `src/core/config.cpp`/`.hpp`, `telemetry.hpp`, `process_resources.*`, `engine_measure.*`, `freq_manager.*`, the driver headers for tuning windows, `~/.claude/skills/shipping` |

No GPL/LGPL/AGPL source was read. No software was downloaded; nothing was added to any Windows store; no prompt appeared (every OS call used here is a registration by handle or callback, a read, or a counter query).

## Owner decisions

1. **Is HTTPS alone enough for the update manifest, or must it be signed?** The mechanism exists; enforcing it means choosing a key and a signing step in the release flow.
2. **Signing the manifest: who generates and holds the Ed25519 seed (offline), and when `kOwnerKeyHex` is filled in.** No key was generated, stored or read by this lane.
3. **Buying a code-signing certificate** (OV, EV or Azure Trusted Signing), its cost, and the timestamp service; then wiring `sign-setup.ps1` into the release.
4. **Usage reports in the first-run wizard.** The wizard shows the switch defaulted ON (the product decision on `AppConfig::telemetryEnabled`); the PECR note in `config.hpp` applies - the wizard is the natural place to ask for consent instead, if the owner wants that.
5. **A sync service of our own** (an account and a server) for multi-PC sync: not built; the folder-based planner needs none.
6. **Starter preset list**: review the frequencies and the 88-470 MHz window for RTL-SDR / SDRplay / Pluto.
7. **Reset scope**: whether "reset everything" should ever include plugins and logs (built as opt-in) and whether the `reset-<stamp>` folder is removed automatically after some days.
8. **Keep awake**: the display is never held on; say so if a recording should also keep the screen on.
9. **`PRIVACY.md:234`** still describes usage reporting as a hand edit of `telemetryEnabled`; the switch exists in the UI.
