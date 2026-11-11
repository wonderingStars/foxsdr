# Network output parts: TCP fan-out, Beast and SBS, AIS, APRS-IS, I/Q out (branch feat/net-outputs)

Date 2026-11-11. Worktree `foxsdr-wt/feat-net`, branch `feat/net-outputs` cut from `engine/multi-radio` b9208fba, build `D:/foxsdr-build/feat-net` (configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`, the web lane's other arguments: Visual Studio 17 2022 x64, vcpkg toolchain, Release). Nothing pushed or merged. **Nothing is wired**: no window, engine, settings or plugin file was edited. The tests list is a glob, so `tests/CMakeLists.txt` was not touched either; one test inventory table WAS edited, as the brief allowed: `tests/thread_inventory_rows.inc` (three rows for the three new worker threads; the gate found it); the shutdown-budget table needed nothing (see "Shutdown budget and thread inventory").

| Part | Files | Test | Commit |
|---|---|---|---|
| 1 TCP fan-out, ring, socket shim | `src/net/tcp_fanout.{hpp,cpp}`, `src/net/spsc_ring.hpp`, `src/net/net_sock.{hpp,cpp}` | `tests/test_tcp_fanout.cpp`, `tests/net_test_support.hpp` | 9ca0d58a |
| 2 Beast and SBS | `src/net/aircraft_out.{hpp,cpp}` | `tests/test_aircraft_out.cpp` | 0ce60184 |
| 3 AIS out, UDP sink | `src/net/ais_out.{hpp,cpp}`, `src/net/udp_sink.{hpp,cpp}` | `tests/test_ais_out.cpp` | c84f1aa6 |
| 4 APRS-IS client | `src/net/aprs_is.{hpp,cpp}` | `tests/test_aprs_is.cpp` | 1ab8001a |
| 5 I/Q out | `src/net/iq_out.{hpp,cpp}` | `tests/test_iq_out.cpp` | 26cc591f |
| 6 Terms report | this record | none (reading only) | with the record |
| table edit | `tests/thread_inventory_rows.inc` (three exempt rows) | `test_thread_inventory` | eeba5309 |

## What existed (searched before building)

| Thing | What is there |
|---|---|
| A TCP server class | `net/cat_server.{hpp,cpp}`: the CAT (rigctl-style) control server. One acceptor thread, one thread per connection, cap 4, loopback by default, line-based requests answered through an engine session. It has no producer-to-many fan-out, no queue, no wait-free hand-over, so it could not be reused; its bind policy (loopback default, all-interfaces deliberate) is what `TcpFanout` copies. `net/web_server` is cpp-httplib (HTTP, not a stream). `source/iiod_client` and `source/rtl_tcp_source` are TCP CLIENTS (Pluto's iiod; an rtl_tcp server's 8-bit stream), not servers. `core/gps_reader` reads a serial GPS. |
| Aircraft, AIS or APRS output | None in the application (`src`, `tests`). In the plugins repository (`plug-labels`, read-only): `src/adsb/adsb_feed.{h,cpp}` is an opt-in Beast-format OUTBOUND CLIENT inside the ADS-B plugin (its own sockets, a fixed ring and a worker thread, off unless the user types an address, MIT, written from the format description). It sends Beast from the plugin to an aggregator; it is not a server and the host cannot see it. The AIS and APRS plugins have no network code at all (only the ABI header mentions a network, to say plugins fetch tiles themselves). |
| What the plugin ABI offers a host | `CascadeIqDecoderApi::poll_text` (decoded text lines up to the host), `CascadeTrackSourceApi::poll_tracks` (id, label, lat, lon, altitude in metres, flags, age: decoded tracks only). Nothing carries RAW frame bytes with a timestamp and level (Beast), raw AIS message bits, or a structured APRS packet. See the hand-off. |
| I/Q at the host | The engine owns the samples; plugins get `process(handle, float* interleavedIq, frames)`. There is no "network sink" in `src/sink`. |

Reused: the project's bind-policy reasoning (loopback by default), `tests/test_check.hpp`. Not reused: nothing else fitted. The Beast byte layout is implemented again here (the plugin's is private to the plugin and MIT; this tree is commercial and the host side must not depend on a plugin file).

## Sources (clean room: no radio-program source was read)

| Fact | Page (title, URL, date read 2026-11-11) | Status |
|---|---|---|
| Beast framing: 0x1A marker, type '1' '2' '3', payload 2/7/14, 6-byte big-endian 12 MHz timestamp, signal byte, 0x1a doubled, worked example frame | "beast.md" in firestuff/adsb-tools `protocols`, https://github.com/firestuff/adsb-tools/blob/master/protocols/beast.md (a format description; the repository is not a radio program) | verified; the worked example is a test |
| SBS BaseStation port-30003 lines: 22 fields, transmission types 1 to 8 | The primary page (woodair.net "SBS-1 BaseStation Tutorial 4.2 / Barebones 42 Socket Data") and homepages.mcb.net, the Kinetic manual PDF and a Scribd copy could NOT be read (connection reset, refused, binary PDF, scripts). Secondary descriptions that could be read: the Virtual Radar Server .NET help page "BaseStationMessage Class" (property list), the go-sbs package page (eight transmission types by name), a search summary of the tutorial (22 fields at most, field 2 the transmission type, MSG1 first eleven fields only, callsign MSG1 only, vertical rate MSG4 only, squawk MSG6 only). | **field positions, per-type field sets (beyond those facts), the YYYY/MM/DD and HH:MM:SS.mmm layout, the -1/0 flags and CRLF are UNVERIFIED against the primary page** |
| AIVDM sentence layout, armouring table, fill bits, fragment fields, channel, the two worked examples | "AIVDM/AIVDO protocol decoding", https://gpsd.gitlab.io/gpsd/AIVDM.html (a format page of a GPL program's project; only that page was read, first 100000 of 206930 characters) | verified; the examples (with checksums 5C, 3E, 55) reproduce exactly |
| APRS-IS login, `pass -1`, `vers`, `filter`, `#` comments and keep-alives, 512-byte lines, clients may generate only qAR and qAO | "Connecting to APRS-IS", http://www.aprs-is.net/Connecting.aspx (read by me and by the research agent) | verified |
| The never-gate list and the path rule | "IGate Details", http://www.aprs-is.net/IGateDetails.aspx (read by me) | verified |
| qAR / qAO; receive-only IGates use qAO; a login must not be exactly eight characters of 0-9 A-F; unique logins | "q Construct", http://www.aprs-is.net/q.aspx (read by me) | verified. The page gives NO callsign length/SSID rules; the 3..9 characters, one hyphen, SSID of one or two characters, not -0 rules come from the Connecting page as summarised by the research agent and are UNVERIFIED by me |
| Server login acknowledgement text (`# logresp CALL verified, server X`) | not on any page read | **UNVERIFIED**; the client looks for the words `logresp` and `verified` / `unverified` |
| Raw 8-bit unsigned = (u - 127.5) / 127.5; which programs use it | convention from memory | **UNVERIFIED** |
| Default ports (30005 Beast, 30003 SBS, 10110 AIS, 14580 APRS-IS filter port, 1234 I/Q) | 14580 from the Connecting page; 30005 and 30003 are the customary ports; 10110 and 1234 are conventions | 10110, 1234, 30005, 30003 UNVERIFIED as standards (they are only defaults the wiring may change) |

No account was created, no callsign or key other than N0CALL-style invented test values was typed anywhere, no file was downloaded (the Kinetic PDF was fetched by the fetch tool into the tool's own result folder; nothing from it is used), nothing was installed, and no test or program contacted an outside host: every socket is on 127.0.0.1.

## Part 1: `TcpFanout` (one producer, many listeners)

**API.** `FanoutConfig` (bind address default `127.0.0.1`, `allowAllInterfaces`, port 0 = OS choice, `maxClients` 1..64, `clientQueueBytes`, `ringBytes`, `stallMs`, `pollMs`, `socketSendBuffer`, `greeting`), `start(error)`, `stop()`, `publish(data, n)` (producer thread only), `stats()`.

**Design.** `publish` copies the record into a wait-free single-producer / single-consumer ring of length-prefixed messages (`MessageRing`: free-running 64-bit head and tail, acquire/release, no lock, no allocation, no system call, no wait; a full ring drops and counts). One IO thread pops the ring, appends each record to every client's queue and sends on readiness with a non-blocking WSAPoll/poll loop (2 ms wake). A record that does not fit a client's bounded queue disconnects THAT client (`droppedSlow`); a client holding bytes and not progressing for `stallMs` is disconnected (`droppedStalled`); a connection past `maxClients` is accepted and closed at once (`refusedFull`). Bytes a client sends are read and discarded, at most 64 KiB per client per turn, never parsed or echoed. `0.0.0.0` is refused unless `allowAllInterfaces`; only dotted-quad IPv4 is accepted (no resolver on the start path).

**Shutdown budget and thread inventory.** `stop()` raises a flag and joins one thread that wakes every 2 ms: measured **16.0, 16.6, 16.2 ms** (worst of 6 rounds with 8 clients attached and the producer publishing; the figure is one Windows timer tick) against the project's budget spent in whole seconds. No `std::chrono` wait constant is declared in the new files, deliberately: `tests/test_shutdown_budget.cpp` discovers `constexpr std::chrono::...` declarations under `src/` and demands a row for each; the waits here are plain integer milliseconds (`kDefaultPollMs`, `stallMs`, `kReadSliceMs`) compared against a clock by the new threads, which nothing waits on from the shutdown path except the join above. The gate shows that held: test_shutdown_budget passed. The thread inventory DID demand rows (see Results). Threads: the fan-out, the UDP sink and the APRS client each own ONE worker thread; none is started by anything yet. When the wiring starts them it must follow `web-bookmarks.md`: add a row to `kKnownWaits` only if a `std::chrono` constant is introduced, register the threads with `core/thread_beats` as `cat_server.cpp` does (and then turn the three exempt rows in `tests/thread_inventory_rows.inc` into covered rows), and call `stop()` from the engine's teardown before `beginShutdown()` ends.

**Proof (`test_tcp_fanout`, 1401 checks).** Real numbers from the run: 32 clients x 2000 records (125 KiB each) all complete and in order in **~990 ms**; a client that never reads is dropped after **2.3 MiB** published while the other client keeps every record and the producer's slowest `publish()` call is **0.001 to 0.012 ms**; a stalled client (3 MiB published, `stallMs` 300) is dropped **310 ms** after the last publish; 300 connect/drop/junk storms from 4 threads leave `accepted == closedByPeer + droppedSlow + droppedStalled` and a fresh client served correctly; an 8 MiB junk flood from one client costs the other client nothing and is counted in `bytesIgnored`; `publish()` costs **0.014 to 0.03 us** per 1 KiB call with the ring full and a client that never reads (57,461 of 200,000 accepted by a 1 MiB ring, the rest counted); the ring passes 300,000 records through a 64 KiB ring between two threads with 0 corrupt or reordered (146,261 delivered, the rest counted dropped; `delivered + dropped == total`).

**Break-it table** (remove or break the behaviour, rebuild, run the named test, see it RED, restore; every row was run, by `netout-breakit.py`; "RED" with a crash means the test program died before printing its summary, which a test that cannot get through also counts as failing):

| # | Behaviour removed | Result |
|---|---|---|
| F1 | greeting sent first to a new client | RED, 10 failed check(s): `test_tcp_fanout.cpp:206  a.waitBytes(3, 2000)` |
| F2 | 0.0.0.0 refused unless asked | RED, 3 failed check(s): `test_tcp_fanout.cpp:132  !f.start(err)` |
| F3 | client cap (closed at once past it) | RED, 6 failed check(s): `test_tcp_fanout.cpp:294  extra1.waitEof(2000)` |
| F4 | slow client dropped when its queue cannot take a record | RED, 3 failed check(s): `test_tcp_fanout.cpp:343 f.stats().droppedSlow == 1` |
| F5 | stall rule (pending bytes, no progress for stallMs) | RED, 2 failed check(s): `test_tcp_fanout.cpp:394  f.stats().droppedStalled == 1` |
| F6 | bytes a client sends are read and discarded | RED, 2 failed check(s): `:476 sentJunk >= 8 MiB` |
| F7 | every client socket closed when the IO thread stops | RED, 6 failed check(s): `:538 sawEof == 8` (first run NOT CAUGHT: the clients had already been dropped as slow before the stop; the test now keeps eight clients reading and asserts none was dropped first) |
| F8 | ring full check (a full ring drops and counts instead of overwriting) | RED: the run hung and was stopped at 240 s |
| F9 | publish refused when not running | RED, 2 failed check(s): `test_tcp_fanout.cpp:192  !f.publish("x", 1)` |
| F10 | record longer than a client queue refused and counted | RED, 3 failed check(s): `test_tcp_fanout.cpp:242  !f.publish(big.data(), big.size())` |
| F11 | dotted quad strictness (a leading-zero field refused) | RED, 1 failed check(s): `test_tcp_fanout.cpp:153  err.find("not an IPv4 address") != std::string::npos` |

## Part 2: aircraft data out (`aircraft_out`)

`beastEncode(AircraftFrame)`: marker, type from the raw length (2 -> '1', 7 -> '2', 14 -> '3', anything else refused), 48-bit big-endian timestamp (higher bits cut), signal byte, every 0x1A after the marker doubled; worst case 44 bytes; a buffer smaller than that is refused, never overrun. `sbsEncode(SbsMessage)`: `MSG,<type>,<session>,<aircraft>,<HEX>,<flight>,<date>,<time>,<date>,<time>,<callsign>,<alt>,<gs>,<trk>,<lat>,<lon>,<vr>,<squawk>,<alert>,<emerg>,<spi>,<ground>` + CRLF. Fields a transmission type does not carry are empty whatever the struct holds (MSG1 callsign; MSG2 alt, gs, track, position, ground; MSG3 alt, position, flags, ground; MSG4 gs, track, vertical rate; MSG5 alt, alert, spi, ground; MSG6 alt, squawk, flags, ground; MSG7 alt, ground; MSG8 ground: **UNVERIFIED beyond the facts in the sources table**). Callsign characters outside A-Z 0-9 space - _ become `?` (a comma, CR or LF can never add a field or a line); hex identity must be six upper-case hex digits; latitude and longitude are written together or not at all and only in range; squawk only when each digit is 0..7; track is rounded and wrapped to 0..359; the clock is clamped so no field gains a digit. `BeastOut` and `SbsOut` wrap `TcpFanout` (ports 30005 and 30003 by convention; the config decides).

**Proof (`test_aircraft_out`, 170,401 checks).** The format page's worked example reproduced byte for byte; hand-built Mode A/C, short and long frames; 20,000 random frames (a quarter full of 0x1A) through a streaming parser written separately, fed in odd chunk sizes: **0 mismatches** over 371,562 bytes, plus resync after a frame cut mid-way; every transmission type 1 to 8 as a field-by-field expected line; 5,000 random messages checked field by field; hostile callsigns; range and boundary rows; the time conversion against known dates (2024-02-29, 2000-02-29, 2099-12-31, -1 ms) and 31,000 consecutive days; both servers over real sockets, byte-exact to two clients.

**Break-it table** (same method as part 1):

Beast rows:

| # | Behaviour removed | Result |
|---|---|---|
| A1 | 0x1A doubled after the marker | RED, 6 failed check(s): `test_aircraft_out.cpp:115  enc(ex) == want` |
| A2 | type byte chosen by raw length (7 and 14 swapped) | RED, 7 failed check(s): `test_aircraft_out.cpp:115  enc(ex) == want` |
| A3 | timestamp big-endian (made little-endian) | RED, 6 failed check(s): `test_aircraft_out.cpp:115  enc(ex) == want` |
| A4 | timestamp 48 bits (a 7th byte written) | RED, 10 failed check(s): `test_aircraft_out.cpp:115  enc(ex) == want` |
| A5 | frame lengths other than 2, 7, 14 refused | RED: the test program crashed before its summary line (cause not examined: the broken code accepts a value that overruns a buffer or reaches a null pointer, or the test indexes output the broken build did not produce) |
| A6 | buffer smaller than the worst case refused | RED, 2 failed check(s): `test_aircraft_out.cpp:149  beastEncode(f, buf, kBeastMaxFrameBytes - 1) == 0` |
| A7 | signal byte = amplitude x 255 (x 256) | first run NOT CAUGHT; added `beastSignalFromAmplitude(0.99) == 252` (0.99 x 255 = 252.45, x 256 would give 253); now RED, 1 failed check: `test_aircraft_out.cpp:167  beastSignalFromAmplitude(0.99) == 252` |

SBS rows:

| # | Behaviour removed | Result |
|---|---|---|
| S1 | callsign characters outside the set replaced by ? | RED, 4 failed check(s): `test_aircraft_out.cpp:371  f.size() == 22` |
| S2 | hex identity upper-case only (lower case accepted) | RED, 1 failed check(s): `test_aircraft_out.cpp:350  sbsEncode(m, buf, sizeof(buf)) == 0` |
| S3 | MSG4 carries only speed, track, vertical rate (altitude added) | RED, 596 failed check(s): `test_aircraft_out.cpp:295  sbs(m) == expectLine("4", "", "", "451", "90", "", "", "-1280", "", "", "", "", "")` |
| S4 | latitude/longitude range check | RED, 2 failed check(s): `test_aircraft_out.cpp:383  splitCsv(sbs(p))[14] == "" && splitCsv(sbs(p))[15] == ""` |
| S5 | squawk digits 0..7 only | RED, 3 failed check(s): `test_aircraft_out.cpp:408  splitCsv(sbs(q))[17] == ""` |
| S6 | track wrapped to 0..359 (negative not wrapped) | RED, 1 failed check(s): `test_aircraft_out.cpp:420  splitCsv(sbs(t))[13] == "350"` |
| S7 | CRLF terminator (LF only) | RED, 11897 failed check(s): `test_aircraft_out.cpp:267  sbs(m) == expectLine("1", "KLM1023", "", "", "", "", "", "", "", "", "", "", "")` |
| S8 | true flag written -1 (written 1) | RED, 2772 failed check(s): `test_aircraft_out.cpp:277  sbs(m) == expectLine("2", "", "0", "12", "271", "52.30861", "4.76389", "", "", "...` |
| S9 | clock clamped (a 5-digit millisecond field) | RED, 1 failed check(s): `test_aircraft_out.cpp:435  g[6] == "9999/12/31" && g[7] == "23:59:60.999"` |
| S10 | 400-year leap rule | RED, 2 failed check(s): `test_aircraft_out.cpp:449  s(951782400000ll) == "2000/02/29 00:00:00.000"` |
| S11 | a too-small line buffer refused (truncated line returned) | RED, 2 failed check(s): `test_aircraft_out.cpp:360  sbsEncode(m, buf, 20) == 0` |
| S12 | fields a type does not carry written empty | RED, 1895 failed check(s): `test_aircraft_out.cpp:267  sbs(m) == expectLine("1", "KLM1023", "", "", "", "", "", "", "", "", "", "", "")` |

## Part 3: AIS out (`ais_out`) and the UDP sink (`udp_sink`)

`aisEncode(bits, nbits, options)`: 6-bit armouring (value + 48, +8 above 87), fill bits (zero pad, count in the last sentence only), at most 56 payload characters per sentence by default (the split the worked example uses; up to 60), fragment count and number, one-digit sequence id for multi-sentence messages (empty for one sentence), channel `A`, `B` or empty, XOR checksum between `!` and `*`; 1 to 1008 bits, at most 9 fragments; all refusals return 0 and leave the output empty. `AisOut` sends every sentence over `TcpFanout` and/or `UdpSink` (one sentence per datagram) and rolls the sequence id 0..9. `UdpSink`: one unicast IPv4 destination (127.0.0.1 default), broadcast and multicast refused unless asked, `maxDatagram` 16..65000 (default 1400), the same wait-free ring and one sender thread, ring and socket drops counted, stop in about a millisecond.

**Proof (`test_ais_out`, 9,527 checks).** The two worked examples of the AIVDM page (single sentence, `*5C`; two sentences with sequence id 3, fill 2, `*3E` and `*55`) are decoded by an independent decoder to bits and the encoder reproduces the exact strings, which also settles the page's ambiguous checksum prose (XOR between `!` and `*`). Every length 1 to 1008 round-trips (all sentences within 82 bytes: longest at the default split **78 bytes** with CRLF), fill bits 0 to 5, the 56/57-character split, refusals, UDP delivery of 200 datagrams in order, a flood into an 8 KiB ring (**215 of 100,000 accepted, 99,785 counted dropped, producer took 0.8 ms**), stop while sending (**0.1 ms**), AIS over TCP and UDP together.

**Break-it table** (same method as part 1):

AIS rows:

| # | Behaviour removed | Result |
|---|---|---|
| I1 | armouring: +8 above 87 | RED, 1062 failed check(s): `test_ais_out.cpp:117  v.size() == 1 && v[0] == std::string(kOne) + "\r\n"` |
| I2 | checksum starts after the ! (the ! included) | RED, 7166 failed check(s): `test_ais_out.cpp:117  v.size() == 1 && v[0] == std::string(kOne) + "\r\n"` |
| I3 | fill bits only in the last sentence | RED, 2088 failed check(s): `test_ais_out.cpp:132  v[0] == std::string(kTwoA) + "\r\n"` |
| I4 | empty sequence id for a one-sentence message | RED, 4 failed check(s): `test_ais_out.cpp:117  v.size() == 1 && v[0] == std::string(kOne) + "\r\n"` |
| I5 | split at 56 payload characters (57) | RED: the test program crashed before its summary line (cause not examined: the broken code accepts a value that overruns a buffer or reaches a null pointer, or the test indexes output the broken build did not produce) |
| I6 | channel must be A, B or empty | RED, 5 failed check(s): `test_ais_out.cpp:219  aisEncode(one, 6, o, s) == 0` |
| I7 | sequence id 0..9 | RED, 2 failed check(s): `test_ais_out.cpp:227  aisEncode(one, 6, o, s) == 0` |
| I8 | at most 9 fragments | RED, 1 failed check(s): `test_ais_out.cpp:238  aisEncode(big.data(), 1008, o, s) == 0` |
| I9 | sequence id rolls 0..9 per multi-sentence message | RED, 23 failed check(s): `test_ais_out.cpp:443  s3.ok && s3.seq == "1" && s3.channel == "A"` |
| I10 | pad bits are zero (set to one) | RED, 852 failed check(s): `test_ais_out.cpp:133  v[1] == std::string(kTwoB) + "\r\n"` |
| I11 | talker must be AIVDM or AIVDO | RED: the test program crashed before its summary line (cause not examined: the broken code accepts a value that overruns a buffer or reaches a null pointer, or the test indexes output the broken build did not produce) |

UDP sink rows:

| # | Behaviour removed | Result |
|---|---|---|
| U1 | broadcast and multicast destinations refused unless asked | RED, 2 failed check(s): `test_ais_out.cpp:299  !UdpSink(c).start(err) && err.find("broadcast") != std::string::npos` |
| U2 | datagram longer than maxDatagram refused | RED, 2 failed check(s): `test_ais_out.cpp:319  !s.send(big.data(), big.size())` |
| U3 | destination 0.0.0.0 refused | RED, 1 failed check(s): `test_ais_out.cpp:297  !UdpSink(c).start(err)` |
| U4 | send refused when not running | RED, 2 failed check(s): `test_ais_out.cpp:315  !s.send("x", 1)` |
| U5 | ring drops counted | RED, 1 failed check(s): `test_ais_out.cpp:397  sink.stats().ringDropped + static_cast<std::uint64_t>(accepted) == 100000` |

## Part 4: APRS-IS client (`aprs_is`)

**Receive-only by default.** With `gateway` off the login says `pass -1` whatever passcode is in the configuration, `forward()` refuses, and the client writes nothing at all after the login line (a test compares the server's received bytes to the login line, and the client's own `bytesWritten` to it). No server is built in (`host` empty refuses to start). The passcode is typed by the user (`-1`, or one to five digits up to 32767); no generator exists; it is never in a log, an error, a stats field or `redactedLogin()`.

**Client.** Connect with a sliced timeout (a refused connection ends at once), wait for the server's identification line (up to `loginWaitMs`) and then send `user CALL pass P vers NAME VER [filter F]`; `#` lines are comments, any byte from the server resets the idle timer, `idleTimeoutMs` of silence reconnects; reconnect back-off `initial x 2^n` up to a cap, reset after a session that was answered and lasted `stableMs`; lines over 512 bytes are skipped and counted; 4096 bytes with no line end drops the session; stop joins in tens of milliseconds even while connecting or backing off.

**Gateway rules** (IGate Details page, one test each): not gated when the frame is not a UI frame (control 0x03, PID 0xF0), when the TNC has PASSALL on, when a third-party packet's inner header holds TCPIP or TCPXX, for generic queries (`?`), when the header holds TCPIP, TCPXX, NOGATE or RFONLY (as a path element, any case, with or without `*`); a third-party packet without them goes out with the RF header and the `}` removed; the path is only ever extended with `,qAR,CALL` (or `,qAO,CALL` for a receive-only IGate); q constructs and `I` in an RF header are refused (they never appear on RF); added: CR/LF/NUL in the packet, a nested third-party packet, an unparsable header, more than 8 digipeaters and a result over 510 bytes are refused. In gateway mode a login that is not answered `verified` forwards nothing (`NotOnline`, `NotVerified`).

**Proof (`test_aprs_is`, 360 checks, ~9 s, stand-in servers on loopback only).** Login line equals the page's own example; 9 valid and 22 invalid callsigns; passcodes; the login waits for the server line (a 400 ms wait measured at >= 380 ms) and follows at once when it comes; 15 keep-alive `#` lines over three idle timeouts keep one connection; silence reconnects; measured reconnect gaps with `initial 150 ms, max 600 ms` and four refused connections: **230, 322, 632, 627, 627 ms** (the first includes detecting the drop; 150/300/600/600/600 plus scheduling), and after a stable session the next wait is the initial one (793 ms total with a 600 ms session); stop while connecting or backing off **44.8 to 47.0 ms** (worst of 4); a server that sends a 600-byte line, binary junk, a 1 MiB line, then 2,000 packets in a burst (all delivered in order); gateway mode on a verified stand-in login: exact wire bytes for two gated packets, five refusals counted and absent from the wire, `qAO` setting; `forward()` against a server that never reads: **17 queued, 1,983 `QueueFull` in 1.2 ms**.

**Break-it table** (same method as part 1):

Client rows:

| # | Behaviour removed | Result |
|---|---|---|
| P1 | receive-only login says pass -1 (config passcode sent instead) | RED, 5 failed check(s): `test_aprs_is.cpp:90  line == "user N0CALL-5 pass -1 vers FoxSDR 0.99.109\r\n"` |
| P2 | forward() refuses when the gateway is off | RED, 1 failed check(s): `test_aprs_is.cpp:338  cl.forward(gi) == GateReason::GatewayOff` |
| P4 | forward() refuses an unverified login | RED, 2 failed check(s): `test_aprs_is.cpp:580  cl.forward(gi) == GateReason::NotVerified` |
| P5 | forward() refuses until online | RED, 1 failed check(s): `test_aprs_is.cpp:576  cl.forward(gi) == GateReason::NotOnline` |
| P6 | SSID -0 refused | RED, 4 failed check(s): `test_aprs_is.cpp:65  !aprsNormaliseCallsign(c)` |
| P7 | passcode range 0..32767 | RED, 1 failed check(s): `test_aprs_is.cpp:78  !aprsPasscodeWellFormed(b)` |
| P8 | filter limited to printable ASCII | RED, 1 failed check(s): `test_aprs_is.cpp:136  !aprsBuildLogin(c, line, err)` |
| P9 | idle timeout reconnects | RED, 3 failed check(s): `test_aprs_is.cpp:392  waitFor([&] { return srv.accepted() >= 2; }, 4000)` |
| P10 | any server byte (a # line) resets the idle timer | RED, 3 failed check(s): `test_aprs_is.cpp:388  srv.accepted() == 1` |
| P11 | back-off capped at the maximum | RED, 37 failed check(s): `test_aprs_is.cpp:166  aprsNextBackoffMs(6, 1000, 60000) == 60000` |
| P12 | a 4096-byte line with no end drops the session | RED, 2 failed check(s): `test_aprs_is.cpp:533  waitFor([&] { return cl.stats().hostileDrops >= 1; }, 5000)` |
| P13 | complete lines over 512 bytes skipped | RED, 2 failed check(s): `test_aprs_is.cpp:525  got.size() == 1 && got[0] == "N0OK>APRS:>fine"` |
| P14 | login waits for the server identification line | RED, 2 failed check(s): `test_aprs_is.cpp:357  srv.received(0).empty()` |
| P15 | back-off wait between attempts | RED, 6 failed check(s): `test_aprs_is.cpp:418  gap1 >= 130 && gap1 < 450` |
| P16 | back-off reset after a stable session | RED, 1 failed check(s): `test_aprs_is.cpp:452  sinceStable < 600 + 330` |
| P17 | packet handed to the handler exactly once (here: handed twice) | RED, 4 failed check(s): `test_aprs_is.cpp:322  waitFor([&] { std::lock_guard<std::mutex> g(m); return got.size() == 3; }, 3000)` |

Gate-rule rows:

| # | Behaviour removed | Result |
|---|---|---|
| G1 | rule 1: frame without control 0x03 / PID 0xF0 | RED, 4 failed check(s): `test_aprs_is.cpp:206  gate("N0CALL>APRS:>hi", false).reason == GateReason::NotUiFrame` |
| G2 | rule 2: TNC PASSALL on | RED, 4 failed check(s): `test_aprs_is.cpp:208  gate("N0CALL>APRS:>hi", true, true).reason == GateReason::PassAll` |
| G3 | rule 4: generic query (?) | RED, 5 failed check(s): `test_aprs_is.cpp:223  gate("N0CALL>APRS:?APRS?").reason == GateReason::GenericQuery` |
| G4 | rule 5: TCPIP / TCPXX / NOGATE / RFONLY in the header | RED, 13 failed check(s): `test_aprs_is.cpp:212  gate(pkt).reason == GateReason::ForbiddenPathElement` |
| G5 | rules 9, 10: q construct or I in an RF header | RED, 7 failed check(s): `test_aprs_is.cpp:220  gate(pkt).reason == GateReason::QConstructOnRf` |
| G6 | rule 3: third-party header with TCPIP / TCPXX | RED, 4 failed check(s): `test_aprs_is.cpp:227  gate("N0RF>APRS,WIDE1-1:}N0INT>APRS,TCPIP,N0GATE*:>from the internet").reason == Gate...` |
| G7 | rule 6: third-party packet gated without its RF header and } | RED, 3 failed check(s): `test_aprs_is.cpp:233  d.line == "N0INT>APRS,WIDE2-1,qAR,N0GATE-10:>inner status"` |
| G8 | rule 8: qAO for a receive-only IGate | RED, 3 failed check(s): `test_aprs_is.cpp:197  d.reason == GateReason::Gated && d.line == "N0CALL-9>APRS,WIDE1-1*,qAO,N0GATE-10:>sta...` |
| G9 | 510-byte limit on the gated line | RED, 2 failed check(s): `test_aprs_is.cpp:257  gate("N0CALL>APRS:>" + tooLong).reason == GateReason::TooLong` |
| G10 | CR / LF / NUL in a packet | RED, 3 failed check(s): `test_aprs_is.cpp:250  gate("N0CALL>APRS:>hi\r\nN0EVIL>APRS:>second packet").reason == GateReason::Malformed` |
| G11 | nested third-party packet | RED, 1 failed check(s): `test_aprs_is.cpp:239  gate("N0RF>APRS:}N0A>APRS:}N0B>APRS:>x").reason == GateReason::NestedThirdParty` |
| G12 | NOGATE and RFONLY (optional rules, applied) | RED, 4 failed check(s): `test_aprs_is.cpp:212  gate(pkt).reason == GateReason::ForbiddenPathElement` |
| G13 | at most 8 digipeaters | RED, 1 failed check(s): `test_aprs_is.cpp:248  gate(m).reason == GateReason::Malformed` |

## Part 5: network I/Q (`iq_out`)

**Formats.** 8-bit unsigned `round(x * 127.5 + 127.5)` clamped (zero is 128), 16-bit signed `round(x * 32767)` clamped to +-32767 little-endian, 32-bit float little-endian; every non-finite sample is sent as zero in all three. **Header option** (28 bytes, little-endian): `FXIQ`, version u16 = 1, format u16 (1 u8, 2 s16, 3 f32), sample rate u32, centre frequency f64, sequence u32, flags u32 = 0. TCP: the header is the greeting of each new client, then the stream; UDP: every datagram carries the header with a counter that counts a datagram even when it is then dropped, so a receiver sees the gap. **Headerless** is the default. **Which samples** (the tuned channel or the full band) and at what rate is the caller's choice: `submit(const float* iq, n)` and the header's rate and centre are what the caller says. **Drops** are counted (`samplesIn == samplesQueued + samplesDropped`, `chunksDropped`) and visible in the fan-out's and sink's own counters.

**Proof (`test_iq_out`, 388 checks).** Hand-computed conversion tables for all three formats including clamps, NaN and infinity; quantisation error over -1..1: **8-bit 0.00392 (half a step), 16-bit 0.0000153**; the header bytes against the layout and a hostile-header table; TCP streams in each format with and without the header, byte-exact to an independent conversion of 4,321 random samples (4 chunks and a part); a late client gets only the header; UDP datagrams (header, consecutive sequence numbers, whole samples, sum equals the input); flat-out producer against a client that never reads: **98.7 % of 30 M samples dropped and counted, slowest `submit()` of 100,000 samples 0.67 ms**, the client then dropped by the fan-out; UDP into a 16 KiB ring: 8,581,322 of 10,000,000 dropped and counted equal to the sink's ring drops; stop while submitting flat out **16.2 ms**.

**MEASURED CPU COST at 2.4 MS/s** (4 s of signal; the clock ticks in 15.6 ms steps, so these are bounds):

| Case | Conversion alone (one core) | Whole path in real time, 20 ms blocks (process CPU, includes the IO thread and the in-process reader) | `submit()` calls alone |
|---|---|---|---|
| 8-bit, TCP | 0.56 % | 0.4 % (19.1 MB received, 0 dropped) | 0.93 % |
| 16-bit, TCP | 1.14 % | 2.3 % (38.3 MB, 0 dropped) | 3.53 % |
| float, TCP | 0.21 % | 1.9 % (76.4 MB, 0 dropped) | 0.71 % |
| 8-bit, UDP | | 2.0 % (19.2 MB, 0 dropped) | 0.92 % |
| 16-bit, UDP | | 7.0 % (38.4 MB, 0 dropped) | 2.86 % |
| float, UDP | | 9.8 % (76.8 MB, 0 dropped) | 0.57 % |

The producer-thread cost (conversion plus publish) is 0.6 to 3.5 % of one core at 2.4 MS/s. The UDP rows are higher because the in-process receiver reads about 3,400 to 55,000 datagrams a second in the same process; a real receiver is another process. 76.8 MB/s of float samples is 614 Mbit/s: a gigabit link carries it, a Wi-Fi link does not (the drops counter is how the user would see that).

**Break-it table** (remove or break the behaviour, rebuild, run the named test, see it RED, restore; every row was run, by `netout-breakit.py`; "RED" with a crash means the test program died before printing its summary, which a test that cannot get through also counts as failing):

| # | Behaviour removed | Result |
|---|---|---|
| Q1 | 8-bit scale x 127.5 + 127.5 | RED, 9 failed check(s): `test_iq_out.cpp:45  u[i] == want[i]` |
| Q2 | 8-bit clamp | RED, 3 failed check(s): `test_iq_out.cpp:45  u[i] == want[i]` |
| Q3 | non-finite input sent as zero | RED, 14 failed check(s): `test_iq_out.cpp:49  b == 128` |
| Q4 | 16-bit little-endian | RED, 59 failed check(s): `test_iq_out.cpp:59  s16At(s, i) == ws[i]` |
| Q5 | 16-bit clamp at +-32767 | RED, 1 failed check(s): `test_iq_out.cpp:59  s16At(s, i) == ws[i]` |
| Q6 | header magic FXIQ | RED, 48 failed check(s): `test_iq_out.cpp:101  std::memcmp(b, "FXIQ", 4) == 0` |
| Q7 | header centre frequency field | RED, 27 failed check(s): `test_iq_out.cpp:109  b[12 + i] == static_cast<std::uint8_t>(bits >> (8 * i))` |
| Q8 | header validation on read | RED, 4 failed check(s): `test_iq_out.cpp:124  !iqReadHeader(bad, sizeof(bad), r)` |
| Q9 | UDP sequence number per datagram | RED, 15 failed check(s): `test_iq_out.cpp:305  h.sequence == i && h.format == f && h.sampleRateHz == 250000 && h.centreHz == 162.025e6` |
| Q10 | TCP header sent as the greeting | RED, 18 failed check(s): `test_iq_out.cpp:232  k.waitBytes(hdr + want.size(), 3000)` |
| Q11 | drops counted | RED, 6 failed check(s): `test_iq_out.cpp:207  n.stats().samplesDropped == 1` |
| Q12 | chunk must fit one datagram | RED, 1 failed check(s): `test_iq_out.cpp:180  !IqOut(c).start(err) && err.find("datagram") != std::string::npos` |
| Q13 | header needs a sample rate | RED, 1 failed check(s): `test_iq_out.cpp:154  !IqOut(c).start(err) && err.find("sample rate") != std::string::npos` |
| Q14 | input split into chunks (one oversized chunk) | RED: the test program crashed before its summary line (cause not examined: the broken code accepts a value that overruns a buffer or reaches a null pointer, or the test indexes output the broken build did not produce) |
| Q15 | UDP header on every datagram | RED, 45 failed check(s): `test_iq_out.cpp:304  iqReadHeader(got[i].data(), got[i].size(), h)` |
| Q16 | chunk must fit the TCP queue | RED, 1 failed check(s): `test_iq_out.cpp:172  !IqOut(c).start(err) && err.find("chunk") != std::string::npos` |

## Part 6: TERMS REPORT (reading only; nothing was built for it)

Read 2026-11-11 by a research sub-agent with the web-fetch tool, then spot-checked by me (SondeHub, FlightAware-linked pages, the APRS-IS pages in full; ADS-B Exchange, OpenSky and Flightradar24 pages returned 404 or 403 to me too). The tool summarises pages, so wording is paraphrase. **A verdict of "could not be read" is a finding, not a pass.** Sub-agent claims I did not re-read are labelled.

| Service | Must have (from its pages) | Accepts | Third-party client software (its terms) | Verdict |
|---|---|---|---|---|
| FlightAware | an account to claim the feeder and the feeder id PiAware generates; the feeder terms page returned 404 | PiAware reads Beast; PiAware uploads | Terms of Use (https://www.flightaware.com/about/termsofuse): site use only with hardware or software FlightAware provided; bots barred "except for FlightAware data feeds and APIs" (sub-agent paraphrase); whether a non-PiAware uploader is allowed is NOT stated on a page read | **needs the service's own closed feeder program** (PiAware, fed Beast by part 2) **and the owner's account** |
| Flightradar24 | a sharing key from the `fr24feed` signup, one key per feed, up to three keys per account (support.fr24.com articles 3000115335, 3000115340) | `fr24feed` reads Beast (port 30005 in its sample config) or AVR | ToS and share-your-data page returned 403: **could not be read** | **needs own feeder program** (it reads FoxSDR's Beast port) **and the owner's key**; terms unknown |
| ADS-B Exchange | feed FAQ, legal and terms pages returned 404 to the agent and to me: **could not be read**. UNVERIFIED (community page, not the service's): Beast to `feed.adsbexchange.com:30005`, MLAT 31090, a user UUID | UNVERIFIED | **could not be read** | cannot be decided from the service's own pages |
| adsb.fi | an existing receiver plus coordinates and elevation; no account described (GitHub scripts repo https://github.com/adsbfi/adsb-fi-scripts) | its feed script reads Beast from 127.0.0.1:30005 and names ports 30004 and 31090; no manual endpoint documented; site pages 403 | LICENSE.md pointed to, not read; site terms **could not be read** | buildable only as "its script reads our Beast port"; direct feeding from FoxSDR UNVERIFIED; **owner decides** |
| adsb.lol | a self-generated UUID, coordinates, elevation; no account (https://github.com/adsblol/feed, https://www.adsb.lol/docs/get-started/introduction/) | Beast: the repo's setup line is `feed.adsb.lol,30004,beast_reduce_plus_out`, MLAT `feed.adsb.lol:31090`; the docs say the only requirement is Beast out on 30005 | repo MIT; the privacy and licence page returned 404: data licence **could not be read** | **buildable with part 2** (owner supplies the UUID and position) |
| airplanes.live | an existing receiver plus coordinates; no account described (https://github.com/airplanes-live/feed) | TCP ports 30004 and 31090 to a stated address; the protocol is not named on the page | repo MIT; site how-to-feed and FAQ pages 403: terms **could not be read** | probably buildable with part 2 (Beast); UNVERIFIED; owner decides |
| OpenSky Network | an account; UNVERIFIED (community guide, not OpenSky's): a feeder serial | UNVERIFIED: `opensky-feeder` reads Beast and sends a binary login to `collector.opensky-network.org:10004` | OpenSky's own pages returned 403: **could not be read** | **needs its own feeder program** (UNVERIFIED) **and the owner's account** |
| APRS-IS | a callsign with optional SSID; only a valid passcode may SEND (software authors issue passcodes; the algorithm is not published); receive-only is `pass -1` (https://www.aprs-is.net pages, read by me) | TNC2 text over TCP: 14580 filter port (10152 full feed "use sparingly"); max 512 bytes a line; clients may generate only qAR and qAO; a receive-only IGate uses qAO | the pages describe the protocol and the IGate rules, no software ban | receive-only: **buildable with part 4**. Gateway: buildable with part 4 **plus the owner's licensed callsign and passcode** |
| SondeHub | no key or login described; an uploader callsign (https://github.com/projecthorus/sondehub-infra/wiki, https://www.sondehub.org/) | HTTP PUT of JSON to `/sondes/telemetry`, `/amateur/telemetry`, `/listeners` (a `Date` header on the first) | no terms page found; data CC BY-SA 2.0; the upload table lists third-party tools (SDRangel, marked limited) as supported | acceptable in principle, **needs a new HTTPS JSON uploader (none of parts 1 to 5)** and the owner's callsign |
| MarineTraffic | UNVERIFIED (search snippets from help.kpler.com / support.marinetraffic.com): an account, a station, a port assigned per station | UNVERIFIED: UDP to `listener.marinetraffic.com` on the assigned port | help pages 403, terms **could not be read** | **needs the owner's account and port**; if UDP NMEA as reported, part 3's UDP output fits (UNVERIFIED) |
| AISHub | a join form (name, email, country, station location, equipment); an operating station and feed required; location is shown publicly; API access needs at least 10 vessels (7-day average), 90 % uptime (https://www.aishub.net/join-us) | raw NMEA from your own receiver; the page does not say UDP or TCP or a host | own receiver's data only, "not synthesised, scraped or public-service"; passing raw data on to others is not allowed; the Terms of Use page returned 404: client-software rules **could not be read** | **needs the owner's membership**; part 3's AIVDM is the right data, transport unknown |
| airframes.io | working reception; no account or station id on the page (https://docs.airframes.io/docs/feeding/how) | to `feed.airframes.io`: ACARS UDP 5550, VDL2 UDP 5552 or TCP 5553, HFDL UDP 5556, decoder JSON | terms not on the page | **not covered by parts 1 to 5** (needs decoder JSON from ACARS/VDL2/HFDL decoders) |
| WSPRnet | the site returned 403: **could not be read**. UNVERIFIED (search snippet): HTTP POST of spots with callsign and grid | UNVERIFIED | **could not be read** | cannot be decided; not covered by parts 1 to 5 |
| PSK Reporter | only a callsign in the report; no account (https://pskreporter.info/pskdev.html) | IPFIX over UDP to `report.pskreporter.info` port 4739 (test listener 14739), rate limits, `informationSource` automatic | no formal terms; the operator may block a heavy user | acceptable, **needs a new IPFIX UDP part (none of 1 to 5)**, owner's callsign |
| Broadcastify | UNVERIFIED (snippets): an application through the account's feed menu and approval | UNVERIFIED: its own ScannerCast route, not an open ingest | provider terms **could not be read** | **needs its own feeder program and the owner's account** (UNVERIFIED) |
| OpenMHz | from the trunk-recorder wiki, not OpenMHz: an account at admin.openmhz.com, then a system issues shortName, apiKey, uploadServer | audio as AAC `.m4a` uploads | own site 403: terms **could not be read** | **needs the owner's account and key; a new audio uploader**, none of parts 1 to 5 |

Nothing read says third-party software is FORBIDDEN, but the services that are the most closed (FlightAware, Flightradar24, OpenSky, Broadcastify) only document their own programs, and for twelve of the sixteen at least one of the terms, feeding or legal pages could not be read. Where the parts above already fit, the wiring is a Beast or AIVDM or APRS-IS output that the user points at THEIR OWN feeder program, local program or account; FoxSDR never holds a service credential except the APRS-IS passcode the user types.

## WIRING HAND-OFF (what the wiring lane changes; none of it was done here)

Common to all parts: the new `.cpp` files are compiled into `cascade_engine` already (the glob), so linking needs nothing. Each output is a plain object with `start(error)`, `stop()` and a wait-free producer call; the engine owns one object per enabled output. The settings (below) are strings the user types; every `start()` error is a plain English sentence meant to be shown. **Off by default, loopback by default, `0.0.0.0` only when the user ticks a clearly worded box.**

1. **TCP fan-out.** Nothing to wire on its own. Files the wiring touches: `src/engine/engine.cpp` (own the outputs; stop them in the teardown before `beginShutdown()`'s window, join budget about 20 ms), `src/core/config.{hpp,cpp}` (settings), `src/engine/fox_engine_api.cpp` if the controls are API ops, `src/gui/*` and `src/slint/*` for the panel, `src/core/thread_beats.hpp` registration (see `cat_server.cpp`). Settings per output: enable, address (default 127.0.0.1), "allow other computers" (all interfaces), port, client limit.
2. **Aircraft.** Needs the ADS-B plugin's data. **The plugin interface lacks:** an output of RAW frames with 12 MHz timestamp and signal level (Beast needs them), and decoded fields beyond a `CascadeTrack` for SBS: the track carries id, label, lat, lon, altM, courseDeg, speedMps, ageMs and flags (an emergency flag), which covers MSG 2/3/4's altitude, position, track and ground speed, but NOT vertical rate or the squawk code, and no frame-type distinction. Options: (a) a new host-API call, e.g. `emit_frame(ctx, const uint8_t* raw, size_t len, uint64_t ts12MHz, uint8_t level)`, appended to `CascadeHostApi` under the usual `CASCADE_HOST_HAS` rule and a new capability bit, called from `adsb_plugin.cpp` where it already builds `adsb_feed` frames; or (b) the plugin keeps its own Beast feed (it already has one) and the host only offers SBS built from tracks. The plugins repo is the other lane's. SBS: `SbsMessage` from a track: `hexIdent` = the track id, `latitude`/`longitude`, `altitudeFt` = altM x 3.28084, `trackDeg` = courseDeg, `groundSpeedKt` = speedMps x 1.94384, time from `unix_time_ms`; MSG 1 from the label. Settings: Beast enable and port, SBS enable and port (the two ports differ), plus the shared bind fields.
3. **AIS.** The AIS plugin must hand over raw message bits (it decodes them): same gap as above, a `emit_ais(ctx, const uint8_t* bits, size_t nbits, char channel)` or a poll that returns bits. Files: the plugin's `ais_plugin.cpp`, the host table, `engine`. Settings: TCP enable and port (10110), UDP enable, UDP destination address and port, channel handling. The UDP destination is a typed IPv4 address; broadcast needs its own checkbox.
4. **APRS-IS.** The APRS plugin must hand over heard packets as TNC2 text and a flag for "frame had control 0x03 and PID 0xF0" (`GateInput`). Receive direction: the handler's lines are APRS-IS packets; the host can feed them to the APRS plugin's map as if heard (that needs a plugin entry point to accept packets: not in the ABI). Settings: server host (no default), port (14580), callsign, filter, "Send heard packets to APRS-IS" (the gateway switch, off), passcode (a password-type field, stored like other secrets, never logged), qAR/qAO choice, a status line from `aprsStateName` and the last error. Warnings: gateway on means the user's callsign is on the public network; the licence and callsign belong to them.
5. **I/Q.** Needs a tap on the engine's sample path: the full-band block from the source (before the channel filter) or the tuned channel after decimation; call `IqOut::submit` from that thread. Files: `src/engine/engine.cpp` (and the pass that owns the DSP thread), `scope_tap.hpp` is a model of an existing tap. Settings: enable, TCP or UDP, destination (UDP), port, which stream (channel or full band), format (8-bit, 16-bit, float), "include header", plus a live drops counter. Warn about 76.8 MB/s at float 2.4 MS/s.

## Strings for translation (not added to any catalogue: no UI exists yet)

`start()` errors: "already running", "client limit must be between 1 and 64", "per-client queue must be at least 1024 bytes", "greeting does not fit the per-client queue", "poll interval must be between 1 and 1000 ms", "'%s' is not an IPv4 address (dotted quad, for example 127.0.0.1)", "0.0.0.0 would expose this stream on every network interface; it is refused unless that is asked for explicitly", "could not listen on %s:%d (in use, or not an address of this machine)", "destination port must be 1..65535", "datagram size must be between 16 and 65000 bytes", "0.0.0.0 is not a destination", "broadcast and multicast destinations are refused unless they are asked for explicitly", "unknown sample format", "the header needs a sample rate and a finite centre frequency", "chunk size must be at least one sample", "a chunk does not fit in one datagram; use fewer samples per chunk or a larger datagram size", "a chunk does not fit the per-client queue or the ring; use fewer samples per chunk", "no APRS-IS server has been chosen", "the port must be 1..65535", "a timing value is out of range", "the callsign is not valid for APRS-IS (3 to 9 characters, letters and digits, an optional -SSID of one or two characters, not -0)", "the software name and version must each be one printable word with no spaces", "the filter is too long or contains a line break", "the filter may hold only printable ASCII", "gateway mode needs the passcode you were issued for this callsign (digits only); none was supplied", "the login line would exceed 512 bytes". APRS last-error texts: "could not resolve the server name", "could not start a connection", "the server did not accept the connection", "the server closed the connection", "the connection failed", "the connection failed while writing", "the server sent a line far longer than the 512 bytes the protocol allows; the connection was dropped", "nothing was heard from the server (not even a # keep-alive line) for too long; reconnecting". State names (`aprsStateName`): stopped, connecting, waiting for the server, logged in, online, waiting to reconnect. Gate reason names (`aprsGateReasonName`): gated, "not a UI frame (control 0x03, PID 0xF0)", "TNC has PASSALL on", "malformed packet", "TCPIP, TCPXX, NOGATE or RFONLY in the header", "q construct or I in an RF header", "generic query", "third-party packet that came from the internet", "nested third-party packet", "line too long", "gateway is off", "not logged in", "login not verified", "queue full". These are English diagnostics; the wiring decides which become catalogue keys (the project catalogue has 2522 keys in every language; `tools/i18n_keys.py` was not run: nothing here adds a key).

## UNVERIFIED

* SBS field positions, per-type field sets beyond the secondary facts, date and time text, `-1`/`0` flags and CRLF: the primary page could not be read. One real capture from a running BaseStation-compatible source would settle all of it.
* The APRS-IS server's login acknowledgement text, and the callsign length/SSID rules (only the q page's 8-hex rule and the Connecting page's `-1`, `vers`, `filter`, 512-byte and `#` rules were read by me).
* Whether any aggregator accepts a direct Beast connection from FoxSDR (several document only their own feeder scripts).
* All five outputs against a real receiving program (every peer was a stand-in on loopback). In particular: Beast against a real map or feeder, SBS against a real BaseStation client, AIS against a chart plotter, `#` handling against a live APRS-IS server, the I/Q stream against a real consumer.
* The 8-bit offset convention against specific programs.
* Behaviour on a non-loopback interface and under Wi-Fi loss (only loopback was used).
* Linux and macOS builds (the shim has POSIX branches that were not compiled here).
* The default ports 30005, 30003, 10110 and 1234 as conventions.

## Owner decisions

1. `qAR` or `qAO` for the gateway. The brief asks for qAR; the IGate Details and q pages say a receive-only IGate must use qAO, and this client never gates internet traffic to RF, so it is receive-only in that sense. Implemented as a setting, default **qAR as asked**; the safer default by the page is qAO.
2. Which APRS-IS server (or the round-robin name) the default points at: none is built in.
3. Whether the plugin ABI grows the three raw-data calls (frames, AIS bits, APRS packets) or the plugins keep their own feeds.
4. Whether I/Q out is allowed to listen beyond loopback at all in a shipped build (an unauthenticated 77 MB/s stream), and whether the 28-byte header stays (the headerless stream is what most programs expect).
5. Which aggregators to document in the app as "point their feeder at this port": the terms report above lists only what the services' pages show; twelve of sixteen had at least one unreadable page.

## Results

New tests, all together, run twice (`ctest -R`, -j6, Release): both runs 6 of 6 passed (test_aircraft_out 0.2 s, test_ais_out 0.7 s, test_tcp_fanout 6 to 7 s, test_aprs_is 13 s, test_iq_out 29 to 30 s, test_thread_inventory 7 s). Check counts: test_tcp_fanout 1401, test_aircraft_out 170,401, test_ais_out 9,527, test_aprs_is 360, test_iq_out 388, test_thread_inventory 9,961.

**One flake found by the together-run and fixed**: `test_ais_out` failed once in a parallel run (`sink.stats().sent > 0` after a fixed 50 ms; the sender thread had not been scheduled). Reproduced 4 times in 8 runs with two other tests running beside it, fixed by waiting (bounded) for the first datagram before stopping, then 10 of 10 clean under the same load.

**Headless gate** (`ctest --test-dir D:/foxsdr-build/feat-net -C Release -L "headless|slint" -j4 --timeout 300`, 689 tests, 1988.8 s): **98 % passed, 15 failed**, classified:

| Failed | Class |
|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | known false failures on D: builds |
| test_thread_inventory | **REAL, caused by this branch**: the three new worker threads had no row. Fixed with three exempt rows in `tests/thread_inventory_rows.inc` (commit eeba5309); now 9,961 checks, 0 failed |
| test_bookmark_save_async_stepped, test_tx_audio, test_window_threaded_look | load flakes: passed alone (32.1 s, 15.9 s, 7.4 s) |
| test_excuse_cap (check at line 391, `reportsWritten() == 1`), test_failure_files, test_window_failed_app, test_health_app_stepped (timeout at 120 s), test_pass_control, test_plugin_unload_handshake | **not in the known list; failed again when run alone**. They exercise the application's hang reports, failure files, windows and plugin unload; none includes or calls any file this branch adds. NOT re-run on the base commit (no base build here), so whether they fail there too is UNVERIFIED; the machine was running other agents' builds and tests at the same time (several ctest and MSBuild processes seen), which is the likeliest cause for timing-sensitive tests of this kind, but that is a guess, not a result |

test_shutdown_budget and test_shutdown_budget_free: both Passed (93.4 s and 93.6 s), so the claim that no `std::chrono` wait constant was added held. test_thread_inventory was the table the new threads did need.

Build: the whole tree built (`cmake --build ... --parallel 4`, 556 test executables, 0 compile errors); the new files raised no warnings after one MSVC C4324 padding warning was silenced by a pragma in `spsc_ring.hpp`.

