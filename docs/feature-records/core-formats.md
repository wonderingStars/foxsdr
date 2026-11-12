# Core formats, store, alerts, macros, benchmark (2026-11-11)

Branch `feat/core-formats` (local, cut from `266926bf` = `feat/signal-blocks-2`, not pushed, not merged, no version bump). Eight stand-alone
parts, all **header-only** in new files, each with its own test. **Nothing is wired**: no existing source file was touched; the one existing
file edited is `tests/CMakeLists.txt` (six lines, longer test time limits, see "Build and test"). Conventions are those of
`signal-blocks.md` / `signal-blocks-2.md` (`test_check.hpp`, `AllocProbe`, one test executable per `tests/test_*.cpp` picked up by the glob).

| Part | Row | Files | Test | Commit subject |
|---|---|---|---|---|
| 1 RF64 / WAV I/Q writer | P6 | `src/source/rf64_writer.hpp` | `tests/test_rf64_writer.cpp` | RF64 / WAV I/Q writer |
| 2 SigMF writer | P13 | `src/source/sigmf_writer.hpp` | `tests/test_sigmf_writer.cpp` | SigMF writer |
| 3 KML and GeoJSON export | M4 | `src/core/geo_export.hpp` | `tests/test_geo_export.cpp` | KML 2.2 and GeoJSON (RFC 7946) writers |
| 4 gqrx and CubicSDR bookmark import | C11 | `src/core/freq_import_foreign.hpp` | `tests/test_freq_import_foreign.cpp` | gqrx and CubicSDR bookmark import |
| 5 Decode database | L2 | `src/core/decode_db.hpp` | `tests/test_decode_db.cpp` | Decode database |
| 6 Alert rules, sink interface | L3, N24 | `src/core/alert_rules.hpp` | `tests/test_alert_rules.cpp` | Alert rules, matcher, rule file and notification sink interface |
| 7 Macros | N18 | `src/core/macros.hpp` | `tests/test_macros.cpp` | Macro model |
| 8 DSP benchmark | U13 | `src/core/dsp_benchmark.hpp` | `tests/test_dsp_benchmark.cpp` | DSP benchmark |

## What existed already, and what was left to other lanes

Searched in this tree (`266926bf`) before building:

| Part | Result |
|---|---|
| 1 | No RF64 or BW64 writer anywhere (`grep -ri rf64 src` finds only `source/iq_readers.hpp`, which READS RF64 / BW64). The recorder's own RIFF writer (`core/recorder.*`) is untouched. |
| 2 | No SigMF writer (the reader is in `iq_readers.hpp`; the only other hit is the word "sigmf" in a list of file extensions in `core/diag_log.cpp`). |
| 3 | No KML or GeoJSON code in `src` or `tests`. |
| 4 | The web lane's CHIRP CSV and SDR++ JSON importers are in `core/freq_import_formats.cpp` on `feat-web` (NOT in this tree); read only there for style (`readRecords`, `dropBom`, `trimmed`, result struct). Mine is a NEW file and a NEW result struct. No gqrx or CubicSDR import exists (the words appear only in a licence note in `source/aor_protocol.hpp`). |
| 5 | No decode store. |
| 6 | No alert rules, no toast code (`grep -ri toast src` is empty). |
| 7 | No macro model (the `macro` hits are C++ preprocessor macros and plugin-store text). `core/app_commands.cpp` has the command names; read, not edited. |
| 8 | No DSP benchmark (hits are in an unrelated Slint test and a protocol comment). |

NOT built, because another lane owns them: FLAC, filename templates, rollover, one file per transmission, the recordings browser, seek and loop,
IF recording, frequency lock, band stacking, VFO A/B, scanner models, the command-line parser, the UTC clock (lane `feat-rec`); CHIRP and SDR++
bookmark import (lane `feat-web`).

## Sources

| Fact | Source | Status |
|---|---|---|
| RF64: `RF64` id, `ds64` chunk first, 64-bit sizes and sample count, `0xFFFFFFFF` in the 32-bit sizes, a reserved `JUNK` chunk of the same size so promotion needs no data move | EBU Tech 3306 v1.0, https://tech.ebu.ch/docs/tech/tech3306v1_0.pdf, read 2026-11-11 for `signal-blocks-2.md` (not re-read here) | verified there |
| BW64 uses the same layout with the id `BW64` (ITU-R BS.2088) | not read | **UNVERIFIED**; the writer writes `RF64`, the reader accepts both |
| WAVE_FORMAT_EXTENSIBLE layout, PCM SubFormat GUID | Microsoft documentation as cited in `iq_readers.hpp` | verified there (the exact GUID printed value is noted there as from memory) |
| SigMF v1.2.6: files, `global` / `captures` / `annotations`, `core:` keys, datatypes, ISO 8601 UTC datetime | https://sigmf.org spec v1.2.6, read 2026-11-11 for `signal-blocks-2.md` | verified there |
| `core:version` string `"1.2.6"` is what a strict validator wants | no validator run | **UNVERIFIED** |
| KML 2.2: root, Document, Placemark, Point, LineString, MultiGeometry, Style, TimeStamp, `lon,lat,alt` order, `aabbggrr` colours, LineString needs two points | OGC KML 2.2 (OGC 07-147r2) from memory, NOT opened | **UNVERIFIED** against the schema, including the order of elements inside a Placemark |
| GeoJSON RFC 7946: `[lon, lat(, alt)]`, `geometry: null`, antimeridian advice (section 3.1.9), member order free | https://www.rfc-editor.org/rfc/rfc7946 from memory, NOT opened this session | **UNVERIFIED** against the text |
| XML 1.0 `Char` production | from memory | **UNVERIFIED**; the test's checker uses the same ranges |
| gqrx `bookmarks.csv` | web search 2026-11-11 ("gqrx bookmarks.csv format documentation ..."): no official description found; results were third-party tools (gqrx-scanner https://www.github.com/neural75/gqrx-scanner, gqrx-remote https://github.com/har5ha/gqrx-remote, a Google Groups thread "Band Plan File (BandPlanFile)") seen as search snippets only, not opened. One snippet showed comma-separated `frequency, mode, name` lines. The implemented layout is from the task text and memory | **UNVERIFIED** |
| CubicSDR `bookmarks.xml` | web search 2026-11-11 ("CubicSDR bookmarks.xml format ..."): only the release notes of 0.2.1-alpha1 (https://www.github.com/cjcliffe/CubicSDR/releases/tag/0.2.1-alpha1: file location, `.backup`, `.lastload`, `.failed` copies); no schema. Implemented from memory | **UNVERIFIED** |
| Perseus memory format | web search 2026-11-11 ("Perseus SDR software frequency memory bookmark file format description"): nothing about Perseus's own file. Found only station lists made FOR Perseus (EiBi / MWList CSV) and SDR Console's XML memories (https://www.sdr-radio.com/memories). | **NOT BUILT** |
| InfluxDB, CRC-32 (IEEE, polynomial 0xEDB88320), FNV-1a 32-bit, civil-from-days (Hinnant) | CRC and FNV from memory, checked by the tests only through round trips and a real hash collision; dates checked against known dates (2000-02-29, 2100-03-01, a Tuesday, a Thursday) | CRC value not compared with a reference vector: **UNVERIFIED** as the standard CRC-32 (it only has to agree with itself) |

No program source was read (clean room). Nothing was downloaded or installed; no outside service was contacted; web pages were read only as
search results.

## Part 1: RF64 / BW64 writer (`Rf64Writer`, `repairWavHeader`)

**What.** A WAV I/Q writer (two channels; S16, S24, S32 PCM or F32) that starts as a plain RIFF file with a reserved 28-byte `JUNK` chunk after
`WAVE` (same size as a `ds64` body). When the RIFF size would pass `0xFFFFFFFE` the header is rewritten **in place** as `RF64` (`ds64` with the
64-bit riff size, data size, sample count; data chunk size `0xFFFFFFFF`), so no data moves. The header is brought up to date after every
`headerUpdateBytes` (32 MiB default) and on `flushHeader()` / `close()`. `repairWavHeader(path)` recomputes the sizes of an unclosed file from
its length (drops a torn partial frame, promotes if needed) and refuses files that are not this writer's layout, leaving them untouched.
S24 and S32 are written as WAVE_FORMAT_EXTENSIBLE (PCM SubFormat, front left and right). Conversion clips at full scale and writes non-finite
input as zero.

**Numbers (real runs).**
- Round trips through `iq::openWav`, max error on a 0.5-amplitude tone: F32 0, S16 3.05e-5, S24 8.94e-8, S32 2.33e-10.
- **A real write past 4 GiB**: 1,076,899,897 frames of S16 (4 bytes each: 4.01 GiB of data, just over the 4 GiB boundary) written in **15.4 to 22.5 s idle**
  and **58.5 s** inside ctest on the loaded machine, promoted by the default limit (the test saw the file plain first, RF64 after), read back
  through the existing reader at frames 0, 1000, 0x3FFFFFFF, 0x40000000, 0x40000000 + 777 and the last 1000: all exact; deleted afterwards (checked).
  The test skips with a printed message when under 12 GiB are free in the temp folder and on `D:/foxsdr-build/coref-scratch`.
- Header update interval: a crash test with an update every 80,000 bytes and a 25,000-frame write found 16,384 frames readable before repair
  (the loss is at most one interval plus one 16,384-frame block), all 25,000 after `repairWavHeader`.

**Break-it table** (each: the behaviour removed in a copy of the header, the test run, the checks that went red).

| Behaviour removed | Result |
|---|---|
| promotion past the limit | 9 checks fail |
| `ds64` data size written wrong | 2 fail |
| RF64 data chunk size `0xFFFFFFFF` | 1 fails |
| periodic header update | 1 fails |
| repair drops the torn partial frame | survived at first (the reader floors frames); a check of the header's data size was added, then 1 fails |
| clipping | 2 fail |
| non-finite input as zero | 2 fail |
| repair refuses files without the reserved chunk | survived at first (a second check refused the file); a test with only the chunk id wrong was added, then 2 fail |
| sample-rate validation | 5 fail |
| plain RIFF size field | 1 fails |

**Threads.** One writer, one thread (the recorder worker). No concurrency to test.

**WIRING HAND-OFF.**
- Files the wiring lane changes: `src/core/recorder.cpp` / `recorder.hpp` (the I/Q recorder's file sink), the I/Q record settings in `core/config.*`,
  the record control and its file-size note in `src/gui/` and the Slint recorder page.
- Ops and settings: a setting "Recording format" (WAV 16-bit, 24-bit, 32-bit, float) with the container choice automatic (plain until 4 GiB, then
  RF64); `headerUpdateBytes` as an advanced setting; on start-up the recorder folder scan should call `repairWavHeader` for `.wav` files whose
  header size does not match their length (offer, do not do silently). Today's recorder writes a PCM16 / float32 plain RIFF; whether to keep it for
  small files is the lane's call.
- Strings: "Recording passed 4 GB: the file continues as RF64 (readable by most audio tools)", "Recovered recording %s: %llu samples", "Recording
  stopped: disk full".
- Centre frequency is NOT in a WAV (no standard field); the recorder should keep writing it in the file name (the reader's name guess reads it)
  or add a `.json` sidecar or use SigMF (part 2).

## Part 2: SigMF writer (`SigmfWriter`)

**What.** `<name>.sigmf-data` (cf32_le, ci16_le, ci8, cu8 from complex float, clipped, non-finite as zero) and `<name>.sigmf-meta` (`global`:
`core:datatype`, `core:sample_rate`, `core:version`, `core:description`, `core:author`, `core:recorder`, `core:hw`, `core:license`; `captures` with
`core:sample_start`, `core:frequency`, `core:datetime`; `annotations` with `core:sample_start`, `core:sample_count`, frequency edges, label, comment).
The metadata is valid from `open()` (written atomically, temp file then rename) and again on `flushMetadata()` / `close()`: the sample count is the
data file's length, so a crash leaves a complete readable recording. `addCapture` starts a new segment at the current sample (a retune); two at the
same sample replace each other; `addAnnotation` refuses a start past the written samples, unordered or non-finite edges, and more than 65,536.
JSON comes from nlohmann with the replace handler: quote, backslash and control bytes are escaped, invalid UTF-8 becomes U+FFFD.

**Numbers.** Round-trip max error on a 0.6-amplitude tone through `iq::openSigmf`: cf32_le 0, ci16_le 3.36e-5, ci8 0.00855, cu8 0.00392. Datetime
golden values include 2000-02-29, 2100-02-28 / 03-01 and the end of 2009-02-13T23:31:30.123Z.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| datatype name | 8 fail |
| invalid UTF-8 replaced | 2 fail |
| two retunes at one sample replace each other | 2 fail |
| annotation past the end refused | 3 fail |
| annotation edge ordering | 3 fail |
| clipping | 2 fail |
| `flushMetadata` rewrites the file | 2 fail |
| sample-rate validation | 5 fail |
| January / February year rollover in the date | 10 fail |
| non-finite as zero | 3 fail |
| atomic rename (copy instead) | 5 fail (a `.tmp` is left) |

**Threads.** One writer, one thread.

**WIRING HAND-OFF.** `core/recorder.*` and the I/Q record settings as in part 1: a "SigMF" choice in "Recording format", `addCapture` called from
the retune path (the engine's centre-frequency change) and `addAnnotation` from the same places that will mark transmissions or bookmarks;
`core:hw` from the source's name and `core:recorder` = "FoxSDR <version>" (the writer does not know either). Strings: "SigMF (metadata + data)",
"Annotation added at %s". There is no real SigMF consumer on the desk: proof is against our own reader only (UNVERIFIED against another tool).

## Part 3: KML and GeoJSON export (`KmlWriter`, `GeoJsonWriter`)

**What.** Writers over a `std::ostream` for points and tracks; input structures for aircraft (ICAO, callsign, squawk), ships (MMSI, name), radiosonde
balloons (serial, type) and APRS stations (callsign, comment, symbol), mapped to one `Entity`. Longitude first; numbers through `std::to_chars` (fixed,
7 decimals, trailing zeros removed, `-0` written `0`); points outside the ranges or non-finite are rejected and counted. A track is streamed
(`beginTrack` / `trackPoint` / `endTrack`): memory is constant and `trackPoint` allocates nothing. Tracks are cut at the antimeridian (the crossing
latitude interpolated in unwrapped longitude, segments end at +-180 and start at the other edge); a segment or track with fewer than two usable
points draws no line (KML: a placemark without geometry; GeoJSON: `"geometry": null`). XML text: the five entities escaped, invalid UTF-8
replaced by U+FFFD, characters outside the XML 1.0 `Char` production dropped. GeoJSON properties cannot overwrite `kind` / `name` / `id` / `time` /
`description`.

**Numbers.** One million track points streamed: KML 27.7 MB, GeoJSON 29.7 MB, **0.13 s idle (7.9 and 7.6 M points/s), 0.28 to 0.30 s in ctest**, with
**0 heap allocations** during the stream (allocation probe). A 30,000-point track crossing the antimeridian many times: 6 segments, 30,010 positions,
every segment 2+ positions, none spanning more than 180 degrees of longitude, the whole GeoJSON parses.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| longitude first | 15 fail |
| latitude range check | 9 fail |
| `&` escaped | 3 fail |
| characters outside the XML range dropped | 3 fail |
| antimeridian cut (KML) | 2 fail |
| `geometry: null` for unlocated tracks | the test program crashes (0xC0000409) |
| a lone point is not a line (KML) | 2 fail |
| invalid UTF-8 replaced in JSON | 1 fails |
| reserved properties protected | 1 fails |
| no allocation per point | 1 fails |
| `-0` normalised | 1 fails |
| calendar month rollover in `TimeStamp` | 2 fail |
| non-finite altitude rejected | the test program crashes |

**WIRING HAND-OFF.** `src/core/plugin_host*.cpp` / the track data the map already holds (aircraft, ship, sonde and APRS tracks come from plugin track
calls, `track_probe_plugin` is the fixture), `core/app_commands.*` (a new export op carrying the format and a path, like `BOOKMARK_EXPORT`),
`core/file_pick.*` for the save path, and the map page (desktop and Slint) for an "Export tracks" button on the track list and on a single track.
The caller walks its own track storage and calls `trackPoint` per fix (no copy is needed). Settings: none (last folder). Strings: "Export tracks as
KML (Google Earth)", "Export tracks as GeoJSON", "Exported %zu tracks, %zu points (%zu points skipped: no valid position)". Altitude is metres
(KML `absolute`); aircraft altitudes must be converted from feet by the caller. UNVERIFIED in Google Earth and in a GeoJSON validator (none on the desk).

## Part 4: gqrx and CubicSDR bookmark import (`importForeignBookmarks`)

**What.** `importGqrxCsv`, `importCubicSdrXml`, `importForeignBookmarks` (by content: a `<cubicsdr_bookmarks` root means CubicSDR; a gqrx header comment or
`;`-separated numeric lines mean gqrx) and `importForeignFile` (size checked before reading). Own result struct with the web lane's fields (items,
entries, skipped, modeDefaulted, format, error). gqrx: header comment decides the column order, default Frequency / Name / Modulation / Bandwidth / Tags,
`;`, tab or `,`, the tag-colour block skipped, first tag becomes the group (`Untagged` = none), modes mapped (Narrow FM, WFM variants, AM / AM-Sync,
LSB, USB, CW-L / CW-U, Raw I/Q), bandwidth 0 = the mode's default. CubicSDR: `<group name>` becomes the group, `<modem>` or `<bookmark>` elements with
`frequency`, `bandwidth`, `type`, `user_label` / `label` / `name`; ranges ignored. Hostile input refused: over 16 MiB, NUL, DOCTYPE or ENTITY (never
expanded; only the five entities and numeric references), nesting over 64, over 200,000 entries, gqrx lines over 4,096 bytes, frequencies outside
(0, 1e12] or written as hex / inf / nan; a truncated XML keeps the bookmarks that were complete and says so. Names: 128 bytes at most on a
character boundary, control characters out, invalid UTF-8 as `?`.

**UNVERIFIED.** Both formats are written from descriptions and memory (see Sources); the fixtures are hand-written. A real file of each is needed.
**Perseus is not built**: what was looked for is in the Sources table; without a description or a sample nothing can be written without inventing a
format.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| NUL refused (gqrx) | 2 fail |
| DOCTYPE / ENTITY refused | survived at first (the malformed tag was refused anyway); the test now requires the "ENTITY" message, then 1 fails |
| size cap (gqrx) | survived at first (the empty-file error masked it); the test now sends real content and requires the 16 MiB message, then 1 fails |
| hex / inf / nan frequencies refused | 1 fails |
| control characters removed from names | 1 fails |
| names cut on a character boundary | survived at first (2-byte characters); 3-byte characters used, then 1 fails |
| nesting depth cap | survived at first (a different error); the message is now required, then 1 fails |
| truncated XML reported | 1 fails |
| tag-colour block skipped | survived at first (a second guard); a tag starting with a digit was added to the fixture, then 2 fail |
| `&#0;` refused | 1 fails |
| entry cap (gqrx) | 2 fail |
| `Untagged` means no group | 1 fails |
| gqrx line-length cap | 1 fails |

**WIRING HAND-OFF.** `core/freq_import.cpp` (`importFrequencyFile*` tries the foreign importers when SDR# XML and CSV do not match; fold
`ForeignImportResult` into the web lane's `ImportResult`, then delete the duplicate struct), the Import box in the bookmark panel (desktop and Slint),
`FOXAPP_OP_BOOKMARK_IMPORT_FILE`. Strings: "gqrx bookmarks", "CubicSDR bookmarks", "%zu imported, %zu skipped, %zu with a mode FoxSDR does not have (imported
as NFM)", "The file is larger than 16 MiB", and the error sentences (English, for the log; the box shows a short form).

## Part 5: decode database (`DecodeDb`)

**What.** An append-only store of decode events (time ms, radio, frequency, decoder, kind, text, up to 32 named fields) in an own format (see the header
comment for the byte layout): segment files `seg-<n>.fxd` of at most 8 MiB (configurable 64 KiB to 256 MiB) holding CRC-checked records, each written
with one call and flushed before it is visible; sealed segments get an index file `seg-<n>.fxi` (32-byte entries, decoder table, CRC) used at open
when its recorded data size matches. Search by text words (all must occur, case-insensitive ASCII, in text, kind or a field value), decoder (exact),
radio, time range and frequency range, newest first or oldest first, with limit and offset. Retention by age and by total size, in whole segments.
Torn last record cut at open; a damaged record in the middle skipped by resynchronising on the next valid one. Value caps (decoder 64, kind 64, text
16 KiB, 32 fields, key 64, value 1 KiB) cut on UTF-8 boundaries and counted. One writer, any number of readers; a read-only open for other processes.
No fsync is issued: a process crash is covered, a power loss may lose what the OS had not written (the torn-tail rule covers the rest).

**Numbers (this machine, shared and loaded; 1,000,000 records, 116.4 MB in 7 segments of 16 MiB).**

| Measure | Idle run | Inside ctest (loaded) |
|---|---|---|
| 100,000 single appends, each flushed | 0.38 s (262,564 per s) | 0.75 s (133,199 per s) |
| 900,000 in batches of 1,000, one flush each | 0.69 s (1,298,859 per s) | 1.28 s (702,362 per s) |
| search: newest 100, no filter | 6.7 ms | 11.1 ms |
| search: 100 events by time range, mid file | 0.7 ms | 1.0 ms |
| search: one frequency, 1,000 rows | 5.8 ms | 8.2 ms |
| search: decoder = sonde, newest 100 | 0.6 ms | 1.0 ms |
| search: text RAREWORD (20 in 1M), full scan | 362.6 ms | 646.0 ms |
| search: text `kj4xyz-33 hello`, newest 100 | 13.8 ms | 19.8 ms |
| search: text `kj4xyz-33`, 10,309 rows | 371.3 ms | 659.6 ms |
| search: text matching nothing (full scan of 116 MB) | 358.4 ms | 606.5 ms |
| reopen with index files (6 loaded, 1 scanned) | 0.115 s | 0.175 s |
| reopen by scanning all 7 segments | 0.488 s | 0.713 s |

A text search reads every candidate record (no text index): about 0.36 s per million records idle. The index makes time, frequency, radio and
decoder filters cost milliseconds. A text index (per-segment word or trigram filter) is the obvious next step if a page needs interactive text search over
tens of millions; not built.

Other measured facts: randomized queries against a brute-force reference: **300 run, 0 differ** (20,000 records over many segments, 1 in 11 events out of
time order); torn write simulated at **every cut of a 114-byte record** (and the file shortened by hand at every byte of its last record): 0 wrong
counts, 0 wrong repairs, 0 unwritable afterwards; a real 32-bit FNV-1a collision found by search ('dec323809' and 'dec1144982') and the decoder filter
stays exact; size retention 11 to 5 segments, 681,761 to 289,094 bytes under a 340,880-byte limit, files really deleted; **writer + 3 readers + a
retention thread: 60,000 records, 225,147 to 296,739 searches, 0 bad rows**, what remains is a gap-free suffix.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| torn tail cut at open | 3 fail |
| CRC checked on scan | 2 fail |
| resynchronise after a damaged record | 4 fail |
| time upper bound | 3 fail |
| frequency range inclusive | 2 fail |
| decoder verified under a hash collision | 7 fail |
| size retention | 4 fail |
| segment files deleted | 1 fails |
| stale index refused | 2 fail |
| UTF-8-safe cut | 1 fails |
| offset | 3 fail |
| read-only open refuses writes | 1 fails |
| CRC checked on read | survived at first; a test of damage under a trusted index was added, then 2 fail |
| a bad-header file's number is never reused | 3 fail (the bad file was overwritten) |

The first run of the test found one real defect (the first segment was always created as number 1, overwriting a bad-header `seg-00000001.fxd`).

**WIRING HAND-OFF.**
- Files the wiring lane changes: the engine's decode path (`src/engine/engine.cpp`, `engine_radios.cpp` and the plugin host's decode events, host API level 3 as
  `FEATURE-GAPS.md` plans), `core/config.*` (database folder, retention), a new page in `src/slint/` (and `src/gui/` if the desktop keeps one) for search.
- Ops and settings: `DECODE_DB_ENABLE`, folder (default under the config folder, `decodes/`), retention days (suggest 30) and size (suggest 2 GiB), a
  "Clear" op; search op or a read endpoint for the browser page. `appendBatch` should be used by the decode thread's drain (one flush per drain).
  Call `enforceRetention` once a minute from a housekeeping thread, not the decode thread.
- Controls: a search box, decoder drop-down, radio drop-down, time range, frequency range, "Export results" (CSV through the existing exporters).
- Strings: "Decode history", "Search decodes", "%zu results", "Keep decodes for", "Keep at most", "Clear decode history", "Decode history is damaged
  and was repaired (%zu records lost)".

## Part 6: alert rules (`AlertEngine`, rule file, `NotificationSink`)

**What.** `Rule`: keywords, wildcards (`*`, `?`; never regular expressions), target lists (callsigns, ICAO addresses, MMSIs, squawks; found in named fields
and, except squawks, as whole tokens of the text), filters (decoders, frequency range, radio), minimum interval, per-target interval, hourly cap, quiet hours
(start, end, UTC offset, weekdays; wraps midnight; a `bypassQuietHours` flag), severity. `AlertEngine::process(DecodeEvent)` returns `Alert`s with what matched;
time is the event's time, so a replayed log gives the same alerts; per-rule counters. Rule file: JSON version 1, the whole file refused on the first error
(rule number and name in the message), nesting guard before the parser, atomic save. `NotificationSink` interface with a null sink and a recording sink.
**The Windows toast sink is NOT implemented**: a toast from an unpackaged copy needs an app user model ID registered through a Start-menu shortcut (a system
change) and cannot be tested without showing a toast on the desktop; a packaged (Store) copy has the identity already. The project already links the Windows
Runtime (`runtimeobject`, `core/store_update.cpp` uses C++/WinRT), so no new dependency would be needed, but that does not remove the two problems. The wiring
lane (or the owner) should decide between: toast through `Windows.UI.Notifications` for the packaged copy only; a tray balloon; an in-app list only (the
recording sink's job).

**The test found a real bug** in the first version: the per-rule state held an LRU list and a map of iterators into it, and a `std::vector` of such states
copies them when it reallocates (MSVC's `std::list` move constructor is not `noexcept`), leaving dangling iterators. Address sanitizer reported
heap-use-after-free and the concurrent test crashed. States are now `unique_ptr`s and not copyable.

**Numbers.** 100 rules (keywords, wildcards, callsign and ICAO targets, a decoder filter) over 200,000 events: **3.4 to 5.1 s, 39,000 to 59,000 events per second
(3.9 to 5.9 million rule evaluations per second)** on the loaded machine, every event scanning a 90-byte text. A decode thread emits tens of events per second at
most, so this is not a limit. 247 checks; about 60 hostile rule files refused (each named), random bytes never accepted.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| wildcard anchored to the whole text | 7 fail |
| `?` matches one character | 1 fails |
| target is a whole token | 3 fail |
| squawk only in fields | 1 fails |
| quiet end exclusive | 1 fails |
| quiet morning belongs to the day before | 2 fail |
| bypass flag | 2 fail |
| minimum interval boundary | 4 fail |
| hourly cap boundary (sliding) | 2 fail |
| a suppressed repeat does not extend the quiet period | 2 fail |
| key memory bounded at 4,096 | 1 fails |
| frequency edge inclusive | 1 fails |
| JSON nesting guard | 2 fail |
| duplicate names refused | 1 fails |
| ICAO length | 2 fail |
| squawk octal digits | 2 fail |
| file size cap | 1 fails |
| weekday arithmetic | 6 fail |

(Two mutants first crashed the test program through `process(...)[0]` on an empty result; the test now checks the size first, and the mutants fail cleanly.)

**WIRING HAND-OFF.** The decode path (as part 5: every decode event goes to `AlertEngine::process` on the decode drain, alerts to a worker that calls
`deliver`), `core/config.*` for the rules file path (`alerts.json` beside the config), a settings page for rules (desktop and Slint), the tray / notification
code (new), `core/app_commands.*` for "mute alerts" and "test alert" ops. Controls: rule list with enable tick, a rule form (keywords, wildcards, target lists,
decoder / frequency / radio filters, rate fields, quiet hours with time zone and weekdays, severity), a "Test" button, and the per-rule counters (matched, fired,
quiet, rate limited) so a person can see why nothing fired. Strings: "Alerts", "Alert rule", "Keywords", "Wildcards (* and ?)", "Callsigns", "ICAO addresses",
"MMSI numbers", "Squawk codes", "Decoders", "Frequency range", "Radio", "Minimum time between alerts", "Same target again after", "At most per hour", "Quiet hours",
"From", "To", "Time zone", "Days", "Allow this rule during quiet hours", "Info", "Notice", "Urgent", and the alert titles built by the engine
(`<rule>: <decoder> <MHz>`).

## Part 7: macros (`Macro`, `validateSet`, `Player`)

**What.** A macro is a name, a description, a repeat count and steps: a command name from `core/app_commands.cpp` (`cmd::opName`), the FoxCommand slots
(`num[4]`, `ival[2]`, `text` up to 255 bytes) and a delay before the next step; pseudo-steps `WAIT` and `RUN_MACRO`. Validation takes the vocabulary as a
`KnownCommands` name list (this header does not include `app_commands.hpp`); the test builds it from `cmd::knownOps` / `cmd::opName` and proves every name there is
usable as a macro command name, none duplicated, and that the denied-prefix policy still matches real commands (it fails if a prefix goes stale). **151 commands
are known, 112 are allowed in a macro, 39 are denied** (transmit: `TX_*`, `APP_TX_*`; plugin rights and the plugin store; server and interface configuration;
telemetry; test hooks; device addresses; deleting a bookmark group). Commands that `applyCommand` does not implement (`SETTING_SET`, `PROBLEM_REPORT`, `UPDATE_CHECK`, ...)
are not in the vocabulary, so a macro naming them is refused as unknown. Loop protection: cycles through `RUN_MACRO`, depth 4, repeat 1,000, at least 100 ms of delay
per repeating pass, 10,000 executed steps (multiplied out), 24 h per run, 500 steps, 200 macros. The `Player` takes the clock as an argument (no threads): late polls
catch up without drift; its limits are enforced again at run time for a set that skipped validation. Load refuses the whole file on the first error; atomic save.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| denied commands refused | 7 fail |
| unknown commands refused | 8 fail |
| repeat spin guard (100 ms per pass) | 3 fail |
| cycle detection | 4 fail |
| a diamond is not a cycle | the test program crashes (stack overflow) |
| depth 4 | 1 fails |
| expansion bound (10,000) | 1 fails |
| no drift (steps chain from their due time) | 22 fail |
| player step cap | 3 fail |
| player self-run guard | 3 fail |
| JSON nesting guard | 2 fail |
| `WAIT` takes no text | 1 fails |
| per-step delay cap | survived at first (the total-time check caught it); the test now requires the per-step message, then 1 fails |
| 24 h run cap | 1 fails |
| control characters in text | 2 fail |
| finite numbers | 2 fail |
| repeat cap in the file reader | equivalent mutant: `validateMacro` refuses the same value with the same effect (kept as a second guard) |

**WIRING HAND-OFF.** `core/app_commands.*` and `AppWindow::applyCommand` (a macro step becomes a `FoxCommand`: look the op up by `cmd::opName`, fill the slots, call the
same path a key binding uses), `core/config.*` (macro file path, key bindings to start a macro: "rebindable keys exist" per the survey), the engine API (a `MACRO_RUN` /
`MACRO_STOP` pair so the browser and plugins can start one, if wanted), a page in the desktop and Slint settings with a step editor and a "record what I do" option (not
built: it needs the command path to tee into a recorder). The player's `poll` belongs on a UI or control-thread tick (the control thread already has a tick).
Strings: "Macros", "Run macro", "Stop macro", "Add step", "Wait", "Run another macro", "Repeat", "This command cannot be used in a macro", "The macro runs itself", "The macro
is too long (more than 10,000 steps)".

## Part 8: DSP benchmark (`runDspBenchmark`, `formatTable`)

**What.** Times `dsp::ComplexFFT` (seven sizes), `dsp::Vfo` (three input rates), `dsp::Demodulator` (eight modes at their usual rates) and `dsp::RationalResampler` (three
ratios) against stated reference rates: median of three timed slices, the best slice beside it, samples per second and the real-time multiple. A block that throws is
reported and the run goes on. `tests/test_dsp_benchmark.cpp` passes when every item runs to the end with a finite timing; it prints the table (`--seconds=N` or
`COREF_BENCH_SECONDS` for longer items).

**Numbers (this machine, loaded by about thirteen other lanes, 0.2 s per item; the multiple is median samples per second over the reference rate).**

| Item | Median MS/s | x real time |
|---|---|---|
| FFT forward 1024 | 402.25 | 167.6 (at 2.4 MS/s) |
| FFT 4096 / 16384 / 65536 | 243.62 / 226.14 / 188.66 | 101.5 / 94.2 / 78.6 |
| FFT 262144 | 92.89 | 38.7 |
| FFT 3072 / 15360 | 285.99 / 181.00 | 119.2 / 75.4 |
| Vfo 2.4 MS/s to 48 kHz | 34.56 | 14.4 |
| Vfo 10 MS/s to 50 kHz | 33.19 | 3.3 |
| Vfo 20 MS/s to 200 kHz | 35.00 | 1.7 |
| NFM at 48 kHz | 41.06 | 855 |
| WFM at 250 kHz | 34.81 | 139 |
| AM at 48 kHz | 161.32 | 3,361 |
| DSB / RAW at 48 kHz | 3,997 / 3,939 | 83,279 / 82,052 (a copy and a real part) |
| USB / LSB / CW at 48 kHz | 3.51 / 2.99 / 3.14 | 73 / 62 / 65 |
| Resample 48000 to 44100 | 73.09 | 1,523 |
| Resample 250000 to 48000 | 321.39 | 1,286 |
| Resample 200000 to 48000 | 275.94 | 1,380 |

The finding: the channel filter (`Vfo`) is the only stage that falls near real time at high input rates: 20 MS/s input leaves only 1.7 times real time on one core of this
loaded machine, and the single-sideband demodulators (USB, LSB, CW at about 3 MS/s, 62 to 73 times real time at 48 kHz) are the slowest audio stages. Both are
still far above their needs for one receiver; with eight radios the Vfo figure is the one to watch. These are measurements on a loaded machine, not specifications.

**Break-it table.**

| Behaviour removed | Result |
|---|---|
| median of the slices (returns the maximum) | 2 fail |
| multiple = rate / reference (multiplied instead) | 21 fail |
| a throwing block reported as failed (set to ok) | 1 fails |
| best slice reported (set to half the median) | 22 fail |
| table shows `FAILED: <message>` | 1 fails |

**WIRING HAND-OFF.** A command-line option (`--benchmark`, in the command-line parser the `feat-rec` lane builds) or a "Benchmark" button in Settings > About that calls
`runDspBenchmark` on a worker thread and shows `formatTable`; optionally a line in the problem report ("this machine: Vfo 20 MS/s x1.7"). Strings: "Run DSP benchmark",
"Median MS/s", "x real time". Settings: none.

## Build and test

- Worktree `C:/Users/steve/foxsdr-wt/feat-core`, build directory `D:/foxsdr-build/feat-core`, configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`
  (Visual Studio 17 2022, vcpkg x64-windows, Release); built with `--parallel 3` and only the eight test targets. No warnings were reported for the new files at `/W4`.
- `tests/CMakeLists.txt` is the one existing file edited: six lines giving `test_rf64_writer` a 900 s limit (it really writes 4 GiB; 59 s on the loaded machine against the
  default 120 s) and `test_decode_db`, `test_alert_rules`, `test_dsp_benchmark` 300 s. Nothing fails on a speed; the limits are for a slow machine, not for a hang.
- The new tests were also compiled and run stand-alone with `cl` for the break-it tables (a copy of the header with one behaviour removed first on the include path).
  The break-it tables above are from those runs; the committed headers are the unmodified ones (checked with `git status`).
- Files in the worktree have LF endings and git warns that it will convert them to CRLF on checkout, as for the other files of this repository.
- The 4 GiB test, the million-record test and the speed loops honour `COREF_SKIP_BIG` (used only for the break-it runs).

## UNVERIFIED list

1. BW64 id and ITU-R BS.2088 (not read); the writer writes `RF64`.
2. SigMF `core:version` "1.2.6" accepted by a strict validator; no real SigMF consumer was run.
3. KML 2.2 and GeoJSON RFC 7946 element order and antimeridian handling checked against the writer's memory of the standards only; no Google Earth or GeoJSON validator run.
4. gqrx `bookmarks.csv` and CubicSDR `bookmarks.xml` layouts: from descriptions and memory, no real file seen.
5. CRC-32 equals the standard CRC-32 (only self-consistency is tested; matters only if another tool must read `.fxd` files).
6. The recorded DSP and database timings are from a shared, loaded machine and move by about a factor of two between runs.
7. `fsync` behaviour: a power loss during a database write was not tested (only process crashes and torn files).
8. Thread-safety was checked by the concurrent tests and by address sanitizer on the alert engine; ThreadSanitizer is not available on this compiler.

## Owner decisions

1. **Which commands a macro may run.** The default denies transmit, plugin rights, the plugin store, server and interface configuration, telemetry, test hooks, device addresses and
   deleting a bookmark group (39 of 151). `PLUGIN_COMMAND` (anything a plugin offers) and `RUN` / `SELECT_SOURCE` are allowed; say if any should go. A macro file can come from anywhere,
   so allowing `TX_*` is NOT recommended.
2. **Windows toast**: packaged copy only, a tray balloon, or in-app list only (part 6).
3. **Decode history defaults**: on or off by default, retention (30 days / 2 GiB suggested), and whether the folder lives in the config folder or the recordings folder.
4. **Retention granularity**: whole segments (8 MiB by default, about 70,000 records): a record can outlive its age limit by up to one segment. Smaller segments cost more files.
5. **Text search over a very large history**: 0.36 s per million records is fine for hundreds of thousands; build a text index only if tens of millions are wanted.
6. **RF64 versus BW64 id** for the file the recorder writes (RF64 is what most tools expect).
7. **Whether a plain RIFF I/Q file is still wanted** once the recorder writes the new format (existing playback reads both).
