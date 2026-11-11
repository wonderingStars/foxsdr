# Signal blocks for the pre-release features (2026-11-11)

Branch `feat/signal-blocks` (local, off `b9208fba`, not pushed, not merged, no version bump). Six standalone blocks and their
tests. **Nothing is wired**: no file of the engine, pipeline, receiver, settings, screen, generator or the existing
squelch/recorder classes was touched. Design: `D:/foxsdr-build/prep/pre-release-features-design.md` sections 0-3, 7, 14, 15, 18.
Facts: `D:/foxsdr-build/prep/features-external-facts.md` (sections 11-13), re-checked against the sources named below.

All new code is header-only (so no source list changed). The only existing file edited is `tests/CMakeLists.txt`, one `TIMEOUT 300`
line for `test_ctcss` and `test_dcs` (the same registration the other long tests there carry). Tests are picked up by the glob.

| Block | Header | Test | Commit |
|---|---|---|---|
| 1 CTCSS | `src/dsp/ctcss.hpp` | `tests/test_ctcss.cpp` | e357eaaf |
| 2 DCS | `src/dsp/dcs.hpp` | `tests/test_dcs.cpp` | ea8f2cf0 |
| 3 Replay ring | `src/core/replay_ring.hpp` | `tests/test_replay_ring.cpp` | 0deec9d1, 2e60d45b |
| 4 Clips | `src/core/clip_recorder.hpp` (+ `ReplayRing::sampleFloor()`) | `tests/test_clip_recorder.cpp` | d20162a3 |
| 5 S-meter maths | `src/core/smeter_cal.hpp` | `tests/test_smeter_cal.cpp` | b31f245e |
| 6 ADIF | `src/core/adif.hpp` | `tests/test_adif.cpp` | 89e7b482, f56fc5a0 |
| (test helpers, timeouts) | `tests/alloc_probe.hpp`, `tests/signal_block_gen.hpp`, `tests/CMakeLists.txt` | | e357eaaf, c39a8698 |

Shared test helpers: `alloc_probe.hpp` is the repo's counting-`operator new` pattern (as in `test_engine_publish_alloc.cpp`) in a reusable
header (include in exactly one TU per test); `signal_block_gen.hpp` makes noise, discriminator-shaped noise, tones and a speech-like synthesiser.

## Sources, and what is and is not verified

| Fact | Source read in this session | Status |
|---|---|---|
| CTCSS tone list (39 + 11 extras), 150.0 non-standard, low tones decode slower | Wikipedia "Continuous Tone-Coded Squelch System" (cites RS-220-A 1979, TIA-603-E 2016 p. 10) | **UNVERIFIED against the standard**; the standard text was not read |
| DCS: 134.4 bit/s, 23-bit Golay (23,12), 12 data bits (last 3 = 001) + 11 check, LSB first, 83 codes | Wikipedia "Squelch" (DCS section) and the CTCSS article | **UNVERIFIED against the standard** |
| Golay (23,12): d = 7, corrects 3, generators x^11+x^10+x^6+x^5+x^4+x^2+1 and x^11+x^9+x^7+x^6+x^5+x+1 (reciprocal pair) | Wikipedia "Binary Golay code" | read; textbook fact |
| Which generator/bit order DCS uses | **Not stated anywhere read; derived** (below) | derived; checked against a second third-party table; **UNVERIFIED against the standard and a real signal** |
| Inverted-code equivalence chart (83 rows) | RadioReference wiki, "DCS" page, Inverted Codes table (`wiki.radioreference.com/index.php/CDCSS`) | read; the derived convention reproduces all 83 rows |
| ETSI TS 103 236 (CTCSS and DCS standard) | URL `etsi.org/deliver/etsi_ts/103200_103299/103236/01.01.01_60/ts_103236v010101p.pdf` fetched | **unreadable here** (AES-encrypted PDF, no PDF library allowed); the one document that could settle the DCS convention |
| S9 = -73 dBm below 30 MHz, -93 dBm above, 6 dB per S unit, "S9 + N dB" | Wikipedia "S meter" and hamwaves.com "Decibel & S-Readings", both quoting IARU Region 1 Technical Recommendation R.1 (1981) | **UNVERIFIED against R.1 itself** (its archive copy is not reachable). The 30 MHz edge is the conventional HF/VHF boundary; whether 30.000000 MHz counts as VHF is not stated: implemented as VHF/UHF |
| ADIF 3.1.8: ADI format, data types, SWL, FREQ, BAND, MODE, OPERATOR, header | `https://adif.org/318/ADIF_318.htm` read in full (saved text, grepped sections III.A-C, IV.A) | read; primary |
| Which of OPERATOR / STATION_CALLSIGN a listener goes in | not stated | **UNVERIFIED**; OPERATOR is written |

### The DCS convention, in full (derived, because nothing read states it)

The task says to derive the generator "by testing that the 83 codes are valid codewords". That test cannot discriminate (the check bits are
computed, not given), so a different discriminator was used and a second source was found:

1. Four arrangements are possible: generator 0xAE3 or 0xC75, and transmitted bit t_j as the coefficient of x^(22-j) or of x^j. They form two
   distinct codes (each pair is the same code). Data = t_0..t_11 (code bits LSB first, then 0,0,1), check = t_12..t_22.
2. **Alignment-collision test** (stated reason for only 83 of 512 codes being standard: "to prevent falsing due to alignment collisions"): no
   cyclic rotation of one standard code's word may be a marked word (001 in positions 9-11) of another standard code. Arrangement
   {0xAE3, t_j <-> x^(22-j)} (equivalently {0xC75, t_j <-> x^j}): **0 collisions**. The other: **34**. A random 83-code subset: 12 or more
   (200 samples: min 12, mean 30). (`testAlignmentCollisionProperty`.)
3. **RadioReference inverted-code chart**: the first arrangement reproduces **83 of 83** standard rows (a code transmitted inverted reads as
   the partner code); the other reproduces 4 of 83. (`testInverseEquivalence` holds the chart as data.)
4. Consequence, a property of the code and not a choice: for 82 of the 83 codes, "code A inverted" is on the air the same stream as
   "code B normal" (023/047, 025/244, 026/464, 031/627 ...; 172 reads as 036, which is not standard). A decoder cannot tell them apart, so
   `DcsDecoder` reports the **normal reading as the code and the other as the alternate**, and a chosen code matches either reading.

Used: g(x) = x^11+x^9+x^7+x^6+x^5+x+1 (0xAE3); transmitted bit t_j is the coefficient of x^(22-j); t_0..t_8 = code bits LSB first;
t_9,t_10,t_11 = 0,0,1; t_12..t_22 = check bits (remainder). The test file's own encoder uses the **other** form (0xC75, t_j <-> x^j, search for
check bits) and agrees with the header for all 512 codes.

## Block 1: CTCSS - `src/dsp/ctcss.hpp`

Classes: `CtcssHighpass`, `CtcssDetector`, tone table `kCtcssStandardTones` (39) / `kCtcssExtraTones` (11), `ctcssToneHz(i)` (50, merged,
ascending), `ctcssIsStandardTone`, `ctcssIndexOfHz`.

**Calls and threads**
- `CtcssHighpass::configure(rate)` set-up (allocation-free, but not against a running `process`); `process(in, out, n)` DSP thread, in-place allowed,
  allocation-free (proved by counting `operator new`), non-finite samples read as 0. Rate >= 4 kHz (refused below).
- `CtcssDetector::configure(rate, includeExtraTones)` set-up, allocates. `process(in, n)` DSP thread, reads only, allocation-free (proved).
  `setSelectedToneHz(hz)` (0 = none; false for a tone not in the set in use) and `reset()` control or DSP thread. `detectedIndex()`, `detectedHz()`,
  `gateOpen()` atomics, any thread. Any rate >= 4 kHz; it decimates internally (mean to ~9.6 kHz, 4th-order Butterworth at 320 Hz, to 1.5-2.4 kHz).

**Method.** 10th-order inverse-Chebyshev high-pass (five biquads, double state; stop edge 254.1 Hz at 40 dB, pass edge 300 Hz), poles and zeros
from the textbook formulas, bilinear with pre-warp, computed for the rate in use. Detector: one exponentially-weighted complex single-bin
demodulator per tone, time constant `0.62 / (distance to the nearest other tone)` clamped to 0.06-0.30 s (so 67.0/69.3 Hz, 2.3 Hz apart, are
separable and the high tones are quick). A hop every 10 ms picks the strongest tone; it is PRESENT when it is 10x the median of the bank, 2x each
neighbour, and its normalised score `P / (band mean square x a/(2-a))` exceeds 30 (for noise alone the score is exponential with mean 1, so the
false-lock rate is set directly and equally for fast and slow tones). Lock after 6 hops (60 ms) of the same tone, release after 15 absent hops.
Chosen tone: `gateOpen()` is true only while the lock is on that tone.

**Measured** (`test_ctcss`, 212 checks):
- High-pass at 48000 / 24000 / 12000 / 200000 Hz: 300 Hz -1.092 / -1.088 / -1.072 / -1.093 dB; 400 Hz, 1 kHz, 3 kHz 0.000 dB; 254.1 Hz **-40.00 dB**;
  every one of the 50 tones <= -40.00 dB; DC -40 dB (the stop band is equiripple at 40 dB). Stated tolerance: 300 Hz within -1.2..+0.1 dB, 400 Hz within
  0.35 dB, 1k and 3k within 0.05 dB, stop band >= 40 dB.
- Detection, all 50 tones at 48 and 24 kHz, with speech-like audio (through the 300 Hz high-pass, as a transmitter leaves it), noise and a DC offset:
  50/50 locked correctly at both rates, worst detect time 0.250 s. Clean tone: 67.0 Hz detect 0.250 s, release 0.80 s (48k) / 0.78 s (24k); 250.3 Hz
  detect 0.110 s, release 0.200 s (pinned: 67.0 <= 0.60 / 1.00 s, 250.3 <= 0.25 / 0.30 s; low tones slower than high, asserted).
- Adjacent tones: for all 39 tones, choosing either neighbour never opens the gate; choosing the tone itself opens it; auto reads the right tone (0 wrong).
- False locks: white noise 600 s at 48 kHz **0**, discriminator-shaped noise + DC 300 s **0**, speech after the 300 Hz high-pass 300 s **0**; at 24 kHz 120 s
  each **0**. RAW speech with nothing removed below 300 Hz (a harder case than any real transmitter): 68 locks of any tone in 300 s at 48k (23 in 120 s at
  24k), a chosen single tone 0 / 0 / 1 / 9 openings in 150 s for 67.0 / 100.0 / 123.0 / 203.5 Hz. That is a property of tone decoding on unfiltered speech,
  stated, not hidden.
- Level independence (30 dB down), +-0.5 % off-frequency tones, a 0.8 DC offset, non-finite input, chunking by 1 sample, rates 8 / 16 / 44.1 / 200 kHz.
- No allocation in either `process`.

**Break-it** (each restored; `git checkout`): chosen-tone gate forced open -> adjacent/selection test and the chosen-tone-against-speech test RED;
high-pass order 10 -> 6 -> the 300 Hz, 400 Hz and 254.1 Hz checks RED; `kMinScore` 30 -> 1 -> the white-noise and discriminator-noise false-lock checks and the
release checks RED; neighbour rule removed (`kPeakToNeighbour` 0) -> adjacent/selection and raw-speech checks RED.

**Proven on generated signals only.** No CTCSS recording exists in `D:/foxsdr-samples` (MANIFEST checked) and none with a stated licence was found
(web search 2026-11-11: only SigIDWiki pages with unstated terms). Nothing was downloaded. UNVERIFIED on a real repeater.

## Block 2: DCS - `src/dsp/dcs.hpp`

Classes/functions: `kDcsStandardCodes[83]` (octal), `dcsIsStandardCode`, namespace `dcs` (`encode`, `invert`, `rotate`, `syndrome`, `correct`,
`correctionTable`, `inversePartner`), `DcsDecoder`.

**Calls and threads.** `DcsDecoder::configure(rate)` set-up (allocates; builds the 2048-entry Golay syndrome table and the partner table, local statics, so
never on the audio path). `process(in, n)` DSP thread, allocation-free (proved). `setSelected(code, polarity)`, `setMaxCorrectedBits(0..3)`, `reset()` control
or DSP thread. `detectedCode()`, `detectedPolarity()` (1 normal, 2 inverted), `detectedAltCode()/AltPolarity()`, `isDetected(code, polarity)`, `gateOpen()`,
`lastCorrectedBits()` atomics, any thread.

**Method.** Same style of decimating front end (low-pass 250 Hz), peak-tracking slicer, second-order bit-clock PLL, sample the middle third of each bit.
Every bit time the last 23 bits are a candidate word, tried normal and complemented; accepted when it corrects (<= `maxCorrectedBits`, default 3, exact
syndrome table of the perfect code) to a word with marker 001 and one of the 83 codes. Lock after 3 agreeing words at one alignment, release after 2
consecutive misses at that alignment. A chosen code matches either reading of the stream (see above).

**Measured** (`test_dcs`, 2987 checks):
- All 512 codes make codewords equal to the independent encoder's; minimum distance among the 83 words in both polarities **7**; the correction table has 2048
  distinct patterns; **every** 1-, 2-, 3-bit error pattern (three codes, exhaustive) corrects back; **all 8855 four-bit patterns** decode to some other codeword,
  never the original (595 of them land on a marked standard word, which is why three agreeing words are required).
- Alignment collisions among the 83 standard words: 0. Chart: 83/83 rows; 82 codes have a standard partner, 172 has a non-standard one (036).
- Round trip: all 83 codes x normal/inverted = 166 signals at 48 kHz and at 24 kHz, random start bit and +-1 % clock error: 0 bad, worst lock 0.665 s; the wrong
  polarity chosen never opens the gate.
- Speech (after the 300 Hz high-pass) + noise + DC offset: 10/10 locked; data 10 dB weaker over noise: locked.
- Bit errors in every word: 1, 2, 3 errors lock correctly at 0.53 / 0.53 / 0.67 s; with `setMaxCorrectedBits(2)` three-bit-error words are refused; four-bit
  errors never give the right code; one word in four ruined: the lock holds. Lock 0.52 s, release 0.26 s after the data stops.
- False locks: 4 minutes each of white noise, discriminator noise + DC, RAW speech-like audio, and speech after the 300 Hz high-pass at 48 kHz: **0, 0, 0, 0**;
  a 100 Hz CTCSS tone: 0.
- Rates 8, 16, 44.1, 96 kHz decode; non-finite input, chunking by 1 sample, selection errors, no allocation.

**Break-it:** reciprocal generator (0xC75) -> the independent-encoder check (512 codes), the collision test and the chart test RED; `kMaxCorrectable` 0 -> the 1/2/3-bit
error tests RED; `kLockWords` 1 -> the 3-bit-limit test, the 4-bit test and all false-lock checks RED.

**Generated signals only.** No DCS recording exists in the samples or was found with a licence. The DCS signal in the tests is NRZ +-0.1 full scale shaped by two 350 Hz
one-pole filters; real transmitter deviation, polarity of the discriminator and the 134.4 bit/s tolerance are UNVERIFIED.

## Block 3: replay ring - `src/core/replay_ring.hpp`

`core::ReplayRing`. Mono int16 at the rate given, quantised exactly as `core::Recorder` does (clamp, `round(v*32768)`, saturate; NaN -> 0). One gate bit per
1024-sample block (set if the gate was open for any part of the block). Sample numbers are absolute and keep counting through resizes.

**Calls and threads.**
- `resize(rate, seconds)` control thread, ring not in use elsewhere (the owner installs it with the recorders' pointer hand-off: install after allocation, remove
  before free). Keeps the newest audio and the numbering when growing or shrinking; `seconds <= 0` frees everything (Off = no memory); refuses a bad rate or length or
  > 600 s; a rate change discards the audio and keeps `head()`.
- `write(in, n, gateOpen)` DSP thread, single writer; allocation-free (proved), lock-free, no file/wait; chunks > 16384 samples are split.
- `head()`, `oldest()`, `sampleFloor()`, `capacity()`, `seconds()`, `gateOpenAt(s)`, `copyRange(start, n, out, &first)`, `copyLast(n, out, &first)`,
  `copyGated(start, n, out, cap)` any thread, concurrently with the writer. Seqlock-style: the writer announces the end of the chunk it is writing (`reserved_`)
  before storing and publishes `head_` after; a copy drops the front it cannot prove intact and says where its run starts. Samples are `std::atomic<int16_t>` (relaxed),
  so a concurrent read is defined behaviour. ThreadSanitizer was NOT run (MSVC build); the stress test below stands in.

**Measured** (`test_replay_ring`, 1108 checks): wrap-around with odd write sizes (200 writes), shorter-than-N (returns what exists), exact-N, N > capacity, a single
write larger than the ring; resize grow / shrink (newest kept) / off / on / same / rate change / empty ring; gate bits including a block that opens mid-block, packed
"skip silence" copies, partial first/last blocks, a wrapped-out block reported closed; quantisation corners; 1.5 s of a paced writer against a reader copying the whole
ring every other read (about 50000 reads, 6e8 samples checked, **0 bad reads**, and `goodReads > 10` so the check does not simply refuse everything); no allocation in
`write` (500 calls).

**Break-it:** gate bit never cleared on a new block -> "no stale open block" check RED; overwrite check removed (`safeFrom = 0`) -> the concurrent test reports
**2652 bad reads** RED (the first version of this test did NOT catch it - it only read the newest 30000 of 48128 samples - and was strengthened: commit 2e60d45b); resize copying
shifted data -> both resize content checks RED.

## Block 4: clips - `src/core/clip_recorder.hpp`

`core::ClipTrigger` (pass thread), `core::ClipWriter` (pass pushes, one worker services), `ClipCmd`, `ClipSettings`, `ClipEvent`.

**Contract (pinned by tests).** Clip starts `preRollS` before the pass that saw the gate open (clamped to sample 0) and ends `tailS` after the pass that saw it close; a gate that
reopens inside the tail continues the same clip; **the minimum length is measured on the transmission itself** (first open to last close, no pre-roll or tail - a departure
from reading it as the file length, which pre-roll plus tail would always satisfy); exactly `minS` is kept, a click is dropped (the file is deleted); a gate that never closes splits
every `maxClipS` with no sample lost or doubled at the joint and no pre-roll on the continuation (a short last segment of a split clip is never dropped); `endNow()` for a stop or a radio
removal; settings clamped (pre 0-10 s, tail 0-30, min 0-60, max 10-3600, bad numbers take defaults).

**Calls and threads.**
- `ClipTrigger::configure(settings)` returns the clamped copy. `update(gate, headSample, unixNow, freqHz, modeText, out[2], cap)` once per **pass**, never the DSP thread: appends
  0-2 commands (2 only for a split: Close then Open), integer arithmetic only, allocation-free (proved); with `cap < 2` it does nothing. `endNow(head, out, cap)`.
- `ClipWriter::push(cmd)` pass thread, wait-free into a 32-slot SPSC queue (a full queue counts `commandsLost()` and never blocks; proved allocation-free).
  `service(ring, &events)` and `finishAll(ring, &events)` **one worker thread only**: they create files (through `core::Recorder::prepare`/`openFile`/`begin`, so the same header,
  exclusive-create naming `name-2.wav`, crash contract), copy audio from the ring in 4096-sample chunks, close, delete dropped clips, append `index.csv`. They block on the disk.
  Counters (`clipOpen()`, `clipsSaved()`, `clipsDropped()`, `clipsFailed()`, `commandsLost()`, `lastClipS()`) are atomics for the publish.
- File name `<prefix>_YYYYMMDD_HHMMSS_<freq-kHz>.wav` (time of the gate opening; local time by default like the manual takes, UTC with `Config::utcNames` for tests), mono 16-bit at
  the ring rate; `index.csv` in the same folder, header `start_utc,radio,frequency_hz,mode,duration_s,peak_dbfs,gap` (the design's columns plus `gap`), one row per saved clip.
- Failures are events, never exceptions: unwritable folder (`Failed`, error text, `clipsFailed`, next clip works after `setDirectory`), a disk that refuses writes (clip finishes,
  `writeFailed`), a worker later than the ring (`gap` flag and `,gap` in the index; the lost stretch is absent, not filled), the ring switched off mid-clip, a Close that never came.

**Measured** (`test_clip_recorder`, 118 checks): trigger on synthetic gate traces (basic, hang merge, separate clips, zero tail/pre/min, minimum boundary +-1 pass, two blips inside the
tail, max-clip split of 25 s at 10 s, endNow in each state, clamps, other rates, sample-0 clamp, no allocation in 2000 updates); end to end on real files in a scratch folder with a ramp as
audio - a 5 s transmission with 2 s pre-roll and 3 s tail yields exactly 480000 samples equal to the exact ring slice, name `radio1_clip_20260101_000010_118700.wav`, index row
`2026-01-01T00:00:10Z,1,118700000,NFM,10.000,<peak>,`; hang merge gives one 9.0 s file; a 0.2 s click leaves no file and no index row; the three split files concatenate to one unbroken
ramp; unwritable folder then recovery; read-only handle as "disk full"; worker behind a 5 s ring (gap); finishAll mid-clip gives a valid header and the exact slice; two radios and a same-second name
collision; a real worker thread (2 ms service loop) against a paced feeder - 2 files, 0 lost commands, content exact.

**Break-it:** pre-roll removed -> start/size/content checks RED; hang rule removed (`else if (true)`) -> merge, endNow and file-count checks RED; minimum ignored -> click, boundary, endNow and
dropped-count checks RED; dropped file not deleted -> file-count checks RED; tail ignored -> close sample, size, duration and index checks RED.

## Block 5: S-meter calibration maths - `src/core/smeter_cal.hpp`

Namespace `core::smeter`: `s9Dbm(f)`, `dbmToS(dbm, f)` -> `SReading{sUnit, overS9Db, units, valid}`, `formatS`, `formatDbm`, `sToDbm`, `offsetFromMeasurement(knownDbm, measuredDbfs, offset)`,
`applyCal(entry*, dbfs, gainNow)` -> `CalResult{dbm, calibrated, exact}`, `CalEntry`, `CalTable` (`set`, `find`, `dbm`, `clearAt`, `toText`, `fromText`).

Convention (see Sources): S9 -73 dBm for f < 30 MHz, -93 dBm for f >= 30 MHz, 6 dB per unit, S9 + N dB above (display saturates at +60), S0 floor, `units` = 9 + dB/6 at or below S9 and
`9 + over/60` above (so S9+20 dB = 9.333, the design's "9.33"). Calibration: `offset = known dBm - measured dBFS` for a band; `dBm = dBFS + offset - (gainNow - gainAtCal)` when the driver gain is in dB; with gain
in steps no compensation and `exact` is true only at the calibrated setting. Uncalibrated: the dBFS number stands in as an estimate, flagged. **The squelch scale is not touched.**
All pure; allocation-free (proved) except `CalTable::set`/`fromText`.

**Measured** (`test_smeter_cal`, 247 checks): the published HF table S1..S9 (-121 ... -73) and S9+20/+40 (-53/-33), the VHF table (-141, -105, -99, -93, S9+20 at -73, S9+40 at -53), both sides of the
30 MHz edge (29.999999 MHz S9 vs 30.0 MHz S9+20 dB), 6 dB per unit on both sides, the volt relation (50 uV -> -73 dBm, 5 uV -> -93 dBm into 50 ohm), rounding, clamps, NaN/inf; offset applied;
gain compensation up and down (+10 dB and -12 dB gain at a constant -73 dBm signal still read -73); step-gain not compensated and flagged; result clamped to [-200, +30] dBm; per-band tables,
`[lo, hi)` edges, narrowest band wins, replace-same-band, bounded at 64; text round trip and eight kinds of junk text (whole text refused, table unchanged).

**Break-it:** constants swapped (50 uV as -93 dBm) -> every fixed-point check RED; gain compensation removed -> the two gain checks RED; 5 dB per S unit -> RED; offset sign flipped -> RED.
**Not proven on any real tuner**: needs a signal generator; the owner has none recorded (design section 15).

## Block 6: ADIF writer - `src/core/adif.hpp`

Namespace `core::adif`: `ListenEntry`, `Options`, `Stats`, `adifFromEntries(entries, options, createdUnix, &stats)`, `writeAdiFile(path, entries, options, createdUnix, error, &stats)` (worker
thread; temp file + rename; reports failures), `asciiFold`, `cleanCall`, `cleanGrid`, `utcFromUnix`, `bandFor`, `modeFor`, `formatFreqMhz`, `field`, `header`, `record`. **No reader**: the design does not ask for
one; the test carries an independent reference reader instead.

**Format decisions, each from the spec text.** ADI file: a header text line (first character not `<`), `ADIF_VER 3.1.8`, `PROGRAMID FoxSDR`, `PROGRAMVERSION`, `CREATED_TIMESTAMP` (15 chars), `<EOH>`;
records of `SWL=Y` (type B), `CALL`, `QSO_DATE`, `TIME_ON`, `QSO_DATE_OFF`/`TIME_OFF` (duration), `FREQ` in MHz (up to 6 decimals, trailing zeros trimmed), `BAND` from the enumeration's own edges
(none outside them, e.g. the air band), `MODE` AM/FM/CW/SSB with `SUBMODE` LSB/USB (RAW/DSB/unknown: omitted), `OPERATOR`, optional `STATION_CALLSIGN`, `MY_GRIDSQUARE` (validated 2/4/6/8, normalised),
`COMMENT` (station name), `NOTES` (type M, line breaks as CR LF), `APP_FOXSDR_PEAK_DBFS` and `APP_FOXSDR_RADIO` (type N), `<EOR>`. Lengths are exact byte counts because the output is pure printable ASCII:
the spec forbids the `_INTL` types in ADI files, so non-ASCII text is **folded** (accents dropped, `ae`/`ss`, curly quotes, dashes, ellipsis, NBSP; everything else and invalid UTF-8 -> `?`). That is lossy by
the format's nature; the CSV export (not part of this lane) is the lossless one. `<` in free text is kept (the format is length-delimited). Entries with no call are skipped by default (`requireCall`; counted in
`Stats::skippedNoCall`), as are dates outside 1930-9999 (`skippedBadTime`). Calendar maths is a self-contained days-to-civil algorithm, cross-checked against `gmtime` on 20000 random times.

**Measured** (`test_adif`, 553 checks): a hand-checked golden record compared byte for byte; header requirements through the reference reader; 300 random entries (call, no call, all modes and
bands, UTF-8 and invalid text, `<EOR>` inside notes) parsed back and every field validated against its type (Date/Time/Number/Boolean/String/MultilineString/GridSquare, MODE/SUBMODE/BAND
enumerations, BAND contains FREQ, no duplicate field names, the file is pure ASCII); the length of every non-ASCII text field equals the folded length (and differs from the input's UTF-8 length); band
edges (7.0/7.3, 14.0/14.35, 60 m, air band, 23 cm, 2190 m), midnight roll-over of QSO_DATE_OFF, duration corner cases, options cleaning, skipped entries and counts, empty list, 20000 entries in 60-80 ms
(6.4 MB), file write (creates the folder, replaces atomically, leaves no `.tmp`, reports a blocked folder and an unrenamable target).

**Break-it:** FREQ in kHz -> golden, reader and `formatFreqMhz` checks RED; text not folded (UTF-8 lengths written) -> RED; `SWL` field removed -> RED (the first run **aborted** on a `substr(npos)`
throw instead of listing the failure; fixed, f56fc5a0); `<EOH>` removed -> the test aborts (a segfault on a null field, exit 139: still red).
**UNVERIFIED on a real logger**: no logger was available to import a file; the reference reader is my reading of the specification, not another program.

## Tests run (this machine, Release, 4 jobs)

Build: `cmake -S . -B D:/foxsdr-build/feat-blocks -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF -DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`
(the options of `D:/foxsdr-build/mr-dsp`, minus the Slint window), then `cmake --build ... --config Release --parallel 4 --target <tests>`. Only `cascade_engine` (and its libraries) and the test executables were built.

`ctest -C Release -j4 -R "^(test_agc_modes|test_noise_blanker|test_squelch|test_recorder|test_demod_agc|test_record_start|test_record_finish_async|test_stop_ends_recordings|test_pipeline_agc|test_patch_squelch|test_ctcss|test_dcs|test_replay_ring|test_clip_recorder|test_smeter_cal|test_adif)$"`
-> `100% tests passed, 0 tests failed out of 16` (Total Test time 60.30 s). The ten existing tests are the AGC, noise-blanker, squelch, recorder, record-start, async-finish, stop-ends-recordings, pipeline-AGC and patch-squelch
tests of the same area; the window tests were not run and the application was not started. Own checks: ctcss 212, dcs 2987, replay_ring 1108, clip_recorder 118, smeter_cal 247, adif 553; all 0 failed.
Allocation-free `process`/`write`/`update`/`push` is proved by `AllocProbe` for the HP, both detectors, the ring, the trigger, the writer's `push` and the S-meter functions; `ClipWriter::service` and `adifFromEntries` allocate and are worker-thread calls by design.
Run-to-run: the detector tests are deterministic (seeded generators); the ring and worker-thread tests are timing-dependent in what they sample but assert only invariants. The tests were run once green after the last change plus the break-it runs, not 50x.

## Departures from the design, and why

1. **CTCSS detector is a bank of smoothed single-bin demodulators, not 200-300 ms Goertzel blocks**, with per-tone time constants; same role (a bank over the standard tones with a decision and hysteresis), better separation of the 2.3 Hz pair at one cost per sample. The detector decimates itself from any rate, so the tap rate is the engine lane's choice.
2. **DCS Golay convention derived** (design said check or derive before writing the decoder): the derivation and the two checks are above. **DCS reports a canonical reading and an alternate** (023 normal is 047 inverted); the design's "both polarities N and I" picker cannot offer independent choices for 82 of the 83 codes.
3. **Clip minimum length** is measured on the transmission, not on the file (design wording ambiguous).
4. **A clip's file is created when the gate opens** and deleted if the transmission turns out shorter than the minimum, so a click makes and removes a tiny file. Alternative if the churn matters: the worker could defer creation until the tail has elapsed for short transmissions (needs one extra "pending" state; the ring already holds the audio).
5. **ADIF**: the spec forbids `_INTL` in ADI, so text is folded to ASCII; the design implied exporting what the user typed.
6. **S-meter units** value is `9 + over/60` above S9 (the design's 9.33 example); formatted text is `S9+20 dB`.
7. One `TIMEOUT 300` line in `tests/CMakeLists.txt` (the only edit outside new files).

## Owner decisions needed

- **DCS pairs in the UI.** For 82 of 83 codes "N" and "I" selections are not independent of the other code (023N = 047I). Show "023 (= 047 inverted)" in the readout and let the picker offer the 83 codes in the normal polarity plus an "inverted" tick that maps to the partner, or show both? The block supports either (a chosen code matches either reading).
- **CTCSS latency.** 67.0 Hz needs about 0.25 s to lock and up to 0.8 s to release (250 Hz: 0.11 s / 0.2 s) because the 2.3 Hz spacing forces a narrow filter. The design's 150 ms hold sits on top. Acceptable for the gate, or should the low tones trade separation for speed?
- **ADIF OPERATOR vs STATION_CALLSIGN** for an SWL listener (UNVERIFIED), and whether text may be folded to ASCII in `.adi` or the user wants ADX later.

## WIRING HAND-OFF (what the engine lane must do; nothing below has been done)

The NR list (`docs/agc-noise-blanker-design.md` section 4) applies to every per-radio setting: DSP class + `Pipeline` setter/mirror, op number (`foxsdr_api.h`) and name (`src/core/app_commands.cpp`), range check and remote
profile (`fox_engine_api.cpp`), `Engine::applyCommand` case, state fill and `FoxReceiverState` fields appended, `receiver_snapshot.cpp` compare, `AppConfig` + clamp + writer, `config_halves.hpp`, `SavedRadio` +
`sanitiseSavedRadios`, `applyConfig`/`fillConfig`/start-up push, `engine_radios.cpp` / `engine_radio_saved.cpp`, `isRadioScopedOp`, mock engine + API.md + `docs/api-0.3-additions.md`, the Slint bank, tests, drive scripts, ledger rows, 33 catalogues.
Op numbers from the design: 0x0113 `SET_REPLAY`, 0x0114 `REPLAY`, 0x0115 `REPLAY_SAVE`, 0x0116 `SET_VOX_RECORD`, 0x0117 `SET_TONE_SQUELCH`, 0x0118 `SET_METER_UNITS`; allocate them in one table (conflict table, section 18).

**A. Replay ring (feature 1; `ReplayRing`).** Own it in `Receiver` (or `Pipeline`) as a `std::unique_ptr<ReplayRing>`-style member, one per radio. Control thread: `resize(Pipeline::kAudioRateHz, seconds)` on an op (0 = Off frees it) with the recorder-style hand-off: stop the DSP thread from touching it, resize/replace, install again (pipeline.hpp:807-831 contract). DSP thread: call `write(monoOut_, n, squelchOpenNow)` in `pipeline.cpp` right beside `audioRecorder_->writeAudio` (the finished, post-limiter mono `monoOut_`, ~line 2433), one call per block, with the gate the pipeline already computed (`squelchOpen_`, or the tone-gate once feature 6 exists). Nothing else is needed on the DSP thread for the ring. The replay *player* (swapping the ring read into the sink write, cross-fades, ducking) is NOT in this lane: it needs `Pipeline` edits (sink write at ~2446-2466). Reading for "Save replay": the engine pass hands `copyLast(n, buf)` (or `copyGated` for skip-silence) to a worker that writes the WAV with `Recorder::prepare`/`openFile`/`begin`. State to publish: `replayLenS`, `replayFillS = (head - oldest) / rate`. Memory: 96 kB/s/radio.

**B. Clips (feature 2; `ClipTrigger` + `ClipWriter`).** Needs the ring (A). Per radio, in `Receiver`: a `ClipTrigger`, a `ClipWriter`, and the worker. Pass thread (the per-radio poll next to `pollRecordStarts`, `pumpExtraReceivers` for radios 2..8 and the first radio's equivalent): once per pass `trigger.update(pipeline.squelchOpen() /* the gate that includes the tone once feature 6 is in */, ring.head(), unixNowUtc, frequencyHz, modeName, cmds, 2)` and `writer.push(cmd)` for each. Worker: one thread (or a shared one servicing every radio's writer round-robin) calling `writer.service(ring, &events)` every 50-100 ms; turn events into the publish (`voxClipOpen = writer.clipOpen()`, `voxClipsToday` = count of `Saved` events dated today, `voxLastClipS = lastClipS()`). Stop and radio removal: `trigger.endNow(ring.head(), out, 2)` -> `push` -> `writer.finishAll(ring)` BEFORE the ring or the receiver is freed (RADIO_REMOVE "takes end first" order; `test_stop_ends_recordings` needs a clip case). `Config`: `directory = recordDir_/ "clips"`, `prefix = "radio<N>_clip"` (build it from the radio number or from `takePrefix`, whose exact output this lane did not read), `radio` = slot + 1, `rateHz = kAudioRateHz`, `utcNames = false`. Settings to save per radio: on/off, pre-roll, tail, minimum (and `maxClipS` if exposed). Refuse the tick when the squelch is off (threshold at the floor), as the design says. `pushing` while a worker is stalled loses commands (counted): publish `commandsLost()` as an issue-report word if wanted. The manual Record audio key is independent and untouched. Note `ClipWriter` takes a `Recorder`, so `ClipWriter` objects must not be moved after construction (non-copyable).

**C. CTCSS / DCS (feature 6).** Pipeline edit, per radio, NFM only, zero cost otherwise. (1) **Tap**: feed `CtcssDetector::process(audioBuf_, m)` / `DcsDecoder::process` from the NFM discriminator output `audioBuf_` where the decoder tap is taken (before AGC and squelch, ~pipeline.cpp:2091-2110); that stream is at the channel rate: pass that rate to `configure` (both accept any rate >= 4 kHz; reconfigure on a channel-rate change, with `reset()` on a retune). Only the active one runs (kind Off/CTCSS/DCS). (2) **Gate**: `Squelch` needs the `setExternalGate(bool)` the design describes (`open_ && gate_` as the ramp target; `squelch.hpp` was not touched here); publish a new atomic `toneGateOpen_` beside `squelchOpen_` and leave `squelchOpen_` the carrier state (auto-squelch and the heard-seconds counters read it). Each block: `squelch.setExternalGate(kind == Off || detector.gateOpen())`. With a chosen tone the detector holds the lock; with auto-detect (no choice) the readout `detectedHz()` / `detectedCode()+detectedPolarity()+detectedAltCode()` works with the squelch Off. (3) **Remove tone**: `CtcssHighpass::process` on the mono audio after the AGC, off by default, at the rate of the buffer it is given (it computes its own coefficients; call `configure` again if that rate changes; ~50 ms of settling after a reset). (4) State to publish: `toneKind`, `toneHz`, `toneCode`, `toneDetectedHz`, `toneDetectedCode` (+ alt), `toneGate`. Saved per radio. Bookmark tone fields and `tuneToBookmark` are the design's section 7 and are untouched. The DCS "chosen code" is `setSelected(code, polarity)`; the picker issue is under Owner decisions. Bit-identical audio with the tone squelch Off: nothing in these blocks writes the audio except the optional high-pass.

**D. S-meter calibration (feature 14; `smeter_cal.hpp`).** Pass thread only, display only, no DSP thread. Where `engine.cpp` fills `signalDb`/`sMeter` (~4774-4775): `const CalResult r = calTable.dbm(signalDb, frequencyHz, totalGainDb)`; `const SReading s = dbmToS(r.dbm, frequencyHz)`; append `FoxReceiverState` fields `meterUnits`, `signalDbm` (= `r.dbm`), `sUnits` (= `s.units`; `formatS` for text), `calibrated` (= `r.calibrated`, plus `r.exact` if the sheet wants it). The existing `sMeter` bar and `signalDb` stay on dBFS, and the squelch stays on dBFS. Calibration lives per DEVICE (key as the gains maps are keyed), not in `SavedRadio`: a `CalTable` per device key, persisted with `toText()`/`fromText()` (one string in the engine half of the settings, refused wholesale if any line is bad). `FOXAPP_OP_CAL_MEASURE`: average the dBFS over 3 s on the pass, call `offsetFromMeasurement(userDbm, avg, off)`, `table.set({lo, hi, off, totalGainDb, gainIsDb})` with the band the user chose (default two: [0,30e6) and [30e6, 6e9)); `CAL_CLEAR` -> `clearAt`. `totalGainDb` and `gainIsDb` come from `GainInfo` (device_source.hpp:96); the Airspy R2 and similar step gains pass `gainIsDb = false`. `SET_METER_UNITS` 0x0118 is the per-radio display choice. Amend the `plugin_abi.h` sMeter comment and MANUAL only when a plugin asks.

**E. Listening log / ADIF (feature 13; `adif.hpp`).** Worker thread only. The engine's `ListenLog` (design section 14) holds rows in whatever form it likes; for export convert to `adif::ListenEntry` (`startUnix` UTC seconds, `durationS`, `frequencyHz`, FoxSDR mode text, `call` from Notes/bookmark/decoder, `station` = bookmark name, `notes`, `peakDbfs`, `radio`) and call `writeAdiFile(path, entries, options, nowUnix, error, &stats)` from the export worker; show `stats.skippedNoCall` to the user ("12 rows have no station call and were not exported"). `Options`: `operatorCall` from a new setting (the listener's callsign/SWL id), `myGrid` from the locator the plugin ABI already has, `programVersion` = the app version string. CSV export and the row builder (merge window, minimum duration, split of long signals) are not in this lane. PRIVACY.md gains the design's sentence.

## UNVERIFIED (consolidated)

CTCSS tone table against TIA-603-E; DCS code list against TIA-603-E; the DCS Golay generator / bit order against the standard or any real signal (derived, two third-party checks); the "inverted" on-air definition; DCS data deviation and bit-rate tolerance; detect and
release times of both detectors on real repeater audio (these are this block's measured numbers, not a standard's); S9 = -73/-93 dBm against R.1 itself and the tie at exactly 30 MHz; OPERATOR versus STATION_CALLSIGN for a listener; acceptance of the `.adi` by a real
logger; thread-safety of `ReplayRing` beyond the stress run (no ThreadSanitizer on this build); everything about real recordings (none exist for CTCSS, DCS or squelch-keyed voice; nothing was downloaded).
