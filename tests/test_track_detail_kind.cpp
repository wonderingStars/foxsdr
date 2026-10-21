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

#include <cmath>
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

// A track-info plugin that knows one aircraft: what a registry lookup is, with
// nothing about a position in it.
void* infoCreate() { return reinterpret_cast<void*>(1); }
std::int32_t infoGet(void*, const char* id, std::uint32_t, CascadeTrackInfo* out) {
    if (id == nullptr || std::strcmp(id, "400A1B") != 0) { return CASCADE_INFO_MISSING; }
    out->registration = "G-TEST";
    out->typeCode = "B738";
    out->typeName = "Boeing 737-800";
    out->manufacturer = "Boeing";
    out->operatorName = "Test Airways";
    out->country = "United Kingdom";
    return CASCADE_INFO_READY;
}
void infoRelease(void*, const CascadeTrackInfo*) {}
std::int32_t infoPollText(void*, char*, std::size_t) { return 0; }
void infoDestroy(void*) {}

CascadeTrackInfoApi makeInfoApi() {
    CascadeTrackInfoApi a{};
    a.structSize = static_cast<std::uint32_t>(sizeof(CascadeTrackInfoApi));
    a.create = &infoCreate;
    a.get_info = &infoGet;
    a.release_info = &infoRelease;
    a.poll_text = &infoPollText;
    a.destroy = &infoDestroy;
    return a;
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

    // [4] A TARGET HEARD WITHOUT A POSITION (0.99.75) goes through the same adapter and
    // comes out saying so: no coordinates, no range, and the registry lookup works for
    // it exactly as it does for a placed one (the cache is asked by id and kind, and
    // nothing about it needs a position).
    {
        const double kNaN = std::nan("");
        HostTrack heard = track(CASCADE_TRACK_AIRCRAFT, "400A1B", 10668.0, kNaN);
        heard.t.latDeg = kNaN;
        heard.t.lonDeg = kNaN;
        heard.t.flags = CASCADE_TRACK_FLAG_NO_POSITION;

        const CascadeTrackInfoApi infoApi = makeInfoApi();
        cascade::gui::TrackInfoCache cache;
        cache.attach(&infoApi);
        CHECK(cache.active());
        const TrackDetailInput in =
            cascade::gui::makeTrackDetailInput(heard, &cache, true, 51.0, 0.0);
        CHECK(!in.hasPosition);
        CHECK(in.infoActive);
        CHECK(in.infoKnown);
        CHECK(in.registration == "G-TEST");
        const std::string text = allText(cascade::gui::buildTrackDetailLines(in));
        std::printf("--- heard-without-a-position block ---\n%s", text.c_str());
        CHECK(text.find("reg     G-TEST") != std::string::npos);
        CHECK(text.find("pos     no position yet") != std::string::npos);
        CHECK(text.find("nan") == std::string::npos);
        CHECK(text.find("range") == std::string::npos);
        CHECK(text.find("alt     10668 m") != std::string::npos);

        // The same aircraft once it has a position: the adapter says so, and the block
        // gives the coordinates and the range back.
        HostTrack placed = heard;
        placed.t.latDeg = 51.5;
        placed.t.lonDeg = -1.0;
        placed.t.flags = 0;
        const TrackDetailInput in2 =
            cascade::gui::makeTrackDetailInput(placed, &cache, true, 51.0, 0.0);
        CHECK(in2.hasPosition);
        const std::string text2 = allText(cascade::gui::buildTrackDetailLines(in2));
        CHECK(text2.find("pos     51.50000, -1.00000") != std::string::npos);
        CHECK(text2.find("range   ") != std::string::npos);
        // A flagged track whose numbers a buggy plugin filled in is still unplaced,
        // and so is one with a NaN coordinate and no flag.
        HostTrack buggy = placed;
        buggy.t.flags = CASCADE_TRACK_FLAG_NO_POSITION;
        CHECK(!cascade::gui::makeTrackDetailInput(buggy, nullptr, false, 0.0, 0.0).hasPosition);
        HostTrack nanOnly = placed;
        nanOnly.t.lonDeg = kNaN;
        CHECK(!cascade::gui::makeTrackDetailInput(nanOnly, nullptr, false, 0.0, 0.0).hasPosition);
        cache.detach();
    }

    return testSummary("test_track_detail_kind");
}
