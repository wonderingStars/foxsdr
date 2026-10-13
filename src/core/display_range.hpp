// display_range.hpp - THE dB WINDOW THE SPECTRUM AND THE WATERFALL ARE DRAWN THROUGH, and the
// choice between the program's own window and the user's. No ImGui, no config: the window
// pushes a floor and a ceiling in, the config loader asks the same questions, and a test
// pushes whatever it likes.
//
// WHY IT EXISTS (0.99.76, a dislike from a French user on 0.99.75, RTL-SDR V4): "the spectrum
// and the waterfall are very milky, one cannot see the signals ... an effort on this problem
// would be welcome". The owner's answer: "allow the users to set it themselves to what they
// prefer by right clicking on it". The window was always adjustable - Display > Min dB and
// Max dB - but the Display section is folded until it is opened (0.99.74), so nobody who was
// not looking for it found it. The right-click menu puts the same two numbers where the
// picture is.
//
// WHAT "AUTOMATIC" MEANS, because the two views that draw this window meant different things
// by it before there was a word for it:
//   - the RECEIVER's spectrum and waterfall have always used ONE fixed window, -110 to 0 dB,
//     unless the sliders moved it. Automatic is that window.
//   - a patch Spectrum part fits its own window to every line it draws (the 20th-percentile
//     bin to the loudest, never under 30 dB: gui/patch_scope_math.hpp). Automatic leaves that
//     as it was.
// MANUAL is the user's floor and ceiling, on all of them, so the picture and the strength key
// cannot disagree about what a colour means.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DISPLAY_RANGE_HPP
#define CASCADE_CORE_DISPLAY_RANGE_HPP

#include <cmath>
#include <string_view>

namespace cascade::core {

// The receiver's own window. core/config.hpp's dbMin/dbMax default to the same two numbers
// (tests/test_display_range.cpp holds the two to each other: core/config.hpp does not include
// this file, and a default that drifted would make "Automatic" mean something else on a fresh
// install than on a reset).
inline constexpr float kDefaultFloorDb = -110.0f;
inline constexpr float kDefaultCeilingDb = 0.0f;

// What the two sliders - the menu's and Display's - may reach. They were literals at the
// Display section's two SliderFloat calls; one table now, so the menu cannot offer a floor the
// Display slider would clamp.
inline constexpr float kFloorMinDb = -160.0f;
inline constexpr float kFloorMaxDb = -20.0f;
inline constexpr float kCeilingMinDb = -100.0f;
inline constexpr float kCeilingMaxDb = 20.0f;

// The ceiling stays at least this far above the floor: a thinner span renders as a solid
// waterfall and a wall-to-wall trace, and a zero or inverted one is a divide by zero where dB
// is mapped to pixels. AT LEAST: a span of exactly this is legal, and the config loader keeps it.
inline constexpr float kMinRangeSpanDb = 10.0f;

struct DbPair {
    float floorDb = kDefaultFloorDb;
    float ceilingDb = kDefaultCeilingDb;
    bool operator==(const DbPair& o) const { return floorDb == o.floorDb && ceilingDb == o.ceilingDb; }
};

enum class RangeMode { Auto, Manual };

// The config file's spelling of a mode.
inline const char* rangeModeName(RangeMode m) { return m == RangeMode::Manual ? "manual" : "auto"; }

// Whether a ceiling is far enough above a floor. A thousandth of a decibel short counts as far
// enough: a floor pushed to "ceiling - 10" (or a ceiling to "floor + 10") and then subtracted back
// is not always the same float, and a pair that was legal when it was written must still be legal
// when it is read. The loader asks this too, so the writer and the reader cannot disagree.
inline bool spanIsEnough(float floorDb, float ceilingDb) {
    return ceilingDb - floorDb >= kMinRangeSpanDb - 1e-3f;
}

// Whether a pair is a window the display can be drawn through: two numbers, the ceiling far
// enough above the floor. Says nothing about the sliders' bounds - a file written before the menu
// existed may hold a floor the slider would not reach, and that range is still theirs.
inline bool isUsablePair(const DbPair& p) {
    return std::isfinite(p.floorDb) && std::isfinite(p.ceilingDb) &&
           spanIsEnough(p.floorDb, p.ceilingDb);
}

// The window a pair of sliders may be left at, after one of them moved.
//
// Each end is first held to its own slider's bounds. Then, when the ceiling is not
// kMinRangeSpanDb above the floor, THE END THE HAND MOVED GIVES WAY (floorMoved says which): the
// other is where the user left it, and correcting it would make an untouched slider jump under
// their eyes - the rule the Display section's two sliders have always followed, which this is
// now the single copy of. Pushing the moved end can never leave its own bounds: the bounds
// overlap only where the span rule is what is being settled.
//
// A floor or ceiling that is not a number (a web request, a corrupt file) is the standard window.
inline DbPair clampManualRange(float floorDb, float ceilingDb, bool floorMoved) {
    if (!std::isfinite(floorDb) || !std::isfinite(ceilingDb)) { return DbPair{}; }
    DbPair p;
    p.floorDb = std::fmin(std::fmax(floorDb, kFloorMinDb), kFloorMaxDb);
    p.ceilingDb = std::fmin(std::fmax(ceilingDb, kCeilingMinDb), kCeilingMaxDb);
    if (!spanIsEnough(p.floorDb, p.ceilingDb)) {
        if (floorMoved) {
            p.floorDb = p.ceilingDb - kMinRangeSpanDb;
        } else {
            p.ceilingDb = p.floorDb + kMinRangeSpanDb;
        }
    }
    return p;
}

// The window in force for a mode: the program's own for Automatic, the user's pair for Manual.
inline DbPair rangeInForce(RangeMode mode, const DbPair& manual) {
    return mode == RangeMode::Manual ? manual : DbPair{};
}

// The mode a saved file means. The word wins, both ways. A file without one - written before
// there was a mode, when the pair WAS the window - is Manual if the pair is anything but the
// program's own, so a range someone set with the Display sliders is still theirs; an unknown word
// or a value that was not a word at all is read the same way.
inline RangeMode resolveRangeMode(std::string_view saved, const DbPair& pair) {
    if (saved == "auto") { return RangeMode::Auto; }
    if (saved == "manual") { return RangeMode::Manual; }
    return pair == DbPair{} ? RangeMode::Auto : RangeMode::Manual;
}

// The whole state the menu works on: which window is in force, and the pair Manual uses or
// returns to. Header-only and free of the GUI so that "moving a slider is a choice", "Automatic
// keeps the pair" and "Reset forgets it" are each a line a test can read.
class DisplayRange {
public:
    RangeMode mode() const { return mode_; }
    bool manual() const { return mode_ == RangeMode::Manual; }

    // What the spectrum, the waterfall and the strength key are drawn through right now.
    DbPair inForce() const { return rangeInForce(mode_, manual_); }

    // What Manual uses, and what is saved as dbMin / dbMax: kept while Automatic is in force, so
    // choosing Automatic and then Manual again returns to the numbers the user had.
    DbPair manualPair() const { return manual_; }

    // The radio keys. The pair is not touched.
    void setMode(RangeMode m) { mode_ = m; }

    // A slider moved. Dragging one is choosing a range, so this is Manual from then on.
    void setManual(float floorDb, float ceilingDb, bool floorMoved) {
        manual_ = clampManualRange(floorDb, ceilingDb, floorMoved);
        mode_ = RangeMode::Manual;
    }

    // Reset: Automatic, and the remembered pair is forgotten too, so the next Manual starts from
    // the program's own window rather than from a range the user threw away.
    void reset() {
        mode_ = RangeMode::Auto;
        manual_ = DbPair{};
    }

    // From the config. The loader has already refused a pair it cannot use; this does not rely on it.
    void restore(RangeMode m, const DbPair& pair) {
        mode_ = m;
        manual_ = isUsablePair(pair) ? pair : DbPair{};
    }

private:
    RangeMode mode_ = RangeMode::Auto;
    DbPair manual_{};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_DISPLAY_RANGE_HPP
