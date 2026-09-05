// rail_banks.hpp - the FUNCTION SELECT rail as five banks, and the motion
// between them, as arithmetic the tests can reach.
//
// WHY THE RAIL HAS BANKS. Until 0.77.0 the left column was one scrolling list
// of nineteen sections under five engraved captions, four of them open by
// default and the rest closed because - the code's own words - opening more
// "would push the Display controls off a 720p column". Reaching the scanner
// meant scrolling past the decoders; opening the recorder shoved the band-plan
// controls out of sight; and every section's open state fought every other's
// for the same 700 pixels. A 1960s bench does not do that: its function
// selector is a row of pushbuttons, one lit, and the panel under it shows that
// function's controls and nothing else. That is what this is - the five
// captions become five keys, the rail shows one bank at a time, and the
// sections inside a bank keep their own open/closed state exactly as before.
//
// WHY THERE IS MOTION. A drawer that pops from closed to open in one frame,
// and a bank that swaps its whole contents in one frame, both read as the
// screen glitching rather than as a control being operated. The two motions
// below are short - a fifth of a second - and eased, so a section unfolds and
// a bank fades in the way a real panel's lamp comes up. Long enough to be seen
// as movement, too short to ever be waited for.
//
// PURE ARITHMETIC, NO ImGui TYPES. The window owns the widgets and the clock;
// this header owns what fraction of a drawer is showing after `dt` more
// seconds, which key the keyboard just asked for, and what a bank is called -
// so tests/test_rail_banks.cpp can pin all three without a GL context.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_RAIL_BANKS_HPP
#define CASCADE_GUI_RAIL_BANKS_HPP

namespace cascade::gui {

// The five banks, in the order the rail's captions always had them: what the
// samples pass through, what is made of them, how it is shown, the ways in
// from outside, and the application talking about itself.
enum class RailBank : int { SignalPath = 0, Decode = 1, View = 2, Extend = 3, System = 4 };
inline constexpr int kRailBankCount = 5;

// The word on the key. Short, because five keys share the rail's width and a
// key that has clipped its own label looks like a design decision rather than
// a fault (see test_app_rail.cpp for the same argument about the rows).
inline const char* railBankLabel(RailBank b) {
    switch (b) {
        case RailBank::SignalPath: return "SIGNAL";
        case RailBank::Decode: return "DECODE";
        case RailBank::View: return "VIEW";
        case RailBank::Extend: return "EXTEND";
        case RailBank::System: return "SYSTEM";
    }
    return "";
}

// The engraved caption over the bank's sections - the long form the rail
// always carried, kept so the group the user is looking at is named in full.
inline const char* railBankCaption(RailBank b) {
    switch (b) {
        case RailBank::SignalPath: return "SIGNAL PATH";
        case RailBank::Decode: return "DECODE";
        case RailBank::View: return "VIEW";
        case RailBank::Extend: return "EXTEND";
        case RailBank::System: return "SYSTEM";
    }
    return "";
}

// A saved bank index, clamped. The config carries an int and an old or edited
// file can say anything; whatever it says, the rail opens on a bank that
// exists.
inline RailBank railBankFromIndex(int index) {
    if (index < 0) { return RailBank::SignalPath; }
    if (index >= kRailBankCount) { return RailBank::System; }
    return static_cast<RailBank>(index);
}

// --- motion ------------------------------------------------------------------

// How long a drawer takes to unfold or fold, and a bank to come up.
inline constexpr float kRailDrawerSeconds = 0.18f;
inline constexpr float kRailBankFadeSeconds = 0.14f;

// Smoothstep: no motion at either end, fastest in the middle, which is how
// anything on a spring or a damper moves. Clamped, so a fraction that has
// overshot on a long frame still lands exactly on 0 or 1.
inline float railEase(float t) {
    if (!(t > 0.0f)) { return 0.0f; }
    if (t >= 1.0f) { return 1.0f; }
    return t * t * (3.0f - 2.0f * t);
}

// The progress of a drawer after `dt` more seconds, moving from `progress`
// (0 = folded, 1 = unfolded) towards its target at a constant rate over
// `seconds`. Linear in progress; the HEIGHT a caller draws is
// natural * railEase(progress), which is where the easing lives. A dt of zero
// or less changes nothing; a dt longer than the whole motion lands exactly on
// the target, so a stalled frame cannot leave a drawer part-open.
inline float railDrawerAdvance(float progress, bool open, float dt,
                               float seconds = kRailDrawerSeconds) {
    float p = progress;
    if (!(p > 0.0f)) { p = 0.0f; }
    if (p > 1.0f) { p = 1.0f; }
    if (!(dt > 0.0f)) { return p; }
    const float step = (seconds > 0.0f) ? dt / seconds : 1.0f;
    if (open) {
        p += step;
        return p >= 1.0f ? 1.0f : p;
    }
    p -= step;
    return p <= 0.0f ? 0.0f : p;
}

// Whether a drawer at `progress` still needs drawing at all: anything above
// zero is on screen, including a section folding shut, which is drawn until
// the last pixel of it has gone.
inline bool railDrawerVisible(float progress) { return progress > 0.0f; }

// The keyboard's way to the banks: F1..F5, one per key, left to right. The
// argument is the index of the function key pressed this frame (0 for F1, -1
// for none); the answer is the bank it selects, or -1.
inline int railBankForFunctionKey(int fkeyIndex) {
    if (fkeyIndex < 0 || fkeyIndex >= kRailBankCount) { return -1; }
    return fkeyIndex;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_RAIL_BANKS_HPP
