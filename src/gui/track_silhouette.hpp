// track_silhouette.hpp - the aircraft symbols the map and the radar scope draw,
// and the rule that picks one from a track's broadcast category.
//
// WHY THIS IS A HEADER AND NOT TWO COPIES. The outline and the code that draws
// it used to live in map_view.cpp and again in scope_view.cpp, the second
// carrying a comment saying it was "lifted from the map so the two pictures
// draw the same aircraft". That comment was the whole problem: it was a promise
// maintained by hand, and it was about to be asked to hold across nine shapes
// instead of one. Now there is one table and one drawing routine, and the map
// and the scope cannot disagree because there is nothing left to disagree with.
//
// SHAPE IS THE CHANNEL, NOT COLOUR. On the map colour already carries altitude
// and age, and on the scope it carries the sweep. The only unused visual
// channel at a nine-pixel marker is the outline, which is also the one the eye
// reads a silhouette with - an aeroplane is recognised by its plan form long
// before its colour means anything.
//
// AND NOT SIZE EITHER. The obvious way to show a jumbo is to draw it bigger.
// It does not work: on a map, a bigger symbol reads as a NEARER one, and two
// aircraft at the same altitude drawn at different sizes look like a depth cue
// that is not there. Heavy differs from medium by SPAN and TAILPLANE while
// keeping the same overall length, so the shape changes and the footprint does
// not.
//
// The tables are half outlines: the right-hand side only, nose up, in a unit
// box with y negative toward the nose. The left half is the mirror walked
// backwards, which makes the two sides identical by construction rather than
// by proof-reading, and skips both centreline points so no vertex repeats.
// Shapes are concave, hence AddConcavePolyFilled.
#ifndef CASCADE_GUI_TRACK_SILHOUETTE_HPP
#define CASCADE_GUI_TRACK_SILHOUETTE_HPP

#include <cmath>
#include <cstdint>

#include "core/plugin_abi.h"
#include "gui/theme.hpp"
#include "imgui.h"

namespace cascade::gui {

// The generic aeroplane, and what a medium airliner is. This is the shape the
// application drew for every aircraft before categories existed, unchanged - so
// an aircraft that states no category looks exactly as it always has, and the
// new shapes are additions rather than a re-style of the whole map.
inline constexpr float kSilAirliner[][2] = {
    {0.00f, -1.00f},  // nose
    {0.13f, -0.70f},  // cockpit taper
    {0.13f, -0.26f},  // wing root, leading edge
    {0.98f, 0.16f},   // wing tip, leading edge
    {0.98f, 0.38f},   // wing tip, trailing edge
    {0.13f, 0.20f},   // wing root, trailing edge
    {0.13f, 0.62f},   // fuselage ahead of the tail
    {0.48f, 0.88f},   // tailplane tip, leading edge
    {0.48f, 1.02f},   // tailplane tip, trailing edge
    {0.08f, 0.94f},   // tail root
    {0.00f, 0.96f},   // tail, on the centreline
};

// Heavy. The same aeroplane with more span and a bigger tailplane, at the same
// length - see the note above about why it is not simply larger.
inline constexpr float kSilHeavy[][2] = {
    {0.00f, -1.00f}, {0.15f, -0.72f}, {0.15f, -0.26f}, {1.22f, 0.14f},
    {1.22f, 0.40f},  {0.15f, 0.20f},  {0.15f, 0.60f},  {0.62f, 0.86f},
    {0.62f, 1.02f},  {0.09f, 0.94f},  {0.00f, 0.96f},
};

// Light. Short, straight, untapered wing and a small tail. The wing does NOT
// sweep, and that is the cue that survives at nine pixels when the difference
// in span does not.
inline constexpr float kSilLight[][2] = {
    {0.00f, -1.00f}, {0.15f, -0.74f}, {0.15f, -0.34f}, {0.74f, -0.24f},
    {0.74f, 0.02f},  {0.15f, 0.10f},  {0.15f, 0.66f},  {0.40f, 0.84f},
    {0.40f, 0.98f},  {0.09f, 0.92f},  {0.00f, 0.94f},
};

// Glider. Very long, very thin, straight wings on a slender fuselage: the
// span-to-chord ratio is unmistakable even when the whole symbol is tiny, and
// nothing else in the family looks remotely like it.
inline constexpr float kSilGlider[][2] = {
    {0.00f, -1.00f}, {0.09f, -0.76f}, {0.09f, -0.30f}, {1.45f, -0.10f},
    {1.45f, 0.02f},  {0.09f, 0.12f},  {0.09f, 0.70f},  {0.34f, 0.86f},
    {0.34f, 0.98f},  {0.06f, 0.92f},  {0.00f, 0.94f},
};

// Ultralight, hang-glider, paraglider - the microlight. A swept delta with NO
// tailplane, and the missing tail is the cue: every other flying shape here
// has one.
inline constexpr float kSilMicrolight[][2] = {
    {0.00f, -1.00f}, {0.10f, -0.60f}, {0.86f, 0.62f},
    {0.86f, 0.84f},  {0.10f, 0.52f},  {0.10f, 0.88f},
    {0.00f, 0.90f},
};

// Unmanned. Slender straight wing, no tailplane, short body - a small shape
// that is plainly not a light aeroplane and plainly not a microlight.
inline constexpr float kSilUav[][2] = {
    {0.00f, -0.90f}, {0.09f, -0.62f}, {0.09f, -0.28f}, {0.92f, -0.14f},
    {0.92f, -0.01f}, {0.09f, 0.10f},  {0.09f, 0.72f},  {0.00f, 0.74f},
};

// Lighter-than-air, and a parachutist under canopy. A plain rounded body with
// no wings at all. The two share a symbol deliberately: both are round, slow
// and rare, and inventing a distinct glyph for a skydiver that nobody would
// recognise is worse than one that reads as "not an aeroplane".
inline constexpr float kSilBalloon[][2] = {
    {0.00f, -0.92f}, {0.30f, -0.66f}, {0.44f, -0.16f}, {0.40f, 0.36f},
    {0.24f, 0.74f},  {0.10f, 0.92f},  {0.00f, 0.96f},
};

// On the ground and not flying at all: an airport fire engine, a tug, a
// surveyed obstruction. A blunt lozenge, because the one thing that must be
// obvious is that it is not an aircraft.
inline constexpr float kSilSurface[][2] = {
    {0.00f, -0.62f}, {0.46f, -0.44f}, {0.46f, 0.44f}, {0.00f, 0.62f},
};

struct Silhouette {
    const float (*half)[2];
    int count;
    // Rotorcraft are not drawn from a table at all - see addTrackSymbol. A
    // rotor disc is a different CLASS of object from a winged outline and
    // reads as one instantly, which no arrangement of wings achieves.
    bool rotor;
};

// The widest half-table is the glider's eleven points, which mirrors to twenty
// vertices. Sized with headroom and asserted, so a future shape that overruns
// it is a build failure rather than a stack write.
inline constexpr int kSilhouetteMaxVertices = 24;

inline constexpr int silCount(const float (*t)[2], int bytes) {
    return static_cast<int>(bytes / static_cast<int>(sizeof(t[0])));
}

#define CASCADE_SIL(tbl) \
    Silhouette { (tbl), static_cast<int>(sizeof(tbl) / sizeof((tbl)[0])), false }

// The whole mapping, in one place, total over the ABI's vocabulary.
//
// Several categories deliberately SHARE a shape. Medium 1, Medium 2 and high
// vortex are all airliners and differ by tonnage in a way no nine-pixel symbol
// can honestly show; high performance is rare enough that giving it a glyph
// nobody would recognise buys nothing. Sharing is not a loss of information -
// the category survives in the ABI and in the detail panel - it is a refusal
// to draw a distinction the eye cannot read.
//
// AN UNKNOWN VALUE FALLS THROUGH TO THE GENERIC AEROPLANE, which is what makes
// this safe against a newer plugin: a category this build has never heard of
// draws as it always did rather than vanishing or asserting.
inline Silhouette silhouetteForCategory(std::uint32_t category) {
    switch (category) {
        case CASCADE_AIRCRAFT_LIGHT: return CASCADE_SIL(kSilLight);
        case CASCADE_AIRCRAFT_HEAVY: return CASCADE_SIL(kSilHeavy);
        case CASCADE_AIRCRAFT_GLIDER: return CASCADE_SIL(kSilGlider);
        case CASCADE_AIRCRAFT_ULTRALIGHT: return CASCADE_SIL(kSilMicrolight);
        case CASCADE_AIRCRAFT_UAV: return CASCADE_SIL(kSilUav);
        case CASCADE_AIRCRAFT_LIGHTER_THAN_AIR:
        case CASCADE_AIRCRAFT_PARACHUTIST: return CASCADE_SIL(kSilBalloon);
        case CASCADE_AIRCRAFT_SURFACE_EMERGENCY:
        case CASCADE_AIRCRAFT_SURFACE_SERVICE:
        case CASCADE_AIRCRAFT_GROUND_OBSTRUCTION: return CASCADE_SIL(kSilSurface);
        case CASCADE_AIRCRAFT_ROTORCRAFT:
            return Silhouette{kSilAirliner, 0, true};
        case CASCADE_AIRCRAFT_MEDIUM1:
        case CASCADE_AIRCRAFT_MEDIUM2:
        case CASCADE_AIRCRAFT_HIGH_VORTEX:
        case CASCADE_AIRCRAFT_HIGH_PERF:
        case CASCADE_AIRCRAFT_SPACE:
        case CASCADE_AIRCRAFT_NONE:
        default: return CASCADE_SIL(kSilAirliner);
    }
}

// The category carried by a track, or NONE for anything that is not an
// aircraft. Kinds other than aircraft must never be given a wing shape, and
// the ABI leaves those bits undefined for them.
inline std::uint32_t trackCategory(std::uint32_t kind, std::uint32_t flags) {
    if (kind != CASCADE_TRACK_AIRCRAFT) { return CASCADE_AIRCRAFT_NONE; }
    return CASCADE_TRACK_CATEGORY(flags);
}

// ---------------------------------------------------------------------------
// Drawing.
//
// One routine for the map and the scope. `filled` false draws the outline
// only, which is how an aircraft with NO REPORTED ALTITUDE is told apart from
// one at sea level: those are different facts, and a hue comparison at nine
// pixels is not a reliable way to separate them where a hollow shape against a
// solid one is.
// ---------------------------------------------------------------------------

inline constexpr double kSilPi = 3.14159265358979323846;

// The rotor disc. Not a silhouette at all, and that is the point: a helicopter
// is a different class of thing from a winged aircraft and the symbol says so
// before the eye has resolved any detail. The blades carry the heading, so the
// disc is not merely a circle with no orientation.
inline void addRotorDisc(ImDrawList* dl, const ImVec2& c, double courseDeg, float scale,
                         ImU32 col, bool filled) {
    // The rim is a shadow: black on today's bench, the preset's shadow ink.
    const ImU32 rim = theme::shadow(static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFFu));
    const double a = (std::isnan(courseDeg) ? 0.0 : courseDeg) * kSilPi / 180.0;
    dl->AddCircle(c, scale * 0.95f, col, 0, scale * 0.16f);
    // Two blades, offset from the heading so neither lies along it: a blade
    // drawn straight up would be read as a nose.
    for (int k = 0; k < 2; ++k) {
        const double ang = a + (k == 0 ? 0.6108652 : 2.1816616); /* 35 and 125 deg */
        const float dx = static_cast<float>(std::sin(ang)) * scale * 0.95f;
        const float dy = static_cast<float>(-std::cos(ang)) * scale * 0.95f;
        const ImVec2 p0(c.x - dx, c.y - dy);
        const ImVec2 p1(c.x + dx, c.y + dy);
        dl->AddLine(p0, p1, rim, scale * 0.30f);
        dl->AddLine(p0, p1, col, scale * 0.16f);
    }
    if (filled) { dl->AddCircleFilled(c, scale * 0.24f, col); }
}

// The symbol for one track. `category` is a CASCADE_AIRCRAFT_* value; anything
// unrecognised draws the generic aeroplane.
inline void addTrackSymbol(ImDrawList* dl, const ImVec2& c, double courseDeg, float scale,
                           ImU32 col, bool filled, std::uint32_t category) {
    const Silhouette sil = silhouetteForCategory(category);
    if (sil.rotor) {
        addRotorDisc(dl, c, courseDeg, scale, col, filled);
        return;
    }
    // Course 0 is north, which on screen is straight up; an unknown course
    // (NaN by ABI contract) draws the shape pointing north rather than
    // inventing a heading line the way the tick for other kinds would.
    const double a = (std::isnan(courseDeg) ? 0.0 : courseDeg) * kSilPi / 180.0;
    const float ca = static_cast<float>(std::cos(a));
    const float sa = static_cast<float>(std::sin(a));
    ImVec2 pts[kSilhouetteMaxVertices];
    int n = 0;
    const auto put = [&](float x, float y) {
        if (n >= kSilhouetteMaxVertices) { return; }
        pts[n++] = ImVec2(c.x + (x * ca - y * sa) * scale, c.y + (x * sa + y * ca) * scale);
    };
    for (int i = 0; i < sil.count; ++i) { put(sil.half[i][0], sil.half[i][1]); }
    // Mirror, skipping both centreline points so no vertex repeats.
    for (int i = sil.count - 2; i >= 1; --i) { put(-sil.half[i][0], sil.half[i][1]); }
    if (n < 3) { return; }

    // A BLACK RIM ON EVERY SHAPE, fading with the marker. Requested after use
    // over real basemap tiles: a small red silhouette over urban tile colours
    // or another aircraft's trail loses its edge, and the rim is what keeps it
    // reading as a shape rather than a smudge. The rim takes its alpha FROM
    // the fill colour so an ageing target fades as one thing - a solid black
    // outline around a ghost would read as a different, newer object.
    // (theme::shadow: black on today's bench, the preset's shadow ink.)
    const ImU32 rim = theme::shadow(static_cast<int>((col >> IM_COL32_A_SHIFT) & 0xFFu));
    if (filled) {
        dl->AddConcavePolyFilled(pts, n, col);
        dl->AddPolyline(pts, n, rim, ImDrawFlags_Closed, 1.5f);
    } else {
        // The hollow variant is a CUE (no reported altitude - see above), so
        // the black cannot replace the coloured outline; it goes UNDER it,
        // wider, as a halo. The cue survives, the contrast arrives.
        dl->AddPolyline(pts, n, rim, ImDrawFlags_Closed, 3.25f);
        dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, 1.5f);
    }
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRACK_SILHOUETTE_HPP
