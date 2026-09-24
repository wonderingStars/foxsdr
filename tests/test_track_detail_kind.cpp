// Tests for gui/track_detail_view.hpp - the adapter from a host track to the
// detail block says WHAT THE TARGET IS.
//
// WHY THIS FILE EXISTS. buildTrackDetailLines letters a satellite's altitude
// and speed in kilometres and km/s and an aeroplane's in feet and knots, and it
// decides which from TrackDetailInput::kind. makeTrackDetailInput never set
// that field, so it kept its default (Aircraft) and the one-step helper beside
// it, drawTrackDetail(), described the ISS at "1381234 ft" doing "14890 kt".
// The two live callers worked around it by assigning .kind by hand after the
// call, each with its own copy of the translation; the next caller of the
// helper would not have known to.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/track_detail_view.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::core::HostTrack;
using cascade::gui::TrackDetailInput;
using cascade::gui::TrackDetailLine;
using cascade::gui::TrackKind;

namespace {

HostTrack track(std::uint32_t kind, const char* id, double altM, double speedMps) {
    HostTrack ht;
    std::snprintf(ht.t.id, sizeof(ht.t.id), "%s", id);
    ht.t.kind = kind;
    ht.t.latDeg = 51.5;
    ht.t.lonDeg = -1.0;
    ht.t.altM = altM;
    ht.t.speedMps = speedMps;
    ht.t.courseDeg = 90.0;
    ht.t.ageMs = 1000;
    ht.plugin = "test";
    return ht;
}

std::string allText(const std::vector<TrackDetailLine>& lines) {
    std::string s;
    for (const TrackDetailLine& l : lines) {
        s += l.text;
        s += '\n';
    }
    return s;
}

}  // namespace

int main() {
    // [1] A satellite comes out of the adapter AS a satellite, and the block
    // built from it straight away - exactly what drawTrackDetail() does - is
    // in orbital units, not aviation ones.
    {
        const HostTrack iss = track(CASCADE_TRACK_SATELLITE, "25544", 421000.0, 7660.0);
        const TrackDetailInput in = cascade::gui::makeTrackDetailInput(iss, nullptr, false, 0.0, 0.0);
        CHECK(in.kind == TrackKind::Satellite);
        const std::string text = allText(cascade::gui::buildTrackDetailLines(in));
        std::printf("--- satellite block ---\n%s", text.c_str());
        CHECK(text.find(" ft") == std::string::npos);
        CHECK(text.find(" kt") == std::string::npos);
        CHECK(text.find(" km") != std::string::npos);
    }

    // [2] Every other kind the ABI names, and one it does not.
    {
        CHECK(cascade::gui::makeTrackDetailInput(track(CASCADE_TRACK_AIRCRAFT, "4CA123", 10000.0, 230.0),
                                                 nullptr, false, 0.0, 0.0)
                  .kind == TrackKind::Aircraft);
        CHECK(cascade::gui::makeTrackDetailInput(track(CASCADE_TRACK_VESSEL, "235000001", 0.0, 5.0),
                                                 nullptr, false, 0.0, 0.0)
                  .kind == TrackKind::Vessel);
        // A station and anything this build has no name for keep the aviation
        // units: an APRS station reports feet and knots.
        CHECK(cascade::gui::makeTrackDetailInput(track(CASCADE_TRACK_STATION, "M0ABC-9", 100.0, 0.0),
                                                 nullptr, false, 0.0, 0.0)
                  .kind == TrackKind::Other);
        CHECK(cascade::gui::makeTrackDetailInput(track(CASCADE_TRACK_UNKNOWN, "x", 100.0, 0.0),
                                                 nullptr, false, 0.0, 0.0)
                  .kind == TrackKind::Other);
        CHECK(cascade::gui::makeTrackDetailInput(track(99u, "future", 100.0, 0.0), nullptr, false,
                                                 0.0, 0.0)
                  .kind == TrackKind::Other);
    }

    // [3] An aeroplane still reads in feet and knots.
    {
        const TrackDetailInput in = cascade::gui::makeTrackDetailInput(
            track(CASCADE_TRACK_AIRCRAFT, "4CA123", 10000.0, 230.0), nullptr, false, 0.0, 0.0);
        const std::string text = allText(cascade::gui::buildTrackDetailLines(in));
        CHECK(text.find(" ft") != std::string::npos);
        CHECK(text.find(" kt") != std::string::npos);
    }

    return testSummary("test_track_detail_kind");
}
