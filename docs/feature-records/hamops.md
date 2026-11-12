# hamops lane record (ham-operations models and protocol clients)

Branch `feat/hamops`, created from `engine/multi-radio` d3630d78. Worktree `D:\foxsdr-wt\en-hamops`, build folder `D:/foxsdr-build/en-hamops`
(`-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`, other arguments from `feat-web`). Nothing pushed, nothing merged, no version changed (none).
New-file lane: the only existing file edited is `tests/CMakeLists.txt` (one block at its end, giving the hamops tests the repository root for their
source scans). No `engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`, recorder, `cat_protocol.cpp`, `src/input/*`
or `.slint` file was touched (`git diff --name-only d3630d78..HEAD` lists only `src/core/`, `src/net/`, `src/input_hid/`, `tests/`, `tools/`, `docs/`).
Nothing is wired into the engine or a screen: every piece is a model, a protocol client against a stand-in, or a parser. The wiring and screen hand-off is
section 7.

## 0. Step 0 table (read first; reading only)

Checked by `git grep` over `feat/core-formats`, `feat/signal-blocks` (1..4), `feat/net-outputs`, `feat/net-map-models`, `feat/knob-input`, `feat/unblocked-desk`,
`feat/recplay`, `feat/radio-families`, `feat/rfmon`, `feat/webmodels`, `feat/tuning` and `engine/multi-radio` for: cabrillo, omnirig, flrig, Genius, xplane,
band map, report descriptor, whisper, transcript. Result: no branch has a Cabrillo writer, an OmniRig/FLRig/HRD/DXLab client, Antenna Genius, X-Plane,
a band map, a HID descriptor parser or a transcript store. Two files merely mention a rig program: `feat/unblocked-desk` `src/net/cat_emulation.hpp` (a
research comment naming OmniRig) and `feat/radio-families` `src/source/rig_cat.hpp` (a comment saying flrig was not read). `feat/signal-blocks` has
an ADIF 3.1.8 writer for SWL listening reports (`src/core/adif.hpp`, not in this base, a different record shape), `feat/core-formats` has a decode
database, alert rules and a named-command-sequence macro model (not the text-expansion macros asked for here).

| Row | Verdict | What |
|---|---|---|
| Cabrillo export | new file model | `qso_log.hpp`, `cabrillo.hpp`; six contest tables (CQ WW, CQ WPX, ARRL SS, CW and SSB) from opened pages |
| Contest macros | new file model | `contest_macros.hpp`: text only (the `core-formats` macro model sends command sequences: different) |
| ADIF broadcast | new file model | `adif_qso.hpp` (QSO record), `net/adif_broadcast.hpp` (UDP/TCP, 127.0.0.1, off by default); the SWL writer on `signal-blocks` is not in this base, the broadcaster sends any ADI record text so it works with both |
| Upload to Club Log, QRZ Logbook, eQSL | not built (decision) | needs accounts: owner decision recorded in the brief |
| FLRig client | blocked | XML-RPC method list documented only in third-party text and GPL material (section 5) |
| OmniRig client | new file model | `omnirig_client.hpp` over an abstract port; MIT-licensed type library from the maker; the COM bridge is a hand-off |
| HRD client | blocked | no maker document for the protocol on any page opened (section 5) |
| DXLab Commander client | partial | READ side built (`radioinfo_udp.hpp`: the RadioInfo UDP broadcast, documented on the N1MM page and named on Commander's help); TCP command set not documented on any page that opened: not built |
| Multiplier and band-map window | new file model | `contest_score.hpp`, `band_map.hpp`; the window is a screen lane, spot input from the `dx-spots` plugin is a hand-off |
| Flight-sim traffic (MSFS, X-Plane) | partial | X-Plane UDP receive side built (`xplane_udp.hpp`, `net/xplane_listener.hpp`); MSFS needs the SDK (a download): not built |
| Station accessories | partial | Antenna Genius built (`antenna_genius.hpp`, `net/antenna_genius_tcp.hpp`); UltraBeam: no protocol document found (section 5); Power/Tuner/Station Genius deliberately left out (could reach a transmitter) |
| Generic HID button boxes | new file model | `input_hid/hid_descriptor.hpp`, `hid_map.hpp`; event struct copied by value from the knob lane; the join is a hand-off |
| Speech to text of received voice | not built (decision) + record model | `transcript_store.hpp` only; engine and model are downloads (section 5) |
| SSTV transmit, test patterns, repeater mode; VARA chat, beacons, file and mail | not built | transmit, not this lane's (VARA: a closed modem, per the brief; its page was not opened) |

## 1. What was built

All files are header-only (no `.cpp`, so `cascade_lib` and the thread inventory under `src/` are untouched; no `std::thread` anywhere in what this lane added).

| File | What |
|---|---|
| `src/core/qso_log.hpp` | `Qso`, UTC split (Hinnant civil_from_days), callsign check (A-Z 0-9 /), Cabrillo mode code, ADIF band table |
| `src/core/cabrillo.hpp` | Cabrillo v3 header (value lists validated), QSO lines from per-contest field tables, chronological, truncation, CR LF, file writer for a user-chosen path |
| `src/core/contest_macros.hpp` | `ContestSession`: F1..F12 templates, `{MYCALL} {CALL} {NR} {NR:1..5} {RST} {X:KEY}`, serial number that only moves on `logged()`, `sentTokens()` for the Cabrillo QSO. Text only |
| `src/core/adif_qso.hpp` | ADIF 3.1.4 QSO record and header |
| `src/net/loopback_sock.hpp` | non-blocking IPv4 helpers, no thread, no name resolution |
| `src/net/adif_broadcast.hpp` | one record per UDP datagram and/or TCP listener clients; off by default; 127.x only unless switched; bounded |
| `src/core/contest_score.hpp` | parameterised scoring (dupes per band / band+mode / contest, multipliers per band or once), two EXAMPLE rule sets (not sponsor rules) |
| `src/core/band_map.hpp` | spot store: merge, age-out, bounded size, worked/needed flags, stack in frequency order, next-up/next-down |
| `src/core/omnirig_client.hpp` | OmniRig client model: reads and two writes (frequency, mode); no keying call exists |
| `src/core/radioinfo_udp.hpp`, `src/net/radioinfo_listener.hpp` | RadioInfo XML reader (DXLab Commander, N1MM+), per-radio table, loopback listener |
| `src/core/antenna_genius.hpp`, `src/net/antenna_genius_tcp.hpp` | Antenna Genius line protocol, parsers, client, loopback-capable TCP transport |
| `src/core/xplane_udp.hpp`, `src/net/xplane_listener.hpp` | X-Plane DATA/RREF parser, bound track store, loopback listener |
| `src/input_hid/hid_descriptor.hpp`, `hid_map.hpp` | HID report descriptor parser, report decoder, device model, mapping file and mapper |
| `src/core/transcript_store.hpp` | transcript record model |
| `tests/hamops_util.hpp`, `tests/test_*.cpp` (14 files) | tests |
| `tools/hamops_mutation_check.py` | the break-it runner |

## 2. Tests (real result lines)

Own tests, from `ctest -C Release -R "^(test_cabrillo|...)$" -V` on the final tree (every one `0 failed`):

```
test_adif_broadcast:     5133 checks, 0 failed     (loopback UDP/TCP stand-ins, 500-record order run, slow client, fuzz)
test_adif_qso:          31981 checks, 0 failed
test_antenna_genius:    62137 checks, 0 failed     (scripted transport, real 127.0.0.1 TCP stand-in device, fuzz 30000 lines)
test_band_map:         110756 checks, 0 failed
test_cabrillo:           5276 checks, 0 failed
test_contest_macros:    20131 checks, 0 failed
test_contest_score:     81180 checks, 0 failed     (fuzz against a reference model)
test_hamops_scan:        2057 checks, 0 failed     (all 18 src files: no stdout/stderr, no thread, no wide bind, no engine include, dump() policy)
test_hid_report:       138628 checks, 0 failed     (HID 1.11 example descriptors, fuzz 20000 descriptors + 5000 mapping texts)
test_omnirig_client:    52248 checks, 0 failed     (compile-time no-keying-member concepts, 20000-step randomised run)
test_radioinfo_udp:     43080 checks, 0 failed     (hostile XML, fuzz 40000 documents, loopback listener)
test_transcript_store:  35643 checks, 0 failed
test_xplane_udp:       174940 checks, 0 failed     (byte-exact layouts, fuzz 30000 datagrams, loopback listener)
```

(`test_hamops_scan` was 2056 before the dump() check was added; `test_hid_report` was re-run after the `saveMapping` fix: 138628.) Thirteen test programs, about 760,000 checks.
Fuzz runs are deterministic (a fixed seed in `hamops_util.hpp`), so a red is reproducible. No address-sanitizer binary was built or run, so the sanitizer DLL rule did not apply.


## 3. Break-it table

`python tools/hamops_mutation_check.py D:/foxsdr-build/en-hamops`: for each row one behaviour is broken in the source, the test rebuilt and run (RED expected), the
file restored with `git checkout` (new timestamp, so MSBuild rebuilds it), rebuilt and run again (GREEN expected). Output of the three runs of this session:

| # | Feature | Behaviour removed or broken | RED in | Restored |
|---|---|---|---|---|
| 1 | Cabrillo | QSO lines no longer sorted chronologically | `test_cabrillo` (test) | GREEN |
| 2 | Contest macros | `logged()` no longer moves the serial number | `test_contest_macros` (test) | GREEN |
| 3 | ADIF QSO record | every field length is one too long | `test_adif_qso` (test) | GREEN |
| 4 | ADIF broadcast | non-loopback addresses accepted without the switch | `test_adif_broadcast` (test) | GREEN |
| 5 | Contest score | dupe check disabled | `test_contest_score` (test) | GREEN |
| 6 | Band map | age-out boundary `>` changed to `>=` | `test_band_map` (test) | GREEN |
| 7 | HID parser | sign extension of signed fields removed | `test_hid_report` (test) | GREEN |
| 8 | HID mapping | transmit/ptt refusal at load removed | `test_hid_report` (test) | GREEN |
| 9 | X-Plane source | latitude/longitude range check removed | `test_xplane_udp` (test) | GREEN |
| 10 | OmniRig, no transmit | refusal to change frequency while the rig transmits removed | `test_omnirig_client` (test) | GREEN |
| 11 | OmniRig, no transmit | a keying member (`putTx`) added to the port | `test_omnirig_client` (COMPILE: the `static_assert` on the concepts) | GREEN |
| 12 | RadioInfo reader | numeric character references accepted | `test_radioinfo_udp` (test) | GREEN |
| 13 | Antenna Genius | refusal to switch while the port transmits removed | `test_antenna_genius` (test) | GREEN |
| 14 | Transcript store | words bound removed from eviction | `test_transcript_store` (test) | GREEN |
| 15 | Source scan | a `fputs(..., stderr)` function added to `qso_log.hpp` | `test_hamops_scan` (test) | GREEN |
| 16 | X-Plane listener (receive only) | a send added to the listener | `test_xplane_udp` (test) | GREEN |
| 17 | dump() policy | the bare `j.dump(2)` put back in `saveMapping` | `test_hamops_scan` (test) | GREEN |

Row 17 is the regression the full headless gate found (section 8). Output lines of the runs, as printed (`name target red=True (test|compile) restored-green=True`) were all `red=True ... restored-green=True`.

Not run as a mutation: the fuzz runs (they hold invariants rather than behaviours; they are the check for every parser of outside bytes: HID descriptors 20000
mutated/random descriptors + random reports, mapping texts 5000, X-Plane datagrams 30000, RadioInfo documents 40000, Antenna Genius 30000 lines + random bytes,
macro templates 20000, Cabrillo 3000, ADIF 20000). The RadioInfo "<!" refusal is redundant with the element-name check (both refuse the same inputs), so
removing only that line is not visible; the character-reference refusal was mutated instead.

## 4. UNVERIFIED

- Nothing was run against a real device, simulator, logger or COM server (none is on this desk, and the brief forbids opening any): FLRig, OmniRig, DXLab
  Commander, N1MM+, Antenna Genius, X-Plane, a HID button box. Every client is proven against stand-ins built from the maker's documents.
- Antenna Genius: the line terminator the device uses on replies (the reader accepts CR, LF, CR LF); whether `port get` is also followed by an empty OK line (the
  client completes on the first line and counts a later empty one as unmatched); `ping`, `info`, `unsub` and the `sub` forms for antenna/output statuses (those
  pages were not opened); the maximum antenna number ("1 to xx" on the page; 16 assumed as a bound).
- RadioInfo from DXLab Commander: Commander's page names a RadioInfo message and the N1MM-compatible port 12060 but gives no wire format; it is taken to be N1MM's
  XML. UNVERIFIED against a real Commander. The 10 Hz unit is from the N1MM page.
- X-Plane: no opened page says which DATA index or which dataref is latitude/longitude/altitude/heading, so none is built in (the binding is data); the DATA
  prologue's 5th byte is accepted as any value; the datagram layouts are from a community documentation site (xppython3, Peter Buckner, licence not stated) and a
  second copy of the DATA description (nuclearprojects.com, author and licence not stated); Laminar's own "Exchanging Data with X-Plane" file is in an X-Plane
  install and was not readable.
- ADIF: `CONTEST_ID` is written as the Cabrillo contest id; the ADIF contest enumeration was not read, so it is UNVERIFIED that every id here is an enumeration
  value. The ADIF specification does not define a UDP or TCP framing: the framing in `adif_broadcast.hpp` is this lane's own (stated in the header).
- Cabrillo: the sponsors' own rules pages (as opposed to the Cabrillo templates they publish) were not opened for the exchange definitions: the exchange
  tokens of CQ WW (RST, CQ zone), CQ WPX (RST, serial), ARRL SS (serial, precedence, check, section) follow the sample lines on the opened pages. The CQ WW page does
  not say the second exchange field is the CQ zone (its samples show a two-digit value); this table treats it as a number. The notes page says ADDRESS up to four
  lines, the header page up to six: the writer accepts six. CATEGORY-STATION values for the ARRL SS template page (CLASS-I, CLASS-C on the School Club Roundup page)
  are not in the header page's list and are refused by the validator.
- Contest scoring: the example rule sets are not any sponsor's rules (no scoring page opened). The N1MM band-map window semantics were not opened.
- Cut numbers in contest macros (0 -> T, 9 -> N ...) are not built: no opened source states the mapping.
- HID: the real devices' descriptors were not available; the test descriptors are the HID 1.11 examples assembled by hand (boot mouse B.2, joystick D.1 with its typo
  `Usage Minimum (Button 4)` read as Usage Maximum, boot keyboard) and one button box of this lane's own.
- Windows: the loopback sockets were run on this PC; no Windows prompt was raised by this lane's tests (every bind is 127.0.0.1; nothing touches a certificate
  store, a key store, a file dialog or the firewall), but the OS prompt behaviour was not itself observed on the visible desktop.

## 5. Not built, blocked, owner decisions

**FLRig (XML-RPC client) - BLOCKED, free-source check done.** Looked for a non-GPL description: the FLRig manual index on w1hkj.org (opened) documents no XML-RPC interface
and states no licence; the `xmlrpc.html` page there does not exist (404); searches found the method names (`rig.get_vfo`, `rig.get_mode`, `rig.get_ptt`, `rig.set_ptt`,
`rig.client_string`, `rig.get_xcvr`, `system.listMethods`) only in third-party snippets and in GPL source (fldigi), which are closed to this lane. XML-RPC itself is an open
format, but the method list is the interface and it is not documented outside those. Alternatives for the user: OmniRig (built here) reaches the same rigs; FoxSDR's own rigctld
server exists. Decision for the owner: ask the FLRig maintainer (W1HKJ) for a statement that the method list may be implemented, or accept no FLRig client.

**HRD (Ham Radio Deluxe) - BLOCKED.** HRD's own support pages (found by search) name an internal TCP/IP connection on port 7809 between its modules and a Remote Server on
7805 for HRD-to-HRD use; none describes a message format. Alternatives: OmniRig client (built) and the rigctld server work without HRD. Decision: ask HRD support for the
interface description.

**DXLab Commander TCP commands - NOT BUILT.** The maker's help page (`dxlabsuite.com/commander/Help/NetworkService.htm`, opened) gives the ports (base 52000, commands on base + 2) and
the UDP RadioInfo service, but not the command list or framing. A PDF named "Commander TCP/IP messages" exists but only as a copy inside a GPL project's repository (`repo.radio`
mirror of WSJT-X), which asks for a sign-in and is closed to this lane. The READ side (RadioInfo UDP) is built. Decision: obtain the document from DXLab (Dave AA6YQ) or leave
Commander read-only.

**UltraBeam - NOT BUILT.** The RCU-06 manual (WiMo copy) returned HTTP 403; a search found no protocol document. Decision: ask UltraBeam (or WiMo) for the RCU-06 remote-control
description.

**Power Genius, Tuner Genius, Station Genius - deliberately not built** (a command to an amplifier or tuner could reach a transmitter). Antenna Genius commands left out: reboot,
stack, network, conf, auth, flex, group, output, band set/init, antenna set.

**Speech to text of received voice - NOT BUILT (owner decision), record model built.** The engine and a model are downloads, which the standing rules forbid. Alternatives named from
pages opened on 2026-11-11: whisper.cpp (https://github.com/ggml-org/whisper.cpp): MIT licence, plain C/C++ without dependencies, models downloaded as single ggml files (tiny 75 MiB,
base 142 MiB, small 466 MiB on disk), 16-bit WAV in, an experimental `--print-colors` option for low/high-confidence words (whether numeric per-word probabilities are exposed was not
settled by the page); OpenAI Whisper (https://github.com/openai/whisper): "code and model weights are released under the MIT License", multilingual tiny/base/small can translate to English
(English-only variants cannot). No patent question arises from those two pages. Vosk (https://alphacephei.com/vosk/): offline, about 50 MB per language, 30+ languages, but its page
states no licence and no per-word confidence: UNVERIFIED, not a recommendation until its licence page is read. Decision for the owner: allow a one-time, user-initiated download of
whisper.cpp and a model by the user (the application would not download it), or none. Built meanwhile: `transcript_store.hpp` (timestamped text, confidence per word, confidence class for
colouring, bounded, in memory only).

**Uploads to Club Log, QRZ Logbook, eQSL** - need accounts: owner decision recorded in the brief, not built. The ADIF broadcast (built) lets a user's own logger do the upload.

**SSTV transmit, test patterns, repeater mode; VARA keyboard chat, beacons, file and mail over RF** - not this lane's and not built: all transmit; VARA is described in the brief as a closed
modem (not looked up here).

**MSFS** - SimConnect needs the Microsoft SDK, a download: not built. **ForeFlight XGPS/XATT/XTRAFFIC** simulator protocol: the maker's page returned HTTP 403: not built.

## 6. Sources opened (2026-11-11) and their licence or status

| URL | Used for | Licence / status |
|---|---|---|
| https://wwrof.org/cabrillo/cabrillo-v3-header/ | Cabrillo header tags, value lists, limits | WWROF; no licence stated |
| https://wwrof.org/cabrillo/cabrillo-qso-data/ | generic QSO: line, mode codes, field rules | WWROF; no licence stated |
| https://wwrof.org/cabrillo/cabrillo-specification-notes/ | line endings, truncation, always-present tags | WWROF; no licence stated |
| https://wwrof.org/cabrillo/ | index of the specification pages | WWROF |
| https://www.cqww.com/cabrillo.htm | CQ WW QSO line, categories | CQ Magazine; no licence stated |
| https://cqwpx.com/cabrillo.htm | CQ WPX QSO line and samples | CQ Magazine; no licence stated |
| https://www.arrl.org/cabrillo-format-tutorial | ARRL November Sweepstakes template and samples | ARRL; no licence stated |
| https://adif.org/314/ADIF_314.htm | ADI syntax, field definitions, band enumeration | ADIF specification, freely published; no licence text read |
| https://www.dxatlas.com/OmniRig/ | OmniRig is freeware, documents' list | Afreet Software, FREEWARE |
| https://github.com/VE3NEA/OmniRig and raw `OmniRig.ridl`, `LICENSE` | IRigX/IPortBits members and RigParamX values | MIT, (c) 2024 Alex Shovkoplyas |
| https://n1mmwp.hamdocs.com/appendices/external-udp-broadcasts/ | RadioInfo XML and units, other broadcast types | N1MM Logger+ docs; "all rights reserved" footer only |
| https://dxlabsuite.com/commander/Help/NetworkService.htm, `.../index.htm` | Commander ports, UDP RadioInfo, no TCP list | DXLab; no licence stated |
| https://github.com/4o3a/genius-api-docs/wiki (pages: Antenna-Genius-TCPIP-API, -port, -band, -antenna, -sub, Antenna-Genius-Status-Messages) | Antenna Genius API | 4O3A; no licence stated |
| https://www.usb.org/sites/default/files/hid1_11.pdf (read as text) | HID item tables, sign rules, examples | USB-IF specification, free to read |
| https://xppython3.readthedocs.io/en/latest/development/udp/rref.html, `.../data.html` | RREF and DATA layouts | copyright Peter Buckner, no licence stated |
| https://nuclearprojects.com/xplane/xplaneref.html | DATA layout (second description) | author and licence not stated |
| https://www.x-plane.com/kb/x-plane-10-40-release-notes/ | RREF and RPOS exist, port 49000 | Laminar Research notes; no layouts |
| https://github.com/ggml-org/whisper.cpp, https://github.com/openai/whisper, https://alphacephei.com/vosk/ | STT alternatives | MIT, MIT, not stated |
| https://www.w1hkj.org/flrig-help/index.html (and `xmlrpc.html`: 404) | FLRig: no XML-RPC documentation | no licence statement on the page |

Opened but unusable: https://support.foreflight.com/... (403), https://www.wimo.com/.../ultrabeam_RCU06_EN_9c9e.pdf (403), https://repo.radio/w4kek/WSJT-X/.../Commander TCPIP Mesages.pdf (sign-in page;
the repository is GPL: closed anyway), https://wwrof.org/cabrillo/cabrillo-qso-templates/ (404), https://developer.x-plane.com/article/exchanging-data/ (404).
Web searches returned snippets used only to find which pages to open (nothing was taken from a snippet). No GPL, LGPL or AGPL source was read; no software was downloaded; the HID
specification PDF was saved by the page-reading tool into the session's own folder and read as text.

## 7. Wiring and screen hand-off

Every control: English label, op or fact, range, default. No translated strings were written; these are the English strings.

**Contest log (a screen lane):**
- "Contest" - one of CQ-WW-CW, CQ-WW-SSB, CQ-WPX-CW, CQ-WPX-SSB, ARRL-SS-CW, ARRL-SS-SSB (`cabrilloContests()`); default none.
- "Your callsign" - A-Z 0-9 /, 1-32 characters; "Location" (ARRL section, state or DX, at most 20 characters).
- "Operator category" - SINGLE-OP, MULTI-OP, CHECKLOG (default SINGLE-OP); "Assisted" - ASSISTED, NON-ASSISTED; "Band" - ALL, 160M, 80M, 40M, 20M, 15M, 10M, 6M, 4M, 2M, 222, 432, 902, 1.2G,
  2.3G, 3.4G, 5.7G, 10G, 24G, 47G, 75G, 122G, 134G, 241G, Light, VHF-3-BAND, VHF-FM-ONLY (default ALL); "Mode" - CW, DIGI, FM, RTTY, SSB, MIXED; "Power" - HIGH, LOW, QRP; "Station" - DISTRIBUTED,
  FIXED, MOBILE, PORTABLE, ROVER, ROVER-LIMITED, ROVER-UNLIMITED, EXPEDITION, HQ, SCHOOL, EXPLORER; "Time" - 6-HOURS, 8-HOURS, 12-HOURS, 24-HOURS; "Transmitter" - ONE, TWO, LIMITED, UNLIMITED, SWL;
  "Overlay" - CLASSIC, ROOKIE, TB-WIRES, YOUTH, NOVICE-TECH, YL. All optional.
- "Claimed score" - integer 0.. (blank = omitted); "Club", "Name" (at most 75), "Address" (up to 6 lines of at most 45), "Operators" (call list; the host call prefixed @), "Soapbox" (lines of at most 66),
  "Email", "Grid locator".
- "Export Cabrillo log..." - a user action: `writeCabrillo(header, qsos)` then `writeCabrilloFile(path, text)` at the path from the file dialog; the result's `problems` list is shown as sentences;
  `skipped` is the number of QSOs left out.
- "Macro F1".."Macro F12" - text up to 256 characters; tokens `{MYCALL} {CALL} {NR} {NR:1}..{NR:5} {RST} {X:KEY}`, `{{` and `}}` for braces. Expansion is text only: what a screen does with
  the text (show it, copy it, type it into the log) is the screen's call; there is no transmit path in the model.
- "Next serial number" - 1..99999, default 1; "Exchange values" ZONE, SECT, PREC, CHECK (letters and digits, at most 12); "Default RST" - 2 or 3 characters, default 599 (CW contests) or 59.
- "Score" panel (read-only): QSOs, dupes, points, multipliers (and per band), score. Rule set: the two examples or a rule set built from a sponsor's page (`ScoreRules`).
- "Band map": "Spot age limit (minutes)" 1.. default 30; "Maximum spots" 1..100000 default 200; "Merge spots within (Hz)" 0.. default 500; "Hide worked spots" default off; buttons "Next spot up",
  "Next spot down", "Needed spots only", "Mark worked". Spot input: whoever has a spot (the `dx-spots` plugin) calls `BandMap::addSpot(call, Hz, mode, nowUnix)`.

**ADIF broadcast (settings page, engine-owned):** "Broadcast logged QSOs (ADIF)" default off; "Address" default 127.0.0.1 (dotted IPv4); "UDP port" 0 = none, 1..65535; "TCP listener port"
blank = none, 0 = a free one, 1..65535; "Allow addresses other than this computer" default off. Op: `AdifBroadcaster::start(cfg)`, then `publish(record)` per logged QSO from a worker thread (`pump()` accepts
TCP clients). Facts to show: clients connected, records sent, records refused. The addresses and call data are never written to a setting by this code; the wiring lane decides which of the toggles
the settings file keeps.

**OmniRig (engine-owned bridge, Windows):** implement `OmniRigRigPort` over `IRigX` of "OmniRig.OmniRigX" (Rig1/Rig2) with late binding: reads Status, Freq, FreqA, FreqB, Mode, Vfo, Split, Tx; writes
Freq and Mode only. Do not implement a Tx setter, SendCustomCommand or PortBits (the model has no such member; the test fails the build if one is added to the port). Controls: "Rig" 1 or 2; read-outs
"Frequency", "Mode", "Rig status", "Transmitting" (read-only); user actions "Set frequency", "Set mode". CoCreateInstance must pass no UI-bearing flag and never run in a lane test.

**RadioInfo (DXLab Commander, N1MM+):** "Follow radio info from DXLab Commander or N1MM+" default off; "Address" 127.0.0.1; "Port" (the page suggests 12060; no default is picked; 0 = any); "Radio" 1..4.
Op: `RadioInfoListener::start`, `poll(table, nowUnix)` on a worker; read `RadioInfoTable::get(radio, now, info)` (stale after 30 s), `effectiveTxHz()` for the transmit frequency.

**Antenna Genius:** "Antenna Genius address" (empty; user enters it), "Port" 9007, "Allow a device on the network" default off (the real device is on the LAN, so the wiring lane must show this switch);
read-outs per port: "Band", "Receive antenna", "Transmit antenna", "Transmitting", "Inhibited"; antenna names and band table from the lists; user action "Select antenna" for port A or B (receive and
transmit antenna 0..16, 0 = none) - never automatic, no band-follow in this model: if band-follow is wanted it needs its own owner decision. Op: `AntennaGeniusClient` over `GeniusTcpTransport`.

**X-Plane traffic source:** "Listen for X-Plane traffic" default off; "Address" 127.0.0.1; "Port" (the port X-Plane's data output is pointed at; 0 = any); per aircraft slot (at most 64): "Label" (1..24), sources
for latitude, longitude, altitude, heading, speed: "Kind" DATA (index, column 0..7) or RREF (index), "Scale", "Offset"; "Remove aircraft not heard for (seconds)" default 60; "Maximum aircraft" default 64
(1..1024). The slots can only be filled by someone who has read the dataref list of a real installation (section 4). Output: `FlightTrack` (id, latitude, longitude, altitude in metres, heading, speed in m/s, last
heard) for the map.

**HID button box:** "Button box mapping file" (path; JSON, at most 64 KB); per binding: "Usage page" 1..65535, "Usage" 1..65535, "Action" one of tune, tune.fine, tune.shuttle, volume, squelch, zoom, step.up,
step.down, mode.next, mode.prev, radio.select, command, "Invert" default off, params (`name`, `n`, `divisor`). Op: `parseDescriptor` on the device's report descriptor (read by the Windows HID backend: the knob
lane's `hid_win`), `HidDevice::parseReport` per report, `HidMapper::translate` to `ActionOut`. The join with the knob lane: replace `HidEvent` by its `InputEvent` (identical layout), and feed `ActionOut` to its
mapping consumer; the control ids here start at 0x20 per descriptor and are not its 0x10-based ids. The knob lane's own refusal of transmit/ptt actions is mirrored here at load time.

**Speech-to-text:** nothing to wire until the decision in section 5. `TranscriptStore` is ready for the segments a recogniser would produce.

## 8. Gates

Build: full `cmake --build D:/foxsdr-build/en-hamops --config Release --parallel 3` of every target (573 test programs), exit 0, 0 errors, on the tree before the last fix.
Own tests: 13 of 13 pass (section 2). Break-it: 17 of 17 red then green (section 3).

Headless set (`ctest -C Release -L headless`, 716 tests, `-j 3 --timeout 300`, in seven slices of at most 116 tests, each under 20 minutes on a machine shared with about twenty other
lanes; the slices are `-I 1,100` ... `-I 601,716`). First pass: 31 failures, classified:

* **A real failure of this lane, found and fixed:** `test_json_dump_policy` - `saveMapping` in `hid_map.hpp` called `j.dump(2)` without `error_handler_t::replace` (the policy for all of `src/`).
  Fixed (commit "hid_map.hpp saveMapping passes error_handler_t::replace"), re-run: `scanned 840 files, 35 dump() sites, 0 violation(s)`; `test_hamops_scan` now holds the rule for the lane's own files (mutation row 17).
* **The known false failures of a D: build (brief):** `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`, and `test_window_failed_app` (the Slint-OFF configuration).
* **Known load flakes (brief):** `test_tx_audio` and `test_pass_control` (SEGFAULT, red again alone under load), `test_patch_dest_async_stepped` (green alone).
* **Green when re-run alone (load):** `test_engine_control_thread`, `test_engine_shell_guard` (the fence guard over every `src/net` header, including this lane's five new ones: green), `test_pa_stream_list_guard`,
  `test_remote_key_release_stepped`, `test_recorder_bookmarks_scanner_free`, `test_diag_hang_free`, `test_shutdown_budget_free`, `test_stop_ends_recordings_free`.
* **Still red when re-run alone, in code this lane did not touch (UNVERIFIED at the base: no base build was made for comparison):** `test_engine_api_decode_radio`, `test_engine_api_merge`,
  `test_engine_api_multi_radio`, `test_engine_api_schedule`, `test_engine_api_source_kinds`, `test_engine_api_transmit`, `test_excuse_cap`, `test_failure_files`, `test_plugin_api`, `test_plugin_hostile`,
  `test_radio_setup`, `test_crash_upload_free`, `test_window_threaded_look`. Causes seen in their output: `test_engine_api_source_kinds` says "the fixture scope.wav is not in .../src/slint/tests/drive/fixtures"
  (`*.wav` is git-ignored there and absent from a fresh worktree, so the file-source radios of `decode_radio` and `multi_radio` have no input); `test_engine_api_transmit` prints its own "RERUN (a run outlasted the
  900 ms ...)" lines (the load sensitivity the engine-features lane A record describes: the probe was red under 24 CPU burners at that lane too); `test_engine_api_schedule` could not create a recordings folder
  inside its redirected profile; `test_radio_setup` runs a real probe of the USB bus; `test_plugin_*`, `test_excuse_cap` and `test_crash_upload_free` are timing and fixture-module tests under load.
  Why this lane is not the cause: no existing source file was changed (the diff is new headers, new tests, one block at the end of `tests/CMakeLists.txt`, a tool, this record), the new headers are included by
  nothing in `src/`, so `cascade_engine.lib` and every test linking it are unchanged by this lane; the lane-A record at the base lists 8 failures on a Slint-ON build with fixtures, a different environment.
  This is a claim by construction, not by a base run.

The gate was run on the tree before the one-line fix; after it only `test_hid_report`, `test_hamops_scan` and `test_json_dump_policy` can change and were re-run green (lines above). No window test was run (this lane has none; GUI-labelled
tests need the hidden desktop and were not part of the brief's gate).


## 9. Owner decisions needed

1. Speech to text: allow a user-initiated download of whisper.cpp and a model (section 5), or not.
2. FLRig: ask its maintainer for permission to implement the method list, or no FLRig client.
3. HRD, DXLab Commander TCP, UltraBeam: ask the makers for their interface documents, or leave as is.
4. Band-follow for the Antenna Genius (automatic antenna switching on a band change): not built; a user action only until decided.
5. Uploads to Club Log / QRZ / eQSL need accounts: as in the brief, not built.
6. Which contests beyond the six get a table (each needs its sponsor's Cabrillo page opened).

## 10. config.json

`sha256` of `%APPDATA%\foxsdr\config.json` read at the end of the lane (read-only hash, no program of this lane or its tests touches it; the tests redirect nothing because they open no profile):
`c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` - equal to the value in the standing rules: UNCHANGED.

