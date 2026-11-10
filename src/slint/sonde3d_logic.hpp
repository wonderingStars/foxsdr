// sonde3d_logic.hpp - THE RADIOSONDE ASCENT TRACK'S DATA AND WORDS (docs/engine-stage4.md 6.64, specs/THREED2.md section 1): the host's own record of a balloon's heard positions with their altitudes,
// the burst and climb rate read from it, the local east / north kilometres the scene is drawn in, and the number formats. Pure: no Slint, no engine, no window.
//
// WHAT THE HOST HAS. A radiosonde decoder publishes a TRACK for each balloon (its current latitude, longitude, altitude, course and speed) and a PATH (the ground trail: latitudes and longitudes
// only, no altitudes). The altitude of the ASCENT is therefore recorded here, by the host, from the moment it hears the balloon: SondeHistory takes each tick's position and keeps the samples that
// moved. The track the 3D view draws is what the host has HEARD since this window has been listening, not the balloon's whole flight; the plugin's path, when it has one, is the cyan ground track.
// NOT INVENTED: the launch site (the first thing heard is not a launch unless it is on the ground, which the host cannot know - the receiver's own position is drawn instead), the predicted
// landing, the time scrubber, a burst before one was seen.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace foxsdr::slintview3d {

constexpr std::size_t kSondeMaxTracks = 16;        // the plugin's own limit of balloons
constexpr std::size_t kSondeMaxSamples = 4000;     // per balloon (a few hours at one sample every few seconds)
constexpr double kSondeLadderTopKm = 35.0;         // the altitude ladder's top, as the design's

struct SondeSample {
    double tS = 0.0;
    double lat = 0.0, lon = 0.0, altM = 0.0;
};

class SondeHistory {
public:
    // One heard position of balloon "id" at time tS. Kept when it moved (3 m on the ground or 3 m in altitude) or when 10 s have passed since the last kept sample; a position with no finite
    // coordinates is ignored, an altitude that is not finite is kept as a position without one (NaN altM: the track is drawn on the ground there). Returns whether it was kept.
    bool add(const std::string& id, double tS, double lat, double lon, double altM);
    const std::vector<SondeSample>* of(const std::string& id) const;
    std::size_t trackCount() const { return tracks_.size(); }
    void clear() { tracks_.clear(); }
    // Forgets the balloons that are not in "live" and have not been heard for maxAgeS (the oldest of 16 goes first when a new one needs the room).
    void prune(const std::vector<std::string>& live, double nowS, double maxAgeS);

private:
    std::map<std::string, std::vector<SondeSample>> tracks_;
};

struct SondeDerived {
    bool hasTrack = false;
    std::size_t points = 0;
    double currentAltM = 0.0;           // NaN when the newest sample has none
    double maxAltM = 0.0;
    std::size_t maxIndex = 0;
    bool burst = false;                  // the altitude has fallen well below its maximum: the balloon burst, and this is the descent
    double climbMs = 0.0;                // m/s over the last minute or so; NaN when the track is shorter than 8 s
    bool haveClimb = false;
};
SondeDerived deriveSonde(const std::vector<SondeSample>& s);

// Kilometres east and north of (lat0, lon0) for a point: the equirectangular approximation about the origin (the track is at most a few hundred kilometres).
void localKm(double lat0, double lon0, double lat, double lon, double& eastKm, double& northKm);
// The smallest of 5, 10, 20, 25, 50, 100, 200, 250, 500 that is >= v (and a multiple of 100 beyond).
double niceCeilKm(double v);
// "12.5", "85", "0.8": kilometres with at most one decimal, none for a round number.
std::string formatKm(double km);
// The ladder's fraction (0 at the ground, 1 at kSondeLadderTopKm, clamped) of an altitude in metres; -1 for NaN.
double ladderFraction(double altM);
// The climb rate text: "+5.1 m/s", "-12.4 m/s", "" when unknown.
std::string climbText(double ms, bool have);

}  // namespace foxsdr::slintview3d
