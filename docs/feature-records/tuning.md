# Engine lane `tuning` - record

Branch `feat/tuning`, created from engine/multi-radio d3630d78. New-file model lane: every model is a header under `src/core/` or
`src/dsp/` with its test under `tests/`; no existing source file is edited (the tests are globbed, so `tests/CMakeLists.txt` needed no edit
either). Nothing is wired; the wiring hand-off at the end says exactly where each model plugs in.

## 1. Step 0 - the table (reading only, written before any code)

Searched: the base (d3630d78) and the branches `feat/screen-c`, `feat/recording-models`, `feat/signal-blocks-3`, `feat/signal-blocks-4`,
`feat/screen-b`, `feat/web-bookmarks`, `feat/knob-input`. Those branches are not in this lane's base, so a model that exists only there is
"built elsewhere" (this lane does not copy it); a gap they only partly cover is built here as a new, separate file.

| # | Row | Verdict | Evidence / what this lane builds |
|---|-----|---------|----------------------------------|
| S1 | Scan trigger level separate from the demod squelch, dynamic noise floor | new file model | `feat/screen-c` has per-entry thresholds (C1) against a level, but a FIXED dB; no floor tracking. Built: `core/scan_trigger.hpp` (fixed or floor-relative, per slot, hysteresis). Base `noise_floor_tracker.hpp` is for the squelch's automatic threshold and is not per scan slot. |
| S2 | FFT scan across the captured span without retuning | new file model | Nothing in the base or branches. Built: `core/fft_scan.hpp` (one spectrum frame -> hits per scan channel, floor from the frame itself). |
| S3 | Several search ranges in one scan | new file model (hand-off for the form) | `ScannerParams` holds one range or one `list`. Built: `core/scan_plan.hpp` expands up to 16 ranges into the existing `list` mode. |
| S4 | Scanning backwards / stepping back | new file model | Scanner has no direction (`scanner.hpp` is forbidden). Built in `scan_plan.hpp`: direction, and a cursor with `stepBack`; the plan hands the scanner an already-reversed list. |
| S5 | Save a scan hit as a bookmark | new file model | Built in `scan_plan.hpp`: `bookmarkFromHit` (name, mode and bandwidth from the range the hit lies in; duplicate refused). |
| B1 | Notes / comment / tag field on a bookmark | new file model | `core/bookmark.hpp` is data shared with the engine API; not edited. Built: `core/bookmark_notes.hpp`, a sidecar keyed by frequency + mode. |
| B2 | Favourites that save the whole state | new file model | `Bookmark::favourite` is only a flag. Built: `core/favourite_state.hpp` (display range, spectrum range, waterfall contrast, DSP, versioned JSON). |
| B3 | Marker database edited and exported separately | new file model | `core/freq_markers.hpp` is the store (note edit exists); no export/import. Built: `core/marker_export.hpp` (CSV + JSON out, strict import, merge). |
| B4 | Custom CSV columns overlaid from a station database | new file model | `core/freq_import.hpp` imports CHIRP-like rows into bookmarks; no overlay of foreign columns. Built: `core/csv_overlay.hpp`. |
| T1 | Per-band memory: volume, AGC, attenuator, spectrum limits, zoom, antenna | new file model | `engine.cpp:6579` says nothing is stored per band. `feat/recording-models` `operating_aids.hpp` has band-stacking registers (frequency/mode/bandwidth, three per band) and a per-MODE memory (bandwidth, step, AGC): a different record. Built: `core/band_memory.hpp`. |
| T2 | Frequency back/forward history (16 entries) | new file model | Not in the base or any searched branch (`operating_aids.hpp` has VFO A/B, not a history). Built: `core/freq_history.hpp`. |
| T3 | RIT/XIT clarifier on the spectrum; CAT max 0 | new file model + hand-off | `src/net/cat_protocol.cpp:192-193` reports max RIT/XIT 0. Built: `core/rit_xit.hpp` (offsets, limits, spectrum marker positions, CAT numbers). The CAT change itself is a hand-off. |
| T4 | Numbered channel plans (PMR446, CB, FRS/GMRS, MURS, marine VHF) with a channel mode | new file model | Built: `core/channel_plans.hpp` + a channel cursor. Tables typed from the regulators' pages (section 6). |
| T5 | CW stereo placement by pitch | new file model | `feat/signal-blocks-4` `dsp/stereo_place.hpp` places by pan/azimuth, not by pitch. Built: `dsp/cw_surround.hpp` (pitch estimate -> pan, constant-power). |
| T6 | 1750 Hz tone-burst key | new file model (tone only) | Built: `dsp/tone_burst.hpp`, a tone generator with attack/release only. Keying is NOT built (owner decision). |
| T7 | Launching companion programs | new file model, OFF by default | Built: `core/companion_launcher.hpp` (validation, quoting, policy, injected spawner). Real spawn is a hand-off (owner decision). |
| C1 | Shared preset and bookmark packs | new file model (format only) | Built: `core/bookmark_pack.hpp`. Frequency-database DOWNLOADS are an outside service: NOT built, listed. |
| C2 | Keyboard actions (bandwidth up/down, cycle step, cycle mode, centre signal, next/previous bookmark) | new file model + hand-off | `gui/key_bindings.cpp` is lane C's. Built: `core/key_actions.hpp` (action list + handler functions). |
| - | N1MM/DXLog spot feeds | not this lane | plugin lane `dxspots2`. |

Rows that need a forbidden file only for the last step (wiring): S1-S5 (engine scanner/pipeline), T1 (engine band switch), T2, T3 (CAT,
engine NCO), T4-T7, C2: each has its standalone model here and a hand-off section below. No row is `blocked`; none was already built in
a form that makes the new file redundant.

## 2. What was built (all standalone, none wired)

| Row | File | One line |
|-----|------|----------|
| S1 | `src/core/scan_trigger.hpp` | Scan trigger apart from the squelch: Fixed or floor-relative per scan slot, hysteresis, floor follows down at once and up slowly and is never taught by a carrier. Versioned JSON for the config. |
| S2 | `src/core/fft_scan.hpp` | One spectrum frame to hits per scan channel with no retune; median floor from the frame; adjacent channels merged; channels the frame does not cover are counted. |
| S3-S5 | `src/core/scan_plan.hpp` | Up to 16 ranges expanded into the Scanner's existing list mode (100000 points at most, refused rather than truncated), Forward/Backward, `ScanCursor` with step back, `bookmarkFromHit`, versioned JSON. The real `Scanner` is driven by a backward plan in the test. |
| B1 | `src/core/bookmark_notes.hpp` | Note (2000 bytes) and up to 16 tags per bookmark, a sidecar keyed by frequency + mode, search and tag lookup, versioned JSON. |
| B2 | `src/core/favourite_state.hpp` | Whole-state favourite: display range, spectrum range, waterfall contrast, DSP; named store (200), versioned JSON. |
| B3 | `src/core/marker_export.hpp` | Marker list to CSV (formula-guarded) and JSON, strict import, merge into a live `FreqMarkers` (new numbers, existing notes kept). |
| B4 | `src/core/csv_overlay.hpp` | Read-only overlay of chosen columns of the user's own station CSV by frequency (nearest within a tolerance); skipped and cleaned rows counted. |
| T1 | `src/core/band_memory.hpp` | Volume, AGC, attenuator, spectrum limits, zoom and antenna by band name, field mask, one `onBandChange(from, to, current)` call. |
| T2 | `src/core/freq_history.hpp` | 16-entry back/forward stack; the retune a back() causes is ignored; a dial spin is one step. |
| T3 | `src/core/rit_xit.hpp` | RIT and XIT offsets with limits (default 9999 Hz), spectrum marker positions, clear-on-retune, the CAT numbers. |
| T4 | `src/core/channel_plans.hpp` | PMR446 (analogue 16, digital 16 and 32), CEPT CB 40, FRS 22, GMRS 30, MURS 5, marine VHF 59 (ITU Appendix 18) + `ChannelCursor` (next, previous, go to, nearest). |
| T5 | `src/dsp/cw_surround.hpp` | Dominant-pitch estimate (Goertzel bank, 10 Hz bins, 1024-sample window) to a constant-power pan; hold and glide back to the centre; noise does not move it. |
| T6 | `src/dsp/tone_burst.hpp` | A tone only: 1750 Hz default, exact length, raised-cosine attack and release, phase continuous. No transmit path (source scan test). |
| T7 | `src/core/companion_launcher.hpp` | Model, OFF by default: full paths only, no shell, `.exe` only, quoting proven by a round trip against the documented parse, origin gate, injected `Spawner`. |
| C1 | `src/core/bookmark_pack.hpp` | `foxsdr-pack` version 1 (bookmarks + notes + favourites); strict import; import plan that never overwrites. Downloads: NOT built (an outside service). |
| C2 | `src/core/key_actions.hpp` | The six missing key actions: ids, English labels, proposed chords, and pure handlers (bandwidth step, cycle step, cycle mode, centre on signal, next/previous bookmark). |
| - | `src/core/csv_text.hpp`, `src/core/tuning_text.hpp` | Shared: bounded strict CSV reader/writer with formula guard; user-text rules. |

No existing source file was edited. `tests/CMakeLists.txt` was not touched either: it globs `test_*.cpp`, so the four new tests are picked up by a re-configure
(a new test file is registered by the glob but built only after cmake is run again).

## 3. Not built, and why

- **Frequency-database downloads** (C1): an outside service; the brief says list, not build.
- **UK 27/81 CB channels (27.60125-27.99125 MHz)**: Ofcom's IR 2027.2, IR 2027 and the citizens-band page answered HTTP 403 for every URL tried (web.archive.org is refused by the tool),
  and the statutory text that names the band edges (SI 2006/3418) came back empty. The spacing and the number-to-frequency rule are therefore not verified from an opened page and the
  plan is not in the table. Not blocked in principle: opening IR 2027.2 in a browser is enough; it is a 40-row addition to `channel_plans.hpp`.
- **Marine four-digit channel designators, ASM 1 and 2, and the digital-only entries** of Appendix 18: the table text extracted from the PDF interleaves these rows with the notes too loosely to pair every row safely; the 59 two-digit and AIS rows are cross-checked as a set (section 5).
- **Keying of the 1750 Hz tone, and anything that transmits** (owner decision, section 6).
- **Starting a real program** (owner decision): the spawner is a hand-off.

## 4. Wiring and screen hand-off (every control: English label, op or fact, range, default)

The files the wiring touches belong to other lanes (engine, scanner page, bookmark page, key bindings, CAT, `.slint`); nothing below was done here.

**Scanner (lane C's scanner page and the engine's scanner tick)**
- Form row "Trigger": Fixed / Above noise floor (default Above noise floor); "Level" -160..20 dB (-70); "Margin" 0..60 dB (8); "Hysteresis" 0..30 dB (2); "Floor rise" 0..10 dB per reading (0.1; advanced). Fact: `ScanTriggerConfig`, saved as `ScanTrigger::configToJson()` (version 1).
  Engine: where the tick builds the `squelchOpen` flag for `Scanner::tick`, use `ScanTrigger::update(scanner.index(), channelLevelDb)` with the level the pipeline already measures for the squelch; call `reset()` on every reconfigure. The demod squelch is untouched.
- "Scan whole span without retuning" (check box, default off): the engine's spectrum thread calls `fftScan(binsDb, n, centreHz, spanHz, grid, options)` on the frame it already makes (bins ascending in frequency: convert from FFT order first); new hits go to the hit list; when `channelsOutOfSpan > 0` the engine retunes by the span and repeats.
- "Ranges" list, at most 16 rows (start MHz, stop MHz, step kHz, mode, bandwidth Hz, label), buttons "Add range" and "Remove"; "Direction": Forward / Backward (Forward); a key/button "Scan back" (`ScanCursor::back()`) beside the existing "Skip". Engine: `ScanPlan::toParams(base)` returns the `ScannerParams` with `list` set (the Scanner itself is unchanged). Saved: `ScanPlan::toJson()` (version 1).
- Button "Save hit as bookmark" on the scan status line: `bookmarkFromHit(hz, plan, bookmarks, "Scan hits")` then the page's existing bookmark add; nothing is added when it returns empty (already bookmarked).

**Bookmarks (bookmark page, FreqManager)**
- Editor fields "Note" (multi-line, 2000 bytes) and "Tags" (comma separated, 16 at most, 32 bytes each): `BookmarkNotes::set/find`; file `bookmark_notes.json` beside `bookmarks.json` (text from `toJson()`, saved the way FreqManager saves); call `rekey()` when a bookmark's frequency or mode is edited; filter box "Tag" via `byTag`, search box via `search`.
- "Save view as favourite" (name, 64 bytes) and "Recall": fill `FavouriteState` from the window (display centre and span, spectrum top and range, waterfall floor and ceiling, AGC, NR, NB, notch, squelch, de-emphasis), apply it on recall; file `favourites.json` (`FavouriteStore`).
- Markers page: buttons "Export CSV...", "Export JSON...", "Import...": `markersToCsv/markersToJson`, `parseMarkersCsv/parseMarkersJson`, then `importInto(live, list, mergeHz)`; the page says "added N, merged M".
- "Station database..." (file chooser, then "Frequency column", "Unit" Hz/kHz/MHz (kHz), "Columns to show" up to 16, "Delimiter" comma / semicolon / Tab): `StationOverlay::load`; each row shows `lookup(hz, tolerance)` pairs; settings saved with `overlayConfigToJson` (version 1); the page says "skipped N rows" from `skippedRows()`.
- "Export pack..." and "Import pack...": `exportPack`, `importPack`, then `planImport` against the list and the page's add; "N added, M already there".

**Tuning and operating**
- Per-band memory (engine.cpp `followBandModeOnTune`, the band-switch code): call `BandMemory::onBandChange(fromBandName, toBandName, current)` with the plan entries' names (empty for a gap) and apply the fields of `result.apply`; settings page "Remember per band": check boxes Volume / AGC / Attenuator / Spectrum limits / Zoom / Antenna (all on) = `setEnabledFields`. File `band_memory.json` (version 1).
- History: buttons and keys "Back" and "Forward" (`FreqHistory::back/forward`); the engine's tune function calls `push(hz, mode, nowMs)` on every retune (the model ignores the push a back/forward causes).
- RIT/XIT: controls "RIT" on/off, "RIT" offset -9999..9999 Hz (step 10, default 0), "XIT" on/off and offset, "Clear"; "Clear on large retune" 0..20000 Hz (0 = never). Spectrum: `marks(vfoHz)` gives the receive marker (RIT) and the transmit marker (XIT). Engine: `receiveShiftHz()` is added to the receive oscillator. CAT: `src/net/cat_protocol.cpp:192-193`: replace the two `0` lines by `catMaxRit()` and `catMaxXit()`; the CAT commands that set the offsets are not specified here (not verified).
- Channel mode: "Channel plan" list (8 plans, `channelPlans()`), "Channel" up/down (`ChannelCursor::next/prev`), "Go to channel" box (`goTo(label)`); the tune sets `freqHz`, the mode `modeHint` (a default) and the bandwidth `tuneBandwidthHz()`. Marine duplex channels show both frequencies; the GMRS and MURS labels are this program's (`numberingIsOurs`).
- CW surround: switch "CW surround" (off), "Centre pitch" 200..1500 Hz (700), "Spread" 50..2000 Hz (400), "Invert sides" (off): `CwSurround::process(mono, left, right, n)` in the CW receiver's audio stage, where the stereo output exists.
- Keys (key_bindings.cpp, lane C): `keyActionList()` gives ids `bandwidth_up`, `bandwidth_down`, `cycle_step`, `cycle_mode`, `centre_signal`, `next_bookmark`, `previous_bookmark`; English labels "Bandwidth wider", "Bandwidth narrower", "Next tuning step", "Next mode", "Centre the signal", "Next bookmark", "Previous bookmark"; proposed chords Ctrl+Right, Ctrl+Left, Ctrl+S, Ctrl+M, Ctrl+Space, Ctrl+Down, Ctrl+Up (check against the conflict table); handlers `bandwidthStep`, `cycleStep`, `cycleMode`, `centreOnSignal`, `nextBookmark`, `previousBookmark`.
- Tone burst and launcher: no controls proposed until the owner decides (section 6).

**English strings to add to the catalogue** (none translated here): the quoted labels above; the models' refusal texts are English status-line texts, written beside each refusal in its header.

## 5. Sources opened (URL, what, licence / status)

| URL | What was read | Status |
|-----|---------------|--------|
| https://docdb.cept.org/download/1491 | ECC Decision (15)05, amended 2 March 2018, decides 2-4: PMR446 band and channel plan | CEPT public document; read in full as text (pdftotext, an installed tool) |
| https://www.etsi.org/deliver/etsi_en/300400_300499/300433/02.01.00_20/en_300433v020100a.pdf | Draft ETSI EN 300 433 V2.1.0 (2016-02), clause 4.4 table 1: CB carrier frequencies, 10 kHz separation | ETSI public draft; read as text; the table is in two column blocks and the second block pairs by text order (21, 22, 24, 25, 23, 26 ... 40) |
| https://www.ecfr.gov/api/versioner/v1/full/2026-11-01/title-47.xml?part=95&section=95.563 | 47 CFR 95.563 FRS channels | US federal regulation, public domain; read through the fetch tool's summary of the XML, not the raw text |
| the same with `section=95.1763` | 47 CFR 95.1763 GMRS channels (four groups) | same, summary |
| the same with `section=95.2763` | 47 CFR 95.2763 MURS channels (five frequencies) | same, summary |
| https://www.law.cornell.edu/cfr/text/47/part-95/subpart-B | titles of the subpart B sections (to find 95.563) | Cornell LII, public law text |
| https://search.itu.int/history/HistoryDigitalCollectionDocLibrary/1.49.48.en.102.pdf | ITU Radio Regulations 2024, Volume 2, Appendix 18 (Rev. WRC-19), pages 319-322 | ITU publication, free download; read as text; facts only |
| https://www.itu.int/dms_pub/itu-r/md/00/cm/cir/R00-CM-CIR-0019!!PDF-E.pdf | opened, content unreadable by the fetch tool | not used |
| Ofcom IR 2030, IR 2027, IR 2027.2, citizens-band.pdf, the IR 2009 PDF | HTTP 403 | not read; nothing taken from search snippets |
| https://www.legislation.gov.uk/uksi/2006/3418/pdfs/uksi_20063418_en.pdf | returned empty | not read |

Web-search result pages were used only to find the primary pages above; no value in any table comes from a snippet. No rival program's data file was opened and no GPL, LGPL or AGPL source was read.
The FRS, GMRS and MURS values come from the fetch tool's summary of the eCFR XML (a small model, not the raw text), and agree with the values the rules are known to give; treat the bandwidth of FRS (12.5 kHz) as the summary's statement. The decimal frequencies were not re-read from the raw XML: UNVERIFIED at that level.

The marine table was checked by script against the extracted Appendix 18 text: every computed frequency (ship transmit for 01-28 and 60-88, the coast frequency of the two-frequency channels, AIS 1 and AIS 2) occurs in the table's text, and the only frequency in the table's text that is not computed is 160.900 MHz (channel 2006, Note r, not built). That proves the SET of frequencies; the row-by-row pairing rests on the table's visible order and on the duplex rule (coast = ship + 4.600 MHz), which holds for every pair legible in the extracted text (for example 01 = 156.050 / 160.650, 18 = 156.900 / 161.500, 28 = 157.400 / 162.000).

## 6. Owner decisions

1. **Companion-program launcher (T7)**: built as a model, OFF by default, never starts a process in a test. Wiring it makes the application execute programs the user chose. Decide: whether to wire it at all; whether "start with the app" should exist; whether the Windows spawner (`CreateProcessW` with `lpApplicationName` = the validated full path, `bInheritHandles` FALSE, no shell) is acceptable.
2. **1750 Hz tone burst (T6)**: a tone generator only. Sending it to a transmitter (Pluto NFM transmit) is not built and nothing here may reach a transmit path; decide whether it ever should, and where the keying would sit.
3. **CAT clarifier (T3)**: reporting a maximum RIT/XIT other than 0 changes what a logging program sees; decide with the CAT owner.
4. **GMRS and MURS channel numbers**: the rules give frequencies only; G1..G30 and M1..M5 are this program's labels.
5. **Defaults the owner can change**: PMR446/CB/marine mode hints, the bandwidth tables of the key actions, the history coalesce time (1.5 s), the scan trigger numbers, the 9999 Hz clarifier limit, the CW surround tone ratio (100).

## 7. Tests (real result lines)

Run through ctest from `D:/foxsdr-build/en-tuning` (Release), after the last source change:

```
ctest -C Release -R "^(test_tuning_scan|test_tuning_bookmarks|test_tuning_operate|test_tuning_dsp|test_freq_import|test_freq_manager|test_freq_markers|test_noise_floor_tracker|test_scanner)$" -j2 --timeout 300
100% tests passed out of 9
```

| Test | Result line (own run) | What it covers |
|------|-----------------------|----------------|
| `test_tuning_scan` | `test_tuning_scan: 1850 checks, 0 failed` | trigger (fixed, relative, drifting noise with jitter, refusals, JSON), FFT scan (noise, off-centre signal, sample-rate error, out-of-span, merge, fixed mode, no-data bins, failure paths), plan (ranges, overlap, limits at the edge and one over, direction, cursor, the real `Scanner` walking a backward plan, hit to bookmark, JSON refusals); fuzz `plan fuzz: 30000 runs, 3246 parsed, 433 accepted` |
| `test_tuning_bookmarks` | `test_tuning_bookmarks: 31272 checks, 0 failed` | CSV read and write (limits, property round trip of 3000 random rows), notes and tags, favourites, marker export and import, station overlay, packs; fuzz `20000 runs each; accepted csv 14366, marker csv 1174, marker json 451, overlay 12693, pack 550, notes 274, favourites 216` |
| `test_tuning_operate` | `test_tuning_operate: 22545 checks, 0 failed` | band memory, history (capacity, truncation, dial-spin coalescing, clock going backwards), RIT/XIT, the 8 channel plans and the cursor, launcher (path refusals, a quoting property test of 20000 random argument lists against the documented `CommandLineToArgvW` parse, origins, the 2-second window, JSON), launcher source scan, key actions; fuzz `30000 runs each; accepted band memory 2687, history 530, launcher 328` |
| `test_tuning_dsp` | `test_tuning_dsp: 32277 checks, 0 failed` | tone burst (exact length, shape, no click, frequency, chunking invariance, limits, source scan: only `<cmath>` and `<cstddef>`, no transmit identifiers), CW surround (pan law, pitch within 6 Hz at 10 dB SNR and 10 Hz at 0 dB, noise does not move it, hold and return, chunking, disabled bit-exact, limits) |

Concurrency: every store here is for ONE thread by design (each header says so); there is no shared store, so there is no concurrency test. The source scans (launcher, tone burst) read the headers through `__FILE__`.

Signals in the DSP tests carry noise (white, 10 dB and 0 dB signal-to-noise) and frequency offset (437.3, 612.9, 702.1, 823.4, 1023.1 Hz, none on a bin); sample-rate error is exercised in the FFT scan (span +100 ppm). The tone burst's frequency is its own setting, so no sample-rate error applies to it.

Real findings the tests produced while writing (each fixed, not weakened): the CW tone detector first compared the peak with the MEAN of the bank, which a pure tone cannot exceed by more than about 22 times because its lobe spans nine of the 131 bins (a tone was not detected); a median reference let a white-noise window through about once in 190 windows; the shipped reference is the mean of the lowest three quarters of the bank with a ratio of 100 (white noise peaked at 34 in 560 windows measured, a 10 dB tone is far above). The history's first draft replaced the very first entry during a dial spin; the shipped rule keeps the first entry and any entry that follows a pause.

## 8. Break-it table (behaviour removed, test red, source restored, test green)

Each row is one mutation of one header by `D:/foxsdr-build/tuning-mut.py`: apply, rebuild the test, run it, restore. After each batch the restored sources were rebuilt and the test returned green. One mutation per behaviour; the numbers are the failed checks of the mutant.

**Scanner rows (S1-S5)**

| Mutation (behaviour removed) | Result |
|---|---|
| S1 floor taught by a carrier | RED rc=1 test_tuning_scan: 1850 checks, 1 failed |
| S2 merge of adjacent channels | RED rc=1 test_tuning_scan: 1850 checks, 4 failed |
| S2 nearest-bin fallback | RED rc=1 test_tuning_scan: 1850 checks, 1 failed |
| S2 out-of-span count | RED rc=1 test_tuning_scan: 1850 checks, 2 failed |
| S3 shared points kept once | RED rc=1 test_tuning_scan: 1850 checks, 1 failed |
| S4 backward direction | OLD TEXT NOT FOUND |
| S4 step back wraps | RED rc=1 test_tuning_scan: 1850 checks, 2 failed |
| S5 duplicate hit refused | RED rc=1 test_tuning_scan: 1850 checks, 2 failed |
| S3 point cap | RED rc=1 test_tuning_scan: 1850 checks, 1 failed |
| S4 backward direction | RED rc=1 test_tuning_scan: 1850 checks, 5 failed |

**Bookmark rows (B1-B4, C1) and CSV text**

| Mutation (behaviour removed) | Result |
|---|---|
| CSV quote inside an unquoted field | RED rc=1 test_tuning_bookmarks: 31370 checks, 1 failed |
| CSV text after a closing quote | RED rc=1 test_tuning_bookmarks: 31374 checks, 2 failed |
| CSV formula guard | RED rc=1 test_tuning_bookmarks: 31321 checks, 6 failed |
| B1 tag folded to lower case | RED rc=1 test_tuning_bookmarks: 31272 checks, 3 failed |
| B1 rekey onto an existing record | RED rc=1 test_tuning_bookmarks: 31272 checks, 3 failed |
| B1 version refused | RED rc=1 test_tuning_bookmarks: 31272 checks, 2 failed |
| B2 waterfall ceiling above floor | RED rc=1 test_tuning_bookmarks: 31284 checks, 4 failed |
| B2 repeated favourite name refused | RED rc=1 test_tuning_bookmarks: 31272 checks, 2 failed |
| B3 guard removed on reading | RED rc=1 test_tuning_bookmarks: 31272 checks, 2 failed |
| B3 repeated marker number | RED rc=1 test_tuning_bookmarks: 31272 checks, 2 failed |
| B3 merge keeps an existing note | RED rc=1 test_tuning_bookmarks: 31272 checks, 1 failed |
| B4 unit scale | RED rc=3221225477  |
| B4 tolerance edge | RED rc=3221225477  |
| B4 cell cleaning counted | RED rc=1 test_tuning_bookmarks: 31272 checks, 1 failed |
| C1 duplicate needs the same mode | RED rc=1 test_tuning_bookmarks: 31272 checks, 1 failed |
| C1 pack version refused | RED rc=1 test_tuning_bookmarks: 31272 checks, 3 failed |
| C1 pack size limit | RED rc=1 test_tuning_bookmarks: 31272 checks, 1 failed |

Two of the station-overlay mutants ended in an access violation (rc 3221225477) instead of a counted failure: the test indexes `lookup(...)[0]` and the mutant returns an empty list. Red either way; indexing through a bounds-safe helper would make the failure a counted one.

**Operating rows (T1-T7, C2) and signal blocks (T5, T6)**

| Mutation (behaviour removed) | Result |
|---|---|
| T1 band switch inside one band | RED rc=1 test_tuning_operate: 22545 checks, 2 failed |
| T1 field mask on recall | RED rc=1 test_tuning_operate: 22545 checks, 3 failed |
| T2 equal entry not recorded | RED rc=1 test_tuning_operate: 22545 checks, 8 failed |
| T2 forward entries dropped on a new push | RED rc=1 test_tuning_operate: 22545 checks, 4 failed |
| T2 16-entry capacity | RED rc=1 test_tuning_operate: 22545 checks, 2 failed |
| T2 first entry kept in a spin | RED rc=1 test_tuning_operate: 22545 checks, 8 failed |
| T3 offset clamp | RED rc=1 test_tuning_operate: 22545 checks, 6 failed |
| T3 clear on retune edge | RED rc=1 test_tuning_operate: 22545 checks, 2 failed |
| T4 CB channels 23/24 order | RED rc=1 test_tuning_operate: 22545 checks, 1 failed |
| T4 marine duplex 78-86 | RED rc=1 test_tuning_operate: 22545 checks, 2 failed |
| T4 cursor wraps back | RED rc=1 test_tuning_operate: 22545 checks, 4 failed |
| T7 origin gate | RED rc=1 test_tuning_operate: 22545 checks, 5 failed |
| T7 master switch | RED rc=1 test_tuning_operate: 22545 checks, 9 failed |
| T7 .exe only | RED rc=1 test_tuning_operate: 22640 checks, 6 failed |
| T7 UNC and device paths | RED rc=1 test_tuning_operate: 22545 checks, 1 failed |
| T7 double launch window | RED rc=1 test_tuning_operate: 22545 checks, 6 failed |
| C2 cycle step wraps | RED rc=1 test_tuning_operate: 22545 checks, 1 failed |
| C2 bookmark list wraps | RED rc=1 test_tuning_operate: 22545 checks, 1 failed |
| C2 centre dead band | RED rc=1 test_tuning_operate: 22545 checks, 1 failed |
| T6 attack shaping | RED rc=1 test_tuning_dsp: 32277 checks, 10 failed |
| T6 trigger while playing | RED rc=1 test_tuning_dsp: 32277 checks, 1 failed |
| T6 burst length | RED rc=1 test_tuning_dsp: 32277 checks, 2 failed |
| T6 frequency limit | RED rc=1 test_tuning_dsp: 32277 checks, 1 failed |
| T5 pan law | RED rc=1 test_tuning_dsp: 32277 checks, 22 failed |
| T5 invert | RED rc=1 test_tuning_dsp: 32277 checks, 3 failed |
| T5 hold then centre | RED rc=1 test_tuning_dsp: 32277 checks, 1 failed |
| T5 tone ratio gate | RED rc=1 test_tuning_dsp: 32277 checks, 2 failed |
| T5 disabled passes through | RED rc=1 test_tuning_dsp: 32277 checks, 1 failed |
| T7 quote escaping (backslash run before a quote) | RED rc=1 test_tuning_operate: 22545 checks, 761 failed |

The first attempt at the quote-escaping mutant did not match the source text (an escaping slip in the mutation file) and was re-run with the exact text: red, 761 failed checks, restored green.

## 9. UNVERIFIED and not run

- **Nothing is wired and no screen was built**: section 4 is a hand-off, none of it was exercised in a running program.
- **The full headless gate was NOT run**: `ctest -L headless` over the whole tree (about 600 tests) was not built or run here. This lane edited no existing file (`git diff --name-status d3630d78 HEAD` lists additions only), so no existing test can change; the machine was at full load for hours (one build and run of one test took minutes). What was run: the four new tests and the five existing tests nearest the new files (`test_scanner`, `test_freq_markers`, `test_freq_manager`, `test_freq_import`, `test_noise_floor_tracker`): 9 of 9 passed.
- FRS, GMRS and MURS frequency digits were taken from the fetch tool's summary of the eCFR XML, not from the raw text (section 5).
- The UK 27/81 CB table is not built (section 3).
- The bandwidth tables of `key_actions.hpp` (`defaultBandwidths`) and the mode hints of the channel plans are defaults, not from any document; the CB mode (AM) in particular is a default (the allowed modulation depends on the national rule, which was not read).
- The marine table's row-by-row pairing is checked as a set and by the duplex rule, not against a row-per-row opened page (section 5).
- The launcher's real spawner was not written or run; only the stand-in.
- The real `config.json` was not touched: sha256 after the work `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3`, equal to the value in the standard rules.
- No radio was opened and no connection was made except read-only fetches of the documents in section 5; nothing was downloaded into the repo (two PDFs and their extracted text sit in the tool's own result folder and `/tmp/tuning-itu`, outside the repo); no Windows dialog was raised (no certificate, signing, firewall, credential or file-dialog call; the launcher test starts no process).
