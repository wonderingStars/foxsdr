# Radio families (R7), by open documents and stand-in devices (branch feat/radio-families)

Date 2026-11-11/12. Worktree `foxsdr-wt/feat-radios`, branch `feat/radio-families` cut from the tip of `feat/radio-sources` (5765d6b7), build `D:/foxsdr-build/feat-radios` (Release, `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). Nothing pushed or merged. Every part is a stand-alone class with tests; NOTHING is registered with the engine, the Source menu or the settings: that is the wiring lane (hand-off below).

Brief (`queued-briefs/unblocked-radios.md`): for each family, find an openly published wire or USB protocol description or a permissively licensed interface document (licence first); where one exists, build the source against a stand-in, with refusal paths, marked UNVERIFIED ON HARDWARE; where none exists, say exactly what was looked for and what would unblock it.

**Outcome in one table**

| Family | Result |
|---|---|
| RFspace NetSDR, SDR-IP, Cloud-IQ | BUILT (`AscpSource`, driver key `rfspace`) from CuteSDR's BSD-licensed interface files plus Afedri's documents; RFspace's own interface specification is on request only and was NOT obtained |
| Afedri SDR-Net / AFE822x | BUILT (same source, model "Afedri"): single-channel network mode only |
| Red Pitaya | BUILT (`Hpsdr1Source`, profile `redpitaya`): it presents itself as an HPSDR Hermes radio; the same source also serves Metis/Hermes-class radios (`generic`) |
| Aaronia SPECTRAN V6 (RTSA-Suite HTTP server) | BUILT (`AaroniaSource`, key `aaronia`), no authorisation |
| AntSDR | NOT BUILT, nothing needed: the iio firmware is a fork of PlutoSDR's; the existing `PlutoSource` (IIOD) takes `host=192.168.1.10` (UNVERIFIED that the IIO device names match) |
| RFspace SDR-IQ | NOT BUILT: USB (FTDI) radio; no protocol document found |
| FUNcube Dongle Pro / Pro+ | NOT BUILT: no HID command document |
| KrakenSDR | NOT BUILT: coherent I/Q only through Heimdall (GPL), wire format undocumented |
| RFNM | NOT BUILT: only source of the protocol is librfnm (MPL-2.0, not on the permitted-to-read list) |
| Icom IC-R8600 I/Q output | NOT BUILT: the I/Q Output Reference Guide is behind Icom's agreement page / third-party mirrors |
| Perseus | NOT BUILT: only a C library (LGPL-3.0); no protocol document |
| ELAD | NOT BUILT: no developer document found |
| Harogic | NOT BUILT: the programming guide documents a vendor library (htraapi), not a wire protocol |
| WiNRADiO | NOT BUILT: DLL API only |

## Clean-room statement and everything opened

No GPL/LGPL/AGPL source or port was read. No executable, installer, library, driver or script was downloaded. Documents (PDF and .doc) and CuteSDR's source text were fetched into a scratch folder outside the repository (or read through the fetch tool) and read there. No outside server or radio was contacted; the radios on the desk were not opened; every test talks to a stand-in on 127.0.0.1. `%APPDATA%/foxsdr/config.json` was never opened for writing: sha256 at the end `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).

| URL / document | Licence or status | Used for |
|---|---|---|
| https://sources.debian.org/data/main/c/cutesdr/1.20-4/debian/copyright (opened FIRST) | CuteSDR upstream: `interface/*` and `gui/*` are "MOEBSD" = Simplified BSD (Copyright 2010-2011 Moe Wheatley); `siqs/*` and `debian/*` GPL-3+ | decided that `interface/*` may be read |
| CuteSDR 1.20 `interface/ascpmsg.h`, `protocoldefs.h`, `sdrinterface.cpp`, `netiobase.cpp`, `dataprocess.cpp`, `gui/sdrsetupdlg.cpp` (https://sources.debian.org/src/cutesdr/1.20-4/) | Simplified BSD, notice below | ASCP message header and types, control-item numbers, setup and start sequence, UDP packet layout and sizes, sequence rule, rate tables, RF-gain steps |
| Afedri, "AFE822x SDR-Net protocol extensions" (8 p., https://afedri-sdr.com/index.php/downloads/category/2-documentation?download=242) | manufacturer's document, no licence stated | literal bytes of the SDR-IP header examples, frequency and RF-gain items, main-clock item, UDP stream picture, rate function |
| Afedri, "SDR Network Control Box software description" rev 6 (23 p., download=193) | same | default port 50000, multi-stream +1, sample-rate rules |
| RFspace NetSDR User's Guide (http://www.rfspace.com/support_files/NetSDR_Guide_0112.pdf, 8 p.) and http://www.rfspace.com/RFSPACE/Support.html | no licence | confirms the interface specification is "on request only" (contact info@rfspace.com) |
| RFspace SDR-IQ getting-started guide (1 p.) | no licence | no protocol in it |
| "HPSDR - USB Data Protocol" V1.58 and V1.60 (https://github.com/TAPR/OpenHPSDR-SVN/raw/master/Documentation/USB_protocol_V1.60.doc, text via antiword) | no licence stated; the repository's file listing shows no licence file; published by the project for implementers | the 512-byte frame layout, C&C addresses, padding table |
| https://pavel-demin.github.io/red-pitaya-notes/sdr-receiver-hpsdr | no licence stated | Red Pitaya = Hermes emulation, 48/96/192 kSPS, 0 to 61.44 MHz, links the two documents above |
| https://github.com/softerhardware/Hermes-Lite2/wiki/Protocol | no licence stated | **opened by a fetch tool before the licence question was settled**; its summary agrees with the USB document; nothing here depends on it; HL2 extensions NOT built. Disclosed because the earlier lane had decided not to read it. |
| Aaronia, "RTSA HTTP Stream Server Endpoints" (12 p., https://v6-forum.aaronia.de/wp-content/uploads/asgarosforum/3637/RTSA-http-Stream-Server-Endpoints-11.pdf) | no licence stated; the manufacturer's published description for other programs | the whole Aaronia source |
| https://github.com/TAPR/OpenHPSDR-SVN (listing) | no licence shown | where the USB document lives |
| https://afedri-sdr.com/index.php/downloads, `/category/2-documentation`, `/category/3-misc`; https://github.com/alexander-sholohov/SoapyAfedri | site: none; SoapyAfedri GPL-3.0 (listing only, NOT read) | found which documents exist |
| FUNcube: https://www.funcubedongle.com/ , `?page_id=1225`, `?page_id=313`; User Manual `MyImages/FCD2ManualV4.pdf` (5 p.) | no licence statement anywhere | the manual has no HID commands (it says only to check the sample rate is 192000) |
| Perseus: https://github.com/Microtelecom/libperseus-sdr | LGPL-3.0 (COPYING.LESSER), C library, no protocol document | not read |
| RFNM: https://github.com/rfnm (listing), https://github.com/rfnm/librfnm , https://rfnm.com/ , https://rfnm.com/docs/ | librfnm and others MPL-2.0; docs site has no API/protocol page | not read (MPL-2.0 is not on the permitted list) |
| AntSDR: https://antsdr-doc-en.readthedocs.io/ (E200 getting started), https://github.com/MicroPhase/antsdr-fw | no licence stated; firmware "forked from analogdevicesinc/plutosdr-fw" | board address 192.168.1.10, `iio_info -u "ip:192.168.1.10"` |
| WiNRADiO: https://winradio.com/home/developer-g33ddc.htm | no licence text | DLL API only |
| Harogic: https://lambdaphoto.co.uk/pdfs/HAROGIC/LAMBDA_API_ProgGuide.pdf (7565 lines of text, read by index and search) | no licence text seen | documents the vendor C library |
| Icom: https://icomamerica.com/support/firmware_driver/1972 | shows an agreement block ("I have read, fully understood and agree the above.") | see IC-R8600 |
| KrakenSDR: https://github.com/krakenrf/krakensdr_docs and its wiki page "08.-GNU-Radio-Block" | no licence stated | the page says only "receives coherent IQ data from Heimdall via a TCP socket connection" |
| ELAD: web search only | | no developer document found |

**CuteSDR NOTICE** (Simplified BSD, required where its numbers are reproduced): "Copyright 2010 Moe Wheatley. All rights reserved. THIS SOFTWARE IS PROVIDED BY Moe Wheatley ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED." Only the byte layouts and numbers (facts of an interface) are used; no code was copied, and the file headers of `ascp_protocol.hpp` carry the notice.

## Part 1: RFspace / Afedri "ASCP" (SDR-IP) network radios, `AscpSource`

Files: `src/source/ascp_protocol.hpp` (framing and numbers), `ascp_source.{hpp,cpp}`, test `tests/test_ascp_source.cpp`. Commit 743a8598.

**What was built.** Message header (13-bit length, 3-bit type), a TCP stream assembler that refuses impossible lengths and then believes nothing more, the control items used, frequency ranges (5-byte values), the four UDP packet sizes (16-bit 1028/516, 24-bit 1444/388) with sequence accounting (first packet 0, the counter skips 0 on wrap), Afedri's real-rate function, and `AscpSource`: identify by the interface name (NetSDR, SDR-IP, CloudSDR/CloudIQ, "AFEDRI SDR-Net", "AFE822x SDR-Net"; anything else, including USB-only names, is refused and the name is quoted, printable and length-bounded), serial, frequency range, tune (range-checked), RF gain 0/-10/-20/-30 dB, sample-rate table per model (Afedri: any 8 kS/s to 2 MS/s, the radio reports the real rate), start (setup items, rate, receiver on; 24-bit below 1.5 MS/s, small packets at 100 kS/s and below), stop, 2 s keepalives, one reader thread that serves TCP and UDP with `select`, a silence fault, lost/late/bad packet counts, A/D-overload status count, datagrams from any address other than the radio's counted bad.

**UNVERIFIED ON HARDWARE** (no NetSDR, SDR-IP, Cloud-IQ or Afedri here). Specifically: that the radio sends its UDP stream to the source address and port of the one-byte keepalive datagram (CuteSDR binds its UDP socket to the TCP port number and sends the keepalive from it; this client does the same by default, `udpport=0` for any port); the rate tables, the 24/16-bit switch and the MTU switch (all CuteSDR's choices); the RF-gain steps and what they mean on an Afedri (the Afedri document says values above 0 mean its own gain scale); Cloud-IQ's name strings (CuteSDR's). **Not built:** Afedri dual/diversity channel modes (channel number fixed to 0), the HID-over-TCP forward (header E0 09) and the USB HID/audio mode, the SDR-IQ/SDR-14 (USB, below), SDR-IP's "display frequency" item.

**The unobtained document.** RFspace's "NetSDR/SDR-IP Interface Specification" is "available on request" (their support page; the guide says "contact info@rfspace.com"). Asking for it is an owner step (an e-mail to a vendor; this lane may not contact outside services). CuteSDR (a working client) was used instead.

HAND-OFF (wiring): driver key `rfspace`, args `rfspace=host[:port],rate=Hz,udpport=N,timeout=ms` (port default 50000). Screen: "Radio address" (host or host:port; default port 50000); "Sample rate" (the list from `supportedSampleRatesHz()`, range 62.5 kS/s to 2 MS/s by model, default the model's third entry: NetSDR 250 kS/s); "RF gain" (steps 0, -10, -20, -30 dB, default 0; name "RF gain"); "UDP port" (advanced, default = control port, 0 = any); `AscpSource::localUdpPort()`, `packetsLost()`, `packetsLate()`, `badPackets()`, `overloadReports()` for the health row. Strings: the `lastError()` sentences ("could not reach the radio at ...", "this does not look like an RFspace or Afedri network radio ...", "... calls itself \"...\", which this build does not know (it knows NetSDR, SDR-IP, Cloud-IQ and Afedri SDR-Net)", "the radio tunes A to B Hz; C Hz is outside that", "the RF gain takes 0, -10, -20 or -30 dB", "stop the radio before changing its sample rate", "the radio stopped sending", "the radio closed the connection"). Thread: one (base class, already in the inventory).

## Part 2: HPSDR protocol 1 frame layer, `Hpsdr1Source` (Hermes-class radios and the Red Pitaya)

Files: `src/source/hpsdr1_frames.hpp`, `hpsdr1_source.{hpp,cpp}`, test `tests/test_hpsdr1_source.cpp`; `net_sample_source.{hpp,cpp}` gain `UdpTransport::setReceiveBuffer` (a descheduled reader lost packets of a 1500 packet/s stream under load). Commit f21b9567. The earlier `HpsdrSource` is untouched (its Metis envelope, discovery and sequence accounting are reused).

**What was built.** The receive frame (sync 7F7F7F, five C&C bytes, 24-bit big-endian I/Q for 1 to 8 receivers, the document's padding table, microphone ignored), the command frame (speed bits, receiver count, duplex, receiver and transmitter NCO, drive 0, attenuator 0 to 31 dB; MOX never set, audio zero), and `Hpsdr1Source`: discovery, settings before Start, refresh of the full command set about every 100 ms, live tune/rate/attenuator, lost/late/bad/overload counts, silence fault. Profiles: `generic` (48/96/192/384 kHz, attenuator, no range claimed) and `redpitaya` (48/96/192 kHz, 0 to 61.44 MHz, no gain: both numbers are Pavel Demin's page, not a manufacturer's specification).

**UNVERIFIED ON HARDWARE**: how often a real board needs the command frames (the refresh rate is a choice); duplex = 1 and the transmitter frequency equal to receiver 1's (set so that either reading of the document gives the right answer); the 24-bit full scale (2^23); the attenuator's dB per step; whether a Red Pitaya needs anything beyond this. **Not built:** receiver 8's frequency (the document lists no address), more than one receiver delivered to the pipeline, the bandscope, the Hermes-Lite 2 extensions, transmit of any kind, Penelope/Alex/Apollo controls.

HAND-OFF (wiring): driver key `hpsdr1`, args `hpsdr1=host[:port],mac=aa:bb:...,profile=generic|redpitaya,rate=Hz,timeout=ms` (no `hpsdr1=` = broadcast discovery; `discoverHpsdr` from the earlier lane is the Source-menu scan). Screen: "Radio address"; "Radio type" (HPSDR / Hermes-class, Red Pitaya); "Sample rate" (48, 96, 192 kHz; 384 kHz for the generic type; default 192 kHz); "Attenuator" (0 to 31 dB, whole dB, generic type only, default off = not sent). Counters for the health row: `packetsLost`, `packetsLate`, `badPackets`, `overloadFrames`, `commandPacketsSent`. Strings: "no HPSDR radio answered the discovery request", the MAC sentence, "this radio takes 48000, 96000 or 192000 samples per second", "the Red Pitaya receiver tunes 0 to 61.44 MHz", "the radio stopped sending". Thread: one (base class).

## Part 3: Aaronia RTSA HTTP stream server, `AaroniaSource`

Files: `src/source/aaronia_protocol.hpp` (HTTP head, chunk decoder, record parser, request bodies), `aaronia_source.{hpp,cpp}`, test `tests/test_aaronia_source.cpp`. Commits 6a4717d9, 953422f2.

**What was built.** A minimal HTTP/1.x head parser, a chunked-transfer decoder, a record parser (incremental brace scan that respects strings; I/Q packets returned, other payloads counted and skipped; JSON sample arrays, or raw float32 after a record separator; a pair containing NaN/Inf becomes 0), capture and stream request bodies, and `AaroniaSource`: `/info` as the proof it is an RTSA server block, capture (centre, span) and stream start before the stream is opened, `GET /stream?format=...&rate_adaption=0`, the reader per `start()`, measured sample rate (pairs / (endTime - startTime)), centre from the packets' frequency fields, timestamp-gap count (the server drops data past 8 MByte), silence fault.

**Authorisation (Basic, RToken) is NOT built**: it involves a secret; where FoxSDR keeps one is a wiring-lane decision. A server that asks is refused with a sentence saying so.

**UNVERIFIED** (no SPECTRAN, no RTSA-Suite) and the points the document leaves open: the delimiting around a raw record (taken as JSON, optional LF, one RS byte, then pairs x 8 bytes), that `frequencySpan` sets the I/Q rate, the default port (54664 is the document's example), float32 byte order (little-endian assumed). int16 and float16 are not offered (the scale direction and half-float decoding were not verified).

HAND-OFF (wiring): driver key `aaronia`, args `aaronia=host[:port],format=json|float32,rate=Hz,timeout=ms`. Screen: "Server address" (port default 54664); "Sample format" (JSON default; float32 advanced); "Sample rate" (any 1 kS/s to 1 GS/s; sent as the capture span); the centre frequency is the receiver's usual tuning. Counters: `gapCount`, `otherPackets`, `nonFiniteSamples`. Strings: "could not reach the Aaronia server at ... - is the address right, and is the HTTP server block running?", "the Aaronia server wants a user name and password, which this build does not send ...", "... it does not look like an RTSA HTTP server block", "the Aaronia server refused the stream", "the Aaronia server stopped sending", "the Aaronia server's stream is malformed". Thread: one (base class), started per `start()`.

## Families not built: what was looked for, what would unblock

**RFspace SDR-IQ.** Looked for: RFspace support page (documents listed: the SDR-IQ getting-started guide, which holds no protocol; "SDR-IP protocol specifications" and "NetSDR protocol specifications" on request only), CuteSDR (drives the SDR-IQ through the FTDI D2XX library, a vendor driver library). Not built because the radio is USB through an FTDI chip and no document of its framing was obtained. UNBLOCK: RFspace's interface specification (on request, e-mail) or the owner's decision to use FTDI's D2XX/VCP. Alternative route that exists now: CuteSDR-compatible control over a virtual COM port is not documented here (UNVERIFIED).

**FUNcube Dongle Pro / Pro+.** Looked for: an official HID command document: the manufacturer's Pro+ page lists the User Manual, a firmware manual, the frequency-control program and its SOURCE (no licence stated, therefore not read), older versions; the Pro page lists PDFs for software set-ups and says a "documented API firmware" was in progress; the User Manual was read and holds no HID commands. Third-party command descriptions (Hamlib's header LGPL, fcdctl, qthid, Linrad's driver, SDRangel) are GPL/LGPL and were not read. UNBLOCK: the manufacturer's HID command list, or the owner's decision that the vendor's own frequency-control source (no licence stated) may be read. The I/Q itself is ordinary USB audio at 192 kS/s (Pro+, per the manual); only tuning needs the HID commands. Existing route: a SoapySDR module the user installs themselves.

**KrakenSDR.** Looked for: wiki "08.-GNU-Radio-Block" (says only that Heimdall serves coherent I/Q over a TCP socket; no frame layout, sizes or ports), the docs repository (no licence stated, no I/Q format page). Heimdall (the DAQ) is GPL-3.0 and was not read. UNBLOCK: a written description of Heimdall's I/Q frame, or the owner's decision on reading GPL source (clean-room rule says no). What exists now: the board is five RTL-SDR tuners; FoxSDR's native RTL-SDR driver and the eight-radio engine can open the five dongles as five radios; coherence needs the noise-source calibration, which is the DF lane's, not this one's.

**RFNM.** Looked for: https://rfnm.com/docs/ (hardware pages, Soapy SDR page; no API or protocol document), https://github.com/rfnm (librfnm MPL-2.0, soapy-rfnm MPL-2.0, webgui, rfnmctl). The librfnm README states a "versioned wire protocol" and gives no prose description. MPL-2.0 is not on the permitted-to-read list (Apache-2.0, MIT, BSD, Boost, public domain). UNBLOCK: an owner decision that MPL-2.0 source may be read (weak copyleft: a clean-room risk to weigh) or a written protocol description from RFNM. Existing route now (patent-free, no new code): RFNM's own SoapySDR module, if the user installs it, through FoxSDR's SoapySDR source.

**Icom IC-R8600 I/Q output.** Looked for: Icom's "I/Q Output Reference Guide". The Icom America page for the USB I/Q package (https://icomamerica.com/support/firmware_driver/1972) shows an agreement block before the package; the previous lane found the CI-V guides on icomjapan.com behind an AGREE button that prohibits reproduction and transfer; the copies of the I/Q guide that turned up are third-party mirrors (ManualsLib, ManualOwl), not read. Search snippets only (UNVERIFIED, from third-party text, NOT used): USB bulk IN endpoint 0x86, 16-bit and 24-bit little-endian I/Q, sync words, sample rates 240 kHz to 5.12 MHz, CI-V command `1A 13 01` switching the output. UNBLOCK: the owner opens Icom's guide and agrees to its terms, or points at a freely readable copy; then a WinUSB bulk reader against a stand-in endpoint is a small job.

**Perseus.** Looked for: https://github.com/Microtelecom/libperseus-sdr (LGPL-3.0 C library with its own firmware/FPGA data files; no protocol document); the maker's own site was not opened. UNBLOCK: a protocol description, or the owner's decision on LGPL source. Possible existing routes (NOT checked here): a SoapySDR module for the Perseus that the user installs, or HDSDR-style ExtIO (the ExtIO decision is the survey's R8).

**ELAD (FDM-S2, FDM-DUO, FDM-S3).** Looked for: a developer document (web search; only user manuals; the FDM-DUO has a CAT interface whose command set was not found). UNBLOCK: ELAD's interface document.

**Harogic (SA/NX/PX/SAM series).** Looked for: https://lambdaphoto.co.uk/pdfs/HAROGIC/LAMBDA_API_ProgGuide.pdf, "API Programmers Reference Manual, API Version 0.55.27": it documents the vendor's C library (`htraapi`, `.h`, `.so`/DLL, SWP/IQS/DET/RTA modes, `IQS_GetIQStream`), not a wire protocol. A vendor library may not be downloaded or linked. UNBLOCK: a USB/network protocol description from Harogic, or the owner's decision to allow the vendor library.

**WiNRADiO (G31DDC, G33DDC, G39DDC).** Looked for: https://winradio.com/home/developer-g33ddc.htm: the SDK is a 32-bit DLL (`G33DDCAPI.dll`) with a header and an HTML specification of the DLL calls; no wire or USB protocol. UNBLOCK: a protocol description, or the owner's decision to allow the vendor DLL (out-of-process, like the ExtIO question).

**AntSDR (E200, E310, E316).** Nothing built on purpose. The firmware repository says it is "forked from analogdevicesinc/plutosdr-fw" and the E200 guide uses `iio_info -u "ip:192.168.1.10"`; the existing `PlutoSource` (IIOD, TCP 30431, limits read from the board) takes `uri=` or `host=`. UNVERIFIED: that the IIO device names match a Pluto's (no `iio_info` output is in the documents read) and that the UHD-firmware boot mode is not meant (the guide: UHD firmware boots from the SD card, iio firmware from qspi). Wiring hand-off: offer "AntSDR (IIO firmware)" with the default address 192.168.1.10 in the Pluto source's rows; no new class.

## Tests (real result lines)

`D:/foxsdr-build/feat-radios/tests/Release/<name>.exe`, Release:

| Test | Result line | Repeats |
|---|---|---|
| test_ascp_source | `test_ascp_source: 322519 checks, 0 failed` | 14 consecutive full runs green after the last change |
| test_hpsdr1_source | `test_hpsdr1_source: 845333 checks, 0 failed` | 8 consecutive green |
| test_aaronia_source | `test_aaronia_source: 25951 checks, 0 failed` | 10 consecutive green |
| test_radio_families_sinks | `test_radio_families_sinks: 26 checks, 0 failed` | ctest 0.55 s |
| test_soapy_remote_source (after the `UdpTransport` header change) | `9088 checks, 0 failed` | 1 |
| test_sdrconnect_source (same) | `298 checks, 0 failed` | 1 |
| test_thread_inventory, test_shutdown_budget, test_shutdown_budget_free, test_health_paths, test_json_dump_policy | Passed (ctest) | json policy first FAILED on `aaronia_protocol.hpp` (two `dump()` without the replace handler), fixed in 953422f2, then Passed |

**What the tests cover.** ASCP: literal bytes printed in Afedri's document (7050000 Hz on channel 2, RF gain 0x64, the main-clock request and its 80 MHz answer, the UDP header 04 84, the rate function's 249350 example), assembler byte by byte, every impossible length, ranges, the four UDP sizes and 24-bit extremes, sequence wrap and loss/late/repeat, six stand-in radios (NetSDR 250 k and 62.5 k, SDR-IP, Cloud-IQ, two Afedri) streaming a tone with noise and an 800 ppm clock error checked to 2.5 Hz (the error is shown to be in the data), refusals (arguments, dead port, unknown name, empty name, SDR-14, silent, junk length, UDP port taken), NAK on optional items and on gain, silent radio fault, dropped control connection, lost/repeated/bad datagrams, garbage on the control connection, overload status, 50 start/stop and 50 open/close cycles with no abandoned reader, rate changes between starts, fuzz of assembler, datagram parser and range parser. HPSDR1: the document's padding table against 504/(6n+2), literal command frames, decode for 1 to 8 receivers against an independent extraction, fuzz (MOX never set, sync always), five stand-in radios (generic 48/192/384, Red Pitaya 96/192) with a tone, noise and a 700 ppm clock error, settings reach the radio before Start, retune while running, refresh rate, no MOX/audio/drive ever sent, refusals, silent radio, lost packets, frames without sync, overload flags, 50 + 50 cycles. Aaronia: HTTP head literals and refusals, chunk decoder (byte by byte, extensions, trailer, every refusal), records (JSON and raw, braces and escaped quotes in strings, split at every byte, non-I/Q skipped, seven refusals with nothing believed afterwards, an unbounded header), request bodies, fuzz, a stand-in HTTP server in JSON and float32 formats (tone with noise and a 1500 ppm clock error found where the measured rate puts it, capture-then-start order, retune and stop requests, a second start), refusals (dead port, 401, 404, HTML, no name, an SSH banner, control 500), eight stream faults, gaps/other packets/NaN, 30 + 30 cycles.

### Break-it table (change the named line, see RED, restore, GREEN; script `D:/foxsdr-build/ur-breakit.sh`)

| Part | Removed or changed | Result with it changed |
|---|---|---|
| ASCP | the datagram length check in `parseUdpPacket` (`(h & 0x1FFF) != n`) | 2 failed (the UDP-size and bad-header tests), restored: 322519/0 |
| HPSDR1 | I and Q swapped in `decodeRxFrame` | the literal-decode tests red (`out[0][0].real()=-1 vs 0.99999988`), restored: 845333/0 |
| Aaronia | the odd-sample-count refusal in `takeHeader` | 2 failed (the refusal test and "nothing believed afterwards"), restored: 25951/0 |
| Sinks | a `std::fprintf(stderr, serial_...)` added to `ascp_source.cpp` | 1 failed (`v.empty()`), restored: 26/0 |

## Found while testing (kept for the next lane)

* **The inherited `test_hpsdr_source` is red on this machine under load, with or without this lane's changes** (checked with the `net_sample_source` change reverted and the new sources moved away: 25, 44, 52 failed; with them: 22, 16, 15, 25 failed; failures at the 50-cycle open loop, `opened`/`t.start()`). The previous lane recorded 1855/0. Not caused here; not investigated further (the fix is probably in its discovery window of 500 ms against a thread that polls). Re-run it alone when the machine is quiet.
* **Windows ephemeral ports.** Loopback tests that need one number for a TCP listener and a UDP socket must pick the number at random: Windows reserves blocks of about 100 ports (`netsh int ipv4 show excludedportrange protocol=tcp` lists them) and the OS's own sequential choice walks into one block and fails 60 times in a row (WSAEACCES 10013). Also `srctest::waitRead` blocks until the socket is readable or the flag drops (it never times out): a loop that must do other work needs its own `select`. And `WSAStartup` must be called before the first socket of a test process.
* **A loopback stand-in cannot test "UDP port = TCP port" with its own socket on that number** (the radio's more specific bind wins the client's datagrams), hence `udpport=0` in the stand-in tests and one test that checks the default binding alone.
* A fast start/stop loop outruns a one-thread stand-in by many commands; the cycle tests keep it in step by waiting for its counters.
* `tests/src_test_net.hpp`: `TcpServer::start` takes an optional port (default 0, unchanged).
* Files here are LF in the index and the working tree for new files; the older files of this tree are CRLF in the working tree (a Python replace with `\n` patterns silently fails on them: use the Edit tool or normalise).

## UNVERIFIED (everything not run)

* Every source against a real radio or server: NetSDR, SDR-IP, Cloud-IQ, Afedri, an HPSDR radio, a Red Pitaya, an RTSA-Suite server.
* The full headless suite (`ctest -L headless`, 767 tests) was NOT run: only the tests above and the scans that read the new files were built here. The inherited test_hpsdr_source is red as described.
* The two lines of the document's behaviour each source assumed (see each part): the ASCP UDP destination, the HPSDR refresh rate and duplex, the Aaronia raw-record delimiting and rate control.
* That `AscpSource` copes with an Afedri that answers "SDR-IP" (Emulation ID on): it accepts the name, which is then treated as an SDR-IP and uses its rate table (80 MHz clock), not the Afedri real-rate path.
* Authorisation for Aaronia; the HPSDR bandscope and second receiver; Afedri dual channel.

## Decisions for the owner

1. **RFspace interface specification** (NetSDR/SDR-IP, on request): e-mail info@rfspace.com for it? It would replace CuteSDR's behaviour as the source for the tables, the UDP destination rule and Cloud-IQ. Same ask removes the "UNVERIFIED" from part 1. (This lane may not contact outside services.)
2. **Vendor source with no licence stated** (FUNcube's `FCHid2.002` frequency-control program, from the manufacturer's own page): may it be read for the HID command list? Without it FUNcube tuning stays blocked.
3. **MPL-2.0 source** (RFNM's `librfnm`): may it be read? The clean-room rule lists Apache-2.0, MIT, BSD, Boost and public domain only.
4. **Icom's agreement** for the I/Q Output Reference Guide (IC-R8600): the owner reads and accepts, or supplies a copy.
5. **Vendor libraries / DLLs** (Harogic `htraapi`, WiNRADiO `G33DDCAPI.dll`, Perseus `libperseus-sdr`): all stay blocked unless an out-of-process vendor-library host is accepted (the same decision as ExtIO, survey R8).
6. **Aaronia credentials**: where does FoxSDR keep a server user name and password or an RToken (never in settings, never logged)? Until decided, only unauthenticated server blocks work.
7. **Red Pitaya numbers** (48/96/192 kSPS, 0 to 61.44 MHz) come from Pavel Demin's page, not Red Pitaya's: acceptable for the profile, or wait for a Red Pitaya statement?
8. **KrakenSDR**: is the multi-radio route (five RTL-SDR radios plus the DF lane's calibration) the intended one, with no Heimdall client?
