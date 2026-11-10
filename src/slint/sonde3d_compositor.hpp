// sonde3d_compositor.hpp - THE RADIOSONDE ASCENT TRACK'S PICTURE (docs/engine-stage4.md 6.64, specs/THREED2.md section 1): the balloon's heard track as a line through an axis box - east and north in
// kilometres along the floor, altitude up - coloured by altitude on the way up, a white star at the burst and a dashed pink line down, the cyan ground track on the floor, an orange triangle at the
// receiver, a dot at the balloon's last heard position with a drop line to the ground; painted by the shared CPU renderer (scene3d.hpp) into a caller-owned buffer. The words (axis ticks and
// captions, the balloon's tag and the burst's) are returned as Label3D rows for Slint to place. See sonde3d_logic.hpp for what is the host's record and what is not drawn.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "scene3d.hpp"

namespace foxsdr::slintview3d {

struct SondeLocal {
    double eastKm = 0.0, northKm = 0.0, altKm = 0.0;     // altKm NaN: no altitude heard (drawn on the ground)
};

struct SondeInput {
    std::vector<SondeLocal> track;                        // the heard samples, oldest first
    bool burst = false;                                   // the track has burst: the samples after the highest are the descent
    std::size_t burstIndex = 0;                           // the sample of the highest altitude
    std::vector<std::pair<double, double>> ground;        // the plugin's ground trail (east, north km); the track's own footprint is drawn when it is empty
    bool haveRx = false;
    double rxEastKm = 0.0, rxNorthKm = 0.0;
    scene3d::Camera camera;
    std::string captionEast = "km E", captionNorth = "km N", captionAlt = "altitude km";
    std::string tag;                                      // the label at the balloon's last position ("S4720113  18460 m  +5 m/s"), "" none
    std::string burstLabel;                               // the label at the burst ("BURST 31 km"), "" none
};

struct SondeResult {
    bool hasTrack = false;
    bool burst = false;
    double topKm = 35.0;                                  // the altitude axis's top
    double eastLoKm = 0.0, eastHiKm = 0.0, northLoKm = 0.0, northHiKm = 0.0;
    std::size_t ascentSegments = 0, descentSegments = 0, groundSegments = 0;
    std::vector<scene3d::Label3D> labels;
};

SondeResult paintSonde3d(scene3d::Scene& scene, scene3d::Rgba* px, int w, int h, const SondeInput& in);
scene3d::Camera sonde3dDefaultCamera();
const scene3d::Ramp& sonde3dRamp();                       // the altitude ramp (viridis): the ladder's and the key's colours
constexpr float kSondeBoxX = 1.9f, kSondeBoxY = 1.2f, kSondeBoxZ = 0.95f;

}  // namespace foxsdr::slintview3d
