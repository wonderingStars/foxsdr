# Extracting the engine from the FoxSDR app (plan, draft 0.2)

Status: **a plan.** No app code has moved. The work happens later, in the
FoxSDR app repository, one stage at a time, each stage shipped and measured
before the next. This repository supplies the contract (`foxsdr_api.h`),
the conformance tests the extracted engine must pass (the mock engine's
tests in `tests/`), and the first interface plugin.

## 1. Where things are today (app 0.99.35)

- `AppWindow` (`src/gui/app_window.cpp`, ~23 000 lines, and
  `app_window.hpp`, ~3 700) owns almost everything: the `Pipeline`, the open
  device, the recorder, the transmitter, the plugin host and runner, the
  patch runner, the scanner, bookmarks, the config, the web server and the
  CAT server, and it draws the whole interface.
- `Pipeline` (`src/core/pipeline.hpp`) is already engine-shaped: its own
  source and DSP threads, a latest-frame spectrum slot, lock-free atomic
  mirrors for every parameter the GUI polls per frame (added after the
  0.96.2 GUI-thread stall report), bounded joins for drivers that never
  return.
- **There is already one shared control path** —
  `AppWindow::applyControlRequest(const net::ControlRequest&)` — and three
  clients use it: the web remote (`/api/control`, validated by
  `net::parseControlRequest`, queued, applied on the GUI thread's next
  frame), CAT (`net::executeCatLine`, which uses four of its fields), and
  plugins (`applyPluginApi`, host API level 1's queued controls).
- **No desktop control uses it.** Every widget, key and gesture changes
  state directly (the UI inventory behind docs/API.md section 10). The two
  paths have already drifted: `running=false` from the web does not stop
  recordings while the STOP dome does, and the mode, bookmark-tune and
  record branches of `applyControlRequest` are inline copies rather than
  calls to the GUI's own helpers.
- Plugins already read a **snapshot** (`PluginApiCore`, published each frame
  through `SeqlockBox`) and write **queued requests** — the same shape as
  this API, one level down.
- Constraints the extraction must respect: the source centre-frequency
  setter is documented GUI-THREAD-ONLY; device and audio opens block (57 s
  observed on one machine) and already run on workers; `Transmitter::tick()`
  is called by the frame loop and is the transmitter's dead-man handle.

## 2. The mapping: what exists → the API

| App today | API |
|---|---|
| `ControlRequest.running` | `RUN` (and it must stop recordings, as the dome does) |
| `centerHz` | `SET_CENTRE` |
| `vfoOffsetHz` | `SET_VFO_OFFSET` |
| `mode` | `SET_MODE` (bandwidth to the mode default, as now) |
| `bandwidthHz`, `squelchDb`, `volume` | `SET_BANDWIDTH`, `SET_SQUELCH`, `SET_VOLUME` |
| `dbMin`, `dbMax` | `SET_DISPLAY_RANGE` |
| `deemphasisIndex`, `stereoEnabled` | `SET_DEEMPHASIS`, `SET_STEREO` |
| `nrEnabled`, `nrStrength` | `SET_NR` |
| `notchEnabled`, `notchFreqHz`, `notchQ`, `autoNotch` | `SET_NOTCH`, `SET_AUTO_NOTCH` |
| `sourceKind` + `soapyArgs` | `SELECT_SOURCE` (id from `LIST_DEVICES`) |
| `antenna`, `sampleRateHz`, `gainName`+`gainDb`, `agc` | `SET_ANTENNA`, `SET_SAMPLE_RATE`, `SET_GAIN`, `SET_DEVICE_AGC` |
| `scanDevices` | `SCAN_DEVICES` |
| `recordIq`, `recordAudio` | `RECORD_IQ`, `RECORD_AUDIO` |
| `bookmarkAdd`, `bookmarkTune`, `bookmarkRemove` | `BOOKMARK_ADD`, `BOOKMARK_TUNE`, `BOOKMARK_REMOVE` (by id, not list index) |
| `scannerActive`, `scannerSkip`, `scanStart/Stop/StepHz` | `SCANNER_RUN`, `SCANNER_SKIP` |
| `pluginFetch`, `pluginInstall`+`acknowledgeNotice`, `pluginRemove` | `STORE_FETCH`, `STORE_INSTALL`, `STORE_REMOVE` |
| `pluginTuneName`+`pluginTuneAllowed` | `PLUGIN_GRANT` |
| `pluginPresetName`+`pluginPresetIndex` | `PLUGIN_PRESET` |
| `transmitPtt` (bounded remote hold) | `TX_PTT` (the same hold, for every session) |
| `/api/status` fields | `FoxReceiverState` |
| `/api/spectrum` (8-bit bins, `since` cursor) | `read_spectrum` (float bins, `since` seq) |
| `/api/audio` (48 kHz L16 stream) | `read_audio` |
| `/api/login`, `/api/session`, `/api/logout` | transport login (docs/TRANSPORTS.md) |
| CAT `F`/`M`/`f`/`m`… via `executeCatLine` | an engine-internal session per connection, TUNE + SETTINGS (docs/TRANSPORTS.md 2.6) |
| Host API L1 `get_state` | `read_state` |
| `get_gain`, `get_sample_rates` | `LIST_GAINS`, `LIST_SAMPLE_RATES` |
| `set_frequency`, `set_vfo_offset` (TUNE grant) | `SET_FREQUENCY`, `SET_VFO_OFFSET` (TUNE grant) |
| `set_mode` … `set_muted` (SETTINGS grant) | the same ops (SETTINGS grant) |
| `set_marker` / `remove_marker` / `clear_markers` | stay in the plugin ABI; read by interfaces as `LIST_MARKERS` |
| `log` | `NOTICE` events |
| `add_command` / `poll_command` | stay in the plugin ABI; pressed by interfaces as `PLUGIN_COMMAND` |
| `settings_get` / `settings_set` | stay in the plugin ABI (a plugin's own store) |
| `get_stream_info` | GAP in this API (not needed by interfaces yet) |

Decoder plugins keep `plugin_abi.h` unchanged: the engine hosts them, and
the plugin host API's queued controls and this API's commands drain into
the same control path.

## 3. The performance gate — measure BEFORE anything moves

The promise is that the in-process path is as fast as today. That is only a
promise if today is measured first, on the same machines, by a script, and
every stage is re-measured against it. Record for the release the
extraction starts from (0.99.35), on the development desktop (B200 and
RTL-SDR) and on the Raspberry Pi:

| Measure | How (all exist in the app today or are a few lines) | Gate |
|---|---|---|
| **Max sustained sample rate** | For each offered rate, run 60 s streaming with the standard page open; read `Pipeline::ringDroppedSamples()` before and after. The highest rate with zero drops. | no lower after a stage |
| **Frame time** | Mean and p99 GUI frame time over 60 s at 2.048 MS/s WFM with the spectrum, waterfall, DEMOD SCOPE and three decoders open (`--frames` bounded runs already exist); how each is produced is below. | p99 not worse by more than 1 ms; mean within 5 % |
| **Tune-to-audio latency** | Signal generator with a tone at +100 kHz; submit a retune; time from the command to the first block in `audioTap()` whose tone power crosses half its steady value (`audioSamplesProduced()` gives the sample clock). Repeat 50×. | median not worse by more than one frame (16.7 ms) |
| **CPU at a fixed rate** | Process CPU over 60 s at 2.048 MS/s WFM, interface idle and interface busy (dragging a slider continuously). | within 5 % |
| **GUI-thread stalls** | `HangWatchdog` reports and the longest frame over a 10-minute soak. | none new |
| **Memory** | Working set after 10 minutes. | within 5 % |

These are to be scripted in the app repository (`tools/measure_engine.*`)
so the same numbers are produced before and after, not re-derived by hand.

### 3.1 How a number is produced, and when a difference counts

A single run proves nothing about a millisecond: frame time on a desktop
moves with the compositor, the GPU clock and whatever else is running. So:

- **Runs.** Every measure is taken in **5 runs** per build (the 10-minute
  soaks: 2). Tune-to-audio latency is 50 retunes per run, so 250 per build.
- **Order.** Baseline and candidate runs are **interleaved** on the same
  machine in one session - A B A B A B A B A B - so a drift over the
  session (thermal throttling, a background update) lands on both builds
  instead of on one. The machine is on mains power, on its normal power
  plan, with the same window size, the same device and the same radio
  input, and nothing else started; the script records the OS build, GPU
  driver, CPU governor and whether a battery was charging, and refuses to
  compare runs whose records differ.
- **Warm-up.** The first 5 s of every run are discarded (shader compiles,
  first-touch allocations, the DSP ring filling).
- **Frame time, exactly.** The app records, for every frame, the steady
  clock at the START of the frame (before event polling) into a
  preallocated array (a debug switch, e.g. `FOXSDR_FRAME_LOG=path`, writes
  it out at exit; nothing is written during the run). Frame time is the
  interval between consecutive frame starts, so it includes the swap and
  any stall, which is what a user sees. Runs are made with vsync OFF so the
  interval is the work, not the display's 16.7 ms. **p99** is the
  nearest-rank 99th percentile of that run's intervals after the warm-up
  (at ~60 s and several hundred frames a second, thousands of samples, so
  the 99th is a real tail, not two frames); **mean** is their mean. The
  work time (frame start to swap return) is logged beside it for diagnosis.
- **Statistic.** Each run gives one value (one p99, one mean, one median
  latency, one CPU figure). A build's value is the **median of its 5 runs**.
- **Variance rule.** If the baseline's 5 runs spread by more than the gate
  itself (for p99: max - min > 1 ms; for the 5 % gates: max/min > 1.05),
  the environment is too noisy to judge: fix it (close things, pin the
  clock) and measure again. A gate is not judged on noisy runs.
- **When a difference counts.** A stage FAILS a gate when the candidate's
  median is worse than the baseline's by more than the gate's allowance
  AND the two sets of 5 runs do not overlap at all (every candidate run
  worse than every baseline run: with 5 against 5 that happens by chance
  1 time in 252). A difference inside the allowance passes; a
  difference beyond it with overlapping runs is re-measured with 10 runs a
  side before anything is decided.
- **Reporting.** Each gate reports both medians, both ranges and the
  verdict, in the stage's commit message and release notes.

## 4. The stages

Each stage ends shippable, measured against section 3, and with the app's
full test suite green.

**Stage 1 — one control path for everything.** Introduce
`AppWindow::applyCommand(const FoxCommand&)` in the app, behind which
`applyControlRequest` becomes a translation (ControlRequest field → op).
Convert every desktop widget and key binding to submit commands to a local
queue drained at the top of the frame (still the GUI thread, so no
threading changes yet). Fix the two known divergences on the way (RUN stops
recordings; the mode/bookmark/record branches call the one helper). Tests:
one per op through `applyCommand`, and a table test that every
`ControlRequest` field maps to an op.

**Stage 2 — one snapshot for everything.** Publish a `FoxReceiverState`
each frame (reuse `SeqlockBox`), and make `/api/status`, CAT's reads and
the plugin host API's `get_state` read from it. Merge `PluginApiCore`'s
snapshot into this one.

**Stage 3 — move ownership into an `Engine`.** Create `src/engine/Engine`
in the app holding the `Pipeline`, the device, recorder, transmitter,
plugin host and runner, patch runner, scanner, bookmarks, config, crash
handler and telemetry. `AppWindow` holds a reference. The engine gets its
control thread; command application moves there from the GUI thread. The
GUI-THREAD-ONLY source setter becomes CONTROL-THREAD-ONLY (the same
single-thread rule, a different thread); blocking opens stay on workers.
`Transmitter::tick()` moves to the control thread, and the per-frame
dead-man becomes the per-session keep-alive (docs/API.md 6.10) — with the
mock engine's `test_transmit_safety` ported to run against the real engine.

**The TX thread keeps its own dead-man, pointed at the control thread.**
Today `Transmitter::threadBody` (transmitter.cpp, "THE DEAD-MAN'S HANDLE")
compares `lastTickMs_`, stamped by the GUI thread's `tick()` every frame,
with `kKeyAliveWait` (1 s) on every block, and unkeys itself when the GUI
thread has stopped - because the thread that would normally release the
key has stopped too. After stage 3 the session keep-alives are judged BY
the control thread, so a wedged control thread would judge nothing: the
TX thread's staleness check therefore stays, re-pointed at the control
thread's tick (stamped once per control pass), with the same 1 s bound and
the same unconditional break out of the TX loop. Two independent watchdogs
on two threads, as today: the control thread enforces every session's
keep-alive, the latch, the hold and the local consent to remote transmit;
the TX thread enforces that the control thread is alive. A test wedges the
control thread (a test-only hook that parks it) with a key down and
requires the TX thread to unkey within 1 s + one block.

**Stage 4 — the engine behind the C table.** The `Engine` exports
`FoxEngineApi`. `AppWindow` stops touching `Engine` members and reads and
writes only through the table: `read_state` once per frame, `read_spectrum`
once, one `submit` per frame. Enforced by building the interface as its own
CMake target that cannot see engine headers. The standard interface in this
repository and the app's own window then converge into one interface
plugin.

**Stage 5 — web and CAT become transports.** `/api/control` becomes a
`submit` on a REMOTE session; `/api/status` a `read_state`; each CAT
connection an engine-internal session (REMOTE flag, TUNE + SETTINGS, never
TRANSMIT), authorised by the local operator enabling CAT rather than by a
token (docs/TRANSPORTS.md 2.6). The strict parsing stays (unknown keys are
errors).
Their private copies of control logic are deleted.

**Stage 6 — the remote transport.** The WebSocket session server and the
client library (docs/TRANSPORTS.md), and the standard interface running on
a laptop against an engine on the Pi, passing the same render tests.

**Stage 7 — split the binaries.** `foxsdr-engine` (a headless service, for
the Pi), `foxsdr` (host = engine + standard interface in-process, what
users run today), interface plugins in the catalogue.

## 5. Risks

- **Threading moves** (stage 3) are where regressions hide: anything that
  assumed "the GUI thread applies it" now runs on the control thread. Every
  GUI-THREAD-ONLY comment in the app is a checklist item.
- **Plugin UI is drawn by the host today** (panels, instruments, pictures,
  preset bars, command keys). An interface plugin must draw them from data,
  so the API gaps in docs/API.md 10.7 (panels, instruments, command lists)
  must close before stage 4 can remove the host drawing.
- **The patch page** couples its canvas to its runner tightly; it is the
  biggest gap and probably its own stage.
- **The dead-man semantics change** from "the frame loop stopped" to "the
  session stopped answering". The keep-alive must be at least as tight
  (1 s) as `kKeyAliveWait`, and the remote hold stays 2 s.
- **Performance** is not expected to be the risk (the heavy work does not
  move; the measured API cost is microseconds per frame), which is exactly
  why it must be measured rather than assumed.
