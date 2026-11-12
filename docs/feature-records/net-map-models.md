# Network services and map models (2026-11-11)

Branch `feat/net-map-models` (local, cut from 266926bf, not pushed, not merged, no version bump). Ten survey rows (N21 two parts, N16, M2, M3, M9, Q5, M8, M5, Q7a, M1b two parts)
of `D:/foxsdr-build/prep/feature-gaps-survey.md` as **stand-alone header-only blocks with tests**. **Nothing is wired**: no existing source file was edited, and not even
`tests/CMakeLists.txt` (its glob picked the new `test_*.cpp` up after a re-configure). Conventions followed from `signal-blocks-2.md`: header-only, `tests/test_check.hpp`,
real numbers printed as `INFO` lines, a break-it table per part, a WIRING HAND-OFF. The survey's file references were treated as hypotheses and checked (see "What existed").

| Row | What | Header(s) | Test | Checks |
|---|---|---|---|---|
| N21a | Prometheus text-format metrics + handler | `src/net/metrics.hpp` | `tests/test_metrics.cpp` | 123 |
| N21b | Webhooks (queue, retries, time-outs, http only) | `src/net/webhook.hpp`, `src/net/webhook_http.hpp` | `tests/test_webhook.cpp` (+ `tests/netmap_stand_in.hpp`) | 221 |
| N16 | MCP server core + loopback listener | `src/net/mcp_server.hpp`, `src/net/mcp_http.hpp` | `tests/test_mcp_server.cpp` | 522 |
| M2 | ADS-B heat grid + 24 h range outline per altitude band | `src/core/adsb_coverage.hpp` (+ `src/core/safe_file_io.hpp`) | `tests/test_adsb_coverage.cpp` | 552 |
| M3 | Track store for timeline replay | `src/core/track_store.hpp` | `tests/test_track_store.cpp` | 201 |
| M9 | Tile cache (viewed tiles only) | `src/core/tile_cache.hpp` | `tests/test_tile_cache.cpp` | 638 |
| Q5 | Level against GPS position | `src/core/rf_position_log.hpp` | `tests/test_rf_position_log.cpp` | 760 |
| M8 | Sun, Moon, radio sources az/el | `src/core/sky_position.hpp` | `tests/test_sky_position.cpp` | 770 |
| M5 | Lookup links | `src/core/lookup_links.hpp` | `tests/test_lookup_links.cpp` | 578 |
| Q7a | Antenna calculators | `src/core/antenna_calc.hpp` | `tests/test_antenna_calc.cpp` | 54 |
| M1b | Maidenhead 4/6/8 + distance/bearing; APRS path hops | `src/core/locator.hpp`, `src/core/geo_math.hpp`, `src/core/aprs_path.hpp` | `tests/test_locator.cpp`, `tests/test_aprs_path.cpp` | 74 + 71 |

Commits (`git log 266926bf..HEAD`): one per part (b8c18489 locator, 0d10ee32 APRS, 08bc770d antenna, 068a3836 links, 9640ef7e sky, 54964698 ADS-B, 07ba2e0e tracks, 52bb09cc tiles,
af488676 RF position, 32b5da94 metrics, 77ca399a webhooks, 60a8ac96 MCP) plus three follow-ups that closed gaps the break-it runs found (60386f98, 05af44e9, 3e340bcf).
The MCP commit message says "over 90 forbidden tool names"; the test prints **86** (`INFO forbidden tool names tried: 86`) plus 26 forbidden method names: 86 is the true count.

## What existed already (searched before each part)

| Part | Search result |
|---|---|
| Metrics, webhooks, MCP | Nothing: no `prometheus`, `/metrics`, `webhook`, `jsonrpc` or `modelcontext` anywhere in `src` or `tests`. |
| HTTP clients (the webhook TLS question) | **TLS clients exist but none is reusable**: `core/telemetry.cpp`, `core/crash_upload.cpp`, `core/feature_request.cpp`, `core/plugin_repo.cpp` post over WinHTTP on Windows and cpp-httplib built with `CPPHTTPLIB_OPENSSL_SUPPORT` elsewhere (`CMakeLists.txt` comment at the `cascade_httplib` target), each inside its own `.cpp`. `tests/http_post_stub.hpp` is a Windows-only loopback stub for those. No TLS was added here. |
| Web server pattern | `net/web_server.cpp` registers routes on a `httplib::Server` (`svr.Get("/api/status", ...)` near line 3525) and refuses an unsigned request with `deny(res, "sign in first")`; `tests/test_web_server.cpp` starts it on a free port and uses `httplib::Client`. The new tests reuse that pattern. |
| Maidenhead | `core/maidenhead.hpp` has `maidenheadGrid6` (lat/lon to 6 characters only, for the plugin receiver-locator capability) and `src/slint/globe_logic.hpp` finds a locator in text. No decoder, no 4/8 characters, no distance. Left untouched; the new file is `core/locator.hpp`. |
| Great circle / coverage | `src/gui/track_metrics.hpp` has `greatCircleKm` (R = 6371), `initialBearingDeg`, `destinationPoint`, altitude colour bands and **`CoverageMap`: 72 five-degree buckets of the greatest range, session only, "NOT PERSISTED, deliberately"**, no altitude bands, no heat grid, no 24 h window. It is the desktop's current consumer (`map_view.cpp`). Not touched. |
| Solar system | `src/slint/solar_ephemeris.hpp` (3D orbits view) computes planet positions from JPL elements and the Moon's direction and distance from a Meeus series, verified against JPL Horizons. It gives no observer azimuth / elevation and no radio sources, so M8 is new; the screen lane may want one shared Moon series (hand-off 8). |
| NMEA | `core/nmea.*` (parser, line assembler) and `core/gps_reader.*` exist and are reused by Q5. |
| Tile cache | `net/web_server.cpp` keeps an in-page JavaScript `tileCache` Map and the server holds plugin-supplied tiles in memory (`setTile`, `takePendingTileRequests`); basemap plugins fetch tiles themselves. **No disk cache, no host-side fetch.** |
| ADS-B decoder | No decoder in this tree (the plugin lives elsewhere). `D:/foxsdr-samples/MANIFEST.md` lists the ADS-B recordings (`dump1090-modes1` 0.178 s of 2 MS/s, `sdrangel-adsb-2400k` 102 s) as "NOT decoded in FoxSDR (no headless route found)". No decoded positions exist on disk. |
| Antenna formulae, lookup links, track store | Nothing. (`track_metrics.hpp` has track *rows*, not a time-indexed store.) |

## Sources, and what is and is not verified (all read 2026-11-11 unless stated)

| Fact | Source | Status |
|---|---|---|
| Prometheus text format 0.0.4: content type, HELP / TYPE, escaping (`\\`, `\n` in HELP; `\\`, `\"`, `\n` in label values), NaN / +Inf / -Inf, one group per metric, unique series | https://prometheus.io/docs/instrumenting/exposition_formats/ | verified; the page does not give the name regexes ("usual restrictions"): the long-standing ones are used (UNVERIFIED as quoted text) |
| MCP revision **2026-07-28** (the page served as "latest"): stateless `_meta` (protocolVersion + clientCapabilities required, -32602 / 400), `server/discover`, `tools/list`, `tools/call`, `resultType`, notification = 202, unknown method = 404 + -32601, -32022 UnsupportedProtocolVersion with `data.supported`, -32020 HeaderMismatch, `MCP-Protocol-Version` / `Mcp-Method` / `Mcp-Name` headers with the `=?base64?...?=` form, Origin validation (403), bind to 127.0.0.1, no batches, servers MUST validate inputs and rate-limit tool calls | https://modelcontextprotocol.io/specification/latest (Overview), `/2026-07-28/basic.md`, `/basic/transports/streamable-http.md`, `/basic/versioning.md`, `/server/tools.md`, `/server/discover.md` | verified |
| MCP tool annotation names (`readOnlyHint`, `destructiveHint`, `idempotentHint`, `openWorldHint`) for this revision; HTTP status for an unknown tool (200 is used); that `Accept` must also list `text/event-stream` (not required here: no SSE is ever opened) | the tools page says only "optional properties describing tool behavior" | **UNVERIFIED** |
| HTTP caching: freshness precedence (max-age, then Expires minus Date, then heuristic), invalid Expires = expired, `max-age` = 1*DIGIT, non-integer = stale, `no-store`, `no-cache`, conflicting directives = most restrictive | RFC 9111 (June 2022) https://www.rfc-editor.org/rfc/rfc9111.html, sections 4.2.1, 5.2.2, 5.3 | verified **but only the first 100 000 of 107 472 characters of the page were read** (the tail was not) |
| OSM tile policy: no bulk download, no prefetch, **"offline use is not permitted on tile.openstreetmap.org"**, honour cache headers, at least 7 days when they cannot be read, unique User-Agent | https://operations.osmfoundation.org/policies/tiles/ (no date on the page) | verified (as summarised by the fetch tool) |
| Maidenhead structure (A-R / 0-9 / A-X / 0-9; 20x10 deg, 2x1 deg, 5'x2.5', 30"x15"; FN31pr contains 41.71463 N 72.72713 W; a subsquare is under 10.4 km) | https://en.wikipedia.org/wiki/Maidenhead_Locator_System | verified; the page did not carry the Munich example, JN58td was derived from its rules (cell SW corner 48 07.5 N 11 35 E) |
| Sun: approximate coordinates, "about 1 arcminute" | USNO https://aa.usno.navy.mil/faq/sun_approx | verified |
| Moon: orbital elements, 12 / 5 / 2 perturbation terms, topocentric parallax, horizontal conversion, "1 to 2 arcminutes" | P. Schlyter https://stjarnhimlen.se/comp/ppcomp.html | verified |
| March equinox 2025-03-20 09:01 UT | https://en.wikipedia.org/wiki/March_equinox (cites USNO; the USNO page itself returned HTTP 500) | verified second-hand |
| Eclipse 2024-04-08: greatest eclipse 18:18:29 UTC at 25 18 N 104 06 W, magnitude 1.0566 | https://en.wikipedia.org/wiki/Solar_eclipse_of_April_8,_2024 | verified |
| J2000 positions of Cas A, Cyg A, Crab, M87, Sgr A* | the five Wikipedia articles (Crab: the header line is used, its table says 05h34m31.8s +22 01 03) | verified as printed |
| Sidereal time `280.46061837 + 360.98564736629 (JD - 2451545)`; IAU 1976 precession angles (zeta, z, theta); annual precession constants 3.075 s / 1.336 s / 20.04" | from memory | **UNVERIFIED as printed numbers**; the test checks the sidereal constant against the equation of time from the USNO page (agreement 0.1 to 3.1 s; 11 Feb: found +855 s, the published equation of time that day is about -14m15s) and the precession against the annual constants (69.4 s vs 69.2 s, 0.1397 vs 0.1397 deg) |
| Dipole 143 / f m and 468 / f ft, k 0.94 to 0.98; quarter-wave monopole = half a dipole; FSPL +32.45 (km, MHz) and the 120.05 dB worked example; coax VF 0.66 / 0.77-0.86 / 0.93 | Wikipedia "Dipole antenna", "Free-space path loss", "Velocity factor" | verified (the Dipole page: first 100 000 of 144 266 characters read) |
| Yagi starting lengths (0.495 / 0.473 / 0.440 wavelength, spacings), radials 5 % longer | memory of amateur designs | **UNVERIFIED**, labelled as starting values |
| APRS-IS q constructs and their place in the path | http://www.aprs-is.net/q.aspx | verified |
| APRS `*` marks the element and every one before it as used; WIDEn-N semantics | memory of the APRS protocol reference | **UNVERIFIED** |
| Radio horizon 4.12 (sqrt(h1) + sqrt(h2)) km | memory (4/3 earth radius); used only to GENERATE the synthetic ADS-B day | UNVERIFIED, not a pinned behaviour |
| All ten lookup-link URL patterns | memory; no site documentation read | **UNVERIFIED, every row marked so in the result** |

No program source was read (clean room). No file was downloaded; web pages were read through the fetch tool. No outside service was contacted by any test (every network test runs against stand-ins on 127.0.0.1).

## Part 1: Prometheus metrics (`net/metrics.hpp`)

**Built.** `MetricsRegistry` (counters and gauges, families defined first, `inc` / `set` / `add` / `setCounter` for the engine's own totals, `render()`), `MetricsHandler::handle(method, target, authorization)` returning status, content type and body, and
`StandardMetrics::publish(EngineSnapshot)` for the five metrics asked for: `foxsdr_decodes_total{decoder}`, `foxsdr_dropped_samples_total`, `foxsdr_signal_level_dbfs`, `foxsdr_dsp_thread_cpu_ratio`, `foxsdr_connected_clients`.
Bounds: 64 series per family, 1024 in all, 64 families, 4 labels per series, 128-byte label values (cut on a UTF-8 boundary); a dropped sample is counted and exported as `foxsdr_metrics_dropped_series_total`. Invalid UTF-8 and control bytes are replaced by U+FFFD.
Optional bearer token (constant-time compare) because a Prometheus scraper cannot use the cookie sign-in; with no token the handler adds no authentication of its own (the web server's sign-in gate in front is the control).

**Proof.** Golden text; an independent reader written from the format document (names, one HELP / TYPE per family, TYPE before samples, contiguous groups, unique series, escapes, value syntax) run on the output of hand-made, escaping, value-format and 3000 random-byte label cases; handler table; **100 scrapes over a real loopback HTTP server while a writer thread updated the registry: 100 valid, 0 bad** (`INFO scraped 100 times ...`).

Break-it (one mutation at a time, header restored from git after each; "caught by" is the first failing check):

| Behaviour | Mutation | Result |
|---|---|---|
| Label value `"` escaped | escape removed | 6 checks fail (`t_total{d="a\"b\\c\nd"}` text, parser) |
| HELP line feed escaped | raw newline kept | 2 fail (HELP text, parser) |
| Cardinality bound | bound removed | 5 fail (64 / 936 / dropped counter) |
| Counter vs gauge type | check removed | 3 fail (WrongType) |
| Control bytes replaced | rule removed | **first run: NOT caught**; check for raw control bytes added (`05af44e9`); now 1 fails |
| NaN spelling | `nan` | 1 fails |
| Bearer token compared | comparison removed | 3 fail |
| Method restriction (405) | removed | 2 fail |
| Counter refuses negative increments | removed | 1 fails |

## Part 2: webhooks (`net/webhook.hpp`, `net/webhook_http.hpp`)

**Built.** `parseUrl` (http only; https only if the transport says so; refuses credentials, whitespace and control characters, non-ASCII hosts, bad ports, over 2048 bytes, every other scheme, and drops a fragment), `eventToJson` (kind, title, detail, time, freq_hz, source; cut to 16 KiB; invalid UTF-8 replaced so it cannot throw), `WebhookSender` (bounded queue that drops the **oldest**, one worker, retries on network error, time-out, 408, 429, 5xx with doubling back-off to a cap, other statuses permanent, **redirects never followed**, `stop()` wakes a back-off at once) and `HttpTransport` on the vendored cpp-httplib. Off by default (empty URL: no thread, no network).

**Proof (real transport, loopback stand-ins).** 200 delivered with the exact body, `Content-Type: application/json`, `User-Agent: FoxSDR-webhook/1`, order kept for 21 events. Retries: 500, 500, 200 gave gaps of **93 ms and 167 ms (base 80 ms)**; six attempts with base 20 ms and cap 50 ms took 260 ms. Statuses: 17 rows (500 502 503 599 408 429 retried; 400 401 403 404 410 413 301 302 307 100 199 not). A 302 with `Location` was not followed (no request ever reached the target). **Slow receiver** (700 ms answers, 150 ms time-out): time-outs counted, first event gave up after **364 ms**; **1000 `enqueue` calls while the worker was stuck took 0 ms in total, worst single call 4 us**, 900 oldest dropped. **Receiver closes early / reads then closes / answers garbage / never answers**: 3 attempts each, 3 network errors (3 time-outs for the black hole, 719 ms). Nothing listening: refused, 2 attempts. **Receiver comes up late**: delivered by a retry. Queue: 501 events into 10 slots behind a held post: 490 dropped, events 491 to 500 delivered in order. `stop()` during a 5000 ms back-off took **0 ms**. 4 threads x 2000 events into 50 slots: 150 delivered + 7850 dropped = 8000, none lost.

Break-it:

| Behaviour | Mutation | Result |
|---|---|---|
| Drop the oldest | drops the newest | 1 fails (delivered order 491..500) |
| Which statuses retry | everything retries | 26 fail |
| Back-off doubles | constant | 3 fail (gap2 >= 150, gap2 > gap1) |
| Attempt cap | removed | 25 fail (never gives up; run ends on the 240 s cap) |
| https refused without TLS | check removed | 5 fail |
| Credentials in URL refused | check removed | 3 fail |
| `stop()` interrupts a back-off | wait ignores stop | 1 fails (`took < 500`) |
| Redirects not followed | `set_follow_location(true)` | 3 fail |
| Read time-out honoured | removed | 7 fail and the run hit the 240 s cap (exit 124) |

## Part 3: MCP server (`net/mcp_server.hpp`, `net/mcp_http.hpp`)

**Built to revision 2026-07-28** (see Sources). Handler `McpServer::handle(body)` -> `Reply{status, body}`: `server/discover`, `tools/list`, `tools/call`; `McpHttpPolicy` (Host and Origin must be loopback, optional bearer token, POST only, content type, Accept, body cap, the three `Mcp-*` headers incl. the base64 form); `McpHttpServer` binds **only** a loopback address (the single start call takes a bind address and refuses anything else before opening a socket; "localhost" resolves to 127.0.0.1; 4 worker threads; 66 KiB library payload cap). **Off by default** (`HttpPolicyConfig::enabled == false` answers 404 to everything).

**Exactly five tools**: `get_status`, `list_bookmarks`, `recent_decodes` (read) and `tune`, `set_mode`. The `McpHost` interface the wiring implements has exactly those five operations, so there is no code path to transmit, keying, any file write, any setting, any listener or any plugin: the test class implementing the five is not abstract, and a sixth pure virtual would stop the test compiling. Limits: body 64 KiB, nesting 32, strings 256 bytes, frequency 1 kHz to 7.2 GHz (the host may refuse more), mode one of the eight names of `core/demod_names.hpp` exactly, at most 200 decodes / 500 bookmarks, text results cut to 512 bytes, tool calls limited by a token bucket (burst 20, 10 a second).

**Proof (a host that counts every call it receives).**
- **86 forbidden tool names** (transmit, tx, ptt, key, record, start_recording, write_file, save, set_setting, set_config, enable_web_server, start_server, listen, bind, set_bind_address, set_password, exec, shell, load_plugin, install_plugin, upload, scan, shutdown, case and whitespace variants, `tune\0transmit`, `tune/../transmit`, `tune;transmit`, 100 000-byte names ...) and **26 forbidden methods** (resources/*, prompts/*, sampling/*, tasks/*, subscriptions/listen, initialize, ping, ...): every one refused (-32602 "Unknown tool", -32601 with HTTP 404, 413 for the oversized one), **host call count unchanged: 0**. A notification that names `tune`, `set_mode` or `transmit` is answered 202 and runs nothing.
- A 40-row argument table (missing, wrong type, out of range, `transmit` / `ptt` / `power_w` / `path` / `__proto__` extras): all `isError`, host untouched.
- Envelope: malformed bodies, batches, non-objects, bad ids (null, float, bool, array, object), missing / wrong `_meta`, unsupported version (-32022 with `supported`), a body over 64 KiB -> 413, 60 000 '[' and 33-level nesting -> 400, 3000 random bodies and every single-byte mutation of a good `tools/call` give valid JSON replies or none.
- HTTP policy: 11 foreign Hosts and 11 foreign Origins -> 403 (a good token does not excuse a bad Host), header mismatches -> 400 / -32020 (tool named `transmit` in the body while the header says `tune` never reaches the host), rate limit (a burst of 20 accepted, the next 10 refused, 5 back after half a second, never more than the burst), `GET` / `DELETE` -> 405.
- **Real loopback listener**: tools/list 200, `tune` reached the host with 162.55 MHz, Origin evil.example -> 403, a spoofed `Host: evil.example` header -> 403 (the client library let it be sent), `GET` / `DELETE` -> 405, 100 000-byte body refused, listener refused to start for 14 non-loopback or malformed addresses, `boundHost() == "127.0.0.1"`, restart works.
- A scan of the header's non-comment lines finds no file, process, library-loading or socket code (`ofstream`, `fopen`, `CreateFile`, `filesystem`, `popen`, `system(`, `LoadLibrary`, `socket`, `bind(`, `listen(` ...) and no `ptt` / `keying`.

Break-it:

| Behaviour | Mutation | Result |
|---|---|---|
| Closed tool allow-list | prefix match | **not caught alone** (an equivalent mutant: the exact-match dispatch below it still refuses with the same error); with the second check also removed: 9 fail |
| Unknown arguments refused | check removed | 12 fail |
| Notification never runs | treated as a request | 7 fail |
| Origin / Host validation | removed | 12 / 12 fail |
| `Mcp-Name` must match the body | removed | 7 fail |
| Mode enum | removed | 7 fail |
| Tune range | removed | 7 fail |
| Rate limit | removed | 5 fail |
| Body size cap | removed | 2 fail |
| Nesting cap | removed | 1 fails |
| Version check | removed | 2 fail |
| Batch refusal | removed | **not caught**: equivalent (the next rule refuses a non-object with the same -32600 / 400) |
| Bearer token | compare removed | 2 fail |
| Host exceptions contained | one catch changed | **not caught alone** (a second `catch (...)` remains); both changed: uncaught exception, test aborts |
| Output text cleaning | invalid bytes kept | **not caught**: equivalent for invalid UTF-8 (the JSON writer replaces it too); the 512-byte cut is pinned separately |
| list_bookmarks limit 500 | removed | 3 fail |
| Loopback-only bind | removed | 28 fail |

## Part 4: ADS-B coverage (`core/adsb_coverage.hpp`)

**Built.** Sparse heat grid (0.05 degree cells, weights that halve every 3 days, at most 200 000 cells, faded cells then the weakest tenth evicted) and the maximum range heard per 5 degree bearing bin per altitude band (below 5000, 5000, 10000, 20000, 30000, 40000 ft and above, unknown) over the last 24 hours kept in 10-minute buckets (window exact to 10 minutes). Refuses non-finite and out-of-range values, null island, > 1000 km, > 24 h stale or future. Atomic save, 64 MB capped load, damaged lines skipped and counted, wrong version refused, bearings dropped if the receiver moved more than 1 km. One mutex.

**Proof.** Bands, ageing (weight 10 -> 5 -> 2.5, gone after 12 half-lives), outline per bin and band, the 24 h window (kept at 23.8 h, gone at 25 h), refusals, bounded cells (20 000 positions into 100 cells), save / load round trip identical per band, truncated / junk / wrong-version / missing / 64 MB files, 4 writers + reader. **Synthetic day** of 300 000 candidate positions kept only if below the radio horizon: every band's maximum within 0.4 to 2.7 km of the horizon of the band's top, 72 of 72 bins filled:

| Band | Max range | Horizon at band top |
|---|---|---|
| below 5000 ft | 171.2 km | 173.9 km |
| 5000-9999 | 238.9 | 240.5 |
| 10000-19999 | 334.2 | 334.7 |
| 20000-29999 | 406.1 | 407.0 |
| 30000-39999 | 467.3 | 467.9 |
| 40000 and above | 495.1 | 495.5 |

**Real ADS-B data: NOT DONE.** No decoded positions exist on this machine and no decoder is in this tree (the manifest says the recordings were not decoded). The test looks for `D:/foxsdr-samples/adsb-decoder/decoded-positions.csv` (`unix_s,lat,lon,alt_ft`, receiver assumed at the sample page's lat 51, lon 0) and prints `SKIP` when it is absent, which is what ran.

Break-it: stale guard removed (2 fail), outline never ages (3), heat never fades (4), range cap removed (3), null island accepted (2), moved receiver keeps bearings (1), unbounded cells (2). All caught.

## Part 5: track store (`core/track_store.hpp`)

**Built.** Tracks of aircraft, ships, balloons, stations kept for 6 hours by default (60 s to 30 days), sorted, out-of-order inserts and same-time replacement, `at(kind, id, T)` with **great-circle interpolation** (antimeridian, pole, identical and antipodal points handled), hold 30 s then gone dark across gaps over 300 s, `snapshot(T)`, thinned `trail()`, three memory bounds (5000 points a track, 500 000 in all, 5000 tracks), atomic save and 128 MB capped load. Ids 1 to 24 of `A-Za-z0-9_./-`.

**Proof.** Exact / interpolated / held / dark table; 60 N 0 E to 90 E midpoint **67.79 N, 45 E** (a straight blend gives 60); 179 E to 179 W midpoint at +-180; over the pole; refusals (7 bad ids, 5 bad inputs); retention sweep; bounds (100 000 adds into 400 tracks / 5000 points); persistence with damaged files; 4 writers + reader; **replay of 200 tracks x 721 points over 100 random times: worst position error 2.2e-10 degrees, 20 000 interpolated samples**.

Break-it: hold forever (5 fail), interpolate across gaps (3), linear lat/lon (3), no retention (2), no per-track cap (1), same-time duplicate inserted (3), long ids (4), no track cap (4). All caught.

## Part 6: tile cache (`core/tile_cache.hpp`)

**Built.** Disk cache `root/<source>/<z>/<x>/<y>.tile`: validated keys (source `[a-z0-9][a-z0-9_-]{0,31}`, z 0..22, x and y below 2^z) and generated paths, image-only payloads (PNG, JPEG, WebP, up to 1 MiB), CRC-checked 32-byte header, size cap (default 256 MB) with least-recently-used eviction that survives a restart (file time), expiry from Cache-Control / Expires / Date per RFC 9111 (see Sources: private cache ignores s-maxage, invalid max-age = stale at once, conflicting max-age = stale, `no-store` = not stored and a copy removed, `no-cache` = stale at once, nothing at all = 7 days, capped at 30 days), `Fresh` / `Stale` / `Miss`, corrupt files deleted. **No fetching, no prefetch, no bulk download** (a scan of the header finds no network code).

**Proof.** HTTP-date cases (the RFC example 06 Nov 1994 08:49:37 = 784111777, 1 Jan 1970 = 0, invalid forms refused); a table of expiry cases; 18 hostile keys (traversal, upper case, slashes, NUL, 33 characters, z 23, x = 2^z, negatives) refused with nothing written outside the root; store / stale / no-store / no-cache / replace; LRU order; restart rebuild by file time; 8 kinds of damaged file (one flipped bit, truncated, empty, trailing byte, bad magic, absurd size, zeros) each returned as a miss and deleted; 4 threads on a capped cache with no torn read.

Break-it: no-store stored (4 fail), max-age ignored (12), evicts the most recent (4), no CRC (3), any payload accepted (4), x unchecked (4), stale served as fresh (3), invalid max-age fresh (7), `/` allowed in a source (2). All caught.

## Part 7: level against GPS position (`core/rf_position_log.hpp`)

**Built.** `onNmeaLine(line, hostTime)` through the project's `parseNmeaSentence` (checksum, status A / quality 1-5), `addFix`, `addReading(t, level_db, freq)` paired by time (linear between fixes up to 10 s apart, else the nearest fix within 5 s, else dropped), jump (> 400 m/s within 60 s), out-of-order and null-island fixes refused, 25 m cells (longitude step scaled by the first fix's latitude) holding count, **mean in linear power**, maximum, minimum; bounded (200 000 samples, 100 000 cells); numbers-only CSV (`samplesCsv`, `gridCsv`) written atomically.

**Proof from a generated NMEA track** (GGA + RMC at 1 Hz, a 280 s drive at 10 m/s, readings every 0.1 s, level = -30 - 20 log10(distance) + 0.5 dB noise, transmitter 300 m off the path): **2800 readings stored, worst pairing error 0.049 m** (NMEA text quantisation), **strongest cell mean -79.3 dB at 9 m from the closest approach**, mean level by distance ring -81.3 dB (0-500 m), -87.2 (500-1000 m), -91.6 (1000-1500 m); the cell mean, max and min equal an independent recomputation from the raw samples to 1e-9; CSV parses back; the mean of -10 and -20 dB is -12.6 dB (linear), not -15.

Break-it: mean of dB values (6 fail), no jump check (2), **latitude not interpolated (first run: NOT caught; the fixtures only varied longitude; fixed in `05af44e9`, now 1 fails)**, stale fix used (3), null-island fix (1). 

## Part 8: sky positions (`core/sky_position.hpp`)

**Built.** Sun (USNO), Moon (Schlyter series with equatorial parallax), five radio sources (Cas A, Cyg A, Taurus A, Virgo A, Sgr A*) precessed from J2000, `skyPositions(lat, lon, unix)` -> 7 bodies with az / el / RA / dec. No refraction (geometric elevation). **No Stellarium link.**

**Accuracy, against published values.** March equinox 2025 (09:01 UT): Sun declination **+0.0027 deg**, RA 0.006 deg (the page promises 1 arcminute = 0.017). Eclipse 2024-04-08 18:18:29 UT at 25.3 N 104.1 W: **Moon-Sun separation 0.047 deg** (published: under 0.015 deg; the Moon's formulae promise 1 to 2 arcminutes), Sun elevation 69.94 deg, azimuth 150.25. Sun transit at 0 E on five dates agrees with 12h minus the equation of time to 0.1 to 3.1 s (855 s on 11 Feb). Cas A at 51.5 N: elevation 20.45 to 82.55 deg (circumpolar limits 20.4 and 82.6); Sgr A* peaks at 9.48 deg (limit 9.5). Moon distance over April 2024: 56.18 to 63.63 Earth radii. The radio-source positions are checked for consistency (culmination, circumpolar limits, precession against the annual constants), **not against a published az/el**.

Break-it: the first run missed a 0.46 degree sidereal-time offset and removed precession (the eclipse compares Moon with Sun at one instant, so a common hour-angle error cancels); the transit and precession checks were added (`60386f98`) and now catch them (5 and 2 fail). Moon longitude term removed (2), no Moon parallax (1), Sun equation of centre removed (6).

## Part 9: lookup links (`core/lookup_links.hpp`)

**Built.** One table (`sites()`) of 10 patterns (ADS-B Exchange, Planespotters, FlightAware by ICAO; Flightradar24, FlightAware by registration; VesselFinder, MarineTraffic by MMSI; QRZ, HamQTH, Callook by call sign). **Every row is marked UNVERIFIED** (`verified == false`; no site documentation was read). Strict subject alphabets (ICAO = 6 hex; registration 2-10 of `A-Z0-9-`; MMSI = 9 digits; call sign 3-12 of `A-Z0-9/`), then percent-encoding of everything outside RFC 3986 unreserved; only https from the literal table text; invalid subject = no link. Nothing is fetched.

**Proof.** 578 checks including 20 000 random hostile subjects (every produced link passes a clean-URL check), `W1AW/P` -> `.../db/W1AW%2FP`, injection attempts (`&`, `#`, `?`, `%`, `@`, `../`, newlines, NUL, Unicode) all produce no link.

Break-it: no escaping (1 fails), ICAO length unchecked (2), a row claims verified (2), an http pattern (49 fail). All caught.

## Part 10: antenna calculators (`core/antenna_calc.hpp`)

**Built.** Wavelength, half-wave dipole (143 / f, end-effect factor 0.90..1.0, default 0.954), quarter-wave ground plane (radials +5 % UNVERIFIED), Yagi starting lengths (UNVERIFIED, labelled), coax length from velocity factor, free-space path loss (constant computed from c: 32.4478), far-field flag; units in the names, `std::nullopt` for non-finite, zero, negative or out-of-range input (10 kHz to 300 GHz).

**Proof.** 14.2 MHz dipole 10.07 m = 33.0 ft (468 / f); **10 km at 2.4 GHz = 120.05 dB** (page example), +6.02 dB per doubling; 100 MHz quarter wave at VF 0.82 = 0.615 m (page: 0.62 m).

Break-it: FSPL constant with 2 pi (2 fail), VF above 1 accepted (1), dipole factor unchecked (2), far field at 1 wavelength (1). All caught.

## Part 11: locators and APRS paths (`core/locator.hpp`, `core/geo_math.hpp`, `core/aprs_path.hpp`)

**Built.** `locator::decode` / `encode` for 4, 6, 8 characters (centre of the cell, error at most half a cell), `between` (distance, bearing); `geo::` haversine (R = 6371.0088 km), initial bearing, destination, wraps. `aprs::parseHeader` / `parsePath`: hops typed as digipeater, alias (WIDEn-N with requested / remaining), marker (TCPIP...), q construct, IGate, other; `*` marks used; `stationPath()` for drawing; 512 bytes, 16 elements, refuses rather than guesses.

**Proof.** FN31pr and JN58td cells; **20 000 random points x 3 lengths round-trip within half a cell**; the 10.4 km subsquare claim (the widest cell, on the equator, measures about 10.36 km); sphere geometry exact (111.195 km a degree, quarter meridian 10 007.54 km); London-Connecticut 5400 to 5650 km at 280 to 300 degrees. APRS: typical RF-to-Internet header, the `*` rule, all ten q constructs, a table of refusals, 20 000 random headers.

Break-it: locator corner not centre (5 fail), subsquare letters to Z (1), 5-character accepted (2), pole not folded (1), bearing not wrapped (3), longitude not wrapped (1); APRS used-only-on-starred (2), SSID 16 (1), element cap (1), q with `*` (1). All caught.

## Departures from the request, and why

1. **ADS-B real-data run not done** (Part 4): nothing to run on; the test skips when the file is absent.
2. **Webhook https is refused**, not supported: the project's TLS code is inside four `.cpp` files and is not callable from a new header; none added.
3. **MCP over Streamable HTTP only** (the transport this revision defines for loopback); **no stdio transport** was built (it would mean FoxSDR being launched by the client); no legacy `initialize` handshake, no subscriptions, resources, prompts or tasks.
4. `core/locator.hpp`, not `maidenhead.hpp`, because the old file exists and must not be edited.
5. No existing source file or CMake list was edited; the test glob needed a re-configure only.
6. Tile cache **does not offer offline use of stale tiles by itself**: it reports `Stale`; see owner decisions.
7. Track altitudes are metres; ADS-B coverage altitudes are feet (the band table is in feet); the wiring maps.

## UNVERIFIED list

- Every lookup-link pattern; Yagi lengths and the 5 % radials; the APRS `*` and WIDEn-N semantics; the sidereal constant and the precession angles / annual constants as printed numbers (their effects are tested); the 4.12 radio-horizon constant (generator only).
- MCP: annotation field names and the unknown-tool HTTP status for this revision; whether any client in use speaks 2026-07-28 (a client of an earlier revision gets -32601 on `initialize`).
- RFC 9111 and the Dipole page were read in part; the OSM page has no date; the Prometheus name regexes are the usual ones, not quoted.
- Nothing was compiled for Linux or macOS (MSVC Release only); `tests/netmap_stand_in.hpp` has a POSIX branch that was not built; no sanitiser run; no TSAN.
- The real transports were tested against loopback stand-ins only: no real Prometheus, no real webhook receiver, no real MCP client, no real GPS, no real ADS-B decode.
- Mounting on the web server, the sign-in gate, and the shutdown-budget effect of a webhook worker (`stop()` can wait one attempt, up to connect + read time-outs, 8 s by default) were not exercised.

## Owner decisions needed

1. **Metrics scraper authentication.** The web sign-in is a cookie session a scraper cannot use. Bearer token in settings (the handler supports it), a separate listener, or sign-in only?
2. **Webhook https.** Plain http sends alert text in clear. Build a TLS transport on the existing WinHTTP / OpenSSL code (a change to those files), or accept http and say so on the screen?
3. **MCP exposure.** Off by default and loopback only; should it ever need a token (the policy has one), and is exposing decoded text and positions to an AI assistant acceptable (it is personal data of third parties)? Should a legacy `initialize` handshake be added for clients on earlier revisions?
4. **Offline maps vs the OSM policy.** The policy forbids offline use on `tile.openstreetmap.org`. The cache stores only viewed tiles and honours headers; may the screen show a stale tile when the network is down, and should the default basemap be a server whose terms allow it?
5. **Coverage persistence.** The desktop's `CoverageMap` is deliberately session-only (one antenna in one place). The new model persists to a file; keep that, or reset on an antenna change button, or stay session-only?
6. **GPS trail privacy.** The level-against-position CSV is a location history: where it is stored, whether it is on by default (not), and a retention rule.
7. **Retention defaults.** Track store 6 h and 500 000 points; ADS-B heat half-life 3 days; tile cache 256 MB: confirm or change.

## WIRING HAND-OFF (what the engine, settings and screen lanes must do; nothing below has been done)

### 1. Metrics (`net/metrics.hpp`)
- **Web server** (`net/web_server.cpp`, route block near `svr.Get("/api/status", ...)`): add `svr.Get("/metrics", ...)` that calls `MetricsHandler::handle("GET", req.path, req.get_header_value("Authorization"))` behind the same sign-in check as `/api/status` (or the bearer token, owner decision 1) and copies status, content type and headers.
- **Engine / status path:** one `MetricsRegistry` + `StandardMetrics` owned by the web server host; a ~1 s timer fills an `EngineSnapshot` (decodes per plugin from a host-side tally of `RadioStatus::decoded` / the decode path, the pipeline's ring-drop counter, `RadioStatus::signalDb`, the DSP thread measurement from the existing measurement code (`core/engine_measure.*`, not read), the web session count) and calls `publish`.
- **Settings:** `metricsEnabled` (default false), `metricsBearerToken`. No new control op.
- **Strings:** "Metrics endpoint (Prometheus)", "Scrape token", "Off / On", "Scrape URL: http://<host>:<port>/metrics".

### 2. Webhooks (`net/webhook.hpp`, `net/webhook_http.hpp`)
- **Owner:** the engine (one `WebhookSender` with an `HttpTransport`), started when a URL is set and the feature is on, stopped in the shutdown path (**check the shutdown budget**, `docs/engine-stage4.md` 6.10 and `tests/test_shutdown_budget.cpp`: `stop()` can wait one attempt; lower the time-outs or detach if it does not fit).
- **Inputs:** `WebhookEvent` from the alert-rule lane (worktree feat-core) and the decode-event path (host API level bump); map rule name to `source`, decoder text to `detail`.
- **HTTPS:** implement `Transport` on top of `core/telemetry.cpp`'s WinHTTP code / the cpp-httplib OpenSSL build (owner decision 2).
- **Settings / ops:** `webhookUrl`, `webhookEnabled` (default false), event filter, "Send a test" (one `eventToJson` with kind "test" through `enqueue`). Read-back from `stats()`: delivered, dropped, gave up, last status.
- **Strings:** "Webhook URL (http://...)", "Send a test event", "Delivered / Dropped / Failed", the `urlErrorText` sentences ("only http:// URLs are accepted", "user names and passwords in the URL are not accepted", ...).

### 3. MCP server (`net/mcp_server.hpp`, `net/mcp_http.hpp`)
- **Implement `McpHost`** in the net layer from what the web server already holds: `status()` from the `RadioStatus` provider (`net/web_server.hpp`), `bookmarks()` from `RadioStatus::bookmarks` (name, freq, mode; no group today), `recentDecodes()` from `RadioStatus::decoded` (plugin + text only: time and frequency are not carried today, so `DecodeInfo::unixTime` would be "now"), `tune()` / `setMode()` through `net/web_control.cpp` (`centerHz`, `mode`) and `controlRequestToCommands` (`net/control_ops.cpp`) so one set of limits applies (`kMinCenterHz` 0 to `kMaxCenterHz` 6 GHz there; align `Config::minTuneHz` / `maxTuneHz`).
- **Start / stop** `McpHttpServer` with the other servers; settings `mcpEnabled` (default false), `mcpPort`, `mcpBearerToken`; show the URL and "loopback only".
- **Strings:** "MCP server for AI assistants", "Loopback only (127.0.0.1)", "Port", "Token", "Allows: read the receiver status, bookmarks and recent decodes; tune and change mode. It cannot transmit."

### 4. ADS-B coverage (`core/adsb_coverage.hpp`)
- Feed `add(time, lat, lon, altFt)` from the track path where plugin tracks reach the host (`core/plugin_api.cpp`, the same place `gui/track_metrics.hpp`'s `CoverageMap::record` is fed from `gui/map_view.cpp`); receiver position from the existing home position.
- Screen: `heat(now)` for a heat layer, `outlinePolygon(now, band)` for the outline, a band selector (labels from `bandLabel`); load at start and `save` on exit / every few minutes under the per-user data folder; a "Reset coverage" control (owner decision 5).
- **Strings:** "Coverage heat map", "Range outline (24 h)", the six `bandLabel` texts, "Reset coverage".

### 5. Track store (`core/track_store.hpp`)
- Feed from the same track path (kind from the plugin: aircraft, vessel, balloon / radiosonde, APRS station); screen: a timeline control calling `snapshot(T)` per frame, `trail()` for tails, `timeRange()` for the slider; settings `trackRetentionHours`, load / save file.
- **Strings:** "Replay", "Retention (hours)", "Now", "Play / pause".

### 6. Tile cache (`core/tile_cache.hpp`)
- **Basemap plugins fetch tiles themselves**, so the host never sees the bytes: the cache needs either a host API call through which the plugin's tile fetch is routed, or to live inside the plugin; the screen (or the host API) calls `get()` before a fetch and `put()` (with the response's Cache-Control / Expires / Date) after a tile has been drawn. Cache folder under the per-user data folder; size setting; "Clear tile cache".
- Decide stale-while-offline (owner decision 4); send the unique User-Agent the OSM policy asks for.
- **Strings:** "Map tile cache", "Size limit (MB)", "Clear cache".

### 7. Level against position (`core/rf_position_log.hpp`)
- Feed `onNmeaLine(line, hostTime)` from `core/gps_reader.*` (the reader already owns the port and the line assembler) and `addReading(t, level, freq)` from the pass thread next to the S-meter fill; screen: `cells()` as a heat layer, "Export CSV" calling `saveSamplesCsv` / `saveGridCsv`; start / stop control (default off, owner decision 6).
- **Strings:** "Log signal level against GPS position", "Cell size (m)", "Export samples (CSV)", "Export grid (CSV)", "No GPS fix".

### 8. Sky positions (`core/sky_position.hpp`)
- Screen page: `skyPositions(lat, lon, now)` with the receiver position setting; show az / el and "below the horizon"; elevation is geometric. Consider sharing the Moon series with `src/slint/solar_ephemeris.hpp`.
- **Strings:** "Sun", "Moon", "Cassiopeia A", "Cygnus A", "Taurus A (Crab)", "Virgo A (M87)", "Sagittarius A*", "Azimuth", "Elevation", "Below the horizon".

### 9. Lookup links (`core/lookup_links.hpp`)
- Track and bookmark context menus: `build(Subject, text)` and open the chosen URL with the host's existing open-URL call (not checked); show "may not open the right page" for unverified rows (all of them today); nothing is sent until the click.
- **Strings:** "Look up on ...", the site labels from the table, "Link pattern not verified".

### 10. Antenna calculators (`core/antenna_calc.hpp`)
- A Slint page with frequency in MHz and the six calculators; show feet beside metres (`metresToFeet`); label the Yagi result "starting lengths, tune with an analyser".
- **Strings:** "Antenna calculator", "Frequency (MHz)", "Half-wave dipole", "Quarter-wave ground plane", "Yagi starting lengths", "Coax length", "Velocity factor", "Free-space path loss", "Distance (km)", "Wavelength".

### 11. Locators and APRS paths (`core/locator.hpp`, `core/aprs_path.hpp`)
- FT8 / PSK Reporter spots: the DX-spots plugin or the globe code calls `locator::decode` for a marker and `between` for distance / bearing (`src/slint/globe_logic.hpp` already finds a locator in text; `core/maidenhead.hpp` stays for the receiver locator). APRS plugin: hand the packet header to `aprs::parseHeader`; the map draws `stationPath()` as lines between stations the track store knows.
- **Strings:** "Locator", "Path", "Digipeaters", "Gated by", "Hops: used / remaining".
