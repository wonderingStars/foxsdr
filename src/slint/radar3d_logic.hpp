// radar3d_logic.hpp - THE 3D AIRSPACE'S DATA AND WORDS (docs/engine-stage4.md 6.64, specs/THREED.md section 2): the aircraft the radar scope already plots, as east / north / altitude in
// nautical miles and feet around the antenna, the altitude to flight level and to a height in the scene, the ALTITUDE SCALE's three factors. Pure: no Slint, no engine, no window.
//
// THE DATA IS THE SCOPE'S: radar_logic's RadarScope decides which aircraft are plotted (kind, age, range, FILTER) and keeps their trails; the publisher hands this file those aircraft and
// trails, each position already measured from the antenna by the classic's own polar maths (gui/scope_view.hpp scopeRelative). An aircraft with no altitude (an ADS-B position message
// without one) is drawn ON THE GROUND and said so ("NO ALT"), never at an invented height.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace foxsdr::slintview3d {

constexpr double kRadar3dMaxFl = 400.0;                         // the altitude key's top (FL400); an aircraft above it is drawn at the top and labelled with its real level
constexpr std::array<int, 3> kRadar3dScales{1, 2, 4};           // ALTITUDE SCALE: the vertical exaggeration factors, x1 / x2 / x4
constexpr double kRadar3dHeightAtX1 = 0.55;                     // the scene's height at x1, in units where the range is 1 (the render's FL400 stands about half the range high)

struct Radar3dTrailPoint {
    double eastNm = 0.0, northNm = 0.0;
    double altFt = std::numeric_limits<double>::quiet_NaN();
};

struct Radar3dAircraft {
    std::string id, label;                                       // label: the callsign, else the id
    double eastNm = 0.0, northNm = 0.0;
    double altFt = std::numeric_limits<double>::quiet_NaN();     // NaN = unknown
    double courseDeg = std::numeric_limits<double>::quiet_NaN(); // NaN = unknown
    bool emergency = false, selected = false;
    std::vector<Radar3dTrailPoint> trail;                        // oldest first
};

// What the scope hands the 3D view each tick: whether the scene is wanted (the SCOPE tab is up and the receiver has a position), the scope's range and TRAILS option, the plotted
// aircraft and the tube's own corner words (so the 3D view says what the tube says).
struct Radar3dFeed {
    bool shown = false;
    double rangeNm = 100.0;
    int trailMode = 1;                                           // 0 off, 1 line, 2 ribbon
    std::vector<Radar3dAircraft> aircraft;
    std::string plottedText, rangeCorner, modeText, rxText;      // "14 PLOTTED", "100 NM", "MODE ADS-B", "51.5072 -0.1276"
    std::string emptyText;                                       // the HOME list's empty title while nothing is plotted ("" otherwise)
};

// East and north (nautical miles) of a point `rangeNm` away on `bearingDeg` (clockwise from north). A point on top of the receiver (no bearing) or with no range is at 0, 0.
void polarToEastNorth(double rangeNm, double bearingDeg, double& eastNm, double& northNm);
// 31000 ft -> 310; NaN -> NaN.
double flightLevel(double altFt);
// "FL310" (rounded to the hundred feet, three digits at least), "NO ALT" for an unknown altitude.
std::string flightLevelText(double altFt, const std::string& noAlt);
// The height of an altitude in the scene (range = 1): 0 at the ground, kRadar3dHeightAtX1 * factor at FL400, clamped to that top; NaN (unknown) is the ground.
double sceneHeight(double altFt, int factor);
// The index (0..2) of an ALTITUDE SCALE factor, and the factor of an index (clamped).
int scaleFactorOfIndex(int index);
// The labels to put on the scene: the selected aircraft first, then the nearest ones, at most `maxLabels` in all (a crowded scope labels the near ones); returns indices into `aircraft`.
std::vector<std::size_t> labelOrder(const std::vector<Radar3dAircraft>& aircraft, std::size_t maxLabels);

}  // namespace foxsdr::slintview3d
