# AGC choice and noise blanker: signal blocks (lanes A and B)

Branch `mr/agc-nb-blocks` (cut from `engine/multi-radio` at d094501b), 2026-11-10.
Design: `docs/agc-noise-blanker-design.md` (engine-stage4 worktree, untracked), sections 2, 3, 5 and 6.
Scope: `src/dsp`, `src/core/pipeline.*`, `src/source/siggen.*` and tests. Nothing of lane C: no op numbers,
`Engine::applyCommand`, `FoxReceiverState`, config, mock engine, API.md, Slint or strings.

## What was built

### A. AGC (`src/dsp/agc.hpp`, `Pipeline`)

`cascade::dsp::Agc` gains `enum class Mode { Auto, Fast, Medium, Slow, Off }`.

* Auto is the old loop SAMPLE FOR SAMPLE: the Auto branch of `process()` is the old loop body, taken before the
  per-sample loop, the constants are untouched, and Auto is the default. `test_agc_modes` compares it bit for bit
  (not within a tolerance) with the original update rule re-written in the test, on real and complex input,
  including NaN and both infinities.
* Fast, Medium, Slow: a CONSTANT-SLEW loop. The gain moves a fixed number of dB per second: up at 17 dB / 0.1 s,
  0.5 s or 2 s (the 17 dB being a 20 dB step recovered to within 3 dB), down at 4000 dB/s (17 dB in 4.25 ms, inside
  the design's 10 ms). The slews become per-sample factors from the rate of the samples `process()` is given
  (`setSampleRateHz`; the pipeline passes TWICE the channel rate, because the AGC gates the interleaved stereo
  stream, at construction and at every rate rebuild). DEVIATION FROM THE DESIGN TEXT ("the same loop with a quick
  attack and a recovery measured in seconds"): the old loop's speed is `rate * |input|`, so its recovery time depends
  on how loud the signal is and cannot be a time in seconds at every level. The constant-slew loop gives the same
  time at every level and at every channel rate. Cost: a compare and a multiply per sample; floor 1e-3 (-60 dB),
  ceiling the loop's 100 (+40 dB).
* Off: the gain does not move. Entering Off freezes the gain held at that moment (no jump) and reports it as the
  fixed gain; `setFixedGainDb(db)` (0 to 40, NaN lands on 0) then sets it, taking effect at once while in Off.
  `reset()` in Off returns to the fixed gain, so a retune, a demod mode change or a rate rebuild does not turn a fixed
  20 dB into unity. Leaving Off resumes from the held gain, without a pump. Switching between the other modes never
  resets the gain.
* Live readout `gainDb()` (20 log10, floored at -120).

Public signatures (lane C calls these):

```cpp
// src/dsp/agc.hpp
class Agc { public:
    enum class Mode { Auto, Fast, Medium, Slow, Off };
    void setMode(Mode); Mode mode() const;
    void setSampleRateHz(double);   double sampleRateHz() const;
    void setFixedGainDb(float);     float fixedGainDb() const;   // clamp 0..40 (kFixedGainMinDb/MaxDb)
    float gainDb() const;
};
// src/core/pipeline.hpp
void  Pipeline::setAgcMode(cascade::dsp::Agc::Mode);   cascade::dsp::Agc::Mode agcMode() const;
void  Pipeline::setAgcFixedGain(float db);             float agcFixedGainDb() const;   // clamped, live
float Pipeline::agcGainDb() const;                      // applied now, dB, published once per block
```

Pipeline details: `setAgcMode` posts the command and stores a verbatim mirror (like `setNoiseReductionEnabled`);
`setAgcFixedGain` posts and the getter reports the clamped live value, published when the command has run (like
`setNoiseReductionStrength`). ORDER MATTERS for lane C and for the start-up push: set the mode FIRST and the fixed
gain AFTER it, because entering Off freezes the held gain. Mode and fixed gain are mirrored in
`publishParamMirrorsLocked`; the live gain is an atomic stored right after `agc_.process` in the DSP thread.

### B. Noise blanker (`src/dsp/noise_blanker.hpp`, `Vfo`, `SigGen`, `Pipeline`)

`cascade::dsp::NoiseBlanker` (header only, complex baseband at the channel rate, in place allowed):

* Tracks the average MAGNITUDE (20 ms; a cumulative mean for the first 20 ms) of the samples that are not clicks
  or the 0.05 ms after one **[SUPERSEDED: guards are 0.025 ms since the guard change, see "After the guard change"]**; a sample above `threshold x average` (threshold in dB of magnitude, 6 to 30, default
  15) is a click. Never on a sample under 1e-6 (an all-zero stream has an average of zero) and never before 8
  samples have been averaged. A non-finite sample is a click and is replaced by zero before it reaches the delay line
  or the average.
* A delay line of 0.15 ms (30 samples at 200 kHz) gives the look-ahead: the window is placed on the delayed stream.
  Gain ramps to zero over 0.025 ms, ending 0.05 ms before the click reaches the output; zero through the click and
  0.05 ms after the last click sample (every further click sample extends the window); ramps back over 0.025 ms. **[SUPERSEDED: guards are 0.025 ms since the guard change, see "After the guard change"]**
* Stand-down: a ring of 100 one-millisecond buckets counts the samples that WOULD be blanked; above 25% of the last
  100 ms the blanker passes the signal and tracks the average on every sample (so it re-learns the level after a
  strong signal appears); it resumes below 20% (hysteresis: my addition, the design gives only the 25%).
* Readouts: `blankedPercent()` = share of the last 100 ms actually blanked (0 while standing down and while off),
  `standingDown()`.
* Off by default; while off `process()` is not called at all (no delay, no cost). Enabling starts from a clean state
  (a delay line and an average from before the switch-off would blank against the wrong level), so the first
  look-ahead after enabling is silence ramped up; switching off removes the 0.15 ms delay (one tiny step, not a
  click).
* DSP thread: `setSampleRateHz` allocates (construction, rate change); `process` allocates, locks and logs nothing.

`Vfo` hook (`src/dsp/vfo.*`): at the channel rate, in place on the output buffer of stage `noiseBlankerStage()`.

```cpp
// src/dsp/vfo.hpp
void  Vfo::setNoiseBlankerEnabled(bool);        bool  noiseBlankerEnabled() const;
void  Vfo::setNoiseBlankerThresholdDb(float);   float noiseBlankerThresholdDb() const;   // clamped
float Vfo::noiseBlankerBlankedPercent() const;  bool  noiseBlankerStandingDown() const;
std::size_t Vfo::noiseBlankerLookAheadSamples() const;  std::size_t noiseBlankerStage() const noexcept;
// src/dsp/noise_blanker.hpp
class NoiseBlanker { public:
    static constexpr float kMinThresholdDb = 6, kMaxThresholdDb = 30, kDefaultThresholdDb = 15;
    explicit NoiseBlanker(double sampleRateHz = 200000.0);
    void setSampleRateHz(double);  void setEnabled(bool);  bool enabled() const;
    void setThresholdDb(float);    float thresholdDb() const;  void reset();
    void process(const std::complex<float>* in, std::complex<float>* out, std::size_t n);
    float blankedPercent() const;  bool standingDown() const;  std::size_t lookAheadSamples() const;
};
// src/source/siggen.hpp
void SigGen::setImpulses(double rateHz, float levelDb, int widthSamples);   // rate <= 0 or width < 1: off
void SigGen::clearImpulses();
// src/core/pipeline.hpp
void  Pipeline::setNoiseBlankerEnabled(bool);       bool  noiseBlankerEnabled() const;
void  Pipeline::setNoiseBlankerThresholdDb(float);  float noiseBlankerThresholdDb() const;  // clamped, live
float Pipeline::noiseBlankerBlankedPercent() const; bool  noiseBlankerStandingDown() const; // once per block
```

The settings survive `Vfo::setBandwidthHz` and `Vfo::reset`; a rebuilt `Vfo` starts from the defaults, so
`Pipeline::setInputRateHz` copies enabled and threshold across the rebuild (signal state restarts).
`Pipeline::start` resets the blanker's signal state through `Vfo::reset`. Setter pattern as noise reduction:
`setNoiseBlankerEnabled` posts with a verbatim mirror; the threshold posts and the getter reports the clamped live
value. Everything fed from `chanBuf_` hears the blanked signal (meter, scope tap, demod, decoders); the I/Q recorder,
I/Q plugins and patch tap the raw block upstream and are untouched (proved in `test_pipeline_nb`).

`SigGen` impulses: periodic and deterministic (a real click of `widthSamples` samples, amplitude 10^(dB/20), every
`round(fs/rate)` samples, at least one clear sample between), added LAST in `generate()`; chunking invariance is
kept bit for bit and with impulses off `generate()` is byte-identical to before (tested).

## Section 6, "To verify": the answers, from the code

1. Where `Vfo` applies the narrow filter, and from which bandwidth. `Vfo::designStages` (vfo.cpp): `narrow =
   bwRequest < kNarrowBelowChannelFraction (0.25) * channelRate`. A narrow design appends ONE stage with
   decimation 1 (`narrowTaps(channelRate, bwRequest)`) AFTER the channel filter; the anti-alias part before it is
   designed for `max(0.25 * channelRate, 2 * bwRequest)`. So with a narrow bandwidth the stage list is
   `[alias stages..., channel stage, narrow stage]` and the blanker goes on the output of `stages_.size() - 2`;
   otherwise on `stages_.size() - 1`. For a 204.8 kHz channel the narrow stage applies below 51.2 kHz. Below
   `kStagedMinInputRateHz` (4 MS/s) or with a prime decimation the channel filter is ONE windowed sinc at the input
   rate (`factors.size() < 2` path), still followed by the narrow stage when narrow: `[sinc(D), narrow]`, blanker on
   index 0. Pinned in `test_noise_blanker` `testPlacement` for 2.048 MS/s narrow and wide, 200 kHz / decimation 1
   narrow and wide, 10 MS/s / 50 narrow (4 stages, hook at 2) and a bandwidth change that moves it.
2. Channel rate = input rate. `decimFactor_ == 1` gives `factors.size() < 2`, one stage with decimation 1 (a
   filter at the input rate), plus the narrow one when narrow. The blanker sits after the first of them: the
   same place, "after the last decimating stage" being the sole stage. Tested (`Vfo(200000, 1, 3000)` and `150000`).
3. Does the AGC really run in RAW mode. Yes: `Pipeline::processBlock` calls `agc_.process(ilvBuf_...)` with no mode
   test (the only mentions of RAW in pipeline.cpp are the raw I/Q taps and a comment); `DemodMode::RAW` is
   `Re{x}` passthrough, so the loop levels it like any audio. Not forced Off: the mode is the user's, and RAW is a
   diagnostic mode where they may well pick Off or a fixed gain. Recommendation to lane C: leave it free.
4. Recovery times and threshold range: see "Settled values" below.
5. "Whether the live readouts can ride the signal-level path": the AGC gain and the blanked share are published as
   relaxed atomics once per block in the DSP thread, exactly as `signalDb_` is; nothing in the pipeline needs a new
   sequence group. Whether the ENGINE publishes them without bumping the mode sequence is lane C's.

## Settled values and measured figures

All figures are from this tree (Release, MSVC), generated signals only.

AGC, 20 dB step on a constant-magnitude complex input from a settled loop, time to be back within 3 dB of target
(`test_agc_modes`, printed):

| Mode | wanted | 150 kHz channel (fs 300 kHz) | 250 kHz channel (fs 500 kHz) | 20 dB rise pulled back |
|---|---|---|---|---|
| Fast | 0.100 s | 0.1001 s | 0.1001 s | 4.25 ms (both rates) |
| Medium | 0.500 s | 0.5023 s | 0.4976 s | 4.25 ms |
| Slow | 2.000 s | 2.0273 s | 2.0532 s | 4.25 ms |

Level independence (base level 0.05 against 0.4, step -20 dB, Fast and Medium): equal within 5% (asserted).
Steady 1 kHz tone (the detector sees |x| and crosses the target every cycle): output peak 0.501 / 0.500 / 0.500 and
gain ripple 0.090 / 0.026 / 0.013 dB for Fast / Medium / Slow. On the running pipeline (USB, 1 kHz tone stepped
-20 dB, `test_pipeline_agc`): Off ratio 0.100 and the gain readout does not move; Fast ratio 0.995 one second later,
readout +19.98 dB; Slow ratio 0.254 one second later, readout +8.56 dB (17 dB takes 2 s); fixed gain 0 to 12 dB moves
the audio 11.99 dB. Settled and unchanged from the design: 0.1 s / 0.5 s / 2 s and under 10 ms (4.25 ms).

Blanker (fixture: tone 0.003, noise 3e-4 per component, clicks 0.3 wide 3 samples 100 a second at 200 kHz; residual
= power near the tone after removing the tone, `residualPower` in `test_noise_blanker`):

| Case | residual off | residual on | improvement | blanked |
|---|---|---|---|---|
| block, 200 kHz, 3 kHz-wide average | -52.36 dB | -73.31 dB | 20.95 dB | 1.40% |
| after dense then sparse (resume) | | | 20.95 dB | |
| through `Vfo`, 2.048 MS/s, 10 us clicks, 3 kHz narrow filter | -54.71 dB | -71.65 dB | 16.94 dB | 1.51% |
| `Vfo`, 150 kHz (no narrow stage), same clicks | | | 14.97 dB | |
| pipeline, USB audio, LS-fit sinusoid residual vs tone | -6.24 dB | -22.07 dB | 15.83 dB | 1.55% |

Floors pinned in the tests (with margin): block > 15 dB, `Vfo` narrow > 15 dB, `Vfo` wide > 10 dB, pipeline > 10 dB,
resume > 12 dB.
No harm: a clean tone with noise through the blanker equals the input delayed by 30 samples BIT FOR BIT after the
first look-ahead (power difference 0.0000 dB, residual -85.55 dB off and on); through `Vfo` and the narrow filter the
power difference is -0.0000 dB (0.248 dB both); 100% AM at 5 kHz with no impulses: blanked share exactly 0, output
bit-identical to the delayed input; a click of 0.01 on the 0.003 tone (under the 15 dB default) is left alone.
Stand-down: 3000 clicks a second (about 60% of the stream would go) stands down, share reads 0 and the output is
bit-identical to the delayed input; sparse clicks afterwards resume (improvement 20.95 dB again); a 40 dB level step
(0.001 to 0.1) is passed whole after the average catches up, share 0.
Threshold: 15 dB sees a 0.03 click on the 0.003 tone, 30 dB does not.

Tuning honestly: the guards (0.05 ms before and after) were swept on the fixture: 0.025 ms gives 24.2 dB (block) /
19.9 dB (`Vfo`), 0.05 ms gives 20.9 / 16.9, 0.1 ms gives 17.0 / 13.7. Shorter wins on generated clicks because they
are rectangles with no skirts; real clicks ring and have skirts the fixture does not have, so 0.05 ms is kept as the
margin and the 0.025 ms figure is not a recommendation. Starting values kept: average 20 ms, look-ahead 0.15 ms, **[SUPERSEDED: guards are 0.025 ms since the guard change, see "After the guard change"]**
ramps 0.025 ms, threshold 6 to 30 dB (default 15), stand-down 25% of 100 ms. The blanking is a trade, not a free
lunch: it also removes the tone for the length of the window (here 1.4% of the time), so on clicks only a few dB over
the signal it gains little (a 0.042 click, 40 dB over the noise floor, on the 0.003 tone: 3.98 dB, measured on the
block with the same fixture) and on clicks 30 dB or more over the signal it gains 20 dB and more.

## Proofs

Build: `cmake --build D:/foxsdr-build/mr-dsp --config Release --parallel` exit 0 after each commit.

* Commit A (3745e552). Headless gate `ctest -L "headless|slint" -j4 --timeout 300`: 738 tests (736 + the two new)
  with 6 failures, all environmental or load: `test_airband_data`, `test_band_plan`, `test_converter_call_sites`,
  `test_page_resize_id`, `test_png_decode` look for the repository root by walking up from the working directory
  and the mandated build folder `D:/foxsdr-build/mr-dsp` is outside the worktree, so they cannot find it (they fail
  in 0.02 to 0.08 s with `!root.empty()` / `!path.empty()`); run with the worktree as the working directory they pass
  (115, 5174, 5, 22, 49 checks, 0 failed). `test_startup_state_free` failed once under load and passed alone
  (`ctest -R`, 1.87 s).
* Commit B (f2c3ffac). Headless gate: `100%` is not reached, `99% tests passed, 7 tests failed out of 740` (738 + the
  two new): the same five root-finding tests as above (pass with the worktree as working directory), and two load
  flakes that passed alone with `ctest -R`: `test_engine_api_03` (failed in 94 s in the gate, passed alone in 57.38 s)
  and `test_pass_control` (failed at 72.91 s, passed alone in 46.42 s). The five new/affected tests in the gate:
  `test_agc_modes` Passed, `test_pipeline_agc` Passed, `test_noise_blanker` Passed, `test_pipeline_nb` Passed,
  `test_vfo` Passed, `test_siggen` Passed.
* Window gate (hidden desktop, `FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j4`): `99% tests passed, 2 tests failed out of 225`.
  `test_map_open_inside_app` (Exit code 0xc0000409 in the gate) passed alone (19.65 s): load flake. `test_diagnostics`
  failed on a STALE STAMP, not on this change: the binary says `3745e55284f8-dirty` while HEAD had moved to f2c3ffac
  (`git HEAD now: f2c3ffaced4c, binary says: 3745e55284f8-dirty`, the test compares the embedded commit with `git`);
  after a rebuild at the new HEAD it passed (`1/1 Test #305: test_diagnostics Passed 3.36 s`).
* Break-it, AGC: with `const double fs = sampleRateHz_;` replaced by a constant 400000 in a copy of agc.hpp,
  `test_agc_modes` gives `85 checks, 9 failed` (recovery time and rate-independence checks for all three modes);
  the real file was never edited for this. Restored by construction (the copy was discarded).
* Break-it, blanker: with the trigger condition replaced by `false &&` in a copy of noise_blanker.hpp,
  `test_noise_blanker` gives `75 checks, 10 failed` (improvement, blanked share, both stand-down checks, resume, the
  threshold pair and both `Vfo` improvements). Copy discarded.
* Both break-it runs were built with g++ in the scratchpad against a modified copy; the MSVC builds in the tree
  were never touched. The same tests under g++ and MSVC print the same figures.

## Not covered, UNVERIFIED, left for later

* REAL IMPULSE NOISE IS UNVERIFIED. No ignition or fence recording exists in D:\foxsdr-samples and none was
  downloaded; the blanker is proven on generated rectangular clicks only. Release notes must say so (design 3.3).
* No listening test (the "by ear" tuning of the design): the values were tuned on measured figures.
* CPU cost of the blanker was not measured (one sqrt, a few compares and a delay-line copy per channel sample, only
  while on).
* Everything of lane C: ops, `Engine::applyCommand`, `FoxReceiverState`, config, mock engine, API.md, Slint,
  strings, the golden snapshot, the fuzz seed.
* Multi-radio: each `Pipeline` owns its own `Agc` and `Vfo`, so both are per radio by construction; not exercised
  with more than one pipeline here.

## Choices to flag

1. Constant-slew loop for the timed modes instead of "the same loop" (above).
2. Entering Off freezes the HELD gain, which can be below 0 dB; `agcFixedGainDb()` then reports it unclamped until a
   fixed gain is set (the setter clamps to 0..40). Lane C should set the fixed gain after the mode.
3. Stand-down hysteresis (resume below 20%).
4. Look-ahead 0.15 ms as designed, but the window uses 0.05 ms before and after (the look-ahead only has to cover
   ramp plus pre-guard, 0.075 ms), so the lag is longer than the window needs; kept at the design figure. **[SUPERSEDED: guards are 0.025 ms since the guard change, see "After the guard change"]**
5. Disabling the blanker removes the 0.15 ms delay at that instant.

## Real power-line noise (leon2001)

Added 2026-11-10 on the same branch. Tool: `tools/nb_real_noise/main.cpp` (off-line, `EXCLUDE_FROM_ALL`, links `cascade_engine`,
build by name: `cmake --build D:/foxsdr-build/mr-dsp --config Release --parallel --target nb_real_noise`). No product code changed
(`src/` untouched). It feeds the recording block by block through the real `Vfo` (the product's channel path with the blanker hook) at a
chosen offset and bandwidth, and measures power on 1 ms bins of the `Vfo` output (after the narrow filter), from 1 s into each pass.

The recording: `leon2001.wav` (sm5bsz.com, Linrad blanker test file), 2 channels PCM16, 96 kHz, natural power-line pulses. It is
untrusted local test data and is NOT in the tree; the tool takes its path as an argument. Nothing derived from the samples is stored in the
tree except the figures below.

### Facts about the file that differ from the brief

* It is 7,823,360 frames = 81.5 s at 96 kHz (the brief said 163 s; that is the number of frames over 48 kHz).
* The `Vfo` chose decimation 1 (`round(96000 / 200000) = 0`, folded to 1): channel rate 96,000 Hz = input rate. The blanker look-ahead is
  14 samples (0.146 ms); at this rate the 0.05 ms guards (the product value then; since superseded by 0.025 ms) are 5 samples and the 0.025 ms ramp 2 samples (200 kHz in the product gives
  twice as many samples for the same times, so a guard result here is coarse, see caveats).
* The long-term spectrum is flat at about -13 dB (re one unnormalised FFT unit) because the impulses fill every 340 ms frame; the real
  noise floor, found from the 25th percentile of 5.3 ms frames, is about 39 dB lower (-52 dB). The mean power of a 3 kHz channel is 20 dB
  above its median (impulse-dominated); this is a dense, strong impulse recording.

### I/Q orientation: UNDECIDED

The criterion in the brief (the strong signal must sit at +24 kHz) cannot be applied: there is NO strong signal at +/-24 kHz in either
orientation. Evidence (`spectrum`, which prints both orientations): narrow lines of the mean spectrum are at +2183 Hz and -7087 Hz
(with I = left; both mirror with I = right) and nothing near 24 kHz; the quiet-frame spectrum shows lines at DC, -7125, +2250 and
-19688 Hz (8 dB over the floor) and only a faint -24375 Hz line (2.8 dB over the floor), which is +24375 Hz if I is the right channel.
That would favour I = right, but it is the weak line, not a strong signal, and the stronger +/-19.7 kHz line has no counterpart in the
brief, so I did not decide. The blanker figures do not depend on it: the blanker works on magnitude and the filters are real, so the
mirrored run (`--iq rl`, offsets negated) printed identical numbers line for line (diffed). Only the sideband of a USB/LSB audio file
depends on it. Default in the tool: `--iq lr` (I = left).

Chosen channels (all with I = left): QUIET at +28000 Hz (no line within +/-12 kHz, which is what the blanker's own channel filter passes;
a 3 kHz and a 500 Hz bandwidth); SIGNAL at +2183 Hz (the strongest narrow line, drifting 2140 to 2340 Hz; modulation unknown); second
station at -7087 Hz (the stronger of the two by median power in 3 kHz: -69.5 dB against -71.6 dB).

### Command lines

```
nb_real_noise.exe <wav> spectrum
nb_real_noise.exe <wav> measure --quiet 28000 --sig 2183 --strong -7087      (tables 1, 2, 5; add --thr N for another threshold)
nb_real_noise.exe <wav> sweep   --quiet 28000 --sig 2183                    (table 3, and 4 with the guard copy below)
nb_real_noise.exe <wav> cpu     --sig 2183                                  (table 6)
nb_real_noise.exe <wav> audio   --sig -7087 --mode am  --best 24 --out D:/foxsdr-build/mr-dsp/nb-audio --start 20 --dur 30
nb_real_noise.exe <wav> audio   --sig 2183  --mode usb --best 24 --out D:/foxsdr-build/mr-dsp/nb-audio --start 20 --dur 30 --gain <G of the AM run>
```
(run with a working directory outside the tree; all figures here are from the Release build at tip 9b42201d plus the tool.)

### 1 and 2. Default threshold (15 dB), guards 0.05 ms (the product value then; superseded)

Power in dB re full scale squared, 1 ms bins. "Drop" = OFF minus ON.

| Channel | OFF mean / median / p99 | ON mean / median / p99 | drop mean / median / p99 | blanked | stood down |
|---|---|---|---|---|---|
| quiet 3 kHz (+28000) | -65.75 / -86.30 / -58.71 | -72.50 / -90.93 / -59.33 | 6.76 / 4.63 / 0.62 | 16.9 % | 17.6 % |
| quiet 500 Hz (+28000) | -70.64 / -87.95 / -61.51 | -77.30 / -96.12 / -63.50 | 6.66 / 8.17 / 1.99 | 16.9 % | 17.6 % |
| signal 3 kHz (+2183) | -63.27 / -71.63 / -55.42 | -66.83 / -82.86 / -56.79 | 3.56 / 11.23 / 1.37 | 14.5 % | 3.1 % |

(Signal mean) / (quiet 3 kHz mean): OFF 2.48 dB, ON 5.67 dB: a gain in signal-to-noise of 3.20 dB (the signal channel keeps its own power,
the noise channel loses 6.8 dB). A second view, line to the noise beside it in 43 ms frames, medians (`lineStats`): 6.30 dB OFF, 9.53 dB
ON, gain 3.23 dB; weak because the line is modulated and wanders, so it is a cross-check, not the figure.

### 3. Threshold sweep, guards 0.05 ms (the product value then; superseded), quiet 3 kHz channel unless stated

| thr dB | blanked % | stood down % | drop mean | drop median | drop p99 | 500 Hz drop mean / p99 | signal blanked % / stood % | signal mean/quiet mean gain |
|---|---|---|---|---|---|---|---|---|
| 6 | 8.78 | 68.8 | 1.49 | 2.88 | 0.10 | 1.48 / 0.38 | 11.56 / 53.1 | 0.04 |
| 9 | 11.93 | 50.7 | 2.67 | 3.71 | 0.20 | 2.67 / 0.73 | 14.66 / 28.1 | 0.22 |
| 12 | 14.76 | 32.8 | 4.36 | 4.25 | 0.34 | 4.32 / 1.18 | 15.33 / 9.8 | 1.01 |
| 15 (default) | 16.90 | 17.6 | 6.76 | 4.63 | 0.62 | 6.66 / 1.99 | 14.49 / 3.1 | 3.20 |
| 18 | 17.23 | 9.6 | 8.99 | 4.77 | 1.14 | 8.91 / 3.01 | 12.69 / 1.7 | 6.27 |
| 21 | 16.90 | 5.4 | 11.15 | 4.85 | 1.94 | 11.00 / 4.49 | 10.40 / 0.7 | 9.14 |
| 24 | 16.02 | 2.4 | 12.23 | 4.86 | 2.69 | 12.16 / 5.80 | 8.05 / 0.2 | 10.77 |
| 27 (extra) | 13.68 | 0.8 | 8.82 | 4.61 | 1.32 | 8.79 / 3.18 | 5.45 / 0.0 | 7.85 |
| 30 | 3.40 | 0.0 | 1.07 | 1.85 | 0.05 | 1.07 / 0.25 | 0.74 / 0.0 | 0.95 |

Best threshold on this recording (product guards): 24 dB, by the drop of the mean (12.23 dB) and of the 99th percentile (2.69 dB), with
the blanker standing down 2.4 % of the time. The default 15 dB gives 6.76 dB and 0.62 dB and stands down 17.6 % of the time; 6 and 9 dB
stand down most of the time (68.8 %, 50.7 %) and gain almost nothing. 30 dB is nearly blind (the pulses seldom exceed 30 dB over the average).

Why the 99th percentile barely moves: `[diag]` in `measure` (1 ms blocks, stand-down flag per bin): at 15 dB, 96.8 % of the strongest 1 % of
the ON bins fell inside a stand-down, i.e. the strongest bursts are the ones the 25 % rule hands through. The blanker blanks about 17 % of the
stream here (a 5-sample guard on each side of every pulse), so dense bursts cross the 25 % line of the last 100 ms.

### 4. Guards: 0.025 ms against 0.05 ms

The guards are private `static constexpr` members, so this is a COPY of `noise_blanker.hpp` in a scratch build, not the tree:
`D:/foxsdr-build/mr-dsp/nb-guard/` (own CMake project, `copy025/dsp/noise_blanker.hpp` with `kPreGuardS` and `kPostGuardS` = 0.000025, put
first on the include path; the tool is compiled together with `src/dsp/vfo.cpp` and linked to the same `cascade_engine.lib`; `copy050/` is a
byte copy of the real header and its build `nb_guard050.exe` printed `measure` output identical to the in-tree tool, so the scratch path
reproduces the product). Same file, channels and sweep:

| thr dB | blanked % | stood down % | drop mean | drop median | drop p99 | 500 Hz drop mean / p99 | signal mean/quiet mean gain |
|---|---|---|---|---|---|---|---|
| 6 | 13.64 | 45.3 | 3.34 | 4.43 | 0.28 | 3.31 / 0.87 | 1.24 |
| 9 | 16.64 | 20.2 | 6.54 | 5.22 | 0.60 | 6.52 / 1.92 | 3.21 |
| 12 | 16.94 | 4.7 | 11.94 | 5.34 | 2.37 | 11.94 / 5.41 | 8.23 |
| 15 | 15.06 | 1.7 | 15.62 | 5.07 | 21.39 | 15.51 / 18.11 | 12.07 |
| 18 | 13.57 | 0.1 | 21.83 | 4.90 | 23.91 | 21.86 / 25.08 | 19.09 |
| 21 | 12.19 | 0.0 | 22.88 | 4.69 | 23.24 | 22.83 / 24.57 | 20.85 |
| 24 | 10.85 | 0.0 | 18.28 | 4.36 | 21.60 | 18.33 / 22.35 | 16.81 |
| 27 | 8.55 | 0.0 | 9.23 | 3.67 | 1.39 | 9.26 / 3.45 | 8.25 |
| 30 | 0.74 | 0.0 | 0.35 | 0.62 | 0.01 | 0.35 / 0.08 | 0.22 |

With 0.025 ms the blanker stands down on only 1.7 % at 15 dB, and the 99th percentile falls 21 dB; the best threshold is 21 dB (22.88 dB
mean, 23.24 dB p99). So on this recording the guard length is a larger lever than the threshold: the 0.05 ms guard makes the would-be
blanked share cross the 25 % stand-down line. This is a measurement on one recording at 96 kHz (2-sample guards); the generated-click
record above preferred the shorter guard too (24.2 against 20.9 dB) with the opposite worry that real clicks have skirts. Not applied in that task; applied afterwards,
see "After the guard change".

### 5. Harm check (15 dB, product guards)

The mean of a station's channel is mostly impulse power, so its change is not a measure of harm. The harm figure is the station level on
the 1 ms bins in which the QUIET channel (blanker off) shows no impulse (under 4x its own median; 61.9 % of all bins), ON against OFF:

| Station (3 kHz) | OFF mean / median / p99 | ON mean / median / p99 | blanked | stood down | level change on impulse-free bins | of those bins, lost over 3 dB |
|---|---|---|---|---|---|---|
| -7087 Hz (strongest by median) | -64.38 / -69.47 / -57.15 | -69.08 / -82.97 / -59.89 | 14.7 % | 3.6 % | -0.008 dB | 3.63 % |
| +2183 Hz | -63.27 / -71.63 / -55.42 | -66.83 / -82.86 / -56.79 | 14.5 % | 3.1 % | -0.061 dB | 2.78 % |

(Mean change -4.70 dB and -3.56 dB: that is the impulse power removed from their channels, not the stations. The 3 % of bins that lose over
3 dB are the neighbours of blanked windows and the 0.15 ms delay shifting a 1 ms bin edge.)

### 6. CPU cost (best of five, OFF and ON interleaved so both meet the same background load)

| Measurement | OFF | ON | blanker adds |
|---|---|---|---|
| A. recording through `Vfo(96000, 1, 3000)` (channel rate 96 kHz), per input sample | 198.55 ns | 202.67 ns | 4.12 ns (2.1 %) |
| B. synthetic 2.4 MS/s through `Vfo(2400000, 12, 3000)` (channel rate 200 kHz), per input sample | 47.18 ns (11.32 % of one core at 2.4 MS/s) | 47.57 ns (11.42 %) | 0.39 ns = 0.094 % of one core |
| C. `NoiseBlanker::process` alone, per channel sample | | | 4.18 ns on the recording (many blanked samples), 3.20 ns synthetic |

At the product's geometry (2.4 MS/s, 200 kHz channel) the blanker costs 0.06 to 0.09 % of one core; even at a 2.4 MHz channel (decimation 1)
it would be 1.0 %. Two earlier runs of this table, taken while other sessions were loading the machine and with OFF and ON in separate
loops, were inconsistent (one had ON faster than OFF) and were discarded; the interleaved figures agree with C.

### 7. Audio for listening (outside the tree)

`D:\foxsdr-build\mr-dsp\nb-audio\`: `am_off.wav`, `am_on15.wav`, `am_on24_best.wav` (station at -7087 Hz, AM, 10 kHz channel) and
`usb_off.wav`, `usb_on15.wav`, `usb_on24_best.wav` (station at +2183 Hz, USB, 12 kHz channel as the pipeline gives a sideband mode 4 x 3 kHz).
30 s from t = 20 s, 48 kHz mono PCM16, ALL SIX with the same gain 192.555 (the loudest file, `am_on24_best`, peaks at 0.9; the Vfo ->
product `Demodulator` -> `RationalResampler(1, 2)` chain, no AGC). `Pipeline` was not used: it needs a source and opens a real audio
stream. The modulation of the two stations is not known (the AM or USB choice is a guess), and the sideband of the USB file depends on the
undecided I/Q orientation.

### Caveats and UNVERIFIED

* One recording, 96 kHz, 81.5 s; its pulses are dense and strong, one kind of noise. The figures say nothing about ignition or fence noise.
* The best threshold and the guard preference are from one recording and one channel geometry (96 kHz; the product runs the blanker at about
  200 kHz where the guards are twice as many samples). Not tuned in the product (superseded: the guards were then shortened, see "After the guard change").
* Power drops are measured on the `Vfo` output after the narrow filter, on 1 ms bins; blanked and stand-down shares are the blanker's own
  100 ms readouts sampled every 10 ms of signal.
* The audio files were not listened to by the author of this section.
* The quiet channel was picked by a spectrum search with the median spectrum and by eye from the quiet-frame spectrum, not optimised.

### After the guard change (0.025 ms)

Decision of the orchestrator, 2026-11-10: the product guards before and after a click are now 0.025 ms (`kPreGuardS`, `kPostGuardS` in
`src/dsp/noise_blanker.hpp`), each never under `kMinGuardSamples` = 2 samples at any channel rate. Before, the pre-guard was
`round(0.05 ms x rate)` (0 samples under 10 kHz) and the post-guard the same with a floor of 1 sample. Unchanged: default threshold 15 dB,
look-ahead 0.15 ms (the 14 samples at 96 kHz), ramps 0.025 ms, stand-down 25 % and resume 20 %. Every statement above that gives 0.05 ms
as the product value is superseded by this change (marked in place, history kept).

Generated-click figures (`test_noise_blanker`, `test_pipeline_nb`; the pins only moved UP, by the same margin the first lane used):

| Case | improvement before (0.05 ms) | after (0.025 ms) | blanked before / after | pin before -> after |
|---|---|---|---|---|
| block, 200 kHz | 20.95 dB | 24.23 dB | 1.40 / 0.90 % | 15 -> 18.0 |
| resume after stand-down | 20.95 dB | 24.22 dB | | 12 -> 15.2 |
| `Vfo`, 2.048 MS/s, 3 kHz narrow | 16.94 dB | 19.90 dB | 1.51 / 1.02 % | 15 -> 17.9 |
| `Vfo` wide, 150 kHz | 14.97 dB | 16.80 dB | | 10 -> 11.8 |
| pipeline, USB audio | 15.83 dB | 18.93 dB | 1.55 / 1.05 % | 10 -> 13.0 |

All other checks were green unchanged (`test_noise_blanker` 75 checks, 0 failed; `test_pipeline_nb` 26 checks, 0 failed; the no-harm
figures are the same: power difference -0.0000 dB). Break-it, on a COPY of the header in a scratch build (`D:/foxsdr-build/mr-dsp/nb-break/`,
trigger condition replaced by `false &&`): `test_noise_blanker: 75 checks, 10 failed`, as in the first lane.

Real noise, in-tree tool, same file, channels and commands as above (the sweep is line for line identical to the scratch-build table 4 and
`measure` equals the earlier scratch `nb_guard025` output):

| Channel (15 dB) | OFF mean / median / p99 | ON mean / median / p99 | drop mean / median / p99 | blanked | stood down |
|---|---|---|---|---|---|
| quiet 3 kHz | -65.75 / -86.30 / -58.71 | -81.37 / -91.36 / -80.11 | 15.62 / 5.07 / 21.39 | 15.06 % | 1.7 % |
| quiet 500 Hz | -70.64 / -87.95 / -61.51 | -86.15 / -96.81 / -79.63 | 15.51 / 8.87 / 18.11 | 15.06 % | 1.7 % |
| signal 3 kHz (+2183) | -63.27 / -71.63 / -55.42 | -66.82 / -82.87 / -56.80 | 3.55 / 11.24 / 1.37 | 9.58 % | 0.2 % |

Signal-to-noise gain, (signal mean)/(quiet mean): OFF 2.48 dB, ON 14.55 dB, gain 12.07 dB (3.20 dB at 0.05 ms).

| thr dB | blanked % | stood down % | drop mean | drop median | drop p99 | signal mean/quiet mean gain |
|---|---|---|---|---|---|---|
| 12 | 16.94 | 4.7 | 11.94 | 5.34 | 2.37 | 8.23 |
| 15 (default) | 15.06 | 1.7 | 15.62 | 5.07 | 21.39 | 12.07 |
| 18 | 13.57 | 0.1 | 21.83 | 4.90 | 23.91 | 19.09 |
| 21 | 12.19 | 0.0 | 22.88 | 4.69 | 23.24 | 20.85 |
| 24 | 10.85 | 0.0 | 18.28 | 4.36 | 21.60 | 16.81 |

(The full sweep 6 to 30 dB is table 4 above.) With the new guards the best threshold on this recording is 21 dB (22.88 dB mean, 23.24 dB
p99); 15 dB gives 15.62 / 21.39 dB, against 6.76 / 0.62 dB with the old guards, and the default threshold is kept as ordered. At 15 dB the
strongest 1 % of the ON bins are still 85.7 % inside stand-downs, but stand-down is now 1.7 % of the time instead of 17.6 %.

Harm check at 15 dB (impulse-free bins, as in table 5): station -7087 Hz level change -0.008 dB (blanked 9.8 %, stood down 0.1 %, 3.75 % of
those bins lost over 3 dB); station +2183 Hz -0.056 dB (blanked 9.6 %, stood down 0.2 %, 2.81 % lost over 3 dB). No more harm than with 0.05 ms.

Audio, `D:\foxsdr-build\mr-dsp\nb-audio\` (the old guards' files are in `guard-0.05ms\`): `am_off`, `am_on15`, `am_on24_best`,
`am_on21_best` (station -7087 Hz, AM) and the same four `usb_*` (station +2183 Hz, USB); 30 s from t = 20 s, one gain for all twelve,
193.042 (0.9 over the loudest file of the first AM run, `am_on24_best`, which peaks at 0.90; no file clips). Not listened to by the author.

UNVERIFIED after the change: one recording only; at 200 kHz (the product's channel rate) the 0.025 ms guards are 5 samples, not the 2
samples measured here, and real clicks with skirts may want more; no listening test; the CPU table above was not repeated (a guard change
does not alter the per-sample work).
