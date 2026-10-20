// fixture_group14_plugin.cpp - a REAL track source for the tests of the map and the plugin windows (window port group 14,
// part a): the one module in the tests that plots targets through the plugin ABI's CASCADE_CAP_TRACK_SOURCE, so a window
// that polls its plugins draws a map page, a target register and a details window with no radio, no network and no file.
// Until it existed no hermetic frame could put a track in the window's plugin UI (it is filled only by the poll of a
// loaded track source), so the seven consumers of tracks - the page filter and the coverage feed of drawPluginWindows, the
// target register, the details window, the offer to take the middle of the traffic, the radar scope's aircraft and the
// Patch page's Map part - were held by code reading only.
//
// It reports a FIXED set, so a test can state every figure it reads back:
//
//   id      kind      latitude  longitude  altitude  course  speed  age
//   G14A1   aircraft    51.60      -0.60     3000 m      90°   120 m/s     1 s
//   G14A2   aircraft    51.40      -0.30     6000 m     180°   200 m/s     2 s
//   G14A3   aircraft    51.50      -0.10     9000 m     270°   220 m/s     3 s
//   G14OLD  aircraft    50.00       1.00    (none)     (none) (none)     120 s    - past the 60 s an aircraft is kept
//
// (so the page counts THREE visible targets, the fourth is stale and never listed, and the middle of the three heard is
// exactly 51.5 N and -1/3 W), and one path, "G14-trail", three vertices along the first aircraft's course.
//
// SINCE PART D (the plugin windows) it also declares a PANEL and an INSTRUMENT, so the windows the plugin UI feeds besides
// the map have something to draw with no radio and no file (the other fixtures declare neither):
//
//   panel       "Group Fourteen Panel"       two columns "Item" and "Reading"; rows: the heading "G14 summary", the cells
//                                            "aircraft" "3" and the cells "stale" "1"
//   instrument  "Group Fourteen Instrument"  kind GENERIC, always has a reading: seq 7, flags LOCK, values[0] 42, text[0]
//                                            "G14 READY"; memory columns "Event" and "Detail", one row "G14 memory" "m1"
//
// The descriptor name is "Group Fourteen Tracks" and the ids start "G14". A SECOND library built from this file under another
// name and prefix (-DFIXTURE_NAME, -DFIXTURE_PREFIX: "Group Fourteen Tracks B" and "G14B", ids G14BA1... and G14B-trail) is the
// second track source the tests of "one map page per plugin" need.
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#ifndef FIXTURE_NAME
#define FIXTURE_NAME "Group Fourteen Tracks"
#endif
#ifndef FIXTURE_PREFIX
#define FIXTURE_PREFIX "G14"
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct Source {
    CascadePathPoint trail[3] = {{51.60, -0.60}, {51.60, -0.55}, {51.60, -0.50}};
};

CascadeTrack make(const char* id, double lat, double lon, double altM, double course, double speed, std::uint64_t ageMs) {
    CascadeTrack t{};
    std::snprintf(t.id, CASCADE_TRACK_ID_CHARS, "%s", id);
    std::snprintf(t.label, CASCADE_TRACK_LABEL_CHARS, "F-%s", id);
    t.latDeg = lat;
    t.lonDeg = lon;
    t.altM = altM;
    t.courseDeg = course;
    t.speedMps = speed;
    t.ageMs = ageMs;
    t.kind = CASCADE_TRACK_AIRCRAFT;
    t.flags = 0;
    return t;
}

void* create() { return new Source(); }
void destroy(void* handle) { delete static_cast<Source*>(handle); }

int32_t pollTracks(void*, CascadeTrack* out, uint32_t cap) {
    const CascadeTrack all[4] = {
        make(FIXTURE_PREFIX "A1", 51.60, -0.60, 3000.0, 90.0, 120.0, 1000u),
        make(FIXTURE_PREFIX "A2", 51.40, -0.30, 6000.0, 180.0, 200.0, 2000u),
        make(FIXTURE_PREFIX "A3", 51.50, -0.10, 9000.0, 270.0, 220.0, 3000u),
        make(FIXTURE_PREFIX "OLD", 50.00, 1.00, std::nan(""), std::nan(""), std::nan(""), 120000u),
    };
    uint32_t n = 0;
    for (; n < 4u && n < cap; ++n) { out[n] = all[n]; }
    return static_cast<int32_t>(n);
}

int32_t pollPaths(void* handle, CascadePath* out, uint32_t cap) {
    Source* s = static_cast<Source*>(handle);
    if (s == nullptr || cap == 0u) { return 0; }
    std::snprintf(out[0].id, CASCADE_TRACK_ID_CHARS, FIXTURE_PREFIX "-trail");
    out[0].kind = CASCADE_TRACK_AIRCRAFT;
    out[0].flags = 0;
    out[0].points = s->trail;
    out[0].count = 3u;
    return 1;
}

// --- the panel: a fixed table ----------------------------------------------------------------------------------------------
void* panelCreate() { return new int(0); }
void panelDestroy(void* handle) { delete static_cast<int*>(handle); }
uint32_t panelColumns(void*, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "Item");
    std::snprintf(headings[1], CASCADE_PANEL_CELL_CHARS, "Reading");
    return 2u;
}
int32_t panelRows(void*, CascadePanelRow* out, uint32_t cap) {
    if (cap < 3u) { return 0; }
    std::memset(out, 0, 3u * sizeof(CascadePanelRow));
    out[0].kind = CASCADE_ROW_HEADING;
    std::snprintf(out[0].cells[0], CASCADE_PANEL_CELL_CHARS, "G14 summary");
    out[1].kind = CASCADE_ROW_CELLS;
    std::snprintf(out[1].cells[0], CASCADE_PANEL_CELL_CHARS, "aircraft");
    std::snprintf(out[1].cells[1], CASCADE_PANEL_CELL_CHARS, "3");
    out[2].kind = CASCADE_ROW_CELLS;
    out[2].flags = CASCADE_ROW_FLAG_MUTED;
    std::snprintf(out[2].cells[0], CASCADE_PANEL_CELL_CHARS, "stale");
    std::snprintf(out[2].cells[1], CASCADE_PANEL_CELL_CHARS, "1");
    return 3;
}

const CascadePanelApi kPanel = {
    static_cast<uint32_t>(sizeof(CascadePanelApi)),
    "Group Fourteen Panel",
    &panelCreate,
    &panelColumns,
    &panelRows,
    &panelDestroy,
};

// --- the instrument: a fixed reading and one memory row --------------------------------------------------------------------
int32_t instrumentState(void*, CascadeInstrumentState* out) {
    out->flags = CASCADE_INSTRUMENT_FLAG_LOCK;
    out->seq = 7u;
    out->values[0] = 42.0;
    std::snprintf(out->text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "G14 READY");
    return 1;
}
uint32_t instrumentColumns(void*, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "Event");
    std::snprintf(headings[1], CASCADE_PANEL_CELL_CHARS, "Detail");
    return 2u;
}
int32_t instrumentRows(void*, CascadePanelRow* out, uint32_t cap) {
    if (cap < 1u) { return 0; }
    std::memset(out, 0, sizeof(CascadePanelRow));
    out[0].kind = CASCADE_ROW_CELLS;
    std::snprintf(out[0].cells[0], CASCADE_PANEL_CELL_CHARS, "G14 memory");
    std::snprintf(out[0].cells[1], CASCADE_PANEL_CELL_CHARS, "m1");
    return 1;
}

const CascadeInstrumentApi kInstrument = {
    static_cast<uint32_t>(sizeof(CascadeInstrumentApi)),
    "Group Fourteen Instrument",
    CASCADE_INSTRUMENT_GENERIC,
    &panelCreate,
    &instrumentState,
    &instrumentColumns,
    &instrumentRows,
    &panelDestroy,
};

const CascadeTrackSourceApi kTracks = {
    static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)),
    &create,
    &pollTracks,
    &pollPaths,
    &destroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_TRACK_SOURCE, static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &kTracks},
    {CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)), &kPanel},
    {CASCADE_CAP_INSTRUMENT, static_cast<uint32_t>(sizeof(CascadeInstrumentApi)), &kInstrument},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    FIXTURE_NAME,
    "1.0.0",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
