// instrument_tone_alert_math.hpp - the arithmetic behind the tone-alert face.
//
// EVERYTHING HERE IS PURE, and that is the point: no ImGui, no drawing, no
// state. The face in instrument_tone_alert.cpp is a few hundred lines of
// ImDrawList calls that cannot be run in a test, so every decision that could
// be got wrong - where a tone sits on the engraved scale, what a slot the
// plugin left empty prints, how the deck is divided at a window size nobody
// tried - is made here and pinned in tests/test_instrument_tone_alert.cpp.
//
// The slot map this serves is plugin_abi.h's CASCADE_INSTRUMENT_TONE_ALERT:
//   values[0] tone A Hz   values[1] tone B Hz
//   values[2] A seconds   values[3] B seconds
//   text[0] code   text[1] table and group   text[2] MATCHED / UNMATCHED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_TONE_ALERT_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_TONE_ALERT_MATH_HPP

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace cascade::gui::tone_alert {

// --- the engraved scale ------------------------------------------------------
//
// The span cut into the panel between the two tone bars, in Hz. It is not a
// round number chosen to look tidy: two-tone sequential paging tones run from
// 282.2 Hz (the lowest Plectron tone) to 3824 Hz (Reach single-tone channel
// 01) across the published Midian tone-signalling charts, and the great bulk
// of the traffic - every Motorola Quick Call II reed group, every GE Type 99
// tone - sits between about 280 Hz and 2800 Hz, which is also the 300 to
// 3000 Hz band the trade literature quotes for a two-tone A or B element. A
// scale drawn to 3900 would spend a quarter of its length on channels almost
// nothing uses and squash the part that matters; a scale drawn to 1500 would
// put half the tables off the end. 250 to 3000 is the band the equipment
// actually works in, and it divides into eleven even 250 Hz graduations.
//
// A TONE OUTSIDE IT IS NOT HIDDEN. The bar is pinned at the stop and the
// figure printed underneath is the measured one, so the panel says "at least
// this much, and here is the number" rather than quietly drawing a wrong
// height. overRange()/underRange() are what the face uses to letter the stop.
inline constexpr double kScaleLoHz = 250.0;
inline constexpr double kScaleHiHz = 3000.0;

// The engraved numbers on that scale, and the minor ticks between them. Both
// are here rather than in the drawing so a test can prove every one of them
// lands inside the scale it is cut into.
inline constexpr double kMajorTickHz[] = {500.0, 1000.0, 1500.0, 2000.0, 2500.0};
inline constexpr int kMajorTickCount =
    static_cast<int>(sizeof(kMajorTickHz) / sizeof(kMajorTickHz[0]));
inline constexpr double kMinorTickStepHz = 250.0;

// A slot the plugin left empty. RULE 2 OF EVERY FACE: zero is not a tone at
// 0 Hz, it is no tone at all, and the difference is the whole reason this
// predicate exists instead of a bare `!= 0.0` at four call sites. A negative
// or non-finite figure is corrupt and is treated the same way - as nothing to
// draw - rather than being clamped into something plausible.
inline bool haveTone(double hz) { return std::isfinite(hz) && hz > 0.0; }

// THE VALUE OF A SLOT AS THE FACE MUST READ IT. `have` is false until the
// plugin has answered poll_state with 1 at least once, and until then the
// struct's contents mean nothing whatever they happen to hold - so every slot
// on this face goes through here on its way to being drawn.
//
// It exists because leaving that check to each call site got it wrong the
// first time it was drawn: the frequency readouts were gated on `have` and the
// two tone bars were not, so a face with no reading printed "--" under two
// bars indexed at 746.8 and 879.0 Hz. One function, used by the readouts and
// the bars alike, is what makes that combination unrepresentable.
inline double slotOrNone(bool have, double v) { return have ? v : 0.0; }

// Likewise for a duration. A page whose tone lasted no time did not happen.
inline bool haveDuration(double sec) { return std::isfinite(sec) && sec > 0.0; }

// Where a tone sits on the engraved scale: 0 at the bottom stop, 1 at the top.
// Clamped, so a tone off either end of the scale is drawn AT the stop and
// never off the panel. Callers with no tone must not call this - they draw no
// index at all - and it returns 0 for them only so that a mistake is a bar at
// the bottom rather than a NaN fed to a vertex.
inline double toneFrac(double hz) {
    if (!haveTone(hz)) { return 0.0; }
    const double f = (hz - kScaleLoHz) / (kScaleHiHz - kScaleLoHz);
    if (!(f > 0.0)) { return 0.0; }
    if (f > 1.0) { return 1.0; }
    return f;
}

// Whether that clamp actually bit. The face letters the stop when it did, so
// a pinned index is never read as a measurement of 2900 Hz.
inline bool overRange(double hz) { return haveTone(hz) && hz > kScaleHiHz; }
inline bool underRange(double hz) { return haveTone(hz) && hz < kScaleLoHz; }

// --- what the readouts print -------------------------------------------------
//
// One tenth of a Hz, which is what the decoder in front of this face measures
// to: its parabolic peak interpolation is good to a few tenths on a clean
// tone, and printing more figures than that would be inventing precision.
// An empty slot prints the dash, never a zero.
inline void formatHz(char* out, std::size_t cap, double hz) {
    if (out == nullptr || cap == 0u) { return; }
    if (!haveTone(hz)) {
        std::snprintf(out, cap, "--");
        return;
    }
    std::snprintf(out, cap, "%.1f", hz);
}

// Hundredths of a second: published two-tone elements are quoted to 10 ms
// (150 ms, 1 s, 3 s) and the detector's own frames are 8 ms apart, so the
// second decimal is real and a third would not be.
inline void formatSec(char* out, std::size_t cap, double sec) {
    if (out == nullptr || cap == 0u) { return; }
    if (!haveDuration(sec)) {
        std::snprintf(out, cap, "--");
        return;
    }
    std::snprintf(out, cap, "%.2f", sec);
}

// --- the result flag ---------------------------------------------------------
//
// text[2] is the contract's MATCHED / UNMATCHED, and the face colours the two
// differently - phosphor for a page whose tones a published table names, amber
// for one it does not. Amber rather than the alarm rust on purpose: an
// unmatched page is NOT a fault. A fire department's tone pair is a local
// fact, most of them were never in anybody's chart, and a receiver that
// painted every one of them red would be crying wolf about the ordinary case.
//
// ANYTHING ELSE IN THE SLOT IS PRINTED AS IT ARRIVED, in the neutral cream,
// because a plugin that sends a third word is telling the user something and
// the face's job is not to force it into one of two buckets it may not be in.
enum class Result { kNone, kMatched, kUnmatched, kOther };

inline bool equalsNoCase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) { return false; }
    std::size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') { ca = static_cast<char>(ca - 'a' + 'A'); }
        if (cb >= 'a' && cb <= 'z') { cb = static_cast<char>(cb - 'a' + 'A'); }
        if (ca != cb) { return false; }
    }
    return a[i] == '\0' && b[i] == '\0';
}

inline Result resultOf(const char* text) {
    if (text == nullptr || text[0] == '\0') { return Result::kNone; }
    if (equalsNoCase(text, "MATCHED")) { return Result::kMatched; }
    if (equalsNoCase(text, "UNMATCHED")) { return Result::kUnmatched; }
    return Result::kOther;
}

// --- the deck ----------------------------------------------------------------
//
// The face is three bays side by side under the plate: the alerting lamps on
// the left, the two tone bars and their shared scale in the middle, the glass
// plate carrying the code on the right. The proportions are fixed fractions of
// whatever width the window gives, with a floor on each so that a narrow
// window loses width evenly rather than crushing one bay to nothing.

// The smallest deck this face will draw into. Below it the face draws the
// plate and its lamps only, which is still an instrument that says whether
// anything is ringing - and is a great deal better than three unreadable bays.
inline constexpr float kMinDeckW = 300.0f;
inline constexpr float kMinDeckH = 96.0f;

struct Columns {
    bool valid = false;
    float gap = 0.0f;
    float lampW = 0.0f;
    float barsW = 0.0f;
    float glassW = 0.0f;
};

// Splits `w` into the three bays. The two gaps are taken off the top and the
// remainder divided; lamp and bars are held to their fractions and the glass
// takes what is left, so the sum is EXACTLY w whatever the rounding does -
// a face whose bays add up to less than its own panel leaves a seam, and one
// that adds up to more draws over the edge, which rule 3 forbids.
inline Columns columnsFor(float w, float h) {
    Columns c;
    if (!(w >= kMinDeckW) || !(h >= kMinDeckH)) { return c; }
    c.valid = true;
    c.gap = 10.0f;
    const float inner = w - 2.0f * c.gap;
    c.lampW = inner * 0.22f;
    c.barsW = inner * 0.34f;
    c.glassW = inner - c.lampW - c.barsW;
    return c;
}

// The middle bay again: two bars with the engraved scale cut between them.
// The scale keeps a fixed share because its numbers are lettered at a fixed
// size and a proportional share would let them run into the bars.
struct BarColumns {
    bool valid = false;
    float barW = 0.0f;
    float scaleW = 0.0f;
    float gap = 0.0f;
};

inline BarColumns barsFor(float barsW) {
    BarColumns b;
    if (!(barsW >= 96.0f)) { return b; }
    b.valid = true;
    b.gap = 6.0f;
    const float inner = barsW - 2.0f * b.gap;
    b.scaleW = inner * 0.34f;
    if (b.scaleW > 62.0f) { b.scaleW = 62.0f; }
    b.barW = (inner - b.scaleW) * 0.5f;
    return b;
}

// --- type ---------------------------------------------------------------------

// How far the lettering is scaled from the design size. The face was drawn
// against the 600 x 460 window the host opens instruments at, whose face bay
// is about 580 x 270; a larger window gets slightly larger type up to a stop,
// and a smaller one shrinks rather than clips. The floor is where a caption
// stops being readable at all, and below it the face gives up bays instead of
// setting type nobody can read.
inline float typeScale(float w, float h) {
    if (!(w > 0.0f) || !(h > 0.0f)) { return 0.72f; }
    const float byW = w / 580.0f;
    const float byH = h / 270.0f;
    float s = byW < byH ? byW : byH;
    if (s < 0.72f) { s = 0.72f; }
    if (s > 1.30f) { s = 1.30f; }
    return s;
}

// The smallest lettering this face will draw. Anything that will not fit at
// this size is clipped instead of shrunk, because type below it is a smudge
// and a smudge is not a caption.
inline constexpr float kMinTextPx = 9.0f;

// The size a line has to be drawn at to fit `maxW`, given that it measures
// `wAtBase` at `basePx`. Never larger than basePx - a caption is not grown to
// fill its bay - and never smaller than kMinTextPx.
inline float fitPx(float basePx, float wAtBase, float maxW) {
    if (!(basePx > 0.0f)) { return kMinTextPx; }
    if (!(wAtBase > 0.0f) || !(maxW > 0.0f)) { return basePx; }
    if (wAtBase <= maxW) { return basePx; }
    float px = basePx * (maxW / wAtBase);
    if (px < kMinTextPx) { px = kMinTextPx; }
    if (px > basePx) { px = basePx; }
    return px;
}

// --- the lamps ----------------------------------------------------------------

// A LAMP THAT IS MERELY ON IS NOT AN ALERT. The station's alert lamp blinks at
// 2 Hz for as long as the plugin holds the flag, which is what separates it
// from the steady lamps beside it and is the whole reason cue.nowSec is passed
// to a face at all.
inline bool alertBlink(double nowSec) {
    if (!std::isfinite(nowSec)) { return true; }
    return std::fmod(nowSec, 0.5) < 0.28;
}

}  // namespace cascade::gui::tone_alert

#endif  // CASCADE_GUI_INSTRUMENT_TONE_ALERT_MATH_HPP
