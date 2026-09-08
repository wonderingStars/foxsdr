// instrument_nav_bearing_math.hpp - the arithmetic behind the VOR course
// indicator's face, with no ImGui in it so tests can hold it still.
//
// WHY THIS IS A SEPARATE FILE. Everything below is a claim about a navigation
// instrument that a user may act on: which way the deviation bar leans, which
// of TO and FROM is showing, what "094" under the index means. A drawing can
// be looked at; a claim has to be tested, and it cannot be tested through an
// ImDrawList. scope_view.hpp/scope_face.hpp already split along this line for
// the same reason and it is the split that lets tests/test_scope_view.cpp run
// without a graphics context.
//
// THE ONE THING IN HERE THAT IS EASY TO GET WRONG AND IMPOSSIBLE TO SEE.
// Feeding a course indicator the wrong sign does not produce nonsense, it
// produces a confident, plausible, MIRRORED answer - the same failure mode the
// plugin's own vor_signal.h opens with, one layer up. So solveCourse() is
// derived below from the geometry rather than transcribed from memory, and the
// derivation is written out where the test can be read against it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_NAV_BEARING_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_NAV_BEARING_MATH_HPP

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace cascade::gui::navbearing {

// --- angles ------------------------------------------------------------------

// Into [0, 360). NaN and infinity come back as 0 rather than propagating into
// a card rotation, because a card drawn at a NaN angle is a card that is not
// drawn at all and the plugin's figures are not this face's to trust blindly.
inline double wrap360(double deg) {
    if (!std::isfinite(deg)) { return 0.0; }
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) { d += 360.0; }
    return d;
}

// Into (-180, +180], the form a difference of two bearings has to be in before
// anything compares it with a threshold.
inline double wrap180(double deg) {
    double d = wrap360(deg);
    if (d > 180.0) { d -= 360.0; }
    return d;
}

// The reciprocal: the radial FROM a station and the course that flies INTO it
// are the same line read from the two ends, and printing only one of them
// invites the reader to assume it is the other. The plugin's own text output
// prints both for exactly this reason.
inline int reciprocal(int deg) {
    int d = deg % 360;
    if (d < 0) { d += 360; }
    d += 180;
    if (d >= 360) { d -= 360; }
    return d;
}

// The whole degrees a card index or a readout shows. Rounded to nearest and
// then folded, so 359.7 reads 000 and not 360.
inline int roundedDeg(double deg) {
    const double d = wrap360(deg);
    int v = static_cast<int>(std::lround(d));
    if (v >= 360) { v -= 360; }
    if (v < 0) { v = 0; }
    return v;
}

// --- the course solution -----------------------------------------------------
//
// THE DERIVATION, because the sign is the half of this that cannot be seen to
// be wrong. Put the station at the origin and the receiver at bearing R from
// it (R is the RADIAL - ICAO Annex 10 3.3.1.2, the bearing of the point of
// observation with respect to the VOR). The user selects a course C on the
// omni bearing selector, which names the line through the station along C and
// its reciprocal C+180.
//
// FROM, |wrap180(R - C)| <= 90: the receiver is on the C side of the station,
// so flying C takes it away. Let d = wrap180(R - C). Take C = 0 for the
// picture: the receiver sits at bearing d from the station, so its easting is
// sin(d) - east of the course line for d > 0. Flying north, east is the right
// hand, so the receiver is RIGHT of course and the bar - which is drawn where
// the course is, not where you are - goes LEFT. Deflection = -d.
//
// TO, |wrap180(R - C)| > 90: the receiver is on the far side, so flying C
// takes it towards the station. Let d' = wrap180(R - C - 180). Again with
// C = 0: the receiver is at bearing 180 + d', easting sin(180 + d') = -sin(d'),
// so d' > 0 puts it WEST of the line while it flies north, i.e. left of
// course, and the bar goes RIGHT. Deflection = +d'.
//
// FULL SCALE IS TEN DEGREES, and each of the five dots a side is two - the
// figure the KI 208's own specification gives ("+/-10 degrees off course gives
// full scale deflection", Bendix/King installation manual 006-00140-0004,
// table 1-1). Beyond that the bar sits on its stop, which is what a real one
// does; it does NOT keep travelling and it does not wrap round.
struct CourseSolution {
    bool to = false;          // the selected course leads TO the station
    double deflectionDeg = 0.0;  // signed, + is bar to the right, unclamped
    double barFrac = 0.0;        // -1..+1 of full scale, clamped
    bool offScale = false;       // |deflection| exceeded full scale
};

inline constexpr double kFullScaleDeg = 10.0;
inline constexpr int kDots = 5;  // per side, one every two degrees

inline CourseSolution solveCourse(double radialDeg, double courseDeg) {
    CourseSolution s;
    const double d = wrap180(radialDeg - courseDeg);
    if (std::fabs(d) <= 90.0) {
        s.to = false;
        s.deflectionDeg = -d;
    } else {
        s.to = true;
        s.deflectionDeg = wrap180(radialDeg - courseDeg - 180.0);
    }
    double f = s.deflectionDeg / kFullScaleDeg;
    if (f > 1.0) {
        f = 1.0;
        s.offScale = true;
    } else if (f < -1.0) {
        f = -1.0;
        s.offScale = true;
    }
    s.barFrac = f;
    return s;
}

// --- the compass card --------------------------------------------------------

// The lettering an aviation card carries every thirty degrees: the cardinals as
// letters and everything else as the bearing with its trailing zero dropped, so
// 120 degrees is "12". Two characters at most, which is what makes it fit
// between the graduations. `out` needs 4 bytes.
//
// Returns false and writes nothing for a bearing that is not a multiple of 30 -
// a card is lettered only where it is lettered, and inventing a numeral for
// 47 degrees would put a figure on the dial that no card has.
inline bool cardLabel(int deg, char* out, std::size_t cap) {
    if (out == nullptr || cap < 4u) { return false; }
    out[0] = '\0';
    int d = deg % 360;
    if (d < 0) { d += 360; }
    if (d % 30 != 0) { return false; }
    switch (d) {
        case 0: std::snprintf(out, cap, "N"); return true;
        case 90: std::snprintf(out, cap, "E"); return true;
        case 180: std::snprintf(out, cap, "S"); return true;
        case 270: std::snprintf(out, cap, "W"); return true;
        default: std::snprintf(out, cap, "%d", d / 10); return true;
    }
}

// Where a card bearing lands on screen, in degrees clockwise from the top,
// once the card has been turned to put `underIndex` beneath the top index.
// One function, because the graduations, the numerals and the selected-course
// pointer all sit on the same card and a second copy of this expression is how
// a pointer comes to sit a degree off its own scale.
inline double cardScreenDeg(double cardDeg, double underIndex) {
    return wrap360(cardDeg - underIndex);
}

// --- the readouts ------------------------------------------------------------

// A bearing as a card reads it: three figures, zero padded, no degree sign -
// "094", never "94" and never "94.3". `out` needs 4 bytes.
inline void formatBearing(double deg, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    std::snprintf(out, cap, "%03d", roundedDeg(deg));
}

// --- the ident's Morse -------------------------------------------------------
//
// A VOR sends two or three letters in Morse and nothing else (ICAO Annex 10
// 3.3.6.5), so the alphabet is the twenty-six letters and the ten figures, from
// ITU-R M.1677-1 Annex 1 Part I. This is a SEPARATE transcription from the
// plugin's, deliberately: the face draws the pattern under the letters as a
// check that the ident is what it says it is, and a check that reads its answer
// out of the thing it is checking is not a check.
inline const char* morseFor(char c) {
    char u = c;
    if (u >= 'a' && u <= 'z') { u = static_cast<char>(u - 'a' + 'A'); }
    switch (u) {
        case 'A': return ".-";
        case 'B': return "-...";
        case 'C': return "-.-.";
        case 'D': return "-..";
        case 'E': return ".";
        case 'F': return "..-.";
        case 'G': return "--.";
        case 'H': return "....";
        case 'I': return "..";
        case 'J': return ".---";
        case 'K': return "-.-";
        case 'L': return ".-..";
        case 'M': return "--";
        case 'N': return "-.";
        case 'O': return "---";
        case 'P': return ".--.";
        case 'Q': return "--.-";
        case 'R': return ".-.";
        case 'S': return "...";
        case 'T': return "-";
        case 'U': return "..-";
        case 'V': return "...-";
        case 'W': return ".--";
        case 'X': return "-..-";
        case 'Y': return "-.--";
        case 'Z': return "--..";
        case '0': return "-----";
        case '1': return ".----";
        case '2': return "..---";
        case '3': return "...--";
        case '4': return "....-";
        case '5': return ".....";
        case '6': return "-....";
        case '7': return "--...";
        case '8': return "---..";
        case '9': return "----.";
        default: return nullptr;
    }
}

// The whole ident's pattern, letters separated by a space: "LBA" becomes
// ".-.. -... .-". A character with no Morse (which a well-behaved plugin never
// sends, and a misbehaving one might) is dropped rather than drawn as a gap
// that would read as a letter of silence. Always NUL-terminated; truncated on
// a letter boundary rather than mid-pattern, so what is shown is always a whole
// number of letters and never half of one that would read as a different one.
inline void identMorse(const char* ident, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    out[0] = '\0';
    if (ident == nullptr) { return; }
    std::size_t n = 0;
    for (const char* p = ident; *p != '\0'; ++p) {
        const char* code = morseFor(*p);
        if (code == nullptr) { continue; }
        std::size_t need = 0;
        for (const char* q = code; *q != '\0'; ++q) { ++need; }
        if (n != 0u) { ++need; }  // the space before it
        if (n + need + 1u > cap) { break; }
        if (n != 0u) { out[n++] = ' '; }
        for (const char* q = code; *q != '\0'; ++q) { out[n++] = *q; }
    }
    out[n] = '\0';
}

// --- the layout --------------------------------------------------------------
//
// FIT THE RECTANGLE YOU ARE GIVEN. The window is user-resizable and this face
// has three things competing for it: a round dial that is useless below a
// certain size, a column of gauges, and a strip of glass readouts. Rather than
// let them overlap, the smaller sizes DROP furniture in a fixed order - the
// gauge column first, then the readout strip - and the dial takes what is left.
// Everything downstream is measured from `dialR`, so one number decides the
// whole drawing and a test can pin it.
struct Layout {
    bool drawAnything = false;
    bool drawGauges = false;   // the confidence / reference / variable column
    bool drawReadout = false;  // the radial + inbound + course glass strip
    bool drawIdent = false;    // the ident glass and its Morse
    float dialCx = 0.0f;
    float dialCy = 0.0f;
    float dialR = 0.0f;
    float gaugeX0 = 0.0f;  // left edge of the gauge column
    float gaugeX1 = 0.0f;
    float readoutY0 = 0.0f;  // top of the glass strip under the dial
    float readoutY1 = 0.0f;
};

inline constexpr float kMinDialR = 40.0f;      // below this a card cannot be read
inline constexpr float kGaugeColumnW = 132.0f; // three bays plus their gutters
inline constexpr float kReadoutH = 54.0f;
inline constexpr float kIdentH = 40.0f;

// `w` and `h` are the space BELOW the plate's rule, in pixels. A zero or
// negative rectangle - which a window dragged to nothing produces every time -
// comes back with drawAnything false and every field zero, so a caller that
// honours the flag cannot divide by it.
inline Layout layout(float w, float h) {
    Layout l;
    if (!(w > 0.0f) || !(h > 0.0f)) { return l; }
    if (!std::isfinite(w) || !std::isfinite(h)) { return l; }

    float dialW = w - 8.0f;
    float dialH = h - 8.0f;
    // The gauge column only earns its keep when the dial it stands beside is
    // still worth looking at afterwards.
    if (w - kGaugeColumnW - 16.0f >= kMinDialR * 2.0f && h >= 150.0f) {
        l.drawGauges = true;
        l.gaugeX1 = w - 6.0f;
        l.gaugeX0 = l.gaugeX1 - kGaugeColumnW;
        dialW = l.gaugeX0 - 12.0f;
    }
    if (dialH - kReadoutH >= kMinDialR * 2.0f) {
        l.drawReadout = true;
        dialH -= kReadoutH;
    }
    if (l.drawReadout && dialH - kIdentH >= kMinDialR * 2.0f) {
        l.drawIdent = true;
        dialH -= kIdentH;
    }

    float r = (dialW < dialH ? dialW : dialH) * 0.5f;
    if (r < kMinDialR) { return l; }  // no room for an instrument at all
    l.drawAnything = true;
    l.dialR = r;
    l.dialCx = 4.0f + dialW * 0.5f;
    l.dialCy = 4.0f + dialH * 0.5f;
    l.readoutY0 = 4.0f + dialH;
    l.readoutY1 = l.readoutY0 + kReadoutH;
    return l;
}

}  // namespace cascade::gui::navbearing

#endif  // CASCADE_GUI_INSTRUMENT_NAV_BEARING_MATH_HPP
