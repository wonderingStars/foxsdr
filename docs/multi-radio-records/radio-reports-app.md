# Wave 8, radio reports: the app side (D4, D5, C2, C8, L1, L3, L7, L11)

Branch `feat/radio-reports-app`, cut from `d4f855c5` in `D:/foxsdr-wt/en-tuning`, build folder `D:/foxsdr-build/en-tuning`, scratch `D:/foxsdr-build/rra-scratch/`.
Written 2026-11-16. NOTHING IS PUSHED, DEPLOYED OR SENT. No radio was opened, no outside request was made, no window was shown on the visible desktop
(the one test that builds engine windows ran on the hidden desktop `FoxSDRTestsRRA`). The contract (`docs/radio-reports-contract.md`) and the Worker
(`telemetry-worker/`) are NOT edited: what is wrong with them is under CONTRACT ISSUES. The real `%APPDATA%/foxsdr/config.json` still has sha256
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (read at the start and at the end).

## What was built, in the order of the brief

| # | Row | Verdict | Where and the proof |
|---|---|---|---|
| 1 | The on-disk queue (C2) | BUILT | `src/core/radio_reporter.{hpp,cpp}`: folder `radio-reports` beside `config.json`; a file per failure; an append-only segment per session for the journal (header line, one message a line, `.pos` cursor, rotation at 256 KiB); every append forced to disk (`FlushFileBuffers`/`fsync`) before the call returns; bounds 200 failure files, 4 MiB, 14 days, the oldest dropped and counted in the next message's `dropped`. `test_radio_reporter`: a child process is ended with `_Exit` (no destructor, no flush) after queueing an open's start, a failure and a close, and the parent starts a new session over the same folder and delivers all three (`kill` block); the cursor survives a restart (accepted lines are not sent again); the 200-file and 4 MiB bounds drop the oldest and say so. |
| 2 | The radio failure report at every call site | BUILT | `Engine::reportFailedOpen` (`src/engine/engine_radio_reports.cpp`) called from `finishDeviceOpen` (user pick, saved-radio retry, automatic reopen; a busy-again retry too, which the anonymous count skips; and the open the user had moved on from, with `superseded`), `applyConfig` (start-up restore), `restoreExtraRadioDevice` (radios 2 to 8), the patch radios (`engine_patch_radios.cpp`) and the sound-card input (`engine_soundcard.cpp`). The serial is its own field and is out of the reason text; no cap of the old reports applies; valid without an install id when Usage reporting is off; sent with Diagnostics on or off. Proof: `test_radio_reports_engine` drives the start-up restore, three saved-radio retries (every failure its own file, attempts 1 to 3, triggers startup-restore and saved-retry), the HF+ sentence (class `control`, no serial in the reason), the anonymous form, the superseded open, and the added-radio restore, against a fake radio, and hands all 23 messages to the Worker's own validator (`node`, `telemetry-worker/radio-reports.js`): 0 refused. NOT DRIVEN, held by a source scan only: the patch-radio and sound-card sites (the structural check is in the same test); UNVERIFIED by a drive. Break-it: the report taken out of `finishDeviceOpen` turns `fails.size() == 3` red; restored. |
| 3 | The first-session record (L7) | BUILT | `Reporter::firstSession`, called by `Engine::radioReportsPass` about a minute in and by `reportFailedOpen` on a failure; only when `launches == 1`, at most one a minute and 6 a session; fields exactly the usage record's (from `Engine::telemetrySnapshot`, the same function the usage record uses); it never settles a ledger. Proof: `test_radio_reports_engine` (a minute in, the health tokens so far in `health`, `ledger->encoded()` identical before and after), `test_radio_reporter` (not for a returning user, a bad trigger refused, 6 at most, a minute apart), Worker validator 0 refused. |
| 4 | The journal, the bound, the beat's `radios`, the usage record's `window` | BUILT (the event points under row 5) | The six kinds and `session_env` as `radio_found`, `radio_open` (start, ok), `radio_settings` (at open), `radio_stream`, `radio_recovery`, `radio_close`, `session_env`; the per-day bound with the first-and-last rule and the dropped count (100 failures and 3,000 journal events a UTC day, the first of each (radio, class) or (radio, kind, word) always passes, the newest later one is held and released at the day change, the next start or quit with `dropped` set; stream events at most one a minute per radio and 5 minutes after the day's 500th). `HeartbeatSender::setRadios`/`nextBeatJson` (the beat's `radios`, at most 8, sent only while the live switch is on), `TelemetryReport::window`, `ServiceOptions::windowKind` (set by `src/main.cpp` "legacy" and `src/slint/main.cpp` "new"). Proof: `test_radio_reporter` (bound: 20 failures with a limit of 5 give 5 + 1 sent, 14 said in `dropped`, the last of the loop kept, the first of a new pair passes, the day roll-over releases the held one; the journal bound likewise: 4 + 1 + 1 sent and `dropped == 7`; nothing dropped uncounted), `test_radio_reports_fields`. Break-its on the reporter, all RED and restored: the daily bound removed (26 reds in the one run), the live switch ignored, `bad id` treated as a refusal, turning Usage reporting off keeping the journal. |
| 5 | "Opened, no samples after N seconds" (L3); "a session ended with no radio opened" (L1); why `scan_none` is rare | BUILT, with one CONTRACT ISSUE | L3: `Engine::radioReportsPass`, 20 s after an open, `radio_stream` with `samplesPerSec` 0 (the contract's own meaning of 0), urgent; the test opens a fake radio that reads nothing, ages it 30 s and finds the event, and with samples flowing finds none. L1: `session_end` at the first step of the teardown (the survey's top-ten items 1, 2 and 5 are the pipeline's fault latch for "radio died", the no-samples look, and the scan that lists nothing: `radio_close` cause `died` is read from `Pipeline::faulted()`, `radio_found` from every scan). `session_end` is NOT in the contract (CONTRACT ISSUE 2). **`scan_none` is rare because it cannot fire**: `Engine::scanNative()` always appends the Pluto and rtl_tcp address rows, so `nativeDevices_.empty()` is never true after a scan (the check ran only before the first native scan: 34 installs ever sent it). Fixed: `foundNativeRows()` counts the rows that are radios; the "a radio was listed" milestone had the same fault and is fixed with it. The existing `test_health_app` case "a scan that finds nothing" never covered it: its hook returns a list without the address rows, and its `listed()` helper counts them, so it takes the "something listed" branch. `test_radio_reports_engine` proves it with a scan that lists the two address rows and nothing else (RED when `nativeDevices_.empty()` is put back, GREEN fixed). |
| 6 | L11: the update-channel field, and a marker for tests and CI | (a) NO APP DEFECT; (b) BUILT, Worker side is a CONTRACT ISSUE | (a) The application sends `uch` (`TelemetryReport::toJson`, filled from `interfaceCfg_.updateChannel`, default `stable`; held by `test_radio_reports_engine` through the engine's own snapshot). The field was added by `297aa241` (2026-10-27) and is first in `v0.99.104`; a copy older than that sends none, which is what "blank on all 214 installs active in 7 days" reads as when most of them are older. I could not check the live data (no outside request): the owner should group `blob15` by version before believing it is a defect. (b) `syntheticRun()` (environment: `FOXSDR_SYNTHETIC` set and not 0, `CI`, or `GITHUB_ACTIONS`) puts `synthetic: 1` on the usage record and the beat; the Worker does not drop it yet (CONTRACT ISSUE 3). The owner's machines are covered by setting `FOXSDR_SYNTHETIC=1` there. |
| 7 | Never sent | BUILT | `test_radio_reports_never_sent`: a scripted session whose log ring holds a frequency in six forms (MHz, Hz, kHz with a comma, a bare number of hertz, two frequencies), a bookmark, a recording name, a folder, paths (drive, home, UNC), an account name, a computer name, a position, decoded text, a callsign, a home address and a host name, and the radio's serial in a sentence; radios fail, the journal runs with hostile free text, the first session is built from a usage record with a `position` and a `freq` key. The queue files as written (switch OFF) and every request the sender makes (switch ON, to a stand-in) are searched for 17 secrets: **0 leaks**, plus a structural pass (no forbidden key at any depth, no string with a path or a hertz number): 0. It found three leaks in the first draft (a bare frequency with no unit in a radio line, a UNC path whose share and file survive the upload scrub's host mask, and a `band` field that accepted a number); all three are fixed. Break-it: the scrub taken out of `scrubText` gives 7 leaks and RED; restored. The text rules themselves are held to the Worker's own parity table, `radio-text-rules.json` (557 strings, 0 differ), in `test_radio_report`. |

### Event points, by kind

| Kind | Built at | Not built |
|---|---|---|
| `radio_fail` | the five sites above | `firmware` and `library` (no driver hands them over: NOT BUILT) |
| `radio_found` | `radioReportsScanLanded`, after the native scan (usable rows; radios present on the bus with no usable driver bound, binding `wrong-driver`, with `usbId`) and after the SoapySDR scan | the first-run radio check (window code: `radioreport::noteFound()` is the call it makes; OPEN ROW); `usbHost`; the other bindings (`vendor-missing`, `not-visible`, `no-device`, `not-checked`) |
| `radio_open` | `launchDeviceOpen` and the restores (start, written to disk before the driver is called), `finishDeviceOpen` and the restores (ok, with `openMs`, `sampleRateSps`, and the band by name) | patch radios (failures only) |
| `radio_settings` | at open (rate, gain mode, antenna port, band) | user and automatic changes and `refused` (the G3 control-refused paths, 140 sites: OPEN ROW) |
| `radio_stream` | `closeDspMinute` (samples a second, drops, ring high-water percent, open seconds, expected rate), and the no-samples look | `modes`, `plugins`, `dspLoadPct`, `usbErrors`, `stalls`, `pluginFaults`, and `note` from "this sounds wrong" and the silence/clipping/nothing-decoded detectors (C4: window and plugin lanes) |
| `radio_recovery` | the automatic reopen (start, ok, gave-up) and the native-to-SoapySDR route fallback (G6) | the SDRplay and vendor-call recoveries |
| `radio_close` | `radioReportsPass` and `radioReportsQuit`: cause `user`, `switch-radio`, `quit`, `died` | `unplugged`, `fault`, `error` (no source for them yet) |
| `session_env` | once a session, read on a worker by `core/session_env.cpp` (processor without its clock speed, threads, RAM, graphics adapter and driver, USB host controllers, displays and scaling, audio API) | `audioClass` |
| `first_session` | row 3 | |
| `session_end` | `radioReportsQuit` (CONTRACT ISSUE 2) | |

## THE ONE LINE THE RELEASE LANE CHANGES

`src/core/radio_report.hpp`: `inline constexpr bool kRadioReportsLiveSend = false;` becomes `true` when the Worker and the site are deployed (deploy order, contract
section 11). With it false: every new kind is queued and none is posted (`test_radio_reporter`: "a build with the switch off sends none and still queues them", then
the same queue goes out when the switch is on), and the beat does not name radios (`test_radio_reports_fields`). The usage record's `window` and `synthetic` fields are
ordinary fields of a message that already exists and are sent now (an old Worker ignores them). An interactive run arms the send in `Engine::configureHeartbeat()`;
a bounded or hermetic run never does and has no queue folder at all.

## CONTRACT ISSUES (what is wrong or missing; the contract and the Worker are NOT edited)

1. **The ring's lines have a level word the contract's radio-line pattern cannot match.** `DiagLog::write` makes `HH:MM:SS.mmm info <text>` or `... warn <text>`; the
   contract's pattern (`^(stamp )?(source|soapy|...): `) has no level word, so the Worker would refuse every raw ring line as `bad-field`. The app drops the level word and
   keeps the stamp, so what it sends matches the pattern. The prefix list is otherwise confirmed against the code's real `diagLogf` prefixes (107 `source`, 35 `patch`, 20
   `soapy`, 12 `aor`, 6 `radio`, 5 `rx888`, 4 `pluto`, 4 `airspyhf`, 3 `rtltcp`, 2 `mirisdr`...), with one gap: a two-word prefix such as `radio setup:` (16 lines) is a radio
   line the pattern cannot express, and is left out.
2. **There is no kind for "a session ended with no radio opened" (L1).** The app sends `session_end` (envelope as the other journal kinds, `radioKey` empty) with the body
   `{v, opened: false, scans: 0..1000000, openFailures: 0..1000000, listed: [at most 8 model names, scrubbed], sessionSec}`. The Worker answers `refused: unknown-kind`, and
   the sender treats that as a refusal: dropped and counted, never retried. Add the kind (field table in `radio-journal-spec.js`) before the switch is turned on.
3. **The test marker needs a Worker line.** The usage record and the beat carry `synthetic: 1` from a test, CI or the owner's machine. The Worker must drop such a record
   (no row in `foxsdr_usage`, no beat row) or the 44 "new installs" of 0.99.97 stay in every figure. The radio kinds carry no marker (the contract has no field for one): a
   journal from the owner's own machine counts as a user's until the contract gives the envelope an optional `synthetic`.
4. **`dropped` cannot be said for a message with no radio key.** A `session_env`, `first_session` or `session_end` line lost to the folder's bounds is counted locally
   (`Stats::overflowDropped`) and never reaches the site. The bounds are 200 files and 4 MiB; at that point the loss is the oldest journal lines of a machine that has been
   offline for days, and the site would want to know. Either allow `dropped` on those kinds or accept the loss.
5. **Order.** The sender takes `seq` when a message is admitted, not when the event happened, so a message held by the daily bound and released at the day's end has a
   later `seq` than events whose `t` is later than its own `t`. The site's idempotence by (installId, sessionId, seq) is unaffected; a page that orders by `seq` will show the
   held event out of time order. Order by `t` (and `age`).
6. **`scrubUploadLine` turns "serial port COM#" into "serial <stripped> COM#"** (it reads `port` as the serial's value). The Worker accepts both; the report loses the word
   `port`. Harmless; the fixture `valid-radio-fail-serial-port-word` is the already-scrubbed text of a case the app cannot produce from the raw sentence, and the parity test
   skips only the reason field of it (and of `valid-radio-fail-scrub-markers`, whose second scrub would mask the port number), holding both to the Worker's validator instead.
7. **The band table is the application's.** `radioreport::bandName()` (lf below 300 kHz, mf to 3 MHz, hf to 30 MHz, vhf-low to 87.5 MHz, fm-broadcast to 108 MHz, air-band to
   137 MHz, vhf-high to 300 MHz, uhf to 1 GHz, l-band to 2 GHz, shf above, `other` for a frequency that is not positive); the contract names the words and not the edges.
   A site that wants to say "FM broadcast" for 100 MHz should say the same edges.
8. **The Worker-side `bad id` rule is a literal string.** The sender keeps a message answered `400 bad id` (the Worker deployed today) and retries it with the back-off; the
   text is from the contract, section 11, and was not read from the deployed Worker (not read here). If the deployed Worker says it differently, the app drops every message at
   its first send.
9. **`firmware`, `library`, `usbHost`, `audioClass`, `modes`, `plugins`, `dspLoadPct`, `usbErrors`, `stalls`, `pluginFaults` are optional in the tables and the app lane does
   not fill them** (no accessor, or the data is the window's or a plugin's): nothing to change in the contract, listed so a reader of the site does not look for them.

## Decisions made

* The reporter is a process-wide object (`radioreport::global()`), because the failure sites are in the engine, a worker lambda and a window thread. What happens before the
  queue can be opened (the start-up restore runs before Usage reporting is read) is kept in memory (at most 32 failures and 64 events) and filed, with the time it happened,
  when `telemetryStartup` configures it; a run with no config directory drops it.
* `seq` is taken at admission (see CONTRACT ISSUE 5); `age` is recomputed at every attempt from the event's own time (session start plus `t`).
* A message the daily bound holds is stored in `state.json` beside the queue (not in `AppConfig`), so a crash does not lose "the last of a failure loop".
* Turning Usage reporting off deletes every queued journal and first-session line and re-issues each queued failure anonymously (the owner's rule: every failure reaches him).
* Free text goes through `scrubText` at the door (the radio's serial out, every path as `<path>`, a bare long number masked, a frequency masked) and is checked again by the
  C++ port of the contract's four text rules; anything the rules still refuse is withheld whole. A `band` that is not one of the names refuses the event.
* The sender never posts from a fault handler and quit waits at most 1.5 s (`flushAtQuit`), then cancels the transfer (`UploadCancel`); a sender thread that still has not
  returned is left to finish and owns what it uses (`shared_ptr<Impl>`).

## SHARED FILE EDITS (everything outside the new files)

`src/engine/engine.cpp` (call sites, `foundNativeRows`, the open worker's `openMs`, the stream minute, the pass, the consent and heartbeat hooks, the quit step; `telemetryJournal`
now builds its record through `telemetrySnapshot()`), `src/engine/engine.hpp` (the declarations and `RadioJournalSlot`), `src/engine/receiver.hpp` (`openMs`),
`src/engine/engine_radio_saved.cpp`, `src/engine/engine_patch_radios.cpp`, `src/engine/engine_soundcard.cpp` (one call each), `src/engine/engine_service.{hpp,cpp}`
(`windowKind`), `src/main.cpp` and `src/slint/main.cpp` (one line each: which window is running), `src/core/telemetry.{hpp,cpp}` (the beat's `radios`, `window`, `synthetic`),
`tests/CMakeLists.txt` (one loop giving four tests the source root and node's path), `tests/test_error_paths.cpp` and `tests/errpath_reporters.txt` and
`tests/errpath_allow.tsv` (two reporters named, 10 PENDING lines removed, the pin lowered), `PRIVACY.md` and `docs/DIAGNOSTICS.md` (the DRAFT text below). No `.slint` file was
touched.

## THE ERROR-PATH GATE

Pin before: **1126** (the base's). Pin after: **1116** (never raised). By group, before to after: `reports-not-in-function` 610 to 602 (the six sites of `finishDeviceOpen` and the two of the patch-radio reconcile: the open-path assignments and warnings that
reached `noteRadioFail` or only the log), `G6-route-fallback` 3 to 1 (the native-driver-refused warnings in `applyConfig` and the open worker),
`G7-patch-radio` 4 to 4, the others unchanged; `reportFailedOpen` and `noteRecovery` are now reporters in `tests/errpath_reporters.txt` and the test's declared-name check reads
`core/radio_reporter.hpp` and `engine/engine.hpp` as well. The new code added no site (3252 before, 3252 after). `test_error_paths`: 24 checks, 0 failed.

## NEW ENGINE COMMANDS

None. (`Engine::noteWindowKind` is a method the service calls; nothing is reachable through the API table.)

## NEW STRINGS

None: the lane adds no user-visible text to a window (the in-app wording is an OPEN ROW).

## DRAFT FOR THE OWNER: the changed privacy wording (awaiting his approval; not legal advice)

Written into `PRIVACY.md` (a new section, **Radio failure reports and the radio journal**, and changes to the opening, "What is never sent" and "Data protection") and into
`docs/DIAGNOSTICS.md` (a note at the top), each marked DRAFT. In plain words, what a switched-off install sends:

> With Usage reporting switched off, FoxSDR sends no usage report, no still-running beat, no journal, no machine description and no first-session record. **It still sends
> one report whenever a radio does not open**, whether Diagnostics is on or off: the radio's driver, model and **serial number**, its firmware and library version where known,
> the reason the driver gave (with serial numbers, account names, computer names, addresses and frequencies taken out and every file path replaced by `<path>`), a class word for
> the reason, which part of FoxSDR tried to open it, how many times this radio has failed in this run, how long the open took, the sample rate asked for, whether you had already
> moved on to another source, how many reports were not sent because of the daily limit, whether Diagnostics was on, up to 40 radio-related lines of the log (scrubbed the same
> way), the FoxSDR version, install channel and update channel, the operating system and build, the processor architecture, the seconds since FoxSDR started, and a random number
> made for that one message. It carries no install identifier, session identifier or sequence number, so two such reports can be tied together only by the radio's model and
> serial number; the combination of version, system build, architecture and channel can still identify a machine in practice, and nothing uses it to. The network necessarily
> shows your address to the server, which does not read or store it. Such a report is kept indefinitely and is erased by quoting the radio's serial number.

With Usage reporting on, the same report carries the install identifier, and the radio journal and the machine description (processor model without its clock speed, threads,
memory, graphics adapter and driver, USB host controllers' names, audio system, displays and scaling) are sent as well. Never in any of it: a tuned or centre frequency (a BAND
by name instead), a bookmark, a file or recording name, anything decoded, a position, an address or host name, a user or computer name, a path. The serial number is kept out
of every other report as before. Nothing is sent by THIS build: the live switch is off.

## UNVERIFIED

* The live send: nothing has run against the Worker or the site (neither is deployed); the real transport ran only against a loopback server (`test_radio_reports_fields`), and the
  Worker's own validator (`node`) was held against every message the tests made.
* The patch-radio and sound-card failure sites are held by a source scan and the gate, not driven.
* `session_env` read on this machine in `test_radio_reports_fields` (counts and the text rules only); the GPU driver version (`DriverVersion` under the adapter's own registry
  key) and the USB host controller names are what Windows said here, not checked against a second machine, and Linux was not built.
* A real person's first-run check, the in-app Diagnostics and Usage wording, the Settings page showing the install identifier and the radios' serials (R8), and a real radio
  failing, are for the mouse drive and the real HF+ reporter.
* The machine-load figures: the reporter reads the queue folder (two directory listings and the segments) once every five minutes and when a bound is swept; not measured.

## OPEN ROWS FOR THE NEXT LANE

| Row | Waits for | Kind of lane |
|---|---|---|
| Turn `kRadioReportsLiveSend` to true | the Worker and site deployed and confirmed, CONTRACT ISSUES 1 to 3 settled | release lane |
| `session_end` in the contract, the Worker and the site; `synthetic` dropped by the Worker; the envelope marker for radio kinds | the owner | Worker lane |
| The in-app Diagnostics and Usage wording, the Settings page showing the install identifier and each radio's serial (R8), the Legacy window's counterparts | the owner's approval of the draft | screen lane |
| `radioreport::noteFound` from the first-run radio check (`app_window_radio_setup.cpp`, `slint/signal_publish.cpp`): `vendor-missing`, `not-visible`, `no-device`, `not-checked` bindings, and its timeout (S4) | screen lane | screen lane |
| `radio_settings` for user and automatic changes and `refused` (G3, 140 sites; G5, G6) | engine lane | engine lane |
| `radio_stream` extras (modes, plugins, DSP load, USB errors, stalls, plugin faults) and `note` from the detectors and "this sounds wrong" (C4) | engine and plugin lanes | engine lane |
| `radio_close` causes `unplugged`, `fault`, `error`; device-dead latches of the native drivers (G1, 26 sites) as events | engine lane | engine lane |
| `firmware` and `library` for each driver; `usbHost` of a found radio; `audioClass` | engine lane | engine lane |
| The remaining PENDING groups this lane did not touch (G1, G2, G3, G8, G9, S1, S2, S4 to S9) | engine and screen lanes | gate lanes |
| Drive the patch-radio and sound-card failure sites (fake patch device, fake sound backend) | engine lane | test lane |
| Store privacy answers, release notes, the Android build's events (C5) | the owner, release lane | release lane |
| The mouse drive over the Diagnostics and Usage pages, a real HF+ failing, the `radio-reports` folder's size after a week | the mouse drive and the owner | drive |
| The measurement of bytes per session and requests per session on real traffic (decision 13, R5) | real traffic | site lane |
| `telemetry-worker/radio-reports.test.mjs:105` splits the manifest on `
` and fails on a CRLF checkout (`telemetry_worker` RED here): `.split(/?
/)` | the Worker's owner | Worker lane |
| `slint_drive_layout-section-overlap-1600` and `-624` (`Read the pitch now` bottom reads 2058, the script says 2028): the layout above CW surround's last key has grown since engine fixes 6; re-measure and move the number, or find the section that grew | the screen lane | screen lane |
| `slint_drive_system-look`: `look.width` stays at 800 after "Interface size: Auto" (intermittent since window gaps 2, red alone here) | the screen lane | screen lane |
| `radioreport::global()` is shared by every engine in a process: a test that builds a second engine in the same process reconfigures it (the engine test says so and re-points it per scenario) | none | note |
| Two copies of FoxSDR running at once share one `radio-reports` folder: the second sends the first's active segment as a leftover (the site de-duplicates by session and `seq`); a lock per session would stop it | engine lane | engine lane |

## GATE TOTALS (Release, `D:/foxsdr-build/en-tuning`, hidden desktop `FoxSDRTestsRRA`, a machine at 50 to 100 percent CPU from other lanes)

The tree was built whole once (`cmake --build . --config Release --parallel 4`, exit 0, no compiler or linker error) and the targets this lane changed were rebuilt after each
later edit. The base was built before any change as far as the engine library and 325 test targets (no error); I did not build the whole base tree to compare against.

**Headless slice** (`ctest -L headless -E "slint|telemetry_worker" -j2 --timeout 300`, 1070 tests): `99% tests passed, 14 tests failed out of 1070`, `Total Test time (real) = 4213.15 sec`.
The 14, each re-run alone:

| Test | Verdict |
|---|---|
| `test_thread_inventory` | MINE, FIXED: two new rows in `tests/thread_inventory_rows.inc` (the sender thread and the machine reader's `std::async`); passes (43.7 s) |
| `test_radio_reports_engine`, `_stepped` | MINE, FIXED: the first-session scenario needed the engine's launch count to be 1 (a hermetic engine holds 0); then the stepped variant's stream figure assumed a 60 s window exactly (a slow run made it 61): the figure is now held to the window it states. 120 checks, 0 failed |
| `test_engine_api_transmit`, `test_engine_api_wire_a_display`, `test_gui_file_jobs`, `test_plugin_unload_handshake`, `test_sentinel_beat_source`, `test_snr_tap`, `test_window_threaded_look` | LOAD (frame counts, gaps and file waits judged by wall time): each passed alone (123 s, 9.8 s, 108 s, 4.3 s, 8.4 s, 0.11 s, 5.4 s) |
| `test_engine_api_merge` | LOAD: "a run outlasted the 900 ms the rules judge time by" on dozens of cases (runs of 3 to 6 s); failed alone once at 200.8 s and PASSED alone later at 35.5 s |
| `test_pass_control`, `test_health_app_stepped` (and `test_health_app`) | the known load flakes: red alone too under this load (a pass cadence of 16.7 ms; `slow.startup.250ms=1` left in the ledger the test compares). `test_health_app` passed 252 of 252 on a quieter run earlier in the lane |
| `test_diagnostics_free` | the stamp: the binary named an earlier commit than HEAD; passes after the rebuild at the final commit (below) |

**Not in that slice:** `telemetry_worker` (the Worker's own node test, which I did not change) is RED on this checkout: `telemetry-worker/radio-reports.test.mjs:105` splits
`MANIFEST.sha256` on `\n`, and this machine checks out with `core.autocrlf=true`, so every file name carries a `\r` and the first read fails with ENOENT
(`refuse-address-in-reason.json`). The committed manifest is LF (`git ls-files --eol`: `i/lf w/crlf`). It is the Worker's test and not editable here: `.split(/\r?\n/)` fixes it.
The `slint`-named headless tests: only that one other test matched, and it passed.

**GUI gate** (`ctest -L gui -j3 --timeout 300`, `FOXSDR_FRAME_CAP_HZ=60`, 384 tests): `98% tests passed, 6 tests failed out of 384`, `Total Test time (real) = 3469.98 sec`. The 6, re-run alone:
`slint_drive_fixture-weather-decoder` (state `count.decoderLines == 4`, read 2: timing; PASSED alone), `slint_drive_tools-airband` (PASSED alone), `test_diagnostics` (the stamp, below),
`slint_drive_layout-section-overlap-1600` and `-624` (`expect "Read the pitch now" bottom <= "2028"`: it reads 2058; RED alone, and NOT MINE: this lane changed no `.slint` file and no layout
(`git diff d4f855c5 -- src/slint/ui` is empty), the two scripts date from engine fixes 6, and the layout above that key has grown since; an OPEN ROW for a screen lane), and
`slint_drive_system-look` (`state look.width == 1600 -> FAILED: it is 800`: recorded as intermittent by window gaps 2, `window-gaps-2.md`, which saw it pass 3 of 7 alone-runs with one binary; RED alone here; not mine for the same reason).

**Error-path gate:** `test_error_paths` 24 checks, 0 failed; `errpath_gate_selftest` 42 checks, 0 failed; pin 1126 to 1116.
**Lane tests (all green at the last run):** `test_radio_report` 129 checks, `test_radio_reporter` 199, `test_radio_reports_fields` 38 + the session reader, `test_radio_reports_never_sent` 7,
`test_radio_reports_engine` 120, each of the engine tests also in its `_free` and `_stepped` variants; `test_telemetry` 292, `test_health_events` 114874, `test_stall_count`, `test_exit_cause_app`.

## EXPECTATIONS CHANGED

* `tests/test_health_app.cpp`, the helper `withoutStagesOrMachine`: it set aside `reached.*` and `setup.*` (facts about THIS machine) before comparing the real binary's failure counts.
  It now also sets aside `scan_none`. Before: a run on a computer with nothing listed left only the failure counts; after the L1 repair the same run also counts `scan_none`, which is a
  machine fact of the same kind. On this PC (a usable RTL-SDR and a USRP are listed) the token never appears, so the helper's change is not exercised here: it is for a machine
  with no radio, and is UNVERIFIED there.
* `tests/test_error_paths.cpp`: `kPendingPin` 1126 to 1116 and the reporter-declaration check now also reads `core/radio_reporter.hpp` and `engine/engine.hpp` (a rename of either still fails it).
  `tests/errpath_allow.tsv`: 10 PENDING lines removed.
* `tests/thread_inventory_rows.inc`: two rows added.

## STAMP TESTS, CONFIG HASH

(see the end of this file)
