# OSS network items: SpyServer client, Aaronia RTSA, SDRuno .s1b import, directory listing (lane ossnet, branch feat/oss-net)

Date 2026-11-12. Worktree `D:/foxsdr-wt/en-ossnet`, branch `feat/oss-net` cut from `engine/multi-radio` at ba2a4679, build `D:/foxsdr-build/en-ossnet` (Release, `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`). Nothing pushed or merged. Brief: `queued-briefs/STANDARD-RULES.md`, `unblocked-radios.md`, `oss-common.md` (points 1 to 8). Every item is OFF until the user turns it on, raises no Windows dialog and stores no credential. No test connects to an outside service: every server below is a stand-in on 127.0.0.1.

## Step 0: what the base had, where each item belongs, what was opened

| # | Item | In the base? | Belongs in | Source used | Licence opened / file headers |
|---|---|---|---|---|---|
| 1 | SpyServer client | No (only a comment in `core/diag_log.cpp` that scrubs addresses in SpyServer log lines) | host: new `DeviceSource` `spyserver_*` in `src/source` (stand-alone, wiring hand-off below) | racerxdl/spy2go (Go) and the spyclient mirror Tubbz-alt/spyclient (Python, sam210723) | spy2go: LICENSE = standard Apache-2.0 text, template unfilled. spyclient: MIT, "Copyright (c) 2020 Sam". Neither repo's source files carry a header of their own. |
| 2 | Aaronia RTSA-Suite | YES: `AaroniaSource` (key `aaronia`, `radio-families.md` part 3) read from the maker's PDF, with five hypotheses | the same files; extended | hb9fxq/gr-aaronia_rtsa and the library it calls, hb9fxq/libspectranstream | both: MIT, "Copyright (c) 2021 Frank Werner-Krippendorf"; every file read carries the MIT header; NONE is GPL |
| 3 | SDRuno .s1b import | No | host: `core/freq_import_sdruno.hpp` beside `freq_import_foreign.hpp`, dispatched from `importFrequencyList` (the existing import path) | LinuxSheeple-E/SDRunoPlugin_Fran (`loadS1bCsvFile`); vocabulary from the CaptDaags/SDRUno_S1B sample banks | Fran: README permission text = 3-clause BSD wording ("Portions are Copyright (C) 2020 Eric A. Cottrell"), no LICENSE file, GitHub shows none; its bundled CSV reader is ben-strasser's, BSD-3 text in the same README (nothing of it used). CaptDaags banks: no licence file, read only to learn the vocabulary, nothing copied. |
| 4 | Receiverbook listing | No (`web-secure.md` part 6 had only looked at the directory's pages and built nothing) | host: `net/receiver_listing.hpp` plus a route in `net/web_server.cpp` | facts of the document, observed from a public receiver's own `status.json` and the OpenWebRX API wiki page | no source of OpenWebRX or Receiverbook (GPL / AGPL) opened; nothing ported |

Third-party notices (project, URL, commit read, licence text in full): `src/source/THIRD-PARTY-NOTICES.md` (items 1 and 2), `src/core/THIRD-PARTY-NOTICES.md` (item 3), `docs/third-party-notices-net.md` (item 4: none; moved out of src/net at the engine-merge-2 merge because test_engine_shell_guard allows only .cpp, .hpp and .h files directly in src/net). Each ported file also carries a header with the same facts.

## Outcome

| Item | Result | Commit |
|---|---|---|
| 1 SpyServer client | BUILT (`SpyServerSource`, key `spyserver`) against a stand-in server. UNVERIFIED on a real SpyServer. **Airspy's terms for third-party clients are unknown: owner decision before this ships.** | f20e54cb |
| 2 Aaronia RTSA | EXTENDED: the two MIT programs settled two of the five hypotheses; added `format=int16` and `control=remoteconfig` (the reference's way of setting centre and rate). No vendor SDK, no GPL file. | f0a7de49 |
| 3 SDRuno .s1b import | BUILT, through the existing bookmark-import path (one dispatch line in `freq_import.cpp`) | debf86d8 |
| 4 Receiverbook listing | BUILT: public `GET /status.json`, off by default, user-typed public fields only. Registration stays the user's step in their browser. | 88adad8d |

## Item 1: SpyServer client

Files: `src/source/spyserver_protocol.hpp` (command encoders, 20-byte message header, DEVICE_INFO and CLIENT_SYNC parsers, I/Q converters for 8-bit, 16-bit and float, sample-rate table, a message assembler that bounds every length), `spyserver_source.{hpp,cpp}`, test `tests/test_spyserver_source.cpp`.

**Wire format (read from the two open clients).** All integers little-endian. Command = u32 type (0 HELLO, 1 GET, 2 SET, 3 PING), u32 body length, body. HELLO body = u32 protocol version (0x020006A4 = 2.0.1700) + the client's name. SET body = u32 setting + u32 value (settings: 0 stream mode, 1 streaming enabled, 2 gain, 100 I/Q format, 101 I/Q frequency, 102 I/Q decimation). Server messages = 20-byte header (u32 protocol, type, stream, sequence, body size) + body; types 0 DEVICE_INFO (12 u32: device type, serial, max sample rate, max bandwidth, decimation stages, gain stages, max gain index, min and max frequency, ADC resolution, min I/Q decimation, forced format), 1 CLIENT_SYNC (9 u32: can-control, gain, device centre, I/Q centre, FFT centre, min and max I/Q centre, min and max FFT centre), 100/101/103 8-bit, 16-bit and float I/Q. A server that cannot give a device closes the connection (MIT client). A different major or minor protocol version and a body over 1 MiB are refused (spy2go's rules).

**Behaviour.** `open("spyserver=host[:port],format=int16|uint8|float,rate=Hz,timeout=ms")`: refused without a typed address (there is no default host). Connects (IPv4/name via the shared `iiod::connectTcp`), sends HELLO, waits (bounded) for the two description messages, refuses with a sentence a server that closes first, reports no device, speaks another protocol, sends a short description, or does not give this client control; then sends stream mode (I/Q only), format and decimation. A reader thread runs from `open` to `closeDevice` (it folds the server's CLIENT_SYNC messages in); `start`/`stop` send STREAMING_ENABLED 1/0 and flip the keep-or-discard gate. Tuning is checked against the range the server reports; the gain is a stage index 0..MaximumGainIndex (`GainUnit::Steps`, clamped); the sample rate is the nearest decimation stage (`max / 2^stage`). Stream faults latch with a sentence: bad version mid-stream, oversize body, an I/Q body that is not a whole number of samples, the device lost, silence for `timeout`, a closed connection. 8-bit, 16-bit and float messages are read as they come, whatever was asked (a server may force 8-bit). Audio, FFT, 24-bit and unknown messages are counted and skipped; I/Q sequence gaps are counted (`sequenceGaps()`). The device serial the server sends is read into memory and never shown or stored.

**Tests** (`tests/test_spyserver_source.cpp`; stand-in server on 127.0.0.1 written in the test from the same two sources, so it cannot show the reading right):
* literal bytes for HELLO ("FoxSDR", 64-byte name cut) and SET (7074000 Hz = D0 F0 6B 00), header decode and the boundaries (1 MiB body ok, 1 MiB + 1 refused; 2.0.1558 ok, 3.0 and 2.1 refused), description parsers (short, exact, longer), rate table and nearest stage including a minimum-decimation floor, converters (odd lengths, NaN, empty, 24-bit refused), assembler byte by byte and split, a 6000-round fuzz;
* the source end to end for four format variants (int16, forced 8-bit, float, asked 8-bit): the tone arrives at the nominal rate of the asked-for stage with the server's 1500 ppm clock error visible (`tone 6989.5 Hz at 250000 S/s`, expected 7000/1.0015), the settings the server saw in order (mode, format, decimation, frequency, three gains, rate change, streaming on, rate change while streaming, streaming off), the serial not in the name;
* 6 argument refusals that never reach the network, calls on a closed source, a nearest-stage rate (70 kS/s asks 62.5 kS/s), a dead port, and 7 stand-in refusals (closes after HELLO, no device, no control, protocol 3.x, short description, silent server within 400 ms, SSH banner garbage);
* 8 stream-fault cases (odd body, huge body, abrupt close, version mid-stream, device lost mid-stream, silence, plus sequence-gap and other-message cases that must NOT fault); 30 start/stop cycles and 30 open/close cycles with no abandoned reader.

## Item 2: Aaronia RTSA (extended)

What the base had and what it asserted: see `radio-families.md` part 3. This lane opened `hb9fxq/gr-aaronia_rtsa` (commit 72b291b0: `lib/rtsa_http_server_source_impl.{cc,h}`, `include/.../rtsa_http_server_source.h`, `grc/...yml`) and `hb9fxq/libspectranstream` (commit d50791e5: `lib/spectranstream.{cpp,h}`, `app/test_req.http`, README), both MIT, both by the author of a client that worked against a real SPECTRAN V6. What they settled:
* **Raw record delimiting** (the base's hypothesis 1): the JSON header up to one 0x1E byte, then at once `samples * 2 * width` bytes, then the next record. The parser already read it this way; now confirmed by a working client.
* **int16** (hypothesis 4): requested as `/stream?format=int16&scale=32767`, value / 32767 is the float. Built (`format=int16`).
* **Setting rate and centre** (partly hypothesis 3): `PUT /remoteconfig` with a numbered request to the named IQ-demodulator block (`centerfreq`, `samplerate`, `spanfreq` both the wanted rate) and a second request to the SPECTRAN block (`centerfreq`). Built as `control=remoteconfig,demod=NAME,spectran=NAME,spectranoffset=Hz` (defaults `Block_IQDemodulator_1`, `Block_Spectran_V6B_1`, offset 0). In that mode no `/control` start or stop is sent (the reference sends none). The default stays `control=capture` (the maker's PDF path). Block names are validated (1..64 of letters, digits, space, dot, dash, underscore); bad arguments are refused before the network.
* Still hypotheses (stated in `aaronia_protocol.hpp`): the measured-rate reading, that `frequencySpan` sets the rate on the `/control` path, the default port and float byte order.
No GPL program (SatDump, SDR++, SDRangel, sdr-aaronia-rs) was opened; no vendor SDK. A server that wants a user name and password is still refused with a sentence; nothing is stored.

**Tests** (`tests/test_aaronia_source.cpp`, extended): int16 parser at every byte split, at -32768 and with a count whose bytes never arrive; the two `/remoteconfig` request bodies compared as JSON with the reference's `test_req.http` text; block-name validation; int16 stream from the stand-in (tone and amplitude 0.5 after /32767); `control=remoteconfig` (request ids 1,2,3,4 across a retune, offset applied, no `/control` request at all, rate honoured by the stand-in), the mission's 500 refusal worded, bad names and offsets refused offline; the fuzz now runs the int16 parser too.

## Item 3: SDRuno memory-bank (.s1b) import

Files: `src/core/freq_import_sdruno.hpp` (header-only, reuses `ForeignImportResult` and the helpers of `freq_import_foreign.hpp`), a 4-line dispatch in `core/freq_import.cpp`, comment lines in `core/freq_import.hpp`, test `tests/test_freq_import_sdruno.cpp`.

**Format as read.** From the plugin's `loadS1bCsvFile`: a CSV of 8 or 9 columns named Frequency, Scan, M, Station, UTC, SubM, Filter, Port, Threshold; the column count decides; double-quote escaping. From public sample banks (vocabulary only): frequency is an integer number of hertz; Scan is Y or empty; mode codes seen are AM, FM, USB, LSB, CW (five codes in 28 banks; **the codes of SDRuno's other demodulators were never seen**); sub-mode NFM; Filter in hertz or with a K suffix; Port is the antenna; files may start with a UTF-8 BOM; a line with an empty first column and a text elsewhere is a title line.

**Mapping.** Frequency to frequency; Station to name (missing: "<MHz> MHz"); Scan Y to the tick; nearest title line above to the group; Filter to bandwidth (else the mode's default); AM/USB/LSB/CW as named; FM to NFM (WFM at 100 kHz or more, or sub-mode WFM); **any other code, and an empty cell, is imported as NFM and counted in `modeDefaulted`; a non-empty unknown code is kept in the name as " [SDRuno mode CODE]"** (a Bookmark has no field for it). Not kept: UTC, Port, Threshold. Detection (`looksLikeS1b`) needs 8+ fields, an integer hertz first and a Scan cell of Y, N or empty, so the generic CSV (checked: this program's own CSV export still imports as "CSV"), gqrx, SDR#, SDR++ and CHIRP files are untouched. Hostile input: 16 MiB, NUL, 200000 rows, 4096-byte lines (skipped and counted), UTF-8 repair, unbalanced quotes literal.

**Tests** (`tests/test_freq_import_sdruno.cpp`, fixtures written here): detection yes/no cases, a bank with BOM, CRLF, title lines, quoted comma and doubled quote, K filters, WFM by filter and by sub-mode; unknown, empty and lower-case codes, long names keeping the raw code; bad frequencies (decimal, letters, zero, 14 digits), nonsense bandwidths, control characters and invalid UTF-8, unbalanced quote, 80-byte title cut to 64 on a character boundary, ungrouped rows, the plugin's header row; refusals (empty, titles only, nothing usable, NUL, 16 MiB + 1, a 5000-byte line, 200001 rows); a 4000-case fuzz with invariants on every item; dispatch through `importFrequencyList` and through `importFrequencyFile` on a disk file.

## Item 4: Receiverbook listing (`GET /status.json`)

Files: `src/net/receiver_listing.hpp` (model, limits, document), `WebServer::setReceiverListing` and the route in `net/web_server.{hpp,cpp}`, tests `tests/test_receiver_listing.cpp`.

**Document** (the facts observed): `{"receiver":{"name","location","gps":{"lat","lon"}},"max_clients":n,"version":"..","sdrs":[{"name","type":"FoxSDR","profiles":[{"name","center_freq","sample_rate"}]}]}`. The observed public document also carries `receiver.admin` (an e-mail address) and `receiver.asl`: **both are left out on purpose.** `gps` is present only when the user typed a latitude and longitude.

**Safety.** Off until a listing with `enabled == true` and valid fields is set; until then the route is a 404 identical to a path never served. Not behind the sign-in (a crawler has no cookie), reachable only where the bind policy lets anyone reach the page (loopback by default). Reads nothing from the receiver. An invalid enabled listing is refused with a sentence and the old one stays. A crawler's hit is not counted as a browser being used (the usage callback skips `/status.json`). Limits: name 1..64 bytes, location 0..96, latitude/longitude in range (both or neither), at most 32 bands (name 1..48, centre 1 Hz..1 THz, rate 1 Hz..10 GHz), listener limit 1..1000, version up to 32 printable ASCII; strict UTF-8, no control characters. **The directory's key handshake (owners claim a receiver with a key) is NOT built:** it would need a secret stored and the directory's client code is AGPL. Registering stays a step in the user's browser.

**Tests** (`tests/test_receiver_listing.cpp`): every limit at its boundary, seven invalid UTF-8 forms, a 20000-case property (accepted text always survives a strict JSON dump) and 4000 generated valid strings; the exact document and its exact key set; the route over real HTTP with a password set (404 fresh, 404 disabled, 404 invalid, 200 with headers `application/json`, `nosniff`, `no-store`, no cookie set; `/api/status` still 401; usage unchanged by the crawler and +1 for the page; update; off again; POST not a route); three readers against a writer (never a malformed document).

## Results (real output)

Final gate, Release, `D:/foxsdr-build/en-ossnet`:

| Test | Result |
|---|---|
| test_spyserver_source | `test_spyserver_source: 126379 checks, 0 failed` (10 consecutive runs; ctest Passed 5.25 s) |
| test_aaronia_source | `test_aaronia_source: 26027 checks, 0 failed` (5 consecutive runs) |
| test_freq_import_sdruno | `test_freq_import_sdruno: 37691 checks, 0 failed` (3 consecutive runs) |
| test_receiver_listing | `test_receiver_listing: 9059 checks, 0 failed` (5 consecutive runs) |
| test_oss_net_sinks | `test_oss_net_sinks: 32 checks, 0 failed` |
| test_radio_families_sinks | `test_radio_families_sinks: 26 checks, 0 failed` |
| test_freq_import, _foreign, _formats | Passed (ctest) |
| test_web_server, test_web_secure, test_web_cors, test_web_pictures | Passed (ctest; web_server 26.95 s, web_secure 20.80 s) |
| test_foreign_modules, test_thread_inventory, test_shutdown_budget, test_shutdown_budget_free | Passed (ctest) |
| test_json_dump_policy | **FAILED, not mine:** `VIOLATION (dump() without error_handler_t::replace within 300 chars): src/core/bookmark_pack.hpp (line 82)`, `scanned 1170 files, 64 dump() sites, 1 violation(s)`. The file is unchanged on this branch (last touched by 8695a422, in the base); every `dump()` site of this lane passes. |

Not run (by the brief): the whole suite.

**Break-it table** (each: behaviour removed, red seen, restored, green seen):

| Item | Behaviour removed | Red lines |
|---|---|---|
| 1 | the "no control" refusal; the sequence-gap count; the 8-bit centring (127.5 to 128) | 4 failed (converter, `ok=1 err=""` for the no-control case, `sequenceGaps() >= 3`) |
| 2 | the `remoteconfig` mode skipping `/control` start; the SPECTRAN offset; int16 scale 32767 to 32768 | 6 failed (int16 samples x3, request count, SPECTRAN value, no-stop) |
| 3 | the dispatch line in `freq_import.cpp`; the K multiplier; the Scan mapping | 10 failed (bank, rows, dispatch, file path) |
| 4 | the 404-when-off branch; the usage exclusion; the name check; gps always emitted | 14 failed (404 cases, usage, name sentence, gps key, concurrency) |

## UNVERIFIED

* A real SpyServer (none on the desk); the 8-bit centring (127.5), the decimation table (`max / 2^stage`), the gain unit (a stage index), the reading of MinimumIQDecimation as the lowest stage, behaviour of a view-only connection (refused here): all hypotheses listed in `spyserver_protocol.hpp`. 24-bit samples, audio and FFT streams are not read.
* A real RTSA-Suite / SPECTRAN V6; the measured-rate reading; whether `control=remoteconfig` or the PDF's `control=capture` is the better default (kept `capture`).
* A bank written by SDRuno itself; every mode code beyond AM, FM, USB, LSB, CW; whether SDRuno writes title lines.
* **What the directory requires of `status.json`** (fields, the `type` string, whether a receiver without `gps` appears on its map): nothing public says so and the lane may not contact it. The user checks the result on the directory's "report a new receiver" page.
* Windows prompts: nothing here touches a certificate store, key provider, firewall rule or file dialog, none of the new tests opens a window, and every listening socket binds 127.0.0.1 only. The tests were run from the shell (not on the hidden desktop) because none can show a window; no prompt was seen, but the visible desktop was not watched.

## Decisions for the owner

1. **Airspy's terms for third-party SpyServer clients are unknown** (no terms page found; `airspy.com/spyserver` returned 404 in the earlier re-check). Options: ask Airspy before this ships; ship it off by default as built; or leave it out of the release. The code is a clean-room-style port of two permissively licensed open clients, with attribution, and nothing of Airspy's was read.
2. **Publishing a location.** The listing puts a name and location text (and optional coordinates) in a third party's hands, on a page that must then be reachable from outside. `web-secure.md` already advises HTTPS and a listener password for such a page; whether the settings screen should insist on them before the toggle works is the owner's call.
3. **The directory's key handshake** (claiming a listed receiver) is not built; it would need a stored secret and the directory's client code is AGPL.
4. The crawler hit not counting as "browser used" in the usage record is a behaviour change to `web_server.cpp`'s post-routing hook: confirm.
5. Default of the Aaronia control method after a hardware test.

## Wiring and screen hand-off (nothing here is registered with the engine, the Source menu or the settings)

**SpyServer** (driver key `spyserver`; `SpyServerSource`; args `spyserver=host[:port],format=,rate=,timeout=`): Source list entry "SpyServer (network)". Controls: "Server address" (text, host or host:port, default empty, nothing connects while empty; help text: "The address of an Airspy SpyServer on your network"); "Sample format" (16-bit, 8-bit, Float; default 16-bit); "Sample rate" (the list from `supportedSampleRatesHz()`, default the highest); "Gain" (steps, 0 to the server's maximum, unit "steps" not dB, from `gains()`); centre frequency as every source. Counters for the health line: `sequenceGaps()`, `otherMessages()`. English strings: every `lastError()` sentence in `spyserver_source.cpp` and `spyserver_protocol.hpp` ("type the address of the SpyServer...", "the SpyServer closed the connection without describing a radio - it may be full, or have no radio free", "the SpyServer is up but reports no radio...", "did not give this client control...", "the SpyServer stopped sending", "lost its radio", and the others). Engine: register the driver key in the native-driver table (engine.cpp, another lane's file) with no auto-discovery.

**Aaronia** (existing key `aaronia`): add to the existing screen: "Sample format" gains "16-bit integer" (`format=int16`); "Control method" ("RTSA capture request" = `capture`, default; "Mission blocks (remote config)" = `remoteconfig`); "Demodulator block name" (text, default `Block_IQDemodulator_1`, only with remote config); "SPECTRAN block name" (default `Block_Spectran_V6B_1`, may be empty); "SPECTRAN centre offset (Hz)" (number, default 0). New sentences: "the demodulator block name must be 1 to 64 letters...", "control must be capture or remoteconfig", "setting the demodulator: ...", "tuning the SPECTRAN block: ...".

**SDRuno import** (no new control): the Import box and drag-and-drop already call `importFrequencyFile`; the result's `format` is "SDRuno memory bank (.s1b)". The import note should mention `modeDefaulted` ("N rows had a mode this program does not know and were imported as NFM; the original code is in the name"). One-line fix for the wiring lane: `core/file_kind.cpp:295` labels every non-"CSV" bookmark format `bookmark-xml` (already true for gqrx and CHIRP).

**Directory listing** (Settings, Web page, new group "Public directory listing"; all host-side state; call `WebServer::setReceiverListing(listing, error)` on every change and once after the server is created; it survives a server stop/start and is empty in a fresh object; show `error` verbatim): toggle "List this receiver for public directories" (default OFF); "Receiver name" (text, 1 to 64 bytes); "Location" (text, 0 to 96 bytes; note: "Shown publicly"); "Latitude" and "Longitude" (numbers, optional, both or neither; note: "Shown publicly"); "Bands" (rows of "Band name" 1 to 48 bytes, "Centre frequency (MHz)", "Sample rate (MS/s)"; at most 32); the listener limit comes from the existing listener setting (1 to 1000) and the version string from the build. Static text under the toggle: "To appear on the directory, open its Report a new receiver page in your browser and enter this page's public address. FoxSDR never contacts the directory. Everything above is shown to anyone who opens the address." The `max_clients` value should be the listener limit the page really enforces (`WebServerConfig::maxListeners`).

## Sources opened (all read-only over HTTPS; nothing downloaded or run)

* https://github.com/racerxdl/spy2go , LICENSE (Apache-2.0), README.md, `spyserver/protocol.go`, `spyserver/SpyServer.go`, `spytypes/spytypes.go`, commit 14102c047be58ea987be26a4d74639f7a506b2ff
* https://github.com/Tubbz-alt/spyclient (mirror of sam210723/spyclient; `sam210723/spyclient` itself returns 404), LICENSE (MIT), README.md, `spyclient/{__init__,spyclient,enums,tuples}.py`, `test/test.py`, listings of `test/` and `docs/` (the bundled vendor binaries in `test/` were not fetched), commit ace7f8ac0b4d9287a454835c9cb655115c2a2f89
* https://github.com/hb9fxq/gr-aaronia_rtsa , LICENSE (MIT), the five files named above, commit 72b291b09c503fa38ce7a7805d1e4e7b860e73ba
* https://github.com/hb9fxq/libspectranstream , LICENSE (MIT), `lib/spectranstream.{cpp,h}` (the header's size listed, `.cpp` read in full), `app/test_req.http`, README.md, commit d50791e5de02d49132e62314909dd18c8d871b57
* https://github.com/LinuxSheeple-E/SDRunoPlugin_Fran , Readme.md, 00Readme_Fran.txt, `SDRunoPlugin_Fran.cpp`, `SDRunoPlugin_Fran.h`, commit bdee270f9c8887493f7e7db47d9157fe309005f4
* https://github.com/CaptDaags/SDRUno_S1B , the 28 `.s1b` banks (fetched as text to scratch, read for the vocabulary only, not committed)
* https://openwebrx.zl3bcj.duckdns.org/status.json (a public receiver's own status page, read once; its operator's e-mail address was not copied anywhere) and https://github-wiki-see.page/m/jketterl/openwebrx/wiki/API (documentation mirror)
* Survey files in `D:/foxsdr-build/prep/oss-recheck/` (`e4-a.md` row 29, `github-pass-1.md`, `-2.md`, `-3.md`, `browser-pass-2.md`).
* NOT opened by rule: SoapySpyServer, miweber67/spyserver_client, SDR++ (GPL); SatDump, SDR++, SDRangel, isaacbentley/sdr-aaronia-rs (GPL); jketterl/openwebrx and jketterl/receiverbook (AGPL / GPL); anything of Airspy's or Aaronia's beyond the PDF the base already cited.

**Searches run** (rule 6): `gh api search/repositories` for `spyserver` (30 hits listed, permissive ones: spy2go Apache-2.0, spyserver-go MIT (a server, not read), docker images), `spyserver client`, `airspy spyserver protocol`; `spectranstream OR spectran_stream OR spectran-stream` (repositories) and code search `spectranstream.h` (found hb9fxq/libspectranstream); web searches `openwebrx "status.json" receiver "max_clients" "sdrs" "profiles" "center_freq"` and `"openwebrx" public receiver "/status.json" example receiver name admin gps`; `receiverbook.de/map?type=openwebrx` fetched (no receiver list in the page text).

## Files touched outside new files

`src/core/freq_import.cpp` (+4 lines), `src/core/freq_import.hpp` (comments only), `src/net/web_server.hpp` (+include, +`setReceiverListing`), `src/net/web_server.cpp` (+route, +method, +members, usage-hook exclusion), `src/source/aaronia_protocol.hpp`, `aaronia_source.{hpp,cpp}`, `tests/test_aaronia_source.cpp`. Not touched: `engine.cpp`, `pipeline.cpp`, `fox_engine_api.cpp`, any `.slint`, `CMakeLists.txt` (all tests and sources are globbed), the sister lane's USB and HPSDR items. No translated strings; English strings are listed above.

`%APPDATA%/foxsdr/config.json` sha256 before and after: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged; no test or run used it).
