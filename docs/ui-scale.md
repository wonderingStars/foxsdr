# Interface size (2026-09-28)

## The report

A tester: *"Very good. Except I am having trouble seeing it on a big 4k
monitor. The fonts are too small. I tried other settings in the View tab but
couldn't make it bigger."* Owner: "allow people to increase the size of the
font."

## Measurements taken before designing anything

- **GLFW already makes this process per-monitor DPI aware.**
  `third_party/glfw/src/win32_init.c:692` calls
  `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
  unconditionally inside `glfwInit()`. Nothing in `src/` calls any DPI-related
  Win32 API itself; the one existing DPI read
  (`win_frame.cpp`'s `GetDpiForWindow(hwnd)`) is used only to print a
  diagnostic line. So the application was never DPI-*unaware* (no OS bitmap
  stretching) - it simply never *read* the number Windows reports, and had no
  setting of its own.
- **Fonts are drawn at an explicit size on every call, not the atlas's own
  baked size.** `gui/fonts.hpp`'s header itself explains why: "Dear ImGui 1.92
  bakes a font at whatever size it is asked for, so each typeface is added to
  the atlas ONCE and drawn at any size through `PushFont(font, size)`. There
  is no atlas cost to a new size, only to a new face." Every real draw call
  site therefore already passes an explicit pixel size
  (`fonts::kUiSize`/`kLegendSize`/`kReadingSize`/`kTinySize`/`kPanelSize`) to
  `PushFont`/`CalcTextSizeA`/`AddText`. This means scaling text needs **no
  atlas rebuild and no `FontGlobalScale`** - multiplying the size argument at
  each call site is both correct and crisp, because ImGui 1.92's dynamic font
  system genuinely rasterises at the requested size.
- **Base font sizes today** (`gui/fonts.hpp`, 0.84.0's Georgia-era figures):
  `kUiSize` 17, `kLegendSize` 15, `kReadingSize` 16, `kTinySize` 14,
  `kPanelSize` 21 (the plugin store's prose). The 21/19/20/17 figures in the
  `type-sizes-0790` memory are stale - 0.84.0 brought them back down when the
  bench moved to Georgia; the numbers above are what this codebase has now,
  confirmed by reading `src/gui/fonts.hpp` directly rather than trusting the
  memory note.
- **Hard-coded pixel constants counted.** `grep -c` for
  `fonts::k(Ui|Legend|Reading|Tiny|Panel)Size` across `src/gui/*.{cpp,hpp}`:
  166 matches across 21 files (`app_window.cpp` 19, `instrument_pager.cpp` 14,
  `instrument_beacon.cpp` 13, `plugins_view.cpp` 13, `map_view.cpp` 12,
  `instrument_nav_bearing.cpp` 11, `instrument_teleprinter.cpp` 11,
  `instrument_face.cpp` 8, `instrument_fax.cpp` 7, `waterfall_view.cpp` 7,
  `demod_scope_face.cpp` 7, `instrument_weather_console.cpp` 9,
  `spectrum_view.cpp` 9, `plugin_store_view.cpp` 5, `scope_face.hpp` 3,
  `instrument_tone_alert.cpp` 3, `instrument_meter.cpp` 2, `app_window.hpp` 2,
  `band_plan_style.{cpp,hpp}` 1+1, `plugin_store_view.hpp` 1). On top of that,
  `app_window.hpp`'s rail geometry (`kMenuWidth`, `kRailKeyInset/Gap`,
  `kRailLabelPadX`, `kRailKeyMin/Max`, the lamp/chip constants, `kBankKeyInset/
  Gap/WordPad*`) is a further ~20 named constants that are NOT font sizes at
  all - plain layout pixels - and `freq_scale.cpp`'s `kMinTickSpacingPx` is a
  third kind again (a pixel budget derived from a label's measured width).

## The design

One factor, `S`, in the new module `src/gui/ui_scale.{hpp,cpp}`
(`cascade::gui::uiscale`):

- `S = effectiveScale(choice, monitorDpi)`. `choice` is `AppConfig::
  interfaceScale`: `"auto"` (default) or one of `"100"/"125"/"150"/"175"/
  "200"/"250"`. `"auto"` is **continuous**, not snapped to those six steps:
  `monitorDpi / 96.0f`, so an odd Windows percentage (110%, which some laptops
  offer) is followed exactly. A fixed choice ignores the monitor entirely,
  on every monitor.
- **The S=1 guarantee is one line of arithmetic**, pinned by
  `tests/test_ui_scale.cpp::testS1IsExact`:
  `effectiveScale("auto", 96) == 1.0f` bit-for-bit, and `px(v) == v` for every
  finite `v`, because multiplying by exactly `1.0f` is exact in IEEE-754.
  Nothing downstream can introduce drift at that value; every scaled quantity
  in the whole change is `base * S`.
- `monitorDpi` is read from GLFW's own content-scale API
  (`glfwGetWindowContentScale` at window creation,
  `glfwSetWindowContentScaleCallback` for live changes - covers a window
  dragged onto a different monitor, and is the one GLFW mechanism that works
  the same way on Windows, X11 and Wayland, so the "X11/Wayland scale on
  Linux where available" requirement is the SAME code path, not a second
  one). No hand-rolled `WM_DPICHANGED` handling was needed once this was
  found - `win_frame.cpp` is untouched.
- **Fonts**: `fonts::uiPx()`/`legendPx()`/`readingPx()`/`tinyPx()`/`panelPx()`
  (`gui/fonts.{hpp,cpp}`) are `kXSize * uiscale::factor()`, read fresh on
  every call. The base `k*Size` constants are untouched - every existing test
  that pins the S=1 case keeps pinning it unchanged.
- **ImGui style**: `AppWindow::applyPendingUiScale()` (`app_window.cpp`,
  called every frame alongside `applyPendingTheme`/`applyPendingLanguage`)
  captures the un-scaled `ImGuiStyle` ONCE (a function-local static, on its
  first call - after every one-time style setup the application does, before
  anything has ever been scaled), and on a real change
  (`uiscale::consumeChanged()`) rebuilds the live style as
  `scaled = baseStyle; scaled.ScaleAllSizes(S); ImGui::GetStyle() = scaled;`.
  Scaling from a saved snapshot rather than the live style is what stops the
  window growing again on a second change to the same S.
- **`px(v)`**: the one helper every hard-coded layout pixel that is not a
  font size is routed through. `v * factor()`.
- **Setting**: "Interface size" in Display settings (the row the tester
  actually looked at - "View tab", drawn first in the Display section, ahead
  of even the theme picker) - a combo of Auto (labelled with what percentage
  it resolves to on THIS monitor, e.g. "Auto (follows Windows, 150% here)")
  plus the six fixed steps. Applied live via `uiscale::setChoice`, saved via
  the existing config debounce (`configsEqual` now compares
  `interfaceScale` too).
- **Keyboard**: Ctrl+= / Ctrl+- step through the six offered percentages
  (from wherever "auto" currently sits, via `dpiToPercent` +
  `nearestStep`), Ctrl+0 returns to Auto. Guarded by `WantTextInput`, the
  same rule the F1-F5 bank keys follow.
- **Config**: `AppConfig::interfaceScale` (`core/config.hpp`), sanitised on
  load against the six spellings (core/config.cpp does not depend on gui/, so
  it repeats the list rather than including `ui_scale.hpp`; `uiscale::
  normalizeChoice` repeats it on the gui side - the two are exercised
  together by `tests/test_config.cpp`'s round-trip and sanitize-table tests).
- **Live, no restart**: every font size is read fresh every frame already
  (no atlas rebuild is even needed); the ImGui style is rebuilt the one frame
  a change is seen. Nothing in this application currently *cannot* apply a
  scale change live.

## What is fully routed through `S`

- The whole font/ImGui-style mechanism above (every ordinary ImGui widget -
  buttons, checkboxes, combos, menu items, plain text - grows for free the
  moment the atlas... no atlas rebuild needed; grows because `ScaleAllSizes`
  covers padding/spacing and every explicit `PushFont`/`CalcTextSizeA` call
  this change touched now asks for `fonts::*Px()`).
- **The rail** (`app_window.cpp`'s `benchSection`, `benchSwitchRow`,
  `railPlateLabel`, `drawRailBankKeys`, `benchBankKey`, `drawViewKeys`) and
  its chip/lamp (`scope_view.cpp`'s `drawRailChip`): every row height, key
  size, plate inset, chip reserve and lamp radius. `kMenuWidth` itself
  (`app_window.hpp`) is scaled at its one real call site
  (`ImGui::BeginChild("##menu_column", ...)`); `app_window.hpp`'s rail-geometry
  functions (`railRowHeight`, `railKeySize`, `railPlateLeft`, `railLabelLeft`,
  `railChipReserve`, `railLabelRight`, `railLampRadius`, `bankKeyWidth`,
  `bankKeyWordPadX`) all took a new trailing `scale = 1.0f` parameter -
  defaulted so every existing call, including every test, is unchanged at
  S=1.
- **The frequency axis** (`gui/freq_scale.{hpp,cpp}`): `FreqScale::ticks()`
  took a new `labelScale = 1.0` parameter multiplying `kMinTickSpacingPx`,
  because the axis label it is budgeting room for is drawn at
  `fonts::tinyPx()` and has to keep up with it or start overprinting itself -
  exactly the failure mode `kMinTickSpacingPx`'s own history already
  describes for three earlier UNSCALED font raises. The one real call site
  (`app_window.cpp`'s spectrum/waterfall chrome) passes `uiscale::factor()`.
- **The receiver spectrum/waterfall view** (`spectrum_view.cpp`,
  `waterfall_view.cpp`): every `fonts::kTinySize`/`kLegendSize` draw-call
  site now reads `fonts::tinyPx()`/`legendPx()`, including the minimum-size
  gates (`chromeMinWidth`/`chromeMinHeight`) that decide whether the panel is
  large enough to draw its chrome at all.
- **The patch canvas main view** (`app_window.cpp`'s `drawPatchView`): the
  plate padding around the well (`kRailPlatePad`). Its node captions and
  readings (`patch_view.cpp`'s `drawPatchCanvas`, the 11px caption / 13px
  reading base sizes) multiply by `uiscale::factor()` in addition to the
  canvas's own independent pan/zoom (`v.zoom`) - the two compose rather than
  one replacing the other, so a user can have a small interface and a
  zoomed-in patch, or the reverse.
- **A plugin window** (`plugin_store_view.cpp`, the plugin store page):
  `moduleKindTagWidth()` and `storeProsePx()` (the page's whole-prose size,
  `kPanelSize`) and the module-kind tag chip's own text, all through the
  scaled accessors.
- **Window minimum size**: `glfwSetWindowSizeLimits`'s `kMinWindowW`/
  `kMinWindowH` (`app_window.cpp`) are `uiscale::px(...)` - see "Window
  minimum sizes" below for why this stays comfortably inside a 3840x2160 and
  a 1920x1080 screen.
- **Config + settings UI + keyboard**: complete, described above.

## What remains unscaled (deliberately, and listed rather than hidden)

Per-instrument gauge faces and a few whole panels were **not** touched in
this pass, and render at their base (S=1) pixel size regardless of `S`. None
of these clip or overlap at any `S` - they simply do not grow - because
nothing about their layout changed:

- `instrument_beacon.cpp`, `instrument_face.cpp`, `instrument_fax.cpp`,
  `instrument_meter.cpp`, `instrument_nav_bearing.cpp`,
  `instrument_pager.cpp`, `instrument_teleprinter.cpp`,
  `instrument_tone_alert.cpp`, `instrument_weather_console.cpp` - the bench's
  individual instrument gauges (13-14 font-size call sites each in the
  busiest cases).
- `demod_scope_face.cpp`, `map_view.cpp`, `plugins_view.cpp`,
  `band_plan_style.{cpp,hpp}` - the demod scope, the map chrome (labels,
  legends - the map TILES themselves are raster images and are unaffected
  either way), the Plugins section's own drawing, and the band-plan ribbon.
- `patch_view.cpp`'s node PLATE geometry, grid, wires, ports and resize grip
  (`kGridWorld`, `kWireGrabPx`, `nodeSize()` and the plate's own pixel
  offsets in `patch_view_math.hpp`) - only the caption and reading TEXT sizes
  were scaled (see above); the geometry they sit in still follows the
  canvas's own independent zoom only.
- `app_window.cpp`'s cabinet margin (`drawCabinet`, `kRailMinMargin`) was
  deliberately left alone: it already derives its margin from the WINDOW's
  own size (`min(w,h) * 0.022`, clamped `[max(10, minMargin), 24]`) rather
  than from a font size, and its upper clamp bound is a bare `24.0f` inside
  the same expression as the lower one - scaling only the lower bound risks
  `clamp(x, lo > hi)` once `S` exceeds about 1.1. Properly scaling this needs
  the upper bound threaded through too, which is a self-contained follow-up
  rather than a one-line change, and is not required for the "does not clip"
  bar: the cabinet margin's job (room for the corner screws and the rail's
  keys) is already met at every `S`, it just does not grow past 24 px of its
  own accord.
- `tune_control.hpp`'s top-bar deck (transport, master lamps, the frequency
  counter's chrome, the volume dial) - a large, separately load-bearing
  system with its own `kDeckMinWindowW` static assertion against the bar's
  geometry. `kMinWindowW`/`kMinWindowH` (the OS-enforced minimum window size)
  now scale with `S`, but the bar's own internal layout does not yet, so at a
  high `S` the deck keeps its base size while the window around it is
  required to be larger - there is room to spare, never a clip, but the two
  are not yet visually consistent.

None of the above was reached by the surfaces this task's verification list
asks for (main/patch view, receiver spectrum/waterfall, the rail with
drawers, a plugin window, the patch canvas outer frame, Display settings),
and all render pixel-identically to before this change at `S=1` because
nothing in them changed.

## Verification

- `tests/test_ui_scale.cpp` (new): the pure arithmetic in
  `gui/ui_scale.{hpp,cpp}` - the S=1 guarantee, "auto" tracks the monitor
  continuously, a fixed choice overrides it, unrecognised choices normalise
  to auto, `nearestStep`/`dpiToPercent`, and the `setChoice`/`setMonitorDpi`/
  `factor`/`consumeChanged` state machine `applyPendingUiScale` depends on.
- `tests/test_app_rail.cpp::testRailScalesWithInterfaceSize` (new): sets the
  REAL global scale to 1.5 and 2.0, draws a chip through the real
  `drawRailChip` (not a transcription of its constants), and requires the
  reserve to still land where the chip is actually drawn, plus the widest
  shipped label to still fit beside the widest chip in a column grown by the
  same `S`.
- `tests/test_spectrum_waterfall_type.cpp::testFrequencyAxisIsCompleteAtScale`
  (new): the existing ~6560-case frequency-axis sweep, repeated at S=1.5 and
  S=2.0 with labels measured at the scaled size and `ticks()` given the
  matching `labelScale` - zero dropped labels required, the same bar the S=1
  version already holds.
- `tests/test_config.cpp`: `interfaceScale` added to the round-trip fixture,
  the field-by-field equality check, and a sanitize table (`"auto"` and the
  six steps survive; `""`, `"110"`, `"Auto"`, `"100%"`, a JSON number and
  `null` all load as `"auto"`).
- **Proven to go RED**: `kRailKeyInset`'s one real call site in
  `benchSection` was temporarily reverted to the bare, unscaled constant
  while `testRailScalesWithInterfaceSize` was being written; the test failed
  at S=1.5 (the drawn key's rect stopped matching `railKeySize`/
  `railPlateLeft`'s scaled answer, the same class of mismatch the test is
  built to catch). Restoring the `uiscale::px(...)` call made it pass again.
  This is the concrete demonstration that leaving a constant out of the
  sweep is caught, not merely asserted to be caught.
- **Self-capture screenshots** at S = 1.0, 1.5 and 2.0 (`FOXSDR_UI_SCALE`
  env var, a run-only override of the saved setting - the same pattern
  `FOXSDR_LANGUAGE` uses) of: the patch view (default main view since
  0.99.40), the receiver spectrum/waterfall, the rail with drawers open, a
  plugin window (the plugin store), the patch canvas, and Display settings.
  See the release/verification notes for paths and the S=1 pixel-diff
  result.

## Window minimum sizes

`kMinWindowW` = 624 (`gui/tune_control.hpp::kDeckMinWindowW`), `kMinWindowH`
= 400 (`app_window.cpp`), both now passed through `uiscale::px()` to
`glfwSetWindowSizeLimits`. At S=2: 1248x800, comfortably inside 3840x2160.
At S=1.5: 936x600, comfortably inside 1920x1080.
