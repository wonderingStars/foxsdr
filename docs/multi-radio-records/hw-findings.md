# Hardware findings lane (2026-11-17): fixes for what the real B200 and RTL-SDR showed

Branch `feat/hw-findings`, cut from `860fbc65` (`engine/multi-radio`), worktree `D:/foxsdr-wt/en-tuning`, build `D:/foxsdr-build/en-tuning`,
scratch `D:/foxsdr-build/hwf-scratch/`. Source: `D:/foxsdr-build/hw2-scratch/RESULTS.md`. Nothing pushed. Receive only on both radios.
Error-path pin at the start: 410 (`kPendingPin`, tests/test_error_paths.cpp).

## Item 1: RTL-SDR analogue IF bandwidth - HIDDEN

What the setting programs (src/source/tuner_r82xx.cpp `setBandwidthHz`, the same formulas as the reference driver): for a requested width
below 2.43 MHz it writes register 0x0a (bit 4 clear), 0x0b (bits 6 and 5 = high-pass sections not added, low nibble = 15 - table index
of a low-pass corner 350..1700 kHz) and chooses an IF of 2.3 MHz - realBw/2 (+ the added high-pass sections). The code's own model is
a passband of realBw (350 kHz for 300 kHz) centred on that IF.

Measured on the real dongle (R820T, serial 00000001) with a scratch probe that opens `RtlSdrSource` directly
(`hwf-scratch/probe.cpp`, FM band, 98.0 MHz, 2.4 MS/s, 4.8 MS recorded per run, counts and levels only, 10th-percentile envelope per 100 kHz of a
4096-point averaged spectrum, `an.py`):

| setting | envelope (dB) at -1100 / -800 / -500 / -400 / -300 / 0 / +1100 kHz |
|---|---|
| automatic (IF 1.815 MHz) | 15.8 / 20.4 / 21.2 / 21.2 / 21.7 / 25.0 / 18.1 |
| 300 kHz (IF 2.125 MHz) | 5.2 / -6.2 / 4.9 / 14.0 / 18.8 / 23.8 / 16.0 |
| 1000 kHz (IF 1.7 MHz) | -5.7 / 14.1 / 21.2 / 20.9 / 21.7 / 25.0 / 16.8 |

Reproduces the check's finding: 300 kHz is one-sided (lower side falls about 26 dB below -450 kHz, upper side untouched to +1.15 MHz).

Why (shown, not assumed):
1. Held at an EQUAL IF (the IF forced to 2.125 MHz with a scratch-only override), the settings 300, 500, 700 and 1000 kHz gave the SAME
   envelope within 1.5 dB at every 100 kHz step (e500/e700/e1000 against 300 kHz: -500 kHz 5.7/5.5/5.2 against 4.9, -400 kHz 14.6/14.5/14.3
   against 14.0). The low-pass nibble that is supposed to set the width has no measurable effect.
2. What moves the edge is the IF position only: with the 300 kHz registers and the IF shifted by -800, -400, +400, +800 kHz the edge moved
   by about the same amount (edge at +0.9 MHz, -0.85, -0.05, +0.3 MHz against -0.43 at no shift). Mapped in absolute IF terms the passband
   is about 0.45 to 2.55 MHz (2.1 MHz wide, centred near 1.5 MHz), whatever width was typed.
3. The register writes do arrive (readback of the status registers showed the tuner answering; the shadow showed 0x8f for automatic,
   0xe6 for 300 kHz, 0xeb for 1000 kHz). The set-up the driver performs at open (src/source/tuner_r82xx.cpp `setTvStandard`) switches on the
   channel-filter extension (register 0x1e bits 6:5) and sets the widest extension (0x0f bit 7 clear), which is the reference driver's
   television configuration and is what keeps the 2.4 MS/s window inside the filter; behind it the width nibble does nothing measurable.

So the IF is not "not centred": no IF placement makes a 300 kHz-wide passband, because no width is applied. It cannot be made true by a
small correct change (it would need the channel-filter extension off, which changes the whole tuner configuration of every sample
rate, and would have to be calibrated on R820T, R828D and the Blog V4 separately). The control is therefore HIDDEN for this release:
`foxsdr::slints5b::ifBandwidthControlOffered()` (src/slint/s5b_logic.hpp) is `false`, `S5Rows::buildS5b` draws nothing for it, and
`test_slint_s5b_host` holds that no `b.ifbw*` row exists whatever the engine's `ifBandwidth` fact says. "Automatic" is exactly as before
(the driver, the engine option `if_bandwidth` and `tests/test_rtlsdr_ifbw` are untouched: 50 checks, 0 failed). The shared label
"Bandwidth (kHz)" is now carried by the driver bandwidth row (SoapySDR) and the tune row only; they stay (a rename would add a key to all
33 catalogues for a label that is not wrong on its own).
Break-it: `ifBandwidthControlOffered()` set to `true` -> `test_slint_s5b_host` 84 checks, 9 failed; restored, 0 failed.
What a true version needs: the filter extension and the width nibble characterised per tuner by a spectrum sweep like the one above,
then a table of (register set, IF) pairs verified to give a passband of the stated width centred on the tuned frequency.

## Item 2: automatic ppm - HIDDEN

How it measures (src/core/ppm_auto.hpp, src/engine/engine_mpx.cpp): the FM stereo pilot PLL's residual frequency is read every 100 ms while
the pilot is locked and its level is at least 0.5; ppm = -mean(df) / 19 kHz x 1e6. The pilot is therefore taken as exactly 19 kHz.
Tolerance: the header says "the standard allows the pilot +-2 Hz (105 ppm)" and cites NO standard by name; the figure matches ITU-R
BS.450 as remembered, but that is not checked here (no outside request is allowed in this lane): UNVERIFIED against the document. The
reported uncertainty is the statistical one only (scatter of the readings), which cannot see a station whose pilot is off.
Real measurements (hardware check, `D:/foxsdr-build/hw2-scratch/RESULTS.md` section 3, 30 s, 256 readings): station A, dongle +18.91 +-0.81,
+17.06 +-0.78, +15.97 +-0.84 ppm in three runs (spread 2.9 ppm, three times the stated uncertainty); the B200 on the same station +17.08 +-0.15;
station B, dongle -3.91 +-6.5 and B200 -2.39 +-0.66. The two radios agree with each other on each station (and carriers compared directly
the day before agreed within 0.8 ppm) but the number changes by about 20 ppm from station to station: it follows the station's pilot,
not the radio. Nothing in the code or the measurements shows it is right as an absolute correction, so applying it ("Use this
correction") would mis-tune the user.
Done: the Automatic ppm panel is not drawn (`foxsdr::slints5::ppmAutoControlOffered()` is `false`, src/slint/s5_logic.hpp; `S5Rows::build`
skips `buildPpm`). The manual ppm setting is untouched. The engine command `FOXAPP_OP_PPM_MEASURE`, its facts and `test_engine_api_ppmauto`
stay. Tests: `test_slint_s5_host` new block (no `m.*` row, no "Automatic ppm" / "Measure the crystal error" / "Use this correction" label
whatever the engine's facts say, a stale press sends nothing); the old row-logic block is kept and now runs with the seam
`S5Rows::setPpmAutoOffered(true)`; `tests/test_engine_api_s5.cpp` testC sets the same seam; drive script
`src/slint/tests/drive/s5-sources.txt` no longer clicks the key. Break-it: rule set `true` -> `test_slint_s5_host` 105 checks, 5 failed; restored, 0 failed.
What a sound method needs: a reference that is not a broadcast station - a frequency standard (GPS-disciplined source, or a carrier whose
tolerance is known to be a fraction of a ppm), or an average over many stations with a stated rejection of outliers and a result that
carries the SPREAD between stations rather than only the PLL's scatter; the test must include two stations with different pilot errors.
EXPECTATION CHANGED (listed in the final report): `src/slint/tests/test_slint_s5_host.cpp` line 151 `r.find(v, "m.start") != nullptr` is now `== nullptr` (the panel is not drawn).

## Item 3: a saved sample rate the radio cannot do - FIXED

Cause: `RtlSdrSource::setSampleRateHz` coerces to the nearest rate it has and returns true. The engine already built the panel's line
"Sample rate coerced: the radio runs at 3.2 MS/s, not the 5 MS/s asked" (visible in the window as a Note: confirmed in a hidden-desktop
run on the real dongle, so the line was NOT absent) but it did not say WHY, and nothing queued the refused-setting event.
Fix (src/core/rate_follow_status.hpp, `applySourceRate`): the line is now "Sample rate coerced: this radio cannot run at 5 MS/s, so it runs at
3.2 MS/s" (which rate and why, one sentence), and a rate that landed elsewhere queues the same `radio_settings` refusal a typed out-of-range
value does (control `rate`, `refusedClass` `range`, reason "the radio cannot run at the sample rate asked; it runs at the nearest rate it can",
no number), unless the driver already reported one on this thread (the RefusalWatch rule). New optional argument `why` ("open" at the
worker open and the patch radios, "restore" at the startup restore; a user's pick keeps the thread default "user").
Test (no hardware): `tests/test_rtlsdr_rate_report.cpp`, a driver on `usb::FakeUsbDevice`: 18 checks, 0 failed (the line, one event, class,
why restore / open, no digit in the reason, a rate it can do queues nothing and clears an old coercion line, a repeat is folded).
Break-it: the event emission switched off -> 13 checks, 2 failed; restored. `test_rate_coercion` (the older contract of the line) 45 checks, 0 failed.
Real dongle (scratch engine, saved 5 MS/s, usage reporting on in the scratch config, all URLs dead): status `sampleRateHz 3200000`, `sourceError`
"Sample rate coerced: this radio cannot run at 5 MS/s, so it runs at 3.2 MS/s"; queue: one `radio_settings` refused `rate`, `refusedClass` `range`,
`why` `restore`, no band, no frequency, no path. Before the fix: no such event (the check's result).
NOTE (not changed): `ControlWhyScope` is never set by the engine; `why` was always "user" for a refusal raised at open. Only this path passes it.

## Item 4: the generator standing in for a radio that did not open - FIXED

Cause: a saved radio kind that names no device (engine.cpp restore) set the error line and filed a failure, but the receiver then ran on the
generator and the window showed RUNNING, FAIL lamp dark; and that failure's class was `other` (the sentence "the saved settings name this kind of
radio but not which one" matched no rule).
Fix, smallest edit, no `.slint` change:
* `core/source_fallback.hpp`: `kStandInTail`, `standInSentence()`, `generatorStandsIn(kind, error)` - true for exactly the sentences
  `radioNotOpenedSentence` builds (every failed open that leaves the generator running uses it: engine.cpp x3, engine_radio_saved.cpp x2).
* engine fact `source.standin` ("1"/"0", engine_api_lists.cpp) = generator live AND that sentence on the error line.
* the window (`slint/main.cpp`, read four times a second): the existing FAIL lamp lights (`set_failed(faulted || stand-in)`), and the RECEIVER chip
  (`host_logic.cpp statusCards`) reads GENERATOR (existing catalogue key, no new string) with the error tone and a red lamp, its one-line detail and
  its expanded lines carrying the engine's own sentence ("rtlsdr did not open: ... The receiver is running on the signal generator.").
* `core/health_events.cpp`: "but not which one" is class `absent` (nearest the contract has; there is no "not configured" word).
Tests: `test_slint_host_logic` (205 checks, 0 failed: GENERATOR, error tone, red lamp, sentence in detail and lines, unchanged without stand-in,
stopped stays STOPPED); `test_source_fallback` (55, 0 failed: the stand-in test accepts exactly the built sentences); `test_health_events`
(114876, 0 failed: the sentence is `absent` for rtlsdr and soapy); `test_radio_reports_engine` (147, 0 failed: the queued failure's
`reasonClass` is `absent`).
Real window (hidden desktop, scratch config, `sourceKind rtlsdr` with empty arguments, real dongle present): `state fact.source.standin == 1` ok;
the accessible chip is "RECEIVER GENERATOR, rtlsdr did not open: the saved settings name this kind of radio but not which one. The receiver is
running on the signal generator."; the screenshot shows the FAIL lamp red and the chip with a red lamp; the queued `radio_fail` has
`reasonClass` `absent` (it was `other`). NOT break-it checked: the chip row (the unchanged-without-stand-in check is the guard).

## Item 5: the B200 reported as model "uhd" - FIXED

What the device says (python SoapySDR on the real B200, enumeration plus one open, receive-only): the enumerated row is `driver=uhd, label=B200
EDR04ZDB2, name=, product=B200, serial=EDR04ZDB2, type=b200`; `getHardwareKey()` "B200"; `getHardwareInfo()` `mboard_id` "B200", `mboard_serial`
"EDR04ZDB2"; `getDriverKey()` "b200". The app names its open source "SoapySDR: B200" (the hardware key).
Cause: `identify("soapy", args)` used the usage record's `sanitiseDevice(args)`, which is only as good as the arguments: a saved row that carries
`product=` gave "uhd b200" (the driver key and the type, lower case); a hand-written `driver=uhd,serial=...` (the check's seed) gave "uhd". The owner's own saved
row has `product=` and `label=` (checked by key presence only), so his real events were "uhd b200", not "uhd".
Fix (`core/radio_report.{hpp,cpp}`): the model of a SoapySDR radio is the row's `product` ("B200"; the RTL module's product string), else a model learned
for that (driver, serial) by `rememberSoapyModel` (the engine remembers the opened device's hardware key in `adoptDeviceMirrors` and every scanned row's own
model), else the old text. A label-only row ("Generic RTL2832U OEM :: 00000001", "HackRF One #0 <serial>", "LimeSDR Mini [USB 3.0] <serial>") gives its label without
serial, `#n`, `::`, bracketed parts and serial-shaped hex tokens. The vendor driver key is in its own field, `library` ("SoapySDR uhd": the contract's optional
field, regex `^[A-Za-z0-9._+ -]{1,48}$`), carried on every radio_fail and every journal event of a SoapySDR radio; the serial is untouched; the key is now
`b200:EDR04ZDB2`. The contract is not changed (the model field and `library` already exist).
Tests: `test_radio_report` (215 checks, 0 failed: B200, the minimal row before and after learning, a row with no serial remembers nothing, the RTL, HackRF, Lime and
Pluto forms, no address or serial in model or library); new `tests/test_soapy_model_report.cpp` (11 checks, 0 failed, the queued radio_fail and radio_settings bodies, a native
radio has no `library`). Break-it: identify using the old text again -> 5 failed; restored. `test_radio_reports_fields` 48, `_never_sent` 7 (0 leaks), `_engine` 147: 0 failed.
Real B200 (scratch engine, usage reporting on, URLs dead): with the owner-style full row the queue has radio_open (twice), radio_settings and radio_close all with model
`B200`, serial `EDR04ZDB2`, key `b200:EDR04ZDB2`, `library` "SoapySDR uhd" (4 events), no frequency, path or address. With the check's hand-written minimal row the first
radio_open (before the open could say) is still `uhd:EDR04ZDB2` and the rest `b200:EDR04ZDB2`: a residual for hand-written rows only (OPEN ROWS).
Other SoapySDR-opened kinds: checked with the stand-in forms of their modules (above), not with hardware. The RTL-SDR opened natively keeps its model from its bus label.

## Item 7: the rtl_tcp server's greeting - FIXED (for the native RTL-SDR and for a radio that itself listens to an rtl_tcp server; "unknown" for the rest)

Cause: `RtlTcpServer` took its tuner type and gain count from `RtlTcpConfig` at start (0 and 0) and the engine never set them, so every client was told
"unknown tuner, no gain list" (the hardware check read `RTL0`, 0, 0 from a served real dongle).
Fix: `RtlTcpServer::setTunerInfo(type, gains)` (atomics read when a client is accepted; the next client gets the new numbers); `Hooks::tunerInfo` and
`ServerSet::pass` refresh it at most twice a second while no client is connected; the engine's hook (`engine_wire_c.cpp`) answers from the served radio:
`RtlSdrSource::rtlTcpTunerCode()` (5 R820T, 6 R828D, 0 when not open) and `tunerGainSteps()` (29), or, for a radio that is itself an rtl_tcp client, what its
server told it; anything else (Soapy, a generator, a file) is the documented "unknown", 0 and 0. The header layout is unchanged: "RTL0", then two big-endian
32-bit numbers, the layout `RtlTcpSource` (this tree's client) reads (`src/source/rtl_tcp_source.cpp`, bytes 4 to 7 the tuner type and 8 to 11 the gain count).
The gain count is 29, not the driver's own aggregate ladder (30 rungs, it counts its own starting point): `rtl_test -t` on the real R820T lists 29 gain values, which is
what a client that sizes its gain list from this number expects (and FoxSDR's server ignores every gain command anyway).
Tests: `test_rtl_tcp_server` (67 checks, 0 failed: unknown stays 0/0, a byte-by-byte stand-in client reads 5 and 29, and the tree's own `RtlTcpSource` reads
tuner R828D and 29 from the same server and 0/0 when never told); `test_rtlsdr_rate_report` (23, 0 failed: the fake R820T is code 5 and 29 steps, 0/0 closed);
`test_engine_api_s4b_rtltcp` (40, 0 failed). Break-it: the accept path using the config's numbers again -> 2 failed; restored.
Real dongle (the window on the hidden desktop serving the real RTL-SDR at 127.0.0.1:18461, a python client): header `RTL0`, tuner 5, gain count 29, then 16.4 MB in 4 s
(2.05 MS/s of 8-bit I/Q). Before the fix: 0 and 0 (the check). Both devices on the bus afterwards.

## Item 6: the crash at start with the B200 present (libusb-1.0.dll+0x10490, signature 28F76B50279E4BB6) - NOT REPRODUCED; one real hole closed; the open path stays exposed

1. Was the helper beside the window in the check's copy? YES. The helper is `cascade.exe` beside the running executable (`enumerateHelperPath`, src/source/soapy_enum_proc.cpp);
   `D:/foxsdr-build/hw2-scratch/app/slint/` held `cascade.exe` and so did `app/engine/`. The check's crash file was overwritten by its own repeat run (the scratch
   folder is recreated per tag), so WHERE it died cannot be read back. What can be shown from the code and logs: a start with a saved B200 spawns NO helper at all. In all 41
   saved-B200 starts below the log has zero "enumeration child" lines and no `cascade-probe` lines (also with Usage reporting on, which is the check's setting): the radio is opened
   in the window's own process by `SoapySDR::Device::make` (src/source/soapy_source.cpp, a `guardedVendorCall` that the header of soapy_enum_proc.hpp itself says cannot catch a fault on a
   thread UHD spawned), and that call runs UHD's device discovery. The helper only ever serves the Source list's scan (`FOXAPP_OP_SCAN_DEVICES*`). So a crash "during device discovery,
   before any radio was open, uptime 0 s" in a saved-B200 start was, by construction, in the WINDOW's own process, not in the helper. UNPROVEN but the only reading the code allows.
2. Reproduction, complete copy (helper beside), real B200 attached, receive only, hidden desktop, scratch config each time, both devices on the bus after:
   * 40 window starts with the B200 as the saved source (`soak01..soak40`, ~15 s each): 40 exit 0, 0 crash files, 0 helper lines.
   * 8 pairs (an RTL-SDR window start, then at once a B200 window start: the order of the check's crash): 8 and 8 exit 0, 0 crash files.
   * 1 more with Usage reporting on (the journal's radio_found path): exit 0.
   * 40 runs of the helper program itself (`cascade.exe --enumerate-json`, which probes every Soapy driver including uhd with the B200 present): 40 exit 0.
   Total 49 B200 window starts and 40 helper runs, 0 crashes. The check saw 1 in about 10. NOT reproduced; no claim that it cannot happen (the header of soapy_enum_proc.hpp measures this
   machine at about one kill per twenty enumerations, and the check saw one in ten starts).
3. Release-risk statement (OPEN ROWS): the in-process `Device::make` discovery at every B200 start and every B200 open is not routed through the helper and cannot be without opening the radio in a
   separate process (a new design, not a small change); if the owner's mouse drive sees this crash in the window it is a release blocker with no fix in this lane. The crash-capture and the
   restart-after-crash behaviour of the window are the only containment today.
4. FIXED (the part the brief names): an INCOMPLETE install no longer walks the bus in the window. `enumerateIsolated` used to answer a missing helper (`SpawnFailed`) with the in-process walk
   ("not crash-isolated"; `allowInProcessFallback` defaulted to true); now a helper path that names no file sets `EnumResult::helperMissing`, runs nothing in this process, logs the reason and
   `noteRecovered(ScanLimit)`, and the engine says it plainly on the Source error line (when nothing else is there) and as fact `source.searchProblem`: "FoxSDR's radio search program (cascade.exe) is missing
   from this install, so no radio search was made. Reinstall FoxSDR." (file name only, never a path). A helper that exists but is refused by Windows keeps the old fallback (unchanged).
   Test (no hardware): `test_soapy_enum_proc` (ctest, 563 checks, 0 failed; run through ctest because the fault-fixture cases need the fixture directories ctest passes): default options with a
   missing helper -> `helperMissing`, no fall-back, no devices, sentence names the file and not the path; an existing helper is not "missing". Break-it: the missing-helper branch disabled -> 3 failed; restored.
   Real engine, incomplete copy (no `cascade.exe` beside `foxsdr-engine.exe`), scan requested through the web control: the status line carried that sentence and the log said the walk was not run in
   the process; the complete copy under the same request: empty error line, scan started normally.

## Expectations changed (both values)
| Test | Was | Now | Why |
|---|---|---|---|
| `src/slint/tests/test_slint_s5_host.cpp` line 151 | `r.find(v, "m.start") != nullptr` | `== nullptr` | the automatic ppm panel is not drawn (item 2) |
| `src/slint/tests/drive/s5-sources.txt` | clicked "Measure the crystal error" and read the note | no click; reads that the engine's correction stays off | same |
| `tests/test_health_paths.cpp` ScanLimit call sites in `soapy_enum_proc.cpp` | 4 | 5 | the missing-helper answer counts like the fallback (item 6) |
| `tests/errpath_allow.tsv` | two lines keyed by the old text of the two `applySourceRate` call sites | the same verdicts under the new keys; one new ALLOW (`pollSourceAsync`, the display of the missing-helper failure) | items 3 and 6; the pin is unchanged |
| `src/slint/tests/test_slint_s5b_host.cpp` IF-bandwidth rows block | drove the row (set, auto, refusals) | asserts none of its rows exist | item 1 |
Error-path pin (`kPendingPin`): 410 before, 410 after (nothing added to or taken from the pending sites).

## Tests run (all Release, `D:/foxsdr-build/en-tuning`)
`test_slint_s5b_host` 84, `test_slint_s5_host` 105, `test_engine_api_s5` 70, `test_rtlsdr_ifbw` 50, `test_rate_coercion` 45, `test_rtlsdr_rate_report` 23, `test_slint_host_logic` 205,
`test_source_fallback` 55, `test_health_events` 114876, `test_radio_reports_engine` 147, `test_radio_report` 215, `test_soapy_model_report` 11, `test_radio_reports_fields` 48, `..._never_sent` 7,
`test_telemetry` 292, `test_rtl_tcp_server` 67, `test_engine_api_s4b_rtltcp` 40, `test_soapy_enum_proc` 563 (ctest), `test_health_paths`, `test_i18n`, `test_error_paths` 35, plus the ctest subset
`test_slint_s5|test_slint_host|test_engine_api_s5|test_engine_api_s4b|test_rtl|test_rate_|test_radio_report|test_health|test_source_fallback|test_soapy|test_telemetry|test_i18n|test_error_paths`
(91 tests; after rebuilding the stale binaries every one passed, `test_health_paths` after its one expectation above) and, on the hidden desktop, `gui_desktop_guard`, `test_health_app`,
`test_health_app_stepped` and `slint_drive_s5-sources`: 5 of 5 passed.
NOT RUN: the full headless gate, the full GUI gate, the Slint drive scripts other than `s5-sources`, the stamp tests, the other `slint_drive_*` scripts (a hidden S5b row is not driven by any), a Linux build.
New strings: none (the chip uses the existing key GENERATOR; the engine's own sentences are not translated).

## OPEN ROWS
| Row | Kind of lane |
|---|---|
| Item 6: the in-process `SoapySDR::Device::make` discovery at every B200 start and open is not routed through the helper and the crash was not reproduced (49 starts, 0 crashes): a decision on opening Soapy radios out of process, or on accepting the risk; watch for it in the mouse drive | design + engine lane; owner decision |
| Item 1: a true IF-bandwidth control needs the tuner's channel-filter extension off and a per-tuner (R820T, R828D, Blog V4) calibrated (register, IF) table measured like `hw-findings.md` item 1 | driver lane with real radios |
| Item 2: a sound automatic ppm needs a frequency standard or a multi-station method with its spread shown (item 2 text) | design lane |
| Item 5: a hand-written Soapy row with no `product` is named "uhd" in its first radio_open until a scan or the open says the model | small engine lane |
| Item 5: the other SoapySDR-opened kinds were checked with stand-in argument forms only, not with hardware | hardware lane |
| Item 7: the gain count for tuners other than R820T/R828D and for a Soapy-opened RTL-SDR is "unknown" (0, 0) | small driver lane |
| Item 4: the receiver chip shows GENERATOR with the engine's sentence; whether the owner wants a different word is his to say at the mouse drive | owner |
| `ControlWhyScope` is never set by the engine, so the `why` of a refusal raised at open is "user" except where this lane passes it (rate) | small engine lane |
| Item 3: a saved rate coerced by a driver other than the RTL-SDR is now reported by the same call but was not driven on hardware | hardware lane |
| Items 3, 4, 5, 7 were confirmed on the real radios; items 1 and 2 are hidden, not fixed | |

## UNVERIFIED
* ITU-R BS.450's pilot tolerance (the code states +-2 Hz and cites nothing; not checked against the document: no outside request in this lane).
* Where the check's crash happened (its file was overwritten); my reading from the code (the window's own open) is not proved.
* A second dongle model, an R828D or a Blog V4, with the rtl_tcp header or the IF filter.
* The Worker's validator against a queued `library` field on a real radio_fail (the contract allows it; the engine suite's validator saw 56 messages, 0 refused, before item 5; a SoapySDR fail was not in that set).
