# Lane `sigmeas` - host-side signal-measurement blocks (branch `feat/sigmeas`)

Engine lane, new-file model. Thirteen headers `src/dsp/sigmeas_*.hpp` (header only: no change to `engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`, the core recorder or any `.slint`), thirteen test files, one real-recording runner. The only existing file touched is `tests/CMakeLists.txt` (four lines appended at its end: the runner's target). Nothing pushed, nothing merged.

## 0. Step 0 table (reading only, written before any code)

Read first: the brief, `STANDARD-RULES.md`, `REMAINING-GAPS.md` "Signal analysis and identification", `DEPTH-GAPS.md` Part B (lines 472-483), the sibling briefs `sigid` and `bitview`, and the Demod Analyzer 0.3.0 source in `D:/foxsdr-wt/pd-winlink-wb/src/demodan/measure.h` (MIT) with that lane's record `docs/feature-records/depth-winlink-wb.md`.

What the plugin already has: `demodan` 0.3.0 (`MEASURE, BIT VIEW, EXPORT SIGNAL`) measures occupied and x-dB bandwidth, a burst list, AM depth, FM deviation, a chirp slope, FSK shift and baud, an OOK pulse-width slicer, autocorrelation, sideband lines and amplitude/frequency/phase against time, as pure functions inside the plugin. The plugin lane marked "cursor", "plot" and "harmonic/sideband cursor" as the host's. A plugin is a separate DLL and the plugin ABI has no call that runs a measurement for the host, so the host (a panel, the web page, the analyser project of `recplay`) cannot use the plugin's code: every row below is therefore a **new block**, written to the same *definitions* (occupied bandwidth 99 % above the floor, x-dB at 3/6/20/26 dB, deviation and depth as meters read them), not copied code. Nothing in the host measured any of these before (`grep` of `src` and `tests` for SINAD, OBW, burst, baud, deviation found only the demodulator and the airband strip).

| Row | Source list | Verdict | Block | Note |
|---|---|---|---|---|
| Cursor symbol period and rate, symbol extraction to a file | REMAINING | new block | `sigmeas_symbols.hpp` | cursor model, run and edge-grid fit, `extractSymbols`, `writeSymbolFile`; the cursors themselves are screen work |
| Amplitude, frequency, phase versus time | REMAINING | new block | `sigmeas_vstime.hpp` | numbers; the plot is screen work |
| FSK analysis, ASK/OOK pulse-width slicer | REMAINING | new block | `sigmeas_symbols.hpp` | the plugin has both; host cannot call them |
| Burst detector and list | REMAINING | new block | `sigmeas_bursts.hpp` | |
| Frequency-hopping and chirp tracking | REMAINING | new block | `sigmeas_hopchirp.hpp` | the plugin has a chirp slope only; hopping exists nowhere |
| Occupied and x-dB bandwidth | REMAINING | new block | `sigmeas_bandwidth.hpp` | |
| FM deviation and AM depth | REMAINING | new block | `sigmeas_modulation.hpp` | |
| Harmonic and sideband cursors | REMAINING | new block | `sigmeas_cursors.hpp` | the plugin's "sideband lines" measure lines; these are cursor marks with level read-out |
| Eight-cursor tuning tool | DEPTH B | new block (model) | `sigmeas_cursors.hpp` `CursorSet8` | drawing is screen lane K |
| SINAD, THD, SFDR, ENOB | DEPTH B | new block | `sigmeas_audio.hpp` | |
| Drift integration (de-chirp) | DEPTH B | new block | `sigmeas_audio.hpp` | |
| Varispeed playback | DEPTH B | new block | `sigmeas_varispeed.hpp` | |
| Amplitude and probability histograms | DEPTH B | new block | `sigmeas_histogram.hpp` | plus the power CCDF |
| MFSK per-character confidence | DEPTH B | new block (model) | `sigmeas_framelog.hpp` | plugin side is a hand-off (section 6) |
| Frames with CRC errors | DEPTH B | new block (model) | `sigmeas_framelog.hpp` | plugin and screen side are hand-offs |

No row is `already built` in the host and none is `not mine`. Lines of the same REMAINING section outside the brief (bit view, field inference, SigMF annotations, waterfall pause, identification library, numbers list) belong to other lanes (`bitview`, `recplay`, `sigid`) and were not touched.

## 1. What was built

Common conventions (`sigmeas_common.hpp`): complex samples are `std::complex<float>`, sums in double; frequencies are Hz from the centre of the span; every result has `valid` and a one-phrase `reason` (`empty span`, `bad sample rate`, `non-finite input`, `span too short`, `no signal found`, `bad parameter`, `no on/off structure`); no block throws, asserts or prints; nothing is allocated inside a per-sample loop; a sample-rate error is not corrected (a true rate (1+e) times the given one scales frequencies by (1+e) and periods by 1/(1+e): the tests pin that). Own radix-2 FFT, Welch spectrum, percentile, Otsu threshold.

| Header | Functions | Definition and source |
|---|---|---|
| `sigmeas_bandwidth.hpp` | `measureBandwidth` | Occupied bandwidth: ITU Radio Regulations No. 1.153 as quoted in ITU-R SM.328-12 (opened): width outside which each tail holds beta/2 = 0.5 % of the total mean power; the total here is the power above the measured noise floor in the signal region. x-dB bandwidth: SM.328-12 section 1.8 (opened), reference = maximum of the power spectral density ("x dBsd"), outermost crossings. Centre = power centroid. Optional selected sub-band and explicit floor. |
| `sigmeas_modulation.hpp` | `measureFmDeviation`, `measureAmDepth` | Deviation from the phase step of the channel, depth (Amax-Amin)/(Amax+Amin) of the envelope; a noise-limited percentile peak and a noise-immune least-squares tone fit (headline when one line holds 90 % of the variance). No standard number is cited (none was opened). |
| `sigmeas_symbols.hpp` | `periodFromCursors`, `refineCursorPeriod`, `fitSymbolPeriod`, `fitEdgeGrid`, `extractSymbols`, `writeSymbolFile`, `analyseFsk`, `sliceOok` | Run-length fit of the bit period, then a least-squares fit of the edge times to a symbol grid (this is what makes the baud accurate); FSK mark/space/shift/baud/jitter/bits; OOK levels, pulses, pulse-width keying, repeat period, bits. Otsu 1979 recalled, not opened. |
| `sigmeas_vstime.hpp` | `computeVsTime` | Shift to DC, second-order CIC, amplitude (linear, dBFS), unwrapped phase, frequency, optional linear-phase removal. |
| `sigmeas_bursts.hpp` | `detectBursts` | Block power, Otsu split in dB with a contrast guard, bridged gaps, edge shares for sub-block edges, per-burst start/length/power/frequency, duty cycle, repeat period and regularity. |
| `sigmeas_hopchirp.hpp` | `trackPeak`, `analyseHops`, `analyseChirps` | Peak-frequency track; dwells, hop set, channel spacing, hop rate by pitch regression; straight sweeps, signed slope, width, direction, period. |
| `sigmeas_audio.hpp` | `measureAudio`, `analyticFromReal`, `dechirp`, `searchDrift` | SINAD, THD (dB, %), THD+N, SFDR (with spur frequency), ENOB, per-harmonic dBc, noise floor, band selectable; drift search by grid then golden section. ENOB (SINAD-1.76)/6.02 verified against manufacturer pages (section 7); the IEEE 1241 text itself was not opened. |
| `sigmeas_varispeed.hpp` | `Varispeed` | No-copy, allocation-free reader, Catmull-Rom, speed +-16, slew, seek. |
| `sigmeas_histogram.hpp` | `amplitudeHistogram`, `magnitudeHistogram`, `powerCcdf` | Counts, probability, density, CDF, CCDF, statistics, crest factor; CCDF and PAPR. |
| `sigmeas_cursors.hpp` | `harmonicCursors`, `sidebandCursors`, `snapToPeak`, `readLevels`, `CursorSet8` | Marks and the eight-cursor model (Free, LockedSpacing, Harmonic). |
| `sigmeas_framelog.hpp` | `symbolConfidence`, `characterConfidence`, `lowConfidence`, `FrameRecord`, `FrameLog` | Posterior of the winning tone from I0(2a sqrt E); a frame record that keeps a failed-CRC frame with its flag in a bounded thread-safe log. |

Runner: `tests/sigmeas_realdata.cpp` (target `sigmeas_realdata`, not a ctest): reads a WAV, prints numbers.

## 2. Accuracy pinned (all on generated signals with noise, a frequency offset and a clock error)

| Measurement | Requirement | Pinned and measured |
|---|---|---|
| OBW 99 % | within 2 % | flat and Gaussian spectra at 20-25 dB SNR, 300 ppm: within 2 % (check passes at 2 %; the Gaussian x-dB widths within 3 %, SNR within 1 dB) |
| FM deviation | within 1 % at 12.5 and 25 kHz | 2.5, 12.5, 25 kHz at 35 dB SNR, 200 ppm: within 1 % (tone estimate); the percentile estimate is noise-limited (4 % at 35 dB, 1 % at 60 dB) |
| AM depth | within 0.5 point | 0.1, 0.3, 0.8 at 35 dB SNR: within 0.5 point (tone estimate); percentile estimate 2.5 points at 35 dB |
| Baud (FSK, OOK) | within 0.5 % with jitter | +-8 % edge jitter, 300 ppm, 45.45/1200/4800 Bd FSK and 2000 Bd OOK: within 0.5 % (measured 0.03 %); FSK shift within 1 % |
| Burst times | within one block | within half a block (finer figure also pinned) |
| Hop rate, chirp slope | tracking | hop rate within 2 %, chirp slope within 2 %, sweep period within 1 % |
| SINAD, THD, SFDR, ENOB | pinned | SINAD and THD within 0.3 dB, SFDR within 0.5 dB, ideal 10/12/13-bit ENOB within 0.1 bit |
| Drift | pinned | 3.7 Hz/s within 1 % at 0 dB SNR |
| Histograms | pinned | bins within 5 binomial sigmas of the analytic probability; CCDF within 15 % of exp(-g) |

## 3. Tests: real result lines

Own tests, final tree (`ctest -C Release -R test_sigmeas`): `100% tests passed, 0 tests failed out of 12`, 7.7 s. Per file:

```
test_sigmeas_bandwidth: 66 checks, 0 failed
test_sigmeas_modulation: 84 checks, 0 failed
test_sigmeas_symbols: 93 checks, 0 failed
test_sigmeas_vstime: 34 checks, 0 failed
test_sigmeas_bursts: 76 checks, 0 failed
test_sigmeas_hopchirp: 54 checks, 0 failed
test_sigmeas_audio: 73 checks, 0 failed
test_sigmeas_varispeed: 57 checks, 0 failed
test_sigmeas_histogram: 120 checks, 0 failed
test_sigmeas_cursors: 117 checks, 0 failed
test_sigmeas_framelog: 60080 checks, 0 failed
test_sigmeas_fuzz: 5276 checks, 0 failed
```

Every block has: a happy path against a known answer, the failure path of each reason, the boundary (short span, single burst, tone only, signal filling the range, constant signal, speed 0), empty input, bad rate, NaN and infinity, and every bad parameter. `test_sigmeas_fuzz` (the brief's "fuzz run"; the blocks parse no files or structs, so the fuzz is over samples, rates and parameters) runs 400 rounds across all blocks, including the frame log and the cursors.

Findings the tests produced and the fixes (each a real bug caught): the edge-grid fit needed whole-period accumulation from edge to edge (a period 2 % off lost count over 1000 symbols); noisy tracks needed a smoothing ladder; FSK shift read 1 % low from class medians (now symbol-centre means of the raw track); digital silence (exact zeros, -300 dB) stretched the burst threshold's histogram (now held to 50 dB under the median block); DC alone read as a tone (now a tone must be within 60 dB of the whole signal and 20 dB over the median bin); a signal that fills the whole range is refused rather than guessed (floor can be given).

## 4. Break-it table (the behaviour removed, red seen, restored, green seen)

Each by `D:/foxsdr-build/sigmeas-breakit.py` (mutates the header byte-exactly, rebuilds, runs; restores byte-exactly, touches, rebuilds, runs).

| Block | Mutation | Mutant | Restored |
|---|---|---|---|
| bandwidth | OBW tail fraction halved | 4 failed | 0 failed |
| bandwidth | floor not subtracted | 1 failed | 0 failed |
| modulation | tone gain x0.97 | 4 failed | 0 failed |
| symbols | edge-grid fit removed | 6 failed | 0 failed |
| symbols (OOK) | pulse-width keying off | 3 failed | 0 failed |
| vstime | centre offset not added to frequency | 3 failed | 0 failed |
| bursts | edge shares replaced by whole blocks | 2 failed | 0 failed |
| bursts | contrast guard off | 2 failed | 0 failed |
| hopchirp | hop-set clustering off | 2 failed | 0 failed |
| hopchirp | chirp slope sign flipped | 4 failed | 0 failed |
| audio | harmonics left out of THD | 4 failed | 0 failed |
| audio | de-chirp phase sign flipped | 3 failed | 0 failed |
| varispeed | speed ignored | 14 failed | 0 failed |
| histogram | probability denominator wrong | 3 failed | 0 failed |
| cursors | harmonic base not divided by order | 2 failed | 0 failed |
| framelog | CRC flag forced true | 7 failed | 0 failed |
| framelog | failed frames kept when keepBad is off | 1 failed | 0 failed |
| framelog | Bessel term removed from the posterior | 20 failed | 0 failed |
| all (fuzz) | shared non-finite guard removed | crash 0xC0000005 | 0 failed |

Not caught: shrinking the default dwell tolerance of `analyseHops` from 2.5 to 0.2 bins left every test green, even with a 6 dB-SNR hopper and an explicit tolerance sweep (the parabolic peak is steadier than 0.2 bin). The default is not pinned; the hop-set break-it above is the hop test's break-it.

## 5. Real recordings (`D:/foxsdr-samples`, counts and numbers only, nothing copied or committed)

Runner: `sigmeas_realdata`. Raw outputs in `D:/foxsdr-build/sigmeas-real-*.txt` (not committed).

| Recording | Measured | Known value | Verdict |
|---|---|---|---|
| POCSAG 512, 1200, 2400 (audio-to-NFM copies, 96 kS/s) | baud 512.00, 1200.00, 2400.00; edge jitter 0.005, 0.020, 0.030 UI | 512, 1200, 2400 Bd | exact |
| POCSAG "1200 variant" copy | baud 1200.99 | 1200 | 0.08 %, but jitter 7.3 UI: the file holds several transmissions on different phases (see limits) |
| POCSAG real I/Q (`sigid-pocsag-iq`, 128 kS/s, 16.4 s), 32 windows of 0.5 s on the lower signal | single-baud windows: 512.00 (7 windows), 1200.00 (9 windows), 1600.00 (3 windows), 75.00 (1), 1500.00 (2); shift 9003-9006 Hz in the 512 and 1200 Bd windows | POCSAG shift 9 kHz (+-4.5 kHz) | 512 and 1200 Bd and the 9.0 kHz shift agree; 1600 Bd windows read shift 9604 Hz (FLEX 1600 is +-4.8 kHz, i.e. 9.6 kHz); 10 windows straddled a change of baud and are flagged by jitter above 0.05 UI or by no result |
| Same file, OBW 99 % of the two signals (bands -20..-1 kHz, 3..28 kHz) | 11.20 kHz and 11.32 kHz | Carson's rule for +-4.5 kHz deviation at 1.2 kBd: about 11.4 kHz | agrees |
| FLEX real I/Q (`sigid-flex-iq`, 39 kS/s, 28 s) | OBW 12.85 kHz; windows at 2 s and 10 s: baud 3199.93 and 3199.99, shift 9495 Hz and 8031 Hz (the second mid-change); other windows no result | FLEX 3200 Bd, +-4.8 kHz deviation | baud exact |
| FLEX 1600 2-level (audio copy) | baud 1599.92, jitter 0.10 UI over 6.9 s | 1600 Bd | 0.005 % |
| AIS real I/Q (48 kS/s, 42.9 s) | OBW 13.1 kHz (two channels in the capture); FSK and the burst list **gave no result** (no on/off structure at the defaults; GMSK at 5 samples per symbol is below the 9-samples-per-symbol floor) | 9600 Bd GMSK | not resolved: reported, not hidden |
| ADS-B `modes1` (2 MS/s, 0.18 s), block 8 samples | 3614 bursts (most partial pulses); 38 in the 120 us class | long Mode S message 120 us | the class is there |
| ADS-B SDRangel 2.4 MS/s, first 8 s, block 12 samples (5 us) | 6431 bursts; length classes 64 us (103 bursts) and 120 us (78 bursts) among the five commonest | short 64 us (8 us preamble + 56 bits), long 120 us (+112 bits) | both standard lengths found |
| ERT SCM `scm-g002` (2.4 MS/s assumed, 8.5 ms) | unit 33330.9 Bd (30.00 us), 74 pulses, keyed 32.5 / 62.5 us | 32768 chips/s is the plugin's hypothesis (its source comment says no allowed source states it) | +1.7 %: the file's rate is assumed, not stated |
| ERT SCM `scm-g001` | unit read as 10 us (3 chips per unit) with 0.44 UI jitter: ambiguous fit; short 30.1 us, long 60.0 us | | the ambiguity is a known limit of a run fit on a handful of runs |
| ERT SCM+ `scmplus-g003` (rate stated: 2359296) | unit 32765.9 Bd (30.52 us), jitter 0.033 UI, 103 pulses | 32768 chips/s | -0.006 % |
| Oregon Scientific THN132N (250 kS/s, 0.52 s) | unit 2047.94 Bd (488.30 us), keyed 446.8 / 929.0 us, jitter 0.051 UI; 2 bursts of 163.7 ms | 1024 Hz bit clock = 2048 Bd half-bit (the weather plugin's source comment) | -0.003 % |

## 6. Wiring and screen hand-off

The blocks are free functions on a span; nothing is wired into the engine (forbidden files). A host panel, the web page and the analyser project call them on the I/Q or audio of the selected box. Suggested engine/API ops for the serial wiring lane: one read op per block taking `{start_s, duration_s, centre_hz, width_hz}` and returning the result struct as JSON (memory only); none is added here.

Controls for the screen lane (English strings; default and range from the code):

| Tool | Label | Op or fact | Range | Default |
|---|---|---|---|---|
| Bandwidth | "Occupied bandwidth (%)" | `obwFraction` | 50.1-99.9 | 99 |
| | "Show x dB widths" | `xDb` list | 1-60 dB | 3, 6, 20, 26 |
| | "Search window low / high (Hz)" | `bandLowHz`, `bandHighHz` | within the span | whole span |
| | "Noise floor override" | `floorPower` | > 0 | auto |
| Modulation | "Audio bandwidth (Hz)" | `audioBandwidthHz` | 0 - 0.45 x rate | rate / 5 |
| | read-outs "Peak deviation", "RMS deviation", "Carrier offset", "AM depth (%)", "Over-modulated" | | | |
| Symbols | "Cursor 1 / Cursor 2", "Symbols between cursors", "Snap to edges" | `periodFromCursors`, `refineCursorPeriod` | count >= 1 | |
| | "Save symbols..." with "Float32 / Text / 0-1 string" | `writeSymbolFile` | | Float32 (written only on the user's click) |
| | "Max baud (Bd)", "Min baud (Bd)", "Run-fit tolerance" | `SymbolParams` | | rate / 6, 0, 0.15 |
| | read-outs "Mark (Hz)", "Space (Hz)", "Shift (Hz)", "Baud", "Edge jitter (UI)", "Pulse widths short / long", "Unit baud" | | | |
| Vs time | "Amplitude", "Frequency", "Phase" plots, "Remove carrier phase", "Max points" | `VsTimeParams` | | off, no limit |
| Bursts | "Block (samples)", "Minimum contrast (dB)", "Bridge gaps (blocks)", "Minimum burst (blocks)", "Maximum bursts" | `BurstParams` | | rate / 4000 (>= 16), 6, 1, 2, 1000 |
| | columns "Start (s)", "Length (ms)", "Frequency (Hz)", "Power (dBFS)", "Peak (dBFS)", "Repeat period (s)" | | | |
| Hop / chirp | "Frame (samples)", "Hop (samples)", "Minimum peak over noise (dB)", "Dwell tolerance (Hz)" | `TrackParams`, `HopParams` | power of two | rate / 200, frame / 4, 10, 2.5 bins |
| | read-outs "Hop rate (Hz)", "Dwell (ms)", "Channels", "Channel spacing (Hz)", "Chirp slope (Hz/s)", "Sweep width (Hz)", "Sweep period (s)", "Direction" | | | |
| Audio | "Tone (Hz)", "Harmonics up to", "Band low / high (Hz)", "Full scale (peak)" | `AudioParams` | harmonics 2-64 | auto, 9, 20 Hz / Nyquist, 1.0 |
| | read-outs "SINAD (dB)", "THD (dB)", "THD (%)", "THD+N (dB)", "SFDR (dBc)", "ENOB (bits)", "Noise floor (dBFS)" | | | |
| Drift | "Drift search from / to (Hz/s)", "Steps" | `DriftParams` | steps >= 8 | -50 / 50, 64 |
| Varispeed | "Speed" | `setSpeed` | -16 to 16 | 1 |
| | "Speed smoothing" | `setSlew` (change per sample) | >= 0 | 0 |
| Histogram | "Bins", "Range low / high" | `HistParams` | 1-65536 | 64, data range |
| | "CCDF range (dB)", "CCDF step (dB)" | `powerCcdf` | step > 0 | 20, 0.5 |
| Eight cursors | "Cursor 1" ... "Cursor 8", "Mode: Free / Locked spacing / Harmonic", "Spacing (Hz)", "Reference cursor", "Snap to peaks", "Read levels" | `CursorSet8` | | Free, 1 kHz, cursor 1 |
| | read-outs "Offset (Hz)", "Ratio", "Level (dBFS)", "Out of range" | | | |
| Harmonic / sideband | "Harmonic cursors", "Sideband cursors", "Sideband spacing (Hz)", "Sideband count" | `harmonicCursors`, `sidebandCursors` | count 1-1000 | |
| MFSK confidence | "Show uncertain characters", "Confidence threshold" | `lowConfidence` | 0-1 | off, 0.9 |
| CRC frames | "Show frames with CRC errors" | `FrameLog::snapshot(includeBad)` | | off |
| | "Keep frames with CRC errors" | `FrameLog::setKeepBad` | | on |
| | "Frames kept" | capacity | >= 1 | 256 |

Plugin-side hand-off for the MFSK and frame rows: an MFSK decoder passes its M tone energies per symbol to `symbolConfidence`, multiplies the posteriors of a character's symbols with `characterConfidence`, and pushes each decoded frame to a `FrameLog` through `FrameRecord::withCrc` (or `withoutCrc` for a protocol with no check value). The plugins decide what bytes may be shown (the log never prints or saves); the log keeps frames in memory only.

## 7. Sources opened and licences

Opened and read: ITU-R Recommendation SM.328-12 (docx, https://www.itu.int/dms_pubrec/itu-r/rec/sm/R-REC-SM.328-12-202509-I!!MSW-E.docx): RR No. 1.153 occupied bandwidth with the 0.5 % default for beta/2, and section 1.8 x dB bandwidth; ITU publication, quoted in comments only (one definition, short). Search-result summaries (not full pages) for ITU-R SM.443-4 (https://www.itu.int/dms_pubrec/itu-r/rec/sm/R-REC-SM.443-4-200702-I!!PDF-E.pdf, text not extractable, not read) and for ENOB = (SINAD - 1.76) / 6.02: https://www.analog.com/en/resources/technical-articles/defining-and-testing-dynamic-parameters-in-highspeed-adcs-part-1.html and https://developerhelp.microchip.com/xwiki/bin/view/products/data-converters/adc-specs/adc-ac-specifications/effective-number-of-bits/ (summaries only; pages not opened in full). The Demod Analyzer source (own, MIT) was read for what it measures. No GPL, LGPL or AGPL source was read; no software downloaded; no outside connection except these document reads.

Not opened (recalled, so flagged): the IEEE 1241 text, audio-standard wording for THD, Otsu 1979, the non-coherent MFSK detector in the textbooks, Abramowitz and Stegun (the Bessel polynomials are checked numerically against `std::cyl_bessel_i` instead). The known values in section 5 come from the project's own plugin sources (ERT 32768 chips/s is marked there as a hypothesis, Oregon 1024 Hz) and the signal standards as the brief names them; POCSAG 512/1200/2400 and 9 kHz shift, FLEX 1600/3200 and +-4.8 kHz, AIS 9600, ADS-B 64 and 120 us are not re-verified from a page in this lane.

## 8. UNVERIFIED, limits, owner decisions

UNVERIFIED:
- Built and run on MSVC (Windows) only; not compiled with GCC or Clang. `test_sigmeas_framelog` uses `std::cyl_bessel_i`, which libc++ may lack.
- No sanitizer run (the standard's address-sanitizer DLL rule was not needed: nothing was built with the sanitizer).
- Standard texts as listed in section 7; the section 5 "known" figures are not re-opened.
- The confidence is a posterior under an estimated tone-to-noise ratio: the Monte Carlo shows the mean posterior within 0.06 of the real accuracy and symbols scored 0.9 or more right 97.9 / 99.7 / 100 % of the time at a2 = 6 / 10 / 16, but not against a real MFSK receiver.
- Headless gate status: see section 9.

Limits (stated in the headers):
- One signal per span: two overlapping signals read as one. FSK and OOK need one baud rate per span: a span holding transmissions on different phases or at different rates gives a wrong baud and a large jitter (above 0.05 UI is the flag; the real POCSAG windows show it). Cut the span with `detectBursts` first.
- FSK/OOK need at least 9 samples per symbol after channel selection (the smoothing ladder); AIS GMSK at 5 samples per symbol is not resolved.
- A run fit on a handful of runs can pick a fraction of the symbol period (ERT `scm-g001`); `ambiguous` flags only the all-equal-runs case.
- Percentile deviation and depth read noise as modulation; the tone estimate does not.
- SINAD above about 82 dB reads low (window skirts); ideal 14-bit reads 13.87 bits.
- A signal that fills the whole analysed range cannot be told from its floor: `measureBandwidth` refuses unless the floor is supplied.
- A hop or a sweep is resolved to the frame hop; a chirp faster than a few bins per frame smears.

Owner decisions: none needed. No identifiers, callsigns, positions or message content are handled, written to disk, or sent; `FrameLog` holds frame bytes in memory only and the host must apply the plugin's display rules before showing them. No network, no new dependency, no Windows dialog (no store, certificate, provider or prompt was touched).

Config check: `%APPDATA%/foxsdr/config.json` sha256 c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 before (the brief's value) and after (computed with `sha256sum` at the end of the work): unchanged.

## 9. Gates (final tree)

Build: the whole tree (`cmake --build D:/foxsdr-build/en-sigmeas --config Release --parallel 3`, configured `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`, MSVC, vcpkg) built with no compile or link error.

Own tests: `ctest -C Release -R test_sigmeas`: 100 % passed, 12 of 12 (7.7 s); `test_sigmeas_framelog` re-run after the Bessel check was added: 60080 checks, 0 failed.

Diff against the base `d3630d78` outside the new files: `tests/CMakeLists.txt`, 4 lines appended (the runner target). No other existing file changed, so no existing test can see a different binary except through that CMake stanza.

Headless set (`ctest -L headless`, 715 tests), run in slices of at most 9 minutes (positions 1-110, 111-260, 261-266, 267-400, 401-560, 561-680 twice because the first was cut by the timeout, 681-715), `-j3` to `-j5` on a shared machine. Every failure was re-run alone:

- Passed alone (load flakes): test_engine_api_plugin_smoke_radio, test_engine_api_schedule, test_feature_request, test_gps_reader, test_os_crash_record, test_hung_window_excuse, test_rtl_tcp_source, test_udp_audio, test_usb_bounded_walk, test_recorder_bookmarks_scanner_stepped, test_remote_key_release_free, test_remote_key_release_stepped, test_window_web_feed_stepped, test_thread_beats, test_transmit_key_race, test_tx_audio, test_window_threaded_look, test_gps_app_free, test_shutdown_budget_free, test_stop_ends_recordings_free, test_failure_files, both `interface_fence_*` probes, `telemetry_worker` (last slice).
- Known false failures of the standard (configuration, not this lane): test_page_resize_id, test_png_decode, test_pass_control, test_window_failed_app (it needs the Slint window it was configured without).
- Still failing alone, all timing bounds or a missing fixture, none touching a sigmeas header: test_excuse_cap (a watchdog report count), test_pa_stream_list_guard (a wait bound, 281 ms against 250), test_engine_api_transmit (26 timing bounds, e.g. 124 ms against 100 ms), test_engine_api_merge (82 "unclean" under a 223 s exhaustive run), test_crash_upload_free (one connection count), test_engine_api_source_kinds (`scope.wav` is not in `src/slint/tests/drive/fixtures` of this worktree). Not proven to fail on the base tree too: no base build was made. They were not re-run on a quiet machine.
- Not re-run alone after the first failure: test_diag_hang_free (its slice failure was a 150 s run; the second attempt was cut by the shell timeout).

Gates not done: a quiet-machine re-run of the six still-failing tests; the base-tree comparison for them.
