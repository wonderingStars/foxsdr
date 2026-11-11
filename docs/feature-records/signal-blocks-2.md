# Signal blocks, second batch (2026-11-11)

Branch `feat/signal-blocks-2` (local, cut from `feat/signal-blocks` 4a654a44, not pushed, not merged, no version bump). Five more standalone
blocks and their tests. **Nothing is wired**: no existing source file was touched (not even `tests/CMakeLists.txt`: the test glob picked the
new files up after a re-configure). Conventions, build and test commands, and the traps of the first batch are in `signal-blocks.md`; this
batch follows them (header-only, `cascade::dsp` / `cascade::core` / `cascade::source::iqfile`, tests with `test_check.hpp`, `AllocProbe` from
`tests/alloc_probe.hpp`, generators from `tests/signal_block_gen.hpp`).

| Block | Header | Test | Commits |
|---|---|---|---|
| 1 Synchronous AM | `src/dsp/sam.hpp` | `tests/test_sam.cpp` | see `git log` (three: block, crossfade check, lock-time pin) |
| 2 IF shift / passband | `src/dsp/passband.hpp` | `tests/test_passband.cpp` | one |
| 3 Noise-reduction methods | `src/dsp/nr_methods.hpp` | `tests/test_nr_methods.cpp` | one |
| 4 I/Q file readers | `src/source/iq_readers.hpp` | `tests/test_iq_readers.cpp` | one |
| 5 Power / SNR logging | `src/core/power_log.hpp` | `tests/test_power_log.cpp` | one plus two test follow-ups |

## What existed already (searched before each block)

| Block | Search result |
|---|---|
| 1 SAM | No synchronous or PLL AM anywhere. `DemodMode::AM` is an envelope detector with a 2 Hz DC blocker (`src/dsp/demod.cpp:271`, constant `kAmDcCutoffHz` above it); `DSB` is `Re{x}` with no carrier tracking. |
| 2 IF shift | Nothing. SSB has one fixed filter whose width is `Demodulator::setSsbBandwidthHz` (`src/dsp/demod.hpp:126`, 300 Hz low edge fixed by design in `demod.cpp`); the channel filter is symmetric. No shift, no independent edges, no CW or AM edge maths. |
| 3 Noise reduction | `dsp::NoiseReduction` (`src/dsp/noise_reduction.hpp:89`): STFT Wiener / spectral subtraction, 512-point, 50 % overlap, an asymmetric-decay tracker standing in for a true minimum, a cross-bin median cap, a gain floor, latency 512 samples. No adaptive line enhancer; no true minimum statistics. Left untouched and used as the comparison in the tests. |
| 4 I/Q readers | `source::IqFileSource` (`src/source/iq_file_source.hpp`): a looping 2-channel RIFF reader for PCM16 and float32 ONLY (it says so, `iq_file_source.cpp:194`; 8/24-bit, extensible, ADPCM refused), no RF64/ds64, no 24/32-bit PCM, no raw files, no SigMF, no seek, no centre frequency. `core/diag_log.cpp:738` only lists `sigmf` among upload-able extensions. |
| 5 Logging | Nothing: no CSV logger, no Influx code, no UDP code in `src` or `tests`. Measurements it can use exist (see "Noise floor and SNR from what the pipeline measures"). |

## Sources, and what is and is not verified

| Fact | Source read this session | Status |
|---|---|---|
| InfluxDB line protocol: syntax, escaping per context, `i` suffix, ns default, no empty tag values, no leading underscore, one point per line | https://docs.influxdata.com/influxdb/v2/reference/syntax/line-protocol/ (read 2026-11-11) | verified |
| InfluxDB 1.x UDP input splits received data by newline, one point per line, 64 KB reads, no error signal | https://docs.influxdata.com/influxdb/v1/supported_protocols/udp/ (read 2026-11-11) | verified |
| InfluxDB v2 `POST /api/v2/write?org&bucket&precision`, `Authorization: Token`, 204 | the API page fetched did not carry the endpoint text | **UNVERIFIED** (from memory; marked in the header) |
| RF64: `RF64` id, `ds64` FIRST chunk, 64-bit riffSize / dataSize / sampleCount, 0xFFFFFFFF means use ds64, JUNK reserved as ds64 | EBU Tech 3306 v1.0 PDF, https://tech.ebu.ch/docs/tech/tech3306v1_0.pdf (downloaded to a temp file and its text extracted by a script, 2026-11-11) | verified; the exact field order after the three 64-bit values (32-bit table length) read as "tableLength", chunk size >= 28 from the JUNK rule |
| BW64 uses the same layout with the id `BW64` | EBU Tech 3306 v2 note on Wikipedia "RF64"; ITU-R BS.2088 not read | **UNVERIFIED** |
| WAVEFORMATEXTENSIBLE layout, channel-mask bits, valid bits left-aligned | https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ksmedia/ns-ksmedia-waveformatextensible (read 2026-11-11) | verified |
| SubFormat GUIDs of PCM and IEEE float (`{00000001-0000-0010-8000-00AA00389B71}`, `{00000003-...}`) | the page above says every format tag has a SubFormat GUID; the base GUID tail seen in https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/subformat-guids-for-compressed-audio-formats; the two exact values from memory | **UNVERIFIED as printed values** (consistent with the pattern) |
| SigMF datatypes, `core:` fields, file extensions | SigMF spec v1.2.6 as served at https://sigmf.org (read 2026-11-11) | verified |
| Raw 8-bit unsigned = (u - 127.5) / 127.5 | a convention of common RTL-SDR tools, from memory | **UNVERIFIED** which tools |
| PLL gain formulas from loop noise bandwidth and damping; the cross-product FLL | Rice, "Digital Communications: A Discrete-Time Approach" (2009) ch. 7; Gardner, "Phaselock Techniques" 3rd ed. | **UNVERIFIED against the books** (from memory); the measured lock times are what is pinned |
| Kaiser window design relations (beta, N from attenuation and transition) | Oppenheim and Schafer, "Discrete-Time Signal Processing" 3rd ed., 7.5.3 | **UNVERIFIED against the book**; measured attenuation (80 dB design, 83 dB at the edge + half the transition) is what is pinned |
| Spectral subtraction and over-subtraction with a floor | Boll, IEEE Trans. ASSP 27(2), 1979; Berouti, Schwartz, Makhoul, ICASSP 1979 | **UNVERIFIED** (from memory) |
| Minimum statistics | Martin, IEEE Trans. SAP 9(5), 2001: the idea only; the bias factor is MEASURED (not Martin's formula) | **UNVERIFIED** against the paper; see the departure below |
| Adaptive line enhancer, NLMS | Widrow et al., Proc. IEEE 63(12), 1975; Zeidler et al., IEEE Trans. ASSP 26(3), 1978; Haykin, "Adaptive Filter Theory" | **UNVERIFIED** (titles and volumes from memory) |
| civil-from-days date algorithm | Hinnant, "chrono-Compatible Low-Level Date Algorithms" | from memory; verified in the test against `std::chrono`'s calendar for 20000 days |

No program source was read (clean room). No file was downloaded except the EBU PDF (to a temp file, text extracted, not kept) and web pages
read through the fetch tool; no sample data files were needed, so `D:/foxsdr-samples/blocks2/` was not created.

## Block 1: synchronous AM (`SyncAm`)

**What it is.** Second-order PLL at the channel rate (loop noise bandwidth 20 Hz default, damping 0.7071, wide 3x loop while searching), a
cross-product FLL assist while searching, product detector, selectable sideband (`Both`, `Upper`, `Lower`) by one complex band-pass (two real
dot products; Blackman-Harris, 641 taps at 48 kHz, transition about 600 Hz), lock indicator, and three states: Searching (envelope output), Locked
(coherent), Held (carrier gone: oscillator free-runs, output stays coherent, up to 2 s by default, then back to Searching and a 10 ms crossfade to
the envelope detector; if the carrier comes back, Held also goes to Searching so the loop re-pulls). Latency `(taps - 1) / 2` (320 samples at 48 kHz)
in every mode so every switch is a crossfade of aligned signals. Output level equals the envelope detector's for an unfaded carrier.

**Calls and threads.** Constructor allocates (set-up). `process(in, n, out)` DSP thread only; allocation-free (proved); non-finite input reads as 0.
`setSideband`, `setLoopBandwidthHz` (5..100), `setHoldSeconds` (0..10): atomics, any thread, picked up at the next `process`. `locked()`, `state()`,
`carrierPresence()`, `carrierOffsetHz()`: atomics, any thread.

**Proof (real runs, `test_sam`, 48 kHz, 1 kHz tone at 50 % depth, complex noise sigma 0.001).**
- Clean carrier: SAM tone 0.5001 against envelope 0.5001 (same level); THD+N 0.283 % for both (the noise floor); lock at 0.031 s.
- Offset acquisition, nine offsets from -200 to +200 Hz: lock at 0.031 s (0 Hz) to **0.106 s (+-200 Hz)**, residual offset error below 0.001 Hz,
  THD+N after lock 0.283 %. Speech-like modulation at +-200 Hz: lock at 0.104 s. Pinned: lock < 0.25 s. Without the FLL assist the same offsets
  take 0.480 s (measured in the break-it), so the pin does depend on it.
- Selective fade (carrier down for 1 s, sidebands untouched), measured late in the fade, SAM against the existing envelope mode on the same input:

  | Fade | SAM THD+N | Envelope THD+N | State mid-fade |
  |---|---|---|---|
  | 20 dB | 0.285 % | 163.8 % | Locked (carrier still 27 % of the channel rms) |
  | 26 dB | 0.285 % | 337.4 % | Held |
  | 30 dB | 0.285 % | 537.1 % | Held |

  Pinned: SAM below 2 % and at least **20 times** below the envelope detector; tone amplitude 0.5 +- 0.03; the state returns to Locked after the carrier
  comes back; audio peaks around the fade edges stay at or below 1.6 (measured 1.37 to 1.44 on a 0.5 tone, from the 2 Hz DC blocker's response
  to the 0.95 carrier step, not noise).
- Sideband selection, a 0.2 unmodulated interferer 1.5 kHz off on the unwanted side: Both leaves it at 0.2000, the single-sideband choice at 0.00114
  (**44.8 dB** upper, **45.0 dB** lower removed; pinned at 40 dB; the floor is the PLL's own phase wobble from the interferer); the wanted tone stays 0.500.
  An interferer 150 Hz from the carrier is only 21.7 dB down (the carrier sits on the filter's -6 dB edge: documented limit).
- Lock indicator: false for 3 s of noise alone (presence 0.035) and when the whole signal vanishes after the hold; true on a carrier; Held within the hold,
  Searching after it, Locked again after the carrier returns (states sampled at 1.5, 2.5, 4.5, 6.5, 9.5 s of a 10 s run). While only noise is on the channel
  for 3.9 s the audio peak is 0.044 (no burst).
- Also: bit-identical output for any chunking, non-finite input, 24 and 96 kHz (THD+N 0.28 %, lock), settings clamped, `reset()`, no allocation, no click
  switching sideband every 250 ms (largest step 0.0703 against the tone's own 0.0710), and no jump when an interferer is removed by the switch
  (0.1177 against 0.1202).

**Break-it** (patch, rebuild, named check RED, `git checkout`):

| Removed or broken | RED |
|---|---|
| Locked goes to Searching instead of Held | the five fade checks (THD, margin, amplitude, state, loss-test state at 2.5 s) |
| Sideband filter bypassed | both interferer-removal checks |
| FLL assist off | lock-time pins (lines 155 and 183: 0.48 s) |
| `locked()` forced true | noise-only `!everLocked` |
| Crossfade step set to 1 | the interferer-removal switch check |

**Departures / limits.** The envelope output during Searching is a design choice (graceful, no phase-spin noise) not asked for in those words. Presence uses the
channel rms, so it assumes a channel filtered to the AM width. A carrier that stays 20 dB below the sidebands for longer than the hold ends in the
envelope detector (distorted but not noisy). Not tested: real recordings, a carrier with real selective-fade phase jumps, interference stronger than the carrier.

## Block 2: IF shift and passband tuning (`passbandEdges`, `PassbandFilter`)

**Maths.** All edges are relative to the tuned frequency, the -6 dB points of the filter. Shift is audio-referred (positive moves toward higher audio:
more positive baseband for USB, more negative for LSB, so one slider means the same in both). USB `[lo+s, hi+s]`; LSB `[-(hi+s), -(lo+s)]`; CW audio
edges around the pitch, beaten down by it (450..950 around 700 Hz is baseband -250..+250); AM `[-lowExtent+s, +highExtent+s]` with the carrier held inside
(+-100 Hz margin, flagged `clamped`). Edges clamped to 0.45 x rate, reversed cuts swapped, 50 Hz minimum width, non-finite reads as default. Hand-computed table
tests (18 rows including the clamps and the USB/LSB mirror).

**Filter.** Linear-phase complex FIR, Kaiser window, 80 dB design, 300 Hz transition by default (805 taps at 48 kHz), centre gain exactly 1, one tap
count for every design (so the same delay `(N-1)/2`). **Glitch-free changes:** the taps are ramped to the new design (one-pole, time constant 8 ms, per 16-sample
block); because the output is linear in the taps this equals crossfading the old and new filter at the cost of one filter, and a retarget in mid-ramp just
bends the ramp. **Off the audio thread:** `setPassband()` designs on the control thread into a preallocated target published through a sequence counter; the DSP
thread only copies and ramps. Measured: a design takes **18 us at 48 kHz (805 taps), 5 us at 12 kHz, 46 us at 192 kHz (2047 taps, capped, transition 471 Hz)**; the
DSP cost is **7 % of one core at 48 kHz** (74 ms per second of signal, one run; 51 ms in another), 0.4 % at 12 kHz, 72 % at 192 kHz (so the wiring should run it
at a decimated channel rate). Both calls allocation-free (proved).

**Threads.** Constructor allocates. `setPassband` control thread, one caller at a time. `process` DSP thread only (in place allowed, non-finite reads as 0).

**Proof (real runs, `test_passband`).**
- Gain 0.00 dB in the flat part and **-83.8 dB** (USB, LSB at 100 Hz outside the 300 Hz edge), -88.3 dB (CW, 60 Hz transition), -94.6 dB (AM) in the stop band; the opposite side of USB at -102.8 dB.
- A tone inside before a shift and outside after: **-83.7 dB** (USB +500 Hz and LSB +500 Hz), -90.1 dB (USB -500 Hz), -99.5 dB (CW +200 Hz), -92.1 dB (AM +1000 Hz), measured after
  the ramp has settled (the 8 ms ramp needs about 9.2 time constants, 74 ms, to reach 80 dB: the first measurement at 4 x taps found -62.7 dB and the wait was corrected, not the pin).
  A second tone that stays inside (the AM carrier, the other tone) stays at 0.00 dB.
- Two-tone test per edge, low cut and high cut moved independently for USB, LSB and AM (extent below and above): the moved edge's tone drops to -82.7 .. -107.6 dB, the other tone stays at 0.00 dB (all six rows).
- Clicks: a sweep of the shift from -200 to +800 Hz in 5 ms steps with a steady 1.5 kHz tone inside the band leaves the output within **3.2e-5** of the delayed tone (-83.9 dB re the 0.5 tone; pinned 2e-4) and the
  largest sample step 0.0980 against the tone's own 0.0982. A jump that drives the tone from mid-band into the stop band: largest step **0.0981 with the ramp (ratio 1.00), 0.5000 without it (ratio 5.09)**; pinned 1.05 x the tone's own step.
- Block-split invariance, in-place and non-finite input, the tap cap reporting its wider transition, same delay for every design.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Ramp off (taps switched at once) | the jump check (step 0.5 against 0.098) |
| Kaiser window replaced by rectangular | stop-band and passband-ripple checks |
| Modulation sign flipped | every gain, shift and edge check |
| LSB shift sign flipped | edge table row, mirror check, LSB shift row |
| AM carrier clamp removed | edge table row (`-100 .. 9500`) |

**Limits.** The 300 Hz default transition and 80 dB are measured design points, not the right answer for every mode: CW wants a narrower transition at a lower rate (the CW rows use 60 Hz at 12 kHz). Not tested in the running pipeline.

## Block 3: noise-reduction methods (`NoiseReducer`, `MinStatSubtractor`, `LineEnhancer`)

**Interface.** `NoiseReducer`: `setStrength(0..1)` (atomic), `process(in, n, out)` (n in, n out, in place allowed), `latencySamples()`, `reset()`, `name()`.
Strength 0 is a bit-exact bypass for both (proved; delayed by the latency for the subtractor).

**(a) `MinStatSubtractor`.** 512-point STFT, 50 % overlap, sqrt-Hann (COLA), per-bin periodogram smoothed (0.85), **minimum over 1.28 s** (six sub-windows of 40 frames), times a bias factor,
capped at 10 x the cross-bin median (a steady tone never pauses and would otherwise be taken for noise), gain from a faster-smoothed power: over-subtraction `1 + 3 s`, floor `10^-s` (-20 dB at 1),
gain smoothed in time. **Latency 512 samples (10.7 ms), measured: 512.** 

**(b) `LineEnhancer`.** NLMS predictor, 128 taps, delay 8 samples, mu 0.01, output `(1 - s) x + s y`. **Latency 0, measured: 0.** Frequency resolution about 375 Hz.

**Threads and allocation.** Constructors allocate; `process` and `reset` DSP thread only; `setStrength` any thread. No allocation in `process`: **proved by counting `operator new` (0 in 20 calls with strength changes)** for both.

**Proof (real runs, `test_nr_methods`; SNR measured against the known clean signal, delayed by the latency, so signal distortion counts).**

| Method | Signal | Input SNR | Output SNR | Improvement |
|---|---|---|---|---|
| Subtractor | 800 Hz tone in white noise | 0.0 dB | +13.6 dB | **+13.6 dB** (pinned > 12) |
| Subtractor | speech-like in white noise | +4.8 dB | +9.9 dB | **+5.1 dB** (pinned > 4) |
| Enhancer | 800 Hz tone in white noise | 0.0 dB | +16.3 dB | **+16.3 dB** (pinned > 14.5) |
| Enhancer | speech-like in white noise | +4.8 dB | +1.1 dB | **-3.7 dB** (pinned > -5; **it makes speech worse**: it keeps only the predictable part; use it for CW and carriers) |

- Strength: tone-in-noise output SNR 0.00 / 4.73 / 9.03 / 13.46 dB at strength 0 / 0.25 / 0.5 / 1 (subtractor), 0.00 / 2.49 / 5.92 / 16.32 dB (enhancer): monotonic.
- Distortion on a clean signal at strength 1 (output against input): subtractor 112 dB on a tone (nothing), 43.7 dB on speech-like (**0.65 %**); enhancer 132 dB on a tone, **1.1 dB on speech-like (88 % distortion)**: at full strength it rewrites speech.
- Noise estimate: **0.975 of the true noise power (-0.11 dB)** on 30 s of white Gaussian noise (pinned within 1 dB); the bias factor 2.2 was set from this measurement.
- Floor and musical noise: noise-only input at strength 1 is reduced by **19.6 dB** (floor -20 dB: not muted); the coefficient of variation of the output's bin powers is **1.62** (Gaussian noise 1.00) against **1.67 for the existing NoiseReduction on the same noise** (pinned below 1.75 and within 0.1 of the existing one).
- Chunk invariance, in-place equals out-of-place, non-finite input, `reset()`, clamped strength.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Mean instead of minimum for the noise estimate | tone and speech improvement pins, strength monotonic, clean-speech distortion, the bias check (5 checks) |
| Gain floor removed | level reduction beyond -21 dB, musical-noise CV, monotonic strength |
| Median cap removed | tone improvement, clean-tone distortion (the tone is eaten), clean-speech distortion |
| Bypass never taken | bit-exact check |
| NLMS weights never updated | tone improvement, clean-tone check, strength |
| Enhancer returns the predictor only | bit-exact bypass, measured latency |

**Departures / limits.** The minimum-statistics bias is measured, not Martin's formula (UNVERIFIED for non-Gaussian or strongly coloured noise); the smoothing is fixed, not optimal. The subtractor cannot tell a steady tone from noise except through the median cap (a strong steady carrier among weak noise loses a little: 2 dB at 20 dB SNR in the cap's own arithmetic, not measured separately). Neither method was run on real receiver audio.

## Block 4: I/Q file readers (`iqfile::`)

**Interface** (`IqFileReader`): `info()` (container, format, sample rate and whether known, centre frequency and whether known, total complex samples, trailing bytes, truncated, guessed-from-name, channel mask, valid bits, SigMF captures and annotations as lists),
`read(dst, n)` (complex floats, 0 at the end, no looping, non-finite floats read as 0), `seek(sample)` (the end is legal, past it is refused), `position()`. Open with `openRaw`, `openWav`, `openSigmf`, `openAuto` (by extension); an error text comes back for every refusal.

**Supported:** raw `.cu8`, `.cs16` (little-endian), `.cf32` (little-endian); WAV PCM 16/24/32 and IEEE float 32, two channels, plain or extensible (valid bits, channel mask, SubFormat GUID read; the GUID must be PCM or IEEE float, otherwise refused naming it), RIFF, RF64 and BW64, sizes 0 and 0xFFFFFFFF in plain RIFF read to the end of the file, ds64 used in RF64;
SigMF `cf32_le/be`, `ci16_le/be`, `cu16_le/be`, `ci8`, `cu8`. **Refused with a message:** 8-bit PCM, 64-bit float, ADPCM, mono, more than two channels, rate 0, bad block align, real SigMF types (`r*`), `cf64`, `ci32`, `cu32`, unknown types, `core:header_bytes`-style non-conforming datasets (not supported, not detected).
Raw files take rate and centre as parameters; `guessFromFileName` offers a guess (unit-suffixed tokens `145.5MHz`, `2.4Msps`; SDR#-style `100000000Hz`; two bare integers after a date and a time) that is only used when `useNameGuess` is set and then flagged `guessedFromName`.
SigMF: centre frequency from the first capture segment, all segments kept, annotations kept in order (a malformed annotation is skipped, a capture without `core:sample_start` is an error); a missing `core:sample_rate` leaves the rate unknown (or takes the fallback parameter), a missing `core:datatype` is an error.

**Threads and allocation.** One thread per reader. `open*` allocates (128 KiB read buffer at most, SigMF metadata at most 8 MiB, lists at most 65536 entries, header walk at most 256 chunks); `read` and `seek` do not. Not wired to any thread.

**Proof (real runs, `test_iq_readers`: 4704 checks; every file written byte by byte in the test).**
- Known values and endianness: cu8 `(0,255)` = (-1,+1); cs16 `00 80 / FF 7F`; cf32 `00 00 80 3F` = 1.0 and `00 00 20 C0` = -2.5 and a NaN reading 0; PCM24 `00 00 80` = -1.0 with sign extension; PCM32 `00 00 00 80` = -1.0; extensible with 20 valid bits in a 32-bit container; SigMF `ci16_be`, `cf32_be`, `cu16_le`, `ci8`.
- Truncated and odd-length files (trailing partial frame counted and ignored; data size larger than the file clamped and flagged); empty file; an odd-sized chunk before `data`; a file that shrinks under the reader (short read, flagged).
- Wrong GUID (`...9B70` refused naming the GUID), `cbSize` 0, mono, 8-bit, float64, ADPCM tag, rate 0, bad block align, `ds64` shorter than its fields, 300 zero-size chunks (bounded walk).
- **Files over 4 GB:** sparse files of 1.2e9 PCM16 frames (4.8e9 bytes): RF64 with ds64, and plain RIFF with the data size field 0xFFFFFFFF and 0; total samples exact, seek to the last frame reads the known bytes `34 12 CC ED`.
- Seek: 300 random seeks and reads across the 16384-frame decode chunk against a sequential read of 40000 frames; end position legal, past it refused and the position unchanged.
- **Header fuzz:** 6000 mutated headers (WAV, extensible, RF64, SigMF metadata): 1420 opened, 4580 refused, none crashed, **largest single allocation 131111 bytes** (the 128 KiB read buffer; pinned below 512 KiB). Run time of the whole test about 54 s (the sparse files and the fuzz writes).

**Break-it:**

| Removed or broken | RED |
|---|---|
| PCM24 sign extension removed | the PCM24 known-value row |
| ds64 value ignored | RF64, BW64 and both over-4 GB RF64 checks (7) |
| "size 0 / 0xFFFFFFFF reads to the end" removed | streamed RIFF rows and both over-4 GB RIFF checks |
| SubFormat GUID unchecked | wrong-GUID and MP3-GUID refusals |
| SigMF centre taken from the last capture | centre-from-first-capture check |
| Big-endian flag ignored | `ci16_be` and `cf32_be` rows |
| File offset truncated to 32 bits | all three over-4 GB read checks |
| Header-walk bound removed | the 300-chunk check |

**Limits.** No real recording of any format was read (none downloaded). The 127.5 centre for cu8 is a convention (UNVERIFIED which tools). WAV files from other recorders that put the centre frequency in a private chunk (`auxi`) are not parsed. Linux: the code is portable but this batch was built and run on Windows only.

## Block 5: power and SNR logging (`powerlog::`)

**CSV logger.** `Record{unixNs, radio, frequencyHz, bandwidthHz, signalDbfs, noiseDbfs, snrDb, hasDbm, signalDbm}`. Columns, never reordered, new ones appended at the end:
`time_utc,radio,freq_hz,bandwidth_hz,signal_dbfs,noise_dbfs,snr_db,signal_dbm` (ISO 8601 UTC with milliseconds and `Z`; `.1`/`.2` decimals, locale-independent; an unavailable value is an empty cell).
`CsvLogger::offer()` is the only producer call: a per-radio interval test then a bounded single-producer ring (default 4096 records, drops counted); **wait-free, allocation-free (proved: 0 `operator new` in 20000 calls; 6 ns per call measured)**.
One worker thread does all file I/O (polls every 10 ms; `stop()` drains what was accepted: 1000 of 1000 written). Rotation by **UTC day** (`base-YYYYMMDD.csv`) and by **size** (`-001`, `-002`, a file never exceeds the limit; a restart appends without a second header and skips full parts).
**Disk refusal:** open or write failure -> counted (`writeFailures`), the record dropped and counted (`droppedDiskFailing`), `diskHealthy()` false, `lastError()` set, no retry sooner than `retryMs` (so 20 records produced fewer than 20 attempts), and on return the header is written into the new file; nothing throws and the queue never grows past its bound.
**Threads:** `offer()` from one producing thread (the engine pass handles all radios), never the DSP thread; everything else control thread; the worker owns the file.

**Influx line protocol (`influxLine`).** Pure, per the reference: measurement `foxsdr_signal`, tags `radio` plus the options' tags, fields `freq_hz` and `bw_hz` as integers (`i`), `signal_dbfs`, `noise_dbfs`, `snr_db`, `signal_dbm` as floats (shortest round-trip, exponents allowed), optional string fields, integer nanosecond time stamp (exact: 1234567890123456789 survives). Escaping: measurement `, space`; tag keys and values and field keys `, = space`; string values `" \`; **backslash is doubled everywhere** so a value ending in a backslash cannot swallow the next delimiter; newlines become `_` (names) or space (string values); empty tag values dropped; leading underscores stripped; a non-finite field is omitted; no field at all gives no line.
**Verified against an independent parser** written in the test from the reference: 4000 random tag values, keys and string fields over the alphabet `a b space , = " \ _ newline Z 9`: every line parses to the original after the documented normalisation. That parser **caught a real bug** in the first version of the formatter (a string-field key was escaped twice); fixed and a golden row added.

**Senders (`InfluxTarget`, `sendUdp`, `sendHttp`).** **Off by default** (`enabled = false`: returns "disabled" without touching the network, proved with live stand-in listeners that received nothing). BLOCKING, worker thread only. UDP: lines grouped into datagrams of at most 1400 bytes in order (200 lines in 19 datagrams, none lost, order kept). HTTP: plain HTTP POST `path?precision=ns&org=&bucket=` (percent-encoded), `Authorization: Token <t>` when a token is supplied, `Content-Type: text/plain`, `Connection: close`; status read from the answer, 2xx is ok. No TLS (put Telegraf or a TLS proxy in front on an untrusted network). Control characters in host, token, path, org or bucket are refused before connecting (proved against a LIVE listener that received nothing).
**Tested only against stand-in listeners inside the test**: 204 ok; 401 not ok with `HTTP 401`; connection closed silently: `no HTTP answer`; a stand-in that never answers: returns after **0.40 s** with a 400 ms timeout; refused port: error; the token never appears in an error string.

**Credentials: what the settings already do.** `core/config.hpp:1414` `std::string testerToken` is stored in the settings file in plain text (a 32-hex tester credential); the web-API password is stored as a PBKDF2 record (`config.hpp` near 1290). This block stores nothing: the token is a per-call argument. If the engine lane persists an Influx token it will be the second plain-text secret in `config.json`; **owner decision** (see below).

**Break-it:**

| Removed or broken | RED |
|---|---|
| Comma not escaped in tag keys | the parser fuzz |
| Integer suffix dropped | golden lines (4 checks) |
| Millisecond field truncated | time, CSV and logger checks (6) |
| Day rotation off | rotation file set, real-file check (4) |
| Size rotation off | rotation file set (2) |
| Header write off | nine header, rotation, restart and real-file checks |
| Queue bound off | accepted / dropped count, drain count |
| Retry back-off off | `writeFailures() < 20` |
| Disabled gate removed | disabled, no-traffic checks (3) |
| SNR computed without subtracting the noise | both SNR values |
| Control-character refusal removed | the two refusals and the live-listener check |

(The first run of the day-rotation break-it crashed the test instead of failing a check: an unguarded `[0]` on a missing file. The check is now bounds-checked.)

### Noise floor and SNR from what the pipeline already measures (read, not changed)

- **Signal level:** `Pipeline::signalPowerDb()` (`src/core/pipeline.hpp:433`), set at `pipeline.cpp:2018` from `meter_.powerDb()`: the channel power after the VFO filter and decimation and before the demodulator, dBFS, a one-pole average of `|x|^2` (alpha 0.05 per channel sample), -200 dB when there is no data. It INCLUDES the noise in the channel.
- **Noise floor:** `Receiver::noiseFloor_` (`src/engine/receiver.hpp:363`), a `core::NoiseFloorTracker` (`src/core/noise_floor_tracker.hpp:75`) that `engine_squelch_auto.cpp:120` feeds with `signalPowerDb()` and the squelch gate state: **the minimum of the last ten seconds of 100 ms block means**, readings taken while the gate is open are not fed once an estimate exists (a carrier never teaches it; valve after 30 s), reset on a retune, bandwidth or mode change; `floorDb()` is NaN until half a second is held. NB: it runs only where AUTOMATIC squelch is running (`engine_squelch_auto.cpp`); the wiring must feed it for logging when AUTOMATIC is off (the call is cheap).
- **Recommended SNR:** because the signal reading includes the noise, `snr = 10 log10((10^(S/10) - 10^(N/10)) / 10^(N/10))` with `S = signalPowerDb()` and `N = noiseFloor_.floorDb()`: `powerlog::snrFromTotalAndFloor`. It returns NaN when `S` is under 0.1 dB above `N` (nothing to speak of, and the subtraction is meaningless) or either is not a measurement; the logger writes an empty cell. Log `S` and `N` as well as the SNR.
- **Caveat:** the floor is a minimum of block means, which sits below the mean noise power by a small bias (the minimum of ten seconds of 100 ms averages; not measured here), so the SNR reads slightly high on a pure-noise channel. For a log meant to be compared across days that is a constant offset, not a drift; do not call it calibrated.
- **dBm:** from `core::smeter::CalTable::dbm()` (first batch) where a calibration exists, else leave `hasDbm` false.

## Tests run (this machine, Release, 4 jobs)

Build: the same configure as the first batch (`D:/foxsdr-build/feat-blocks`, one re-configure for the new test files), `cmake --build ... --config Release --parallel 4 --target <tests>`; only the engine library and the test executables were built. The window was not built and the application was not run; no radio was opened.

Own checks: sam 101, passband 109, nr_methods 39, iq_readers 4704, power_log 88; all 0 failed. Final combined run (the first batch's sixteen plus these five):

`ctest -C Release -j4 -R "^(test_agc_modes|test_noise_blanker|test_squelch|test_recorder|test_demod_agc|test_record_start|test_record_finish_async|test_stop_ends_recordings|test_pipeline_agc|test_patch_squelch|test_ctcss|test_dcs|test_replay_ring|test_clip_recorder|test_smeter_cal|test_adif|test_sam|test_passband|test_nr_methods|test_iq_readers|test_power_log)$"`
-> **`100% tests passed out of 21`** (second run). The FIRST run of the same command reported `95% tests passed, 1 tests failed out of 21`: `test_record_finish_async` (a first-batch,
timing-sensitive test, 41.9 s under the four-way load of the longest tests) failed; it passed alone (`100% tests passed out of 1`, 30.5 s) and in the second full run. The log of the failed run was overwritten by the
second run, so the failing check was not captured: **recorded as a load-dependent flake of an existing test, cause not examined.** All five new tests passed in both runs.

Run-to-run: `test_power_log` was run nine times in a row green (timing-dependent worker thread and stand-in listeners). The others are deterministic (seeded) except `test_iq_readers` (disk speed) and were run once green after the last change plus the break-it runs, not 50x.

## Departures from the request, and why

1. **SAM** outputs the envelope detector while Searching (graceful fallback), and the PLL gain formulas and FLL are from memory of the books (UNVERIFIED); the lock times are measured.
2. **Passband** uses a tap ramp (equivalent to a crossfade at one filter's cost); 80 dB / 300 Hz are defaults, not requirements.
3. **Noise reduction:** the minimum-statistics bias factor is measured, not Martin's; the existing `NoiseReduction` is not modified and is used as a comparison only.
4. **I/Q readers** are header-only like the rest of the batch; the existing `IqFileSource` is not changed (the layer is for it to call). BW64 is read like RF64 without having read BS.2088.
5. **Logger** has no TLS and no token storage; HTTP is plain to the host the user names.
6. No existing file was edited, including `tests/CMakeLists.txt` (not needed this time).

## Owner decisions needed

- **Where an Influx token lives.** It would be a second plain-text secret next to `testerToken`. Options: environment variable only, the same settings file, or no token support (Telegraf in front). The block takes it per call, so any choice works.
- **ALE strength on speech.** The line enhancer makes speech worse at any strength above about 0.5; expose it only in CW / carrier contexts, or cap its strength there?
- **Sideband default for SAM** (Both, or Upper/Lower from the mode) and whether the lock lamp shows "Held" separately.
- **Logging interval default** (1 s per radio is a guess) and whether a rotated CSV set needs a retention limit (nothing deletes old files).

## UNVERIFIED list

- InfluxDB v2 HTTP write endpoint details (query names, header, 204); plain HTTP only, no TLS.
- BW64 identical to RF64; PCM and float SubFormat GUID values as printed; the 127.5 centre of raw cu8.
- Every textbook formula named above (Rice, Gardner, Kaiser relations, Boll, Berouti, Martin, Widrow, Zeidler): the numbers pinned are measured, the formulas are not checked against the books.
- Minimum-statistics bias on non-Gaussian or coloured noise; ALE behaviour on real CW; SAM on real fading AM broadcasts; every reader on real recordings; the HTTP/UDP senders against a real InfluxDB or Telegraf; POSIX socket paths (not compiled here).
- ThreadSanitizer was not run (MSVC build); the ring buffer and the seqlock are covered by their tests only.

## WIRING HAND-OFF (what the engine, settings and screen lanes must do; nothing below has been done)

### 1. SAM (`dsp/sam.hpp`)
- **Engine:** a new `DemodMode::SAM` (enum and `kDemodModeCount` 8 -> 9, name table, `modeFromName`, config list) is the simplest wiring, or a flag on AM. In the demod stage of `Pipeline::processBlock` (`pipeline.cpp` near the AM/other demod call around line 2018's neighbourhood) hold a `std::unique_ptr<dsp::SyncAm>` built at the channel rate in set-up; call `process(chan, m, audio)` where `Demodulator::process` is called for AM. Note the extra latency (320 samples at 48 kHz) in recordings and the audio chain. Rebuild on a channel-rate change (constructor allocates).
- **Ops / state (suggested):** `SET_SAM_SIDEBAND` (0 both, 1 upper, 2 lower), `SET_SAM_LOOP_BW` (Hz); read-back `samState` (0 searching, 1 locked, 2 held), `samLocked`, `samOffsetHz`. Settings: `samSideband` (int), `samLoopBandwidthHz`.
- **Screen strings:** "SAM", "Sideband: Both / Upper / Lower", "Loop bandwidth", lock lamp "SYNC", "Carrier offset" (Hz), "Carrier lost - holding".
- **Existing code to leave alone:** the envelope AM path stays the default.

### 2. Passband (`dsp/passband.hpp`)
- **Engine:** replace or follow the channel filter (`dsp::Vfo` low-pass) with a `PassbandFilter` per receiver, run at a decimated rate (the 192 kHz row costs 72 % of a core), fed by a `PassbandSpec` built from mode, low cut, high cut, shift (and CW pitch). `setPassband(spec)` from the control thread on every slider change (about 20 us, allocation-free); `immediate = true` only at start and on a mode change. SSB then needs the `Demodulator`'s own sideband filter to stop duplicating it (a hand-off item for whoever owns `demod.cpp`: today it hard-wires 300 Hz and a width).
- **Ops / state:** `SET_IF_SHIFT` (Hz, +-), `SET_LOW_CUT`, `SET_HIGH_CUT` per radio; settings `ifShiftHz`, `lowCutHz`, `highCutHz`; reply the resulting `edges()` (lo, hi, clamped) so the screen can draw the passband on the spectrum.
- **Screen strings:** "IF shift", "Low cut", "High cut", "Passband", "Reset passband"; the spectrum's passband overlay should use `edges()`.
- Tuned-frequency display must not change with shift (the maths keeps the tuned frequency fixed by construction).

### 3. Noise-reduction methods (`dsp/nr_methods.hpp`)
- **Engine:** a per-radio `std::unique_ptr<dsp::NoiseReducer>` chosen by a method index (0 existing `NoiseReduction`, 1 `MinStatSubtractor`, 2 `LineEnhancer`) in the audio chain where `NoiseReduction` is called today (48 kHz mono, after demod and before the AGC/limiter as the existing one). Switching method = construct the new one off-thread and swap at a block boundary (the constructor allocates); use the recorders' pointer hand-off. Keep the 512-sample latency in mind for the subtractor when it is bypassed (the existing one reports the same).
- **Ops / state:** `SET_NR_METHOD`, `SET_NR_STRENGTH` (0..1); settings `nrMethod`, `nrStrength`.
- **Screen strings:** "Noise reduction", "Method: Standard / Spectral (min. statistics) / Line enhancer (CW)", "Strength"; a hint under Line enhancer: "for CW and carriers; not for voice".

### 4. I/Q readers (`source/iq_readers.hpp`)
- **Engine / source:** `IqFileSource::open()` (`iq_file_source.cpp`) can delegate to `iqfile::openAuto` and keep its looping and pacing: it owns the loop (`seek(0)` at the end), the reader owns formats. `IqInfo` supplies rate, centre (SigMF first segment), total samples (a position bar), and `annotations` (bookmarks on the timeline). For raw files the file dialog must ask for rate (and centre) and may pre-fill from `guessFromFileName` showing "guessed from the file name". The read must stay off the GUI thread when a file is on a network share (an `open` reads a few KB, `read` one 128 KiB chunk at a time).
- **Ops / state:** `OPEN_IQ_FILE` already exists for WAV; extend with `rate` and `centerHz` and `format` for raw; state adds `iqFileFormat`, `iqFileTotalSamples`, `iqFilePosition` and a seek op `SEEK_IQ_FILE` (sample index).
- **Settings:** the last raw parameters per file path.
- **Screen strings:** "Sample rate (Hz)", "Centre frequency (Hz)", "Format: cu8 / cs16 / cf32", "guessed from the file name", and the reader's own error texts verbatim ("raw I/Q files carry no sample rate", "unsupported WAVE_FORMAT_EXTENSIBLE SubFormat {...}", ...); "truncated: the file is shorter than its header says".

### 5. Power / SNR logging (`core/power_log.hpp`)
- **Engine:** one `CsvLogger` for the application (or per radio), `start()` on enabling, `stop()` on disabling or shutdown. On the PASS thread, once per pass per radio next to the S-meter fill (`engine.cpp` near the `signalDb` / `sMeter` fill, the same place as hand-off D of the first batch): build a `Record` (UTC `unixNs` from the system clock, radio index, `absoluteHz`, `vfoBandwidthHz`, `S = signalPowerDb()`, `N = noiseFloor_.floorDb()`, `snr = snrFromTotalAndFloor(S, N)`, dBm from the calibration table if present) and call `offer()`. Feed `noiseFloor_` even when AUTOMATIC squelch is off. A second worker (or the same one, extended) turns accepted records into `influxLine` and calls `sendUdp` / `sendHttp` in batches (every second, at most a few hundred lines); never from the pass thread; skip silently when the target is disabled; log (rate-limited) when a send fails.
- **Ops / state:** `SET_POWER_LOG` (enable, interval s, directory, rotate MB), `SET_INFLUX_TARGET` (enable, protocol, host, port, org, bucket); read-back `powerLogWritten`, `powerLogDropped`, `powerLogDiskHealthy`, `powerLogLastError`, `influxLastError`. Settings: `powerLogEnabled` (default off), `powerLogDir`, `powerLogIntervalS` (1), `powerLogRotateBytes`, `influx.*` (default off; the token per the owner decision above).
- **Screen strings:** "Log power and SNR to CSV", "Interval (s)", "Folder", "Start a new file every ... MB", "Disk problem: the log is paused (N records dropped)", "Send to InfluxDB (UDP / HTTP)", "Host", "Port", "Organisation", "Bucket", "Token", "No TLS: use only on a trusted network".
- **Default folder:** under the same per-user folder as the other logs, never the install folder; the logger creates the directory itself.
