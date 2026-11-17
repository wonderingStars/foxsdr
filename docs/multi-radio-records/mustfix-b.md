# Must-fix lane B: every radio failure reaches the owner (the radio rows that still reported to nobody)

Branch `feat/mustfix-b`, cut from `054b5a99` (engine/multi-radio after engine merge 8) in `D:/foxsdr-wt/en-tuning`, build folder `D:/foxsdr-build/en-tuning`, scratch
`D:/foxsdr-build/mfb-scratch/`, hidden desktop `FoxSDRMFB`. NOTHING IS PUSHED, DEPLOYED OR SENT: `kRadioReportsLiveSend` is still `false`. No radio was opened, no outside
request, no download, nothing on the visible desktop. The contract, the Worker and the site are NOT edited (CONTRACT ISSUES 15 to 17). `src/source/pluto_tx.cpp` and
anything that keys a radio were not touched. No `.slint` file was touched. The real `%APPDATA%/foxsdr/config.json` sha256 is under CONFIG HASH.

## The rows

| # | Row | Verdict | Proof |
|---|---|---|---|
| 1 | The radio groups still pinned as unreported | BUILT for 49 of the 54 radio sites; the 5 left are in the transmit file | `test_error_paths` 35 checks 0 failed, pin 459 to **410** (by group below). A refusal the driver does not say is now said by the ENGINE: `rre::RefusalWatch` / `noteRefusalIfSilentSince` count the refusals raised on the calling thread, so the engine's report is made only when the driver's own did not happen (no double). Used at the ten `applyCommand` sites, the sample-rate command and the tune. |
| 1a | The two Airspy HF+ calibration warnings (the fault that started this work) | ALREADY BUILT | `airspyhf_source.cpp` 437 and 444: `noteControlRefused(... "the receiver refused to take back its stored crystal trim; carrying on without it", why "open")` sits in the same block as the `diagWarnf`, so the gate sees them reported; they were never among the 27 PENDING. The event is held while the radio is still opening and said when the engine announces which radio opened. |
| 1b | `(void)` casts of project calls (`rx888_source.cpp:438` `(void)boot.controlOut(...)`) | JUDGED, information | The jump to the FX3 firmware makes the device leave the bus: a failure there is the expected outcome (the code's own comment, and the reference driver ignores it); what proves the upload is `awaitStreamer`, whose failure is the open's `radio_fail`. `ascp_source.cpp:338,410` are unused-result casts after the failure was already filed (`noteControlRefused` on the line above). |
| 1c | The ten MUST REPORT `diagLogf` rows, radio subset | BUILT for the driver install (both windows, `noteInstallOutcome`) and the SDRplay service restart (both windows, `noteServiceFault`); `source/sdrplay_service.cpp` "SDRplay API Service" line is reached by `noteServiceFault` through `sdrplay_source.cpp`; Pluto TX x2 are the transmit file (OPEN ROWS) | `test_mustfix_b` (installer) |
| 2 | Failures sorted as `other` (L2, RR1-L2) | BUILT | `classifyRadioOpen` now knows the USB transport's real sentences (`winusb_device.cpp` `lastErrorText`: a bulk/isochronous transfer failed, `WinUsb_ResetPipe` and `SetPipePolicy` failed, `opening the radio failed (Windows error N)` for the codes 5, 32, 121, 433, 1117, 1167, 31, all read from the SDK's `winerror.h` on this machine), the RTL-SDR driver's own set-up sentences (tuner, demodulator, I2C, PLL, endpoint), and the six drivers' "no <radio> with serial <n> is connected" which said `other` until now. `other` itself carries the scrubbed sentence (a test proves the reason survives and the path and the serial do not). The "stopped answering while ... a control transfer failed" sentence was already `control` (2026-11-15). 29 sentences, one check each, each naming where it is in the code: `test_mustfix_b`; `test_health_events` 114874 checks 0 failed. Break-it: the "stopped answering on" clause removed gave RED (`the tuner stopped answering on the I2C bus: control, wanted timeout`), restored GREEN. |
| 3 | Radios Windows cannot see (C3) | PARTLY | The first-run check's table now carries HydraSDR, AOR, Icom IC-R8600 and the Mirics ids, each from the driver's own table (`hydraSdrUsbIds`, `aorUsbIds`, `icr8600UsbIds`, `msi2500::deviceModels`), judged through the same WinUSB branch with the existing translated Zadig sentence (no new string). `radiosetup::noteFindings` (one core call from both windows) files every radio present with no usable driver as `radio_found` source `first-run-check`, with the model from the id's product name, the id `vvvv:pppp`, the binding word, once per session, no serial (see below). NOT DONE: the RX888 (its ids 04b4:00f1 and 04b4:00f3 are Cypress FX3's own, which every FX3 board shares; `tests/test_radio_setup.cpp` holds the decision "never claimed", and a weakened test is not an option), FUNcube (a HID device: Windows' class driver, no id table in the driver), Fobos, G33/G39 DDC, SDR-IQ, Pluto, Perseus (a vendor library, FTDI D2XX or a network address: none has a USB match table of its own). Proof: `test_mustfix_b` (a stand-in bus per family: wrong driver, then usable with WinUSB; the radio_found event; the second call says nothing; a planted serial in the instance id is not in the event), `test_radio_setup` 688 checks 0 failed. SEE THE FINDING under L1.1: in the new window the check is not automatic. |
| 4 | Native-to-SoapySDR fallback | BUILT | The native failure, with its sentence, is now a `radio_fail` of its own on both routes (the engine had filed only the `routefallback` recovery, which needs Usage reporting). The worker leaves what failed in a mutex-guarded list that `finishDeviceOpen` (GUI thread, where the scanned labels live, so the radio keeps ONE key) files; the saved-radio route files it in place. Proof: `test_radio_reports_engine` (both routes, with Usage reporting OFF). |
| 5 | The serial each driver reports (RR1-serial) | AUDITED, nothing to fix; two UNVERIFIED items | Every native driver's `args` is `serial=<d.serial>` where `serial` is the last segment of the USB device node's instance id (`winusb_device.cpp lastInstanceSegment`): the serial string the device itself reported; a bus-generated id (contains `&`) is dropped to empty. A device with no serial gets `index=<n>`, which `serialFromArgs` does not read: the radio's serial is empty and its key is `<model>:-`, never an index, a bus position or "0". SDRplay: the API's `SerNo`. Pluto and rtl_tcp: by address, no serial. Airspy HF+: the serial is lower-cased with the `AIRSPYHF SN:` prefix stripped (`radioKey` upper-cases it). UNVERIFIED (needs the hardware): (a) a STOCK RTL-SDR reports the serial `00000001`, which very many units share, so two of them on one install, or the site keying a radio by model and serial across installs, would merge different units; (b) the HF+ serial is stripped of its prefix on the native route and arrives as SoapyAirspyHF reports it on the Soapy route, so one radio may get two keys if the two formats differ. The hardware lane should read both from one real unit. |
| 6a | OA-28 native Lime, USRP, BladeRF source kinds | ALREADY BUILT (none exists) | `tune_control.hpp:1754` (`isNativeSourceKind`) and `wire_b_source_kinds.hpp:20` name no such kind; there is no source file for them; the Source list reaches those radios only through SoapySDR rows. `bladerf` appears only as a needed-library hint (`needed_library.cpp:227`) and as setup guidance text (`guidanceForDriver`). Nothing to hide. |
| 6b | D5.12 the `telemetryEnabled` note | BUILT (comment only) | `src/core/config.hpp` above `telemetryEnabled`: with Usage reporting off a failed radio is still reported, what off does stop is named. The approved user wording was not touched. |
| 6c | L1.1 the funnel and the first-run check in a release built from this tree | PARTLY: the funnel is in; the first-run check is Classic-automatic and new-window-on-demand | `reached.*`: `engine.cpp` (`noteReached` Launched, Window, Enumerated, Working; Streaming and Audio in `health_events.cpp`; WebSpectrum in `web_server.cpp`), engine code shared by both windows; tests `test_health_app`, `test_health_events`. `setup.*`: counted by `radiosetup::noteFindings` (`radio_setup.cpp`). FINDING: the Classic window starts the probe on every launch (`radioSetupFrame`), but the NEW window starts it only when the person opens the Radio setup page (`signal_publish.cpp` `on_source_setup`), and before this lane it never counted `setup.*` at all. It counts now, when the page is opened. Making the new window run the check at launch is a screen-lane change (OPEN ROWS). |
| 7 | LX-9 `test_engine_api_multi_radio` abort and timeout | RUN 50 TIMES, no abort and no timeout; ONE check failure, cause not proven | 50 runs alone on the hidden desktop (`D:/foxsdr-build/mfb-scratch/lx9.bat`): 49 exit 0, run 41 exit 1: `test_engine_api_multi_radio.cpp:869 fs::file_size(dir / f) > 44u` (a recording file of the two-radio take still holding only its 44-byte header when checked; 368 checks passed, 1 failed). That is a timing race between the recorder's asynchronous writer and the test's fixed 200 ms wait, or a recording that really ended empty: the failing file's name was not printed, so which one is unknown. No fix was made on a guess. |

### The pin, by group (before to after)

`G3-control-refused` 27 to 3 (the three `pluto_tx.cpp` sites), `G2-stream-health` 12 to 0, `G1-device-dead` 2 to 2 (both `pluto_tx.cpp`, transmit: never edited),
`S8-sdrplay-service` 4 to 0, `G7-patch-radio` 1 to 0, `S5-driver-install` 5 to 0, `S4-first-run-check` 2 to 0. Total **459 to 410**. How each left:
reported (the 10 `applyCommand` option and gain/antenna sites, the sample-rate command, `noteTuneRefused`, the two SDRplay service restarts, the Aaronia control, the
HackRF sweeper's stop); reported one level up (`DETAIL-OF`: the Aaronia HTTP round trips, the sweeper's pipe failure); ALLOW with the reason written out in the allow list
(the network radios' stream failures, which `NetLink::fault` latches and `Engine::radioReportsPass` files as ONE `radio_close`, cause `fault`, tested; the open-path sentences
that become the open's `radio_fail`; the converter change that asks nothing of the radio; the decoder plugin that will not start; the CAT rig link; an I/Q file read error; the
enumeration helper's closed pipe; the helper's own command-line usage text; the two user-run SDRplay diagnostic tool results; the timed-out first-run probe, which ends as
`not-checked` and is counted `setup.not-checked.none`: `test_mustfix_b` drives a probe that never finishes).

### A radio that is not one of the engine's registered sources (new helpers, `src/core/radio_events.{hpp,cpp}`)

`fileSideRadioFailure` (a `radio_fail`, sent with the Diagnostics switch on or off and Usage reporting on or off) and `noteSideRadioStopped` (a `radio_close`). Used by the HackRF
sweeper (`wire_b_sweep.cpp`: a HackRF that would not open, configure or start for a sweep; a sweep that stopped under the radio) and by `winusb::noteInstallOutcome` (a one-click
driver install that was refused, found no device, had no administrator rights or failed). A cancelled install is the person's choice and reports nothing.

## NEW ENGINE COMMANDS
None.

## DRIVER FILES TOUCHED
`src/source/aaronia_source.cpp` (one wrap, `control()`), `src/source/hackrf_sweep.cpp` none (its sites are DETAIL-OF/ALLOW in the allow list). Everything else is core or engine:
`core/radio_events.{hpp,cpp}`, `core/health_events.cpp`, `core/radio_setup.{hpp,cpp}`, `core/winusb_install.{hpp,cpp}`, `engine/engine.cpp`, `engine/wire_b_sweep.cpp`.
Window code, one call per site: `gui/app_window_radio_setup.cpp`, `gui/app_window_sdrplay.cpp`, `slint/signal_publish.cpp`.

## SHARED FILE EDITS
`src/engine/engine.cpp` (the watches, the fallback hand-off; `engine.hpp` is NOT changed, so the 197 files that include it were not recompiled), `src/slint/signal_publish.cpp`
(three lines), `tests/CMakeLists.txt` (one name), `tests/test_error_paths.cpp` (`kPendingPin`), `tests/errpath_allow.tsv`, `tests/errpath_reporters.txt` (five names:
`noteRefusalIfSilent`, `noteRefusalIfSilentSince`, `noteRefusalIf`, `fileSideRadioFailure`, `noteSideRadioStopped`), `tests/test_radio_reports_engine.cpp` (new scenarios and a
second fake, no existing expectation changed).

## EXPECTATIONS CHANGED
None. (`test_radio_setup.cpp`'s "04b4:00f1 is never claimed" first failed against my first draft, which added the RX888; the DRAFT was wrong, the test stays and the RX888 was taken out.)

## NEW STRINGS
None translated: no user-visible string was added. The report reasons are English driver-style sentences, as the neighbouring ones are.

## CONTRACT ISSUES (numbered on from part 2's fourteen; the contract, the Worker and the site are NOT edited)
15. **No class for a USB transfer that failed while a radio was being opened** (a bulk or isochronous read, a pipe reset: `winusb_device.cpp`). The app sends the nearest allowed class, `control`, plus the scrubbed sentence. A `transfer` word in the `reasonClass` list of 2.1 and 3.6 would say it.
16. **No class for "the tuner/demodulator inside an RTL-SDR would not initialise"** (PLL did not lock, I2C repeater, register write). Sent as `control` plus the sentence; a `tuner` word would separate a faulty dongle from a refused USB transfer.
17. **A radio that stopped under the HackRF sweeper has no `radio_open`** before its `radio_close` (the sweeper is not the receiver and journals no open). The app sends the `radio_close` (cause from the sentence, `openSec` 0). The site must show a close without an open rather than refuse or hide it; or a `radio_open` for sweeps would balance it.

## TESTS RUN (Release, `D:/foxsdr-build/en-tuning`, all green unless said)
`test_mustfix_b` 112 checks; `test_radio_reports_engine` 145 (hidden desktop `FoxSDRMFB`; the Worker's validator saw 56 messages, 0 refused); `test_radio_reports_never_sent` 7
checks 0 leaks over the new events (a planted-secret search of the queue and the wire; the helpers' own text scrub bypassed on purpose was still caught by the reporter's guard, so
the test stayed green: defence in depth, not a gap); `test_radio_events` 90; `test_health_events` 114874; `test_health_paths`, `test_apply_command`, `test_radio_setup` 688,
`test_radio_setup_inventory` 31, `test_radio_setup_exit`, `test_radio_setup_app` (hidden desktop, 60 s), `test_winusb_install`, `test_diagnostics` (hidden desktop), `test_radio_report`
187, `test_radio_reporter` 214, `test_errpath_sweep_a`, `errpath_gate_selftest`, `test_error_paths` 35 (pin 410), `test_i18n` 458737, `test_rtl_tcp_source` 518, `test_hydrasdr_source`
413, `test_airspy_source` 802, `test_airspyhf_source` 421, `test_saved_radio_retry` 39, `test_upload_scrub` 316. `cascade` and `foxsdr-slint` both build (the two windows). Break-its on the
real tree: the engine watch made inert, RED in `test_mustfix_b` (3 checks); the "stopped answering on" clause removed, RED (1 check); both restored.

## NOT RUN
The full headless gate and the full GUI gate (a merge agent was running the gates in another worktree; the brief said not to start them), the stamp tests after a rebuild at the final
commit, and the whole-tree rebuild (the tree was NOT rebuilt after the last `config.hpp` comment edit, which is a comment only). Linux and macOS were not built.

## CONFIG HASH
The real `%APPDATA%/foxsdr/config.json` sha256 read at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

## UNVERIFIED
* No radio was opened: the driver call sites, the sweeper and the installer were driven by stand-ins and fake radios, never against a real device. A real HackRF sweep failure and a real
  failed WinUSB install were not made to happen.
* The site's handling of a `radio_close` with no `radio_open` before it (the sweeper) is not known (CONTRACT ISSUE 17).
* The stock-serial and the HF+ two-route serial questions (row 5) need hardware.
* The in-range tune refusal is deliberately not filed by the engine (a busy driver, not a fact about the radio); a driver that refuses in range must say so itself.
* The sound card is not a registered source (`registerSource(rx().device_)` ignores a null), so its own refusals are held and then dropped; not changed here.
* The Slint window's first-run check only runs when the Radio setup page is opened; the new count and report are made then.

## OPEN ROWS FOR THE NEXT LANE

| Row | Waits for | Kind of lane |
|---|---|---|
| `pluto_tx.cpp`: G1 2 (`noteFaultOn`: `tx fault` log and the "stopped answering" error) and G3 3 (`setCenterFrequencyHz`, `setSampleRateHz`, `silenceOn`); each needs `noteDeviceFault(this,"pluto",...)` and `noteControlRefusedText(this,"pluto",...)` as `pluto_source.cpp` does | the transmit owner | transmit lane |
| Make the new window run the first-run radio check at launch, as Classic does, so `setup.*` and the first-run `radio_found` come from every install | a screen-lane change (not built: a new behaviour) | screen lane |
| RX888, FUNcube, Fobos, G33/G39 DDC, SDR-IQ, Pluto, Perseus in the first-run check | the RX888 ids are Cypress FX3's own (cannot be claimed by id); the others have no USB match table in their drivers | hardware / owner decision |
| Is a CAT rig link a radio for the owner's reports? (judged accessory, shown on `cat.link.error`) | the owner | owner decision |
| A decoder plugin that refuses to start on a patch radio | the plugin-health lane | plugin lane |
| Who reports the SDRplay API service being down when no RSP is open (contract issue 12) | the contract | Worker / contract lane |
| The `test_engine_api_multi_radio` line 869 file-size check: print the name, find whether the writer or the recording is short | none | test lane |
| Serial questions of row 5 | hardware | hardware lane |
| CONTRACT ISSUES 15 to 17 | the contract, the Worker and the site | Worker lane |
| Turn `kRadioReportsLiveSend` to true | the Worker and site deployed, CONTRACT ISSUES 1 to 3 and 9 to 17 settled | release lane |
