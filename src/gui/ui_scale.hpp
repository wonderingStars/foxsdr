// ui_scale.hpp - ONE INTERFACE SCALE FACTOR, S, that every hard-coded layout
// pixel and every drawn font size is routed through, so the whole bench grows
// or shrinks together instead of drifting apart the way a font-only raise
// always has (see gui/fonts.hpp's own history of chasing kMenuWidth and the
// frequency axis pitch across three separate size increases).
//
// WHY THIS EXISTS. A tester on a 4K monitor: "I am having trouble seeing it
// on a big 4k monitor. The fonts are too small. I tried other settings in the
// View tab but couldn't make it bigger." There was no such setting. GLFW
// already makes this process per-monitor DPI aware (win32_init.c calls
// SetProcessDpiAwarenessContext itself during glfwInit — nothing in this
// application needs to ask for that), so on a 4K panel at Windows' own 150%
// or 200% scaling the window already renders at full native resolution with
// no OS stretching; it is simply a small window on a very big, very sharp
// screen, and until now nothing in the product read Windows' own scale
// number or offered a way to override it.
//
// THE MODEL. S is the product of two independent things, and a user's own
// choice always wins where they have made one:
//
//   - "auto" (the default): S follows the MONITOR the window is currently
//     on, continuously — GLFW's content-scale callback fires on every DPI
//     change, including one from being dragged to a different monitor, and
//     S becomes exactly dpi/96 with no snapping. On an ordinary 96 dpi
//     (100%) monitor this is exactly 1.0f, bit-for-bit, which is the whole
//     of the "must not change anything at S=1.0" guarantee: nothing else in
//     this file, or in any caller, can introduce drift at that value because
//     every scaled quantity is a plain multiply by a float that IS 1.0f.
//
//   - a fixed choice — 100/125/150/175/200/250 percent, offered in Display
//     settings as "Interface size" (the View tab a tester actually opened)
//     and by Ctrl+=/Ctrl+-/Ctrl+0 — pins S to that value on every monitor,
//     ignoring what Windows reports. This is the override the tester asked
//     for: "allow people to increase the size of the font" when Windows'
//     own number is not what they want, or is not being read correctly by
//     whatever compositor they are running.
//
// WHAT S DRIVES. Two kinds of quantity, and there is no third:
//
//   - A FONT SIZE passed to PushFont/CalcTextSizeA. Dear ImGui 1.92 draws a
//     loaded face at ANY requested size with no atlas cost (gui/fonts.hpp's
//     own header says so) — it is genuinely rasterised at that size, not
//     stretched — so multiplying the size argument by S is the whole of
//     "rebuilt at size*S, not blurry FontGlobalScale": fonts::uiPx() and its
//     three siblings below are that multiplication, done in one place so a
//     call site can never scale a size argument by hand and get it wrong.
//
//   - A HARD-CODED LAYOUT PIXEL that is not derived from a font size at all
//     — a column width, a key inset, an axis tick pitch, a window's minimum
//     size. px() is that multiplication. Everything ImGui derives from its
//     own style (WindowPadding, ScrollbarSize, ItemSpacing...) is scaled a
//     different way, once, by ImGui::GetStyle().ScaleAllSizes(S) against a
//     saved un-scaled snapshot (gui/app_window.cpp, applyPendingUiScale) —
//     never by px(), which would double it.
//
// PURE ARITHMETIC, NO ImGui AND NO WIN32 TYPES, for the same reason
// gui/app_window.hpp's rail geometry is pure: this header is included from
// that header and from test files that construct none of either.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_UI_SCALE_HPP
#define CASCADE_GUI_UI_SCALE_HPP

#include <string>

namespace cascade::gui::uiscale {

// The fixed choices offered in Display settings and by Ctrl+=/Ctrl+-, as
// whole percent. Widest first is not required; kept ascending because that is
// the order the combo and the keyboard step through it in.
inline constexpr int kSteps[] = {100, 125, 150, 175, 200, 250};
inline constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

// The config string for "follow the monitor" — AppConfig::interfaceScale's
// default, mirroring "auto" for languageSetting (core/i18n.hpp).
inline constexpr const char* kAuto = "auto";

// The offered step nearest `rawPercent`, clamped to the table's own ends. Pure
// so a test can pin it without a display; used only where a UI genuinely needs
// to snap to a step (Ctrl+= stepping up from an arbitrary "auto" value) — the
// "auto" scale itself is never snapped, see effectiveScale().
int nearestStep(int rawPercent);

// dpi/96 as a whole percent, rounded to the nearest integer: what "100%",
// "125%" etc. mean on a monitor reporting `dpi`. dpi == 0 is treated as 96
// (undetected, i.e. unscaled) rather than dividing by zero.
int dpiToPercent(unsigned dpi);

// `raw`, if it names "auto" or one of kSteps as decimal text ("125"); "auto"
// otherwise. A hand-edited config, or one written by a build that offered a
// step this one does not, degrades to auto rather than refusing to start or
// silently freezing on a percentage nothing here recognises.
std::string normalizeChoice(const std::string& raw);

// The scale factor `choice` resolves to when the monitor the window is on
// reports `monitorDpi` (96 = no Windows scaling, i.e. 100%). This is the
// entire rule in one pure function:
//   effectiveScale("auto", 96)  == 1.0f   exactly (the S=1 guarantee)
//   effectiveScale("auto", 144) == 1.5f   (a 150%-scaled monitor, followed)
//   effectiveScale("200", 96)   == 2.0f   (the user's own choice, any monitor)
float effectiveScale(const std::string& choice, unsigned monitorDpi);

// --- live state --------------------------------------------------------------
//
// One process-wide value, read every frame by fonts.cpp's scaled accessors
// and by every draw call site's px(). Set from two independent inputs — the
// user's saved/chosen setting and the monitor the window currently reports —
// either of which can move at runtime (a Display combo pick; a drag between
// monitors, via GLFW's content-scale callback in AppWindow::run).

// Records the user's choice (normalised). No-op, and factor() unchanged, if
// `choice` already normalises to what is already in force.
void setChoice(const std::string& choice);

// Records the monitor's current DPI (0 treated as 96). No-op if unchanged.
void setMonitorDpi(unsigned dpi);

// The live scale factor: effectiveScale(choice(), monitorDpi()), recomputed
// the moment either input changes — never stale between the two calls above
// and a caller reading factor().
float factor();

const std::string& choice();
unsigned monitorDpi();

// True exactly once after factor() has changed, then false again until the
// next change — for a caller (gui/app_window.cpp's applyPendingUiScale) that
// must rebuild something expensive (the ImGui style snapshot) only when the
// number actually moved, the same "dirty between frames" shape gui/fonts.hpp
// already uses for the interface language and theme.
bool consumeChanged();

// A hard-coded layout pixel, scaled by the live factor. v * 1.0f is v,
// bit-exact, for every finite v — the S=1 guarantee extends to every call
// site that uses this rather than its own arithmetic.
float px(float v);

}  // namespace cascade::gui::uiscale

#endif  // CASCADE_GUI_UI_SCALE_HPP
