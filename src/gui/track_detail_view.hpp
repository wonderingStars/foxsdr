// track_detail_view.hpp - the one place a target's detail block is drawn.
//
// WHY THIS FILE EXISTS. The same block - callsign, registry entry, position,
// altitude, speed, course, range and age - is shown in three places: hovering a
// target on the map, hovering a row in the list beside it, and the details
// window the list's per-row button opens. It was written out twice by hand and
// the two copies had already drifted: the altitude band, the units and the
// registry fields each changed in one and not the other inside a week.
//
// The split is deliberate. WHAT THE BLOCK SAYS is built by
// buildTrackDetailLines in track_metrics.hpp, which is pure and tested against
// the exact strings a user reads. HOW IT IS DRAWN is the handful of ImGui calls
// below, which a test cannot reach and which therefore contain no decisions.
// The adapter in between is here too, so no caller has to know how a
// TrackInfoCache answer becomes a detail line.
//
// GUI THREAD ONLY, like everything else in this directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_GUI_TRACK_DETAIL_VIEW_HPP
#define CASCADE_GUI_TRACK_DETAIL_VIEW_HPP

#include <vector>

#include "core/plugin_ui.hpp"
#include "gui/track_info_cache.hpp"
#include "gui/track_metrics.hpp"
#include "imgui.h"

namespace cascade::gui {

// Everything the block can say about `ht`, gathered from the track itself and
// from the track-info plugin's cache.
//
// ASKING THE CACHE IS WHAT STARTS THE LOOKUP, which is why this takes the cache
// rather than an already-fetched answer: hovering a target is exactly the
// moment its registration should be queued for.
//
// `info` may be null, and is whenever no track-info plugin is installed.
inline TrackDetailInput makeTrackDetailInput(const cascade::core::HostTrack& ht,
                                             TrackInfoCache* info, bool hasHome,
                                             double homeLatDeg, double homeLonDeg) {
    TrackDetailInput in;
    in.label = (ht.t.label[0] != '\0') ? ht.t.label : ht.t.id;
    in.id = ht.t.id;
    in.source = ht.plugin;
    in.latDeg = ht.t.latDeg;
    in.lonDeg = ht.t.lonDeg;
    in.altM = ht.t.altM;
    in.speedMps = ht.t.speedMps;
    in.courseDeg = ht.t.courseDeg;
    in.ageMs = ht.t.ageMs;
    in.hasHome = hasHome;
    in.homeLatDeg = homeLatDeg;
    in.homeLonDeg = homeLonDeg;

    if (info != nullptr && info->active()) {
        in.infoActive = true;
        const TrackInfoCache::Info* d = info->get(in.id, ht.t.kind);
        if (d == nullptr) {
            // Nothing cached yet: the plugin has been asked and has not
            // answered. Distinct from an answer of "not in my data", which is
            // a d that exists with known == false.
            in.infoPending = true;
        } else if (d->known) {
            in.infoKnown = true;
            in.registration = d->registration;
            in.typeCode = d->typeCode;
            in.typeName = d->typeName;
            in.operatorName = d->operatorName;
            in.country = d->country;
        }
    }
    return in;
}

// Draws the block into the CURRENT ImGui window. No decisions here: which
// lines exist, what they say and which are unknown were all settled by
// buildTrackDetailLines.
inline void drawTrackDetailLines(const std::vector<TrackDetailLine>& lines) {
    for (const TrackDetailLine& l : lines) {
        if (l.known) {
            ImGui::TextUnformatted(l.text.c_str());
        } else {
            // Dimmed, which is the whole visual difference between "the source
            // does not know" and a value.
            ImGui::TextDisabled("%s", l.text.c_str());
        }
        if (l.separatorAfter) { ImGui::Separator(); }
    }
}

// The block for one target, gathered and drawn. The two-step version above is
// kept separate for the details window, which draws other things around it.
inline void drawTrackDetail(const cascade::core::HostTrack& ht, TrackInfoCache* info,
                            bool hasHome, double homeLatDeg, double homeLonDeg) {
    drawTrackDetailLines(
        buildTrackDetailLines(makeTrackDetailInput(ht, info, hasHome, homeLatDeg, homeLonDeg)));
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_TRACK_DETAIL_VIEW_HPP
