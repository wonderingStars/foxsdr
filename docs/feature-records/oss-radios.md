# Radio families from open-source drivers and the makers' own documents (lane `ossradios`, branch `feat/oss-radios`)

Date 2026-11-12. Worktree `D:/foxsdr-wt/en-ossradios`, branch `feat/oss-radios` cut from `engine/multi-radio` at ba2a4679, build `D:/foxsdr-build/en-ossradios` (Release, `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). Nothing pushed or merged. No radio was opened; no software, library, driver or firmware was downloaded (documents and individual source files were read over HTTPS into a scratch folder outside the repository; permissive source was read, never built or run). No outside server was contacted by any test (loopback only). `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged; read only to hash it).

Versions: no versioned file was changed (no plugin, no `kEngineVersion`-style constant is touched).

## Step 0: what the base already had, and what each item became

Base = `engine/multi-radio` ba2a4679, which already contains the earlier lanes `feat/radio-families` (ASCP/NetSDR/SDR-IP/Afedri `AscpSource`, `Hpsdr1Source`, `AaroniaSource`), `feat/hpsdrduo` (HPSDR frame layer, `Hpsdr2Source`, the RSPduo rate rules `sdrplay_dual_tuner.*`) and `feat/sweepsrc` (`SweepLine`, stitcher, tinySA). The earlier lane's record `docs/feature-records/radio-families.md` listed ten families as not built; the table says what this lane did about each of YOUR six items.

| # | Item | In the base | Source ported from, licence file and header read | Result |
|---|---|---|---|---|
| 1 | RSPduo dual-tuner rate and decimation table | `checkDuoRate` accepted decimation 1 only in dual/master modes; its "output rates" were the ADC rates 6 and 8 MHz | pothosware/SoapySDRPlay3 `Settings.cpp`: LICENSE.txt MIT "Copyright (c) 2015 Charles J. Cliffe", file header MIT "Copyright (c) 2020 Franco Venturi"; SDRplay API specification v3.15 | BUILT (new files): output-rate map with decimation 32/16/8/4/2/1, IF, bandwidth, validation |
| 2a | NetSDR / SDR-IP (network) | BUILT by the earlier lane from CuteSDR + Afedri documents (RFspace's specification then thought "on request only") | RFspace/MoeTronix interface specifications, published at moetronix.com (see Sources) | CHECKED against the maker's specifications: 65617 literal checks; no behaviour change was needed |
| 2b | SDR-IQ (USB through FTDI) | not built | SDR-IQ Interface Specification 1.04 (moetronix.com) | BUILT up to the FTDI call (owner decision): protocol layer, source over an abstract byte link, stand-in radio; the D2XX link is the hand-off |
| 3 | Icom IC-R8600 I/Q over USB | not built | ibelinp/ic-r8600-usb-iq (LICENSE MIT "Copyright (c) 2026 Pieter Ibelings", with a note that Icom's firmware is not covered) and dmitryelj/SoapyICR8600 (LICENSE MIT "Copyright (c) 2018 D. Eliuseev"; no file header names another origin) | BUILT on the existing WinUSB transport (no libusb); firmware is supplied by the user, never shipped or fetched |
| 4 | FUNcube Dongle Pro and Pro+ | not built | pothosware/SoapyFCDPP (LICENSE_1_0.txt BSL-1.0; `SoapyFCDPP.cpp` carries the SPDX line; `fcd.c`, `fcd.h` "Albin Stigo" headers; `fcdcmd.h` no header) | BUILT: HID control + the dongle's sound input; provenance caveat below |
| 5 | HackRF sweep mode | the receiver driver says in its header that sweep is not built; the sweepsrc record called it blocked | greatscottgadgets/hackrf `host/libhackrf/src/hackrf.c` and `hackrf.h`: the header of EACH file is BSD 3-clause; the repository is GPL-2.0 as a whole and its tools/firmware were NOT opened | BUILT, LINEAR style only |
| 6 | HPSDR protocol 2 discovery and tables, Metis UDP layer, HL2 extensions | protocol 2 (`hpsdr2_protocol.*`: discovery, Appendix A tables for boards 1 to 4), the Metis envelope (`hpsdr_protocol.*`) and the protocol 1 frame layer were BUILT by feat/hpsdrduo; HL2 extensions were not | openHPSDR Ethernet Protocol v4.4 and Metis V1.33 (already used); Hermes-Lite 2 wiki page "Protocol" as FACTS ONLY (no licence; gateware GPL; nothing of the repository used) | protocol 2 / Metis RE-CHECKED against the document (no change needed); HL2 extensions BUILT as `profile=hl2` of `Hpsdr1Source` |

Not mine and not touched: Orion Mk II, Saturn and Atlas tables (their only sources are GPL: a helper lane).

## What was built (files)

All new files carry a header comment naming the sources, licences and what is inferred. `src/source/THIRD-PARTY-NOTICES.md` (new) holds, per project: URL, the commit read, the files read, and the licence text in full.

| Item | New files | Edited files |
|---|---|---|
| 1 | `src/source/sdrplay_rate_map.{hpp,cpp}`, `tests/test_sdrplay_rate_map.cpp` | none |
| 2a | `tests/test_ascp_maker_spec.cpp` | none |
| 2b | `src/source/sdriq_protocol.hpp`, `sdriq_source.{hpp,cpp}`, `tests/test_sdriq_source.cpp` | none |
| 3 | `src/source/icr8600_protocol.{hpp,cpp}`, `icr8600_source.{hpp,cpp}`, `tests/test_icr8600_source.cpp` | `src/usb/usb_device.hpp` (a non-pure `UsbDevice::readBulkOnce`, default refuses), `src/usb/winusb_device.cpp` (its WinUSB implementation mirroring `writeBulk`) |
| 4 | `src/source/fcd_protocol.{hpp,cpp}`, `fcd_control.{hpp,cpp}`, `fcd_hid_win.cpp`, `tests/test_funcube.cpp` | none |
| 5 | `src/source/hackrf_sweep.{hpp,cpp}`, `tests/test_hackrf_sweep.cpp` | none |
| 6 | `src/source/hl2_protocol.hpp`, `tests/test_hl2.cpp` | `hpsdr_protocol.{hpp,cpp}` (DiscoveryReply keeps the raw datagram), `hpsdr1_frames.hpp` (CommandState `lnaDirect`/`lnaCode`), `hpsdr1_source.{hpp,cpp}` (profile `HermesLite2`) |

No `engine.cpp`, `pipeline.cpp`, `fox_engine_api.cpp` or `.slint` file was touched. No new thread kind: `Sdriq`, `Icr8600` and the HL2 profile use `NetSampleSource`'s reader; `HackRfSweeper` has no thread (the owner polls); `FuncubeSource` adds none.

## Item 1: RSPduo output-rate map

`resolveDuoRate(mode, masterFsHz, outputHz)` returns ADC rate, IF type, IF bandwidth, decimation factor, `decimation.enable` and `wideBandSignal`; `validateDuoSetting` checks a complete stored setting; `duoOutputRatesHz` is the rate menu.
* LOW-IF path (dual tuner, master/slave, and single tuner up to 2 MS/s): ADC 6 MHz / IF 1.620 MHz, or (master) 8 MHz / IF 2.048 MHz; the API's down-conversion delivers 2 MS/s, decimation 1, 2, 4, 8, 16, 32 gives 2 M, 1 M, 500 k, 250 k, 125 k, 62.5 k.
* Single tuner zero IF: 96 / 192 / 384 / 768 kS/s are ADC 3.072 MS/s with decimation 32 / 16 / 8 / 4; above 2 MS/s up to 10.66 MS/s the ADC rate is the output rate, no decimation.
* The IF bandwidth follows the output rate (< 300 k: 200 kHz, < 600 k: 300 kHz, < 1.536 M: 600 kHz, < 5 M: 1.536 MHz, < 6 M: 5, < 7 M: 6, < 8 M: 7, else 8 MHz).
* **Two findings.** (a) The earlier lane's "output rates 6 and 8 MHz" of the two-tuner modes (`documentedLowIfOutputRatesHz`) are ADC rates; the stream after LIF down-conversion is 2 MS/s (SoapySDRPlay3 `getSampleRate`). (b) At 8 MHz SoapySDRPlay3 sets the same narrow per-rate bandwidths as at 6 MHz, which the API specification's low-IF list does not allow (8 MHz only with 1.536 MHz, or 5 MHz for single tuner); the map uses 1.536 MHz at 8 MHz and keeps SoapySDRPlay3's choice in `soapyBw`. The older `sdrplay_dual_tuner.*` is untouched; the new map is the authority for the rate menu.
* The SDRplay API stays the user-installed library; nothing here loads it.

## Item 2: RFspace

**2a. NetSDR / SDR-IP.** The maker's specifications ARE published (https://moetronix.com/svdownload.htm: SDR-14 1.02, SDR-IQ 1.04, SDR-IP 1.03, NetSDR 1.05). `test_ascp_maker_spec` pins the existing `ascp_protocol.hpp` to the byte strings printed in them: requests, the five-step startup example of NetSDR 5.1, the 14.010 MHz example, RF gain bytes 00/F6/EC/E2, the small-packet item, the printed responses fed byte by byte (name, two-band range response with 15 bytes per band, NAK, stop), the four UDP headers/sizes (A4 85 / 84 81 / 04 84 / 04 82), the sequence rule with the wrap, and the rate grid (NetSDR 80 MHz/4k, SDR-IP standard 80 MHz/10k, 32 kHz to 2 MHz, 24-bit at most 1,333,333 Hz). What this settles from the earlier lane's UNVERIFIED list: the layouts, sizes, headers, sequence rule and rate grids are confirmed; "the radio sends its UDP stream to the client's IP address" is confirmed by NetSDR 1.3 ("normally goes back to the same IP address as the TCP control socket"); the item 0x00C5 (data output UDP IP and port) exists to override it and is NOT sent by the source; the UDP source port the radio sends to is "the port number of the TCP socket" (ambiguous between the server's port and the client's: CuteSDR's reading is kept). Also noted: the source switches to 16-bit above 1.5 MS/s while the maker's 24-bit limit is 1,333,333 Hz; no table rate lies between the two (the Afedri path, whose own limit is not the maker's, is the only way to reach that window). No change to `AscpSource` was needed.

**2b. SDR-IQ.** `sdriq_protocol.hpp` (items, parameter encodings, the seven output-rate codes 8138 to 196078 Hz, 8194-byte Data Item 0 blocks) and `SdriqSource`. The link to the radio is `SdriqLink` (write / read with timeout / interrupt / close); `defaultSdriqOpener()` returns no link and the sentence "the SDR-IQ talks through FTDI's D2XX driver, which this build does not include". **The firmware-update items, the RS232 pass-through, the security code and the A/D calibration item are never sent** (the stand-in fails the test if any appears). A boot-loader status is refused. HAND-OFF for the call: an `SdriqLink` over FTDI's D2XX API (`FT_OpenEx` by serial, `FT_SetTimeouts`, `FT_Purge`, `FT_Read`/`FT_Write`, `FT_Close`), loaded at run time from the driver the user installed; nothing of FTDI's was read or downloaded.

## Item 3: IC-R8600

USB ids 0C26:0022 (loader) and 0C26:0023 (streaming), bulk 0x02 (CI-V out), 0x88 (CI-V answers), 0x86 (I/Q); CI-V frames `FE FE 96 E0 <payload> FD [FF]`; the start-up order captured against HDSDR (frequency, attenuator, antenna, RF gain, pre-amp, IP+, prepare, ON with the rate code); BCD frequency and RF gain; six rate codes (240 k to 5.12 M); the I/Q stream with its `00 80 00 80` sync markers, which the `IqAligner` strips while locking I/Q phase to them and forwarding whole pairs (a lost sample is completed with Q = 0 and counted). Where the two projects agree, the bytes are tested literally against SoapyICR8600's arrays. **What the user must supply, never shipped or fetched:** Icom's firmware: either Icom's own "USB I/Q Package for HDSDR" (its driver loads the firmware, the port then shows as 0023) or `firmware=<IC-R8600_usb_iq.spt or the JSON sequence made from it>`. `parseFirmware` reads both formats and refuses anything that is not the vendor RAM write, writes outside the FX2's RAM / scratch RAM / CPUCS, more than 256 records or 64 KiB, or a sequence that does not begin by holding and end by releasing the CPU. WinUSB must be bound to 0C26:0022 (only if the firmware is loaded by us) and to 0C26:0023: this lane does not install drivers. Not in the two projects, so not claimed: the radio's frequency limits (an NG answer is reported to the user).

## Item 4: FUNcube Dongle Pro / Pro+

HID commands (query, frequency in Hz with the ppm trim, LNA, mixer, IF gain, bias tee, the Pro's six IF stages), the 64-byte report layout, USB ids 04D8:FB31 (Pro+, 192 kS/s) and 04D8:FB56 (Pro, 96 kS/s). `FcdControl` is over an abstract `HidLink`; `fcd_hid_win.cpp` is the production link on the Windows HID API (SetupAPI + hid.dll: part of Windows; only interfaces whose device path carries the dongle's VID/PID are opened). `FuncubeSource` wraps the existing `SoundCardSource` (I/Q stereo) and the HID tuner. Safety: command 255 (reset to bootloader) is never sent; the bias tee is switched OFF at open and at close. **Provenance caveat:** `fcdcmd.h` has no header and its macro names match the (GPL) libfcd's; this lane opened no GPL source to compare; the repository's licence is BSL-1.0 (README, LICENSE_1_0.txt, SPDX line in the .cpp) and `fcd.c`/`fcd.h` carry only the author's copyright. The brief's `fcddirect.cpp` is a Soapy test program with no command in it.

## Item 5: HackRF sweep

`buildInitRequest` (request 26: wValue/wIndex = num_bytes, step LE32, offset LE32, style, start/stop LE16 MHz pairs; checked literally), `BlockSplitter` (cuts the bulk stream into blocks, checks the 7F 7F + LE64 frequency header and that the frequency is believable for the plan, resyncs by scanning), `blockSpectrum` (Welch, Hann, centre +- step/2, DC spike removed), `SweepAssembler` (puts blocks on each range's frequency grid, completes a pass when the centre frequency stops rising; output is `core::SweepLine`) and `HackRfSweeper` (open, configure, start, poll, stop; reuses `hackrf_protocol.hpp` for sample rate, filter and gains; no thread). LINEAR style only: the interleaved style's combination rule is inside `hackrf_sweep.c` (GPL), which was not opened, so it is refused by name. Mode 5 needs USB API 0x0104; the API version cannot be read through `UsbDevice`, so an older firmware shows as a failed INIT_SWEEP with that hint.

## Item 6: HPSDR

**Already in the base and re-checked:** protocol 2 discovery and Appendix A tables for boards 1 to 4, the Metis envelope. This lane re-read v4.4: the Board Type list and Appendix A for board 1 (DSP clock 122.88 MHz, one RF ADC, 48 to 1536 ksps, 238 I&Q samples per 24-bit packet) agree with the code. The text extraction of boards 2 to 4 is column-garbled by the PDF layout and was not re-parsed (the earlier lane read the PDF page by page). **Built now:** the Hermes-Lite 2 extensions as `profile=hl2` of `Hpsdr1Source` (details in the commit and in `hl2_protocol.hpp`): Board_ID 6 recognition and extended discovery fields; NCO addresses for receivers 1..12 (8..12 at 0x12..0x16 - the gap the generic document left); the direct LNA gain (address 0x0A bit 6; -12 to +48 dB; default +20 dB); the Start/Stop watchdog bit (`watchdog=off`, default on); tuning 0 to 38.4 MHz; the three status words (addresses 0..2) as raw counts. **Not built on purpose:** I2C, AD9866 SPI, PA bias, EEPROM (fixed IP and MAC), the clock/synchronisation/master words (0x39), reset on disconnect, PureSignal, CW, TX/MOX, the wide-band stream.

## Tests (real result lines; Release; `D:/foxsdr-build/en-ossradios/tests/Release/<name>.exe`)

| Test | Result line | Repeats / notes |
|---|---|---|
| test_sdrplay_rate_map | `test_sdrplay_rate_map: 46295 checks, 0 failed` | literal expectations from SoapySDRPlay3, the specification's section 3.15 as a stub, a cross-check with `checkDuoRate`, 20000-case fuzz |
| test_ascp_maker_spec | `test_ascp_maker_spec: 65617 checks, 0 failed` | |
| test_sdriq_source | `test_sdriq_source: 1442 checks, 0 failed` | 8 consecutive runs; tone + noise + 300 ppm clock error at three rates, I/Q order proven by the sign of the measured frequency |
| test_icr8600_source | `test_icr8600_source: 828495 checks, 0 failed` | 6 consecutive runs; 3000 ppm clock error, random odd chunking, firmware fuzz (820 accepted / 3180 refused of 4000) |
| test_funcube | `test_funcube: 565 checks, 0 failed` | 5 consecutive runs; the Pro IF distribution is within 3 dB of the request over 3..57 (characterisation) |
| test_hackrf_sweep | `test_hackrf_sweep: 1085 checks, 0 failed` | 30 consecutive runs; splitter fuzz recovers blocks from noise streams |
| test_hl2 | `test_hl2: 594 checks, 0 failed` | 50 consecutive runs after a race in the test itself was fixed |
| test_ascp_source | `test_ascp_source: 322519 checks, 0 failed` | unchanged |
| test_hpsdr1_source | `test_hpsdr1_source: 845333 checks, 0 failed` | unchanged after the hl2 edit |
| test_hpsdr_frames / test_hpsdr_source | `94225 checks, 0 failed` / `1855 checks, 0 failed` | unchanged |
| test_sdrplay_dual_tuner | `test_sdrplay_dual_tuner: 61678 checks, 0 failed` | unchanged |
| ctest, 20 touched or scanning tests | 19 passed, 1 failed: `test_json_dump_policy` | the failure is `src/core/bookmark_pack.hpp:82` (a `dump()` without the replace handler); that file is byte-identical to the base (`git diff ba2a4679 -- src/core/bookmark_pack.hpp` is empty), not this lane's |
| test_thread_inventory, test_radio_families_sinks, test_health_paths, test_usb_winusb, test_hackrf_source, test_soundcard_source, test_hpsdr2, test_shutdown_budget, test_shutdown_budget_free | Passed (ctest) | shutdown budget 79.6 s |

The whole suite was NOT run (brief). The stand-ins are written from the same documents the code uses, so these tests prove consistency, not that a real radio agrees.

### Break-it table (change the named line, see RED, restore, GREEN; script `D:/foxsdr-build/eor-breakit.sh`)

| Item | Removed or changed | Failed checks with it changed |
|---|---|---|
| 1 | decimation for 500 kS/s 4 -> 8 | 9 |
| 1 | 8 MHz bandwidth override removed | 22 |
| 2a | UDP 24-bit large size 1444 -> 1440 | 2 |
| 2a | range band size 15 -> 10 (survived the first test; a truncation check was added) | 9 |
| 2b | rate-echo check in `start()` removed | 1 |
| 2b | I and Q swapped in `decodeBlock` | 8 |
| 2b | name check "SDR-IQ" removed | 12 |
| 2b | tuning range check removed | 2 |
| 3 | marker lock in `IqAligner` removed | 2 |
| 3 | scratch-RAM bound in `firmwareAddressOk` removed | 6 |
| 3 | NG accepted on mandatory commands | 3 |
| 3 | CI-V even-length pad removed | 31 |
| 4 | bias-tee-off at open removed | 1 |
| 4 | Pro+ LNA two-state check removed | 3 |
| 4 | stale-answer drain removed (survived the first test; sharpened) | 1 |
| 4 | band gap 240-420 MHz removed | 5 |
| 4 | bootloader-reset guard removed | 0: unreachable through the public API (no public call can form command 255), reported honestly |
| 5 | style byte dropped from the INIT_SWEEP data | 5 (the first version of the test crashed in its stand-in on the short payload; the stand-in was made robust, then it failed cleanly) |
| 5 | implausible-frequency check removed from the splitter | 319 |
| 5 | pass-wrap detection removed | 2 |
| 5 | an extra unknown request after stop (survived the first test; sharpened) | 1 |
| 6 | direct-LNA bit 6 removed | 2 |
| 6 | board check for profile=hl2 removed | 3 |
| 6 | watchdog bit removed | 2 |
| 6 | receiver 8 at the wrong NCO address | 5 |

## Sources opened (URL, licence or status)

| URL | Licence / status | Used for |
|---|---|---|
| https://github.com/pothosware/SoapySDRPlay3 : LICENSE.txt, Settings.cpp (commit 48bd8b41) | MIT | item 1 |
| https://sdrplay.com/wp-content/uploads/2026/04/SDRplay_API_Specification_v3.15.pdf | no licence stated | item 1 (LIF conditions, DecimationT) |
| https://moetronix.com/svdownload.htm ; files/sdriqinterfacespec104.pdf, sdripinterfacespec103.pdf, NetSdrInterfaceSpec105.pdf ; (files/SdrIQSerialPort.pdf listed, not used) | the maker's published specifications; no licence stated; no text reproduced | items 2a, 2b |
| https://github.com/ibelinp/ic-r8600-usb-iq : LICENSE, README.md, firmware/README.md, tools/spt_to_json.py, examples/linux/r8600_iq.c, examples/python/icr8600_load.py (commit 4b1be8a3) | MIT (firmware excluded) | item 3 |
| https://github.com/dmitryelj/SoapyICR8600 : LICENSE, WinUSBDevice.cpp/.h, Settings.cpp, Streaming.cpp, SoapyICR8600.hpp (commit 0e0ebff2) | MIT | item 3 cross-check |
| https://github.com/pothosware/SoapyFCDPP : LICENSE_1_0.txt, README.md, SoapyFCDPP/fcdcmd.h, fcd.c, fcd.h, SoapyFCDPP.cpp, SoapyFCDPP.hpp, fcddirect.cpp (commit 96c31ed2) | BSL-1.0 (see caveat) | item 4 |
| https://github.com/greatscottgadgets/hackrf : host/libhackrf/src/hackrf.c, hackrf.h only (commit 3a63b299) | BSD 3-clause in the header of each file; repository GPL-2.0 (nothing else read) | item 5 |
| https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol (raw wiki markdown) | no licence; facts only; repository not read | item 6 |
| https://github.com/TAPR/OpenHPSDR-Firmware : Protocol 2/Documentation/openHPSDR Ethernet Protocol v4.4.pdf | no licence line in the PDF; repository GPL-3.0, no firmware opened | item 6 re-check |

Code-host searches: this lane did not need new discovery searches for its six items (every source came from the named files in `D:/foxsdr-build/prep/oss-recheck/`); for FUNcube the manufacturer's own HID command document was not looked for again beyond what the earlier lane recorded (no licence-stated source is read).

## UNVERIFIED on hardware (none of these radios is on the desk)

* **RSPduo / RSP**: that the map's ADC rate, IF, bandwidth and decimation combinations are accepted by the API on a real RSPduo (6 vs 8 MHz bandwidth rule; coherence of the two tuners, which no document states).
* **NetSDR / SDR-IP**: everything the earlier lane marked (the UDP port rule, 24/16-bit switch point, RF gain meaning on an Afedri); this lane confirmed layouts only against the maker's text.
* **SDR-IQ**: the whole radio and, above all, the FTDI link (not built).
* **IC-R8600**: the radio, Icom's real firmware and the Windows driver binding of 0022/0023; the WinUSB `readBulkOnce` path (compiled only: no WinUSB fake exists for it); the radio's frequency limits; the RF-gain code scale (SoapyICR8600: "from observation").
* **FUNcube**: the HID report framing on Windows (that the answer's byte 0 echoes the command, byte 1 is a flag, text starts at byte 2, that Sets are answered), the Windows HID transport (only enumeration runs here: 0 dongles), the sound input's name and I/Q orientation (`swapiq` exists for it), the Pro's gain distribution against the E4000 datasheet (not opened).
* **HackRF sweep**: that the 10-byte header lies inside the num_bytes block, that the header frequency is the block's centre, the tuning rule start + offset + k * step, the USB API version requirement (cannot be read).
* **Hermes-Lite 2**: every bit position (the wiki is the only source), the polarity of the TX-inhibit bit and the units of temperature, power and current (not claimed), the 38.4 MHz limit, the +20 dB default.

## Wiring and screen hand-off (nothing here is registered with the engine, the Source menu or the settings)

Strings are English; none is translated.

* **SDR-IQ** driver key `sdriq`, args `sdriq=<serial>,timeout=<ms 100..60000, default 3000>`. Controls: "Radio" (serial, default first found); "Sample rate" (8138, 16276, 37793, 55556, 111111, 158730, 196078 S/s from `supportedSampleRatesHz()`, default 196078); "RF gain" (0, -10, -20, -30 dB, default as read from the radio); "IF gain" (0, 6, 12, 18, 24 dB, default 0); frequency 0 to 30 MHz. Health: `dataBlocks()`, `otherBlocks()`, `overloadReports()`. Sentences: "the SDR-IQ talks through FTDI's D2XX driver, which this build does not include ...", "... calls itself \"...\", not an SDR-IQ ...", "the SDR-IQ is in its firmware boot mode ...", "the SDR-IQ stopped sending". Wiring needs the D2XX `SdriqLink` (owner decision 1).
* **IC-R8600** driver key `icr8600`, args `firmware=<path>,timeout=<ms 100..10000, default 1500>`. Controls: "Icom firmware file (optional)" (file chooser, only used when the port waits for firmware); "Sample rate" (240, 480, 960 kS/s, 1.92, 3.84, 5.12 MS/s, default 1.92 MS/s); "RF gain" (0 to 255, a code, step 1, default 255 - the reference says a lower value may give an all-zero stream); "Attenuator" (0, -10, -20, -30 dB, default 0); "Pre-amp" (off/on, default on); antenna "ANT 1" / "ANT 2" / "ANT 3" (2 and 3 on HF only); frequency entered in Hz. Health: `syncMarkers()`, `realignments()`, `refusedOptional()`. Sentences: "the IC-R8600 I/Q port is waiting for Icom's firmware (0C26:0022). Install Icom's \"USB I/Q Package for HDSDR\" ... FoxSDR does not include or download Icom's firmware.", "no IC-R8600 I/Q port found ...", "the IC-R8600 did not come back as 0C26:0023 within 5 seconds ...". Enumeration for the Source menu: `icr8600UsbIds()` through `enumerateWinUsb`. The USB transport change (`readBulkOnce`) is shared code: merge it first.
* **FUNcube Dongle** driver key `funcube`, args `index=<0..15>,audio=<sound input name>,api=<host API>,swapiq=0|1,ppm=<-1000..1000>`. Controls: gains "LNA" (Pro+: 0 or 10 dB; Pro: -5 to 30 dB in 2.5 dB register steps), "Mixer" (Pro+: 0 or 20 dB; Pro: 4 or 12 dB), "IF" (Pro+: 0 to 59 dB, Pro: 3 to 57 dB, whole dB); antenna choice "Bias tee off" / "Bias tee on" (Pro+ only, default off); "Frequency correction" ppm; sample rate fixed (192 kS/s Pro+, 96 kS/s Pro); "Swap I and Q" advanced. Enumeration: `enumerateFcdHid()`. Sentences: "no FUNcube Dongle Pro or Pro+ control interface found (is it plugged in?)", "no stereo sound input with \"FUNcube\" in its name was found; name the dongle's input with audio=<input name>", "... sound inputs with \"FUNcube\" in their name were found; choose one with audio=<input name>".
* **HackRF sweep** is not a source: it produces `core::SweepLine` records. Call from a worker: `HackRfSweeper::open("serial=...|index=...")`, `configure(SweepConfig)`, `start()`, `poll(lines, timeoutMs, timeS)` in a loop, `stop()`; feed the lines to the sweep lane's stitcher/heatmap/history. Controls: "Sweep ranges" (1 to 10 pairs of whole MHz, 1 to 6000), "Step" (default 10 MHz, at least 1 kHz, must equal the spectrum step), "Offset" (default 5 MHz = half a step), "Sample rate" (2 to 20 MS/s, default 20), "FFT size" (power of two 32 to 65536, default 1024), "LNA" (0 to 40 dB, 8 dB steps, default 16), "VGA" (0 to 62 dB, 2 dB steps, default 16), "Amplifier" (default off), "Remove centre spike" (default on). Health: `goodBlocks()`, `badBlocks()`, `resyncs()`, `passes()`.
* **RSPduo / RSP rate menu**: use `duoOutputRatesHz(mode, masterFsHz)` for the menu and `resolveDuoRate` for the API fields; run a restored setting through `validateDuoSetting` before writing it. `masterFsHz` is 6 or 8 MHz ("Master sample rate" in master/slave mode, default 6 MHz).
* **Hermes-Lite 2**: `profile=hl2` on the existing `hpsdr1` key; `watchdog=on|off` (default on); controls: "Radio type" gains "Hermes-Lite 2"; "LNA gain" (-12 to +48 dB, whole dB, default +20); "Sample rate" 48, 96, 192, 384 kS/s; tuning 0 to 38.4 MHz; health row from `hl2Info()` (gateware major/minor, hardware receivers, wide-band format) and `hl2Telemetry()` (raw temperature, forward power, reverse power, current, firmware version, ADC overload). Sentences: "the radio that answered is not a Hermes-Lite 2 (it says board ...)", "this Hermes-Lite 2 sent a discovery reply too short to carry its extension (60 bytes expected)", "the Hermes-Lite 2 receiver tunes 0 to 38.4 MHz".

## Decisions for the owner

1. **FTDI D2XX** (SDR-IQ): the build stops at the call as decided. Allowing a run-time `ftd2xx.dll` link (loaded from the driver the user installed, nothing shipped) completes the SDR-IQ; the hand-off is in `sdriq_source.hpp`.
2. **FUNcube command-file provenance**: `fcdcmd.h` in a BSL-1.0 repository has no header and the same macro names as a GPL library. Accept the repository's licence statement, or obtain the manufacturer's HID command list (none was found) and re-check the numbers against it.
3. **Hermes-Lite 2 wiki**: the page and repository carry no licence. The lane used the interface facts only. An explicit permission from the project (a question in its issue tracker or mailing list; this lane may not contact outside services) would remove the doubt.
4. **HackRF interleaved sweep** needs the combination rule of `hackrf_sweep.c` (GPL): keep linear only, or hand the interleaved style to a GPL helper.
5. **RSPduo at 8 MHz**: the map departs from SoapySDRPlay3's bandwidths (documented above); an RSPduo test would settle which the API accepts.
6. **Shared USB code**: `UsbDevice::readBulkOnce` and its WinUSB implementation are edits to shared files; the WinUSB part has no automated test (no WinUSB fake exists for it).
7. **Pre-existing red**: `test_json_dump_policy` fails on `src/core/bookmark_pack.hpp:82`, a file this lane did not touch.
8. **Icom firmware**: the product text for the user (where Icom's installer places `IC-R8600_usb_iq.spt`, and that WinUSB must be bound to the port) is the owner's to word; this lane only wrote the error sentence.
