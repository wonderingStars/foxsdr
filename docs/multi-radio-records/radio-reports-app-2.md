# Wave 8, radio reports, app side, part 2: the driver-level events and the radio faults nothing reported

Branch `feat/radio-reports-app-2`, cut from `312e7b02` (part 1's tip) in `D:/foxsdr-wt/en-tuning`, build folder `D:/foxsdr-build/en-tuning`, scratch `D:/foxsdr-build/rra2-scratch/`,
hidden desktop `FoxSDRRRA2`. Written 2026-11-16. NOTHING IS PUSHED, DEPLOYED OR SENT: `kRadioReportsLiveSend` is still `false`. No radio was opened, no outside request
was made, no download, nothing on the visible desktop. The contract, the Worker and the site are NOT edited (CONTRACT ISSUES 9 to 14 below). Transmit code
(`pluto_tx.cpp`, the PlutoTx open) was not touched. The real `%APPDATA%/foxsdr/config.json` sha256 is under CONFIG HASH.

## What was built

One header, `src/core/radio_events.{hpp,cpp}`, is the whole mechanism; every driver site is ONE call to it, never a hand-written message:
`noteControlRefused` / `noteControlRefusedText` (G3), `noteDeviceFault` (G1), `noteStreamTrouble` (G2), `noteVendorCallFault` and `noteServiceFault` (G5, G8, G9, S8).
The engine registers each opened source object (`registerSource`, from `journalOpenOk`, the pass, and the patch radios) so a driver's `this` finds the radio's model and
serial; a driver call raised while its radio is still OPENING is held (16, two minutes) and said about the right radio when the engine announces it; a call that can never
be attributed is dropped and counted (`unattributedCount()`), and no radio is ever made up. Every entry point is exception-safe (`lostCount()`), because several drivers call
it from `noexcept` fault paths. Repeats inside five seconds (refusals) or ten (vendor faults) are folded and counted into the radio's own `dropped`
(`Reporter::countDropped`), so a slider dragged against a refusal is one event and a number. The reporter's stream coarsening no longer swallows a `radio_stream` with
`usbErrors` or `stalls` (the daily bound's first-and-last rule and `dropped` keep a loop from flooding).

| # | Row | Verdict | Where and the proof |
|---|---|---|---|
| 1 | A radio that fails after it opened (G1 device dead, G2 stream health) | BUILT | Every native driver's dead latch (RTL-SDR, Airspy, Airspy HF+, HackRF, Mirics, Pluto RX, RX888, AOR, RTL-TCP, SDRplay, G33/G39 DDC), SoapySDR's absorbed vendor fault and its read exceptions / consecutive-error fault, SDRplay's event log (removed, master lost) and stalled stream, patch radios; the 60 s health window with errors or a stall in nine drivers. `radio_close` gains `unplugged`, `fault`, `error` (`closeCauseFor`: the driver's own sentence and `deviceDead()`), said once per open at the place the driver detects it; the engine's own close of the same ending is not a second (`takeCloseReported`), and when only the pipeline saw it, the engine now refines `died`. Proof: `test_radio_events` (90 checks), `test_radio_reports_engine` (129): the unplug drive finds exactly one close, cause `unplugged`. Break-it: `takeCloseReported` made to return false: RED in both (3 and 1 reds); restored. |
| 2 | A setting the radio refuses (G3) | BUILT for the native drivers, SoapySDR and SoapyRemote; the rest NOT BUILT, see OPEN ROWS | One call per refusal site in 26 driver files: rate, frequency, gain, gain mode, antenna, bias tee, ppm, direct sampling, decimation, and "other" for the rest (the control from a fixed word list, never the value; frequency has no `refused` word in the contract: CONTRACT ISSUE 9). A value the user typed out of range is reported with its own class (reason starts `out of range:`, fixed text, never the number). `why` (open/user/auto/restore) from a thread scope, default `user`. Open-time refusals (the HF+ crystal trim and front-end options "carrying on without") are reported with `why` open, once the engine says which radio opened. Proof: `test_radio_events` (the four settings, the class, the scope, held-while-opening, folding). 27 G3 sites remain PENDING, each named in OPEN ROWS; 41 sites were judged and written out (ALLOW with the reason, or DETAIL-OF the driver site that reports). |
| 3 | The 14 faults the sweep found | see the table below | |
| 4 | The silent signal generator | BUILT | `Engine::applyConfig` had four restore branches (sound card, file, soapy or native WITH arguments) and no else: a saved `rtlsdr` with empty `nativeArgs` ran the generator with no error. A saved soapy or native kind (every kind `isNativeSourceKind` names, which includes KiwiSDR, raw I/Q and the wire-b radios) with no device named is now an open failure: a plain sentence in `sourceError_` (the existing English `radioNotOpenedSentence`), a log line, and a `radio_fail` (trigger `startup-restore`). The other source kinds were checked: soundcard, file and siggen have their own branches; an unknown kind string is not a kind this build can name. Proof: `test_radio_reports_engine` "a saved radio kind that names no device is an open that failed" (RED with the branch disabled, GREEN restored). |
| 5 | Vendor and service faults (G8, G9, S8, G5) | BUILT in the source layer; S8 window sites NOT BUILT | SDRplay ReleaseDevice / Uninit failures and "stopped without Uninit" (`radio_recovery` `release` / `uninit`, gave-up), the service state (`servicestate`, only while an SDRplay radio is open: with none open it is information and the call is dropped and counted, so no phantom radio is made), the RTL PLL (`pllunlocked`, gave-up, once per open as the existing counter is), SoapySDR vendor faults and module-unload faults, SoapyRemote stop. Existing counters are kept (nothing removed). The four S8 sites in `app_window_sdrplay.cpp` and `signal_publish.cpp` are WINDOW code: PENDING. |
| 6 | `firmware`, `library`, `usbHost`, `audioClass`; patch-radio and sound-card sites driven | PARTLY | `firmware`: the Airspy HF+ (the one driver that hands its firmware over) puts it on `radio_open` ok. NOT BUILT: `library` and every other driver's firmware (no accessor exists; nothing is probed to learn it), `usbHost` of a found radio (the enumeration gives no controller), `audioClass` (the audio layer knows a device by name only; a class would be a guess). The sound-card refusal sites (rate, centre) are reported by `noteControlRefused`; the sound-card and patch-radio OPEN failure sites are still held by part 1's source scan; the patch radio's new start-failure and fault sites are scanned, not driven (UNVERIFIED). |

### The sweep's 14 faults

| # | Fault | Verdict |
|---|---|---|
| 1 | a sound card's rate-change reopen refused | BUILT: `soundcard_source.cpp` `setSampleRateHz` (two sites) and `setCenterFrequencyHz` call `noteControlRefused` |
| 2 | an I/Q recording that would not open | BUILT through the recorder's existing reporter: `noteRecovered(IqFileGone)` at the user open (`engine_disk_work.cpp`) and at the patch node. The word's own comment says "the saved recording did not reopen at start-up"; using it for these two is a stretch the owner may want a word of its own for (no word added: the vocabulary is the Worker's) |
| 3 | a rate the radio refused (API and open) | BUILT at the driver (every native driver's `setSampleRateHz` refusal, SoapySDR, SoapyRemote, sound card). The three engine sites (`applyCommand`, `openDeviceSync`, the open worker) stay PENDING: reporting there too would double-report |
| 4 | a source that is gone since the last scan | BUILT: `Engine::selectSourceById`'s last line files a `radio_fail` (trigger `user-pick`) |
| 5 | the clipboard could not be opened | NOT BUILT: window code (`slint/tools_publish.cpp`), as the brief says |
| 6 | the interface half of the settings unreadable | NOT BUILT: `slint/host_duties.cpp` is window code |
| 7 | an open result dropped because the user moved on | BUILT, journal only: a SUCCEEDED open that is dropped is journalled as `radio_open` ok then `radio_close` `switch-radio` (`journalOpenSuperseded`); it is not a fault of the radio and no failure is filed. (A FAILED one was already filed by part 1 with `superseded`.) |
| 8 | the PlutoTx open | NOT BUILT: transmit code, never touched |
| 9 | plugin files that cannot be hashed / manifest | NOT BUILT: plugin/health lane, not a radio |
| 10 | a screenshot / the system menu refused | NOT BUILT: window code |
| 11 | the settings file damaged at the headless engine's start | NOT BUILT: not a radio; engine start-up lane |
| 12 | `--patch-file` unreadable, frame log not written | NOT BUILT: classic window developer options |
| 13 | a tune refusal after open | BUILT at the driver (the same `noteControlRefused` frequency sites); the engine's `noteTuneRefused` log stays PENDING for the same reason as 3 |
| 14 | the plugin strike store | NOT BUILT: plugin/health lane |

## Driver files touched (every one a minimal edit: an include, a `namespace rre = ...` alias, and one call per site)

Native: `rtlsdr_source.{cpp,hpp}`, `airspy_source.{cpp,hpp}`, `airspyhf_source.{cpp,hpp}`, `aor_source.{cpp,hpp}`, `hackrf_source.{cpp,hpp}`, `mirisdr_source.{cpp,hpp}`,
`pluto_source.{cpp,hpp}`, `rtl_tcp_source.{cpp,hpp}`, `rx888_source.{cpp,hpp}`, `sdrplay_source.{cpp,hpp}`, `sdrplay_service.cpp`, `g33ddc_source.cpp`, `g39ddc_source.cpp`.
Other: `soapy_source.cpp`, `soapy_remote_source.cpp`, `hpsdr_source.cpp`, `hpsdr1_source.cpp`, `hpsdr2_source.cpp`, `icr8600_source.cpp`, `ascp_source.cpp`, `sdriq_source.cpp`,
`extio_client.cpp`, `raw_iq_net_source.cpp`, `soundcard_source.cpp`, `fcd_control.cpp`. Engine: `engine.cpp` (the saved-kind branch, the gone-since-scan report, the superseded
success), `engine.hpp` (two declarations/fields), `engine_radio_reports.cpp`, `engine_patch_radios.cpp`, `engine_disk_work.cpp`. Core: `radio_reporter.{hpp,cpp}`.
Not touched: `pluto_tx.cpp` (transmit), any `.slint` or window file, the contract, the Worker, the site.

## Events, by kind, and the test that makes each happen

| Kind | Made by | Test |
|---|---|---|
| `radio_settings` (`refused`, `why`, class in the reason) | `noteControlRefused` | `test_radio_events` (four settings, out-of-range class, `auto` scope, held while opening, folding into `dropped`); engine drive |
| `radio_close` `unplugged` / `fault` / `error` | `noteDeviceFault`; the engine's refinement of `died` | `test_radio_events` (three causes, once per open), engine drive (one close, `unplugged`) |
| `radio_stream` with `usbErrors` / `stalls` | `noteStreamTrouble` (not coarsened) | `test_radio_events` (two in one minute both pass) |
| `radio_recovery` (`vendorcall`, `pllunlocked`, `release`, `uninit`, `servicestate`, `moduleunload`, `nonfinite`) | `noteVendorCallFault`, `noteServiceFault` | `test_radio_events` (ok and gave-up, service with and without an open radio) |
| `radio_open` ok + `radio_close` `switch-radio` (superseded success) | `journalOpenSuperseded` | source-scan only: UNVERIFIED by a drive |
| `radio_fail` (saved kind with no device; gone since the scan; patch radio that would not start) | `reportFailedOpen` | engine drive (saved kind); the other two are scanned, not driven |
| never sent | every helper handed a frequency in four forms, a path, the serial, an address, a host name | `test_radio_events`: 0 leaks, the serial only in its own field and the radio key |

## NEW ENGINE COMMANDS
None.

## SHARED FILE EDITS
Everything under "Driver files touched" except the new files; also `tests/CMakeLists.txt` (one name added to part 1's loop), `tests/test_error_paths.cpp`
(`kPendingPin`, and the declared-name check also reads `core/radio_events.hpp`), `tests/errpath_reporters.txt` (six names), `tests/errpath_allow.tsv`.

## NEW STRINGS
None translated. The one new sentence a person can read, "the saved settings name this kind of radio but not which one", goes into `sourceError_` through the existing English
`radioNotOpenedSentence`, as every driver reason and neighbouring engine sentence does (those are not `tr()`'d either). If the owner wants it in the 33 catalogues that is one
`tr()` and 33 entries; not done, because the sentence is composed beside untranslated driver text.

## THE ERROR-PATH GATE

Pin before: **1116** (part 1's). Pin after: **928** (never raised). By group, before to after: `G3-control-refused` 140 to 27, `G2-stream-health` 29 to 12,
`G1-device-dead` 26 to 2 (both `pluto_tx.cpp`, transmit), `G9-vendor-fault` 10 to 0, `G8-sdrplay-lifecycle` 5 to 0, `G5-rtl-pll` 3 to 0, `G6-route-fallback` 1 to 0,
`G7-patch-radio` 4 to 1 (the decoder node, a plugin), `S8-sdrplay-service` 5 to 4 (window code), `reports-not-in-function` 602 to 593, `unsure` 88 to 86; the others unchanged.
The six reporter names are `noteControlRefused`, `noteControlRefusedText`, `noteDeviceFault`, `noteStreamTrouble`, `noteVendorCallFault`, `noteServiceFault` (the vendor reporter is
`noteVendorCallFault` because `SoapySource` already has a member `noteVendorFault`, which would have made unrelated sites in `soapy_source.cpp` look reported: the first
run showed it). Sites judged information are ALLOW with the reporter path or reason written out (the rate coercion notes, the READ calls that found a driver busy, a
before-any-radio runtime check, a latch read by `Engine::radioReportsPass`, open-path sentences that become the `radio_fail` reason, a measurement). Sites reported one level up are
`DETAIL-OF` the driver site that reports (FUNcube transport, Pluto IIO transport, the RTL tuner and demodulator, SDRplay parameters). `test_error_paths`: 30 checks, 0 failed.

## CONTRACT ISSUES (numbered on from part 1's eight; the contract, the Worker and the site are NOT edited)

9. **`radio_settings.refused` has no `frequency` word, and nothing for the other controls the radios refuse** (HDR, notches, dither, antenna pair swap, bandwidth of a mode).
   The app says `refused: frequency: ...` in the reason only, with no `refused` field. Add `frequency` and `other` to the word list in `radio-journal-spec.js`, the contract
   3.4 table and the site.
10. **No field tells a refusal of the radio from a value the user typed out of range.** The app prefixes the reason `out of range:`. Add `refusedClass` (`radio`, `range`) to 3.4.
11. **`radio_recovery.what` words the app now sends**: `vendorcall`, `pllunlocked`, `release`, `uninit`, `servicestate`, `moduleunload`, `nonfinite` (all match the pattern). The contract
    lists examples only; the site should show them as words. `pllunlocked` is sent with phase `gave-up` (the radio keeps streaming, deaf): a phase `degraded` would say it better.
12. **A service state (the SDRplay API service) has no radio.** With an SDRplay radio open the app sends it as that radio's `radio_recovery`; with none open it sends nothing. A
    session-level kind (`session_fault`) would let the site show "service not running" for an install whose radio never opened.
13. **`dropped` now also counts folded repeats** (a refusal repeated within five seconds, a vendor fault within ten), not only events the daily bound held. The site's wording
    "kept off the wire by the daily bound" is then slightly narrow; "not sent" is true of both.
14. **`firmware` is sent on `radio_open` ok for the Airspy HF+ only**; every other driver's `firmware` and every `library` stay empty (no accessor; nothing probed).

## UNVERIFIED

* Nothing was run against a real radio, the Worker or the site. The helpers were driven by stand-ins (`test_radio_events`), by a fake radio through the real engine
  (`test_radio_reports_engine`), and the Worker's own validator (`node`) was held against every message: 0 refused.
* No real driver was driven through its own failing path: the driver call sites are held by the compiler, the error-path scan and the helper's tests, not by a fake transport per
  driver (that is one test lane per driver; OPEN ROWS). A wrong control word at one site would not be caught.
* The patch radio's start-failure and fault sites, the gone-since-scan report and the superseded-success journal are scanned, not driven.
* `IqFileGone` for a recording that would not open at the user's pick and in a patch node is a use of the word beyond its comment.
* Events raised during an open that then FAILS are never said (the open's own failure is the `radio_fail`); events held for a source that is never registered expire after two
  minutes and are counted.
* Patch radios are registered but never forgotten (no accessor for their source object); a stale entry can only make a pointer-less caller ambiguous (dropped and counted).
* Linux and macOS were not built.

## GATE TOTALS, CONFIG HASH

**Lane tests, run alone (Release, hidden desktop `FoxSDRRRA2` for the engine test), all green at the code tip `d7382478`:** `test_radio_events` 90 checks 0 failed;
`test_radio_reports_engine` 129 checks 0 failed (the Worker's validator saw 40 messages: 0 refused); `test_error_paths` 30 checks 0 failed (pin 1116 to 928). The whole tree built
(`cmake --build . --config Release --parallel 4`, exit 0, no compiler or linker error).

**THE FULL HEADLESS AND GUI GATES WERE STARTED AND HAD NOT FINISHED when this record was committed** (`ctest -C Release -L headless -E "slint|telemetry_worker" -j3` of 1073
tests, then `-L gui` of 384, through the launcher on `FoxSDRRRA2`; output in `D:/foxsdr-build/rra2-scratch/gate_headless.out` and `gate_gui.out`, a `gate.done` file appears
when both end). So: no totals, no red classification, `test_i18n` and the stamp tests after a rebuild at the final commit are all UNVERIFIED here. A reader must read those two
files (and re-run any red alone) before merging. Part 1's known reds (`telemetry_worker` on a CRLF checkout, the two `layout-section-overlap` drives, `system-look`, the
load flakes) are expected to recur and are not this lane's.

The real `%APPDATA%/foxsdr/config.json` sha256 read at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).


## OPEN ROWS FOR THE NEXT LANE

| Row | Waits for | Kind of lane |
|---|---|---|
| Turn `kRadioReportsLiveSend` to true | the Worker and site deployed; CONTRACT ISSUES 1 to 3 and 9 to 14 settled | release lane |
| The 27 G3 and 12 G2 sites left PENDING: `aaronia_source` (HTTP), `hackrf_sweep`, `kiwisdr_source`, `net_sample_source` (UDP), `spyserver_source`, `sdrconnect_source`, `iq_file_source`, `rig_cat` (CAT), `soapy_enum_proc`, `hpsdr discover`; the ten engine `applyCommand` / `openDeviceSync` / `noteTuneRefused` / `changeConverter` sites | the network and CAT sources need a registered "radio" identity first; the engine sites need one DETAIL-OF target per driver or an engine-side report that does not double | engine lane |
| `pluto_tx.cpp` (G1 2, G3 3) and the PlutoTx open (sweep 8) | the transmit owner | transmit lane |
| S8 window sites (`app_window_sdrplay.cpp`, `signal_publish.cpp`), sweep 5, 6, 10, 12 (clipboard, settings half, screenshot, developer options) | window code | screen lane |
| Sweep 9, 11, 14 (plugin hashes, settings file damaged at headless start, strike store) and the decoder node that refused to start | not radios: the health/plugin lane | plugin lane |
| `library`, other drivers' `firmware`, `usbHost`, `audioClass` | an accessor per driver (nothing may be probed); an audio device class from the audio layer | engine lane |
| A fake-transport drive of each native driver's failing path (unplug mid-read, a refused gain, a stall window) | none | test lane, one driver at a time |
| Drive the patch-radio start failure and fault, gone-since-scan and superseded-success sites | none | test lane |
| In-app wording and the privacy draft from part 1 still await the owner's approval | the owner | screen lane |
| A word of its own for "a recording would not open" instead of `IqFileGone` | the owner (the Worker's vocabulary) | Worker lane |
| `radioreport::global()` shared by every engine in a process; two copies of FoxSDR sharing one queue folder | none | note |
