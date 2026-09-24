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

#include <cstdint>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_ui.hpp"
#include "gui/track_info_cache.hpp"
#include "gui/track_metrics.hpp"
#include "imgui.h"

namespace cascade::gui {

// WHAT THE TARGET IS, in the detail block's own vocabulary: the ABI's
// CASCADE_TRACK_* number as a TrackKind. The block decides from this whether an
// altitude and a speed are lettered in feet and knots or in kilometres and
// km/s, so it is set HERE, inside the adapter every surface goes through,
// rather than by each caller after the fact. It used to be the callers' job -
// app_window.cpp and map_view.cpp each carried a copy of this switch and
// assigned .kind by hand - and drawTrackDetail() below, which makes the two
// calls back to back, left no room to do it: a satellite drawn through it
// came out at "1381234 ft" doing "14890 kt".
//
// The translation lives in this header and not in track_metrics.hpp because
// that one is deliberately free of plugin_abi.h; see its file comment.
//
// A STATION AND ANYTHING THIS BUILD HAS NO NAME FOR KEEP THE AVIATION UNITS.
// An APRS station reports its altitude in feet and its speed in knots, so
// those are the right units for it and not a fallback. Only
// CASCADE_TRACK_SATELLITE is orbital - the same rule map_view.cpp's
// orbitalLadder() uses for the map's colours.
inline TrackKind detailTrackKind(std::uint32_t kind) {
    switch (kind) {
        case CASCADE_TRACK_SATELLITE: return TrackKind::Satellite;
        case CASCADE_TRACK_AIRCRAFT: return TrackKind::Aircraft;
        case CASCADE_TRACK_VESSEL: return TrackKind::Vessel;
        default: return TrackKind::Other;
    }
}

// Everything the block can say about `ht`, gathered from the track itself and
// from the track-info plugin's cache - including what kind of target it is.
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
    in.kind = detailTrackKind(ht.t.kind);
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
