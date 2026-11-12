# Lane `rfmon` - spectrum monitoring and measurement blocks (record)

Branch `feat/rfmon`, created from engine/multi-radio `d3630d78`. NEW-FILE MODEL LANE: five stand-alone blocks and their tests, nothing wired, no
existing file edited (not even `tests/CMakeLists.txt`: tests are globbed). No version string changes (none old, none new).

## Step 0 - survey table (read-only, done first)

| Row (REMAINING-GAPS "Spectrum monitoring and measurement" + DEPTH-GAPS A2) | Verdict | Evidence / reason |
|---|---|---|
| RBW / VBW readout and a selectable detector (peak, RMS, average, sample, quasi-peak) | **new file block** (`src/dsp/rbw_detector.hpp`) | `feat/signal-blocks-3` `spectrum_block.hpp` already has FFT size 1,024..524,288, five windows, average / peak hold / min hold and max-per-column reduction: those are NOT redone. It has no RBW readout, no detector choice and no VBW. `core/trace_hold.hpp` (peak / average hold) is a forbidden file and is not a detector either. The new block takes PSD frames, so it sits behind either. Quasi-peak: NOT blocked: the time constants are on two free pages (below); the published-constant model is built, conformance to the CISPR curves is not claimed (paid standard). |
| Spectrum limit lines / frequency mask with pass-fail, alarm, I/Q capture or log line on breach | **new file block** (`src/core/spectrum_mask.hpp`) | Nothing of the kind exists in the tree (grep for mask / limit line: none). Wiring to the main spectrum needs forbidden files (`spectrum_block.hpp`, screen): hand-off below. The Survey Engine version is the `survey-maps` plugin lane's; no file shared. |
| Antenna-factor, cable-loss and path-loss tables with a dBuV/m readout | **new file block** (`src/core/field_strength.hpp`) | Nothing in the tree. |
| Pseudo-Doppler direction finding, one receiver, switched four-antenna array | **new file block** (`src/dsp/pseudo_doppler.hpp`) | Nothing in the tree. The antenna switch is hardware: not built, hand-off. |
| Multi-radio: Matrix overview with hover-to-listen (SDR Console) - models only | **new file block** (`src/core/radio_matrix_model.hpp`) | No existing summary table; `gui/hover_dwell.hpp` is a tooltip clock, not this. Hover-to-listen is defined from an opened maker page. |
| Multi-radio: receivers that follow a master (UNVERIFIED in the list) | **defined and built** | An opened non-GPL manual (SDRuno v1.21) defines it for its virtual receivers (section 2.3 and 4.6); see the model's header and UNVERIFIED below. |

## What was built

1. `src/dsp/rbw_detector.hpp` (+ `tests/rfmon_gen.hpp`, `tests/test_rbw_detector.cpp`), commit `8a21873b`.
   - Window ENBW in closed form (`rbwEnbwBins`), RBW readout (`rbwPlan`: native ENBW, or M combined bins, a request finer than the frame refused and flagged),
     `rbwFftSizeFor` (which FFT length gives a wanted RBW), worst-case tone under-read in dB computed from the window (`rbwToneLossDb`).
   - `RbwTrace`: per display point over bins and frames: Peak, NegPeak, Rms (mean power), AvgVolt, AvgLog, Sample; VBW video low-pass on successive traces (log or power).
   - `QuasiPeakDetector`: charge / discharge / meter time constants per CISPR band (A 45/500/160 ms, B 1/160/160 ms, C/D 1/550/100 ms), frame period must be at most half the charge constant.
2. `src/core/spectrum_mask.hpp` (+ `tests/test_spectrum_mask.cpp`), commit `ba928cf2`. Versioned text mask (`foxmask 1`; `upper` / `lower` segments), `MaskEvaluator`
   (pass/fail, worst margin, offending bin and frequency, breach bin count, skipped non-finite bins), `MaskMonitor` (raise/clear debounce, bounded newest-last event list of 64,
   `formatBreachLine` with no identifiers), `CaptureTrigger` (the event record for "capture I/Q on breach", with hold-off). The capture itself is the recorder's.
3. `src/core/field_strength.hpp` (+ `tests/test_field_strength.cpp`), commit `243e6769`. User-supplied CSV `LossTable` (strictly increasing frequency or refused with its line; interpolation
   linear in dB against log f; end values held AND flagged), dBm to dBuV, antenna factor from gain, free-space path loss, Friis, field of an EIRP source, and `fieldStrengthFromTables`
   (level + antenna factor + cable - preamp, with held-value flags). No manufacturer data shipped.
4. `src/dsp/pseudo_doppler.hpp` (+ `tests/test_pseudo_doppler.cpp`), commit `b38587b6`. `estimatePseudoDopplerBearing`: block-wise boundary alignment that follows a clock that runs fast or slow,
   in-dwell carrier-offset estimation, phase steps per step position summed over rotations, zero-sum removal of the leftover frequency term, bearing = atan2(u - v, u + v) (the ring radius drops out), quality figure.
   Sign convention stated in the header: bearing = direction towards the transmitter, counter-clockwise from antenna 0, 90 = towards antenna 1.
5. `src/core/radio_matrix_model.hpp` (+ `tests/test_radio_matrix_model.cpp`), commit `95355c0f`. `RadioMatrix` (per-radio summary table, up to eight rows, numbers and enums only),
   `matrixGridFor`, `HoverListen` (dwell, grace, baseline, muted skipped), `FollowSet` (offset-keeping follow with lock, chains, cycle refusal, range clamping).

## Tests (real output; official build `D:/foxsdr-build/en-rfmon`, Release, run through ctest with `--timeout 300`)

```
171: test_field_strength: 50471 checks, 0 failed
373: test_pseudo_doppler: 201 checks, 0 failed
375: test_radio_matrix_model: 1127 checks, 0 failed
388: test_rbw_detector: 261483 checks, 0 failed
462: test_spectrum_mask: 45054 checks, 0 failed
100% tests passed out of 5
```

Measured numbers worth keeping (all from the test output):

- Pseudo-Doppler bearing error against SNR per sample (36 bearings x 4 seeds, 128 dwells of 100 samples, carrier offset 0.012 rad/sample, clock error 0.1 percent):
  30 dB rms 0.01 deg worst 0.03; 10 dB rms 0.12 worst 0.29; 0 dB rms 0.39 worst 0.97; -5 dB rms 0.73 worst 1.83, quality 0.93;
  -10 dB rms 69.29 worst 180.00, quality 0.58; -15 dB rms 116.00, quality 0.15. **Working limit about -7 dB per sample: below it the bearing is noise and the quality figure says so (pinned).**
  Clean-signal error under 0.5 degree at every 15 degrees; offsets up to 0.04 rad/sample, clock error up to 0.4 percent over 400 dwells (the boundary slides 160 samples), a start 49 samples late, fractional (97.5) and short (24) dwells all stay within 3-4 degrees.
- RBW: Hann ENBW 1.5 bins and rectangular 1.0 (pinned against the Audio Precision values), every window's closed form equals a numeric sum to 2e-5; a single-bin Hann tone between bins reads 1.4236 dB low and the plan says so; noise reads density x RBW (native ENBW and 8 and 32 combined bins) within 0.08 dB; voltage average -1.05 dB and log average -2.51 dB under the RMS reading (closed forms, within 0.1 dB); mean of the max of 16 frames = H_16 within 6 percent, of the min = 1/16 within 0.008.
- Quasi-peak pulse train equals the closed-form steady state to 1e-6 at 1000, 100, 10 and 1 pulses per second (band C/D, 0.5 ms frames: 0.9987, ..., 0.437) and falls with repetition rate.
- Field strength: dBm to dBuV 106.9897 at 0 dBm / 50 ohm; antenna factor equals 9.73 / (lambda sqrt G) within 0.01 dB at four frequencies and three gains; FSPL constants -147.5522 and 92.4478; **closed loop** EIRP -> Friis -> volts -> + antenna factor = the radiator's field within 1e-6 dB for four frequencies, three distances, two receive gains.

## Defects the tests found in the new code (fixed before each commit)

- Mask loader: `strtod` accepted hex floats (`0x10` read as 16): decimal notation only now (found by the parse-error test). Same guard in the loss-table loader from the start.
- Tone loss for a single bin was divided by the ENBW (wrong for the native reading): now 1.42 dB for Hann, 3.92 dB for rectangular (the textbook scalloping values), test pinned.
- Pseudo-Doppler alignment and frequency estimate lost lock at -5 dB per sample in the first version (rms 86 degrees): windowed boundary steps and lags stepped by three moved the limit down 5 dB.
- A test expectation of mine was wrong, not the code (QP at 5 pulses per second reads 0.80 of the peak, not under 0.3): corrected from the closed form, the test now pins the closed form instead.

## Break-it table (each mutation applied to the header, the test rebuilt and run, then restored; `git status` clean after each)

| Block | Mutation | Result |
|---|---|---|
| rbw | native reading scaled by bin width instead of ENBW | RED: 5 failed (`top=-3.18 vs -1.42`, noise level off by 1.74 dB) |
| rbw | voltage average computed as the power mean | RED: 1 failed (`av - rms=0 vs -1.049`) |
| rbw | VBW alpha forced to 1 | RED: 1 failed (`alpha=1 vs 0.4665`) |
| rbw | quasi-peak charge and discharge constants swapped | RED: 6 failed (`top=0.0023 vs 0.9986`) |
| mask | margin sign of the upper limit reversed | RED: 12 failed |
| mask | alarm raised on the first failing frame (no debounce) | RED: 7 failed |
| mask | capture hold-off removed | RED: 1 failed (`!mon.takeTrigger().pending`) |
| field | dBm to dBuV constant 90 -> 87 | RED: 28 failed |
| field | interpolation linear in f instead of log f | RED: 2 failed |
| field | strictly-increasing check removed from `setPoints` | RED: 1 failed |
| field | strictly-increasing check removed from the CSV parser | RED: 499 failed (the error-path and fuzz checks) |
| pseudo-Doppler | bearing atan2 arguments swapped (sign convention) | RED: 50 failed |
| pseudo-Doppler | carrier-offset estimate forced to zero | RED: 19 failed |
| pseudo-Doppler | boundary alignment forced to the nominal clock | RED: 10 failed (clock-error cases) |
| matrix | hover dwell removed | RED: 3 failed |
| matrix | follower lock ignored | RED: 2 failed |
| matrix | cycle check removed | RED: 1 failed |

## Gates

- Own tests: 5 of 5 pass (above).
- Headless set: see the section "Headless gate" at the end of this file (44 first-pass failures in existing tests, none attributable to this lane; details there).

## Wiring and screen hand-off (nothing here is wired)

All strings are English, no translation made. Engine-side hooks need the forbidden files (`engine.cpp`, `pipeline.cpp`, `spectrum_block.hpp`, `fox_engine_api.cpp`), so they belong to the wiring lane.

### RBW / VBW / detector (spectrum display)
- Engine: feed `RbwTrace::pushFrame` one PSD frame per FFT (W/Hz: `rbwBinPsd` from raw |X|^2, the spectrum block's `SpecScale::Psd` is the same quantity), call `takeTrace` once per displayed trace with the interval since the previous one; per radio.
- Controls (spectrum right-click menu or the display panel):
  - "Resolution bandwidth" (list: Auto = the window's own ENBW, then 1, 3, 10, 30, 100, 300 Hz, 1, 3, 10, 30, 100, 300 kHz, 1 MHz; a value below the FFT's ENBW is shown as the ENBW and flagged), default Auto.
  - Readout line: "RBW {value} ({window})"; when `RbwPlan::toneLossDb` is above 0.1 add "tones read up to {x} dB low at this RBW".
  - "Detector" (Peak, Negative peak, RMS, Average (voltage), Average (log), Sample, Quasi-peak), default RMS.
  - "Video bandwidth" (Off, then 1, 3, 10, 30, 100, 300 Hz, 1 kHz ...), default Off; "Video filter on" (Log, Power), default Log.
  - "CISPR band" (A 9-150 kHz, B 150 kHz-30 MHz, C/D 30 MHz-1 GHz), shown only with Quasi-peak; the message "Quasi-peak needs a frame every {x} ms or faster" when `qpFramePeriodOk` is false.
- Facts to expose: `RbwPlan.rbwHz`, `.combine`, `.toneLossDb`, `.clamped`.

### Limit lines / mask (spectrum display)
- Engine: `MaskEvaluator::prepare` when centre / span / bin count change, `evaluate` per frame, `MaskMonitor::update` with the frame time; on `takeTrigger().pending` ask the recorder (`pre-roll`, `post-roll`) to keep the I/Q around `atS`; log `formatBreachLine` at most once per event (open and close).
- Controls: "Limit lines" (off by default), "Load limit file..." (file chooser: `foxmask 1` text), "Raise alarm after {n} frames" (1-100, default 3), "Clear alarm after {n} frames" (1-100, default 5), "Capture I/Q on breach" (off), "Pre-roll {s}" (0-5, default 0.5), "Post-roll {s}" (0-60, default 2.0), "Minimum time between captures {s}" (0-3600, default 10).
- Status text: "Limit: PASS, worst margin {x} dB" / "Limit: FAIL, {upper|lower} margin {x} dB at {f} MHz"; load errors from `maskErrorText` (+ line number).
- Drawing: `MaskEvaluator::upperLimit(i)` / `lowerLimit(i)` per bin (NaN = no line).

### Field strength (spectrum readout)
- Controls: "Antenna factor table..." and "Cable loss table..." (CSV, see header), "Table frequency unit" (Hz, kHz, MHz, GHz; default MHz), "Preamp gain {dB}" (default 0), "Input impedance {ohm}" (default 50), "Show field strength" (off).
- Readout: "{x} dBuV/m at {f} MHz" and, when `afHeld` or `cableHeld`, "table does not reach this frequency: end value held".
- Errors: `lossErrorText` + line.

### Pseudo-Doppler
- Hardware (NOT built): a four-way antenna switch stepping 0, 1, 2, 3 once per dwell from a clock, and a way to tell the program where antenna 0's dwell starts (a sync pulse, or the switching clock is derived from the receiver's sample clock). Ring radius up to about 0.3 wavelength.
- Engine: collect at least 8 dwells (2 rotations; 128 dwells reads well) of I/Q from the radio, call `estimatePseudoDopplerBearing(x, n, samplesPerDwell, startSample)`; the result is a bearing, a quality, the removed carrier offset and the clock drift.
- Controls (a plugin or the DF page): "Samples per dwell" (>= 8, default 100), "Antenna 0 starts at sample" (sync), "Mounting offset {deg}" (the array's zero against north; the model's bearing is counter-clockwise, a compass is clockwise: bearing_compass = (mount - theta) mod 360 is the screen's to define), readout "Bearing {deg} (quality {q})", warning when quality < 0.5: "Signal too weak or not steady for a bearing".

### Multi-radio overview (screen lane `screen-k`)
- Table columns: "Radio", "State", "Centre", "Tuned", "Mode", "Level", "Listening", "Muted", "Follows", "Overruns" (state words from `matrixRadioStateText`: closed, opening, running, busy, faulted).
- "Hover to listen" (off by default), "Hover delay {ms}" (0-2000, default 150), "Return delay {ms}" (0-5000, default 300), "Skip muted radios" (on). The route change goes through the existing solo / audio-route call with `HoverDecision::listenId` (-1 = the user's own choice).
- "Follow radio..." (menu listing the other open radios; records the offset at that moment), "Stop following", "Lock follower" (SDRuno's LO LOCK), "Keep this distance" (stores the nudged offset). Retunes come from `FollowSet::onMasterMoved`, applied front to back through the existing retune call; limits from the follower's own tuning range.

## UNVERIFIED (not proven here)

- No block was fed from the real spectrum path, a real receiver or an instrument: every proof is on generated signals through a test FFT. Nothing is wired.
- Quasi-peak conformance: the time constants come from two free pages; the CISPR 16-1-1 calibration pulse curves and the meter's critical damping are in the paid standard and NOT checked. Only the meter time constants of the first page were seen (the second lists charge and discharge only).
- The ENBW definition was pinned to the Audio Precision values (1.0 and 1.5) and to a numeric sum of each window; Harris (1978) and the Heinzel-Ruediger-Schilling report were NOT opened (the PDF returned HTTP 403, the LIGO page is an abstract).
- The dBuV/m formulas other than antenna factor and FSPL (dBm to dBuV, EIRP field) are derived from the definitions and cross-checked against each other by the closed loop; no instrument, no third page.
- Hover to listen: the SDR Console page states that the receiver is unmuted while the pointer is in it; dwell and grace are this model's own; what feels right is untested.
- Follow a master: SDRuno v1.21 (2017) describes virtual receivers inside one hardware stream; whether users of separate physical radios want delta-follow with lock is a product decision (owner list). Later SDRuno versions were not read.
- Pseudo-Doppler on real hardware: antenna gain and phase mismatch, multipath, switch leakage and the switch's real settling were not modelled; the simulated settling is two junk samples. One carrier only.
- Windows only was built (MSVC Release); no Linux run. No GUI test (none applies).
- `git` warns that LF will become CRLF in some new files on this machine (autocrlf); content is otherwise unchanged.

## Sources opened (2026-11-11), with licence or status

| URL | Used for | Licence / status |
|---|---|---|
| https://emccalc.com/articles/detector (redirect from emcweb.pages.dev) | QP charge / discharge / meter constants per band, behaviour description | no licence stated; facts only |
| https://rfessentials.com/resources/rf-glossary/cispr-detector/ | second source for QP charge / discharge; CW reads the same on QP, peak, average | no licence stated; facts only |
| https://www.audioprecision.com/blog/fft-spectrum-and-spectral-densities-same-data-different-scaling/ | ENBW of Hann 1.5 and rectangular 1.0 | vendor blog; facts only |
| https://en.wikipedia.org/wiki/Antenna_factor | AF = E/V, 9.73 / (lambda sqrt G) | CC BY-SA 4.0, facts only |
| https://en.wikipedia.org/wiki/Free-space_path_loss | FSPL formulas, -147.55, 92.45, Friis | CC BY-SA 4.0, facts only |
| https://www.sdr-radio.com/receivers | Audio preview (unmute while the mouse is in a receiver window), Matrix | (c) SDR-Radio.com Ltd, maker documentation; facts only |
| https://wtfda.org/wp-content/uploads/SDRs/SDRuno_User_Manual.pdf (copy of the maker's v1.21 manual) | master VRX, LO change moves all VRX, LO LOCK (sections 2.3, 4.6) | (c) SDRplay Limited 2017, documentation; read for facts, only short quotes; the file was kept by the fetch tool outside the repository |
| https://dcc-llo.ligo.org/LIGO-T1400010/public | confirmed the Heinzel et al. report exists; abstract only, no table | public abstract |
| https://holometer.fnal.gov/GH_FFT.pdf | tried: HTTP 403, not opened | - |
| https://incompliancemag.com/emi-measurement-receiver-requirements-cispr-16-1-1/ | tried: returned no text | - |
| https://appliedacousticschalmers.github.io/scaling-of-the-dft/notes_on_windows | opened: no ENBW values | - |
| https://www.sdrplay.com/docs/SDRplay_SDRuno_User_Manual.pdf, https://www.sdrplay.com/helpsys/pre318.php | tried: HTTP 404 | - |

No GPL / LGPL / AGPL source or port was read. No software, library, driver or script was downloaded (the SDRuno manual is a document; it was fetched by the web-reading tool and read as text with the system `pdftotext`).

## Owner decisions

1. RBW defaults: Auto (native ENBW) and detector RMS are proposals (RMS reads noise as a power; Peak is the usual "spectrum analyser" default and shows bursts).
2. Is the published-constant quasi-peak model acceptable under the name "Quasi-peak (CISPR)", or should it be labelled "Quasi-peak (approximate)" until checked against the standard?
3. Follow-a-master semantics (delta-follow with lock) and whether it applies to separate radios at all.
4. Hover dwell and return delay defaults (150 ms / 300 ms).
5. Whether the pseudo-Doppler hardware (a four-way antenna switch and its control) is wanted; without it the block has no input.

## Headless gate

Build: `cmake --build D:/foxsdr-build/en-rfmon --config Release --parallel 3`, exit 0 (611 targets; the full tree, built in slices of at most 500 s because the machine was shared).

Headless set: `ctest -C Release -L headless -j 3 --timeout 120`, 708 tests, run in nine index slices under 10 minutes each (the slices that were cut by my own 535 s limit were re-run over the missing range, so every index ran at least once). 664 of the 708 passed first time; **44 failed first time**, all in existing tests. THIS LANE ADDED NO LIBRARY CODE (five headers and five test programs; `cascade_engine` is built from byte-identical sources to the base apart from the commit stamp), so none of the 44 can come from it; they were re-run once, with `-j 2` and `--timeout 240` or 300, to classify (a baseline binary was NOT built to compare, so "not from this lane" rests on that reasoning plus the reruns, not on a side-by-side):

- Known false failures of a D: build (brief): `test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`: failed again, as expected. `test_window_failed_app`: failed again (configuration: CASCADE_BUILD_SLINT=OFF, as the brief says).
- Passed on the rerun (load): `test_plugin_api`, `test_bounded_hooks`, `test_scanner_draft_input_free`, `test_scanner_draft_input_stepped`, `test_soundcard_app_paths_stepped`, `app_selftest`, `test_engine_api_conformance`, and the other 14 of the 44 first-pass failures (`test_engine_api_saved_radios`, `test_engine_control_thread`, `test_engine_main_damaged_config`, `test_engine_main_smoke`, `test_engine_shell_guard`, `test_failure_recording`, `test_gps_reader`, `test_hackrf_source`, `test_pa_stream_list_guard`, `test_plugin_hostile`, `test_rtlsdr_live`, `test_sentinel`, `test_sentinel_proc`, `test_sdrplay_probe`, `test_soapy_enum_proc`, `test_soapy_source`, `test_soundcard_source`, `test_thread_beats`, `test_transmit_liveness`, `test_usb_bounded_walk`: all passed in the rerun of the first 36 of the 44).
- Still failing after the rerun, and why as far as seen: `test_engine_api_source_kinds` (0.05 s: "the fixture scope.wav is not in D:/foxsdr-wt/en-rfmon/src/slint/tests/drive/fixtures": a generated fixture absent from this fresh worktree; `test_engine_api_decode_radio` fails on the same kind of file source, radio 1 stays at 2000000 Hz instead of 48000); `test_engine_api_schedule` (an unwritable-folder case: "recorder: cannot create directory ..." at line 292) and `test_failure_files` / `test_excuse_cap` (disk-failure and report-count cases in the temp folder under OneDrive) - not investigated further; `test_crash_upload_free` (stand-in server connection count, 122 s, load); `test_pass_control`, `test_tx_audio`, `test_health_app_stepped` (the brief's load-flake list); `telemetry_worker` (240 s timeout, needs node and the worker tooling). **All of these are UNVERIFIED as baseline failures**: they were not run on a build without this lane. They are in the final report as "existing tests failing here, unexplained beyond the above".
- Window (gui) tests: not run (this lane has no window code).

`config.json` (`%APPDATA%oxsdr\config.json`) sha256 at the end: c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 (unchanged; no test of this lane reads or writes it; the only file the new tests write are their own `rfmon-*-test-*.tmp` files in the working directory, removed at the end of the test).
