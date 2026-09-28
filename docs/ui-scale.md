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

## Round 2: `style.FontScaleMain`, the half the first pass missed

The first pass shipped `ScaleAllSizes` (padding/spacing) and the custom-drawn
rail/axis/patch text, and screenshotted it as done. A second look at
`receiver_s200.png` found the result inconsistent: **ordinary ImGui widgets -
the "Signal generator" combo, "Refresh", "Look for network USRPs", every
status-column line - were not growing at all**, and **the whole top deck
(START dome, MASTER lamps, the nixie counter, the VOLUME dial, the two
meters) was not growing either**, which is exactly the control cluster a 4K
user needs biggest.

The missing piece was `style.FontScaleMain` (ImGui 1.92's actual "scale every
font" knob; `imgui.h` - *"recap: ImGui::GetFontSize() == FontSizeBase *
(FontScaleMain * FontScaleDpi * other_scaling_factors)"*). `ScaleAllSizes`
never touches anything font-related - grep its body in `imgui.cpp` and there
is no font field in the list - so nothing using the DEFAULT bound font (every
plain `ImGui::Text`/`Button`/`Combo`/`Checkbox`, and every
`ImGui::PushFont(font, someBaseSize)` call anywhere in the codebase) had ever
been scaled. `applyPendingUiScale` now sets `scaled.FontScaleMain =
uiscale::factor()` on the same style object `ScaleAllSizes` already rebuilds.

**This changes the rule for `ImGui::PushFont`, and it is the opposite of the
rule for raw `ImDrawList::AddText`/`ImFont::CalcTextSizeA`.** PushFont's size
argument is documented as the PRE-FontScaleMain base - passing an
ALREADY-scaled size (`fonts::uiPx()`, `capPx` computed from it, anything
carrying `uiscale::factor()`) into PushFont scales it a SECOND time. Every
real `ImGui::PushFont` call site in the codebase was audited for this
(`grep -rn "ImGui::PushFont(" src/gui/*.cpp`, 20 hits) and fixed where it
mattered: `benchSection`'s header font (`app_window.cpp`), the master lamp
words and the bias-tee caption and the frequency-edit field on the deck, the
mute banner's font-size ladder, and three PushFont calls in
`plugin_store_view.cpp` that had started from `storeProsePx()` (already
scaled) - each now divides back out by `uiscale::factor()`, or (where the
value was a bare base constant already, as in every instrument face) is left
alone, because those get scaled automatically and correctly by
`FontScaleMain` with no code change at all.

**The top-bar deck now scales.** `drawToolbar` already drew everything
through one local `scale` and three `X()`/`Y()`/`S()` reference-unit lambdas
(`app_window.cpp`) - a pre-existing "shrink to fit a narrow window, never
grow past 100%" mechanism (`gui::deckScale`, `tune_control.hpp`). The fix
composes rather than replaces it:
`deckScaleOnly = deckScale(availW / S, layout, kBarMinScale)`, then
`scale = deckScaleOnly * S`. At S=1 this is `deckScale(availW, ...) * 1.0f` -
identical to before. At S>1 the deck asks "does availW hold my S-scaled
size", shrinking in the same absolute pixels the narrow-window rule always
used once it does not, and growing up to S otherwise - so the whole cluster
(dome, lamps, counter, dial) scales together, screenshotted at
`deck_s150.png`/`deck_s200.png`. `capPxBase` (a second, NOT-S-scaled variant
of the deck's caption size) exists purely to satisfy the PushFont rule above
for the master lamp words.
**The two meters (SAMPLE RATE, VOLUME) now scale too** - their reference
header called them "pinned to the bar's right edge, they do not scale",
which was true when "scale" only ever meant *narrower*; `kMeterW`,
`kMeterGap`, `kMeterRightMargin` and the face height are now multiplied by
the same combined `scale` in `drawToolbar`, and `deckMetersFit`
(`tune_control.hpp`) was unified onto `deckCoreW(c) * scale` for BOTH counter
sizes - the 1x-counter branch used to compare against the literal unscaled
`kDeckCoreW` regardless of `scale`, which was harmless while `scale` could
only shrink but would have let the meters overlap a cluster that had grown
past them. Screenshotted wide (`deck_wide_s150.png`, `deck_wide_s200.png`,
2200x900 and 2600x1000) to show the meters actually drawn, since the default
1280-wide window does not hold them at high `S` any more than it held them
at 1x on a narrow one - dropped, not overlapped, which is the existing
design's own answer to "not enough room".

**The rest of "what remains unscaled" from the first pass is now scaled for
its text, via the SAME sweep** (`fonts::kXSize` -> `fonts::xPx()` plus the
PushFont audit above): `instrument_beacon/face/fax/meter/nav_bearing/pager/
teleprinter/tone_alert/weather_console.cpp`, `demod_scope_face.cpp`,
`map_view.cpp`, `plugins_view.cpp`, `band_plan_style.{cpp,hpp}`. None of
these had a PushFont call using an already-scaled size except
`instrument_fax.cpp` (3 sites) and `instrument_meter.cpp` (1 site), both
fixed the same way. `band_plan_style.cpp`'s `bandRibbonGeometry` had a
`constexpr float kSmallLabelPx = fonts::uiPx();` - `fonts::uiPx()` reads
live process state and cannot be `constexpr`; changed to `const`, the only
compile fix this sweep needed. **What is NOT scaled in these files**: any
hard pixel constant that is not a font size - gauge dimensions, marker
sizes, padding unrelated to text - was out of scope for this pass; only the
lettering was swept.

**The cabinet margin clamp is fixed.** `drawCabinet`'s
`std::clamp(..., std::max(10.0f, minMargin), 24.0f)` had a LITERAL upper
bound that the first pass correctly identified as a hazard (scaling only the
floor could invert the clamp) and left alone. Both ends now scale
(`uiscale::px(10.0f)`, `uiscale::px(24.0f)`), and the two real call sites
(the main window's own cabinet, and every page's via `beginPage`) now pass
`uiscale::px(kRailMinMargin)` instead of the bare constant - fixing the
plugin store's own margin too, since it shares the same `drawCabinet`.

**Dropped in this round, on the coordinator's instruction**: clipping
`spectrum_view.cpp`'s header caption to its own panel. That fix is the
owner's own separate session (`task_dd24393d`). The font-size scaling this
change already carries there (`legendPx`/`tinyPx` in `drawChrome`, committed
in round 1) is untouched; nothing else in `spectrum_view.cpp` changed in
round 2.

**Patch node plate geometry remains unscaled, and was deliberately left
that way again.** The only clean way found to scale it uniformly - folding
`S` into the patch canvas's own `View::zoom` at construction - would also
feed back into `ui.view.zoom`, the value the mouse-wheel handler reads AND
WRITES every frame (`zoomAbout` in `patch_view_math.hpp` sets
`ui.view.zoom = v.zoom` after a scroll), which is itself part of the
persisted patch state. Composing S there risks the interface-scale factor
getting silently baked into a user's saved zoom the first time they
scroll-wheel-zoom a patch, compounding on every subsequent load. That is a
correctness risk to persisted user data, not merely a visual gap, and was
judged not safe to take under this task's time budget; only the caption/
reading TEXT sizes in `patch_view.cpp` (round 1) scale.

## What is fully routed through `S`

- The whole font/ImGui-style mechanism above (every ordinary ImGui widget -
  buttons, checkboxes, combos, menu items, plain text - grows for free via
  `style.FontScaleMain`, and `ScaleAllSizes` covers padding/spacing) plus
  every explicit `PushFont`/`CalcTextSizeA`/`AddText` call this change
  touched, each using the rule that matches how it reaches the screen (see
  "Round 2" above for exactly which rule applies where).
- **The top-bar deck** (`app_window.cpp`'s `drawToolbar`): the dome, the
  MASTER lamps, the bias-tee key, the frequency counter, the VOLUME dial and
  the SAMPLE RATE / VOLUME meters, composed with the deck's own existing
  narrow-window shrink rather than replacing it (Round 2, above).
- Every instrument face, the demod scope, map chrome, the Plugins section
  and the band-plan ribbon (their TEXT only - Round 2, above).
- The cabinet margin, both ends of its clamp, on the main window and on
  every page (Round 2, above).
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

After round 2, two things remain, both for stated reasons rather than by
omission:

- **`patch_view.cpp`'s node PLATE geometry**: the grid, wires, ports, resize
  grip and the plate's own pixel offsets (`kGridWorld`, `kWireGrabPx`,
  `nodeSize()` in `patch_view_math.hpp`) still follow the canvas's own
  independent zoom only - only the caption/reading TEXT sizes scale. See
  "Round 2" above for why: the clean fix (folding `S` into `View::zoom`)
  risks corrupting a user's PERSISTED patch zoom the first time they
  scroll-wheel-zoom, which was judged too risky to take under this task's
  time budget.
- **Non-font hard-coded pixels inside the instrument faces, the demod scope,
  map chrome, the Plugins section and the band-plan ribbon** - gauge
  dimensions, marker sizes, padding that is not a font size. Round 2 swept
  every FONT SIZE in these files (they now grow with `S`, either via
  `fonts::xPx()` in a raw draw or automatically via `style.FontScaleMain` in
  a `PushFont`), but did not attempt every other pixel constant they contain.

Both are listed here rather than silently left as they were. Neither clips
or overlaps at any `S` - they simply do not grow to the same degree the rest
of the interface now does.

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
