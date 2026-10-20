// tune_local.hpp - THE FIGURE A DRAG HAS SENT, kept until the engine's readback shows it (the pump move's step P4; the survey's
// section 1.6 A).
//
// The VFO drag (the passband's centre, its two edges) sends a command and draws the band it will have made at once: the passband the
// spectrum panel measures PEAK IN PASSBAND over and paints must be the one the drag just set, and a command lands at the engine's
// next pass - and is read back a publish after that. So the window keeps the figure it sent (TuneLocalFigure) and shows it in place
// of the readback for as long as the readback has not caught up.
//
// WHEN IT GOES. It used to go when the NEXT state was read ("one pass, by which time the state carries the command"), which is right
// only while the frame IS the pass. With the pass on a thread of its own a read can precede the pass that lands the drag - a pass
// stalled in a rescan, a frame that simply came first - and the passband would snap back for a frame and the next drag step start
// from the old figure. It now follows the rule the radar scope's gain knob has (gui/scope_gain.hpp, ScopeGainLocal), for the same
// reason: the figure stands until
//   - the readback has come to EQUAL it (the engine took exactly that: the readback is the figure from here), or
//   - a moment has gone by (kTuneLocalHoldS: the engine took something else - a limit the window does not know, a refusal - and the
//     readback is the truth).
// A figure that was never sent never stands.
//
// No ImGui, no engine: tests/test_tune_local.cpp holds every rule by itself, and the window's own tests drive it through a frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_TUNE_LOCAL_HPP
#define CASCADE_GUI_TUNE_LOCAL_HPP

namespace cascade::gui {

// How long a sent figure is shown when the readback does not come to equal it.
inline constexpr double kTuneLocalHoldS = 0.5;

// A figure the window sent and the engine has not yet shown.
struct TuneLocalFigure {
    bool valid = false;
    double hz = 0.0;       // the figure sent, as the engine's own limits will leave it
    double sentAtS = 0.0;  // when (the window's clock)
};

// Does the kept figure still stand? Not when none was sent, when the readback has come to equal it, or when the hold has run out.
inline bool tuneLocalStands(const TuneLocalFigure& l, double readbackHz, double nowS) {
    return l.valid && readbackHz != l.hz && (nowS - l.sentAtS) < kTuneLocalHoldS;
}

// The figure the panel draws and the next drag step starts from: the kept one while it stands, else the readback.
inline double tuneLocalShown(const TuneLocalFigure& l, double readbackHz, double nowS) {
    return tuneLocalStands(l, readbackHz, nowS) ? l.hz : readbackHz;
}

// Keeps `hz`, sent at `nowS`.
inline void tuneLocalSent(TuneLocalFigure& l, double hz, double nowS) {
    l.valid = true;
    l.hz = hz;
    l.sentAtS = nowS;
}

// Drops the kept figure when it no longer stands (the readback has caught up or the hold is over). Returns whether it was dropped.
inline bool tuneLocalRelease(TuneLocalFigure& l, double readbackHz, double nowS) {
    if (!l.valid || tuneLocalStands(l, readbackHz, nowS)) { return false; }
    l.valid = false;
    return true;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TUNE_LOCAL_HPP
