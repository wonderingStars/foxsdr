// hover_dwell.hpp - HOW LONG THE POINTER HAS RESTED over a panel, for a tip that appears only
// when someone has stopped there. No ImGui.
//
// WHY IT EXISTS (0.99.76, the owner): "also add a tool tip when the mouse is over it for 4 secs
// saying you can right click to change it". The spectrum and the waterfall already show their
// gestures after ImGui's normal hover delay, and a person who is tuning sees that tip flash by
// and learns to ignore it; the right-click menu (the display range, the trace, the markers) is
// the one thing those tips cannot afford the room to say. This is the clock behind a second
// tip that only a resting hand reaches.
//
// THE RULES (tests/test_hover_dwell.cpp holds each):
//   - the rest begins on the first frame the pointer is over a region, and the tip is due once
//     kDisplayTipDwellS seconds have passed since;
//   - the pointer MOVING more than kDisplayTipSlopPx from where the rest began starts a new rest
//     where it now is (a hand that is tuning is not resting; a hand that trembles is);
//   - leaving the region ends it, and so does anything that "blocked" the frame: a button
//     held (a press, a drag, a pan), a context menu opening or already open;
//   - the tip comes back after another full dwell, never at once;
//   - two regions share one clock but not one rest: the spectrum and the waterfall each ask
//     every frame, and one saying "not over me" must not undo the rest the other is timing;
//   - a region nobody asked about for kDisplayTipMaxGapS (another view had the screen) did not
//     see a rest, and reset() says the same outright;
//   - a clock that is not a number or goes backwards ends the rest rather than trusting it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_HOVER_DWELL_HPP
#define CASCADE_GUI_HOVER_DWELL_HPP

#include <cmath>
#include <cstdint>

namespace cascade::gui {

// How long the pointer must rest before the tip appears (the owner: "4 secs").
inline constexpr double kDisplayTipDwellS = 4.0;
// How far it may wander and still be resting, in pixels. A hand on a mouse is never perfectly
// still; a tuning gesture moves far more than this in the first frame.
inline constexpr float kDisplayTipSlopPx = 6.0f;
// How long the region may go unasked about before the rest it was timing is not trusted: a view
// with no spectrum on it (the scope, the patch page) asks nothing, and the pointer can come back
// to the very pixel it left. Longer than any pause a script holds a frame (5 s), shorter than a
// person's absence.
inline constexpr double kDisplayTipMaxGapS = 10.0;

class HoverDwell {
public:
    // Once per frame per region. `region` names what is asked about (any non-zero id: the
    // receiver's spectrum, its waterfall, a patch part's trace); `over` is whether the pointer
    // is over it and free to be told something; `blocked` is anything that must keep the tip
    // down this frame. Returns true when the tip is due.
    bool update(std::uint64_t region, bool over, bool blocked, float x, float y, double nowS) {
        if (region == 0) { return false; }
        if (!over) {
            // Only the region being timed is ended by a "not over me": the other panel asking in
            // the same frame says nothing about this one's rest.
            if (region == region_) { clear(); }
            return false;
        }
        // Over, but something forbids the tip this frame - or the frame cannot be trusted.
        if (blocked || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(nowS)) {
            clear();
            return false;
        }
        if (!active_ || region != region_ || nowS < lastS_ || nowS - lastS_ > kDisplayTipMaxGapS) {
            begin(region, x, y, nowS);
            return false;
        }
        const float dx = x - anchorX_;
        const float dy = y - anchorY_;
        if (dx * dx + dy * dy > kDisplayTipSlopPx * kDisplayTipSlopPx) {
            begin(region, x, y, nowS);   // a move: a new rest, where the pointer now is
            return false;
        }
        lastS_ = nowS;
        return nowS - sinceS_ >= kDisplayTipDwellS;
    }

    // The regions are not being asked about at all (the scope or a tiny window has the screen):
    // forget the rest, so the tip is not due the instant they come back with the pointer where it
    // was left.
    void reset() { clear(); }

    // Seconds the current rest has lasted at `nowS`, or 0 when nothing is resting.
    double restS(double nowS) const {
        if (!active_ || !std::isfinite(nowS)) { return 0.0; }
        return nowS > sinceS_ ? nowS - sinceS_ : 0.0;
    }

private:
    void begin(std::uint64_t region, float x, float y, double nowS) {
        region_ = region;
        active_ = true;
        anchorX_ = x;
        anchorY_ = y;
        sinceS_ = nowS;
        lastS_ = nowS;
    }
    void clear() {
        region_ = 0;
        active_ = false;
    }

    std::uint64_t region_ = 0;   // the region being timed; 0 = none
    bool active_ = false;
    float anchorX_ = 0.0f;       // where the rest began; the slop is measured from here
    float anchorY_ = 0.0f;
    double sinceS_ = 0.0;        // when it began
    double lastS_ = 0.0;         // the newest frame's clock, to catch one that runs backwards
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_HOVER_DWELL_HPP
