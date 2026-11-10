# Multi-radio screen, stages S1 and S2 (2026-11-10)

Branch `mr/screen-s1-s2`, cut from `engine/multi-radio` at d094501b. Design: `docs/multi-radio-screen-design.md` (read-only copy in the engine-stage4 worktree, section 4 build order) and the engine design's limit of eight radios. Two commits: S1 (host) and S2 (pane). Layout One is untouched: `RadioPane` is not placed in `MainWindow`.

## S1 - the host (commit "Slint host: the per-radio struct RadioView ...")

### What moved where

| From (main.cpp locals / lambdas) | To |
|---|---|
| `state` of the picture, `bins`, `frame`, `lastInfo`, `sinceSeq`, `traceState`, `view`, `wells`, `frameTimes`, `lastFrameMs`, `lastTunedHz`, `lastBandDragTick`, the picture half of `sigs` | `RadioView` (src/slint/radio_view.hpp), one per slot, held by `RadioSet` |
| `WellBuffers`, `kSpecW/H`, `kWfW/H` | radio_view.hpp; the buffers take a `PictureSize` (default 1024 x 340 trace, 1024 x 238 waterfall) |
| lambdas `fullCentre`, `fullSpan`, `currentView`, `frac`, `hzAtFrac`, `dbAtFrac`, `zoomView` | methods of `RadioView`; the lambdas in main.cpp remain as one-line calls on the selected radio |
| the spectrum half of the 530-line tick | `paneTick(RadioView&, PlotSize, selectedRadio)` (reads, paints, derives figures into `rv.fig`, touches no window property) and `publishPicture(RadioView&)` (figures to the main window's properties, per-radio signature cache) |
| literals 980 and 330 (axis ticks, marker hit test, marker-drop width) | `PlotSize` (`layoutOnePlot` = 980 x 330 today; `rv.plot` for gestures) |
| `kSpectrumPlotWidthPx` | the radio's painted picture width (`wells.size.traceW`, 1024 today: same value) |
| `submit(FoxCommand)` | `submit(FoxCommand, radio = 0)`; `aimAt()` writes `FoxCommand.target = slot + 1` for a non-zero radio, 0 leaves the command alone (the selected radio) |

Kept once per tick for the selected radio, as the design says: the strip, panels, banks, lists, plugin kit, the 3D history (`selectedRadio` flag in `paneTick`), all publishers. `state` in main.cpp remains the SELECTED radio's state (read once a tick); at the top of the tick it is copied into `selected().state`. Shared host state stays shared: trace mode, range, band-plan switch, average length.

### Interfaces the next stage binds to

```
constexpr int kMaxRadios = 8;
struct PlotSize    { int w = 980, h = 330; };
struct PictureSize { uint32_t traceW = 1024, traceH = 340, wfW = 1024, wfH = 238; };
struct WellBuffers { PictureSize size; traceMemory(); waterfallMemory(); traceImage(); waterfallImage(); flip(); int detaches, frames; ... };
struct PaneFigures {            // plain data, filled by paneTick; S3 turns it into one RadioPaneData
  bool newFrame; ViewWindow view; string spectrumCaption, spanText, footLeft, peakText, peakAge; bool peakAgeSet, showPeak;
  vector<Tick> ageTicks, freqTicks, dbTicks; float vfoPos, vfoHalf; vector<FigureBand> ribbon; vector<FigureBookmark> bookmarks;
  vector<FigureMark> marks, wfMarks; vector<FigureMarker> markers; int markerCount; string keyFoot, footTuned, footRest; vector<string> keyLabels; };
struct RadioView {
  int slot;                                  // 0-based; radio number = slot + 1 (radioNumberOf)
  FoxReceiverState state; vector<float> bins, frame; FoxSpectrumInfo lastInfo; uint64_t sinceSeq;
  TraceState traceState; ViewWindow view; WellBuffers wells; deque<int64_t> frameTimes; int64_t lastFrameMs;
  double lastTunedHz; int lastBandDragTick; Signatures sigs; PlotSize plot; PaneFigures fig;
  fullCentre() fullSpan() currentView() frac(hz) hzAtFrac(f) dbAtFrac(f) zoom(factor, aboutFrac) pan(dFrac) resetView();
};
class RadioSet { RadioView& at(slot) /* made on first use, clamped to 0..7 */; bool exists(slot); int created();
                 void setPictureSize(PictureSize); int selected; RadioView& selectedView(); };
inline void aimAt(FoxCommand&, uint32_t radioNumber);   inline uint32_t radioNumberOf(const RadioView&);
```

S3 supplies, per pane: a reader (`readSpectrum` in main.cpp is the one place that must switch on `rv.slot` once the engine has `read_radio_spectrum`), a `rv.state` fed by `read_radio_state` (today only the selected radio's state exists, copied in), and a `publishPane(rv)` beside `publishPicture(rv)`.

### Decisions (S1)

- `rv.state` is a copy of the strip's `state` for the selected radio, not a second read: the engine at d094501b has one `read_state` and one `read_spectrum`, both the focused radio's.
- A pane that is not on screen is not given to `paneTick`; its `RadioView` keeps `sinceSeq`, so its waterfall restarts when it returns, as the design says.
- The retune-follow rule moved into `paneTick` (it only reads the radio's state, view and `lastInfo`; nothing between its old place and the spectrum read used them).
- `RadioView` pulls `<slint.h>` (it owns `SharedPixelBuffer`s), so its test links Slint (`test_slint_radio_view`, registered in tests/CMakeLists.txt beside the cjk and scene3d_well special cases) rather than being a logic test.

### Proof (S1)

Build: `cmake --build D:/foxsdr-build/mr-screen1 --config Release --parallel` exit 0.

- New `test_slint_radio_view`: 49 checks, 0 failed (view arithmetic, two radios share nothing, slots on first use and the limit of eight, picture size as an argument, `aimAt`, the alternating buffers never detach). Break-it: `aimAt` writing the target unconditionally -> `FAIL test_slint_radio_view.cpp:116 d.target == 9u`, restored -> green.
- Headless gate `ctest -L "headless|slint" -j4 --timeout 300`: 737 tests (736 + 1). 9 failed: five (`test_airband_data`, `test_band_plan`, `test_converter_call_sites`, `test_page_resize_id`, `test_png_decode`) look for the repository by walking up from the working directory and the prescribed build folder is outside the repository (`FAIL !root.empty()`); each run from the repository root as a plain executable passes (`test_band_plan: 5174 checks, 0 failed`). Three load flakes (`test_pa_stream_list_guard`, `test_patch_iqfile`, `test_patch_radio`) passed alone. The ninth was my own new test's wrong sign on `panBy`, fixed.
- Window gate (`FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j4`, hidden desktop): 225 tests, 224 passed; `slint_drive_plugin-vor-decoder` failed under load and passed alone (19.19 s). No script, expectation or golden edited; every one of the 154 drive scripts ran unchanged.
- Ledger: regenerated after S1, `git status` showed `docs/slint-drive-coverage.md` unchanged (byte-identical); `--check`: `ledger: 2130 rows, 517 asserted, 83 driven, 1530 not drivable, 0 unclassified`.

## S2 - the pane (commit "Slint RadioPane ...")

### What was built

- `ui/radio_pane.slint`: `RadioPaneData` and `RadioPane` (interfaces in COMPONENTS.md, section "radio_pane.slint"; the callbacks all carry the radio's number first). Header 30 px, readout row 28 px, compact `SpectrumView`, `Splitter`, `WaterfallView` without its strength key, 2 px outline in the radio's colour when `selected`.
- `SpectrumView.compact` (spectrum.slint): `plot-top` becomes the ribbon plus its names (`ribbon-h + 15`, 0 with no ribbon) instead of 76; caption, caveat, trace-mode label, peak block, SPAN chip and the airband captions are not drawn; plugin-mark and bookmark names sit just inside the plot's top. Default `false`: the receiver view is byte-for-byte unchanged.
- `Skin.radio-1 .. radio-4` and `Theme.radio-colour(n)` (public pure function, 1-based, by position so radios 5 to 8 repeat 1 to 4). Workbench: blue #4da3ff, amber #f5a524, green #3ddc84, violet #b48cff. Night Watch keeps its all-red rule: #ff6a4c, #ffd2c6, #b83a26, #ff9a80 (the tag's number tells them apart).
- Specimen `ui/specimens/radio_pane_specimen.slint` (six panes: four at the hand-over's 556 x 353, two at 420 wide with a long device name) and `tests/test_slint_radio_pane.cpp`; COMPONENTS.md entry and file-table row; twelve DEFERRED rows 4200-4211 in docs/slint-port-tick.md (the pane's controls, "built, not placed until S3").
- Accessible labels: "Stop Radio 2" / "Start Radio 2", "Select Radio 2", "Radio 2 spectrum", "Radio 2 waterfall", "Resize Radio 2", "Radio 2" (the pane) arrive from the host in `RadioPaneData` (`stop-label`, `select-label`, `spectrum-label`, `waterfall-label`, `resize-label`, `prefix`); the pane builds the figures' labels as `prefix + ": " + text`: "Radio 2: 145.500000 MHz", "Radio 2: <device>", "Radio 2: <mode>", "Radio 2: <decoder>", "Radio 2: SPAN 2.400 MHz", "Radio 2: GAIN 32 dB". The specimen's English texts are the contract the host's `tr()` sentences must reproduce in S3/S5. The test asserts each exists exactly once across six panes and that the bare "STOP", "START", "Spectrum", "Waterfall" are no pane control (so no new label collides with the 86 scripts that use bare STOP/START).

### Decisions and flags (S2)

1. NO NEW CATALOGUE KEYS IN S2 (flag for S3/S5). `test_i18n` refuses any `@tr` key missing from the 33 catalogues AND any `src/core/lang_assets.hpp` that is stale (`py -3.14 tools/embed-lang.py` regenerates that 35 MB generated file in `src/core`, a folder this lane must not touch). I first added the 18 new words to the catalogues, found the generated header stale, and reverted. The pane therefore has no new `@tr` key: every sentence with a verb, a noun or the radio's number ("Stop Radio 2", ...) is built by the HOST and arrives in `RadioPaneData` (the host's `tr()` keys, with real translations, are S3/S5's); the readout captions are properties whose defaults are words the catalogues already hold (GAIN, SQUELCH, VOLUME, SAMPLE RATE, BIAS TEE, PEAK IN PASSBAND, ON, OFF, RUN, DEC, MUTE, FAIL, STOP, START), and the host passes the hand-over's short SQL / VOL / RATE / BIAS / PEAK once they are catalogue keys. The default captions are long, so with them the row drops figures sooner than in the specimen (which sets the short words).
2. THE READOUT ROW IS PLACED, NOT LAID OUT: it drops whole figures from the right (PEAK, decoder chip, BIAS, RATE, VOL, SQL, GAIN) so SPAN stays and no figure is cut in half. At 556 px every figure, the decoder chip and PEAK fit; at 420 px the decoder chip and PEAK are gone. Whether the decoder chip should outrank BIAS or RATE in a short pane is a design call left open.
3. The R-tag's text colour is `Theme.bg` on the radio's colour (readable on all four Workbench colours and the four reds as rendered; contrast not measured).
4. The pane carries its own `split-ratio` (0.45); the workspace may persist it per radio later.
5. `RadioPaneData` is one struct per pane (the design's `[RadioPaneData]` model). Cursor text/position are part of it (a pane's hover is that pane's).
6. Radio colours for Night Watch deviate from "R1 blue, R2 amber, R3 green, R4 violet" on purpose (that skin is red-only; `test_slint_theme` holds its look to it).

### Proof (S2)

Build: `cmake --build D:/foxsdr-build/mr-screen1 --config Release --parallel` exit 0.

- New `test_slint_radio_pane` (fake data for six radios, both skins, the element tree by accessible label, real pointer events): see the run line in the final report; it checks the four radio colours by position (5 and 6 repeat 1 and 2), the selection outline, the compact plot top at y 86 in every pane (134 would be the full header) and 58 with the ribbon off, the waterfall under the splitter, every label found exactly once across six panes, the caption rule (wide pane true, narrow false), the overflow rule (device name shrinks, frequency keeps its width and stays inside the pane), the readout row (PEAK in a 556 px pane, not in a 420 px one; decoder chip and SPAN), and that a click reports its own radio (STOP in pane 2 -> 2, tag of pane 3 -> select 3, spectrum click in pane 2 at x 0.25, wheel in pane 4, waterfall click in pane 3, the accessible default action of STOP).
- Headless gate `-L "headless|slint" -j4 --timeout 300`: 738 tests (736 + `test_slint_radio_view` + `test_slint_radio_pane`); 9 failed - the same five repository-root walkers as in S1 (pass from the repository root) and four load flakes (`test_engine_api_03`, `test_engine_control_thread`, `test_link_request_poll`, `test_health_app_stepped`) that passed alone. `test_slint_house_rules`, `test_slint_theme`, `test_slint_main_screen`, `test_slint_spectrum`, `test_i18n` passed.
- Window gate (hidden desktop, `FOXSDR_FRAME_CAP_HZ=60`, `-L gui -j4`): `100% tests passed out of 225`.
- Ledger: `ledger: 2142 rows, 517 asserted, 83 driven, 1542 not drivable, 0 unclassified` (+12 DEFERRED rows 4200-4211, nothing else moved).

- Break-it: `RadioPane` with `compact: false` -> `test_slint_radio_pane: 176 checks, 16 failed` (the plot starts at 134 instead of 86); restored -> green.
- Pictures: `src/slint/tests/slint-shots/radio-pane-workbench.png` and `radio-pane-night.png` under the build folder (looked at).

## Not covered

- Everything seen of the pane is FAKE DATA in a component test; `RadioPane` has not been in the real window, with a real radio, or on the hidden-desktop GUI beyond the existing tests (it is not placed).
- No per-radio engine reads exist at d094501b: `paneTick` can only be exercised for the selected radio through the real window, and there is no multi-pane path to run.
- The pane picture size (the stage S0 measurement) is not fixed: the images fill their wells and the buffers take `PictureSize`.
- Contrast ratios of the radio colours, the elide/clip sweeps at 1280 x 720 in the long languages (S6), tooltips for the pane's gestures, a link key (S5), the empty-slot card (S4).
- `docs/engine-stage4.md` was not edited, as instructed.
