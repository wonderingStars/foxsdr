# Signal blocks, third batch (2026-11-11)

Branch `feat/signal-blocks-3` (local, cut from `feat/signal-blocks-2` 266926bf, not pushed, not merged, no version bump). Seven more stand-alone blocks and
their tests, closing the top items of the owner's gap list that no earlier lane covers. **Nothing is wired**: only new files were added (`git diff
--diff-filter=M 266926bf..HEAD` is empty), not even `tests/CMakeLists.txt` (the test glob picked the new files up after a re-configure). Conventions,
build and test commands, and the traps of the earlier batches are in `signal-blocks.md` and `signal-blocks-2.md`; this batch follows them (header-only,
`cascade::dsp` / `cascade::core`, tests with `test_check.hpp`, `AllocProbe` from `tests/alloc_probe.hpp`, break-it runs, no program source read).

| Part | Header | Test |
|---|---|---|
| 1 Spectrum, size and window choice | `src/dsp/spectrum_block.hpp` | `tests/test_spectrum_block.cpp` |
| 2 Notch bank | `src/dsp/notch_bank.hpp` | `tests/test_notch_bank.cpp` |
| 3 CW pitch | `src/dsp/cw_pitch.hpp` | `tests/test_cw_pitch.cpp` |
| 4 Waterfall colour maps | `src/core/colour_maps.hpp` | `tests/test_colour_maps.cpp` |
| 5 Waterfall speed and time stamps | `src/core/waterfall_timing.hpp` | `tests/test_waterfall_timing.cpp` |
| 6 MQTT publisher, decode JSON, webhook | `src/core/mqtt_publisher.hpp`, `src/core/decode_json.hpp`, `src/core/decode_webhook.hpp` | `tests/test_mqtt_publisher.cpp`, `tests/test_decode_webhook.cpp` |
| 7 Rotator control | `src/core/rotator.hpp` | `tests/test_rotator.cpp` |

Commits (`git log 266926bf..HEAD`): part 1 6ab7a185; part 2 21be58c6, 85b48fe4, ba1fe35a; part 3 be5c118c; part 4 d0a06a41; part 5 991b5f5d, a3bafdbd;
part 6 fa0979ab; part 7 9aa888dc, c0fa2faa; this record last.

Machine for every measurement: Intel Core i9-14900K (24 cores, 32 threads, 3.2 GHz base), 63.7 GB RAM, Windows 11 Pro 10.0.22631, MSVC, Release build
in `D:/foxsdr-build/feat-blocks`, 4 build jobs. That is a high-end desktop: a laptop-class machine may need two to four times the CPU figures below (not measured).

## What existed already (searched before each part)

| Part | Search result |
|---|---|
| 1 | `dsp::SpectrumEstimator` (`src/dsp/spectrum.hpp`): FFT size fixed at construction, windows from `dsp/window.hpp` (rectangular, Hann, Hamming, Blackman-Harris only: no flat-top), an EMA average only (no peak or min hold), no reduction to columns, output not level-corrected per window (its comment says the GUI does not add the coherent gain). The pipeline builds one with `kFftSize = 1024` (`src/core/receiver_tables.hpp:40`; member `estimator_` at `src/core/pipeline.hpp:985`); `src/core/patch_radio.cpp:19,199` has its own 2048-point one. The FFT is `dsp::ComplexFFT` (`src/dsp/fft.hpp`, pffft, sizes 2^a 3^b 5^c with a >= 5): REUSED. `dsp/window.hpp` is NOT reused (its enum lacks flat-top and its Hamming is fine but one shared table of coefficients was wanted next to the level maths); `specFillWindow` is a new function with the same periodic form. |
| 2 | `dsp::Notch` (`src/dsp/notch.hpp`): a textbook biquad, double state, frequency and Q setters that keep the state, a bit-exact bypass when disabled: REUSED as the filter of every slot. `dsp::AutoNotch` (same file) is a tone-seeking one-notch detector: untouched. The pipeline holds one `Notch` per channel (`src/core/pipeline.cpp:349-351`) plus the two `AutoNotch` (`:355-357`); no bank, no width in Hz, no glide. |
| 3 | The 700 Hz is the constant `kCwToneHz = 700.0` at `src/dsp/demod.cpp:100`. `Demodulator::setMode(CW)` (`demod.cpp:222-228`) sets `shiftDown_` to `kCwToneHz / rate_ - bfo` and `shiftUp_` to `+bfo`, where `bfo` is `ssbBfoHz_`, the Weaver f0 of the voice SSB filter (`ssbShape`, `demod.cpp:145`; about 1.95 kHz at the 3 kHz default): "USB with the input pre-shifted up by 700 Hz". So a carrier on the tuned frequency beats at 700 Hz (measured 699.985 Hz on the real class), but the filter is the voice SSB filter: pass band 300 to 3600 Hz (centre 1950 Hz) at the default setting and 150 to 1200 Hz at its narrowest (`setSsbBandwidthHz` clamps to 1 kHz), measured by sweep on the real `Demodulator`. Pitch not adjustable, no CW width, no CW-lower sense. |
| 4 | The waterfall colour path is `gui::waterfallColor()` (`src/gui/waterfall_view.cpp`, `buildColorLut` near line 96, `colorLut()` near 190): a 256-entry table interpolated in RGB between five theme-driven stops ("phosphor"), rebuilt when `theme::generation()` moves, ImGui `ImU32`. The legend is sampled out of the same function. No OKLab, no alternative maps. The new maps are plain `Rgb8` tables in `core`, independent of ImGui and of the theme. |
| 5 | `WaterfallView::addLine` stamps each row with `nowSeconds()` (`src/gui/waterfall_view.hpp:80,105`) and draws a time scale from the stamps; the line rate is whatever frames arrive (no pacer, no max/average combining, no speed choice). Nothing computes round-time ticks, UTC/local or DST. |
| 6 | The project's HTTP client is `core::postBounded()` / `authRequestBounded()` (`src/core/crash_upload.hpp:394,427`; WinHTTP on Windows, cpp-httplib elsewhere): https anywhere, plain http only to loopback, timeouts, cancel, response cap: REUSED by the webhook. No MQTT code anywhere. The socket helpers of batch 2's `powerlog::detail` (`dial`, `closeSock`, `ensureNet`) are REUSED for the MQTT and rotctld sockets. `tests/http_post_stub.hpp` (loopback HTTP stand-in) is reused for the webhook test; `nlohmann::json` (already in the project) is the independent JSON parser. |
| 7 | No rotator, rotctld or GS-232 code anywhere (`grep` of `src` and `tests` for rotctld, GS-232, rotator, azimuth/elevation command code found only map and tracking display code). |

## Sources, and what is and is not verified

| Fact | Source read this session | Status |
|---|---|---|
| Hann sidelobe -31 dB and noise bandwidth 1.50 bins, Blackman-Harris 4-term sidelobe -92 dB, rectangular -13 dB and 1.00 | Keysight 89600B help "Window Shapefactor and Equivalent Noise Bandwidth", https://helpfiles.keysight.com/csg/89600b/webhelp/Subsystems/gui/content/windows_shapefactor_and_equiv_noisebw.htm (search result, 2026-11-11) | verified (rounded figures); the test measures -31.48, -92.03, -13.26 |
| Blackman-Harris 4-term coefficients 0.35875, 0.48829, 0.14128, 0.01168 | Harris, Proc. IEEE 66(1), 1978; the same numbers are already in `dsp/window.hpp` | from the paper via the existing code; **UNVERIFIED against the paper** |
| Flat-top five-term coefficients | the first four digits of each (0.2156, 0.4160, 0.2781, 0.0836, 0.0069) confirmed as MATLAB's flat-top set by the Lyons note "Scalloping Loss Compensation" quoted in a search result (https://www.dspguru.com/files/Scalloping%20Loss%20Compensation-Lyons.pdf); the eight-digit values (0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368) are from memory | **partly UNVERIFIED** (digits beyond the fourth). Heinzel, Rüdiger, Schilling 2002 (the primary source for flat-tops) could not be read: its PDF was unreachable (403 / 404). The measured window gives ENBW 3.7702 bins and sidelobe -93.01 dB, which are the figures published for this set |
| Hamming (0.54, 0.46) sidelobe -42.7 dB, 3 dB widths of the windows | analytic / measured in the test; no primary source read | measured; the textbook 3 dB widths (0.89, 1.44, 1.30, 1.90 bins) are matched to 0.02 |
| OKLab matrices (as updated 2021-01-25), public-domain / MIT code | Björn Ottosson, "A perceptual color space for image processing", https://bottosson.github.io/posts/oklab/ (published 2020-12-23, read 2026-11-11) | verified (the code block was fetched; the reference L a b of sRGB red in the test is from memory, tolerance 2e-3: **UNVERIFIED digits**) |
| sRGB transfer function constants (12.92, 0.0031308, 1.055, 0.055, 2.4, 0.04045) | IEC 61966-2-1, from memory (the OKLab page does not print them) | **UNVERIFIED against the standard**; checked by round trip and the 0.5 -> 0.21404 value |
| Red-green dichromacy matrices (protan, deutan, severity 1.0) | Machado, Oliveira, Fernandes, IEEE TVCG 15(6) 1291-1298 (2009), matrices as printed at https://www.inf.ufrgs.br/~oliveira/pubs_files/CVD_Simulation/CVD_Simulation.html (read 2026-11-11) | verified as printed; applied to linear sRGB (the page does not say so) |
| MQTT 3.1.1: packet types and flags, Remaining Length (4 bytes, 268,435,455), CONNECT layout and flags, client id rules, CONNACK codes, PUBLISH flags and topic rules, packet identifier rules, DUP on re-delivery, PUBACK, keep-alive 1.5x, PINGREQ | OASIS MQTT Version 3.1.1 OASIS Standard (29 Oct 2014), https://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html (read 2026-11-11, first 100,000 of 170,584 characters) | verified. **Section 4.4 (message retry) and 4.8 (errors) were beyond what was read**: the re-send with DUP and the same identifier after a reconnect follows from 2.3.1 and 3.3.1-1 but 4.4's own text was not read |
| viridis, magma, inferno, plasma licence | https://raw.githubusercontent.com/BIDS/colormap/master/LICENSE.txt (title "mpl-colormaps by Nathaniel Smith & Stefan van der Walt": "To the extent possible under law, the persons who associated CC0 with ..." = the CC0 waiver; read 2026-11-11) and the repository listing https://github.com/BIDS/colormap | verified (the full CC0 legal code was not on the page) |
| turbo licence | gist header, https://gist.github.com/mikhailov-work/6a308c20e494d9e0ccc29036b28faa7a: "Copyright 2019 Google LLC.", "SPDX-License-Identifier: Apache-2.0", "Author: Anton Mikhailov" (read 2026-11-11) | verified |
| rotctld protocol: default port 4533, one command per line, "RPRT x" for set commands, get_pos = azimuth line then elevation line, P/p/S/K, "pass 0.0 for elevation if unsupported" | rotctld(1), Debian testing, libhamlib-utils 4.7.2, page dated 2020-09-09, https://manpages.debian.org/testing/libhamlib-utils/rotctld.1.en.html (read 2026-11-11) | verified; no Hamlib source read |
| GS-232A: CR terminator, any case, CR after every command and LF with data, "?>" for an invalid command, three-digit angles, Maaa, Waaa eee, S, C ("+0nnn"), C2 ("+0aaa+0eee"), B, R L A U D E, Xn, azimuth 000-360 or 450, elevation 000-180 | Yaesu GS-232A operation manual copy, https://www.qsl.net/on7eq/pdf/GS232A.pdf (text extracted from the PDF by a script, 2026-11-11) | verified for the A; the manual's serial settings were not in the extracted text |
| GS-232B reply to C2 in the form "AZ=aaa EL=eee" | a third-party description in a search result only | **UNVERIFIED**; the parser accepts both forms |

No program source was read (clean room). Downloads: one PDF (the GS-232A manual, saved by the fetch tool, text extracted, not kept); no software, library or source
was downloaded or installed; no outside service was contacted except by reading the web pages above.

## Part 1: spectrum with a choice of size and window (`SpectrumBlock`)

**What it is.** FFT sizes 1,024 to 524,288 (powers of two, checked), windows rectangular, Hann, Hamming, Blackman-Harris 4-term and five-term flat-top; `hop`
(new samples between frames) is derived from a wanted frame rate (`specPlan`: floor `N/16`, reported) so the frame rate stays fixed while the size grows; modes
Instant, Average (EMA, alpha), PeakHold (optional dB/s decay) and MinHold; output fftshifted, either all `N` bins or `columns` (up to 16,384) reduced by the
MAXIMUM per column (`specReduceMax`: column c covers bins [c N / cols, (c+1) N / cols), so every bin lies in exactly one column). Two scalings: `Tone` (a complex
tone of amplitude A exactly on a bin reads 20 log10 A for every window: bin power divided by (sum w)^2) and `Psd` (dB per Hz: divided by fs sum w^2).

**Threads.** The constructor and `prepare()` allocate (control thread). `process()` is the DSP thread's and allocates nothing. `latest()`, `swapIn()`,
`collectRetired()` belong to one control thread. A size, window, hop or column change is built by `prepare()`, handed over by `swapIn()` (one atomic exchange) and
adopted by the next `process()`; the engine it replaces goes into one of four parking slots and is freed by the control thread; if every slot is full the DSP
thread keeps the old engine (never frees). Frames reach the reader through a three-slot triple buffer (wait-free both sides).

**Proof (real runs, `test_spectrum_block`, 62,746 checks).**
- Published figures per window (16x zero-padded transform of a 1,024-point window): ENBW 1.0000 / 1.5000 / 1.3628 / 2.0044 / 3.7702 bins; first-null half width
  1 / 2 / 2 / 4 / 5 bins; 3 dB width 0.886 / 1.440 / 1.302 / 1.899 / 3.724 bins; highest side lobe -13.26 / -31.48 / -42.67 / -92.03 / -93.01 dB (rect, Hann,
  Hamming, BH4, flat-top); coherent gain 1 / 0.5 / 0.54 / 0.35875 / 0.21558. Pinned: ENBW to 1e-6..5e-3, null width to 0.07, side lobes to 0.1..3 dB.
- A single-bin tone (amplitude 0.7) at EVERY size 1,024 to 524,288, all five windows to 64k and BH4 and rectangular above: the full-resolution peak is at the right
  bin and within **0.0000 dB** of 20 log10(0.7) (pinned 0.05 dB); reduced to 1,000 columns (which divides no size) the peak is within 0.0000 dB (pinned 0.5 dB as asked)
  and equal to the unreduced peak to 1e-4 dB. A tone half a bin off: scalloping 3.92 / 1.42 / 1.75 / 0.83 / 0.010 dB (rect, Hann, Hamming, BH4, flat-top).
- Noise of power 0.25 at 2.4 MS/s, 65,536 points: PSD scale reads -69.817..-69.824 dB/Hz against the exact -69.823 for every window; tone scale reads
  10 log10(P ENBW / N) to within 0.006 dB for every window (e.g. flat-top -48.423 against -48.422).
- Fixed frame rate: 30 fps at 2.4 MS/s for every size 1,024 to 524,288 (hop 80,000 samples: 29 to 31 frames in the second second); 10 MS/s with 1,024 points gives hop
  333,333 and a negative overlap (gaps between frames, reported); 100 fps at 524,288 is capped to 73.2 fps by the N/16 floor and says so.
- Averaging 40 noise frames cuts the dB spread from 5.66 to 0.99; peak hold, min hold, decay (100 dB/s), mode change restarting the hold, chunk-independent frames (bit-identical).
- Zero `operator new` in `process()` for all four modes, full and reduced output, including the call that adopts a new engine; 300 reconfigurations while the DSP thread
  runs flat out (183 frames read, 126,870 process calls) with no leak or double free (a live-engine counter returns to its start value).

**Measured cost and memory** (`FOXSDR_BLOCK_BENCH=1 test_spectrum_block.exe`; Blackman-Harris, Average, 1,000 display columns, 30 fps wanted, hop floor N/16; frames cost
per second of input shown as a share of ONE core of the i9-14900K; the cost per frame is the same at 2.4 and 10 MS/s because the frame rate is fixed and the FFT dominates):

| FFT size | ms per frame 2.4 MS/s | share of a core | ms per frame 10 MS/s | share of a core | memory counted (block's buffers) | working-set growth |
|---|---|---|---|---|---|---|
| 1,024 | 0.013 | 0.0 % | 0.012 | 0.0 % | 69 KB | 4 KB (noise) |
| 2,048 | 0.017 | 0.1 % | 0.017 | 0.1 % | 122 KB | 94 KB |
| 4,096 | 0.028 | 0.1 % | 0.027 | 0.1 % | 229 KB | 221 KB |
| 8,192 | 0.048 | 0.1 % | 0.053 | 0.2 % | 442 KB | 442 KB |
| 16,384 | 0.096 | 0.3 % | 0.094 | 0.3 % | 868 KB | 864 KB |
| 32,768 | 0.215 | 0.6 % | 0.205 | 0.6 % | 1.7 MB | 1.7 MB |
| 65,536 | 0.447 | 1.3 % | 0.439 | 1.3 % | 3.4 MB | 4.5 MB |
| 131,072 | 2.510 | 7.4 % | 1.395 | 4.2 % | 6.8 MB | 7.4 MB |
| 262,144 | 5.326 | 15.8 % | 6.687 | 22.4 % | 13.6 MB | 17.9 MB |
| 524,288 | 12.215 | 36.3 % | 11.640 | 34.7 % | 27.3 MB | 35.7 MB |

(One run each; the large sizes spill the cache and vary by 2x between runs.) **Recommendation for this machine class:** default **8,192** bins (0.05 ms per frame, 0.1 % of a
core, 0.44 MB; 293 Hz resolution at 2.4 MS/s); ceiling **131,072** for the general case (7 % of a core on this CPU, so budget 15 to 30 % on a laptop-class one: not measured);
262,144 and 524,288 only as an expert option on a desktop (16 to 36 % of a core here, 14 to 36 MB), with the control saying so. The existing 1,024 is the cheapest row.

**Break-it** (patch, rebuild, named check RED, `git checkout`):

| Removed or broken | RED |
|---|---|
| Max per column replaced by "first of the column" | 157 checks: reduced tone -35.9 dB against 0, partition, exact small case |
| Level divides by N^2 instead of (N coherent gain)^2 | 69 checks: tone level, scalloping, tone-scale noise (-58.4 against -52.4) |
| Peak hold keeps the minimum | 3: hold level, third-bin level, decay |
| Hop ignored (hop = N) | 19: 30 fps, chunk invariance, allocation-test frame count |
| One frame per `process()` call | 21 |
| DSP thread drops the retired engine without parking it (leak) | 2: live-engine counts |
| Average alpha forced to 1 | 2: averaged level, noise spread |
| `collectRetired()` body emptied | not RED: the destructor and the next `swapIn()` still free (no behaviour lost) |

**Limits.** Non-finite input samples are the caller's to filter. The first frame needs a full window of input (no frame before N samples); a swap loses the old ring, so the new
size starts empty. `latest()` is a single-reader API.

### WIRING HAND-OFF (part 1)
- Existing: `core/pipeline.hpp:985` `estimator_` and the pipeline's frame publication (`core/spectrum_frame.hpp`); `core/receiver_tables.hpp:40` `kFftSize`; `core/patch_radio.cpp:19,199`.
  The frame type must carry a variable bin count; the waterfall texture width and `mapLineToPixels` (`gui/waterfall_view.hpp`) take `n` already; the GUI can call `specReduceMax` for its pixel width.
- Ops and settings needed: `SET_SPECTRUM_SIZE` (1,024..524,288, power of two), `SET_SPECTRUM_WINDOW` (5 values), `SET_SPECTRUM_MODE` (Instant / Average / Peak hold / Min hold), `SET_SPECTRUM_AVERAGING`
  (alpha), `SET_SPECTRUM_PEAK_DECAY`, `RESET_SPECTRUM_HOLDS`, `SET_SPECTRUM_FRAME_RATE`; settings fields for each (defaults: 8,192, Blackman-Harris, Average, 0.3, 0, 30 fps). Controls: size and window drop-downs, mode buttons, averaging slider, a reset-hold button.
- The engine pass calls `prepare()` + `swapIn()` on the control thread on any change, `process()` on the DSP thread, and reads `latest()` from ONE thread (the one that publishes frames).
- Strings: "Spectrum size", "Window", "Rectangular", "Hann", "Hamming", "Blackman-Harris", "Flat top", "Spectrum mode", "Instant", "Average", "Peak hold", "Min hold", "Averaging", "Peak decay", "Reset holds", "Frame rate", "FFT size above the recommended ceiling uses a lot of CPU".

## Part 2: notch bank (`NotchBank`)

**What it is.** Up to ten manual notches on audio (default 48 kHz): frequency (20 Hz to 0.45 fs), width (the -3 dB bandwidth, 1 to 2,000 Hz; Q = f/width) and on/off, in series, each an existing
`dsp::Notch`. A retune is a glide (3 ms time constant, updated every 16 samples of the STREAM, so any chunking gives identical output) and an on/off change is a 10 ms cross-fade
(a notch that is switched on starts from a cleared state and fades in; one that is switched off keeps running until the fade reaches zero, then is cleared and skipped). With no slot on and
none fading, `process()` is a single atomic load and a return (bit-exact passthrough; `blocksWorked()` stays 0). Settings from any one control thread (atomics), `process()` on the DSP thread, allocation-free.

**Proof (`test_notch_bank`, 98 checks).** Depth at the centre **144.4 to 154.6 dB** for widths 10, 50, 200 Hz at 300 Hz, 1, 3, 10 kHz (float rounding is the floor; pinned 100 dB);
-3 dB points at width/2 either side: -2.89 and -3.10 dB; 3 widths away on either side within 0.15 dB (pinned: above -1 dB); two notches 100 Hz apart (1000 and 1100 Hz, width 40): -154.9 and -150.4 dB at the
centres, -1.3 dB between them, -0.04 dB at 1400 Hz; all ten on (300 to 3000 Hz in 300 Hz steps, width 30): shallowest centre 144.4 dB, worst midpoint -0.10 dB, 6 kHz untouched.
Clicks (steady 2.5 kHz tone at 0.5, largest sample step against the tone's own 0.16281): notch moved between 300 and 1900 Hz every 20 ms, width 10 Hz: ratio **1.001**; switched on and off every 100 ms: **1.016**;
on a tone sitting exactly on the notch, switched every 100 ms: 1.000; swept THROUGH the tone: 1.000 (an honest dip, no step); for comparison the bare `Notch` with its frequency jumped: **1.351**. Glide: 1105 Hz one block after a
1000 -> 2000 Hz retune, 2000.000 Hz after 100 ms. Fade out of a tone on the notch: peak 0.015 in the first 0.5 ms, 0.264 around 5 ms, 0.500 (the tone) after 12 ms. Cost: 0.00 ns per sample with none on, about 2.3 ns with one and
14 to 16 ns with ten (the 48 kHz budget is 20,833 ns). Bit-identical output for chunk sizes 1, 3, 5, 32, 100, 480, 1600 across a retune and a switch-off; bad index, NaN and infinite settings refused; frequency 0, negative, above Nyquist and width 0 or huge stay
finite; 20,000 random setting changes against a running DSP thread; zero `operator new`.

**A real bug the break-it round found:** `setOn(false)` hit the early-out before the fade could start (the counter that kept the filter running was only updated inside a block), so a switched-off notch was cut off in one sample. Fixed (a count of slots whose filter is running) and
the glide, the fade and the zero-work counter got their own tests.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Frequency set at once (no glide) | 2: glide check (1105 Hz after a block) and the concurrent test |
| Fade step 1.0 (no fade) | 2: fade-out check (0.015 / 0.264) |
| Early-out removed | 2: `blocksWorked() == 0` and the cost pin |
| Block position reset on every call | 4: chunk invariance |
| Q = f x width instead of f / width | 18: depth, -3 dB points, two notches, ten, bounds |
| Slot never starts when switched on | 22 |

### WIRING HAND-OFF (part 2)
- Existing: `core/pipeline.cpp:349-351` builds `notchL_` / `notchR_` (one `Notch` per channel) and `:355-357` the two `AutoNotch`; the chain is "... -> Notch -> AutoNotch -> NoiseReduction -> peak Limiter" (`core/pipeline.hpp:11`). The bank is mono: one per channel with the same settings (or one for a mono path).
- Ops and settings: `SET_NOTCH` (index 0..9, frequency, width, on) and `CLEAR_NOTCHES`; settings array of ten {freq, width, on}. Controls: ten rows of frequency, width and a switch, or a list with add and remove.
- Strings: "Notches", "Notch frequency", "Notch width", "Notch on", "Clear all notches".

## Part 3: CW pitch (`cwTuning`, `cwFilterTaps`)

**What it is.** A pure helper: for a pitch 300 to 1,200 Hz, a sideband (Upper or Lower) and a -6 dB width (50 Hz up to 2 (pitch - 50 Hz) and 0.4 fs), the numbers the Weaver chain of `Demodulator::processSsb`
needs: the BFO (= the pitch, so the pass band is centred), the first and second mix frequencies in `Demodulator`'s sign convention (both 0 and +-pitch with a centred filter), the low-pass cutoff (width/2), a suggested transition
(width/2 clamped 50..400 Hz) and the odd tap count (capped at 2,047 like the SSB filter) for it. The audio is pitch + d for the Upper sense and pitch - d for the Lower, d being the offset from the tuned frequency.

**Proof (`test_cw_pitch`, 162 checks).** Driven through a Weaver chain built like `processSsb` (mix, real-tap low-pass through `FirDecimator`, mix, real part): a carrier ON the tuned frequency comes out at the chosen pitch within **0.081 Hz worst**
over 92 runs (48 and 192 kHz, pitches 300 to 1,200 Hz, both sidebands, widths 100 and 500 Hz; pinned 1 Hz); a signal 100 Hz above the zero-beat gives pitch + 100 (Upper) and pitch - 100 (Lower) within 1 Hz; the -6 dB pass band is
centred on the pitch within **0.000 Hz** and its width within 0.23 % over seven cases and both sidebands (pitch 300 to 1,200, width 100 to 1,000 Hz). The existing `Demodulator` in CW mode, measured for comparison: carrier at 699.985 Hz,
upper-sideband sense (+150 Hz gives 850 Hz), pass band 300 to 3600 Hz (centre 1950) at the default width and 150 to 1200 Hz (centre 675, still 1,050 Hz wide) at its narrowest setting.

**Break-it:** BFO = pitch + 100 Hz: 21 RED; low-pass cutoff = width instead of width/2: 15 RED; Lower second mix sign dropped: 5 RED; pitch clamp removed: 2 RED. The Lower FIRST mix sign is not testable: with the BFO equal to the pitch it is 0 Hz in both senses (equivalent mutant).

### WIRING HAND-OFF (part 3): the exact lines to change in `src/dsp/demod.cpp` and `demod.hpp` (NOT changed here)
- `demod.cpp:100` `constexpr double kCwToneHz = 700.0;` becomes the default of a member `cwPitchHz_` (plus `cwWidthHz_ = 500`, `cwSideband_ = Upper`).
- `demod.cpp:222-228` (`case DemodMode::CW:` in `setMode`): replace the body with `const CwTuning t = cwTuning(cwPitchHz_, cwSideband_, cwWidthHz_, rate_); shiftDown_.setFrequency(t.firstMixHz / rate_); shiftUp_.setFrequency(t.secondMixHz / rate_);` and design the CW filter:
  `ssbFilter_ = FirDecimator(windowedSincLowpass(cwFilterTaps(rate_, t.lowpassTransitionHz), t.lowpassCutoffHz / rate_, WindowType::Hamming), 1);` (the voice SSB filter must be restored when the mode leaves CW: `ssbFilter_ = FirDecimator(designSsbTaps(rate_, ssbBandwidthHz_), 1)` as `setSsbBandwidthHz` does at `:197-198`).
- `demod.cpp:197-205` (`setSsbBandwidthHz`) re-designs `ssbBfoHz_` and the filter for every mode that runs the chain: for CW it must not overwrite the CW design (call the CW branch instead); `processSsb` (`:303`) is unchanged.
- `demod.hpp`: add `setCwPitchHz`, `setCwWidthHz`, `setCwSideband`, each restarting a running CW mode like `setSsbBandwidthHz` does; update the mode comment at `demod.hpp:67`.
- Pipeline: `core/pipeline.cpp:497` (the CW test) and `:505` (`demod_.setSsbBandwidthHz(vfoBandwidthHz_)`): the CW width replaces the bandwidth control while in CW; `engine/tool_modes.cpp:120` sets CW for the tools.
- Ops and settings: `SET_CW_PITCH` (300..1200), `SET_CW_WIDTH` (50..2000, clamped by the pitch), `SET_CW_SIDEBAND`; settings `cwPitchHz` (700), `cwWidthHz` (500), `cwSideband`. Controls: pitch slider with the value in Hz, a width choice (50, 100, 250, 500 Hz...), a CW-U / CW-L switch.
- Strings: "CW pitch", "CW filter width", "CW upper", "CW lower", "Hz".

## Part 4: waterfall colour maps (`colour::buildColourMap`, `colourMap`, `exactMap`)

**What it is.** Five 256-entry maps, computed (no data copied from anywhere): **Deep sea** (dark blue to teal to green to pale yellow), **Ember** (black, red, orange, yellow, white), **Slate** (neutral grey), **Spectrum** (rainbow-like: violet-blue,
cyan, green, yellow, orange, with rising lightness) and **Blue-yellow safe** (lightness plus the blue-yellow axis only, a stays 0, for red-green colour blindness). Each is a path through OKLab from (L, C, h) key points, pulled into the sRGB gamut by shrinking
chroma (lightness and hue kept), sampled densely and then 256 entries taken with EQUAL CHORD length in OKLab (a bisection on the chord so the 256th entry lands on the end), rounded to 8 bits choosing among 64 neighbouring roundings the one closest to
the exact colour whose lightness does not fall below the previous entry's (monotone in the shipped table, not only in the float maths). Slate is kept exactly grey. `exactMap()` returns the float path for a GPU texture.

**Proof (`test_colour_maps`, 4,432 checks).** Lightness is non-decreasing in every shipped table (L first/last: 0.140/0.960, 0.000/1.000, 0.085/0.979, 0.300/0.929, 0.201/0.920); the exact steps are equal chords (coefficient of variation 0.0000, all steps within 2 % of the mean); the 8-bit
tables over windows of 16 entries lighter than L = 0.35: Deep sea 0.738..1.039 of the mean step, Ember 0.924..1.026, Slate 0.950..1.061, Spectrum 0.838..1.054, Blue-yellow safe 0.766..1.046 (pinned 0.70..1.10; **single 8-bit steps are quantisation noise** and the darkest entries cannot be even, because the first
8-bit step above black is already about 0.07 in OKLab lightness); distinct colours 256 / 243 / 234 / 255 / 256. Blue-yellow safe: largest |a| 0.0013 (the red-green axis is unused), b from -0.120 to +0.102. Under Machado 2009 dichromacy simulation the smallest 8-entry step relative
to normal vision is **0.64 / 0.65 (protan / deutan) for the safe map**, against 0.11 / 0.44 for the rainbow and 0.41 / 0.55 for Ember; lightness lost over 8 entries 0.0000. OKLab black, white, red, green, blue, a 216-colour round trip (error below 1e-5 and the 8-bit value recovered), clamps of `colourAt`, determinism, the out-of-range `MapId` fallback.

**Break-it:** lightness rule off: RED (mono); safe map hue moved to 110 degrees: RED (a-axis); chord bisection off: 7 RED; gamut bisection off: 26 RED; OKLab b-row sign flipped: 4,106 RED.

**LICENCES of the well-known maps (reported, nothing copied).** viridis, inferno, magma and plasma are published in the BIDS "mpl-colormaps" repository (https://github.com/BIDS/colormap) by Nathaniel Smith and Stéfan van der Walt under **CC0** (the repository's `LICENSE.txt` is the CC0 waiver:
"To the extent possible under law, the persons who associated CC0 with mpl-colormaps ..."): they may be added by name and by their data with no condition (a credit is courteous, not required). **Turbo** is by Anton Mikhailov, "Copyright 2019 Google LLC", `SPDX-License-Identifier: Apache-2.0`: it may be included, but Apache-2.0 asks that the licence and the notice
be carried with the copied table (and any changes marked). The primary pages gave no trademark statement for the names; none was searched for. The owner may add viridis, inferno, magma and plasma by name under CC0; turbo needs its Apache-2.0 notice shipped with the app (a third-party notices file).

### WIRING HAND-OFF (part 4)
- Existing: `gui/waterfall_view.cpp` `buildColorLut` (near line 96), `colorLut()` (near 190) and `waterfallColor()` (declared `gui/waterfall_view.hpp`, used at `gui/app_window.cpp:12018` and by the legend sampled from the same function near `waterfall_view.cpp:656`). Add a map id to the LUT builder: for an id other than "theme" copy `colourMap(id)` into the `ImU32` table (`IM_COL32(r,g,b,255)`) and rebuild when the id changes (the existing `theme::generation()` trigger).
- Ops and settings: `SET_WATERFALL_MAP`; setting `waterfallColourMap` = "theme" (the present phosphor, default) | "deep-sea" | "ember" | "slate" | "spectrum" | "safe-blue-yellow" (the ids are stable keys: `colour::mapId`). Control: a drop-down in the waterfall settings.
- Strings (translate): "Colour map", "Theme", "Deep sea", "Ember", "Slate", "Spectrum", "Blue-yellow safe" (`colour::mapName`).

## Part 5: waterfall speed and time stamps (`LinePacer`, `tickRows`)

**What it is.** (a) `LinePacer` turns spectra (any rate, each with a time stamp in ns) into lines at a chosen period from "every spectrum" (period 0) to ten minutes; `speedLadderNs()` (0, 50 ms ... 10 min), `periodForSpan(24 h, rows)` (86.4 s per line for 1,000 rows, 80 s for 1,080) and
`periodForSlider` (log scale) are the speed helpers. Spectra within one line period combine by Max or by Average (in the power domain). Lines sit on the grid T(k) = t0 + k x period in integer nanoseconds, computed by multiplication, t0 aligned to a multiple of the period on the clock (or the first spectrum),
so nothing drifts; a stretch with no input becomes bounded gap lines (flagged, at the -200 dB floor) and then the grid is jumped forward keeping its phase; a clock that jumps back by more than a period restarts the grid (counted). (b) `tickRows` picks the round interval (100 ms, 200, 500, 1 s ... 30 min, 1 h, 2, 3, 6, 12 h, 1, 2 days) for a speed and a minimum pixel spacing, and gives the rows that carry a label, in UTC or local time (`Zone`: UTC, fixed offset, EU and US daylight-saving
rules, for the tests; the wiring supplies the OS offset), labelled "HH:MM:SS.d" / "HH:MM:SS" / "HH:MM" / "MM-DD", with local midnight flagged as a day line carrying the date.

**Proof (`test_waterfall_timing`, 20,768 checks).** 24 hours simulated with 4 ms of arrival jitter: **86.4 s per line** (2,592,000 spectra at 30/s, 1,000 lines), **1/3 s per line** (8,640,000 spectra at 100/s, 259,200 lines, a period that is not a whole number of milliseconds) and 1 min (1,440 lines): the line-time error against t0 + k x period is **0 ns** for every line and the
last line is 0 ns on the grid; line counts equal the ideal. Max and average combine exactly (a one-second line of ten spectra gives the maximum; -10 dB and -20 dB average to -12.596 dB). Gaps: 5 empty periods give 5 gap lines; one hour of silence at 1 s per line gives exactly 8 (the cap) and one resync, and the next line is back on the grid; a clock jump back of an hour restarts once; negative times floor correctly. Zero `operator new` in 2,000 pushes.
Ticks: every tick of a 14-speed x 3-row-height x 3-spacing x UTC/local sweep (6,237 ticks, a +5:45 zone) is at a round time of its interval, inside the rows, and at least the minimum spacing from the previous one; across midnight UTC the 00:00 tick is a day line "11-12" / "2026-11-12" at the right row; London spring forward (2026-03-29 01:00 UTC) gives local hourly
labels `D, 02:00, 03:00, 04:00, 05:00, 06:00` (no 01:00, nothing doubled); London fall back (2026-10-25) shows 01:00 TWICE at two rows; New York 2026-11-01 the same; six-hour local ticks across the change choose 12 h (a one-hour margin for intervals of 2 h and more, because the ticks sit 5 h apart across a spring change). The EU and US rules, `civilFromDays` over 1.7 million days and the weekday arithmetic are checked.

**Break-it:** line time = arrival time (grid not fixed to the clock): 10 RED; max replaced by min: 2; power average replaced by a dB average: 2; gap lines off: 3; local offset ignored: 1,107; day line off: 5; backwards-clock restart off: 2; DST margin removed: RED once the choice is pinned (12 h). The final "drop a tick closer than the minimum spacing" guard is an equivalent mutant under the modelled zones (it only matters for a zone whose daylight-saving shift is not a whole hour, such as Lord Howe Island's 30 minutes, which `Zone` does not model).

### WIRING HAND-OFF (part 5)
- Existing: `WaterfallView::addLine` stamps rows with `nowSeconds()` (`gui/waterfall_view.hpp:80,105`; ring of stamps near `:400`) and `gui/app_window.cpp:12018` draws the rows. Put a `LinePacer` in front of `addLine` (configure with the spectrum's bin count, the period from the speed control, Max or Average), pass each emitted line to `addLine` with the pacer's line start time as its stamp (UTC ms), and draw labels from `tickRows` (rows are the ring rows; newest = row 0).
  The wiring must supply the zone: `Zone::fixed(offset)` is not enough for a long session; add an OS-backed `Zone` (Windows `GetDynamicTimeZoneInformation`, POSIX `localtime_r`/`tm_gmtoff`) evaluated per instant, with whole-minute offsets.
- Ops and settings: `SET_WATERFALL_SPEED` (index into `speedLadderNs()` or a slider through `periodForSlider`, or "fit 24 hours"), `SET_WATERFALL_COMBINE` (Max / Average), `SET_WATERFALL_TIMES` (Off / UTC / Local). Settings `waterfallSpeed`, `waterfallCombine`, `waterfallTimeStamps`.
- Strings: "Waterfall speed", "Every spectrum", "1 line per second" ... (generated from the ladder: "{n} s per line", "{n} min per line"), "Fit 24 hours", "Combine lines by", "Maximum", "Average", "Time stamps", "Off", "UTC", "Local".

## Part 6: MQTT publisher, decode JSON and webhook

**MQTT (`mqtt::Publisher`, OASIS 3.1.1).** CONNECT with client id (default "foxsdr-" + 8 hex digits), optional user name and password (the password flag only together with a user name, 3.1.2-22), keep-alive and PINGREQ (sent after half the interval idle; no PINGRESP within the interval drops and re-makes the connection), PUBLISH at QoS 0 and 1, the retain flag, PUBACK handling with a window of `maxInflight`, re-send with DUP set and the same packet identifier after
`ackTimeoutMs` (three tries, then the connection is dropped) and again after a reconnect, clean DISCONNECT at `stop()` (it first tries up to `flushMs` to deliver what is queued), reconnect with doubling back-off (`backoffMinMs` to `backoffMaxMs`, reset after a CONNACK). **PLAIN TCP ONLY: no TLS** (the block links no TLS library): the user name and the PASSWORD CROSS THE NETWORK IN CLEAR TEXT: LAN or a local TLS proxy only; the settings text must say so.
**Off by default** (`enabled = false`: `start()` returns false, `publish()` returns Disabled, the network is never touched). Hand-over: `publish()` is called from ONE producing thread and is **wait-free and allocation-free** (proved: 0 `operator new` over 5,000 calls into a full ring including refusals): a bounded ring of fixed slots (default 256 x 2,048 bytes) with a per-slot atomic state; when the broker is away the OLDEST message is overwritten and counted (`droppedOldest`); in the rare
instant the worker is copying the oldest slot the NEW message is dropped instead (`droppedNewest`). All socket work is on the publisher's own thread. The reader accepts only CONNACK, PUBACK and PINGRESP and stores at most two bytes (a hostile length cannot allocate: `sizeof(BrokerReader)` is 16 bytes or less); anything else, a wrong length or a five-byte length is a protocol error: the connection is closed and re-made.
A message is not queued for the wire by an unstarted publisher except in the ring (tests use that to prove the ring).

**Decode JSON (`decodejson::toJson`).** `{"plugin","time" (ISO 8601 UTC, ms),"unix_ms","frequency_hz" (omitted when not finite),"text","truncated" (only if cut),"fields":{...}}`; RFC 8259 escaping, DEL and controls as \u00XX, invalid UTF-8 (overlongs, surrogates, above U+10FFFF, truncated sequences) becomes U+FFFD so the output is always valid JSON in valid UTF-8; text cut on a character boundary at 2,000 bytes by default; duplicate field keys keep the first; non-finite numbers are null.
**Webhook (`webhook::send`).** HTTP POST of that JSON through the existing `core::postBounded` (https anywhere; plain http ONLY to loopback: a LAN webhook therefore needs https, see the owner decisions); off by default; blocking (worker thread); the URL (it may hold a token) never appears in an error text.

**Proof (`test_mqtt_publisher`, 2,370 checks; `test_decode_webhook`, 1,587,787 checks).** Every network test talks to a stand-in broker or HTTP server on 127.0.0.1 inside the test. Codec: the Remaining Length table of the standard (0, 127, 128, 16,383, 16,384, 2,097,151, 2,097,152, 268,435,455), CONNECT, PUBLISH (QoS 0, QoS 1 + retain, DUP, empty payload),
PUBACK, PINGREQ and DISCONNECT byte for byte against bytes worked out by hand; every topic and string rule (wildcards, empty, U+0000, 65,536 bytes, bad UTF-8, a surrogate, QoS 2, identifier 0, empty client id with CleanSession 0). Reader: good packets split at every byte position; 14 hostile inputs (CONNACK of length 0, 1, 3; a PUBLISH of 268,435,455 bytes; a CONNACK claiming that length; a five-byte length; flags set; a SUBACK;
reserved types; reserved acknowledge-flag bits) are errors raised within 5 bytes, the reader stays dead afterwards, and 2,000 random 0 to 11 byte strings never crash. Publisher: CONNECT fields as seen by the broker (client id, user, password, keep-alive, flags 0xC2), QoS 0/1 packets, two acks; refusals 4, 5 and 2 give the broker's reason in the error text and a back-off of 40 / 80 / 160 ms gaps (the test's scaled values); 8 hostile
brokers (CONNACK of length 3 and 1, a 268,435,455-byte PUBLISH instead of CONNACK, a five-byte length, HTTP garbage, a huge length after a good CONNACK, a SUBACK, a second CONNACK) each give protocol errors, a reconnect, and a prompt `stop()`; accept-and-close, a half CONNACK, a broker that never answers (the 150 ms CONNACK timeout, `stop()` under a second); a hostile broker followed by a good one still delivers.
QoS 1: no ack for the first publish gives a re-send with DUP set and the same identifier, then an ack; never acked gives three tries and a reconnect; a connection dropped after an unacknowledged publish gives a re-send with DUP and the same identifier on the second connection; an ack for an identifier nobody holds is ignored; 40 messages through a window of 3 all acknowledged. Keep-alive: PINGREQ about every half interval, no PINGRESP gives a reconnect, interval 0 gives none. A broker that is away:
1,000 messages into 64 slots gives exactly **936 dropped (oldest first)** and, when the broker appears, exactly the newest 64 in order. Stress: 30,000 messages from a producer thread into 128 slots: 6,293 delivered, 23,423 dropped oldest, 284 dropped newest (the rare path), strictly increasing order, no torn payload, delivered + dropped = offered. The JSON against nlohmann::json over 6,000 random events (alphabet of quotes, backslashes, controls, DEL, separators, 2-, 3- and 4-byte characters) and 4,000 random byte strings; the webhook: off sends nothing, a 200 arrives byte for byte with `Content-Type: application/json`, 400 and 429 are "HTTP n" without the URL, a refused port, a cancel after 300 ms returns after 309 ms, scheme, space, CRLF, 3,000-character and non-loopback http URLs are refused before any connection.

**Break-it:**

| Removed or broken | RED |
|---|---|
| DUP not set on the re-send after a reconnect | 1: the dropped-connection re-send check |
| QoS bits left out of the PUBLISH flags | 18 |
| Retain bit left out | 2 |
| Reader's length check off | 11 |
| Back-off not doubling | 3 |
| Wildcard check off | 5 |
| Dropped-oldest not counted | 3 |
| DISCONNECT not sent at stop | 1 |
| JSON: quote escape off / control characters not escaped / invalid bytes passed through / no character-boundary cut / duplicate keys kept / surrogates allowed / overlongs allowed | 4,570 / 7,609 / 3,751 / 1 / 1 / 19 / 16 |

(A first attempt at the "invalid bytes become U+FFFD" break-it looped forever instead of failing; a test of the real code cannot hang, the mutant did. The backslash-escape mutant was not run: its pattern did not match the quoting used and it was dropped.)

**Where a broker password would be stored (owner note).** Nowhere yet: the block takes it as an option. `core/config.hpp:1414` `testerToken` is stored in the settings file in plain text, and the web API password is a PBKDF2 record (`config.hpp` near 1290). If the wiring saves the MQTT password it would be the second plain-text secret in `config.json` (the Influx token of batch 2 would be the third); options: environment variable only, the Windows credential store, or the settings file with a warning. The webhook URL may also hold a secret.

### WIRING HAND-OFF (part 6)
- Existing: the plugin decode feed (the engine's decode events from the plugins) was NOT examined in this batch: the wiring lane must find where a decode reaches the host (`engine/plugin_ui_port`, the problem and feed code) and fill a `decodejson::DecodeEvent` there. The producing thread must be one thread (publish is single-producer); never the DSP thread's inner loop.
- Ops and settings: `SET_MQTT` (enabled, host, port, client id, user name, password, keep-alive, topic prefix, QoS, retain), `SET_WEBHOOK` (enabled, URL); settings `mqttEnabled=false`, `mqttHost`, `mqttPort=1883`, `mqttClientId`, `mqttUser`, `mqttPassword` (see the note above), `mqttTopicPrefix="foxsdr/decode"` (topic = prefix + "/" + plugin id, which must be a valid topic name), `mqttQos=0`, `mqttRetain=false`, `webhookEnabled=false`, `webhookUrl`. Controls: a settings page with an off-by-default switch, the fields, a status line (`connected()`, `lastError()`, dropped counts from `stats()`), and a visible "unencrypted" warning next to the password.
- Strings: "MQTT", "Publish decodes to an MQTT broker", "Broker host", "Port", "Client ID", "User name", "Password", "Keep-alive (s)", "Topic prefix", "Quality of service", "Retain", "Connected", "Not connected", "Dropped (broker away)", "Unencrypted: the password is sent in clear text; use a trusted network", "Webhook", "Send decodes to a web address (https)", "URL", "Plain http is only allowed to this computer".

## Part 7: rotator control (`rotator::`)

**What it is.** (1) rotctld client: `P az el`, `p`, `S`, `K` and the `RPRT n` reply (default protocol, default port 4533), strict number parsing (finite, whole string), `RotctldLink` over a `Transport` (write, read until a delimiter with a timeout) and a `TcpTransport` (loopback-tested; a 512-byte line cap closes a connection that never ends a line). (2) GS-232A/B: `W aaa eee` / `M aaa` formatter (three digits, rounded, clamped to 0..360 or 450 and 0..180), `S`, `C`, `C2`, `B`, `Xn`,
reply parser for `+0aaa+0eee`, `+0aaa` and the unverified B form `AZ=aaa EL=eee`, the `?>` refusal, `Gs232Link` with the serial port behind the `Transport` interface (the GS-232 has no park command: park is a move to the configured park position). (3) `Tracker` and `TrackerDriver`: from a stream of (time, target az, el) it commands the rotator with a DEAD BAND (no command until the target is `deadBandDeg` from the last commanded position), a MINIMUM INTERVAL,
the nearest reachable azimuth (`nearestAzimuth`: the same compass direction within the rotator's range nearest the current position: 0..360 has one choice, 0..450 two for 0..90; the first command uses the measured position as its reference), elevation limits (below `minTargetElevationDeg` nothing is sent; above the rotator's limit it is clamped), a FLIP for rotators whose elevation reaches 180 (az + 180, 180 - el, chosen when it
saves more than `flipHysteresisSec` seconds of travel at the configured az and el speeds; the first command is never flipped), PARK at the end (once), ABORT (stop, once), and a failed command is retried at the next update (the tracker returns to the position the rotator really holds). **UNVERIFIED ON HARDWARE: no rotator exists here.**

**Proof (`test_rotator`, 226 checks).** Formatters byte for byte; the rotctld link against a stand-in daemon (set, get, stop, park; RPRT -1 and -11 reported; silence; closed channel; garbage; half a position; seven malformed get_pos pairs including NaN, infinity and 1e999); the GS-232 link against a stand-in controller (set, get in the A and B forms, stop, park as a move, a `?>` refusal, a stray LF before data, silence, azimuth-only M and C); the TCP transport against a real loopback
stand-in (set, get, park, four lines counted; a daemon that closes after the first line; a daemon that streams 2,000 bytes without a newline closes the connection; a daemon that never answers returns at the 200 ms timeout). Tracker rules table; shortest way: a 0..360 rotator crossing north from 352 to 32 degrees must swing back the long way (largest single move 359 degrees), a 0..450 one continues (largest move 1.0 degree, ends at 392); the first command from a measured 400 goes to 410, not to 50. **Simulated passes**
(800 km circular orbit, rotator 6 deg/s in azimuth and 2.7 deg/s in elevation, dead band 2 degrees, 2 s between commands, one target sample per second; pointing error between the rotator and the satellite, worst / mean in degrees):

| Pass (max elevation) | A: az 0-360, el 0-90 | B: az 0-360, el 0-180 with flip | C: az 0-450, el 0-90 | D: az 0-450, el 0-180 with flip | commands (A / B) |
|---|---|---|---|---|---|
| overhead, 0.0 deg off zenith (90.0) | 13.5 / 1.2 | **2.0** / 0.9 (1 flip) | 10.5 / 1.1 | 10.5 / 1.1 (0) | 85 / 85 |
| 0.2 deg (88.2) | 19.7 / 1.4 | **3.3** / 1.0 (1) | 4.1 / 1.0 | 4.1 / 1.0 (0) | 92 / 92 |
| 0.5 deg (85.5) | 25.3 / 1.7 | **8.6** / 1.0 (1) | 2.0 / 0.9 | 2.0 / 0.9 (0) | 96 / 96 |
| 3 deg (64.6) | 83.5 / 4.1 | 38.1 / 1.7 (1) | 2.3 / 1.0 | 2.3 / 1.0 (0) | 96 / 96 |
| 9 deg (32.4) | 176.1 / 6.4 | 176.1 / 6.4 (0) | 2.8 / 1.1 | 2.8 / 1.1 (0) | 70 / 70 |
| 17 deg (13.1) | 169.1 / 8.1 | 169.1 / 8.1 (0) | 173.6 / 8.1 | 173.6 / 8.1 (0) | 53 / 53 |
| 14 deg through north (18.7) | 2.4 / 1.0 | 2.4 / 1.0 (0) | 2.4 / 1.0 | 2.4 / 1.0 (0) | 59 / 59 |

Reading it honestly: the flip rescues a 360-degree azimuth rotator on near-overhead passes (A -> B above), but a 450-degree azimuth rotator (C) already follows most passes because the overlap lets it keep turning, so the flip adds nothing there; the rows with 100+ degree errors are passes that must cross a rotator's travel limit (north for the 360 rotators; the 450 limit for the 17-degree pass): the tracker takes the nearest reachable azimuth and the
rotator has to unwind; avoiding that needs a look-ahead scheduler that pre-positions the rotator (not built). Never a command outside the limits and never two commands closer than the interval in any run. The tracker's park puts the simulated rotator at the park position and counts one park. A link that fails the first two commands is retried at the next update; a failure after a good move re-sends the move.

**Break-it:**

| Removed or broken | RED |
|---|---|
| Nearest azimuth replaced by "last candidate" | 3 |
| Dead band off | 5 |
| Minimum interval off | 16 |
| Flip never chosen | 6 (the near-overhead rows) |
| Park returns nothing | 4 |
| rotctld set-position arguments swapped | 7 |
| TCP 512-byte line cap off | 1 |
| GS-232 angles not zero padded | 9 |
| GS-232 parser accepts angles beyond the ranges | 2 |
| Restore of the last position after a failed move off | 1 |
| `retryNow` also clearing the interval timer | equivalent mutant (the restore already makes the next update free to send) |

### WIRING HAND-OFF (part 7)
- Existing: no rotator code; the targets come from the satellite and aircraft tracking (`plugins` / map code, not examined here): the lane must find a source of (az, el) per second for the chosen target, and a monotonic clock.
- Needs: a serial `Transport` for the GS-232 (COM port; the speed and framing were NOT in the extracted manual text: 9600 baud 8N1 is the usual setting but UNVERIFIED), a worker thread that calls `TrackerDriver::step` about once a second (the transports block up to their timeouts: never the DSP or GUI thread), `begin()` at the start of a track and `finish()` (park) at its end, `abort()` on a user stop.
- Ops and settings: `SET_ROTATOR` (type rotctld / GS-232A / GS-232B, host and port or COM port, azimuth range 360 or 450, elevation range 90 or 180, azimuth only, dead band, minimum interval, park azimuth and elevation, enabled), `ROTATOR_TRACK` (target), `ROTATOR_STOP`, `ROTATOR_PARK`. Settings `rotatorEnabled=false`, `rotatorType`, `rotatorHost`, `rotatorPort=4533`, `rotatorSerial`, `rotatorAzMax=360`, `rotatorElMax=90`, `rotatorDeadBandDeg=2`, `rotatorIntervalSec=2`, `rotatorParkAz`, `rotatorParkEl`. Controls: a rotator page with a status line (`lastError()`, the commanded and measured position).
- Strings: "Rotator", "Rotator type", "rotctld (Hamlib)", "Yaesu GS-232A", "Yaesu GS-232B", "Host", "Port", "Serial port", "Azimuth range", "Elevation range", "Dead band (degrees)", "Minimum time between commands (s)", "Park position", "Track", "Park", "Stop", "Rotator not responding", "Not verified on real hardware".

## Tests run (this machine, Release, 4 jobs)

Build: `cmake --build D:/foxsdr-build/feat-blocks --config Release --parallel 4 --target <tests>` (one re-configure for the new test files); only `cascade_engine` and the test executables were built, no Slint window, no application run, no radio opened.
Own checks (final): spectrum_block 62,746; notch_bank 98; cw_pitch 162; colour_maps 4,432; waterfall_timing 20,768; mqtt_publisher 2,370; decode_webhook 1,587,787; rotator 226; all 0 failed. The MQTT test was run three times in a row green (12.5 s each).

All block tests of the three batches together (29 tests: the first batch's sixteen, the second batch's five, these eight), the command, run twice:
`ctest -C Release -j4 -R "^(test_agc_modes|test_noise_blanker|test_squelch|test_recorder|test_demod_agc|test_record_start|test_record_finish_async|test_stop_ends_recordings|test_pipeline_agc|test_patch_squelch|test_ctcss|test_dcs|test_replay_ring|test_clip_recorder|test_smeter_cal|test_adif|test_sam|test_passband|test_nr_methods|test_iq_readers|test_power_log|test_spectrum_block|test_notch_bank|test_cw_pitch|test_colour_maps|test_waterfall_timing|test_mqtt_publisher|test_decode_webhook|test_rotator)$"`
- run 1 (about 17:22): `100% tests passed, 0 tests failed out of 29`, total test time 88.62 s (`test_record_finish_async` 30.07 s, `test_mqtt_publisher` 12.47 s, `test_cw_pitch` 10.02 s, `test_spectrum_block` 6.20 s).
- run 2 (about 17:24): `100% tests passed, 0 tests failed out of 29`, total test time 85.98 s.
No failure occurred, so no flake or real failure to classify. (The known flake of batch 2, `test_record_finish_async` under load, did not show in these two runs.) ThreadSanitizer was not run (MSVC build): the spectrum block's engine hand-over and triple buffer, the MQTT ring and the notch bank's atomics are covered by their stress tests only.

## Departures from the request, and why

1. **Part 1** uses its own window function (`specFillWindow`) rather than `dsp/window.hpp`, whose enum has no flat-top and which is shared by the pipeline; the FFT is the existing `ComplexFFT`. The flat-top coefficients beyond four digits are from memory (see Sources).
2. **Part 2** adds a 10 ms fade and a 3 ms glide (not asked for) because the bare `Notch` clicks (1.351x the tone's own step) when retuned or switched; the depth pin is 100 dB (measured 144 to 155), not the 40 dB floor of the request.
3. **Part 3** does not edit the demodulator (asked): the hand-off names the lines. The helper makes the filter CENTRED on the pitch by choosing BFO = pitch, which means CW needs its own filter design instead of the voice filter's.
4. **Part 4**: the 8-bit tables are monotone in lightness and chord-equal in the float maths, but single 8-bit steps are not equal (a property of 8 bits near black, stated and measured); the "near-constant step" is pinned over windows of 16 entries (0.738 to 1.061 of the mean).
5. **Part 5**: `Zone` models UTC, fixed offsets and the EU and US rules only; the real OS zone is for the wiring.
6. **Part 6**: the webhook is limited by the existing client to https or loopback; MQTT QoS 2, subscribe, will and TLS are not built. The wait-free hand-over can, in a rare instant, drop the NEW message instead of the oldest (counted).
7. **Part 7**: no serial implementation (only the interface and a stand-in), and no look-ahead pre-positioning for passes that cross a travel limit.

## Owner decisions needed

- **Spectrum default and ceiling**: 8,192 bins default and a 131,072 ceiling (524,288 as an expert option)? Memory and CPU are in the table; a laptop was not measured.
- **Flat-top source**: accept the five-term set measured here (ENBW 3.7702, side lobe -93.01 dB), or have the Heinzel 2002 coefficients checked against the paper (it could not be fetched).
- **Colour maps**: add viridis, inferno, magma and plasma by name (CC0: no condition), and turbo (Apache-2.0: ships with its notice)? Or keep only the five computed maps.
- **MQTT / webhook secrets**: where the broker password and the webhook URL live (settings file in plain text like `testerToken`, an environment variable, or the Windows credential store); and whether a LAN webhook over plain http is allowed (the existing client refuses it; relaxing that rule is a security decision for the shared transport).
- **MQTT QoS and retain defaults** (QoS 0, no retain proposed) and the topic layout (`foxsdr/decode/<plugin>` proposed).
- **Rotator**: which hardware to support first (rotctld covers most), the GS-232B reply form to be checked on a real controller, the serial settings, and whether a look-ahead scheduler (pre-positioning for passes that cross a travel limit) is wanted.

## UNVERIFIED list

- Everything about the rotator on hardware: rotctld against a real daemon, GS-232A and B against real controllers (the B reply form `AZ=aaa EL=eee`, the CR / LF framing of replies, the serial speed), the flip on a real 0-180 elevation rotator, the rotator speeds used in the simulation (6 and 2.7 deg/s are G-5500-like figures from memory).
- MQTT against a real broker (mosquitto, a cloud broker): only stand-ins; section 4.4 of the standard was not read; behaviour with a persistent session (CleanSession 0) is the client side only; POSIX socket paths of the shared helpers (not compiled here).
- The webhook against a real HTTPS endpoint (loopback only was tested) and against a LAN host.
- The flat-top coefficient digits beyond the fourth; the Harris coefficients against the paper; the IEC 61966-2-1 constants; the OKLab reference L a b of sRGB red (tolerance 2e-3); the Machado matrices applied to linear sRGB.
- The spectrum block and the colour maps on real signals and real displays (all synthetic); the cost table on a laptop-class CPU; the waterfall timing against a real OS time zone and a real clock (simulated clock only).
- ThreadSanitizer (not run: MSVC).
