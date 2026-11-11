# Multi-radio screen, stage S0: can the window draw four radio panes? (2026-11-11)

Branch `mr/screen-s0`, cut from `engine/multi-radio` at f2213607. Design: `docs/multi-radio-screen-design.md` (read-only copy in the engine-stage4 worktree), section 4 row S0, sections 3.3, 3.4 and 5. Nothing of the product changed: one new measuring tool (`src/slint/tests/measure_radio_panes.cpp`, a target in `src/slint/tests/CMakeLists.txt` with no `add_test`) and one new sheet (`src/slint/ui/specimens/radio_pane_s0.slint`).

## The answer in short

Four RadioPanes in the 2 x 2 grid at 1440 x 900 and 1600 x 900, each fed two new pictures every frame, fit the frame budget on the one machine measured, in every renderer where the cost could be read:

- Software renderer, one synchronous frame (the host's picture work for four panes plus one full render of the window): about 4.7 ms at 1440 x 900 and 4.9 ms at 1600 x 900 with today's 1024-wide pictures (p95 5.1 and 5.3 ms, worst of three runs 13 and 14 ms), and 3.9 and 4.2 ms with pictures fitted to the pane. One pane costs 1.8 to 2.0 ms.
- OpenGL renderers (femtovg, skia on OpenGL), render-driven: a new frame every 8.3 ms (the display runs at 119 Hz) with four panes as with one, p95 9.3 ms. The CPU side of one four-pane frame is the host's 1.0 ms plus the renderer's 4.0 to 4.4 ms.
- The shipped default (it behaves like `winit-skia`, see "Which renderer") does not read as a cost: the frame clock arrives about every 15.4 ms (median 10 to 14, p95 24 ms) with ONE pane as with four, with today's and with fitted pictures. Whatever sets that cadence is not the panes.

The host's picture work for four panes (the frame copied, `paintTrace`, `scrollWaterfallRow`, the waterfall copy, the ticks, two new `slint::Image` values, four model rows set) is 1.0 ms with today's 1024 x 340 and 1024 x 238 pictures and 0.35 to 0.45 ms with pictures fitted to the pane (563 x 130 and 563 x 140). Setting the four model rows costs 0.04 to 0.12 ms: a struct in a model carries the two images by reference, no pixels are copied there.

## Decision for the pane picture size (design section 4: "fix the pane picture size")

Fit the pictures to the pane: as wide as the pane (563 px at 1440 x 900, 643 px at 1600 x 900) and as high as their wells (about 130 and 140 px in a 353 px pane). The host's work for four panes falls from 1.0 ms to 0.4 ms and the synchronous software frame from 4.7 and 4.9 ms to 3.9 and 4.2 ms; nothing visible is lost, since Slint scales the 1024 px picture down to the plot anyway. The measurement does not force it: today's size fits the budget too. The size is left to S3, which binds panes to `RadioView`s (`RadioSet::setPictureSize`).

## What the harness is

`measure_radio_panes --panes 1|4 --width 1440|1600 --height 900 --pictures default|fitted --frames 600 --warmup 120 --drive frame|render|timer|headless`

- A window (the platform's own backend, chosen with `SLINT_BACKEND`, or with `--drive headless` the render harness's software platform) holding one or four `RadioPane`s (`radio_pane.slint`, the real component, `ribbon-size: 2`, short captions) in the hand-over's grid: workspace right of the 56 px rail and the 252 px panel, under the 34 px title bar, 92 px strip and 34 px toolbar, above the 28 px status bar; panes 563 x 353 at 1440 x 900 and 643 x 353 at 1600 x 900, 6 px gaps. One pane sits where pane 1 sits (the same size as in the four-pane layout).
- Each frame, per pane: the next of 240 made-up spectra (4096 bins: a noise floor near -90 dBFS, three drifting carriers, a burst sweeping across; different for each pane; made before the timed frames), then what `paneTick` does with a new frame in `main.cpp`: the frame assigned into the `RadioView`, `paintTrace` into the trace buffer, `scrollWaterfallRow` into the canonical picture, that picture copied into the alternate waterfall buffer, the peak scan, `frequencyTicks` and `dbTicks`; then `traceImage()` and `waterfallImage()` (two new `slint::Image` over the alternating buffers), a new peak text, `flip()`, and the pane's row of the `[RadioPaneData]` model set. Both textures change every frame and are uploaded again, as live. `paneTick` and `publishPicture` are lambdas inside `main()` and cannot be called from a harness, so the harness repeats the picture-producing half of `paneTick` with the same functions (`paintTrace`, `scrollWaterfallRow`, `RadioView`, `WellBuffers`); it does not repeat the lists, the text-only figures or the engine reads.
- Frame time, four ways (`--drive`).
  - `frame`: the sheet has `animation-tick()` and a `changed` handler that calls the host once per drawn frame; this works for every renderer, including the software one, which has no rendering notifier. The time from one call to the next is the frame time. (In the OpenGL runs the notifier counted 576 to 584 draws for 601 calls, so it is about one call per draw.)
  - `render`: the next update is scheduled from the rendering notifier's AfterRendering, so every update is drawn once before the next (OpenGL renderers only); the "render call" column is BeforeRendering to AfterRendering.
  - `headless`: the render harness's software platform; one frame is the host's work followed by one `render()` of the whole window into a buffer, timed together (the call includes allocating and clearing the 3.9 MB frame, about a millisecond; a full repaint each frame, no damage tracking).
  - `timer`: zero-delay single shots. Found NOT to be one update per drawn frame (femtovg: 301 updates, 180 draws), so no number below comes from it; the first matrix of this stage, timer-driven, was discarded.
- Warm-up 120 frames, then 600 intervals; mean, median, p95, p99 and worst printed; process CPU over the measured part.
- Run only on the hidden desktop (`run_on_hidden_desktop.ps1`), WITHOUT `FOXSDR_FRAME_CAP_HZ`. Before every run `typeperf` took three samples of total CPU use.

### What it does not include

The focus strip, the panel, the banks, the title bar and the status bar are not drawn (the window is the workspace area only; for the GPU renderers, which redraw the whole window, the missing chrome is a small underestimate). The real `paneTick`'s bookmark, mark and marker lists are empty, the ribbon has one segment. No engine threads, receivers or audio ran beside the harness. Frames were made as fast as the window drew them (119 a second here), not at a radio's 30 or so. 4096 bins only. One machine, no integrated GPU, no 60 Hz display, no display-off case, scale factor 1.00 only.

## Which renderer

`foxsdr-slint` selects no backend (no `SLINT_BACKEND`, no renderer call in `src/slint`), so the shipped window uses the SDK's default (Slint 1.18.1, `C:/Users/steve/slint-cpp-1.18.1`; its `slint_cpp.dll` contains skia, femtovg and the software renderer). On this machine the default behaves exactly like `SLINT_BACKEND=winit-skia`: no rendering-notifier callbacks (the graphics API is reported as inaccessible), the same cadence. The API does not name the backend, so "the default is skia" is an inference from that equality, not a read-out. `winit-skia-opengl`, `winit-femtovg` and `winit-software` were also run.

## Results

Three runs of each cell; the table gives the mean of the three runs' statistics, worst = the largest of the three. Milliseconds. "host work" = frame copy, `paintTrace`, `scrollWaterfallRow`, waterfall copy, ticks, images, model rows. "render call" = the renderer's BeforeRendering to AfterRendering (OpenGL) or the `render()` call (software, headless), without presenting.

| renderer and timing | pictures | window | panes | mean | median | p95 | worst of 3 runs | host work (mean) | render call (mean) |
|---|---|---|---|---|---|---|---|---|---|
| software renderer, synchronous (host work + one full render) | default | 1440x900 | 1 | 1.8 | 1.8 | 2.2 | 5 | 0.22 | 1.62 |
| software renderer, synchronous (host work + one full render) | default | 1440x900 | 4 | 4.7 | 4.6 | 5.1 | 13 | 1.01 | 3.64 |
| software renderer, synchronous (host work + one full render) | default | 1600x900 | 1 | 2.0 | 1.9 | 2.1 | 6 | 0.22 | 1.74 |
| software renderer, synchronous (host work + one full render) | default | 1600x900 | 4 | 4.9 | 4.9 | 5.3 | 14 | 1.03 | 3.91 |
| software renderer in a real window, per-frame clock | default | 1440x900 | 1 | 8.2 | 8.5 | 9.1 | 23 | 0.32 | n/a |
| software renderer in a real window, per-frame clock | default | 1440x900 | 4 | 8.2 | 8.3 | 9.2 | 23 | 1.29 | n/a |
| software renderer in a real window, per-frame clock | default | 1600x900 | 1 | 8.2 | 8.5 | 9.2 | 23 | 0.30 | n/a |
| software renderer in a real window, per-frame clock | default | 1600x900 | 4 | 8.2 | 8.3 | 9.1 | 21 | 1.28 | n/a |
| femtovg (OpenGL) real window, render-driven | default | 1440x900 | 1 | 8.3 | 8.2 | 9.2 | 13 | 0.26 | 1.46 |
| femtovg (OpenGL) real window, render-driven | default | 1440x900 | 4 | 8.3 | 8.3 | 9.3 | 16 | 1.08 | 4.04 |
| femtovg (OpenGL) real window, render-driven | default | 1600x900 | 1 | 8.3 | 8.2 | 9.2 | 11 | 0.26 | 1.43 |
| femtovg (OpenGL) real window, render-driven | default | 1600x900 | 4 | 8.3 | 8.3 | 9.3 | 16 | 1.09 | 4.05 |
| skia on OpenGL, real window, render-driven | default | 1440x900 | 1 | 8.3 | 8.2 | 9.2 | 12 | 0.27 | 1.27 |
| skia on OpenGL, real window, render-driven | default | 1440x900 | 4 | 8.3 | 8.3 | 9.3 | 15 | 1.12 | 4.39 |
| skia on OpenGL, real window, render-driven | default | 1600x900 | 1 | 8.3 | 8.2 | 9.2 | 10 | 0.26 | 1.27 |
| skia on OpenGL, real window, render-driven | default | 1600x900 | 4 | 8.4 | 8.3 | 9.4 | 91 | 1.13 | 4.49 |
| shipped default (skia), real window, per-frame clock | default | 1440x900 | 1 | 15.2 | 12.3 | 23.9 | 32 | 0.43 | n/a |
| shipped default (skia), real window, per-frame clock | default | 1440x900 | 4 | 15.5 | 13.6 | 23.6 | 42 | 1.90 | n/a |
| shipped default (skia), real window, per-frame clock | default | 1600x900 | 1 | 15.3 | 12.8 | 24.1 | 39 | 0.41 | n/a |
| shipped default (skia), real window, per-frame clock | default | 1600x900 | 4 | 15.4 | 12.8 | 23.6 | 37 | 1.89 | n/a |
| software renderer, synchronous (host work + one full render) | fitted | 1440x900 | 1 | 1.7 | 1.6 | 1.9 | 4 | 0.09 | 1.56 |
| software renderer, synchronous (host work + one full render) | fitted | 1440x900 | 4 | 3.9 | 3.8 | 4.1 | 8 | 0.39 | 3.46 |
| software renderer, synchronous (host work + one full render) | fitted | 1600x900 | 1 | 1.8 | 1.8 | 2.0 | 5 | 0.10 | 1.72 |
| software renderer, synchronous (host work + one full render) | fitted | 1600x900 | 4 | 4.2 | 4.1 | 4.5 | 8 | 0.43 | 3.75 |
| software renderer in a real window, per-frame clock | fitted | 1440x900 | 1 | 8.3 | 8.3 | 10.1 | 26 | 0.14 | n/a |
| software renderer in a real window, per-frame clock | fitted | 1440x900 | 4 | 8.3 | 8.4 | 10.4 | 25 | 0.51 | n/a |
| software renderer in a real window, per-frame clock | fitted | 1600x900 | 1 | 8.4 | 8.4 | 11.2 | 40 | 0.16 | n/a |
| software renderer in a real window, per-frame clock | fitted | 1600x900 | 4 | 8.3 | 8.3 | 10.7 | 23 | 0.54 | n/a |
| femtovg (OpenGL) real window, render-driven | fitted | 1440x900 | 1 | 8.3 | 8.3 | 9.1 | 10 | 0.10 | 0.68 |
| femtovg (OpenGL) real window, render-driven | fitted | 1440x900 | 4 | 8.3 | 8.3 | 9.3 | 14 | 0.41 | 2.03 |
| femtovg (OpenGL) real window, render-driven | fitted | 1600x900 | 1 | 8.3 | 8.3 | 9.1 | 12 | 0.11 | 0.76 |
| femtovg (OpenGL) real window, render-driven | fitted | 1600x900 | 4 | 8.3 | 8.3 | 9.2 | 17 | 0.45 | 2.01 |
| skia on OpenGL, real window, render-driven | fitted | 1440x900 | 1 | 8.3 | 8.3 | 9.0 | 10 | 0.11 | 0.74 |
| skia on OpenGL, real window, render-driven | fitted | 1440x900 | 4 | 8.3 | 8.4 | 9.2 | 12 | 0.41 | 2.32 |
| skia on OpenGL, real window, render-driven | fitted | 1600x900 | 1 | 8.4 | 8.3 | 9.0 | 99 | 0.12 | 0.83 |
| skia on OpenGL, real window, render-driven | fitted | 1600x900 | 4 | 8.3 | 8.3 | 9.2 | 15 | 0.46 | 2.40 |
| shipped default (skia), real window, per-frame clock | fitted | 1440x900 | 1 | 15.2 | 12.3 | 24.0 | 40 | 0.17 | n/a |
| shipped default (skia), real window, per-frame clock | fitted | 1440x900 | 4 | 15.2 | 10.4 | 23.9 | 26 | 0.68 | n/a |
| shipped default (skia), real window, per-frame clock | fitted | 1600x900 | 1 | 15.3 | 13.3 | 24.0 | 132 | 0.20 | n/a |
| shipped default (skia), real window, per-frame clock | fitted | 1600x900 | 4 | 15.2 | 10.4 | 24.0 | 27 | 0.76 | n/a |

Reading the table: the real-window rows other than the default sit at one frame per display refresh (119 Hz, 8.3 ms) with p95 about 9.2 ms for one pane and for four, so the work fits inside a refresh. The rows that show cost are the "synchronous" row and the "render call" column (software: 4.9 ms for four panes at 1600 x 900). The skia default row has a different shape: mean 15.4, median 10 to 14, p95 24 ms for one pane and for four, today's and fitted pictures. That is not pane dependent and is not explained here (it may be the present path of skia's Direct3D or Vulkan backend on a non-interactive desktop; unproven). The process CPU in that row: 3 to 12 percent of one core for one pane, 9 to 33 percent for four (today's pictures 21 to 33, fitted 9 to 14).

Buffer detaches (a `begin()` on a buffer the renderer still held, which copies it; today's buffers are 1.4 MB and 1.0 MB): none in the render-driven OpenGL runs. In the per-frame-clock runs (femtovg, 600 frames, today's pictures) 46 to 64 for one pane and 160 to 224 for four panes, about 4 percent of the begins: the next update can start while the picture two frames back is still referenced. The alternating pair is nearly enough, not exactly.

## Machine and load

Intel Core i9-14900K (32 logical processors), NVIDIA GeForce RTX 4090 (driver 32.0.16.1088), display 119 Hz, Windows 11 (10.0.22631), scale factor 1.00. The other lanes' builds had finished when the 144-run matrix ran: the total CPU use sampled before each run had a median of 3.7 percent, seven runs were sampled above 15 percent (17, 20, 49 and four at about 50, all in cells of the 1600 x 900 fitted pictures). The worst frames of 91 to 132 ms (two skia-opengl runs, one skia default run) and 634 ms (one femtovg run, whose pre-run sample was 2.6 percent) are therefore not shown to be load; they are unexplained. The first, discarded, timer-driven matrix ran while another lane was building (load 8 to 100 percent).

How far to trust it: the means, medians and p95 agree within about 0.3 ms across the three runs of each cell, so the pane-count effect (software: 1.8 to 4.7 ms) is far outside the noise. The WORST-frame column is not trustworthy: one 5 to 130 ms frame in a 600-frame run is a scheduling event on a shared machine, and the load was sampled only before each run, not during it. One machine, one GPU, one display: nothing here says what an integrated GPU or the software renderer on a slower CPU would cost (the 4.9 ms synchronous software figure is on a 14900K and would be several times larger on a laptop CPU). The shipped default's cadence is unexplained, so for that renderer the result is only that the panes did not change it.

## Not covered

- The real window with four live radios: `paneTick` for more than the selected radio does not exist before S3 and the engine's per-radio spectrum reads.
- The shipped renderer's cost with the host's real 33 ms tick and a real radio's frame rate; the harness draws as fast as the window allows.
- The window around the panes (strip, panel, banks, title bar, status bar).
- A machine without a discrete GPU, a 60 Hz display, a display that is off.
- The cause of the skia default's 24 ms intervals.

## Files

`src/slint/tests/measure_radio_panes.cpp`, `src/slint/ui/specimens/radio_pane_s0.slint`, and an `add_executable` block at the end of `src/slint/tests/CMakeLists.txt` (target `measure_radio_panes`, no ctest test). Raw output of the 144 runs is `D:/foxsdr-build/s0-matrix.log` (not committed; the load sampled before a run is on the line above its RESULT line).

## Full table

Every cell; the three runs are separated by slashes. Columns: mean, median, p95, worst (ms) per run; host and render columns are medians of the runs; CPU is the median process CPU in percent of one core; load is the range of total CPU use sampled before the three runs.

| renderer / timing | window | pictures | panes | runs | mean ms | median ms | p95 ms | worst ms | host ms (mean) | render ms (mean) | CPU % | load before % |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| default/frame | 1440x900 | default | 1 | 3 | 15.5/15.0/15.2 | 12.5/11.4/13.0 | 24.0/23.9/23.9 | 28.2/31.6/25.2 | 0.39 | 0.00 | 5 | 3-8 |
| default/frame | 1440x900 | default | 4 | 3 | 15.4/15.7/15.5 | 12.8/13.6/14.2 | 23.6/23.8/23.5 | 30.9/41.5/30.4 | 1.81 | 0.00 | 30 | 3-8 |
| default/frame | 1600x900 | default | 1 | 3 | 15.5/15.3/15.1 | 12.9/12.6/13.0 | 24.1/24.4/23.8 | 25.2/39.3/28.5 | 0.36 | 0.00 | 6 | 2-8 |
| default/frame | 1600x900 | default | 4 | 3 | 15.5/15.4/15.5 | 12.5/12.9/13.2 | 23.8/23.5/23.5 | 37.2/36.5/30.4 | 1.81 | 0.00 | 29 | 3-8 |
| default/frame | 1440x900 | fitted | 1 | 3 | 15.5/15.1/15.1 | 10.6/12.8/13.4 | 24.4/23.9/23.8 | 25.6/38.1/39.9 | 0.14 | 0.00 | 7 | 3-9 |
| default/frame | 1440x900 | fitted | 4 | 3 | 15.2/15.2/15.0 | 10.0/10.9/10.4 | 23.7/24.3/23.9 | 25.6/25.7/25.2 | 0.58 | 0.00 | 9 | 3-7 |
| default/frame | 1600x900 | fitted | 1 | 3 | 15.5/15.1/15.2 | 14.0/13.1/12.8 | 24.5/23.8/23.7 | 29.2/38.7/132.0 | 0.17 | 0.00 | 7 | 3-20 |
| default/frame | 1600x900 | fitted | 4 | 3 | 15.4/15.1/15.1 | 10.5/10.3/10.6 | 24.1/24.0/24.0 | 27.0/26.0/27.2 | 0.66 | 0.00 | 14 | 3-17 |
| headless-software/synchronous | 1440x900 | default | 1 | 3 | 1.9/1.9/1.8 | 1.8/1.8/1.7 | 2.4/2.4/1.9 | 3.5/4.6/4.9 | 0.21 | 1.64 | 56 | 3-8 |
| headless-software/synchronous | 1440x900 | default | 4 | 3 | 4.7/4.7/4.5 | 4.7/4.6/4.5 | 5.0/5.2/4.9 | 5.3/10.8/13.0 | 0.96 | 3.62 | 54 | 3-9 |
| headless-software/synchronous | 1600x900 | default | 1 | 3 | 2.0/2.0/1.9 | 1.9/1.9/1.9 | 2.2/2.1/2.1 | 2.7/5.9/5.1 | 0.20 | 1.75 | 68 | 3-8 |
| headless-software/synchronous | 1600x900 | default | 4 | 3 | 5.0/5.0/4.8 | 5.0/4.9/4.8 | 5.4/5.3/5.2 | 5.7/13.7/7.5 | 0.98 | 3.93 | 60 | 3-8 |
| headless-software/synchronous | 1440x900 | fitted | 1 | 3 | 1.7/1.6/1.6 | 1.7/1.6/1.6 | 2.1/1.7/1.7 | 3.8/3.9/3.4 | 0.07 | 1.54 | 74 | 3-8 |
| headless-software/synchronous | 1440x900 | fitted | 4 | 3 | 3.9/3.8/3.9 | 3.9/3.8/3.8 | 4.2/4.1/4.1 | 5.6/8.2/6.7 | 0.34 | 3.46 | 65 | 3-7 |
| headless-software/synchronous | 1600x900 | fitted | 1 | 3 | 1.9/1.8/1.8 | 1.8/1.8/1.7 | 2.1/1.9/2.0 | 3.9/4.4/4.5 | 0.08 | 1.73 | 54 | 4-50 |
| headless-software/synchronous | 1600x900 | fitted | 4 | 3 | 4.2/4.3/4.1 | 4.2/4.2/4.1 | 4.5/4.6/4.4 | 5.8/8.3/6.7 | 0.39 | 3.73 | 62 | 3-8 |
| winit-femtovg/frame | 1440x900 | default | 1 | 3 | 8.0/8.0/8.0 | 8.1/8.1/8.2 | 9.1/9.1/9.1 | 9.4/9.3/9.4 | 0.25 | 1.31 | 3 | 3-7 |
| winit-femtovg/frame | 1440x900 | default | 4 | 3 | 8.1/8.0/8.0 | 8.3/8.1/8.2 | 9.0/9.0/9.1 | 9.3/11.1/11.0 | 1.04 | 3.91 | 3 | 3-8 |
| winit-femtovg/frame | 1600x900 | default | 1 | 3 | 8.1/8.1/8.1 | 8.1/8.1/8.3 | 9.1/9.1/9.1 | 9.6/9.3/16.4 | 0.25 | 1.29 | 1 | 3-8 |
| winit-femtovg/frame | 1600x900 | default | 4 | 3 | 8.0/8.1/8.1 | 8.2/8.2/8.2 | 9.1/9.1/9.1 | 9.4/16.8/15.0 | 1.04 | 4.06 | 1 | 2-7 |
| winit-femtovg/frame | 1440x900 | fitted | 1 | 3 | 8.1/8.0/8.1 | 8.4/8.1/8.1 | 8.8/9.1/9.1 | 9.6/9.6/9.6 | 0.09 | 0.66 | 0 | 4-7 |
| winit-femtovg/frame | 1440x900 | fitted | 4 | 3 | 8.1/8.0/8.0 | 8.1/8.4/8.3 | 9.1/9.0/9.1 | 9.8/10.2/13.0 | 0.36 | 1.98 | 1 | 3-9 |
| winit-femtovg/frame | 1600x900 | fitted | 1 | 3 | 8.2/8.0/8.0 | 8.5/8.1/8.1 | 8.8/9.0/9.1 | 10.8/9.4/10.7 | 0.11 | 0.71 | 2 | 3-49 |
| winit-femtovg/frame | 1600x900 | fitted | 4 | 3 | 8.0/8.0/9.6 | 8.1/8.2/8.3 | 9.1/9.1/9.1 | 14.7/9.3/634.3 | 0.42 | 2.16 | 9 | 5-8 |
| winit-femtovg/render | 1440x900 | default | 1 | 3 | 8.3/8.4/8.3 | 8.3/8.2/8.2 | 9.2/9.3/9.2 | 10.0/13.2/10.2 | 0.25 | 1.46 | 0 | 2-8 |
| winit-femtovg/render | 1440x900 | default | 4 | 3 | 8.3/8.3/8.3 | 8.2/8.3/8.3 | 9.3/9.3/9.2 | 13.0/12.6/15.8 | 1.00 | 3.98 | 2 | 3-7 |
| winit-femtovg/render | 1600x900 | default | 1 | 3 | 8.3/8.3/8.3 | 8.3/8.2/8.2 | 9.2/9.2/9.3 | 10.2/10.6/10.7 | 0.25 | 1.39 | 1 | 3-8 |
| winit-femtovg/render | 1600x900 | default | 4 | 3 | 8.3/8.3/8.3 | 8.2/8.3/8.3 | 9.3/9.3/9.2 | 9.8/15.7/12.6 | 1.05 | 4.05 | 2 | 3-7 |
| winit-femtovg/render | 1440x900 | fitted | 1 | 3 | 8.4/8.3/8.3 | 8.4/8.2/8.2 | 8.9/9.2/9.2 | 9.6/10.0/9.7 | 0.08 | 0.65 | 0 | 3-8 |
| winit-femtovg/render | 1440x900 | fitted | 4 | 3 | 8.3/8.3/8.3 | 8.2/8.3/8.4 | 9.3/9.3/9.2 | 9.7/10.6/13.8 | 0.37 | 1.99 | 1 | 3-8 |
| winit-femtovg/render | 1600x900 | fitted | 1 | 3 | 8.4/8.3/8.3 | 8.5/8.3/8.2 | 9.0/9.1/9.2 | 11.8/9.9/9.7 | 0.10 | 0.72 | 0 | 3-50 |
| winit-femtovg/render | 1600x900 | fitted | 4 | 3 | 8.3/8.3/8.3 | 8.2/8.3/8.3 | 9.3/9.2/9.1 | 10.6/9.9/17.5 | 0.40 | 2.00 | 1 | 3-7 |
| winit-skia-opengl/render | 1440x900 | default | 1 | 3 | 8.3/8.3/8.3 | 8.2/8.2/8.2 | 9.2/9.2/9.1 | 9.7/11.9/10.3 | 0.25 | 1.27 | 1 | 3-8 |
| winit-skia-opengl/render | 1440x900 | default | 4 | 3 | 8.3/8.3/8.3 | 8.3/8.4/8.3 | 9.4/9.3/9.4 | 13.2/15.4/11.9 | 1.06 | 4.29 | 2 | 3-8 |
| winit-skia-opengl/render | 1600x900 | default | 1 | 3 | 8.3/8.3/8.3 | 8.2/8.3/8.2 | 9.2/9.2/9.1 | 10.4/10.3/9.6 | 0.24 | 1.26 | 0 | 3-8 |
| winit-skia-opengl/render | 1600x900 | default | 4 | 3 | 8.3/8.3/8.4 | 8.3/8.3/8.3 | 9.4/9.4/9.3 | 10.9/15.8/91.4 | 1.08 | 4.44 | 4 | 2-8 |
| winit-skia-opengl/render | 1440x900 | fitted | 1 | 3 | 8.4/8.3/8.3 | 8.5/8.3/8.2 | 8.9/9.1/9.1 | 9.4/9.6/9.4 | 0.09 | 0.71 | 0 | 3-8 |
| winit-skia-opengl/render | 1440x900 | fitted | 4 | 3 | 8.3/8.3/8.3 | 8.5/8.3/8.3 | 9.1/9.2/9.2 | 10.0/11.7/9.8 | 0.36 | 2.24 | 2 | 2-8 |
| winit-skia-opengl/render | 1600x900 | fitted | 1 | 3 | 8.3/8.3/8.5 | 8.5/8.3/8.2 | 8.9/9.1/9.1 | 9.5/15.3/99.1 | 0.10 | 0.88 | 2 | 3-50 |
| winit-skia-opengl/render | 1600x900 | fitted | 4 | 3 | 8.3/8.3/8.3 | 8.4/8.3/8.2 | 9.2/9.2/9.2 | 15.3/9.8/9.8 | 0.41 | 2.32 | 2 | 2-8 |
| winit-software/frame | 1440x900 | default | 1 | 3 | 8.1/8.1/8.2 | 8.5/8.5/8.5 | 8.9/9.1/9.3 | 19.9/23.0/17.5 | 0.31 | 0.00 | 2 | 3-9 |
| winit-software/frame | 1440x900 | default | 4 | 3 | 8.3/8.2/8.2 | 8.3/8.4/8.3 | 9.2/9.2/9.1 | 23.2/15.7/19.5 | 1.21 | 0.00 | 14 | 3-8 |
| winit-software/frame | 1600x900 | default | 1 | 3 | 8.0/8.3/8.3 | 8.5/8.5/8.5 | 9.0/9.1/9.4 | 16.5/22.6/18.7 | 0.26 | 0.00 | 5 | 3-7 |
| winit-software/frame | 1600x900 | default | 4 | 3 | 8.2/8.1/8.2 | 8.3/8.2/8.3 | 9.2/9.1/9.1 | 19.5/21.3/20.6 | 1.21 | 0.00 | 14 | 2-8 |
| winit-software/frame | 1440x900 | fitted | 1 | 3 | 8.4/8.2/8.3 | 8.5/8.1/8.2 | 10.0/9.2/11.2 | 19.7/25.5/21.5 | 0.12 | 0.00 | 2 | 3-8 |
| winit-software/frame | 1440x900 | fitted | 4 | 3 | 8.2/8.4/8.3 | 8.4/8.4/8.3 | 9.4/11.1/10.7 | 18.1/22.9/25.1 | 0.43 | 0.00 | 7 | 2-8 |
| winit-software/frame | 1600x900 | fitted | 1 | 3 | 8.9/8.2/8.1 | 8.5/8.4/8.3 | 15.7/9.1/9.0 | 40.0/22.5/18.2 | 0.13 | 0.00 | 2 | 3-50 |
| winit-software/frame | 1600x900 | fitted | 4 | 3 | 8.0/8.3/8.6 | 8.2/8.2/8.5 | 9.0/11.0/11.9 | 17.3/20.9/22.6 | 0.46 | 0.00 | 6 | 3-8 |
