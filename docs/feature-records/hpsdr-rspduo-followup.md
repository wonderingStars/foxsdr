# HPSDR frame layer, HPSDR protocol 2, RSPduo rate rules (branch feat/hpsdrduo)

Date 2026-11-11/12. Worktree `D:\foxsdr-wt\en-hpsdrduo`, branch `feat/hpsdrduo` cut from the tip of `feat/radio-sources` (5765d6b7), build `D:/foxsdr-build/en-hpsdrduo` (`-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). Nothing pushed or merged. Stand-alone classes with tests; NOTHING is registered with the engine, the Source menu or the settings (hand-off below). No radio on this desk was opened (no HPSDR radio, no RSPduo); no software, library or driver was downloaded; every network test is UDP to 127.0.0.1.

Versions: none changed (no versioned file is touched).

## Step 0: free-source check (every page opened; "licence" = what the page or document itself states)

| # | URL | Publisher | Opened for | Licence / status |
|---|---|---|---|---|
| 1 | https://github.com/TAPR/OpenHPSDR-Firmware (root, `Protocol 1/Documentation`, `Protocol 2`, `Protocol 2/Documentation` listings; API tree listing) | TAPR / openHPSDR | which documents exist | repo LICENSE is GPL-3.0 (gateware: NONE of it opened) |
| 2 | `Protocol 2/Documentation/openHPSDR Ethernet Protocol v4.4.pdf` (same repo; 2.3 MB, plain PDF, no protection) | openHPSDR project (Phil Harman VK6PH and contributors) | protocol 2: every packet used | the document states none; read as the project's published protocol description (the brief directs this) |
| 3 | `Protocol 1/Documentation/Metis- How it works_V1.33.pdf` (same repo) | openHPSDR | PC-to-device envelope, EP2 rules | states none (read in full by the earlier lane; re-read here for the end-point-2 section) |
| 4 | `Protocol 1/Metis/Documentation/Metis User Manual_V1.4.pdf`, `Protocol 1/Hermes/Documentation/Hermes_User_Manual_V1.18.pdf` | openHPSDR | searched for the frame layout | only the sync pattern appears (LED descriptions); no frame layout, no licence text |
| 5 | https://github.com/TAPR/OpenHPSDR-SVN, `Documentation/USB_protocol_V1.60.doc` (153 088 bytes, Word 97-2003; metadata: author Phil Harman, saved 2019-01-23) and `vu3rdd/doc/HPSDR_USB_protocol_V1_0.pdf` (V1.0, 2007, 6 pages) | openHPSDR | THE FRAME LAYOUT of protocol 1 (sync, C&C, 24-bit I/Q, receiver interleave, padding table, commands) | neither document states a licence; the repository states none at its top level and holds GPL programs in other folders (none opened). The Metis guide names this document as "the latest version of this protocol". The .doc text was pulled out with a short script outside the repository, whitespace kept, so the bit diagrams were read column by column |
| 6 | https://github.com/softerhardware/Hermes-Lite2 (repo metadata and README only) | Hermes-Lite 2 project | whether its wiki could be read | repository has NO licence file: its wiki was NOT READ (the page was fetched to scratch, only its navigation header printed) |
| 7 | https://www.sdrplay.com/rspduo/ | SDRplay | current RSPduo documents | no licence; footer "SDRplay Limited 2014-2026" |
| 8 | https://sdrplay.com/wp-content/uploads/2026/06/RSPduo_Datasheet.pdf (V0.9, Jun 2020, 2 pages) | SDRplay | ADC range, IF modes, bandwidths, dual-tuner limit | no licence stated |
| 9 | https://sdrplay.com/wp-content/uploads/2026/04/SDRplay_API_Specification_v3.15.pdf (41 pages) | SDRplay | LIF conditions, master/slave, defaults (re-read) | no licence stated (earlier lane: terms NOT VERIFIED) |
| 10 | https://alexsradioshop.de/wp-content/uploads/2023/05/RSPduo-Introduction-V2-2.pdf (a reseller's copy of SDRplay's "Introduction to the RSPduo" V2.2) | SDRplay (hosted by a reseller) | pages 4 and 5: dual-tuner rates and the mode matrix | footers say "Confidential" though the document is freely distributed; no licence stated. It is an EARLIER revision than the V3 the specification cites |
| 11 | https://www.sdrplay.com/wp-content/uploads/2018/05/RSPduo-Introduction-V3.pdf; `.../2018/06/RSPDuo-Technical-Information-R1P1.pdf`; `.../docs/RSPduoDatasheetV0.5.pdf` | SDRplay | the document the specification cites | **404** on all three. The reseller mirror of V3 https://storage.eleshop.eu/files/RSPduo-Introduction-V3.pdf answered **403** (bot protection): not worked around |
| 12 | https://www.rigpix.com/miscrx/sdrplay_rspduo_techinfo.pdf, https://sdrplay.nl/wp-content/uploads/RSPduoDatasheetV0.6.pdf | third-party hosts of older datasheets | fetched, NOT used (the sdrplay.com V0.9 datasheet supersedes them) | not applicable |

Closed to the lane and not opened: every HPSDR program or firmware (PowerSDR, Thetis, piHPSDR, SDR++ ...), the SDRplay API library itself (never downloaded), the Hermes-Lite 2 wiki. Web searches were run to find documents; result snippets were not used as sources except to find the URLs above.

## Part 1: HPSDR protocol 1, the frame layer (row 1)

Files: `src/source/hpsdr_frame.{hpp,cpp}` (new), `hpsdr_source.{hpp,cpp}` and `hpsdr_protocol.hpp` (edited; the radio-sources lane's files), tests `tests/test_hpsdr_frames.cpp` (new), `tests/test_hpsdr_source.cpp` (updated).

**What the document gives (document 5), and what is built.**
* Frame = 512 bytes: `7F 7F 7F`, C0..C4, 504 bytes of samples. Device to PC (end point 6): per receiver 3-byte I then 3-byte Q (bits 23-16 first), one 2-byte microphone sample after the last receiver (ignored), `6n+2` bytes per group for n = 1..8 receivers: 63, 36, 25, 19, 15, 13, 11, 10 groups, padding 0, 0, 4, 10, 24, 10, 20, 4 (the document prints the table for 1, 4, 5, 6, 7, 8 and the three-receiver example; 2 receivers is the same arithmetic). `decodeFrame` delivers any one receiver; `Protocol1Decoder` decodes a data packet (two frames) all-or-nothing and keeps a tally (frames, refused, bad sync, samples, per-ADC overflow frames, the software serial).
* Device-to-PC C&C: address = C0 bits 7..3. Address 0: PTT/DASH/DOT in C0 bits 0/1/2, C1 bit 0 "LT2208 overflow", C4 software serial. Address 4 (C0 = 00100xxx): C1..C4 each carry ADC1..4 overflow in bit 0 and the Mercury 1..4 version in bits 7..1 (read from the diagram's column positions, not from memory). Addresses 1 to 3 (analog readings) are not decoded.
* PC to device (end point 2): the same frame with silent audio and I/Q. Built: address 0 (speed 48/96/192/384 kHz in C1 bits 1..0, the three clock/config fields, preamp, number of receivers - 1 in C4 bits 5..3), the receiver NCO frequency (C0 = 0x04 + 2(k-1), 32-bit Hz, MSB in C1) for receivers 1..7, and address 0x0A (preamp, ADC1 input attenuator 0..31 dB with its enable bit). **MOX is always 0 and every audio and I/Q byte is zero; no drive-level, CW or transmitter-NCO address is ever produced** (the stand-in counts them: 0). Sequence number restarts at zero at each Start.
* `HpsdrSource` without an injected decoder: `setSampleRateHz` (48/96/192/384 kHz), `setCenterFrequencyHz` (whole Hz, 0 to 4294967295), one gain "Attenuator" 0..31 dB for Hermes and Angelia boards only (the document names the enable bit for those two), `board=` argument (a radio that answers as another board is refused), the settings sent right behind Start and refreshed every 100 ms while streaming. An injected decoder keeps the old plumbing mode that sends nothing to end point 2.
* Loopback target: discovery and data socket bind 127.0.0.1 only (no wider bind in tests). **The LAN broadcast search is unchanged and runs only when `open()` is given no address; no test calls it.**

**What the document does not say (stated choices, all UNVERIFIED ON HARDWARE):** the numeric format of the 24-bit samples (read as signed two's complement; the protocol-2 document says 2's complement, this one is silent) and the scale (2^-23); that no I/Q swap is needed on receive (the document's swap remark is about transmit); how often the PC must send frames (the sender refreshes every 100 ms, the figure protocol 2's document recommends); a frequency address for receiver 8 (eight receivers can be UNPACKED, seven TUNED); any frequency or rate limit of a board; what a board does with a C&C bit it does not implement; whether the 384 kHz rate exists on every board.

**Break-it table** (script `D:/foxsdr-build/en-hpsdrduo-breakit.py`: apply one mutation, rebuild, run `test_hpsdr_frames`, restore with `git checkout`; failed checks with the mutation in):

| # | Mutation | Failed checks |
|---|---|---|
| 0 | remove the three-byte sync check | 844 |
| 1 | no two's-complement sign extension | 2518 |
| 2 | swap I and Q | 48 |
| 3 | group size 6n+4 | 2565 |
| 4 | always deliver receiver 1 | 30 |
| 5 | NCO frequency little-endian | 9808 |
| 6 | no speed code in address 0 | 2863 |
| 7 | MOX set in the NCO frames | 10223 |
| 8 | sender sequence not restarted | 3 |
| 9 | no periodic refresh | 1 |
| 10 | settings not sent by start() (survived at first: the refresh covers it; a check that `controlPacketsSent() >= 2` right after start() was added) | 1 |
| 11 | address-4 overflow flags not decoded | 3 |
| 12 | `board=` mismatch not refused | 2 |
| 13 | rate check removed from validation | 873 |
| 14 | packet not all-or-nothing | 2 |
| 15 | attenuator enable bit not sent | 2 |
| 16 | source does not restart the EP2 sequence at Start | 2 |

## Part 2: HPSDR protocol 2 (row 1, "if its document is free")

Files: `src/source/hpsdr2_protocol.{hpp,cpp}`, `hpsdr2_source.{hpp,cpp}` (new), test `tests/test_hpsdr2.cpp` (new). Document 2 (V4.4), provenance and byte layouts in the header of `hpsdr2_protocol.hpp`.

**Built.** Discovery (60 bytes) and reply; board table of Appendix A for boards 1 to 4 (Hermes, Hermes 10E/100B, Angelia, Orion; 122.88 MHz DSP clock; 1/1/2/2 ADCs); the General packet (the UDP ports, phase-word flag, reset timer, big-endian 3-byte I&Q, PA and Alex off), the DDC Specific packet (DDC0 on ADC0, 48/96/192/384/768/1536 ksps, 24 bits), the High Priority packet (run bit, DDC0 frequency word = 2^32 f / DSP clock when the reply says phase word, ADC0 attenuator 0..31 dB; **PTT, drive level and every transmit-side byte stay 0**); the order General, DDC Specific, High Priority with run at Start; a High Priority refresh every 100 ms (the document: at least one per second, 100 ms recommended; the stand-in goes to standby after one second without one, as the document says the radio does); run bit cleared at Stop and at close; the DDC I&Q packet (16-byte header, 24-bit two's complement, up to 238 samples, length must equal 16 + 6 x samples, 24 bits per sample) and the status packet (ADC overload, DDC FIFO overflow). `Hpsdr2Source`: ONE unconnected UDP socket (the radio answers the discovery source port from several of its own ports; packets are told apart by source port), 4 MiB receive buffer, loopback target binds 127.0.0.1 only, broadcast only when no address is given, `takeover=1` for a radio that says it is connected elsewhere, port arguments (used by the stand-in).

**Refused with a sentence:** boards without an Appendix A table (Atlas, Orion Mk II, Hermes Lite, Saturn, unknown), a busy radio without `takeover=1`, a radio whose format byte offers no big-endian 3-byte I&Q, a rate outside the six, a frequency outside 0..(DSP clock) as a phase word, an attenuator outside 0..31.

**Not decided by the document (stated choices):** the CIC bytes (zero, "for future use"); no I/Q swap (the document says the spectrum "may appear reversed" and to swap then); host-side sequence numbers run on across Starts; Hermes Lite, Orion Mk II and Saturn limits (no table), so they are refused rather than guessed. UNVERIFIED ON HARDWARE.

**Break-it table** (`D:/foxsdr-build/en-hpsdrduo-breakit-p2.py`, `test_hpsdr2`):

| # | Mutation | Failed checks |
|---|---|---|
| 0 | no two's-complement sign extension | 8960 |
| 1 | 24-bit sample-size check removed | 5 |
| 2 | length-matches-count check removed | 8809 |
| 3 | run bit never set | 23 |
| 4 | wrong DSP clock in the phase word | 12 |
| 5 | DDC0 port not in the General packet | 3 |
| 6 | busy radio accepted | 1 |
| 7 | undocumented board accepted | 10 |
| 8 | no periodic High Priority refresh | 2 |
| 9 | status packets not recognised | 2 |
| 10 | attenuator byte at 1442 | 3730 |
| 11 | ksps = rate / 1024 | 23 |
| 12 | DDC0 enable bit wrong | 5002 |
| 13 | run bit not cleared at stop | 4 |
| 14 | silence is never a fault | 1 |

## Part 3: RSPduo sample-rate rules (row 2)

Files: `src/source/sdrplay_dual_tuner.{hpp,cpp}` (additions only), `tests/test_sdrplay_dual_tuner.cpp`. Documents 7 to 11: what each gave is in the header comment of `sdrplay_dual_tuner.hpp`.

**Found.** The RSPduo Introduction V3 is still not obtainable (404; the reseller mirror 403). SDRplay's datasheet V0.9 (sdrplay.com) and the V2.2 Introduction (reseller's copy of SDRplay's own file), with the specification, give: single tuner = zero IF at an ADC rate of 2 to 10.66 MHz with any of eight IF bandwidths (200, 300, 600 kHz, 1.536, 5, 6, 7, 8 MHz), or low IF as the specification's six conditions list; dual tuner and master/slave = both tuners in LOW IF, ADC rate 6 MHz or 8 MHz only, bandwidth at most 1.536 MHz, master dictates the rate.

**Built.** `DuoMode {SingleTuner, DualTuner, MasterSlave}`, `documentedLowIfRows`, `checkDuoRate`, `documentedLowIfOutputRatesHz`, `documentedDualTunerTable`. Dual tuner and master/slave rows are the INTERSECTION of the Introduction/datasheet limits and the specification's low-IF list (6 MHz with IF 1.620 MHz and bandwidth 0.2, 0.3, 0.6 or 1.536 MHz; 8 MHz with IF 2.048 MHz and bandwidth 1.536 MHz), so a row is refused unless both documents agree; output rates 6 and 8 MHz. `DualTunerRateTable` itself still ships empty; `documentedDualTunerTable()` is the filled one.

**Refused, because no document opened says it:** every decimation factor in dual-tuner and master/slave mode except 1 (the specification shows `decimationFactor` as a byte and lists no values); in SINGLE-tuner mode 1, 2, 4, 8, 16, 32 are accepted because the shipping driver already uses them (field-logged), not because a document lists them. **Open questions for the owner:** (a) the specification's tuner-parameter comment says the default IF in master/slave mode is zero IF for the master, which the Introduction's "both tuners in low IF" contradicts: the table follows the Introduction; (b) the V3 Introduction (pages 4 and 5) was never read, so a difference from V2.2 is possible; (c) the two tuners' coherence is stated by no document opened. UNVERIFIED ON HARDWARE (no RSPduo; the SDRplay API is never loaded here).

**Break-it table** (`D:/foxsdr-build/en-hpsdrduo-breakit-duo.py`, `test_sdrplay_dual_tuner`):

| # | Mutation | Failed checks |
|---|---|---|
| 0 | 8 MHz row missing from the dual-tuner rows | 6 |
| 1 | dual tuner accepts decimation | 233 |
| 2 | zero-IF upper limit 20 MHz | 90 |
| 3 | undefined bandwidth accepted | 118 |
| 4 | any decimation in single tuner | 1014 |
| 5 | wrong IF on a 6 MHz dual-tuner row | 14 |

## Results (real result lines, build `D:/foxsdr-build/en-hpsdrduo`, Release)

| Test | Result line |
|---|---|
| test_hpsdr_frames (new, part 1) | `test_hpsdr_frames: 94225 checks, 0 failed` (12 of 12 consecutive runs green) |
| test_hpsdr2 (new, part 2) | `test_hpsdr2: 67787 checks, 0 failed` (10 of 10 consecutive runs green) |
| test_sdrplay_dual_tuner (part 3, extended) | `test_sdrplay_dual_tuner: 61678 checks, 0 failed` |
| test_sdrplay_ifmode_diag (unchanged, same area) | `test_sdrplay_ifmode_diag: 178 checks, 0 failed` |
| test_hpsdr_source (updated) | `test_hpsdr_source: 1855 checks, 0 failed` at lower load; LOAD-SENSITIVE, see below |
| test_thread_inventory | `test_thread_inventory: 10069 checks, 0 failed` (no thread was added by this lane: both sources use the base class's reader) |
| test_health_paths, test_json_dump_policy | Passed / `3 checks, 0 failed` |
| test_shutdown_budget and _free | Passed, 97.4 s and 85.2 s (the new files declare no named chrono waits: only plain millisecond ints) |

Own tests through ctest: `100% tests passed out of 7` (hpsdr2, hpsdr_source, hpsdr_frames, health_paths, sdrplay_dual_tuner, shutdown_budget, shutdown_budget_free).

### Full headless suite (`ctest -C Release -L headless -j4 --timeout 300 -I a,b`, slices of 90 of the 692 tests; the machine ran at 100% CPU throughout)

Every red test of every slice was then re-run alone. Result:

* the known configuration failures: test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode, test_window_failed_app;
* the known load flakes: test_pass_control, test_patch_dest_async;
* passed when re-run alone: test_audio_open, test_cat_server, test_engine_api_conformance, test_engine_control_thread, test_excuse_cap, test_engine_shell_guard, test_link_request_poll, test_pa_stream_list_guard, test_patch_outputs, test_plugin_hostile, test_rtl_tcp_source, test_sdrplay_probe, test_sentinel, test_server_host, test_transmit_dead_man, test_gps_app_free, telemetry_worker (235 s), test_failure_files (it failed twice under ctest beside other lanes' runs of the same test, passed alone twice and directly: shared temp folder name);
* NOT in the known list, still red or slow alone, and shown NOT to be this branch's: test_bounded_hooks (a timing check, `A::pauses(w) == before + 1u`: 6 of 10 runs red on the radio-sources lane's own build `D:/foxsdr-build/feat-src`, 5 of 10 on this branch); test_crash_upload_free (`srv.connections() == 1` at test_crash_upload.cpp:1782: 1 of 2 runs red on feat-src, 0 of 3 on this branch); test_engine_api_merge (a timing test that reruns itself when a run outlasts 900 ms: 7 min 42 s on feat-src, 414 s and 18 reported failures on this branch under the same load; the end result of the feat-src run was not read); test_soapy_enum_proc (120 s timeout under load, no code of this lane in it); test_hpsdr_source failed 3 of 3 runs at one point of the load (22 to 23 failing checks on this branch, 26 to 45 on the radio-sources lane's original binary at the same moment; 0 failures at lower load).
* Nothing in the suite was red because of the files this lane changed, as far as these comparisons can show; the comparison is by repeated runs on a loaded machine, not by an idle one.

Config check: `%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` before and after (unchanged); no test or tool here used it.

## Found while testing (kept because it is the kind of thing that looks like proof and is not)

* The first run of the thirty-start/stop-cycle test went red about one run in three under load. Cause, from the printed counters: the stand-in radio starts and joins a thread per Start and trailed the PC by seconds (it had seen 7 to 29 of 31 starts when the source's three-second silence bound fired on the last one). The source's behaviour was right; the test now waits until the stand-in has processed every stop. 12 of 12 runs green afterwards.
* A source socket with the default 64 KiB receive buffer lost up to 85 of 2400 protocol-2 packets at 192 kS/s on this loaded machine; `Hpsdr2Source` now asks for 4 MiB (a real radio sends about 6500 packets a second at 1.536 MS/s). The protocol-1 source uses the radio-sources lane's `UdpTransport` (not edited here) with its default buffer: 1500 packets a second at 384 kS/s; unmeasured on real hardware.
* One break-it survived the first table (settings not sent by `start()`): the periodic refresh made the send redundant. A check on `controlPacketsSent()` straight after `start()` closed it.
* `test_hpsdr_source` (the earlier lane's test) is load-sensitive in this tree and in the earlier lane's own build: 1 failing run in 6 for the original binary and 2 in 6 for this branch's, at different lines each time (discovery windows missed under load). Not changed by this lane.

## Wiring and screen hand-off (the registering of the sources is not this lane's)

* Register `HpsdrSource` under `hpsdr` (existing hand-off) and `Hpsdr2Source` under `hpsdr2`. Args: `hpsdr=host[:port],mac=,board=,timeout=` and `hpsdr2=host[:port],mac=,takeover=1,timeout=` (no address = LAN broadcast discovery, which must stay behind a user-started scan; `discoverHpsdr` and `discoverHpsdr2` are the scan lists). One reader thread each, the base class's (already in the thread inventory).
* Controls the screen needs, English: "Attenuator" (0 to 31 dB, step 1, default 0; protocol 1 only Hermes and Angelia, protocol 2 every documented board), sample-rate list ("48 kHz", "96 kHz", "192 kHz", "384 kHz"; protocol 2 adds "768 kHz", "1.536 MHz"), tuning in Hz, option "take over a radio that is connected to another computer", optional "board" check. Counters for a health row: `packetsLost()`, `packetsLate()`, `badPackets()`, `frameTally()` (protocol 1: frames, refused frames, per-ADC overflow frames), `statusPackets()`, `adcOverloadPackets()`, `fifoOverflowPackets()` (protocol 2).
* Sentences (English): "the HPSDR radio stopped sending", "the HPSDR network connection failed", "no HPSDR radio answered the discovery request", "no HPSDR protocol-2 radio answered the discovery request", "no HPSDR radio with that MAC address answered (N other(s) did)", "the HPSDR radio that answered is not the X that was asked for (it said Y)", "board type N (name) has no table in the openHPSDR Ethernet protocol document's hardware appendix (boards 1 to 4), so it is not supported", "the radio says it is already connected to another computer (add takeover=1 to take it over)", "the radio does not offer big-endian 3-byte I&Q, the only format this build reads", "the HPSDR sample rate must be 48000, 96000, 192000 or 384000 Hz" (protocol 2: "... 768000 or 1536000 Hz"), "the HPSDR frequency must be 0 to 4294967295 Hz (a 32-bit value in whole hertz)" (protocol 2: "... 0 up to (not including) the 122.88 MHz DSP clock"), "the HPSDR attenuator must be 0 to 31 dB", "eight receivers can be unpacked but not tuned: the protocol document lists no frequency address for receiver 8", the `checkDuoRate` sentences for the RSPduo.
* RSPduo: in the SDRplay source's rate request call `checkDuoRate(mode, {fsHz, bw, ifType, decimation})` and offer `documentedLowIfOutputRatesHz(mode)` as the rate list of dual-tuner and master/slave mode (6 and 8 MHz).
* No translated strings were written.

## Not done / UNVERIFIED (everything below is unproven because no radio was on the desk)

Every byte on a real HPSDR radio (protocol 1 and 2) and every rate on a real RSPduo; the sign format and scale of protocol-1 samples; the refresh interval of protocol 1; the receiver-8 NCO; Hermes Lite, Orion Mk II, Saturn and Atlas under protocol 2; the 384 kHz rate on each board; the V3 Introduction; the licence status of the documents (none states one). Not built: transmit of any kind, the bandscope, microphone/audio paths, Alex filter control, multi-DDC and synchronous-DDC use (one DDC is received), DUC packets, wideband data.
