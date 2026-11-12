# Signal blocks, fourth batch (2026-11-11)

Branch `feat/signal-blocks-4` (local, cut from `266926bf`, not pushed, not merged, no version bump). Eight standalone blocks (ten headers) and their tests,
closing rows of `FEATURE-GAPS.md` ("Filters and DSP", "Demodulation modes", "Spectrum and waterfall") that batches 1 to 3 do not cover. **Nothing is wired**:
only new files were added; no existing file was edited, not even `tests/CMakeLists.txt` (the test glob picked the new tests up after a re-configure). Conventions
follow `signal-blocks-2.md` (header-only, `cascade::dsp` / `cascade::core`, tests with `test_check.hpp`, `AllocProbe` from `tests/alloc_probe.hpp`, generated
signals only, break-it table per behaviour). New shared test helper: `tests/blk4_util.hpp`.

| Part | Header | Test | Commit |
|---|---|---|---|
| 1 Audio equaliser, high-pass, low-pass | `src/dsp/audio_biquad.hpp` (shared), `src/dsp/audio_eq.hpp` | `tests/test_audio_eq.cpp` | be4d337a |
| 2 CW audio peak filter, channel-filter shape design | `src/dsp/cw_filter.hpp` | `tests/test_cw_filter.cpp` | c999dd4f |
| 3 FM noise squelch | `src/dsp/noise_squelch.hpp` | `tests/test_noise_squelch.cpp` | d4511807 |
| 4 Automatic frequency control | `src/dsp/afc.hpp` | `tests/test_afc.cpp` | 150f64bb |
| 5 I/Q correction | `src/dsp/iq_correct.hpp` | `tests/test_iq_correct.cpp` | d0f76eaa |
| 6 Stereo placement | `src/dsp/stereo_place.hpp` | `tests/test_stereo_place.cpp` | 02f40a22 |
| 7 C-QUAM decoder + pilot | `src/dsp/cquam.hpp` (+ a notch section in `audio_biquad.hpp`) | `tests/test_cquam.cpp` | 86540b08 |
| 8a SNR tap with history ring | `src/core/snr_tap.hpp` | `tests/test_snr_tap.cpp` | 7cc18410 |
| 8b Constellation and eye tap | `src/dsp/symbol_tap.hpp` | `tests/test_symbol_tap.cpp` | fe0b6bfb |
| 8c Zoom FFT | `src/dsp/zoom_fft.hpp` | `tests/test_zoom_fft.cpp` | 359b5ced |
| 9 MPX tap and OIRT notes | notes only (below); nothing built | | (this record) |

S-unit conversion (part 8) was NOT rebuilt: it exists (see below). The other batch-3 lane (spectrum size and window, min hold, manual notches bank, CW pitch, colour maps,
speed and time stamps, MQTT, rotator) and the engine-branch AGC modes and noise blanker were not touched or repeated.

## What existed already (searched in the worktree before each part)

| Part | Search result |
|---|---|
| 1 EQ, HP/LP | No equaliser, shelf or graphic code anywhere in `src`. The only biquad is the notch inside `src/dsp/notch.hpp` (`Notch`, class-local design, not reusable for shelves/peaks); `ctcss.hpp` has its own filters. The audio chain post-processing is notch, auto-notch, noise reduction (`pipeline.cpp:2192-2205`) then soft limiter (`:2386`). Nothing high-pass / low-pass for audio. |
| 2 CW peak, shape | No CW audio filter. The channel filter is `Vfo`'s low-pass (`vfo.cpp`, one shape) and `PassbandFilter` (batch 2, Kaiser, 80 dB): no shape-factor control. `windowedSincLowpass` (`fir.hpp`) offers windows but no Kaiser or shape-factor design. |
| 3 Noise squelch | `dsp::Squelch` (`squelch.hpp:73`): a LEVEL squelch on channel power before the demodulator (`PowerMeter` EMA, 3 dB hysteresis, hold, 5 ms ramp). No noise squelch anywhere. The batch-2 record's `NoiseFloorTracker` serves auto-squelch only. |
| 4 AFC | Only tuner-chip `afc` words in the MSI001 / Mirics drivers (hardware registers, `tuner_msi001.*`, `mirisdr_source.cpp`). No software AFC. |
| 5 I/Q correction | Hardware-level image words in the AirspyHF, RX888 and R82xx drivers (`airspyhf_*`, `tuner_r82xx.cpp`); `patch_strip.hpp` has a DC-block name for patch strips. No software DC / balance / swap. |
| 6 Stereo placement | Nothing. Every non-WFM mode is duplicated to both channels (`pipeline.cpp:2093-2099`); WFM goes through `StereoFm`. No pan, routing, pseudo-stereo or binaural. |
| 7 C-QUAM | Nothing. `DemodMode::AM` is an envelope detector (`demod.cpp`, AM case). |
| 8 Taps | **S-units: exist** - `core/smeter_cal.hpp` (batch 1): IARU Region 1 R.1, S9 = -73 dBm below 30 MHz and -93 dBm from 30 MHz up, 6 dB per S, `dbmToS`, `formatS`, calibration table; NOT rebuilt. **SNR: pieces exist** - `Pipeline::signalPowerDb()` (`pipeline.cpp:2017-2018`), `core::NoiseFloorTracker`, `powerlog::snrFromTotalAndFloor` (batch 2); reused, not rebuilt. **Constellation / eye: none** (the demod scope's `scopeIq_` tap, `pipeline.cpp:2019-2033`, is a raw I/Q rolling window for a vector display: no symbol timing, no eye). **Spectrum: `SpectrumEstimator` (`spectrum.hpp`) exists at the full rate only**; no zoom. |
| 9 MPX | **The MPX tap exists**: `Pipeline::scopeMpx()` (`pipeline.hpp:805`), a `ScopeTap<float>` of 65536 samples (`:1163-1166`), pushed in WFM only at `pipeline.cpp:2057`, at the composite rate, flat, before de-emphasis and before `StereoFm`. See part 9. |

## Sources, and what is and is not verified

| Fact | Source read this session | Status |
|---|---|---|
| Biquad coefficient formulas (peakingEQ, lowShelf, highShelf, LPF, HPF, notch; A, w0, alpha) | "Audio EQ Cookbook", W3C Working Group Note, editor Raymond Toy, 8 June 2021, https://www.w3.org/TR/audio-eq-cookbook/ (read 2026-11-11) | verified (formulas transcribed from the page; the tests also measure every filter, e.g. 12.00 dB on each graphic band centre, -3.01 dB at every filter corner) |
| Butterworth 4th-order section Q values 0.5412 and 1.3066 | textbook (1 / (2 cos(pi/8)), 1 / (2 cos(3 pi/8))); measured -3.01 dB at the corner | verified by measurement |
| Kaiser design relations (beta = 0.1102 (A - 8.7), N - 1 = (A - 7.95) / (2.285 dw)) | Oppenheim and Schafer, Discrete-Time Signal Processing, 3rd ed., 7.5.3 | UNVERIFIED against the book (from memory); the helper measures and iterates, the measured shape factors are what is pinned |
| FM noise squelch principle (discriminator noise rises with frequency; noise above the voice band is quieted by a carrier) | textbook, from memory | UNVERIFIED as a citation; the behaviour is measured |
| Lag-one autocorrelation frequency estimator; peak interpolation by a parabola on log magnitudes | textbook, from memory | UNVERIFIED as a citation; measured |
| Circularity-based I/Q imbalance compensation | Windisch and Fettweis, ISCCSP 2004; Anttila, Valkama and Renfors, IEEE Trans. Veh. Technol. 57(4), 2008 | UNVERIFIED against the papers (names from memory); the maths (gamma such that E[z^2] = 0) was derived and is measured: image rejection 17 to 36 dB before, 106 dB after |
| Pseudo-stereo by complementary combs | Schroeder, "An artificial stereophonic effect obtained from using a single signal", J. Audio Eng. Soc. 6(2), 1958 | UNVERIFIED (from memory); the correlation formula (1 - g^2) / (1 + g^2) and the mono-sum property were derived and are measured |
| Woodworth ITD = (a / c)(theta + sin theta), a = 0.0875 m, c = 343 m/s | Woodworth, Experimental Psychology, 1938; Woodworth and Schlosberg, 1954 | UNVERIFIED against the books (constants as usually quoted); the delay produced is measured against an independent evaluation of the formula |
| C-QUAM system description: sum on amplitude, difference on carrier phase "by an arctangent expression", left advances the phase, 100 % difference = +-0.785 rad, single channel -0.75 limit 1.25 rad, pilot 25 Hz +-0.05 rad (5 % L-R), THD <= 3 %, injection 5 % +-1 % | 47 CFR 73.128 "AM stereophonic broadcasting", 58 FR 66301, 20 Dec 1993: https://www.law.cornell.edu/cfr/text/47/73.128 and https://www.govinfo.gov/content/pkg/CFR-2009-title47-vol4/xml/CFR-2009-title47-vol4-sec73-128.xml (both read 2026-11-11) | **verified (primary, open)** for everything in this row |
| The C-QUAM equation itself, s = (1 + L + R) cos(w t + arctan((L - R) / (1 + L + R))) | the rule's equations are IMAGES (ER20DE93.000 to .002) and could not be read; a web-search summary of a Motorola patent abstract quoted the arctan term (not read in full) | **reconstruction**, consistent with both numbers in the text (arctan(1) = 0.7854; arctan(-0.75 / 0.25) = -1.249 rad against the 1.25 limit); where the pilot enters the arctangent: **UNVERIFIED** |
| ANSI/EIA-549-1988 and NRSC-1 (audio response, pre-emphasis) cited by the rule | not read | not needed (the decoder works on the demodulated channels) |
| IARU Region 1 R.1 S-meter figures | not re-read; see `signal-blocks.md` (secondary sources, UNVERIFIED against R.1 itself) | unchanged |
| OIRT band 65.8 to 74 MHz (the lead of the same article says 65.9 to 74.0), 30 kHz channel grid, stereo by polar modulation with a reduced 31.25 kHz subcarrier, no pilot mentioned, no deviation or de-emphasis stated | Wikipedia, "FM broadcast band", sections "OIRT bandplan" (fetched 2026-11-11; secondary) | secondary source, UNVERIFIED against a standard; used for part 9 notes only |

No program source was read (clean room). No file was downloaded; web pages were read through the fetch tool for citations only.

## Part 1: audio equaliser and high-pass / low-pass (`AudioEq`)

**What.** Bass (low shelf 150 Hz), mid (peak 1 kHz, Q 0.9), treble (high shelf 3.5 kHz), each +-12 dB; a 5-band graphic (peaks at 100, 300, 1000, 2500, 5000 Hz, Q 1.2, +-12 dB);
high-pass choice Off / 100 / 300 Hz and low-pass choice Off / 2400 / 3000 / 6000 Hz, each a 4th-order Butterworth. In place, 48 kHz (any rate). Gains ramp at 1200 dB/s in 16-sample steps with the
coefficients recomputed per step; a filter choice change runs the old and new filter side by side and crossfades over 10 ms. All flat and off: `process()` returns at once (bit-exact), and
`isBypassed()` says so. Threads: setters any thread (atomics), `process()` on the DSP thread; no allocation.

**Measured (`test_audio_eq`, 56 checks).**
- Bass +9: 9.01 dB at 25 Hz, 4.50 dB at the 150 Hz corner, 0.00 dB at 3 kHz. Mid +6: 6.00 dB at 1 kHz, 0.08 dB at 100 Hz. Treble -9: -8.97 dB at 12 kHz, 0.00 dB at 200 Hz.
- Graphic bands: 100 / 300 / 1000 / 2500 / 5000 Hz read +12.00 and -12.00 dB at their own centres; the 1 kHz band at +12 shows 0.69 dB at 4 kHz.
- High-pass 100 and 300 Hz: -3.01 dB at the corner, 0.00 dB at 1.5 kHz, -38.2 dB at a third of the corner. Low-pass 2400 / 3000 / 6000 Hz: -3.01 dB at the corner, -25.0 / -25.5 / -30.6 dB at twice the corner.
- Cost per sample: 0.001 ns when off, 21.6 ns with all eight bands and both filters on. `operator new`: 0 in 40 calls with gain and choice changes.
- Clicks (largest sample step against the tone's own): mid 0 to +12 dB under 1 kHz 0.992; mid +12 to -12 to +12: 0.992; treble the same at 3.5 kHz 0.522; HP Off/300/100/Off 1.006; LP in under 2.5 kHz 0.978.
- Any chunking (1, 7, 64, 333) is bit-identical; NaN and infinity read as 0; gains clamp to +-12 and a NaN gain is ignored; 96 kHz works (mid +6 reads 6.00 dB).

**Break-it** (patch, rebuild, named check RED, restore):

| Removed or broken | RED |
|---|---|
| Gain ramp off | the two mid +12 <-> -12 click checks (the 0 to +12 case alone did NOT show it: a first pass proved that case does not discriminate, so the hard case was added) |
| Filter crossfade off | HP/LP click check (step beyond the tone's own) |
| Bypass early-return removed | cost check (off no longer far cheaper; output stays bit-exact, as it must) |
| Low-shelf sign flipped | bass 25 Hz and corner checks |
| Single section instead of two | 11 checks (corner -5.33 dB, slope, in-band) |

**Limits.** Fixed centre frequencies and Q; no per-band Q or frequency control; denormals in the section states are not flushed (see part 3 for the measured cost of that in silence).

## Part 2: CW audio peak filter and channel-filter shape (`CwPeakFilter`, `designChannelFilter`)

**Peak filter.** Cookbook peaking section, Q = pitch / width, boost 0 to 24 dB, pitch 200 to 2000 Hz, width 30 to 500 Hz measured between the half-gain points (the cookbook's bandwidth). It boosts the tone; it does not cut the
rest. Pitch, width and boost glide (10 ms time constant); off is a bit-exact no-op, `isBypassed()`. **Measured (`test_cw_filter`, 192 checks):** centre gain 12.00 dB and half-gain width 30.0 / 99.9 / 499.1 Hz for requests
of 30 / 100 / 500 at 700 Hz (nine pitch x width combinations, all within 0.3 %); four widths away at most 0.67 dB; a pitch change moves the boost (900 Hz reads 12.0 dB, the old 600 Hz under 0.5 dB); cost 5.8 ns per
sample on, 0 off; 0 allocations; clamps (width 1 -> 30, 9999 -> 500, pitch 50 -> 200, NaN ignored).

**Shape design.** `designChannelFilter(fs, bw6Hz, shapeFactor or ChannelShape, maxTaps)`: linear-phase taps for a wanted -6 dB bandwidth and shape factor (width at -60 dB over width at -6 dB). Sharp 1.3 and medium 1.8
are Kaiser (A = 65 dB; length from the closed form, then up to 12 iterations that measure the real widths, rescale the transition and move the sinc cutoff). **A Kaiser design cannot be made softer than about 2.2**
(measured: whatever is asked, the iteration settles at 2.23 because so short a window moves the -6 dB point), so soft is a **Gaussian** (H = exp(-a f^2), taps exp(-pi^2 k^2 fc^2 / (ln2 fs^2)), cut at 1e-5): fixed shape factor
3.157 (the request is clamped; between 2.0 and 2.6 is not offered and is clamped to 2.0). The tap cap (default 2047) is honoured, `capped` is set and the measured shape factor returned.

| bw6, rate | sharp (1.30) | medium (1.80) | soft (3.157) |
|---|---|---|---|
| 2400 Hz, 48 kHz | 263 taps, SF 1.299, -6 dB width 2399.7 | 99 taps, 1.798 | 73 taps, 3.162 |
| 1500 Hz, 48 kHz | 419 taps, 1.300 | 157 taps, 1.802 | 117 taps, 3.162 |
| 500 Hz, 12 kHz | 315 taps, 1.299 | 119 taps, 1.796 | 89 taps, 3.162 |
| 2400 Hz, 12 kHz | 67 taps, 1.296 | 25 taps, 1.814 | 19 taps, 3.162 |

Seven bandwidth / rate rows are pinned (shape factor within 3 %, -6 dB width within 1 %, sum of taps 1, symmetric, worst response above the measured -60 dB edge under -59.5 dB). Step response within 5 % of final after
**3.25 ms (sharp, 263 taps), 1.23 ms (medium), 0.98 ms (soft)** at 2400 Hz and 48 kHz; overshoot 8.3 % / 4.5 % / 0 (the Gaussian has none). 100 Hz sharp at 48 kHz needs more than 2047 taps: it reports `capped`, 2047 taps, SF 1.91.
A 500 Hz sharp filter at 48 kHz would need about 4000 taps: the wiring should run narrow filters at a decimated rate (the 12 kHz rows).

**Break-it:**

| Removed or broken | RED |
|---|---|
| Q inverted | width, far-gain checks (5129 Hz instead of 30) |
| Shape iteration cut to one pass | shape factor rows (1.15 instead of 1.30, 1.39 instead of 1.80) |
| Gaussian branch off | soft rows (2.23 instead of 3.16), -60 dB check |
| Bypass early-return removed | cost check |
| Pitch / width / boost glide removed (alpha = 1) | **nothing**: the measured largest click ratio is 1.000 without the glide and 1.024 with it. The glide is kept as a precaution and is NOT shown to be needed; recorded, not claimed. |

## Part 3: FM noise squelch (`NoiseSquelch`)

**What.** Detector: 4th-order high-pass at 5 kHz on the discriminator output, 5 ms mean-square, in dB. Opens when the band level has stayed under the threshold for `openDelayMs` (default 20); starts closing when it rises
above threshold + `hysteresisDb` (default 3), closes after `tailMs` (default 100); inside the band the state holds; 5 ms linear gate ramp; starts closed; disabled walks to open in 5 ms then returns at once. `thresholdForQuietingDb(noiseOnlyLevelDb, quietingDb)`
helps set it against a measured noise-only level. Beside, not replacing, `dsp::Squelch`.

**Measured (`test_noise_squelch`, 68 checks).** Noise-only band level -10.57 dB re full scale (discriminator noise sigma 0.30). With 10 dB of quieting required and a carrier that quiets the noise by 20 dB, 20 noise seeds:
**open delay 34.0 to 35.1 ms, close delay 102.8 to 104.1 ms**; tail 0 and open delay 0: 14.2 ms and 3.7 ms; tail 500 ms: 503.7 ms. **False opens on one simulated hour of noise only (172.8 million samples each): 0 at 10 dB, 0 at 6 dB, 0 at 3 dB
required quieting** (20 ms open delay); with NO open delay at 3 dB: about 6 per hour (600 s x 6, so an estimate). Hysteresis: noise 1.5 dB above the open threshold for 10 s holds whichever state it started in. Tail: 60 and 90 ms noise bursts are bridged, 160 ms closes.
3 kHz-limited speech-like audio with the carrier present: gate below unity for 0.0000 of 19.5 s. Cost 3.6 ns per sample running. 0 allocations. Gate steps never exceed the 1/240 ramp. Detector aliasing the audio buffer works.
**Finding:** on a silent detector input the high-pass state decays into denormals and the cost rises to 68 ns per sample (measured by accident in a first run); real noise does not do this, but a wiring that feeds exact silence should set flush-to-zero.

**Break-it:** tail 0 -> close delay, 500 ms tail, bridging checks RED; hysteresis 0 -> hold and bridging RED; open delay 0 -> the `early open` check RED; ramp off -> 5 ms walk check RED; band-pass at 100 Hz (voice reaches the detector) -> carrier never opens it, RED; bypass early-return removed -> cost RED.

## Part 4: automatic frequency control (`AfcTracker`)

**What.** Output is a tuning correction in Hz for the wiring lane; the input is the channel baseband AFTER the receiver applied the current correction (closed loop), so each 43 ms block measures the residual offset. Estimators: `MeanFrequency` (lag-one autocorrelation
phase: AM with carrier, FM) and `Peak` (Hann FFT, parabolic interpolation, peak must be 15 dB over the median bin). Pull-in range (default 2500 Hz), hold range (5000), rate limit (500 Hz/s), loop gain 0.5, dead band 2 Hz, lock band 20 Hz (3 blocks), freeze below a level
threshold (3 dB hysteresis), when Peak finds no peak, or when the wiring says no signal (`setSignalPresent(false)`, to be driven from the squelch). SSB is documented as NOT a job for the mean-frequency mode (the mean frequency of a voice band is not a frequency to centre on). Disabled resets the correction to 0 once.

**Measured (`test_afc`, 38 checks, closed-loop simulation of the retune with 256-sample chunks).**
- Pull-in (setting 2500 Hz, AM 30 % at 1 kHz): carriers at +-1000 Hz acquired (residual 1.3 Hz, locked at 2.13 s); the sweep in 250 Hz steps acquired up to **+2250 and -2250 Hz**, not at 2500 (the edge is noisy: the estimate wobbles by a few Hz around the limit) or 2750.
- Hold (setting 4000 Hz): a 150 Hz/s drift is followed within **15.3 Hz** until the carrier is 4060 Hz off tune; lock is lost there and the correction stops at 4000 Hz.
- Rate limit: a 2000 Hz carrier step with 500 Hz/s: largest slew over one second 512 Hz/s (24 updates of 21 Hz fit a one-second window), within 20 Hz after 3.94 s; with 100 Hz/s: 102.4 Hz/s, 19.82 s.
- Freeze: carrier gone for 5 s (noise only): the correction moved **0.0000 Hz**, frozen throughout; the carrier back at the same place: first unfrozen and locked block 13 ms later. A strong interferer while `setSignalPresent(false)`: unmoved; with the gate back it is caught (2200 Hz).
- FM at +1500 Hz with +-3.7 kHz two-tone deviation: mean error -0.16 Hz, worst 14.4 Hz.
- Peak mode: static tones at 0 / 11.7 / 600.3 / -1234.5 Hz within 0.3 Hz (23.4 Hz bins); a keyed carrier 60 ms / 60 ms: mean error 1.02 Hz, 223 of 468 blocks accepted; 600 s of noise with the level gate wide open: 0 of 14062 blocks accepted.
- AM carrier-to-noise (48 kHz wide) 40 / 20 / 10 / 5 dB: rms error 0.97 / 0.77 / 7.5 / 21.3 Hz.
- 0 allocations in both modes; chunking (1, 37, 4096) gives the identical end state; non-finite input read as 0.

**Break-it:** rate limit off, freeze off, pull-in range ignored, hold clamp off, peak test off, error sign flipped, external gate ignored: each turns named checks RED (the pull-in edge, the freeze, the slew, the hold, the noise-block count). A real bug was found by the Peak-mode test before the commit (negative-frequency peaks were treated as "none") and fixed.

## Part 5: I/Q correction (`IqCorrector`)

**What.** In place, complex baseband at the source rate. DC: one-pole estimate (0.5 s, fast start), subtracted. Balance: blind, from the pseudo-covariance Pp = E[y^2] and Pw = E[|y|^2] (1 s): gamma = (1 - sqrt(1 - |rho|^2)) / conj(rho), z = k (y - gamma conj(y)) has E[z^2] = 0, k restores the
power; |gamma| limited to 0.3 (so an improper signal such as BPSK or one real tone is not eaten). Swap flag: I and Q exchanged (mirrors the spectrum), applied first, 2 ms crossfade. DC and balance ramp in and out over 20 ms; all off: bit-exact no-op.
It does NOT restore the true gain (it cannot know it): the tone ends at the input's power, sqrt((1 + g^2) / 2) of the clean amplitude; this is pinned.

**Measured (`test_iq_correct`, 51 checks).** A generated imbalance on a tone at fs/8 plus a second tone and noise 1e-4, image rejection of the main tone by DFT at 250 kS/s and 2.4 MS/s:

| gain error, phase | image rejection before | after |
|---|---|---|
| -0.92 dB, 3 deg | 24.6 dB | 106.1 dB (250 kS/s), 106.8 dB (2.4 MS/s) |
| -1.94 dB, 10 deg | 17.0 | 106.1 / 106.8 |
| 0 dB, 2 deg | 35.2 | 106.1 / 106.8 |
| +0.83 dB, -6 deg | 23.0 | 106.1 / 106.8 |
| -0.26 dB, 0 deg | 36.3 | 106.1 / 106.8 |

(With a 0.02 noise floor the same runs read 62.0 dB: that is the measuring floor of the 8192-point DFT, not the corrector.) A balanced input stays at 106.1 dB and the estimator reports 108.9 dB. The estimator's own reading of the input image rejection agrees with the measured value to 0.1 dB (24.6 against 24.6).
DC 0.058 (-24.7 dBFS) leaves a residual mean of 1.05e-4 (54.9 dB down); a 20 Hz tone survives at 0.2000. Swap: the tone moves to the mirrored bin; swapping twice is the identity; the swap crossfade keeps the largest step at 1.03 x the tone's own.
DC removal switched on under a 0.3 DC with a 0.01 tone: largest step 1.06 x the tone's own. Cost 10.9 ns per sample (DC + balance), so 2.4 MS/s costs 2.6 % of a core. 0 allocations; chunking (1, 99, 4096) bit-identical; a real (improper) signal leaves gamma at the limit and the output bounded.

**Break-it:** balance not applied, DC not subtracted, swap not applied, swap crossfade off, ramp off, gamma limit removed, power restore removed, gamma sign flipped: each RED on named checks.
**Limits.** The method needs a signal that is statistically alike either side of the centre before the imbalance; a band with ONE side occupied is still fine (it is the imbalance image on the other side that is measured), a signal that is itself improper is not.
Not tested on a real receiver's data. Frequency-dependent imbalance (a wideband front end) is not modelled: one gamma for the whole band.

## Part 6: stereo placement (`StereoPlacer`)

**What.** One per receiver: mono in, left / right out. Pan with the constant-power sine / cosine law (centre -3.01 dB each; a makeup gain setting), routing Left / Right / Both, pseudo-stereo (complementary combs, delay 8 ms, amount 0 to 1) and binaural placement (Woodworth ITD with a fractional delay on the far ear,
broadband level difference 6 dB x sin(azimuth)). Gains and azimuth glide; mode changes crossfade 10 ms; disabled copies the input to both channels bit-exact. **Method and its source: pseudo-stereo is Schroeder's complementary-comb idea (1958, from memory: UNVERIFIED), binaural is the Woodworth spherical-head ITD (1938, UNVERIFIED constants); the ILD is a stand-in for the frequency-dependent head shadow, which is NOT modelled.**

**Measured (`test_stereo_place`, 45 checks).** Pan law: worst deviation from cos / sin and from constant power over 21 positions 6.8e-8; pan -1 gives L 1.0000 R 0; centre 0.7071 each. Left and Right routes: exactly the input and exactly 0. Pseudo-stereo, noise input: L/R correlation 1.0000 / 0.8823 / 0.6000 / 0.0000 for g 0 / 0.25 / 0.5 / 1
(derived (1 - g^2) / (1 + g^2): 1 / 0.8824 / 0.6 / 0); mono sum L + R = 2x / sqrt(1 + g^2) to 1.2e-7; comb levels at 1000 Hz and 1062.5 Hz (g 0.5, 8 ms) 1.342 / 0.447 and 0.447 / 1.342 as derived. Binaural, noise cross-correlation: right channel lags left by -12.61 / -23.30 / -31.43 samples at 30 / 60 / 90 degrees
(Woodworth, evaluated independently in the test: 12.53 / 23.43 / 31.48; 656 us at 90 degrees) and the mirror for negative azimuths; level difference at 1 kHz 3.02 / 5.21 / 6.02 dB (6 x sin: 3.00 / 5.20 / 6.00). Clicks: pan hard left to hard right 1.00 x the tone's own; mode changes Pan/Pseudo/Binaural/Pan 1.34 x (bound 1.6, the comb changes the level). 0 allocations in 40 calls with changes; chunking identical in all three modes; cost 8.6 / 13.4 / 40.0 ns per sample.

**Break-it:** pan ramp off, linear pan law, ITD without the sin term, ILD removed, comb not complementary, mode crossfade off, ears swapped, left route silent: each RED. Two weaknesses of the first test draft were found and fixed before commit: the ITD was compared with the header's own function (a break would not have shown) and the pan click test switched on a zero crossing of the tone.
**Limits.** Linear interpolation for the fractional delay (0.2 dB error below 3 kHz at 48 kHz); a moving source's delay change shifts pitch by up to 0.8 % during the 78 ms glide.

## Part 7: C-QUAM decoder and 25 Hz pilot detector (`CquamDecoder`)

**Source and what is not known.** An open primary source exists and is cited (47 CFR 73.128, table above): the system description and the pilot numbers are verified. The equations in that rule are images; the encoding formula used is a reconstruction (header and table say so), checked against the two angle figures in the text.
Where the pilot enters the arctangent is not stated in what was read: UNVERIFIED. If the real system differs there, the pilot level the detector sees (0.0495 measured for a nominal 0.05) and the 0.5x to 2x window would move.

**Decoder.** Complex baseband in (carrier within a few Hz of DC; larger offsets are the AFC's job), second-order PLL (40 Hz loop for 1 s, then 4 Hz, damping 0.707) whose phase error is the **recovered D itself** (locking on the mean phase of y instead leaves a programme-dependent error, because the mean of arctan((L-R)/(1+L+R)) is not zero for a single channel);
S = |y| / Ac - 1, D = |y| Im(y) / Re(y) / Ac, L = (S + D) / 2, R = (S - D) / 2; D passes a 25 Hz notch (Q 5). Pilot detector: Hann-windowed correlation at 25 Hz over 0.2 s, amplitude between 0.5x and 2x of 0.05 and 6x above the same measure at 15 and 35 Hz (a Hann window has exact zeros 10 Hz from a tone; 20 and 30 Hz would see half the pilot);
stereo on after 3 windows (0.6 s), off after 5 misses (1 s); mono (L = R = S / 2) crossfaded over 50 ms; force mono / stereo.

**Measured (`test_cquam`, 79 checks, generated C-QUAM, carrier offsets 0, +12, -15 Hz, unknown phase).** Separation (wanted channel over the other at the same tone): **41.9 dB at 1 kHz, 31.4 dB at 300 Hz, 51.5 dB at 3 kHz** (4 Hz loop; the 10 Hz loop gave 35.7 / 25.2 / 45.2 dB: the leak is the loop tracking low-frequency D, so it falls with a narrower loop), the same for left only and right only and for a left tone with a right tone at 2 kHz; wanted level 0.400 against 0.4 programme.
Carrier-to-noise (48 kHz wide) 40 / 30 / 20 dB: 41.9 / 41.7 / 37.5 dB. Stereo on 1.60 s after the start; measured pilot level 0.0495. Pilot sweep: 0 / 0.01 / 0.02 / 0.03 rad off, **0.05 / 0.08 / 0.12 on**, 0.3 off; a 25 Hz programme tone of normal level (0.256 read) refused; 15 + 25 + 35 Hz at 0.05 each (bass) refused (25 Hz reads 0.050); two different speech-like channels: on with the pilot, off without; noise alone: off.
Mono programme without pilot: |L - R| exactly 0, L amplitude 0.3000. The pilot in the outputs: 0.0004 (0.025 without the notch). The compatible envelope is exactly 1 + L + R (2.2e-16). 0 allocations; chunking identical.

**Break-it:** D without the cos factor, PLL frozen, stereo always on, notch removed, pilot upper limit removed, lower limit removed, neighbour test removed (needed a new test: bass tones at 15 / 25 / 35 Hz), channels swapped, carrier level average starting at zero (that one was a real bug found by the first run: 0.318 instead of 0.300): each RED.
**Limits.** Separation is a property of this decoder on a clean generated signal; a real station with selective fading, modulation above 100 % or receiver filter asymmetry will be worse, and nothing here was run on a recording. The 25 Hz pilot cannot be told from a 25 Hz programme tone at exactly the pilot level in L-R (documented).

## Part 8: measurement taps

### 8a SNR tap (`core::meas::SnrTap`)
Reuses `signalPowerDb()` (total including noise), `NoiseFloorTracker` (minimum of 10 s of 100 ms block means; gate-open readings not fed) and `powerlog::snrFromTotalAndFloor`; adds `push()`, `latest()`, a lock-free history ring (copied with a sequence check by any thread), `meanSnrDb(seconds)`.
**Measured (`test_snr_tap`, 37 checks):** floor -60 dBFS with 0.5 dB meter scatter; true SNR 0 / 3 / 10 / 20 / 30 / 40 dB reads 0.75 / 3.83 / 10.72 / 20.60 / 30.40 / 40.60 dB (error +0.75 / +0.83 / +0.72 / +0.60 / +0.40 / +0.60; the floor is a minimum of block means, so the SNR reads about 0.4 to 0.8 dB high, as the power-log record warned); floor held -60.22 to -59.87 dB; noise only: mean -5.69 dB (never a positive reading). Ring: capacity rounded to a power of two, oldest first, wraps; a reader thread took 372,217 history copies during 2,000,000 pushes with **0 inconsistent samples**; 91 allocations in 2000 pushes (the floor tracker's `std::deque`, about one per second: this is a pass-thread call, not for the audio callback), 0 in the reads.
**Break-it:** history reversed, lap check removed (reader test RED), SNR as a dB difference (0 and 3 dB rows RED, found by tightening the first pin), gate state ignored (floor -50.4 instead of -60), one-slot accounting: each RED.

### 8b S-unit conversion
**Not built: exists.** `core/smeter_cal.hpp` (batch 1): `s9Dbm(f)` (-73 dBm below 30 MHz, -93 dBm from 30 MHz up), `dbmToS`, `formatS` ("S7", "S9+20 dB"), `sToDbm`, calibration table. Its header cites IARU Region 1 Technical Recommendation R.1 (Brighton 1981) and says plainly the recommendation itself was not opened (secondary sources only): UNVERIFIED against R.1; this batch did not change that.

### 8c Constellation and eye tap (`SymbolTap`)
Matched-filtered complex baseband at a given symbol rate (2 to 256 samples per symbol, integer or not); timing from a fold of |x|^2 modulo the symbol period into min(24, floor(sps)) bins with parabolic refinement and a 0.02 sample per symbol slew (manual timing optional); 4-point Lagrange sampling; rings of the newest N points and M eye traces (2 symbols, centred, P points per symbol); `eyeOpening(rail)`.
**Measured (`test_symbol_tap`, 75 checks; RRC beta 0.35 pairs):**

| signal | sps, true timing | timing error | EVM | eye opening |
|---|---|---|---|---|
| QPSK | 8, 0 / 2.6 / 5.3 / 7.9 | +0.04 / -0.01 / -0.06 / +0.02 | 0.63 / 0.72 / 0.75 / 0.65 % | 0.976 to 0.987 |
| QPSK | 9.6, 3.3 | +0.12 | 3.52 % | 0.951 |
| QPSK | 4, 1.4 | -0.05 | 1.74 % | 0.944 |
| BPSK | 8, 4.4 | -0.02 | 0.81 % | 0.980 |
| 16-QAM | 8, 6.1 | -0.06 | 0.98 % | (not a binary rail) |
| QPSK | 16, 11.7 | -0.05 | 0.59 % | 0.968 |

Noise (QPSK, sps 8, sigma per component 0 / 0.02 / 0.05 / 0.1 / 0.2): EVM 0.6 / 1.3 / 2.7 / 5.2 / 10.3 %, eye 0.99 / 0.98 / 0.94 / 0.88 / 0.75, timing within 0.05 of 3.3. Manual timing right: EVM 0.25 %, eye 0.995; a half symbol wrong: EVM 64.6 %, eye 0.001. The newest eye trace's centre equals the newest constellation point; |I| one symbol either side of the centre is within 0.0125 of 1/sqrt(2).
Rings: capacity - 1 readable; 0 allocations in 20 push + snapshot rounds; a reader thread took 625,882 snapshots during 3.2 million samples with no non-finite or out-of-range point (the tear check itself is the SNR tap's sequence test: two QPSK points mixed would still look valid).
**Break-it:** linear interpolation (EVM 2.86 % at sps 4 against 1.74 %), timing estimate ignored, manual timing ignored, eye centre shifted, sps under 2 accepted, flat (unweighted) fold: each RED. (The first draft of the estimator break did not show because the slew corrected it over 3000 symbols; the break was moved onto the estimate.)
**Limits.** No carrier recovery, derotation or decision: the tap shows what the demodulator gives it. The fold-and-peak timing is a measurement heuristic written for this tap (accuracy about 0.1 sample at non-integer sps, which costs about 3 % EVM there); not tested on a real recording or a signal with ISI-free but energy-flat pulses.

### 8d Zoom FFT (`ZoomFft`)
Mix the centre to DC (`Nco`), Blackman-Harris windowed-sinc low-pass of 40 M + 1 taps and decimate by M (`windowedSincLowpass`, `FirDecimator`), Blackman-Harris windowed `ComplexFFT` every N / 2 decimated samples, fftshifted dB frames (a unit on-bin tone reads 0 dB) published with a sequence lock; `toneLevelDb()` sums the main lobe and divides by the window noise bandwidth (2.004 bins).
**Measured (`test_zoom_fft`, 29 checks).** fs 48 kHz, M 64, N 2048: bin **0.366 Hz**, output rate 750 Hz, usable span +-300 Hz. Two equal tones **3.0 Hz apart (8.2 bins): peaks 2.9 Hz apart, valley 95.8 dB down**; the existing `SpectrumEstimator` (2048 points at the full rate, 23.4 Hz bins) on the same data shows one lump (neighbours 3.6 and 0.6 dB down). Level (fs 192 kHz, M 16, N 1024; 60 tones at random offsets within +-0.35 of the output rate, amplitudes 0.1 to 0.9): **main-lobe power-sum error 0.000 dB; peak-bin reading up to 0.824 dB low (scalloping)**; passband flat to 0.000 dB over +-0.4 of the output rate. Alias rejection: unit tones 0.6 to 5 output rates from the centre, both sides: **strongest spur inside the usable span -111.0 dB**. Dynamic range: a tone 80 dB down, 12 bins from a unit tone, reads -79.81 dB. Retune from +3000 to -4000 Hz: new tone at bin 512, level -12.04 dB (true -12.04); frame rate 59 per 5 s (6000 / 512 = 11.72 per second). 0 allocations in 20 push + latest rounds; chunking (4800 against 37) identical frames; cost **36.8 ns per input sample** at M 64 (2561 taps) and 40 complex-by-real multiply-adds per input sample at any M (7.7 MMAC/s at 192 kS/s): at multi-MS/s source rates feed it from a decimated stream.
**Break-it:** no anti-alias filter, rectangular window, mixer sign flipped, no fftshift, noise bandwidth taken as 1, no overlap, a 10 M-tap filter: each RED.

## Part 9: MPX output tap and OIRT notes (read only; nothing built, nothing edited)

**The MPX tap already exists.** `Pipeline::scopeMpx()` (`pipeline.hpp:805`) returns the `ScopeTap<float>` `scopeMpx_` (`pipeline.hpp:1163-1166`, 65536 samples, 0.33 s at 200 kHz), filled by `scopeMpx_.push(audioBuf_.data(), m)` at `pipeline.cpp:2057`, **WFM only**, at the composite rate (`chanRate`, typically 192 to 250 kHz), flat, after `demod_.process` (`:2040`) and before RDS (`:2062`), the `fmScale_` multiply (`:2070`) and `StereoFm` (`:2084`); in WFM `demod_` is told 0 us de-emphasis (`pipeline.cpp:490`), so the buffer is the raw discriminator output. What an **MPX output** (a stream for an external decoder, a sound device or a file) would still need, as read:
- a second consumer of the same instant: a rolling `ScopeTap` is a window, not a stream; an output wants an `SpscRing` (`dsp/spsc_ring.hpp`) written next to line 2057 and drained by a worker thread (the sink side must not run on the DSP thread), plus a decision about the rate (the composite rate is `chanRate`; a sound device needs 192 kHz to carry the 57 kHz RDS band, so the resampler (`pipeline.cpp:2176` area) would be a second instance for the tap);
- the scale: line 2070 applies `fmScale_` to make +-75 kHz deviation 0.5 full scale after the tap; an output should probably be scaled the same way (a decision for the owner);
- a setting and a state word (below), WFM only (the tap is not pushed in other modes on purpose, `pipeline.cpp:2046-2056`).

**OIRT band handling (65.8 to 74 MHz).** What exists: `band_mode.cpp:27` already suggests WFM for 64 to 108 MHz broadcast (its comment names the OIRT, Japanese and CCIR bands), so tuning there picks the WFM demodulator; the WFM channel bandwidth default is `kWfmUnclippedChannelHz` 150 kHz / 0.9 (`pipeline.cpp:135`, `:239`). What it would NOT do, reading `stereo_fm.hpp` (19 kHz pilot PLL, 38 kHz DSB-SC difference) and `rds.cpp` (57 kHz): OIRT stereo is by **polar modulation with a reduced 31.25 kHz subcarrier and no pilot mentioned** (Wikipedia, "FM broadcast band", "OIRT bandplan", secondary, UNVERIFIED against a standard), so `StereoFm` would find no 19 kHz pilot and the receiver would play mono (`pilotLocked()` false, `stereoActive_` false at `pipeline.cpp:2088-2090`): not run here, inferred from the code. Stereo for OIRT would need a NEW decoder (a polar-system demodulator: subcarrier recovery without a pilot) which needs the system's published specification, not read here: **not built**. Other points for the wiring lane: the same article gives a 30 kHz channel grid for OIRT (tuning step; today's steps are per band plan, `band_plan.cpp`, not read in detail), and states no deviation or de-emphasis for it (so a "WFM" default of 75 kHz deviation and 50 us is an assumption: UNVERIFIED). No RDS expectation in OIRT. The band plan (`band_plan.*`) and the frequency list would need an OIRT row; whether the spectrum / tuner front ends support 65.8 MHz depends on the device and was not examined.

## Tests run (this machine, Release, 3 jobs, build dir `D:/foxsdr-build/feat-dsp4`, configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`, toolchain and triplet as `feat-web`)

Build: `cmake --build D:/foxsdr-build/feat-dsp4 --config Release --parallel 3 --target <the 21 test targets>`: 1 min 43 s for all 21 (engine library already built by the first target). Only the engine library and the test executables were built; the application was not built or run; no radio was opened.
Own checks: audio_eq 56, cw_filter 192, noise_squelch 68, afc 38, iq_correct 51, stereo_place 45, cquam 79, snr_tap 37, symbol_tap 75, zoom_fft 29 (670 in all); all 0 failed. Final combined run, all block tests of batches 1, 2 and 4 present in this tree (ctcss, dcs, replay_ring, clip_recorder, smeter_cal, adif, sam, passband, nr_methods, iq_readers, power_log and the ten above), twice:
`ctest -C Release -j3 -R "^(test_ctcss|test_dcs|test_replay_ring|test_clip_recorder|test_smeter_cal|test_adif|test_sam|test_passband|test_nr_methods|test_iq_readers|test_power_log|test_audio_eq|test_cw_filter|test_noise_squelch|test_afc|test_iq_correct|test_stereo_place|test_cquam|test_snr_tap|test_symbol_tap|test_zoom_fft)$"`
-> run 1: `100% tests passed out of 21` (73.6 s); run 2: `100% tests passed out of 21` (61.3 s). The engine-branch block tests of batch 1 (`test_agc_modes`, `test_noise_blanker` and others) are not in this tree and were not run.
Determinism: everything is seeded; the two timing-sensitive tests (`test_snr_tap` and `test_symbol_tap` reader threads) were run in the two ctest runs and alone several times (break-it runs) without a flake; `test_noise_squelch` (13 s) and `test_afc` (4 s) do their time-based work in simulated time, not wall time. Not run 50x.

## Departures from the request, and why

1. **S-unit conversion not built** (exists in `smeter_cal.hpp`); the SNR estimate reuses the existing floor tracker and formula and adds the ring and the mean, not a new estimator (an M2M4-type blind estimator was not built).
2. **Soft channel shape is a Gaussian with a FIXED shape factor of 3.157**, not a Kaiser design for an arbitrary factor: a Kaiser design cannot go softer than about 2.2 (measured). Between 2.0 and 2.6 is not offered.
3. **AFC "SSB-less modes"** is read as modes without a carrier tone; the Peak estimator serves unmodulated and keyed carriers (CW, FSK-less carriers); SSB voice is explicitly not supported by the mean-frequency mode.
4. **C-QUAM encoding formula is a reconstruction** from the words of 47 CFR 73.128 (equations are images there); everything else in the system description is verified.
5. **Part 9:** notes only; the MPX tap exists, an OIRT polar-modulation decoder was not built (no standard read).
6. The first draft of three tests had a weak pin that a break-it exposed and was strengthened (pan click on a zero crossing, ITD compared with the header's own formula, SNR as a dB difference at low SNR); the pitch / width glide of the CW peak filter is NOT shown to be needed by any check (recorded above).

## Owner decisions needed

- **Centre level of a panned receiver.** The constant-power law puts a centred receiver 3.01 dB below today's "copy to both ears". Default makeup 0 dB (law as stated) or +3.01 dB (unchanged level when centred)?
- **Which of Schroeder-style pseudo-stereo and binaural to offer** for SSB / CW (both are built): pseudo-stereo costs 13 ns and is mono compatible; binaural is only meaningful on headphones.
- **AFC default ranges** (pull-in 2500 Hz, hold 5000 Hz, 500 Hz/s) are the author's guesses; pull-in must stay under the channel spacing of the band in use (so AFC never grabs the neighbour): a per-band default?
- **Noise squelch default** (5 kHz band edge, 10 dB quieting, 20 ms open delay, 100 ms tail): and whether the screen shows it as "quieting in dB" (threshold relative to the measured noise-only level) rather than dBFS.
- **C-QUAM:** is it worth shipping on an equation that is a reconstruction until someone with the standard (or an AM stereo recording) confirms the pilot term and the sign convention? The decoder is as good as the generated signal; real-world separation is unknown.
- **I/Q correction on by default?** It is blind and adaptive; a wideband front end with a frequency-dependent imbalance would see only partial correction; offer as a per-source switch, default off?
- **Zoom FFT source rate:** feed from the full source rate (40 MAC per sample) or only from a decimated stream.

## UNVERIFIED list

- Every citation marked UNVERIFIED in the table: Kaiser relations, Schroeder 1958, Woodworth 1938 constants, Windisch / Fettweis and Anttila / Valkama / Renfors, textbook frequency estimators and the FM noise-squelch principle (the behaviours are measured, the formulas not checked against the references).
- The C-QUAM equation and the pilot's place in it; the pilot / program separation of a real station; any real AM stereo signal.
- IARU R.1 itself (carried over from batch 1); the OIRT facts (secondary source).
- All blocks on real signals: nothing here was run on a recording or a radio; all results are on generated signals.
- Denormal behaviour under silence on other CPUs / compilers; flush-to-zero is not set anywhere in these blocks.
- ThreadSanitizer was not run (MSVC build); the sequence-lock reads (SNR tap, symbol tap, zoom frames) are plain-data copies guarded by a sequence check, tested by hammer tests but formally a data race in the C++ model.
- POSIX build of any of these (Windows only here).
- Wiring: every statement below about existing code was read this session and has not been executed in the application.

## WIRING HAND-OFF (what the engine, settings and screen lanes must do; nothing below has been done)

Line numbers are those of this worktree's `pipeline.cpp` at `266926bf`; the multi-radio engine branch moves them, so re-read there.

### 1. Equaliser, high-pass, low-pass (`dsp/audio_eq.hpp`)
- **Engine:** a per-channel `AudioEq` (left and right, or one mono instance before the stereo split) in the audio post-processing block of `Pipeline::processAudioBlock`, after the noise reduction (`pipeline.cpp:2204-2205`) and before the plugin chain and the soft limiter (`:2217-2220`, `:2386`), at the audio rate (48 kHz) like the notch and NR. It is a bit-exact no-op when flat.
- **Ops / state:** `SET_EQ_BASS` / `_MID` / `_TREBLE` (dB), `SET_EQ_GRAPHIC` (band, dB), `SET_AUDIO_HIGHPASS` (0 / 100 / 300), `SET_AUDIO_LOWPASS` (0 / 2400 / 3000 / 6000); settings the same names per receiver; read-back none needed.
- **Screen strings:** "Equaliser", "Bass", "Middle", "Treble", "Graphic equaliser", "100 Hz", "300 Hz", "1 kHz", "2.5 kHz", "5 kHz", "High-pass filter", "Low-pass filter", "Off", "100 Hz", "300 Hz", "2.4 kHz", "3 kHz", "6 kHz", "Reset equaliser".
- Note for the wiring: set flush-to-zero / denormals-are-zero on the DSP thread once (see part 3).

### 2. CW peak filter and channel shape (`dsp/cw_filter.hpp`)
- **Peak filter:** one `CwPeakFilter` per receiver on the audio after the demodulator in CW mode (before the notch at `pipeline.cpp:2198`), `setPitchHz` from the same CW-pitch setting the demodulator uses (batch 3 lane has the CW pitch: do not duplicate the setting, read it), `setEnabled` only in CW mode. Ops: `SET_CW_PEAK` (enable), `SET_CW_PEAK_WIDTH` (Hz), `SET_CW_PEAK_GAIN` (dB); settings `cwPeakEnabled` (false), `cwPeakWidthHz` (100), `cwPeakGainDb` (12).
- **Shape:** where the channel filter is designed (`Vfo::setBandwidthHz` at `pipeline.cpp:499` and `:506`, `vfo.cpp`), call `designChannelFilter(chanRate-or-decimated-rate, bandwidth, shape)` on the control thread and hand the taps to the filter (`FirDecimator` takes taps in its constructor: a swap by pointer hand-off as the recorders do, never on the audio thread). The sharp shape needs more taps at narrow bandwidth: run narrow CW / data filters at a decimated rate, and show `capped` / the measured shape factor if the cap is hit. Ops: `SET_FILTER_SHAPE` (0 sharp, 1 medium, 2 soft); setting `filterShape` (1); read-back the design's measured `shapeFactor`, `taps`, `delaySamples` (latency).
- **Screen strings:** "CW peak filter", "Peak width", "Peak gain", "Filter shape", "Sharp", "Medium", "Soft", "Shape factor", "Latency", "Filter too narrow for this shape at this rate".

### 3. FM noise squelch (`dsp/noise_squelch.hpp`)
- **Engine:** in NFM only, next to `squelch_.process` (`pipeline.cpp:2160`), as a second gate on the audio. The detector wants the discriminator output WITHOUT de-emphasis: in NFM today `Demodulator::process` applies the user's de-emphasis inside the demodulator (`demod.cpp:255-265`) and `audioBuf_` (`pipeline.cpp:2040`) carries it, so either the receiver runs with de-emphasis Off, or `Demodulator` (owned by someone else; read, not edited) exposes the pre-de-emphasis discriminator output. The two squelches combine as AND (open when both say open) or as a selectable mode.
- **Ops / state:** `SET_NOISE_SQUELCH` (enable), `SET_NOISE_SQUELCH_THRESHOLD` (quieting dB, converted with `thresholdForQuietingDb` against a measured noise-only level: the auto-squelch's `NoiseFloorTracker` already measures a floor on the channel power; a noise-only band level needs its own short measurement (`levelDb()` while the squelch is forced closed)), `SET_NOISE_SQUELCH_TAIL` (ms); read-back `noiseSquelchOpen`, `noiseSquelchLevelDb`. Settings `noiseSquelchEnabled` (false), `noiseSquelchQuietingDb` (10), `noiseSquelchTailMs` (100).
- **Screen strings:** "Noise squelch", "Quieting (dB)", "Tail (ms)", "Open", "Closed", "Noise level".

### 4. AFC (`dsp/afc.hpp`)
- **Engine:** `AfcTracker` on `chanBuf_` right after `vfo_.process` (`pipeline.cpp:2011`) at the channel rate; each pass read `correctionHz()` and, when it changed, retune the VFO to `userOffsetHz + correctionHz` with the existing `Pipeline::setOffsetHz` path (`:1288` area, `vfo_.setOffsetHz`) so the displayed tuned frequency stays the user's (the correction is shown separately). `setSignalPresent(squelch_.isOpen())` every block (`:2162`), `setFreezeLevelDb` from the noise floor + a margin (`signalPowerDb()` at `:2018`), a retune by the user resets it (`reset()`), and switching AFC off applies the 0 correction. Mode by demodulator: AM and FM use `MeanFrequency`, CW uses `Peak`; SSB and the noise-like modes: AFC off (not supported).
- **Ops / state:** `SET_AFC` (enable), `SET_AFC_RANGE` (pull-in Hz); read-back `afcCorrectionHz`, `afcLocked`, `afcFrozen`. Settings `afcEnabled` (false), `afcPullInHz` (2500).
- **Screen strings:** "Automatic frequency control", "AFC range", "AFC locked", "AFC holding (no signal)", "Correction (Hz)".

### 5. I/Q correction (`dsp/iq_correct.hpp`)
- **Engine / source:** on the raw complex samples before the spectrum and the audio path both take them (`Pipeline` drain at `pipeline.cpp:1943-1955`: the block is duplicated into `estimator_.process` and `processAudioBlock`): correct once, in place, at the top of the drained block so the spectrum shows the corrected picture too; or per source in the source layer for sources that need it (a file source does not). Construct at the source rate (rebuild on a rate change; the constructor does not allocate beyond its members). `isBypassed()` makes the off case free.
- **Ops / state:** `SET_IQ_DC` (bool), `SET_IQ_BALANCE` (bool), `SET_IQ_SWAP` (bool); read-back `iqImageRejectionDb`, `iqDc` (re, im). Settings per device (`iqDcRemoval` true for devices that need it, `iqBalance` false, `iqSwap` false) saved with the device.
- **Screen strings:** "I/Q correction", "Remove DC offset", "Balance gain and phase", "Swap I and Q", "Image rejection (dB)".

### 6. Stereo placement (`dsp/stereo_place.hpp`)
- **Engine:** one `StereoPlacer` per receiver replacing the plain "duplicate to both channels" non-WFM branch (`pipeline.cpp:2093-2099`) and, for WFM, applying to the decoded stereo pair is NOT meant (use Both with pan 0). Mono in the audio chain (before the EQ at part 1's position, or after it per receiver) producing `outL_` / `outR_` for that receiver; summing receivers is the mixer's (multi-radio engine's) job and must be re-read on that branch. Pseudo / Binaural only for SSB and CW (and AM), selectable.
- **Ops / state:** `SET_PAN` (-1..1), `SET_ROUTE` (0 both, 1 left, 2 right), `SET_STEREO_MODE` (0 pan, 1 pseudo, 2 binaural), `SET_PSEUDO_AMOUNT`, `SET_AZIMUTH`. Settings per receiver `pan` (0), `route` (0), `stereoMode` (0), `pseudoAmount` (0.5), `azimuthDeg` (0).
- **Screen strings:** "Left / right", "Pan", "Left only", "Right only", "Both", "Stereo effect", "Off", "Pseudo-stereo", "Binaural", "Amount", "Position (degrees)", "Binaural is for headphones".

### 7. C-QUAM (`dsp/cquam.hpp`)
- **Engine:** a new demodulation mode (`DemodMode` has eight entries, `demod.hpp:20`; adding `CQUAM` or an AM-stereo flag changes the enum, `kDemodModeCount`, the name table `demod.cpp:13`, `modeFromName`, the config list): in `processAudioBlock`, when the mode is AM and AM stereo is on, feed `chanBuf_` (the channel baseband, `pipeline.cpp:2011-2033`) to `CquamDecoder::process(chanBuf_, m, leftBuf_, rightBuf_)` INSTEAD of `demod_.process` + the copy at `:2093`; the AGC, squelch and the rest then run on the stereo pair as for WFM. The channel bandwidth for AM stereo must carry the +-5 kHz phase information (the AM channel default is narrower: to check, not read). Tuning must be accurate to within the PLL's few Hz or the AFC (part 4, MeanFrequency) must be on.
- **Ops / state:** `SET_AM_STEREO` (off / auto / mono / stereo = `setForce`); read-back `amStereoActive` (`stereo()`), `amPilotLevel`, `amPllLocked`. Settings `amStereo` (auto when the mode is AM stereo).
- **Screen strings:** "AM stereo (C-QUAM)", "Stereo", "Mono", "Auto", "Pilot", "Stereo pilot found", "No stereo pilot".

### 8. Taps
- **SNR tap (`core/snr_tap.hpp`):** one `SnrTap` per receiver in the engine pass beside the S-meter fill, fed with `signalPowerDb()` (`pipeline.cpp:2018`, the engine's own per-pass reading), the pass time and `squelch_.isOpen()`; `reset()` on a retune, bandwidth or mode change (the same events that reset `noiseFloor_`); do NOT also feed `Receiver::noiseFloor_` twice: either share the tracker (the tap owns it) or accept two trackers. Ops / state: read-back `snrDb`, `snrMean5sDb`, history via the existing snapshot path. Screen: "SNR", "SNR (5 s mean)", "dB"; the S-meter keeps `smeter_cal.hpp`.
- **Symbol tap (`dsp/symbol_tap.hpp`):** per decoder that has a symbol rate (the digital plugins; the demod scope's I/Q window is `scopeIq_`, `pipeline.cpp:2019-2033`, and is NOT symbol aligned): the plugin or the host gives the matched-filtered baseband and the symbol rate. Ops: `SET_SYMBOL_RATE`; state: constellation points and eye traces as arrays (the existing plugin UI port or the web API carries arrays: not checked). Screen: "Constellation", "Eye diagram", "Symbol rate", "Timing locked", "Eye opening".
- **Zoom FFT (`dsp/zoom_fft.hpp`):** next to `estimator_` (`pipeline.cpp:1945`) fed from the source-rate block or a decimated stream; the centre follows the tuned frequency or a screen setting; its frames go to the spectrum view as a second trace with a "Zoom" span selector (M and N choices) and a bin-width read-out. Ops: `SET_ZOOM` (enable, span Hz, centre Hz); settings `zoomEnabled` (false), `zoomSpanHz`. Screen: "Zoom spectrum", "Span", "Resolution (Hz per bin)". (The batch-3 lane's spectrum size and window settings share the same display: coordinate the dB scale: this frame's 0 dB is a unit tone, not the existing `SpectrumEstimator`'s window-attenuated reading, `spectrum.hpp` header comment.)

### 9. MPX output and OIRT
- As read above: a stream consumer next to `pipeline.cpp:2057` and a sink; OIRT: a band-plan row and, only if wanted, a new polar-modulation stereo decoder from a standard that has been read. Ops / state if built: `SET_MPX_OUT` (enable, device), `mpxOutRunning`; strings "MPX output", "Composite (MPX) to sound device", "OIRT band (65.8 to 74 MHz): mono only".
