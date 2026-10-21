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

## Round 3: the STATUS column and the window caption

Round 2's own screenshot (`deck_wide_s200.png`, a 1920x1080 window at S=2)
still had two surfaces frozen at 100%: the whole right-hand STATUS column
(its plate title, every card - AUDIO - UNDERRUNS, DECODER OUTPUT, DECODERS,
SINK, RECORDER, WEB ACCESS, RECEIVER - and the REQUEST A FEATURE / REPORT A
BUG - DISLIKE keys at its foot), and the frameless window's own caption text
("FoxSDR 0.99.42" at the top-left; the caption buttons already scaled).

**`AppWindow::drawStatusColumn()`** (`app_window.cpp`) is now scaled
literal-by-literal: every padding, gap, card-height term and footer-key
offset multiplies by a local `const float s = uiscale::factor();`, and the
two font-size locals it draws with (`tinyPx`, `valuePx`/`baseValuePx`) read
`fonts::tinyPx()`/`fonts::uiPx()` instead of the bare base constants. The
card lambda's `room` (how much width a card's value/caption text is fitted
and then clipped to) is derived from the now-scaled card bounds, so the
existing "fit smaller, then clip" behaviour - the SINK card's long
device-name truncation among it - is unchanged in logic and simply operates
at the scaled size. `statusCaption`'s own internal size
(`fonts::kTinySize` -> `fonts::tinyPx()`) and `benchWordKey` (the function
that draws REQUEST A FEATURE / REPORT A BUG - DISLIKE, and every other bench
word key such as the map's follow key) needed the same fix.

**Two bugs turned out to be shared by more than the column that exposed
them**, which is why finding them here fixed other surfaces at the same
time:

- `scope_view.cpp`'s `addBenchPlate` - the plate-title-plus-rule primitive
  used by the rail's own plate, the STATUS column's "STATUS" title, AND
  `drawPatchView`'s "PATCH" title - clamped its title font to the UNSCALED
  `fonts::kLegendSize` instead of `fonts::legendPx()`. Fixed once, in the
  one shared function, rather than three times.
- `app_window.cpp`'s `drawRailChrome` - shared by the main window's own
  title bar ("##mainrail") and every page's title bar ("##pagerail") -
  clamped the window-title text the same way (`fonts::kLegendSize` ->
  `fonts::legendPx()`). This is the caption fix: "FoxSDR 0.99.42" now grows
  with `S` because the ceiling it was hitting no longer sits still.

**A regression this round introduced and then reverted, worth recording
because a "shipped" build would have made the exact bug report this task
started from worse, not better.** The center panel's rule for hiding the
STATUS column on a narrow window (`drawUi`, just above `drawStatusColumn`'s
call site) is `ImGui::GetContentRegionAvail().x > kStatusWidth + 520.0f` -
`kStatusWidth` is the column's own width, and `520.0f` is a floor on how
much raw spectrum/waterfall width has to remain before the column is worth
keeping. The first attempt at this round scaled BOTH terms
(`kStatusWidth + uiscale::px(520.0f)`), on the reasoning that "a bigger
column should ask proportionally more room first." Building and
re-capturing `deck_wide_s200.png` (1920x1080, S=2 - literally the
coordinator's own reference screenshot) with that change showed the STATUS
column had vanished entirely: the scaled floor asked for 1500px of content
region on a window that only had about 1040px to give after the (correctly
scaled) menu column and cabinet margins, versus roughly 980px asked for by
the original, unscaled floor. `520.0f` is a floor on absolute usable
spectrum pixels, not a font or a padding value, so it stays unscaled;
`kStatusWidth`'s own growth already raises the total ask by exactly the
amount the column itself grew. Caught by re-capturing and comparing against
the specific screenshot the bug was reported against, before this was ever
handed back - see "Verification" below for the same check made permanent
across S=1.0/1.5/2.0/wide-window.

**`tests/test_app_rail.cpp::testBenchPlateTitleScalesWithInterfaceSize`**
(new) draws a bench plate through the real `addBenchPlate` at S = 1.0, 1.5
and 2.0 and checks the returned body-top offset (title height plus a few
independently-scaled gaps) grows in step: measured 35.00 -> 52.50 -> 70.00
px, exactly 1.5x and 2.0x. **Proven to go red**: with the `addBenchPlate`
fix reverted (title clamped back to the unscaled base size), the same
measurement gives 35.00 -> 45.00 -> 55.00 - the gaps around a frozen title
still grow a little on their own, which is why a loose "did it grow at all"
check would have passed the broken case too. The thresholds
(`bodyTop[1] > bodyTop[0] * 1.4`, `bodyTop[2] > bodyTop[0] * 1.8`) sit
strictly between the two measured cases (ratios 1.286/1.571 broken vs.
1.5/2.0 fixed) and were checked against both by hand before being committed
- see the test's own comment for the numbers.

## Round 4: an Opus review - theme/scale composition, a squared font, and five
more surfaces

Rounds 1-3 were reviewed end to end (identity, hit-testing, persistence, the
patch's own zoom, and the full suites all separately confirmed passing) and
still came back REJECTED: two blocking bugs and five surfaces still wrong at
non-trivial window sizes.

**B1 - theme and interface size fought each other.** `applyPendingUiScale`
kept its "unscaled style" baseline as a snapshot captured ONCE, ever, on its
first call. `theme::applyTheme()` (theme.cpp) writes UNSCALED sizes
(`WindowPadding`, `FramePadding`, ...) and the NEW theme's colours straight
into the live `ImGuiStyle` on every theme change, with no idea the interface
can be scaled - and the stale baseline never learned about either. Pick
Daylight Lab, then 150%: the scale change rebuilt the style from the STALE
baseline (whichever theme was live the first time the function ever ran,
always "Today" in practice), so Daylight Lab's colours vanished. Pick a theme
while already at 150%: `applyTheme()`'s own unscaled numbers overwrote the
live style and nothing afterwards ever rescaled them, so the padding reset to
100%. Fixed by extracting the compose step into a pure, independently-tested
function, `cascade::gui::uiscale::composeStyle(unscaledBase, factor)`
(`gui/ui_style_compose.hpp`): `AppWindow::applyPendingTheme` now
re-baselines on whatever `applyTheme()` just wrote and immediately re-composes
at the CURRENT factor, and `applyPendingUiScale` composes from that same,
continuously-refreshed baseline. Either order now converges on the same
style - this theme's colours, sizes at the current S.
`tests/test_ui_scale.cpp::testThemeAndScaleComposeRegardlessOfOrder` drives
the REAL `theme::applyTheme()` through both orders and checks both the
colours and the sizes land the same either way, and separately reproduces
round 3's stale-baseline design to show it fails exactly what the fix passes
(mismatched colours one way, unscaled padding the other).

**B2 - `fittedButton`/`sameLineFittedText` scaled text by S twice.** Both
measured against `ImGui::GetFontSize()` - Dear ImGui 1.92's CURRENT rendered
size, already `FontSizeBase * FontScaleMain * ...` - fitted a smaller size
against that, and then pushed the fitted result straight into `PushFont`,
whose own size argument is documented as the PRE-FontScaleMain base. ImGui
reapplies `FontScaleMain` to whatever is pushed, so an S-scaled fit became an
S^2 one: at 200% a button captioned "AUDIO" rendered so oversized only "AUI"
fit inside its own frame ("SPE" for SPECTRUM, "VEC" for VECTOR, "AV" for
AVERAGE, "PER" for PERSIST - the demod scope's own signal/display keys).
Fixed with one shared function both callers route through,
`pushSizeForRenderedFit(fittedPx)` (`text_fit.hpp`), which divides the
already-fitted size back out by `uiscale::factor()` before it reaches
`PushFont` - at S=1 this is `fittedPx / 1.0f`, bit-exact. Audited every other
`GetFontSize()`-then-`PushFont` pattern in the codebase (`grep -rn
"GetFontSize()"` across `gui/`): every other site feeds a raw
`ImDrawList::AddText`/`ImFont::CalcTextSizeA` call, which takes its size
literally and needed no change - only these two functions had the bug.
Also fixed in the same pass: the demod scope's own signal/display keys were
84 px wide, unscaled (`ImVec2(84.0f, 0.0f)`) - now `uiscale::px(84.0f)`, so
the key itself grows enough to give `fittedButton` more than a sliver of room
as S rises. `tests/test_text_fit.cpp::
testFittedSizeDoesNotDoubleScaleWithInterfaceSize` proves the ImGui contract
directly (`PushFont`/`GetFontSize`/`PopFont`, real font, real style) at S =
1.0/1.5/2.0, and proves it goes RED by pushing the undivided size and showing
ImGui renders it wrong at S>1 (checked with a tolerance of one rounded pixel,
`IM_ROUND` in `imgui.cpp`'s `UpdateCurrentFontSize` - Dear ImGui rounds every
computed font size to the nearest whole pixel, which is not the bug and
would fail a tighter check for the wrong reason).

**M3 - the plugin store's SORT row and its search legend.** `drawSegment`
(the NAME/MAKER/VERSION segmented control) drew its label centred at one
fixed size with no fit and no clip - at the interface size's larger fonts
"MAKER" ran into "VERSION". Fixed by fitting through `text_fit.hpp`, the
same as every other bench control. The "Searches name, maker and
description." legend was measured for `tinyH` (one line) when it draws
WRAPPED to `wellInner` - at a large enough font it wraps to two lines, and
the reserved height for one line let "0 OF 0 MODULES KNOWN" start where the
second line still was. Fixed by reserving `wrapH(uf, tiny, wellInner,
searchLegend)` (the actual wrapped height) instead of a flat one-line
constant, in both the deck's own height budget and the advance after drawing
it.

**M4 - patch node PLATES did not grow with S while their FACE CONTROLS did.**
The node face's controls (`AppWindow::drawPatchFaces`) are bound through
`PushFont(nullptr, FontSizeBase * zoom * 0.92f)` - `FontSizeBase` is the
PRE-scale base, so `FontScaleMain` correctly adds S on top, and the controls
grow with the interface size exactly as everything else does. The PLATE
around them (drawn by `patch_view.cpp`'s `drawPatchCanvas`, from the same
`zoom` with no `S` at all - deliberately, round 2, to avoid ever baking S
into the user's PERSISTED patch zoom) never grew to match: "RADIO  Radio" ran
half off its own header, "Signal generator" wrapped inside a box that never
grew to fit it, and "Ready - press START." vanished. Fixed by composing S
onto zoom AT DRAW TIME ONLY, in a value that is never written back:
`patch_view.cpp` keeps its real `View v` (the one `zoomAbout` reads and
writes, untouched) and adds `const View dv{v.pan, v.zoom *
uiscale::factor()}` right after the pan/zoom block, then every drawing and
hit-testing call in the rest of the function - the node box, ports, wires,
captions - was renamed onto `dv`; the two font-size sites that had their own
explicit `* uiscale::factor()` (now redundant, since `dv.zoom` carries it)
had that removed. `app_window.cpp`'s `drawPatchFaces` gets the same
treatment: `const float drawZoom = zoom * uiscale::factor();`, used for
every SCREEN-SPACE size and position except the font `PushFont` two lines
below, which must keep the RAW `zoom` (FontScaleMain already adds S there;
adding it twice would be B2 again in a new place). Verified by reproducing
the review's own repro config (`patch200drag`'s saved `config.json`, copied
verbatim) at 200%: "RADIO  Radio", "ON  Signal generator", "0.000000 MHz"
and "Ready - press START." all now sit inside a plate sized to hold them,
each on one line. The side panel's own heading ("This patch can[not] run")
is drawn as `TextWrapped` now rather than `TextUnformatted` (which never
wraps), and the panel's own width (`kInspectorW`, previously a bare 236.0f)
is `uiscale::px(236.0f)` - both were part of the same M4 finding, the
heading running off the panel's edge at S=2.

**M5 - first launch at 200% opened a 640x360-logical-pixel window.** Nothing
in `AppConfig` persists a window size, so EVERY launch - not only the very
first - created the GLFW window at a fixed 1280x720 REAL pixels regardless of
the interface size. At S=2 that is a 640x360 window in the logical units
everything else is laid out in: cramped past useless. Fixed right after the
window's initial content-scale detection (still hidden, so no visible jump):
the window is resized to `1280*S x 720*S`, clamped to the WORK AREA of the
monitor it actually landed on (`monitorWorkareaForWindow`, a new shared
helper - GLFW has no direct "which monitor is a windowed, non-fullscreen
window on" query, so it is found the same way GLFW's own DPI logic would,
by which monitor's rect contains the window's position), so a 200% saved on
a 4K desk does not ask a 1366x768 laptop for a bigger window than its own
screen. At S=1 this is exactly 1280x720, unchanged - verified by screenshot
(1282x745, the established S=1 baseline dimensions) - and at S=2 on this
desktop's monitor it opened at 2562x1417 (2560x1440 minus the taskbar).
Window-size PERSISTENCE itself (the review's own "if cheap") was left undone
- there is no saved width/height field in `AppConfig` today, and adding one
is a feature addition beyond this round's scope, not a one-line fix.

**Minors addressed**: `glfwSetWindowSizeLimits` is now re-applied inside
`applyPendingUiScale` itself (not only once at startup), through the same
`monitorWorkareaForWindow` helper, so a LATER scale change - Ctrl+=/-, the
Display combo, or a drag to a different-DPI monitor - never leaves the OS
enforcing a minimum sized for whatever S the window happened to open at, and
never demands a minimum larger than the monitor the window is now on has to
give. Three more unscaled-text sites the round 1/2 sweeps missed: the rail's
own group-caption fit basis (`addBenchGroupCaption`, `scope_view.cpp`) and
its reserved row height (`benchGroup`, `app_window.cpp`) both read
`fonts::kTinySize` instead of `fonts::tinyPx()`, capping every "SIGNAL PATH"/
"DECODE"-style rail caption at S=1 forever (`fitTextPx` only ever shrinks its
input, never grows it); the satellite tracking page's "FOLLOWING %s" /
"The map moves on its own only while a target is followed." strip text, and
the key-binding help page's own cap-width measurement, both raw `AddText`
calls reading the bare constant with no `FontScaleMain` to rescue them. The
Display section's "Auto (follows Windows, 100% here)" combo item, clipped in
the closed preview at 100% ("...100% h..."), is now measured and the combo
widened to its own longest item (`ImGui::CalcTextSize` over every item,
`SetNextItemWidth` before the `Combo` call) rather than shortening the
English (which would have needed retranslating in all 33 catalogs for a
combo-width problem, not a translation-length one). The demod scope's own
readout row (`demod_scope_face.cpp`'s `addReadouts`) had the same shape of
bug as M3's SORT row: the left-aligned signal caption was drawn at one fixed
size with no fit and no idea the centre-aligned time/span reading existed,
and overprinted it on a small enough face at S=2 ("DEMODULATED AUDIO"
running into "10 ms/DIV") - fixed by settling the centre reading's bounds
FIRST, then fitting the caption (and the display-mode word beside it) to the
room left before it.

**Not resolved this round, noted rather than hidden**: the waterfall's own
foot line (`waterfall_view.cpp::drawFootLines`, the "100.3000 MHz WFM -
STOPPED" plate) was found genuinely clipped by the window's own bottom edge
in a short, wide window at 200% (1920x1040) - reproduced and confirmed by
zooming into the captured pixels (the plate's border and the descenders of
"STOPPED" are cut, not merely tight). The function's own height budget
(`waterfallHeight`, computed from `ImGui::GetContentRegionAvail()` at the
top of the receiver view's draw call) and its existing "no room; better
nothing than a clipped half-sentence" bail-out both look internally
consistent against the `h` they are given; the discrepancy is between that
`h` and the TRUE remaining space in the window, which points at the split
between the spectrum and waterfall panels (or something drawn after the
point `avail.y` is read) rather than at `drawFootLines` itself. Given the
review's own bundling of this with two explicitly-optional items (numpad
+/- for the interface-scale shortcuts, listing them in the key-binding help)
and the time this round already ran to, it was investigated and reproduced
but not chased into the layout-budget code with any confidence of a correct
fix, rather than shipping a guess.

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
- `tests/test_app_rail.cpp::testBenchPlateTitleScalesWithInterfaceSize`
  (round 3, new): see "Round 3" above - proven to go RED against the
  reverted `addBenchPlate` fix (35.00 -> 45.00 -> 55.00, short of the
  required 1.4x/1.8x ratios) and GREEN against the real fix (35.00 -> 52.50
  -> 70.00, exactly 1.5x/2.0x).
- **Self-capture screenshots** at S = 1.0, 1.5 and 2.0 (`FOXSDR_UI_SCALE`
  env var, a run-only override of the saved setting - the same pattern
  `FOXSDR_LANGUAGE` uses) of: the patch view (default main view since
  0.99.40), the receiver spectrum/waterfall, the rail with drawers open, a
  plugin window (the plugin store), the patch canvas, and Display settings.
  See the release/verification notes for paths and the S=1 pixel-diff
  result.
- **S=1 re-checked in round 3** against a fresh build of `81691f2`
  (`git archive` into a scratch directory, built independently): the
  receiver, patch and plugin-store captures at S=1 differ from the baseline
  by at most 1 LSB on a handful of pixels (188/129/63 out of roughly
  950,000) with `extrema=(0,1)`, and the same magnitude and pixel count
  reappears between two S=1 captures of the SAME new binary run back to
  back - proving it is pre-existing per-run rendering jitter (almost
  certainly a live-updating element such as the simulated noise floor),
  not something round 3 introduced. The deck close-up, which has no such
  live content in frame, is byte-identical to the baseline.

## Window minimum sizes

`kMinWindowW` = 624 (`gui/tune_control.hpp::kDeckMinWindowW`), `kMinWindowH`
= 400 (`app_window.cpp`), both now passed through `uiscale::px()` to
`glfwSetWindowSizeLimits`. At S=2: 1248x800, comfortably inside 3840x2160.
At S=1.5: 936x600, comfortably inside 1920x1080.
