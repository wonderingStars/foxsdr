// survey3d_compositor.hpp - THE SURVEY ENGINE'S 3D SURFACE (docs/engine-stage4.md 6.64, specs/THREED2.md section 4): the channels the plugin publishes as ridges over frequency and occupancy in
// an axis box - a ridge stands at the channel's frequency, as wide as its width, as high as its peak over the floor, as long as its Seen share - painted by the shared CPU renderer
// (scene3d.hpp) into a caller-owned buffer, with the axis words and the channel-rank numbers returned as Label3D rows for Slint to place. See survey3d_logic.hpp for why the depth axis is
// occupancy and not the design's hour of day.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "scene3d.hpp"
#include "survey3d_logic.hpp"

namespace foxsdr::slintview3d {

struct SurveyInput {
    std::vector<SurveyChannel> channels;     // in the panel's order: by activity, so the first is rank 1
    scene3d::Camera camera;
    std::string captionFreq = "MHz", captionSeen = "Seen (%)", captionDb = "dB";
    std::size_t rankLabels = 6;              // the busiest channels labelled with their rank
};

struct SurveyResult {
    bool hasData = false;
    std::size_t triangles = 0;
    std::size_t ranked = 0;                  // rank labels placed
    double loHz = 0.0, hiHz = 0.0, floorDb = 0.0, loDb = 0.0, hiDb = 0.0;
    std::vector<scene3d::Label3D> labels;
};

SurveyResult paintSurvey3d(scene3d::Scene& scene, scene3d::Rgba* px, int w, int h, const SurveyInput& in);
scene3d::Camera survey3dDefaultCamera();
constexpr float kSurveyBoxX = 1.0f, kSurveyBoxY = 0.8f, kSurveyBoxZ = 0.65f;

}  // namespace foxsdr::slintview3d
