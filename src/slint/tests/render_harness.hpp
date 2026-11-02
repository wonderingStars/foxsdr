// The headless render harness for the Slint front end (docs/engine-stage4.md 6.35).
//
// Fidelity is TESTED, not eyeballed: a test builds a component (a specimen sheet, or the main screen),
// renders it with Slint's software renderer into a pixel buffer - no OS window, no GPU, no foreground
// fight with the desktop - and probes the pixels against the artboard's geometry and colours
// (the <helmet><style> tokens and the inline sizes of Workbench.dc.html / Workbench-PluginKit.dc.html).
// Input is dispatched the way the OS backend would deliver it, so a click, a wheel notch or a key
// chord is exercised in the same process. Every frame can be saved as a PNG for a person to look at.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <slint.h>
#include <slint-platform.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace foxsdr::slinttest {

struct Rgb {
    std::uint8_t r = 0, g = 0, b = 0;
};
constexpr Rgb rgb(std::uint32_t hex) {
    return {static_cast<std::uint8_t>(hex >> 16), static_cast<std::uint8_t>(hex >> 8), static_cast<std::uint8_t>(hex)};
}
std::string hex(Rgb c);                                 // "#rrggbb"
bool nearColour(Rgb a, Rgb b, int tol = 8);             // every channel within tol

// Composites `fg` at `alpha` (0..1) over `bg`, the way the renderer blends a translucent fill, so a test can
// state the expected colour of a 13% VFO band or a 62% backdrop without hard-coding the blended value.
Rgb over(Rgb fg, double alpha, Rgb bg);

struct Frame {
    std::uint32_t width = 0, height = 0;
    std::vector<slint::Rgb8Pixel> px;

    Rgb at(int x, int y) const;                          // outside the frame: black
    bool near(int x, int y, Rgb want, int tol = 8) const;
    // Counts the pixels in [x0,x1) x [y0,y1) within tol of `want`.
    int countNear(int x0, int y0, int x1, int y1, Rgb want, int tol = 8) const;
    // Counts the pixels in the rectangle that differ from `background` by more than tol in any channel: "ink".
    // The test for "text was drawn here" when the typeface is a fallback whose glyph shapes are not ours to assert.
    int countInk(int x0, int y0, int x1, int y1, Rgb background, int tol = 24) const;
    bool rowNear(int y, int x0, int x1, Rgb want, int tol = 8) const;   // every pixel of y in [x0,x1)
    bool colNear(int x, int y0, int y1, Rgb want, int tol = 8) const;   // every pixel of x in [y0,y1)
    // The first row at or below y0 (to y1) whose pixel at x is within tol of want; -1 when none.
    int firstRowNear(int x, int y0, int y1, Rgb want, int tol = 8) const;
    int firstColNear(int y, int x0, int x1, Rgb want, int tol = 8) const;
    // Writes an RGB PNG. Returns false (and says so on stderr) when the file could not be written.
    bool savePng(const std::string& path) const;
};

// Installs the headless platform (the software renderer into a buffer). Once per process, BEFORE any
// component is created; a second call is a no-op.
void installHeadlessPlatform();

// Shows the window at the given size (scale factor 1: a logical pixel is a buffer pixel), runs any pending
// timers and animations, and paints. The window stays shown, so the next call sees the state the last one left.
Frame render(slint::Window& window, std::uint32_t width, std::uint32_t height);
template <class T>
Frame render(const slint::ComponentHandle<T>& component, std::uint32_t width, std::uint32_t height) {
    return render(component->window(), width, height);
}

// Lets timers and animations run: real time passes (the runtime's clock is the wall clock), so a test of an
// auto-repeat waits the repeat interval. Bounded by `advance`.
void settle(std::chrono::milliseconds advance = std::chrono::milliseconds(0));

// Input, as the OS backend would deliver it. Positions are logical pixels of the window.
void moveTo(slint::Window& window, float x, float y);
void press(slint::Window& window, float x, float y, slint::PointerEventButton button = slint::PointerEventButton::Left);
void release(slint::Window& window, float x, float y, slint::PointerEventButton button = slint::PointerEventButton::Left);
void click(slint::Window& window, float x, float y, slint::PointerEventButton button = slint::PointerEventButton::Left);
void scroll(slint::Window& window, float x, float y, float deltaX, float deltaY);
// A key press and release; `text` is the key's text or one of slint::platform::key_codes (Control, F2, ...).
void keyTap(slint::Window& window, std::u8string_view text);
void keyDown(slint::Window& window, std::u8string_view text);
void keyUp(slint::Window& window, std::u8string_view text);
// Ctrl+<text>: Control down, the key tapped, Control up.
void ctrlTap(slint::Window& window, std::u8string_view text);

// Where the tests leave their pictures: $FOXSDR_SLINT_SHOTS, else "slint-shots" under the working directory.
// Created on first use. The returned path ends with the separator.
std::string shotDir();

}  // namespace foxsdr::slinttest
