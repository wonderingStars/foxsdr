# Lane `dspmodels` - new-file model blocks for the display, the patch canvas and the airband monitor

Branch `feat/dspmodels` (from `engine/multi-radio` d3630d78). NEW FILES ONLY: 14 headers under `src/dsp/` and `src/core/`, 12 tests, 2 test helpers.
Nothing in `engine.cpp`, `pipeline.cpp`, `receiver.hpp`, `demod.cpp`, `plugin_abi.h`, `fox_engine_api.cpp`, the core recorder, `patch_graph.hpp`,
`airband_monitor.hpp`, `trace_hold.hpp` or any `.slint` file was edited, and `tests/CMakeLists.txt` was not touched either (tests are globbed;
the longest of mine runs 4.3 s, so none needs a timeout). Nothing is wired: every block is a model with a hand-off at the end of this record.
No version string changed (none old to new).

## 0. STEP 0 TABLE (reading only, done before any code)

Sibling branches read: `feat/signal-blocks-3` (colour maps, notch bank, spectrum block, passband), `feat/signal-blocks-4` (equaliser, high/low-pass,
noise squelch), `feat/screen-b` (colour maps, Display body). Neither signal-blocks branch is in this lane's tree (they are unmerged), so two files
were copied UNCHANGED from them so that my blocks can reuse them: `src/core/colour_maps.hpp` (identical blob `9b658160` on `feat/screen-b`
and `feat/signal-blocks-3`) and `src/dsp/audio_biquad.hpp` (identical blob `e754c133` on `feat/signal-blocks-4`). Identical blobs add cleanly at merge.

| Row | Class | What was found |
|---|---|---|
| Display: averaging choice (linear, RMS, exponential, smoothing, de-comb) | new file block | `core/trace_hold.hpp` has the power mean over a time window (that IS "RMS"; mine reproduces it as `Rms`). `feat/signal-blocks-3` `dsp/spectrum_block.hpp` has an `Average` mode that is an exponential blend with a per-frame weight `alpha` (its header line 117, read) plus peak and min hold: no linear, no log, no smoothing, no de-comb, and its exponential is frame-rate dependent. Built: `src/dsp/trace_average.hpp` (`TraceAverager`: Linear, Rms, Log, Exponential by time constant; `smoothMovingAverage`; `SavitzkyGolay`; `DeComb`). |
| Display: user-editable waterfall gradient (marker editor) | new file block (model); the editor strip itself is screen work | Colour maps exist (`core/colour_maps.hpp`, five OKLab maps, `colour::Table` = 256 x `Rgb8`, read by lane B's compositor through `colourAt()`). No gradient or stop editor exists on `feat/screen-b` (searched `src/core`, `src/slint`, `ui/*.slint`: only unrelated hits). Built: `src/core/gradient_model.hpp`; how the formats join is in section 3. |
| Display: zoom to a dragged selection | new file block | Searched `feat/screen-b` for a rubber-band zoom: none (its "Band zoom" row zooms to a band, `view_follow.hpp` follows the tuner; `signal-blocks-4` `zoom_fft.hpp` is a DSP zoom, not view maths). Built: `src/core/zoom_select.hpp`. |
| Patch canvas: audio mixer node with balance and gain | new file block | `patch_graph.hpp:342` refuses audio fan-in on purpose (`mergeable` is only Text). `signal-blocks-4` `stereo_place.hpp` (its commit title: pan, routing, pseudo-stereo, binaural; not opened) is a placement block for one signal, not an N-input mixer; no mixer exists in `src/core` or `src/dsp` (searched). Built: `src/dsp/audio_mixer.hpp`. |
| Patch canvas: undo/redo, copy/paste, multi-select, bypass | new file block | Searched the patch code (`src/core/patch_*.hpp`, `src/slint/patch_*.cpp`, `src/gui/patch*.hpp`, `patch*.slint`) for undo, redo, copy of nodes, multi-select and bypass: the only hit is a "previous patch" slot that undoes a whole preset LOAD (`patch_logic.cpp:2135`), no edit history, no node copy, no multi-select, no bypass. Built: `src/core/command_stack.hpp`, `src/core/graph_edit.hpp`. |
| Patch canvas: signal-generator and scope nodes (partial) | new file blocks (read first, as the brief says) | `source/siggen.hpp` (hardware-free radio) has tone slots, one noise floor and impulses: no sweep, AM or FM. `gui/demod_scope.hpp` has a trigger, but it is display-side on a finished block and lives in `cascade_gui`. Built: `src/dsp/siggen_model.hpp` (adds sweep, AM, FM, phase-continuous, settings text) and `src/dsp/scope_model.hpp` (streaming trigger/decimation/ring in the engine library). Neither touches the existing code. |
| Patch canvas: IF filter node with edges and notches kept on the RF frequency | new file block, partly overlapping a sibling | `feat/signal-blocks-3` `dsp/passband.hpp` (read) is "IF shift and passband tuning": a linear-phase complex FIR band-pass whose edges move click-free, edges relative to the tuned frequency. It does not anchor edges in RF Hz, has no notches and no retune function. Built a different, cheaper design (rotate, 4th-order Butterworth, rotate back) with the RF anchoring, eight notches and `retune()`: `src/dsp/if_filter.hpp`. The wiring lane may prefer `PassbandFilter` for the band part (its taps would then be re-targeted on retune); I did not test that combination (UNVERIFIED). |
| Airband: output per channel (file or stream per frequency) | new file block | Nothing per channel exists. `net/udp_audio.hpp` (`UdpAudioSender`, `udpAudioPayload`) is reused for the stream; no existing file writer takes a gated per-channel stream. Built: `src/core/airband_writer.hpp` (`ChannelWavWriter`, `LoopbackAudioSink`). |
| Airband: squelch per frequency and an SNR squelch | new file blocks | `dsp::Squelch` already takes a threshold per instance - the monitor passes ONE `squelchDb` to all (`airband_monitor.hpp:343`). `signal-blocks-4` `noise_squelch.hpp` is an FM noise squelch (out-of-band noise), a different thing. Built: `src/dsp/snr_squelch.hpp` and the per-channel config/gate in `src/core/airband_channel.hpp`. |
| Airband: high-pass, low-pass and notch per channel | new file block, partly overlapping siblings | `signal-blocks-4` `audio_eq.hpp` has a switchable high/low-pass but with fixed choices (Off/100/300 Hz and Off/2400/3000/6000 Hz); `signal-blocks-3` `notch_bank.hpp` has ten audio notches with glide and fade. A channel needs arbitrary frequencies, so the chain is built on the shared `audio_biquad.hpp` sections: `ChannelAudioChain` in `src/core/airband_channel.hpp`. The wiring lane may swap `NotchBank` in per channel for its glide and fade (not tested together, UNVERIFIED). |

No row is blocked and none needs a forbidden file to be BUILT. Every row needs a forbidden file (or a `.slint` file) to be WIRED; each such hand-off
is in section 6 as the exact change.

## 1. What was built

| File | What |
|---|---|
| `src/dsp/trace_average.hpp` | `TraceAverager` (Off, Linear = mean amplitude, Rms = mean power, Log = mean dB, Exponential in power with `alpha = 1 - exp(-dt/tau)`), `smoothMovingAverage`, `SavitzkyGolay` (order 0..8, odd window 3..101, coefficients solved from the normal equations, edges fitted), `DeComb` (OUR OWN definition, stated in the header: 33-bin running-median baseline, auto-correlation period search, per-phase median profile, subtract; optional DC-spike repair; bit-exact bypass when no comb is found). |
| `src/core/zoom_select.hpp` | `zoomToSelection` (rectangle in plot pixels to centre/span/dB range; click = no zoom; thin drag = one axis; minimum span and dB range; clamped inside the full range), `zoomOut`, `ViewHistory` (16 deep). |
| `src/core/gradient_model.hpp` | `Gradient` (2..32 stops, ends fixed at 0 and 1, 1/1000 minimum gap, positions in 1/10000), three interpolations (sRGB, linear light, OKLab), `GradientEditor` (add, remove, move, colour, reverse, pick), `validate`, `toTable` (the compositor's own `colour::Table`), `fromMap`, strict text form `FOXGRAD1|...`. |
| `src/dsp/audio_mixer.hpp` | `AudioMixer`: up to 16 inputs, gain -90..+24 dB, balance, mute, solo, master, optional soft limiter, stereo and mono outputs, 5 ms gain ramps, no allocation. Laws stated in the header. |
| `src/dsp/if_filter.hpp` | `IfFilter`: complex band-pass with edges in RF Hz, eight notches in RF Hz, `retune()`, 10 ms cross-fades, span clamp. |
| `src/dsp/siggen_model.hpp` | `SignalGenerator`: Tone, Noise (own xoshiro256** + Box-Muller), Sweep (linear/log, up/up-down), AM, FM, extra noise floor, settings struct, strict text form `FOXGEN1;...`. |
| `src/dsp/scope_model.hpp` | `ScopeModel`: streaming trigger (edge, level, hysteresis, hold-off, pre-trigger), Auto/Normal/Single, min/max decimation, frame copied out of the ring, `pushIq` (I, Q, magnitude, phase). |
| `src/core/command_stack.hpp` | `CommandStack`: redo/undo steps, refusal, nested groups, merging of drags, depth cap 1..10000, saved mark. |
| `src/core/graph_edit.hpp` | `IEditableGraph` (abstract) and `GraphEditor`: undoable add/remove/connect/disconnect/move/set/bypass/paste, copy/paste with new ids and preserved internal links (atomic paste), selection (replace/add/toggle/rectangle/all), `effectiveWires()` (bypass splicing). |
| `src/dsp/snr_squelch.hpp` | `SnrSquelch`: noise-floor estimator (own definition), open/hysteresis/hang, learning period, 5 ms ramp. |
| `src/core/airband_channel.hpp` | `AirbandChannelConfig` (+ validate, sanitise, strict text form `FOXAIRCH1|...`), `ChannelAudioChain` (high-pass, low-pass, 3 notches), `ChannelSquelch` (Level via the existing `dsp::Squelch`, or Snr), `ChannelProcessor` (with a pre-gate tap). |
| `src/core/airband_writer.hpp` | `ChannelWavWriter` (WAV per channel, UTC day folder, size cap, pre-roll and tail, never outside the recordings root) and `LoopbackAudioSink` (UDP to 127.0.0.1 only). |
| `src/core/colour_maps.hpp`, `src/dsp/audio_biquad.hpp` | copied unchanged from `feat/screen-b` / `feat/signal-blocks-4` (identical blobs). |
| `tests/dspmodels_util.hpp`, `tests/dspmodels_alloc.hpp` | helpers (seeded noise, tones, a real FFT frame; an allocation counter). |

## 2. Tests (real output; `ctest -C Release` in `D:/foxsdr-build/en-dspmodels`)

All twelve pass, run together: `100% tests passed, 0 tests failed out of 12` (9.7 s), and six of them repeated ten times with `--repeat until-fail:10`
(`100% tests passed out of 6`, 77.8 s). Check counts: trace_average 4347, zoom_select 100081, gradient_model 46654, audio_mixer 16279, if_filter 2057,
siggen_model 5568, scope_model 188, command_stack 65, graph_edit 295969, snr_squelch 72058, airband_channel 18347, airband_writer 376 - all `0 failed`.

Measured values, quoted from the tests' own output:

```
## test_trace_average
alternating 0/-20 dB, window 2: linear -5.1927 (pin -5.1927), rms -2.9671 (pin -2.9672), log -10
one noise frame: mean dB -2.460 (pin -2.507), mean-amplitude -1.026 dB (pin -1.053), mean power 0.005 dB (pin 0)
rms / linear / log over 64 noise frames: -0.036 / -1.069 / -2.508 dB (pins 0 / -1.053 / -2.507)
rms 16 frames: relative power spread 0.2510 (pin 0.25)
exponential after 1 s, tau 0.25 s: 100 fps -15.5079 dB, 20 fps -15.5079 dB
SG centre rows 5/2 and 7/2 match (-3,12,17,12,-3)/35 and (-2,3,6,7,6,3,-2)/21 to 1e-9
SG 9/3 on a cubic: worst error 7.63e-06 dB (edges included)
SG 5/2 on white noise: variance ratio 0.4905 (pin 0.4857)
peak 40 dB sigma 3 bins: SG 11/4 keeps 39.72, moving average 11 keeps 25.55
de-comb (16-frame noise): period found 64, spur excess 18.19 dB -> 0.01 dB, carrier -75.00 -> -75.00 dB
de-comb (single-frame noise): period found 64, spur excess 20.58 dB -> 1.29 dB, carrier -75.00 -> -75.00 dB
de-comb on real FFT frames: period 256, spur excess 33.42 dB -> 0.67 dB
## test_zoom_select
selection 250..500 x 100..300 px on 2 MHz / 100 dB: centre 99750000.0 Hz, span 500000.0 Hz, -25.00 .. -75.00 dB
20000 random drags: 19214 applied, every result inside the limits
## test_gradient_model
black->white at 0.5: srgb 128, linear-light 188, oklab 99 (pins 128, 188, 99)
stock maps copied to 17 stops: worst OKLab distance 0.0173 / 0.0456 (Ember) / 0.0123 / 0.0153 / 0.0103
fuzz: 40000 mutated gradient strings (749 accepted, all valid) and 5000 random byte strings
## test_audio_mixer
gain accuracy: -60 .. +20 dB steps within 0.001 dB; constant-power pan worst |gL^2 + gR^2 - 1| = 2.22e-16
8 uncorrelated noises: output RMS 0.1401 (pin 0.1414); mute mid-tone: largest sample step 0.0653 (tone slope 0.0654, instant mute would give ~0.5)
## test_if_filter
band 7.100-7.103 MHz: middle -0.00 dB, both edges -3.01 dB, 1.5 kHz outside -24.18 dB (Butterworth 4th order -24.1), far below -72.44 dB; mirror side -38.40 dB
notch 100 Hz wide: centre -57.4 dB, +50 Hz -3.00 dB, +1 kHz -0.006 dB
after a 20 kHz retune: in-window tone -0.002 dB (before -0.002), low edge -3.01 dB
retune of a continuous tone: modulus 1.0000 .. 1.0000, largest sample step 1.5461 (own step 1.7671)
edge carried past the span: edges now 39200 .. 42200 Hz (limit +-43200), modulus 0.9989 .. 0.9989, largest one-sample change 0.00000
## test_siggen_model
noise -30 dBFS asked: measured -29.996 dB; linear sweep -8..+12 kHz in 0.5 s: worst deviation of the instantaneous frequency 0.42 Hz
AM depth 0.6: carrier 0.50119, sidebands 0.15036 / 0.15036 (pin 0.15036)
FM beta 2.4048: lines k=-3..3 match |J_k| within 5.16e-09 (J0 = -2.19e-12, J1 = 0.5191)
40 settings changes in 13320 samples: largest sample step 0.4796 (bound 0.4796)
## test_scope_model
rising trigger at level 0: sample before -0.0517, at the trigger 0.0076
chattering edge, 40 cycles, window 8: frames with hysteresis 0 = 160, with 0.2 = 40
## test_graph_edit
random edit test: 300 sequences, 51992 single ops (15150 refused), 6631 groups, 186 merged drags, 6513 undos and 864 redos checked step by step
## test_snr_squelch
noise only at -60 dB for 60 s: floor -60.06 dB, power -60.21 dB, worst SNR 0.38 dB, ever open: 0
carrier +15 dB over the floor: opened after 0.030 s; carrier removed: closed after 0.440 s (hang 400 ms)
60 s carrier +15 dB: floor moved 3.03 dB (limit 3.0), lowest SNR 11.97 dB, gate lost: 0
real noise rise of +6 dB: the floor followed in 5.5 s (rate 1 dB/s), gate open: 0
gate opening: largest audio step 0.0042 (a 5 ms ramp is 0.0042)
## test_airband_channel
channel text fuzz: 40000 mutated strings (6571 accepted, all valid and exact on re-serialise), 5000 random byte strings
high-pass 300 Hz order 4: 150 Hz -24.10 dB (pin -24.1), 300 Hz -3.01 (pin -3.01), 3 kHz 0.000
notch 1 kHz Q 30: centre -258.6 dB, 1.2 kHz -0.035 dB
same signal, two thresholds: -60 dB channel open 1, -30 dB channel open 0, meter reads -39.8 dB
## test_airband_writer
wav burst test: file 110444 bytes, header data size 110400, pre-roll 4800 + burst 24000 + tail 9600 samples in the first burst
size cap 1 MB: 3 files for 3000000 bytes of audio, none over the cap, no sample lost
loopback stream: 20 datagrams for 9600 samples (packets sent 20)
```

Things the numbers say that are not obvious:
- Linear averaging reads noise 1.05 dB low and Log averaging 2.51 dB low against the true mean power; Rms reads true. That is what the closed forms
  predict and what each mode's name means, and it is the reason the three are offered side by side.
- The generated signals carry noise and a sample-rate error (25 ppm in the IF filter and gate tests); the notch depth after a 20 kHz retune is
  -41 dB rather than -57 dB only because that error detunes a -17 kHz tone by 0.4 Hz against a 100 Hz notch (said in the test).
- The one-pole SNR squelch learned the floor to -60.06 dB for a true -60 dB and never opened in 60 s of noise.

## 3. How the gradient model joins lane B's compositor

`feat/screen-b` (`docs/multi-radio-records/screen-b.md` S7, and `src/core/colour_maps.hpp`, read) paints the newest waterfall line through a
`colour::Table = std::array<colour::Rgb8, 256>` with `colourAt(table, x01)`, and draws the strength key as a 256 x 1 picture of the same table.
`gradient::toTable()` returns exactly that type, entry `i` being the gradient at `i/255`, so a user gradient is one more `Table` for the
compositor: no new format on its side. `serialise()` is the string for a config field. Lane B's window, as read, has no "custom" entry and its
Display settings are not persisted (its own record, item 5); adding the entry and the field is screen work (section 6, row 3).
`fromMap(MapId, n)` samples a stock map into stops as the starting point for editing (17 stops stay within 0.05 OKLab of the stock table).

## 4. Break-it table (the behaviour removed, red seen, restored, green seen; each run through `dspmodels-breakit.sh`: edit, build, ctest, restore, `touch`, build, ctest)

| Feature | File | Mutation | Result |
|---|---|---|---|
| averaging | trace_average.hpp | Rms back-conversion 10 log10 -> 20 log10 | red, green |
| de-comb | trace_average.hpp | profile never subtracted | red, green |
| zoom to selection | zoom_select.hpp | top dB computed from the bottom plus the offset | red, green |
| gradient editor | gradient_model.hpp | linear-light mix without the sRGB decode | red, green |
| mixer node | audio_mixer.hpp | right pan gain `sin(x)` -> `1 - cos(x)` | red, green |
| mixer node | audio_mixer.hpp | 5 ms gain ramp removed | red, green (see note 1) |
| IF filter node | if_filter.hpp | retune restarts the filter (state cleared, oscillator phase reset) | red, green |
| IF filter node | if_filter.hpp | baseband edges computed against a fixed centre | red, green |
| signal-generator model | siggen_model.hpp | FM index halved | red, green |
| signal-generator model | siggen_model.hpp | settings change resets the phase | red, green |
| scope model | scope_model.hpp | hysteresis arming removed | red, green |
| scope model | scope_model.hpp | frame starts one sample after the pre-trigger point | red, green |
| edit history | graph_edit.hpp | undo of a delete forgets the wires | red (random-edit test), green |
| edit history | graph_edit.hpp | redo of a paste takes new ids | red (random-edit test), green |
| command stack | command_stack.hpp | group undo runs forward instead of in reverse | red, green |
| SNR squelch | snr_squelch.hpp | floor rises at the closed rate while open | red, green |
| SNR squelch | snr_squelch.hpp | no hysteresis | red, green |
| per-channel model | airband_channel.hpp | per-channel level threshold ignored (fixed -50 dB) | red, green |
| per-channel model | airband_channel.hpp | 4th-order high-pass built as one section | red, green |
| WAV writer | airband_writer.hpp | size cap ignored (x8) | red, green |
| WAV writer | airband_writer.hpp | pre-roll not flushed | red, green |
| WAV writer | airband_writer.hpp | `pathIsUnder` accepts every path | red, green |
| UDP sink | airband_writer.hpp | closed gate ignored (silence sent) | red, green |

Note 1: the first version of the ramp test muted the tone at a zero crossing (a 0.065 step either way), so removing the ramp stayed green. The test
now mutes at a quarter cycle (a peak, where an instant mute is a 0.5 step) and the mutant is red. Found by the break-it, fixed in the same commit.
Other things the tests found in my own code while being written: the generator's text parser accepted a 15th field (now exactly 14); the scope's
hysteresis test could not fail until the chatter outlasted the frame fill; the strictly-rising check of the limiter curve is a float-resolution
claim only below |x| = 1.2.

## 5. UNVERIFIED

- Nothing is wired, so no window, engine or patch behaviour was exercised: every claim above is about the blocks in isolation. No `.slint` screen was
  looked at; no real radio was opened; no real airband audio was used (generated signals only).
- The headless-suite gate: two standing failures that are outside this lane (section 7); the rest passed, some only on a re-run alone after a load failure.
- The Windows API surface touched is the standard library's file streams and the UDP sender's existing Winsock use on 127.0.0.1; the test binds a
  socket on 127.0.0.1 only. No dialog was raised during any run (nothing was started on the visible desktop; no certificate, key store, firewall
  rule or file dialog is touched by any of this code).
- A UTC clock reading is an argument of `ChannelWavWriter::write`; how the host gets it is not part of this lane.
- `PassbandFilter` (signal-blocks-3) and `NotchBank` (signal-blocks-3) as drop-in replacements for the IF filter's band part and the channel notches:
  not tried.
- `SavitzkyGolay` is written from the least-squares definition; the 1964 paper is cited for the origin of the name and was not opened. The Audio EQ
  Cookbook (the biquad formulas) was not opened by me either: `audio_biquad.hpp` came from `feat/signal-blocks-4` where its author read it; my tests
  check the filters' measured responses against the closed forms of 4th-order Butterworth, not against the cookbook text.
- Numerical limits are float: the exponential average in power and the SG weights are double inside, the traces are float.
- A noise floor that jumps up by more than the SNR open threshold holds the SNR gate open until it creeps up or `relearn()`; `maxOpenS` fixes it
  but is off by default (a continuous broadcast is a legal carrier). Stated in the header; not a defect found by a test.

## 6. WIRING AND SCREEN HAND-OFF (what each forbidden file needs; every control: English label, what it sets, range, default)

The setting keys named below are PROPOSALS: no key was checked against `docs/API.md` or the config halves (forbidden or shared files); the owner of
the wiring lane names them.

### Row 1 - Display trace: average, smoothing, de-comb (`src/slint` Display body, lane B's rows; `trace_hold.hpp` and the spectrum publisher)
Chain per frame: `TraceAverager::push` then `SavitzkyGolay::apply` / `smoothMovingAverage` then `DeComb::process`; the drawn trace is the result.
`trace_hold.hpp`'s Peak mode stays as it is; its `Average` mode is what `AvgMode::Rms` replaces (same maths).
| Label | Sets | Range | Default |
|---|---|---|---|
| "Average" | `AvgMode` | Off, Linear, RMS, Log, Exponential | Off |
| "Average frames" | `setFrameCount` | 1 - 128 | 8 |
| "Average time constant (s)" | `setTimeConstantS` | 0.005 - 60 | 0.5 |
| "Smoothing" | none / moving average / Savitzky-Golay | 3 choices | None |
| "Smoothing width (bins)" | window | odd, 3 - 101 | 9 |
| "Smoothing order" | SG order | 0 - 8 (below the width) | 2 |
| "Remove comb" | `DeComb` on | on / off | off |
| "Comb period (bins)" | `period` | 0 = Auto, 4 - n/3 | 0 |
| "Comb strength" | `strength` | 0 - 1 | 1 |
| "Remove centre spike (bins each side)" | `dcBins` | 0 - 9 | 0 |
| "Comb memory" | `profileMemory` | 0 - 0.99 | 0 |
The axis key for `push` is the same key `TraceHold` takes (centre, rate, bins).

### Row 2 - Zoom to a dragged selection (Slint spectrum and waterfall)
On drag: `zoomToSelection(currentView, rect, plotW, plotH, limits, axes)`; apply `result.view` when `changed`; push the old view on `ViewHistory`.
"Back" pops it. On the waterfall pass `ZoomAxes::FrequencyOnly`. `limits.fullLoHz/fullHiHz` = the capture span; `minSpanHz` default 100.
Key and gesture are the screen lane's choice (none was invented here). Strings: "Zoom to selection", "Back to the last view".

### Row 3 - Gradient editor (lane B's compositor and a new editor strip)
Add a "Custom" entry to lane B's "Colour map" choice; when chosen, `toTable(gradient)` replaces the table the compositor holds. Save `serialise()` in a
config field (proposed `waterfall.custom_map`); on load `parse()` and fall back to the default gradient if `ok` is false (`problemText(r.problem)`
or `r.syntax` is the sentence). Editor strip: markers for each stop (drag = `moveStop`, click empty = `addStop`, delete key = `removeStop`, click a marker =
colour picker = `setColour`), choice "Blend in: sRGB / Linear light / OKLab" (`setInterp`, default OKLab), "Reverse" (`reverse`), "Start from" a stock
map (`fromMap(m, 17)`), "Name" (`setName`). Warn with `lightnessMonotone(toTable(g))` false: "This gradient gets darker as the signal gets
stronger; two levels may look alike." The waterfall must be cleared on a change, as lane B already does for a map change.

### Row 4 - Mixer node (`patch_graph.hpp`, `patch_runner.hpp`, the canvas)
Add `NodeKind::Mixer` whose inputs are N SEPARATE Audio ports (so `connect()`'s one-wire-per-input rule stands) and one Audio output; N is a node
setting 2 - 16, default 4. The runner builds a `dsp::AudioMixer(N, kOutRateHz)` and calls `processMono` for the existing mono Audio port (or `process`
for a stereo sink). Node controls: per input "Gain (dB)" -90..+24 (0), "Balance" -1..1 (0), "Mute", "Solo"; node "Master (dB)" -90..+24 (0),
"Soft limiter" on/off (off), "Limiter threshold" 0.1..0.99 (0.8), "Pan law" Constant power / Linear (Constant power). Meter: `clipCount()`.
Note the patch audio today is MONO at `kOutRateHz`: the mixer pans each mono input to a stereo pair and the mono output is `(L+R)/sqrt(2)`
(a centred input is at unity).

### Row 5 - IF filter node
`NodeKind::IfFilter`, Iq in, Iq out (a channel-like node placed after a radio). Runner: `IfFilter(rate)`, `retune(radio centre)` on every retune
(that is the whole retune handling), `setBand(low, high, on)`, `setNotch(i, rf, width, on)`. Controls: "Pass band - low edge (Hz)" and "high edge (Hz)"
(RF Hz as typed, with the existing frequency entry), "Pass band on", "Notch 1..8 - frequency (Hz)", "- width (Hz)" 5 .. rate/4 (100), "- on". The
node draws `responseDb(rf)` over the radio's span. Calls are made on the DSP thread (the block has no locks).

### Row 6 - Signal-generator node
Today the "siggen" device is `source::SigGen`. A node that carries `GenSettings` (text form `FOXGEN1;...` in the patch file) and a runner that
calls `SignalGenerator::generate` gives the sweep, AM and FM; the old tone slots can be fed from `GenKind::Tone`. Controls: "Signal" Off / Tone / Noise /
Sweep / AM / FM, "Frequency (Hz)" (+-0.49 rate), "Level (dBFS)" -200..0 (-20), "Noise level (dBFS)", "Added noise (dBFS)" (off at -300),
"Sweep start (Hz)", "Sweep stop (Hz)", "Sweep time (s)" 0.001..3600, "Sweep shape" Linear/Log, "Sweep direction" Up / Up and down, "Modulation (Hz)",
"AM depth" 0..1, "FM deviation (Hz)", "Seed".

### Row 7 - Scope node
`NodeKind::Scope` (Audio or Iq in; consumes). Runner feeds `ScopeModel::push` / `pushIq`; the canvas asks `frame(columns, mins, maxs)` when
`frameReady()` and calls `acknowledge()`. Controls: "Trigger" Auto / Normal / Single, "Edge" Rising / Falling, "Level", "Hysteresis", "Hold-off
(samples)", "Pre-trigger (%)" 0..100 (25), "Time base (samples per frame)" 2 .. capacity/2 (1024), "Show" I / Q / Magnitude / Phase, "Arm" (Single).

### Row 8 - Edit history on the patch canvas (`patch_graph.hpp` adapter and the canvas's key handling)
`PatchGraph` already offers what `IEditableGraph` needs: `nodes()`, `wires()`, `find`, `mutableNode`, `addNodeAs(id, kind, name, feed, x, y)` (this is the
"add under a stated id" the undo needs), `removeNode` (takes its wires), `connect` (returns `Connect`; map anything but `Ok` to false), `disconnect(Wire)`.
The adapter packs the rest of a node (`freqHz`, `mode`, `plugin`, `device`, `rateHz`, and whatever else it holds) into the opaque `payload` string and unpacks
it in `setNode`. One thing to check there (not verified): a node restored by undo is appended at the END of `nodes()`, which is the top of the draw order if
that order is the vector order. Keys proposed (the canvas owner decides): Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo, Ctrl+C copy, Ctrl+V paste, Ctrl+D
duplicate, Ctrl+A select all, Delete delete selection, B bypass the selection, shift/ctrl-click add/toggle, drag on empty canvas = `selectRect`.
Every canvas edit that is now direct (`addNode`, `removeNode`, `connect`, node moves, settings edits) goes through the `GraphEditor` instead so it
is one entry; a node drag calls `moveNodes` with absolute positions on every mouse move (they merge into one entry). The runner builds from
`effectiveWires()` so a bypassed node passes input 0 to output 0 (ports of equal type only: the adapter must refuse `setBypass` for a node whose
input 0 and output 0 types differ; the model does not know port types). Menu rows (English): "Undo %s", "Redo %s" (`undoLabel()` / `redoLabel()`),
"Copy", "Paste", "Duplicate", "Select all", "Delete", "Bypass", "Remove bypass". History depth setting (optional): "Undo steps" 1 - 10000 (100).

### Row 9 - Airband monitor per-channel squelch, filter and outputs (`airband_monitor.hpp`, the monitor's list UI, the recorder thread)
- Per-channel squelch: today `buildMonitorSet(channels, block, centre, rate, squelchDb, ...)` sets `ch.squelch->setThresholdDb(squelchDb)` for
  every channel (`airband_monitor.hpp:343-344`). Replace with a per-channel value: add the config (or just its squelch fields) beside `MonitorChannel`,
  keep `AirbandChannelConfig` per list row (text form `FOXAIRCH1|...` fits a bookmark field), and call `ChannelSquelch::configure` / `ChannelProcessor` for
  each strip. `toMonitorChannel(config)` already gives the monitor's own type.
- Order on a strip: demodulated audio -> `ChannelAudioChain` -> squelch gate -> mix; pass `preGate` from `ChannelProcessor::process` to the writer.
- Outputs: ONE `ChannelWavWriter` per channel with output = WavFile, fed from a RECORDER thread (file I/O blocks; never the audio thread) with the
  pre-gate audio and the gate flag; one `LoopbackAudioSink` per channel with output = Udp, `push(gatedAudio, n, gateOpen)`; `start(port, err)` only when
  the user turns it on. `recordingsRoot` is the host's existing recordings folder. Both are OFF by default; no policy change (the monitor already plays
  other people's voice; nothing is kept or sent unless the user switches it on for that channel).
- Row controls (English): "Squelch" Level / Signal-to-noise (Level), "Squelch level (dB)" -120..0 (-50), "Squelch SNR (dB)" 1..60 (10),
  "SNR hysteresis (dB)" 0..30 (3), "Hold (ms)" 0..10000 (400), "High-pass (Hz)" off or 20..3000, "Low-pass (Hz)" off or 300..20000, "Filter slope"
  12 dB per octave / 24 dB per octave (24), "Notch 1..3 (Hz)" 50..20000 with "Q" 0.3..100 (30), "Output" None / File / Stream (None), "One folder per day"
  (on), "File size limit (MB)" 1..2000 (100), "Stream port" 1024..65535 (none; the stream goes to this computer only).

### English strings added by the blocks themselves (no translation was done)
`gradient::problemText`: "A gradient needs at least two colour stops." / "A gradient can have at most 32 colour stops." / "A colour stop must sit between 0 and 1." /
"The colour stops must be in order." / "Two colour stops are too close together." / "The first colour stop must be at the bottom of the scale." /
"The last colour stop must be at the top of the scale." / "The name can use letters, digits, space, - and _ (32 at most)." / "A colour stop position is not a number."
`airband::problemText`: "The channel frequency is not usable." / "The channel width must be between 1 kHz and 200 kHz." / "The channel mode must be AM or NFM." /
"A squelch value is out of range." / "The high-pass must be below the low-pass, each within its range." / "The filter order must be 2 or 4." /
"A notch frequency or Q is out of range." / "The output kind is not known." / "The file size limit must be between 1 and 2000 MB." /
"The stream port must be between 1024 and 65535." / "A stream output needs a port."
`ChannelWavWriter::lastError()`: "The clock reading is not usable." / "No recordings folder is set." / "The recordings folder does not exist." /
"The output folder is not under the recordings folder." / "The output folder could not be created." / "The output file is not under the recordings folder." /
"The output file could not be opened." / "The output file could not be written." / "Writing to the output file failed."
`LoopbackAudioSink::start` error: "The stream port must be between 1024 and 65535." (and the sender's own sentences).

## 7. Gates

**Own tests** (section 2): 12 of 12 pass together; six repeated ten times each, all green.

**Full build** of the tree with the twelve new tests: `cmake --build D:/foxsdr-build/en-dspmodels --config Release --parallel 3` exit 0, 618 targets built
(configured with `-DCASCADE_BUILD_SLINT=OFF -DCASCADE_SOFTGL=OFF`).

**Headless set** (`ctest -C Release -L headless`, 715 tests, in slices each under 10 minutes, `--timeout 250`). `git diff --name-status d3630d78..HEAD` is 28 files, all
status A (added), 8609 insertions: no existing source changed, so every existing test runs the base code and any failure below is the base in this
environment, not this lane. Result per slice (indices):

| Slice | Tests | Failed in the slice | Settled |
|---|---|---|---|
| 1-120 (`-j2`, cut at my wall limit; `test_engine_api_transmit` was still running) | 120 | test_airband_data, test_band_plan, test_converter_call_sites (the three known false failures); test_engine_api_03 (known load flake); test_engine_api_schedule, test_engine_api_merge (load) ; test_engine_api_decode_radio (Timeout 120 s); test_engine_api_source_kinds (fixture missing at first) | schedule, merge, api_03 and the cut-off transmit PASSED alone (transmit 110 s, merge 54 s); decode_radio and source_kinds see below |
| 121-200 | 80 | test_engine_service_options (exception 0xc0000409 under load), test_excuse_cap (72 s), test_failure_files (75 s) | service_options and failure_files passed alone; excuse_cap failed once more alone (73 s) and PASSED on the third run (77.9 s): load |
| 201-300 | 100 | test_page_resize_id (known false), test_pass_control (known load flake) | - |
| 301-420 | 120 | test_png_decode (known false); test_radio_setup, test_sentinel, test_soapy_enum_proc, test_pump_live_traffic | the four PASSED alone |
| 421-540 | 120 | test_window_failed_app (known: configuration, the Slint window is off) | - |
| 541-660 | 120 | none | - |
| 661-800 | 55 | none | - |

**Two standing failures, both outside this lane's files**: `test_engine_api_decode_radio` (Timeout; "real decoders on radios 2 to 5, each on its own
file") and `test_engine_api_source_kinds` (fixture `scope.wav` was missing at first: gitignored, made by `src/slint/tests/drive/fixtures/make_scope.py`;
after I generated `scope.wav`, `tone.wav` and `tones3.wav` with the three standard-library fixture scripts it still fails: `radio.2.source` reads
"siggen" where "file" is expected, sample rate 2000000 where 96000). Cause not diagnosed. Their records (`engine-stage-7.md`) report 20 of 20 green on the
lane that wrote them, so this environment (a fresh worktree with Slint off, no installed decoders, a loaded machine) differs; the code under test is
byte-identical to the base. The three generated fixture files were deleted again after the run (they are ignored by git either way).
Nothing was left running: no process of this lane survived a timed-out `ctest` (checked for any process under the build folder).

**`config.json`**: `%APPDATA%/foxsdr/config.json` sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` before and after (read only, never written
by anything in this lane; no test here starts the application).

## 8. Sources opened

No web page was opened. Documents read are the repository's own: `trace_hold.hpp`, `patch_graph.hpp`, `airband_monitor.hpp`, `dsp/squelch.hpp`, `source/siggen.hpp`,
`net/udp_audio.{hpp,cpp}`, `gui/demod_scope.hpp` (header), and on sibling branches `feat/screen-b` `core/colour_maps.hpp` and `screen-b.md`,
`feat/signal-blocks-3` `dsp/spectrum_block.hpp` (header), `dsp/passband.hpp` (header), `dsp/notch_bank.hpp` (header), `feat/signal-blocks-4`
`dsp/audio_biquad.hpp`, `dsp/audio_eq.hpp` and `dsp/noise_squelch.hpp` (headers). Method names cited without opening the source (Audio EQ Cookbook,
Savitzky and Golay 1964, Ottosson's OKLab post, Bessel functions of the first kind, Butterworth poles) are stated as such in section 5; all are standard
and not subject to any licence the code copies from (every formula was written from its definition; the cookbook sections arrived inside the sibling's
`audio_biquad.hpp`).

## 9. Owner decisions needed

1. Where the pre-roll and tail of a channel recording come from: the record assumes the file output is fed the audio BEFORE the gate (so it holds real audio
   either side of a call). That puts a copy of the audio on the recorder thread even while the squelch is closed. If the owner prefers that a closed
   channel hands nothing at all to the writer, the pre-roll falls to zero and the first syllable can be cut.
2. The mono pan law is constant power (-3.01 dB at the centre) and the stereo balance is linear with unity at the centre; the mono output is `(L+R)/sqrt(2)`.
   Change `PanLaw` default if the owner wants the other (`Linear`: -6.02 dB at the centre).
3. The SNR squelch learns for 0.5 s with the gate closed after start and after `relearn()`; the first half-second of a channel is therefore always muted.
4. Output is off by default for every channel; whether a recording should ever be on by default for a channel the user marks "record" is the owner's
   call (it is not what this lane built).
