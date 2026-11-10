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
  or the 0.05 ms after one; a sample above `threshold x average` (threshold in dB of magnitude, 6 to 30, default
  15) is a click. Never on a sample under 1e-6 (an all-zero stream has an average of zero) and never before 8
  samples have been averaged. A non-finite sample is a click and is replaced by zero before it reaches the delay line
  or the average.
* A delay line of 0.15 ms (30 samples at 200 kHz) gives the look-ahead: the window is placed on the delayed stream.
  Gain ramps to zero over 0.025 ms, ending 0.05 ms before the click reaches the output; zero through the click and
  0.05 ms after the last click sample (every further click sample extends the window); ramps back over 0.025 ms.
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
margin and the 0.025 ms figure is not a recommendation. Starting values kept: average 20 ms, look-ahead 0.15 ms,
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
   ramp plus pre-guard, 0.075 ms), so the lag is longer than the window needs; kept at the design figure.
5. Disabling the blanker removes the 0.15 ms delay at that instant.
