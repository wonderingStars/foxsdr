// instrument_beacon_math.hpp - the arithmetic and the wording behind the
// CASCADE_INSTRUMENT_BEACON face, with no ImGui in it.
//
// WHY THIS IS A SEPARATE HEADER. Everything here is decidable without a
// graphics context - which seven segments a hexadecimal character lights, how
// an age in seconds is lettered, where a needle sits on a centre-zero scale,
// how the face divides the rectangle it is handed - and all of it is exactly
// the sort of thing that is wrong by one for months because nobody can see it
// in a screenshot. scope_view.hpp keeps the radar scope's arithmetic apart from
// its drawing for the same reason and tests 286 checks against it; this is the
// beacon face's half of that split.
//
// THE ONE RULE THAT SHAPES EVERY FORMATTER BELOW. A slot the plugin did not
// fill produces an EMPTY string, never a zero and never a dash pretending to be
// a measurement - "we heard nothing" and "we heard zero" are opposite claims
// and this product has confused them before. Every function that can be handed
// a missing figure takes `have` and answers with nothing when it is false, and
// a non-finite figure is treated as missing rather than printed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INSTRUMENT_BEACON_MATH_HPP
#define CASCADE_GUI_INSTRUMENT_BEACON_MATH_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace cascade::gui::beacon {

// --- the readout -------------------------------------------------------------

// How many characters a COSPAS-SARSAT beacon identity is. C/S T.001 3.2 puts
// the "15 Hex ID" in bits 26-85 of every message, short or long, and it is the
// number a rescue co-ordination centre looks the beacon up by.
inline constexpr int kHexIdChars = 15;

// The seven segments of one cell, as the bits of a mask:
//
//        a            a = 0x01   top
//      f   b          b = 0x02   upper right
//        g            c = 0x04   lower right
//      e   c          d = 0x08   bottom
//        d            e = 0x10   lower left
//                     f = 0x20   upper left
//                     g = 0x40   middle
//
// B AND D ARE LOWER CASE, and that is not a decoration. On seven segments an
// upper-case B is identical to an 8 and an upper-case D is identical to a 0, so
// every real hexadecimal segment display in existence draws them as b and d.
// A beacon identity read off a display that cannot tell B from 8 is a beacon
// identity that gets reported wrong.
//
// Anything that is not a hexadecimal character - including a space, and
// including whatever a plugin puts in the slot when it has no identity - lights
// NO segments, which is the blank cell rule 2 asks for.
inline std::uint8_t segments(char c) {
    switch (c) {
        case '0': return 0x3Fu;
        case '1': return 0x06u;
        case '2': return 0x5Bu;
        case '3': return 0x4Fu;
        case '4': return 0x66u;
        case '5': return 0x6Du;
        case '6': return 0x7Du;
        case '7': return 0x07u;
        case '8': return 0x7Fu;
        case '9': return 0x6Fu;
        case 'A':
        case 'a': return 0x77u;
        case 'B':
        case 'b': return 0x7Cu;  // lower-case b: 8 has the top bar, b does not
        case 'C':
        case 'c': return 0x39u;
        case 'D':
        case 'd': return 0x5Eu;  // lower-case d: 0 has the top bar, d does not
        case 'E':
        case 'e': return 0x79u;
        case 'F':
        case 'f': return 0x71u;
        default: return 0x00u;
    }
}

// Lays the plugin's text[0] into the fifteen cells of the readout, upper-cased,
// and returns how many cells carry a character. `out` must hold 16 bytes and is
// always NUL-terminated; cells past the end of the identity are spaces, which
// light nothing.
//
// IT COPIES RATHER THAN FILTERS. A character the segment table does not know
// occupies its cell and lights nothing, so a plugin that sends something other
// than an identity produces a readout that is visibly wrong rather than a
// plausible identity assembled from the hexadecimal characters in a sentence.
// An over-long string is cut at fifteen for the same reason: the sixteenth
// character of a beacon identity does not exist, so there is nothing honest to
// do with it but drop it.
inline int layHexId(const char* src, char* out) {
    int n = 0;
    if (src != nullptr) {
        for (; n < kHexIdChars && src[n] != '\0'; ++n) {
            char c = src[n];
            if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
            out[n] = c;
        }
    }
    for (int i = n; i < kHexIdChars; ++i) { out[i] = ' '; }
    out[kHexIdChars] = '\0';
    return n;
}

// --- the figures -------------------------------------------------------------

// Full scale of the carrier-error meter, each side of zero. C/S T.001 2.3.1
// requires a beacon's transmitted frequency not to vary more than +2 kHz and
// -5 kHz from its assigned channel over a five year service life, so five
// kilohertz is the outside of what a beacon may legitimately be doing and the
// right place for the end stop. The scale is symmetric although the tolerance
// is not: a meter whose two halves meant different amounts of drift would be
// unreadable, and the figure printed under it carries the exact number.
inline constexpr double kCarrierErrorFullScaleHz = 5000.0;

// Where the needle sits on that scale: 0.5 is dead centre, which is a beacon
// exactly on its assigned frequency. Clamped at both ends, so a wild figure
// parks the needle on a stop instead of drawing it off the face.
inline double errorFraction(double hz) {
    if (!std::isfinite(hz)) { return 0.5; }
    const double f = 0.5 + hz / (2.0 * kCarrierErrorFullScaleHz);
    if (!(f > 0.0)) { return 0.0; }
    if (f > 1.0) { return 1.0; }
    return f;
}

// The carrier error as a line of type. Hertz while it is a hertz-sized number,
// kilohertz beyond ten thousand, and CLAMPED at 999.9 kHz so that a
// double-precision absurdity in the slot cannot produce forty characters of
// figure across the face. Empty when there is no reading.
inline void formatError(double hz, bool have, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    out[0] = '\0';
    if (!have || !std::isfinite(hz)) { return; }
    if (hz > 999900.0) { hz = 999900.0; }
    if (hz < -999900.0) { hz = -999900.0; }
    if (hz > -10000.0 && hz < 10000.0) {
        std::snprintf(out, cap, "%+.0f Hz", hz);
    } else {
        std::snprintf(out, cap, "%+.1f kHz", hz / 1000.0);
    }
}

// The age of the last burst, as a counting readout counts it: seconds while it
// is seconds, then minutes and seconds, then hours. C/S T.001 2.2.1 puts a
// beacon's burst period at a mean of 50 s, so the interesting range is the
// first minute and everything past a few minutes means the beacon has stopped
// or the pass has ended - which is why the format grows rather than saturating.
// Capped at 99:59:59, and empty when there is no reading.
inline void formatAge(double sec, bool have, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    out[0] = '\0';
    if (!have || !std::isfinite(sec)) { return; }
    if (!(sec > 0.0)) { sec = 0.0; }
    if (sec > 359999.0) { sec = 359999.0; }
    const long total = static_cast<long>(sec);
    if (total < 60) {
        std::snprintf(out, cap, "%ld s", total);
    } else if (total < 3600) {
        std::snprintf(out, cap, "%ld:%02ld", total / 60, total % 60);
    } else {
        std::snprintf(out, cap, "%ld:%02ld:%02ld", total / 3600, (total / 60) % 60,
                      total % 60);
    }
}

// --- the words ---------------------------------------------------------------

// The legend engraved across the foot of the panel. It is the whole reason a
// 406 MHz receiver is different from every other decoder on this bench: a burst
// that is not a declared test is a real distress alert with a person on the end
// of it, and the panel says so whether or not anyone is listening.
//
// `which`: 0 the whole legend, 1 its first half, 2 its second. The halves exist
// because on a narrow window the whole legend fits only by shrinking under the
// size an engraving stays readable at, and two lines of readable type beat one
// line of unreadable type. Their concatenation is the whole, which is what the
// test pins - a legend that lost a clause when the window narrowed would be the
// exact failure this face cannot have.
inline const char* warningLine(int which) {
    switch (which) {
        case 1: return "A BURST THAT IS NOT A TEST IS A REAL DISTRESS ALERT";
        case 2: return "REPORT IT TO YOUR RESCUE CO-ORDINATION CENTRE";
        default:
            return "A BURST THAT IS NOT A TEST IS A REAL DISTRESS ALERT - REPORT IT TO "
                   "YOUR RESCUE CO-ORDINATION CENTRE";
    }
}

inline const char* warningJoin() { return " - "; }

// The word the rail's chip carries for a beacon row that has been looked at.
// ALERT while the alert is held, and otherwise how long ago the last burst was
// - which is the one figure that says whether the beacon is still transmitting,
// and the thing a watchkeeper glancing at a rail wants. Falls back to the size
// of the burst log when the plugin fills no age.
inline void chipWord(bool alert, double ageSec, int logRows, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0u) { return; }
    if (alert) {
        std::snprintf(out, cap, "ALERT");
        return;
    }
    if (std::isfinite(ageSec) && ageSec > 0.0) {
        formatAge(ageSec, true, out, cap);
        return;
    }
    std::snprintf(out, cap, "%d LOG", logRows);
}

// --- the layout --------------------------------------------------------------

// WHERE EVERY PART OF THE FACE GOES, decided from the rectangle alone.
//
// The window is user-resizable and the host gives the face between 160 and 360
// points of height when the plugin has a burst log to show beneath it, or the
// whole window when it has not - so this is not one design at one size, it is
// four decks that appear as there is room for them, in the order a beacon
// receiver needs them:
//
//   1  the DISTRESS lamp and the fifteen character identity  - always
//   2  COUNTRY, PROTOCOL and POSITION                        - from ~96 pt
//   3  carrier error, age of the last burst, bursts logged   - from ~160 pt
//   4  the engraved warning legend                           - last
//
// Nothing here reads a font. The decks that letter themselves measure their own
// type at the point of drawing; what this decides is which decks exist and the
// bands they occupy, which is the part that has to be identical every frame and
// provable without a graphics context.
struct Layout {
    bool any = false;      // false when the rectangle cannot hold a face at all
    float x0 = 0.0f;       // the content bounds, inside the panel's margins
    float x1 = 0.0f;

    float readY0 = 0.0f;   // deck 1: lamp, identity, NEW
    float readY1 = 0.0f;
    float lampCx = 0.0f;
    float lampCy = 0.0f;
    float lampR = 0.0f;
    bool newLamp = false;  // room at the right end for the NEW lamp
    float newCx = 0.0f;
    float newCy = 0.0f;
    float newR = 0.0f;
    float wellX0 = 0.0f;   // the identity's glass
    float wellX1 = 0.0f;
    float cellW = 0.0f;    // one segment cell, and the gap between two
    float cellGap = 0.0f;
    float digitH = 0.0f;

    bool plates = false;   // deck 2
    float plateY0 = 0.0f;
    float plateY1 = 0.0f;

    int gauges = 0;        // deck 3: how many of the three bays fit, 0 for none
    float gaugeY0 = 0.0f;
    float gaugeY1 = 0.0f;

    bool legend = false;   // deck 4
    float legY0 = 0.0f;
    float legY1 = 0.0f;

    float used = 0.0f;     // height consumed, measured from the top of the rect
};

inline float clampf(float v, float lo, float hi) {
    if (!(v > lo)) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

// `top` is the y the plate's own title rule left free, `br` the bottom right of
// the whole face. A rectangle too small for even the identity returns
// `any = false` and a used height of zero, which is a face that draws nothing
// rather than a face that draws over its neighbour.
inline Layout layout(float x0, float top, float x1, float bottom) {
    Layout L;
    const float w = x1 - x0 - 20.0f;  // 10 pt of margin each side
    const float h = bottom - top - 8.0f;
    if (!(w > 80.0f) || !(h > 44.0f)) { return L; }
    L.any = true;
    L.x0 = x0 + 10.0f;
    L.x1 = x1 - 10.0f;

    // EVERY DECK GETS ITS MINIMUM BEFORE ANY DECK GETS MORE THAN ONE, and the
    // first attempt at this did the opposite - deck 1 took a third of the
    // window and whatever was left had to cover the other three. At the size
    // the host actually opens these windows at that came to nineteen points
    // short, and the deck it silently dropped was the WARNING LEGEND. A face
    // that decides how large its hero readout is before deciding whether the
    // distress warning is on the panel has its priorities exactly backwards,
    // and it was invisible until the thing was photographed.
    //
    // So: minimums first, in order of what a beacon receiver cannot do
    // without, and only then is the surplus handed out.
    constexpr float kReadMin = 46.0f, kReadMax = 150.0f;
    constexpr float kPlateMin = 44.0f, kPlateMax = 62.0f;
    constexpr float kGaugeMin = 66.0f, kGaugeMax = 108.0f;
    constexpr float kLegMin = 22.0f, kLegMax = 30.0f;

    float readH = (kReadMin < h) ? kReadMin : h;
    float rem = h - readH;
    float plateH = 0.0f;
    float gaugeH = 0.0f;
    float legH = 0.0f;
    if (rem >= kPlateMin) {
        L.plates = true;
        plateH = kPlateMin;
        rem -= plateH;
    }
    // The gauge deck is a moving-coil meter with an engraved caption over it
    // and a figure under it - the tallest thing on the face. Below its minimum
    // there is no face left to draw a needle on, so the deck goes whole rather
    // than becoming three empty boxes.
    if (L.plates && rem >= kGaugeMin) {
        gaugeH = kGaugeMin;
        rem -= gaugeH;
    }
    if (rem >= kLegMin) {
        L.legend = true;
        legH = kLegMin;
        rem -= legH;
    }

    // The surplus, in the order it does the most good: the identity first,
    // because it is what the window is for; then the meters; then the plates;
    // then the legend, which is engraving and does not benefit from being
    // large. Anything still spare is NOT spent - the face ends early and says
    // so through `used`, rather than stretching to fill a window.
    auto give = [&rem](float& deck, float cap) {
        if (!(rem > 0.0f) || !(cap > deck)) { return; }
        const float take = (rem < cap - deck) ? rem : (cap - deck);
        deck += take;
        rem -= take;
    };
    give(readH, kReadMax);
    if (gaugeH > 0.0f) { give(gaugeH, kGaugeMax); }
    if (plateH > 0.0f) { give(plateH, kPlateMax); }
    if (legH > 0.0f) { give(legH, kLegMax); }

    float y = top + 4.0f;
    L.readY0 = y;
    L.readY1 = y + readH;
    y = L.readY1;
    if (L.plates) {
        L.plateY0 = y + 4.0f;
        L.plateY1 = y + plateH;
        y = L.plateY1;
    }
    if (gaugeH > 0.0f) {
        L.gaugeY0 = y + 4.0f;
        L.gaugeY1 = y + gaugeH;
        y = L.gaugeY1;
        // The three bays: the meter, the age and the burst counter. A narrow
        // window keeps them in that order of usefulness rather than squeezing
        // three unreadable ones side by side.
        const float bays = L.x1 - L.x0;
        L.gauges = (bays >= 320.0f) ? 3 : ((bays >= 200.0f) ? 2 : 1);
    }
    if (L.legend) {
        L.legY0 = y + 2.0f;
        L.legY1 = y + legH;
        y = L.legY1;
    }
    L.used = y - top + 4.0f;

    // Deck 1's own furniture. The lamp is as large as the deck allows once its
    // engraved word is taken off, and the identity gets everything else.
    // THE LAMP'S CENTRE IS SET BY ITS BLOOM, not by its lens. A lit lamp is
    // drawn with a halo about a third wider than the glass, so a centre placed
    // one radius in from the margin puts that halo off the left of the panel -
    // which a clip rectangle hides on screen and a vertex buffer does not.
    // AND BOUNDED BY THE WIDTH AS WELL AS THE HEIGHT. On a tall narrow window
    // the deck is deep enough for the largest lens, and a lens sized on that
    // alone eats the column the fifteen cells have to share - which does not
    // fail, it just quietly makes the identity too small to read, on the one
    // window where reading it is the entire point. A third of the width, less
    // the gap, is the lamp's share.
    const float rByHeight = clampf((readH - 22.0f) * 0.40f, 9.0f, 34.0f);
    const float rByWidth = clampf((w * 0.34f - 14.0f) / 2.6f, 9.0f, 34.0f);
    L.lampR = (rByWidth < rByHeight) ? rByWidth : rByHeight;
    L.lampCx = L.x0 + L.lampR * 1.30f + 2.0f;
    L.lampCy = L.readY0 + 6.0f + L.lampR;

    const float afterLamp = L.lampCx + L.lampR * 1.30f + 12.0f;
    L.newR = clampf(L.lampR * 0.26f, 5.0f, 9.0f);
    // The NEW lamp only exists if taking its column off the glass still leaves
    // the identity room to be read; on a narrow window the identity wins.
    const float newCol = L.newR * 2.0f + 20.0f;
    L.wellX0 = afterLamp;
    L.wellX1 = L.x1;
    if (L.x1 - newCol - afterLamp > 150.0f) {
        L.newLamp = true;
        L.wellX1 = L.x1 - newCol;
        L.newCx = L.x1 - L.newR - 4.0f;
        L.newCy = L.readY0 + 8.0f + L.newR;
    }

    // Fifteen cells, a gap between each, inside the glass.
    const float inner = (L.wellX1 - L.wellX0) - 16.0f;
    const float gap = clampf(inner * 0.012f, 1.0f, 4.0f);
    const float cell = (inner - gap * static_cast<float>(kHexIdChars - 1)) /
                       static_cast<float>(kHexIdChars);
    L.cellGap = gap;
    // NO FLOOR WORTH THE NAME. Fifteen cells and fourteen gaps are exactly the
    // glass by construction, and a minimum cell width breaks that identity the
    // moment the window is narrow enough to reach it - the row then runs off
    // the end of its own well rather than becoming small. Small is the honest
    // answer; the lamp above gives up its width first.
    L.cellW = (cell > 0.5f) ? cell : 0.5f;
    // A segment digit is about seven parts tall to four wide; whichever of the
    // cell width and the glass height binds first is the one that decides.
    const float glassH = (L.readY1 - L.readY0) - 26.0f;
    const float byWidth = L.cellW * 1.75f;
    L.digitH = (byWidth < glassH) ? byWidth : glassH;
    if (!(L.digitH > 6.0f)) { L.digitH = 6.0f; }
    return L;
}

}  // namespace cascade::gui::beacon

#endif  // CASCADE_GUI_INSTRUMENT_BEACON_MATH_HPP
