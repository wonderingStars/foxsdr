// aircraft_icons.hpp - the shaded aircraft icons the map and the radar scope
// draw (0.99.25), and the pure decisions behind them.
//
// WHAT CHANGED AND WHY. Until 0.99.24 an aircraft was a flat nine-pixel
// silhouette (gui/track_silhouette.hpp). A tester reported that the single-
// engine aeroplanes at the airfield near him drew with the same icon as the
// airliners - and at that size a light aeroplane's straight wing and an
// airliner's swept one really are one shape. The owner chose, from a mockup,
// a set of top-view icons with shading, a cast shadow and the detail that
// tells them apart (two engines or four, a high wing and a propeller, a rotor),
// drawn 48 px by default and at whatever size the user sets in Display.
//
// WHERE THE PICTURES COME FROM. resources/aircraft/aircraft_icons.svg is the
// source; tools/make-aircraft-icons.py renders it to RGBA once and writes
// gui/aircraft_icon_pixels.hpp, so the application needs no SVG or image
// library. The order of AircraftIcon below IS the order of that file, and
// test_aircraft_icons checks the names agree.
//
// "NO TYPE STATED" IS ITS OWN ICON. The flat set drew an aircraft that
// broadcast no category as an airliner, so an untyped Cessna and a Boeing
// looked identical and the map claimed something it did not know. It is now a
// plain arrowhead: honest about the heading, silent about the type.
//
// ALTITUDE COLOUR moves to a halo under the icon, because the icons now carry
// their own colours. Filled halo = altitude known, ring only = not reported -
// the same distinction the solid and hollow silhouettes used to draw.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_AIRCRAFT_ICONS_HPP
#define CASCADE_GUI_AIRCRAFT_ICONS_HPP

#include <cstdint>
#include <vector>

#include "core/plugin_abi.h"
#include "imgui.h"

namespace cascade::gui {

enum class AircraftIcon : int {
    Airliner = 0,
    Heavy,
    Light,
    Glider,
    Microlight,
    Uav,
    Balloon,
    Surface,
    Helicopter,
    Unknown,
    Count
};

inline constexpr int kAircraftIconCount = static_cast<int>(AircraftIcon::Count);

// The names in enum order - the generator writes the same list into
// aircraft_pixels::kNames, and the test compares the two.
inline constexpr const char* kAircraftIconNames[kAircraftIconCount] = {
    "airliner", "heavy", "light", "glider", "microlight",
    "uav", "balloon", "surface", "helicopter", "unknown"};

// THE SIZE SETTING (Display > "Aircraft icons"), in screen pixels across.
// 48 is the owner's standard (asked for 32, then "i think they need to start
// at 48px" once the set was being built). Below 16 the detail that is the
// point of these icons is gone; 96 is the ceiling because the texture is
// 128 px and must never be drawn larger than itself, and because a busy map
// at that size is already a pile of icons.
inline constexpr int kAircraftIconMinPx = 16;
inline constexpr int kAircraftIconMaxPx = 96;
inline constexpr int kAircraftIconDefaultPx = 48;

inline int clampAircraftIconPx(int px) {
    if (px < kAircraftIconMinPx) { return kAircraftIconMinPx; }
    if (px > kAircraftIconMaxPx) { return kAircraftIconMaxPx; }
    return px;
}

// Which icon a broadcast category draws. Total over the ABI's vocabulary; a
// value this build has never heard of (a newer plugin) draws "no type stated"
// rather than a guess.
//
// Medium 1, Medium 2 and high vortex share the airliner: they differ by
// tonnage in a way no icon can honestly show. ADS-B says only "rotorcraft",
// never which helicopter, so every rotorcraft draws the one helicopter.
AircraftIcon aircraftIconForCategory(std::uint32_t category);

// The four corners of the icon's image, turned to `courseDeg` (0 = north = up
// the screen, clockwise) about `centre`, `sizePx` across. Order: top-left,
// top-right, bottom-right, bottom-left OF THE IMAGE, i.e. for UVs (0,0),
// (1,0), (1,1), (0,1). An unknown course (NaN, the ABI's "not known") points
// north rather than inventing a heading.
struct IconQuad {
    ImVec2 p[4];
};
IconQuad aircraftIconQuad(const ImVec2& centre, double courseDeg, float sizePx);

// The shadow falls down and to the right of the icon ON THE SCREEN, whatever
// the heading, so the light stays in one place while the aircraft turns.
ImVec2 aircraftShadowOffset(float sizePx);

// --- pixel work, pure so it can be tested -------------------------------
//
// halveRgba: one mipmap step, a 2x2 box filter weighted by alpha so a
// transparent neighbour cannot darken an edge. `side` must be even and >= 2;
// the result is (side/2)^2 RGBA. Where all four pixels are transparent the
// colour is their plain average, which keeps the bled edge colour going down
// the chain.
std::vector<std::uint8_t> halveRgba(const std::uint8_t* rgba, int side);

// shadowFromAlpha: the icon's alpha, blurred, as black RGBA at `opacity`.
std::vector<std::uint8_t> shadowFromAlpha(const std::uint8_t* rgba, int side, float opacity);

// --- drawing (needs the GL context current) --------------------------------

// Draws the shadow and the icon. `alpha` fades an ageing target (0..1).
// Returns false when the textures could not be made, so the caller can fall
// back to the flat silhouettes rather than draw nothing.
bool drawAircraftIcon(ImDrawList* dl, const ImVec2& centre, double courseDeg, float sizePx,
                      AircraftIcon icon, float alpha);

// THE WHOLE MARKER, shared by the map and the radar scope so the two cannot
// drift (they once carried two copies of the flat silhouette code):
//   - the altitude halo in `altitudeCol` - filled when `altKnown`, a ring
//     alone when the aircraft reports no altitude;
//   - when `picked`, a heavier ring outside it - the selection;
//   - the shadow and the icon, faded with `altitudeCol`'s alpha (a target's
//     age is carried in that alpha);
//   - and, if the textures could not be made, the flat silhouette from
//     gui/track_silhouette.hpp in the same colour, so an aircraft is never
//     simply missing.
void drawAircraftMarker(ImDrawList* dl, const ImVec2& centre, double courseDeg, float sizePx,
                        std::uint32_t category, ImU32 altitudeCol, bool altKnown, bool picked);

// How far from its centre a marker reaches, for the emergency ring, the label
// beside it and the click target. Never less than the radii the flat
// silhouettes used, so a small setting does not make targets harder to hit.
float aircraftMarkerRadius(float sizePx);

// Deletes the textures. Call while the GL context that made them is current
// (AppWindow::run's teardown), never from a destructor.
void releaseAircraftIconTextures();

}  // namespace cascade::gui

#endif  // CASCADE_GUI_AIRCRAFT_ICONS_HPP
