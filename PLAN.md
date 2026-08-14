# cascade — project plan

A from-scratch, MIT-licensed SDR receiver application whose GUI and feature set
match SDR++, intended for free public release under its own name.

This file is the specification every implementation agent works from. Agents do
not see the conversation that produced it; everything needed is here.

## Legal ground rules (non-negotiable)

- **Clean-room discipline: never read SDRPlusPlus source code** — not the repo,
  not forks, not code excerpts in its issues/wiki. The GUI/feature spec below is
  derived from observed behavior and public documentation, which is the only
  input allowed. If a question seems to need "how does SDR++ do it", answer it
  from first principles or DSP literature instead.
- **Permissive dependencies only.** Approved: Dear ImGui (MIT), GLFW (Zlib),
  pffft (BSD-like), SoapySDR (BSL-1.0), PortAudio (MIT-like), nlohmann-json
  (MIT), GLEW (BSD). Banned from linking: FFTW (GPL — present in vcpkg, do not
  use), librtlsdr (GPL — hardware access goes through SoapySDR's runtime-loaded
  modules instead), GNU Radio VOLK (LGPL). Note: the `volk` package in this
  vcpkg tree is the *Vulkan loader*, not GNU VOLK — unrelated project, same name.
- Direct UHD linkage is deferred until its license terms are verified; the
  default B200 path is SoapySDR's UHD module, loaded at runtime by SoapySDR.

## Architecture

```
source thread          DSP thread                    GUI thread (60 fps)
┌────────────┐  SPSC   ┌─────────────────────┐        ┌──────────────────┐
│ SDR device │ ─ring─▶ │ VFO: NCO mix → FIR  │        │ spectrum panel   │
│ (or siggen │         │ decim → demod → AGC │ ─ring─▶│ waterfall (GL    │
│  or file)  │ ───────▶│ spectrum estimator  │ frames │ texture ring)    │
└────────────┘         └─────────┬───────────┘        └──────────────────┘
                                 │ SPSC ring
                                 ▼
                       audio thread (PortAudio callback)
```

- `src/core/` — version, config store (JSON, later), module registry (later)
- `src/dsp/` — windows, FFT wrapper, spectrum estimator, SPSC ring, FIR
  design + decimation, NCO/mixer, resampler, AGC, demodulators
- `src/gui/` — GLFW/ImGui shell, spectrum + waterfall widgets, menu panels
- `src/source/` — SigGen, IQ file, SoapySDR
- `src/sink/` — PortAudio out, network (later)

Threading contract: every DSP block is a plain object processing arrays;
threads and rings live only at the pipeline layer. Blocks are therefore unit
testable without threads.

## GUI parity spec (from observed SDR++ behavior)

- Dark theme throughout (ImGui dark style as base).
- Top toolbar: play/stop button, volume slider, large frequency readout in Hz
  with per-digit mouse-wheel tuning (grouped digits, dimmed leading zeros).
- Left column (~260 px, collapsible sections): Source, Radio (mode buttons
  NFM/WFM/AM/DSB/USB/CW/LSB/RAW, bandwidth, squelch, snap interval), Sinks
  (audio device + volume), Display (FFT size, waterfall min/max, colormap).
- Center: RF spectrum (line plot, dB axis) above a scrolling waterfall, both
  sharing the frequency axis; draggable divider; VFO shown as a translucent
  band draggable with the mouse; scroll wheel zooms about the cursor.
- Status bar: SNR meter, buffer health.

## Phases

- **P0 — Toolchain + shell** ✋gate: green MSVC build, ctest green, app opens an
  ImGui window with the panel skeleton and `--frames N` self-test flag.
- **P1 — DSP primitives** (parallel, one agent per module, tests mandatory):
  SPSC ring; FFT wrapper + spectrum estimator; FIR design + decimator;
  NCO/mixer; quadrature discriminator + AGC. ✋gate: all module tests green,
  each independently verified.
- **P2 — Render pipeline**: spectrum plot + waterfall GL texture w/ colormaps
  fed by SigGen through the real DSP thread; 60 fps sustained.
- **P3 — Radio chain**: VFO wiring, WFM/NFM/AM/SSB/CW demods (each with
  synthesized-signal tests), PortAudio sink, squelch, S-meter.
- **P4 — Hardware sources**: IQ file (WAV), SoapySDR enumeration + streaming;
  live test on the bench B200 via SoapyUHD.
- **P5 — UX parity + persistence**: frequency entry, zoom, band snap, JSON
  config save/restore, themes, keybinds.
- **P6 — Extras**: recorder, frequency manager, scanner, installer (Inno).

## Testing protocol (applies to every agent)

1. Tests live in `tests/test_<module>.cpp`, one executable per file, discovered
   by glob — never edit `tests/CMakeLists.txt` or the root `CMakeLists.txt`.
   Use the `CHECK`/`CHECK_NEAR` macros from `tests/test_check.hpp`.
2. DSP correctness is proven against an in-test reference (direct DFT, naive
   convolution) — never against constants copied from the implementation.
3. **Red-green is mandatory**: after tests pass, temporarily break the named
   behavior, confirm the test fails for that reason, restore exactly, confirm
   green. A test that never went red proves nothing.
4. No randomness without a fixed seed (small LCG in-test; no <random> device).
5. Run the FULL ctest suite before reporting done, not just your own test.
6. Report evidence: the decisive output lines, not paraphrase.

## Toolchain (Windows, this machine)

```
cmake -S <root> -B <root>/build-<slug> -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build <root>/build-<slug> --config Release
ctest --test-dir <root>/build-<slug> -C Release --output-on-failure
```

Each agent uses its own `build-<slug>` directory (they are gitignored). Agents
never run git commands; the orchestrator commits reviewed work centrally.
