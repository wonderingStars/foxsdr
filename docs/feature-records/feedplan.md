# Lane feedplan: test data and checking recipe for feeding every plugin (headless)

Branch `feat/feedplan` (from engine/multi-radio d3630d78). Release checklist section G: "All 43 plugins fed and checked (radio-email cannot be fed)"; the catalogue is now 77 plugins. This lane prepares the test data and the checking recipe the final real-mouse drive will use. It takes no desktop and does no mouse drive. Nothing was pushed or merged. No file outside the new ones below was edited (the `tests/CMakeLists.txt` glob picks the two tests up without an edit).

## What was built

| item | where | what |
|---|---|---|
| feed table | `D:/foxsdr-build/prep/feed/FEED-TABLE.md` (outside any repo) | one row per plugin of `index.json` of the merged plugins tree (77 rows) plus the built modules without a catalogue row and the 13 depth-lane branches that have not merged: what it decodes, tuning, feed file path, setting steps, wait, what a correct render looks like, status, safe-for-published-screenshots, stand-in server recipes. Status counts: FEED READY 37, FEED FILE ONLY 9, NO FEED YET 23, NEEDS A LIVE FEED 7, NOT FEEDABLE 1 (radio-email). |
| generator | `tests/feed/feedgen.hpp`, `tests/feed/feedsignals.hpp` | I/Q PCM16 RIFF writer, USB / AM / NFM modulators (noise floor, carrier-frequency offset, sample-clock error), SHA-256, and eight signal builders |
| generator test | `tests/test_feed_fixtures.cpp` | `--write <dir>` writes the fixtures and `SHA256SUMS`; with no argument (ctest) it builds every fixture twice and checks the WAV form, rate, length, level, identical sha256, the carrier in the spectrum and the noise floor |
| fixtures | made by the tool; list `tests/feed/SHA256SUMS` | `dtmf-decoder`, `ils-meter`, `ccir-selcall`, `psk31-decoder`, `mpt1327-monitor`, `timesig-decoder` (DCF77), `enocean-monitor`, `per-tester` |
| check harness | `tests/test_engine_api_feed_check.cpp` | loads ONE plugin module per row through the real engine (own plugin folder, hermetic profile), opens the I/Q file with `SELECT_SOURCE "file:"`, tunes, waits until the radio reads the row's centre, starts the plugin, and compares the engine's decoder-output backlog with the row's expectation. Expectation language: text, `num:KEY=V~TOL` (the tolerance rule: text exact, numbers with a stated tolerance), `!absence`, `order:a\|b\|c`, `@lines>=N`, `@markers>=N` |
| cases | `tests/feed/feed-cases-drive.tsv` (the 23 synthetic drive-set fixtures + packet-tnc on the APRS fixture), `tests/feed/feed-cases-new.tsv` (the 8 generated) | tab separated: id, module prefix, wav path, centre, VFO offset, mode, bandwidth, timeout, expectation |
| real-recording cases | `D:/foxsdr-build/prep/feed/feed-cases-real.tsv` (LOCAL ONLY, never committed: it names real recordings and their reference values) | 33 rows over the real recordings in `D:/foxsdr-samples` |
| re-run script | `tests/feed/run_feed_checks.ps1` | builds the two tests, runs the self-tests, regenerates the fixtures and compares with `SHA256SUMS`, makes the drive-set fixtures with the repository's own `make_*.py`, copies the newest module of every plugin id from a built plugins folder, runs all cases files, writes `RESULTS.md`; prints the real `config.json` sha256 before and after |
| results | `D:/foxsdr-build/prep/feed/RESULTS.md` | PASS / FAIL per case with the real output fragment and the tuning read back (callsign-, registration- and id-shaped words in the real-recording rows are masked as ##) |

THE GENERATORS AND THE DECODERS SHARE AN AUTHOR. Every builder was ported from the plugin lane's own test generator (written from the same reading of each standard). A PASS proves that the two halves agree and that the plugin runs end to end inside the engine; it does not prove either half right about the real world. Where a real recording exists the table puts it first. Stated in `feedgen.hpp` and in the table.

## Results (real command output)

Plugin modules: the merged build `D:/foxsdr-build/plug-merge/out/Release` (plugins merge round 2, newest version of each id) plus `example-decoder-2.0.0` copied from the installed plugin folder (the merged build has no such module). Engine: this tree, Release, Slint off.

- Generator self-test: `test_feed_fixtures: 58 checks, 0 failed` (8 fixtures, each built twice, same sha256).
- Harness self-test: `selftest ok (0 failed)` (13 checks of the expectation language).
- Generated cases (8): **8 PASS**. The fragments are the plugin's own lines:
  - DTMF: `+0.807 s  1`, `+0.948 s  2` ... all 16 keys in order 1 2 3 A 4 5 6 B 7 8 9 C * 0 # D (an `order:` term).
  - ILS: `ILS LOC  90:150 Hz +4.5 dB  no depth  90 Hz predominates: left of course, fly RIGHT` (built +4.4 dB, tolerance 0.6).
  - CCIR: `+1.365 s  CCIR call 12345`.
  - PSK31: `1500 Hz: CQ CQ DE FOXSDR FIXTURE K`.
  - PER: `PER PN9 9600 baud  bits 57125  errors 40  BER 0.0007  bad packets 40 of 223  PER 17.9 %` (40 bits flipped, tolerance 3: exactly 40).
  - MPT 1327: `MPT 1327 control channel: system code 11213 (0x2BCD)` and the GTC, BCAST and ALH lines.
  - DCF77: `+120 s  DCF77: 2026-11-11 Wed 12:35 CET = 2026-11-11 11:35 UTC` and `all parity bits ok` (the time of the NEXT minute, as the code means it).
  - EnOcean: `EnOcean RORG 0xF6 data 30 sender device 1 status 0xB0 (original), CRC-8 ok (status bit 7 agrees)` and the D5 and A5 lines. A first fixture built with status 0x30 made the plugin say "status bit 7 does not match the hash type": the fixture's status byte was wrong for a CRC-8 telegram, now 0xB0 / 0x80.
- Drive-set cases (23 fixtures + packet-tnc on the APRS fixture): **24 PASS**. packet-tnc: `RX FOXSDR-1>APRS,WIDE1-1 UI pid=F0 len=17: >FOXSDR FIXTURE 1`, `TNC: KISS server listening on 127.0.0.1:8100` (the plugin opens that loopback socket on start).
- Real-recording cases (33): **23 PASS, 10 FAIL**. Some of the 23 PASSes are weak and the table does not call them FEED READY: HFDL (bursts found, every PDU refused), GOES and LRPT (only the plugin's banner and 'waiting for sync' lines matched `@lines>=2`), RTTY 45.45/170 (garbage text, so its real row was dropped from the table's status).

Real-recording rows that PASS with a reference: CW ARRL 13 WPM (text word ANTENNA from the reference text), POCSAG 512/1200/2400 (`THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 1234567890`), 433 MHz BTHR968 (20.9 C, 45 % as in the recording's expected.json), wM-Bus (heat meter header as in expected.json), SELCAL AB-CD, EAS (the five-county header read raw, then NNNN), APT NOAA-19 (sync locked), SSTV Martin 1 (VIS 44), ACARS, ADS-B (35 lines in the first seconds), VOR (a radial and 88 % confidence), DAB (BBC National DAB, BBC Radio 1), VDL2 (a ground-station XID line), HFDL (bursts found, PDUs refused).

Also PASS: FT8 20 m file (two real decodes in 50 s, `CQ <call> <grid>`, the call masked) and the IBOC sample (`IBOC station: KUT`, 2 spectrum markers). Real-recording rows that FAIL (the plugin prints no decode; reported by name, expectation unproven): ERT scm-g001 (only its status line), FLEX sound file (the plugin says the file is 6400/4 and it handles 1600/2 only), AIS sigid, EPIRB epirbsignal, TETRA sigid, DMR 451 MHz, Inmarsat-C sigid, RS41 sigid (found a carrier, no frame), WEFAX Fax120 copy (listening lines only; a first expectation `start tone` had matched the plugin's own banner, a false pass caught and replaced by `phased at`), FT8 15 s WSJT-X sample (no decode in 95 s: the plugin cuts slots on the PC's UTC clock, so a looped 15 s file is rarely aligned to the slot grid). The RTTY 45.45/170 real copy PASSes its weak expectation but the text is garbage (`ZZVIBVWUZMNQDXHSUQHJ...`): a decode is not demonstrated.

## Findings worth the orchestrator's time

1. **FOXAPI_LIST_TRACKS answers UNSUPPORTED**: map tracks, pictures, panels and instruments cannot be checked headless; the table marks each as a by-hand check in the window.
2. **23 plugins have NO FEED YET**: the generator for each exists in the plugin lane's own test (path in the row) and must be ported; the biggest are M17, WSPR, JT65, LoRa, DVB, HRPT, LTE, GSM, BLE, 802.15.4, UAT, FT8 (LDPC), DominoEX, SITOR-A, Loran-C, DGPS, HF modem id, STANAG 4285 id, analogue TV, remote monitor, SYNOP, Feld-Hell (a picture), rf-measurements.
3. **Ten real recordings are not decoded by their plugins** (list above); one of them (FLEX) is a different speed from what the plugin handles, the RTTY copy decodes garbage, and two PASSes caught as false (a banner line matching the expectation) show why every expectation must name a line only a decode can print.
4. The VOR drive-set fixture was built for radial 090 and reads 086..090 (the drive script's own recorded finding); its row therefore expects 88 +- 3. The real SDRangel VOR file reads radial 264 at 88 %.
5. Tuning matters at plugin start: a plugin judges the band from the centre it sees when it starts, so the harness waits for the radio to read the row's centre first. The first full run, without the wait, printed "receiver is at 100.000 MHz" banners from DAB, AIS and the 433 MHz meters; the wait is now in the harness and the window drive should set the centre before START.
6. The merged plugin build has no `example-decoder` module (it has `hostapi-example`), and `nrsc5-radio-dynamic` is built as `iboc-radio`; `airports-navaids`, `drm`, `radar-sweep` and `hostapi-example` are built but not in `index.json`.

## Tests and break-it

| test | result |
|---|---|
| `test_feed_fixtures` (ctest, no arguments) | `test_feed_fixtures: 58 checks, 0 failed` |
| `test_engine_api_feed_check` (ctest, no arguments: the expectation language) | `selftest ok (0 failed)` |
| `run_feed_checks.ps1 -SkipBuild -Only dtmf` | `generated fixtures match tests\feed\SHA256SUMS`, `PASS dtmf-decoder-gen`, `real config.json sha256 after ... (unchanged)` |

Break-it (behaviour removed, red seen, restored, green seen):

| change | red | restored |
|---|---|---|
| the DTMF fixture's data zeroed (a dead signal) | row `dtmf-decoder-CORRUPT` FAIL: only the banner line, `unmet: order:s  1\|s  2...` | the uncorrupted row PASS |
| the DTMF builder's 1633 Hz column changed to 1700 Hz | the checksum differs from `SHA256SUMS` (`b779d7b7...` against `789af0f5...`) and the row FAILs (key D never reads); the generator self-test stays green: it checks form, level, determinism, carrier and floor, NOT tone values | file restored and touched (the stale-object lesson), rebuilt: `SHA256SUMS identical after restore`, row PASS |
| the numeric tolerance of the expectation language removed (`num:` always true) | `FAIL number outside the tolerance is a miss (21.9 vs 21.7 +-0.1)`, `selftest FAILED (1 failed)` | `selftest ok (0 failed)` |

## UNVERIFIED

- Stand-in server recipes (127.0.0.1 tile, TLE, registry, spot, PostgreSQL and SMTP servers) are written in the table but were not run: no network plugin was exercised.
- Rows marked FEED FILE ONLY or NO FEED YET have no headless proof.
- Pictures, map tracks, panels and instruments (every "by hand" item) were not seen.
- The expectations of real-recording rows other than the ones with a reference (CW text, POCSAG text, BTHR968 values, wM-Bus header, SELCAL) are observed baselines of the plugin's own output, not independent truth.
- The drive-set and generated rows ran on the merged plugin build of merge round 2; a later build can change a plugin's wording.
- Windows SmartScreen / firewall prompts: none were raised (headless exes, no listening socket, no certificate or key store touched), but this was observed only through the absence of a hang.

## Refresh list for the orchestrator after plugins merge round 3

1. Rebuild the plugins repo; run `tests/feed/run_feed_checks.ps1 -PluginSrc <new out\Release>` (add `-SkipBuild` when this tree is already built).
2. Read `RESULTS.md`: a FAIL on a drive-set or generated row is a plugin change (wording or behaviour): update the expectation in the cases file in the same commit, or report the plugin.
3. Copy the `example-decoder` module (not in the merge) into the plugin folder, else its row SKIPs.
4. Add a row to `FEED-TABLE.md` and a case for every depth lane that merged (aeroapps, cwskim, metar, clipfsk, fmsfsk, x10rf, ale, foxhunt, meteorping, olivia, rade, gsc, voiceinv, nrf24, sigid, touchstone, clipupload, vsthost, screenrec, imgsave), and for any plugin whose version or id changed (`index.json`).
5. Port the NO FEED YET generators in priority order (the "FED, EXPECTATION UNPROVEN" ones first: FT8, wM-Bus, stdc, HFDL, DMR, TETRA, VDL2, sonde, DAB; then the new ones), add the `SHA256SUMS` lines in the same commit.
6. Regenerate the table: `python -I D:/foxsdr-build/prep/feed/make_table.py` after `make_results.py` (both outside the repo; they read the results TSVs).

## Wiring and screen hand-off

None: no engine, window or plugin ABI change and no new control. English strings added: none (all text is in this record, the table and the tests).

## Sources opened

Read-only local files: `D:/foxsdr-build/prep/mouse-drive-plan.md` section 1, `D:/foxsdr-samples/MANIFEST.md` and `INVENTORY.md`, the sample folders' `expected.json` / `SOURCE.txt` / `values.txt`, `src/slint/tests/drive/fixtures/make_*.py` and `fixture-*.txt`, `tests/test_engine_api_decode_radio.cpp` and `test_engine_api_plugin_smoke_radio.cpp`, `third_party/foxsdr_api/foxsdr_api.h`, the plugin lanes' own tests `src/{dtmf,ccir,psk31,ils,per,mpt1327,timesig,enocean,hell}/test_*.cpp` of the merged plugins tree (MIT-licensed lane code, the generators ported here), the merged `index.json`. No web page opened. No GPL / LGPL / AGPL source read. Nothing downloaded.

## Owner decisions

- Real-recording results are kept outside the repository (callsigns, registrations, meter and device ids): the committed record names no id. Confirm that the masked `RESULTS.md` may stay in the shared prep folder.
- The 23 NO FEED YET generators are a porting project of its own (the largest lanes are 500-1200 lines of test generator each): decide how many go before the release.
