# sweepsrc: sweep devices as a source, wide-band stitcher and heatmap, trace history files, occupancy model

Lane `sweepsrc`, branch `feat/sweepsrc` (from engine/multi-radio d3630d78). New-file model lane: nothing
existing is edited, `tests/CMakeLists.txt` included (the tests are found by its glob). No version string
changes. No radio, serial port or network was opened.

## Step 0: the table (reading only, done first)

Date of the reading: 2026-11-11. "Free-source check" = the maker's own published description, its terms, and
whether it sits inside a GPL/LGPL repository (closed to this lane).

| Row | Verdict | Why |
|---|---|---|
| tinySA sweep device as a source | **new file, built** (`scanraw` only) | The maker's USB-interface page (tinysa.org wiki, no GPL repository, no licence text on it) gives the exact `scanraw` request and binary reply. The text `scan` reply layout is NOT described there, so it is not built. |
| RF Explorer sweep device as a source | **blocked** (device frames) + **alternative built** | The only maker description of the serial protocol is the "RF Explorer UART API interface specification", a page in the wiki of the RFExplorer-for-.NET repository, which GitHub lists as LGPL-3.0. `rf-explorer.com/API` and the user manual both point only there. Closed to this lane. The manual (61 pages) holds no frame description. Alternative that IS built: `importFreqLevelPairs`, the import of the device's exported "frequency (MHz), amplitude (dBm)" text (format named by the maker's file-formats page). |
| Trace / sweep history as CSV, XML, JSON | **new file, built** | Our own documented schema (versioned), `src/core/sweep_history.*`. |
| Wide stitched sweep heatmap, click-to-tune | **new file, built** (model); **needs a forbidden file** for the screen and for feeding it from a live radio | `src/core/sweep_stitch.*`. Wiring into the main spectrum / a radio tuning loop touches `pipeline.cpp`/`engine.cpp`/`.slint`: hand-off below. |
| Occupancy / duty-cycle report | **new file, built** (host-side model, CSV export) | `src/core/sweep_occupancy.*`. |
| HackRF sweep mode | **blocked**, alternative built | See below. |
| Registering the tinySA as a source kind | **needs a forbidden file** (device registry/ledger) | The standalone source is built; the exact registration hand-off is below. |

### FREE-SOURCE CHECK, device by device

**tinySA / tinySA Ultra.** Opened: `https://tinysa.org/wiki/pmwiki.php?n=Main.USBInterface` (maker's wiki; no
licence or copyright notice on the page, only a "last modified" line; not inside a GPL repository). Used from
it, quoted exactly in `src/source/tinysa_protocol.hpp`: the `scanraw` usage line, the reply frame
`'{' ('x' LSB MSB)*points '}'` (16-bit little endian), the option word (1 unbuffered, 2 continuous), the
conversion "divide by 32 and subtract 128 for the tinySA and 174 for the tinySA Ultra". The tinySA firmware
(GPL) and every community wrapper were not read. NOT in the page, so not guessed and UNVERIFIED ON HARDWARE:
command terminator, echo and prompt before the `{` (the parser discards all bytes before it), reply format of
`version`/`info` (so the model is named by the caller), a scanraw point limit (we cap at 1 048 576), baud rate.
There is no checksum in the frame; its checks are the framing bytes and the exact point count asked for.

**RF Explorer.** Opened: `https://j3.rf-explorer.com/rf-explorer-file-formats` (copyright "All rights
reserved" RF Explorer Technologies SL, no licence text; says the formats may change; names "Export Single Signal
CSV" as one `[frequency (MHZ), amplitude (dBm)]` pair per line and the "Cumulative CSV" whose header contents it
does not show; the .rfe layout is not described); the user manual PDF (`version.rf-explorer.com/docs/RF Explorer
Spectrum Analyzer User Manual.pdf`: licence section, says the firmware is copyrighted, the Windows client is
open source (GPL v3 in that text, LGPL elsewhere), "protected by multiple patents", no frame description);
`https://j3.rf-explorer.com/rf-api-scpi.html` (links to the API page; client LGPL); `https://www.rf-explorer.com/API`
(301 redirect to the GitHub wiki, redirect only, the wiki was NOT fetched); `https://github.com/RFExplorer/RFExplorer-for-.NET`
(licence field only: LGPL-3.0). DISCLOSURE: a web search result summary quoted a few words of that wiki (that
sweep bytes are halved and negated, start/end in kHz, step in Hz); nothing from it was used, and no RF Explorer
decoder is built. The "patents" line in the manual is noted; it is not analysed. Patent-free alternative for the
same result: the user exports a sweep from RF Explorer's own program and imports it (built); the cumulative CSV
is not importable because its header is undocumented.

**HackRF sweep.** Opened: `https://hackrf.readthedocs.io/en/latest/hackrf_one.html` and `.../hackrf_tools.html`
(no licence text on the pages; the documentation is built from the project's repository, GPL-2 firmware / BSD
host, so it is treated as closed for protocol details). Neither page describes the sweep USB request or its
block framing (the tools page gives only the command-line usage and the `hackrf_sweep` CSV line layout);
`.../libhackrf_api.html` returned 404. A web search only turned up the GPL firmware file and third-party host
crates (not opened, not read). So the sweep request is described only inside GPL source: **blocked**, driver not
edited. The same result for the user (a spectrum wider than the sample rate) comes from the stitcher below,
fed by ordinary retuned captures through the driver as it is.


## What was built (all new files)

| File | What |
|---|---|
| `src/core/sweep_line.{hpp,cpp}` | The common record: `SweepLine` (start Hz, stop Hz, N dB values with NaN = none, time), validity check, point <-> frequency functions (`sweepPointHz`, `sweepNearestPoint`). |
| `src/source/tinysa_protocol.{hpp,cpp}` | `scanrawCommand`, incremental `ScanrawParser` (framing and exact-point-count checks), `decodeScanraw` (two models), `sweepOnce` over an abstract `SerialLink`. |
| `src/source/serial_port_win.{hpp,cpp}` | Thin Windows COM wrapper (`WinSerialLink`), UNVERIFIED ON HARDWARE, never opened by a test; `isValidComPortName` is the only way a name reaches CreateFile. |
| `src/core/sweep_stitch.{hpp,cpp}` | `stitchPass` (trim, grid, chained robust level alignment, nearest-centre fill) and `SweepHeatmap` (bounded store, max/mean decimation on both axes, click-to-tune functions). |
| `src/core/sweep_history.{hpp,cpp}` | CSV / JSON / XML save and load of sweep history, caps, line-numbered refusals; `importFreqLevelPairs`. |
| `src/core/sweep_occupancy.{hpp,cpp}` | `makeChannelPlan`, `OccupancyModel` (time-weighted percent per channel, per hour, CSV exports). |
| `tests/test_sweep_{tinysa,history,stitch,occupancy}.cpp` | The proof; found by the existing glob, so `tests/CMakeLists.txt` is untouched. |

Design facts a reader needs (each is also in the file headers): the stitcher's capture layout is "bin N/2 is the
centre"; levels are relative to the lowest-centre capture's gain; the occupancy percentile floor assumes fewer than
(1 - p) of the channels are busy at once (use the fixed floor otherwise); occupancy time is time-weighted with a
60 s gap cap; the history formats are versioned 1 and a reader refuses any other version.

## Tests (real result lines, build folder `D:/foxsdr-build/en-sweepsrc`, Release)

```
test_sweep_tinysa: 35216 checks, 0 failed        (fuzz: 3088 done, 17142 bad, 9770 truncated of 30000)
test_sweep_history: 20326 checks, 0 failed       (fuzz: 584 loads ok, 17416 refused of 18000)
test_sweep_occupancy: 19281 checks, 0 failed
test_sweep_stitch: 30346 checks, 0 failed
  worst gain-correction error: 0.195 dB (limit 0.5)
  tones: worst frequency error 0 Hz (limit 2344), worst level error 0.243 dB (limit 1)
  worst seam step in the noise floor: 0.131 dB (limit 1)
  lopsided droop: worst gain-correction error 0.16 dB trimmed, 1.81 dB untrimmed
  fuzz: 2017 of 3000 random capture sets stitched, the rest refused
```

Stated tolerances (what the stitch test asserts): tone frequency within one bin of the truth; tone level within
1 dB; every capture's gain correction within 0.5 dB (seven chained seams; 0.3 was the first target and the
32-frame version of the test missed it at 0.63 dB, which was noise in the test captures, so the test now averages
128 frames); seams within 1 dB. The tuner error (+-5 ppm, at most 0.5 kHz here) is below the bin width, so the
recovered frequencies land on the nominal cell: a common ppm shift is invisible to any stitcher.

A real fault the tests found: the first cut of the chain added the lower capture's correction twice (the seventh
capture came out 67 dB off); fixed before the stitcher was committed.

## Break-it table (mutation applied, test rebuilt, RED seen, file restored with `git checkout` and touched, rebuilt, GREEN seen)

| Feature | Mutation | RED line (first) | After restore |
|---|---|---|---|
| tinySA frame length check | closing brace accepted at any point count | `test_sweep_tinysa.cpp:160 p.feed(...) == FrameStatus::BadLength` (+2 more) | 35216 checks, 0 failed |
| History reader version check | JSON version check made never fire | `test_sweep_history.cpp:149 !r.ok && r.error == "unsupported version"` | 20326 checks, 0 failed |
| Stitcher level alignment | upper capture correction forced to 0 | `test_sweep_stitch.cpp:194 worstCorr < 0.5` (+ tone level, seam) | 30346 checks, 0 failed |
| Stitcher outlier rejection | every overlap difference kept | `test_sweep_stitch.cpp:194 worstCorr < 0.5` (+ tone level, seam) | 30346 checks, 0 failed |
| Heatmap max time-decimation | merge by min instead of max | `test_sweep_stitch.cpp:449 peak == -20.0f` | 30346 checks, 0 failed |
| Occupancy threshold margin | threshold = floor (margin dropped) | `test_sweep_occupancy.cpp:117 m.percent(...)=81.65 vs 10` (+ more) | 19281 checks, 0 failed |

## UNVERIFIED

- Everything about the tinySA on real hardware: no device on the desk. Command terminator (CR LF is a choice),
  echo/prompt before the opening brace, model detection (the page gives no `version` format), scanraw point
  limit, baud rate (the Windows wrapper sets 115200 as a placeholder a virtual COM port ignores), whether DTR is
  needed.
- `WinSerialLink` open/read/write were never run.
- The tinySA decode sanity bound (+30 dBm) is this lane's own bound, not a device figure; a wrong model is
  detectable only when a level decodes above it.
- COM-port enumeration (listing the machine's ports for a picker) is not built and not tested.
- Stitching has been proven on generated captures only, never on a real RTL-SDR or B200 sweep.
- The occupancy model has not been compared to a real band recording.

## Wiring and screen hand-off

None of this is reachable by a user yet: this lane may not edit the registry, the engine or any `.slint` file.

**Registering the tinySA as a source kind** (needs the device registry/ledger and `engine.cpp`):
1. A new source kind "sweep device" (it produces `SweepLine`s, not I/Q, so it does not go through `IqSource`).
   Kind id `tinysa`, English name "tinySA / tinySA Ultra (sweep)".
2. A worker thread that opens `WinSerialLink`, loops `tinysa::sweepOnce(link, req, line)` with the user's
   start/stop/points and stamps `line.timeS` from the host clock, and hands each line to the display and to
   `SweepHeatmap::addLine` / `OccupancyModel::addSweep`. On `TimedOut`/`LinkFailed` it closes and reports one
   health line (never the port name); `Refused`/`Malformed` count and skip the sweep.
3. Port enumeration is not built (UNVERIFIED how the Ultra's composite device presents its COM port).
4. Feeding the stitcher from an ordinary SDR (RTL-SDR, B200, HackRF through its existing driver) needs a retune
   loop in the engine: tune, wait for settle, take N FFT frames, average in power, build a `SpectrumCapture`
   (centre = the tuned frequency as REQUESTED, rate = the sample rate, bins fftshifted), repeat across the range,
   then `stitchPass` and `SweepHeatmap::addLine`.

**Controls a screen needs** (English strings; every control: label, what it sets, range, default):

| Label | Sets | Range | Default |
|---|---|---|---|
| "Sweep device" | source kind | none / tinySA / tinySA Ultra | none (the model cannot be read from the device: the user must choose) |
| "Port" | COM port | `COM1`..`COM256` | none |
| "Start (MHz)" / "Stop (MHz)" | sweep range | 0 .. 1 000 000, start < stop | none |
| "Points" | `SweepRequest.points` | 2 .. 1 048 576 (the device's own limit is not documented) | 450 |
| "Wide sweep" | enables the stitcher | on/off | off |
| "Edge trim (%)" | `StitchConfig.trimFraction` x 100 | 0 .. 44 | 12 |
| "Heatmap columns" / "Heatmap rows" | `HeatmapConfig.cols` / `maxRows` | 1..65536 / 2..65536, product at most 16 777 216 | 1024 / 512 |
| "Frequency decimation" / "Time decimation" | `freqMode` / `timeMode` | Max / Mean | Max / Max |
| Click on the heatmap | tune to `SweepHeatmap::frequencyAtFraction(x)` | across the picture | n/a |
| "Occupancy" | enables the model | on/off | off |
| "Channels: start (MHz)" / "stop (MHz)" / "step (kHz)" | `makeChannelPlan` | step at least 0.001 kHz, at most 16 384 channels | none |
| "Margin above noise floor (dB)" | `marginDb` | 0 .. 200 | 10 |
| "Noise floor percentile (%)" | `floorPercentile` x 100 | 0 .. 50 | 20 |
| "Fixed floor (dB)" | `fixedFloor` + `fixedFloorDb` | off, or -400 .. 400 | off |
| "UTC offset (minutes)" | `utcOffsetMinutes` | -840 .. 840 | host's local offset |
| "Export channels CSV" / "Export hours CSV" / "Export channel-hours CSV" | the three exports | n/a | n/a |
| "Save sweep history" | `historyToText` | CSV / JSON / XML | CSV |
| "Open sweep history" | `historyFromText` | auto-detected | n/a |
| "Import device export" | `importFreqLevelPairs` | unit MHz / kHz / Hz | MHz |

Plugin hand-off: the stitcher, heatmap, history files and occupancy model are plain C++ in `src/core`; a plugin
needs a host call that takes a `SweepLine` array in and returns the stitched line / report text (not written:
`plugin_abi.h` and `fox_engine_api.cpp` are closed to this lane).

Refusal strings are the `fail(...)` / `refuse(...)` literals in `sweep_history.cpp`, `sweep_stitch.cpp`,
`sweep_occupancy.cpp` and `tinysa_protocol.cpp`; none is shown to a user until a screen exists and none is
translated.

## Owner decisions

1. RF Explorer device support stays blocked unless you allow reading the LGPL wiki, or RF Explorer Technologies
   gives a written description under terms. The export import works now.
2. HackRF hardware sweep stays blocked for the same reason (description only in GPL source); the stitcher gives
   the wide picture through the existing driver, at the speed of a retune per step.
3. Whether the tinySA is worth wiring before any hardware test: the hand-off above is ready, but nothing in it
   has run against a device.

## Sources opened

Every URL is listed in the Step 0 section with its status. Web searches (titles and snippets only; nothing from a
GPL/LGPL repository was opened): RF Explorer serial protocol, tinySA USB command set, HackRF sweep USB request.

## Final gates

Own tests (ctest, in the headless set, after the full Release build of every target, build exit 0):

```
Test #475: test_sweep_history ...   Passed    1.53 sec
Test #476: test_sweep_occupancy ... Passed    1.28 sec
Test #478: test_sweep_tinysa ...    Passed    0.33 sec
Test #477: test_sweep_stitch ...    Passed    1.82 sec
```

Headless set, `ctest -L headless -j 3 --timeout 180` in six slices by index (707 tests, machine at heavy load):

```
1-130:    89% passed, 14 failed of 130
131-260:  95% passed, 7 failed of 130
261-390:  90% passed, 13 failed of 130
391-520:  93% passed, 9 failed of 130
521-650:  96% passed, 5 failed of 130
651-781:  100% passed, 0 failed of 57
```

48 distinct failures in the first pass. Re-run of those 48 (`-j 2`): 19 passed (load flakes). Of the 29 that failed again:

- Named false failures of the standard rules: test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode, test_window_failed_app (no Slint window in this configuration), test_diagnostics_free (tree had uncommitted changes while the record was written).
- Missing in this worktree or on this desk, not code: test_engine_api_source_kinds ("the fixture scope.wav is not in .../src/slint/tests/drive/fixtures"), test_soapy_enum_proc ("Could not find path for image: usrp_b200_fw.hex"), test_soapy_source, test_sdrplay_probe (hardware/driver probes), telemetry_worker (timeout).
- 17 judged-by-timing or suspicious ones re-run ONE AT A TIME (`-j 1`): passed alone: test_engine_main_smoke, test_engine_shell_guard, test_pa_stream_list_guard, test_plugin_api, test_plugin_hostile, test_plugin_repo, test_pump_live_traffic, test_sentinel, test_tx_audio, test_window_threaded_look. Still failing alone: test_engine_api_decode_radio (timeout 120 s), test_engine_api_merge (347 s), test_engine_api_transmit (171 s), test_engine_api_schedule (`test_engine_api_schedule.cpp:292 !ec && size > 44`: the timed recording came out 44 bytes), test_failure_files, test_pass_control (`:479 n >= 80u`, a listed load flake), test_crash_upload_free (`test_crash_upload.cpp:1751`, `:1782 srv.connections() == 1`).

NONE of these touches this lane's files: the lane adds six source files with no static state and four tests, and
edits nothing that exists. They are in code owned by other lanes (engine API, timed recordings, crash upload, plugin
host) and several are wall-clock tests on a machine running about forty builds. I did NOT build a baseline tree
(the previous tip, d3630d78) to prove they fail there too: a second full relink of 600 test programs did not fit.
So "not caused by this lane" is reasoned, not measured; the orchestrator's own baseline run is the proof.

config.json (`%APPDATA%/foxsdr/config.json`) sha256 after the lane, read only: c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3 (unchanged).
No radio, serial port, hidden desktop, certificate store, listening socket or outside connection was used by this
lane's tests. A web search and page reading were the only network use, by the tool, for research.
