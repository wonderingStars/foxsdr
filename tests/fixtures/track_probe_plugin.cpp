// A REAL plugin module that is a TRACK SOURCE, for tests/test_unplaced_list_app.cpp (0.99.75).
//
// WHAT IT IS FOR. The ADS-B plugin (1.9.0) publishes an aircraft it has heard and cannot place
// yet, flagged CASCADE_TRACK_FLAG_NO_POSITION with NaN coordinates, but only when the host has
// said it understands them (CASCADE_STATE_LISTS_UNPLACED_TRACKS in get_state). This module keeps
// that contract exactly, so a bounded run of the real application against it shows the whole
// path: the host sets the bit, the plugin sees it and publishes, and the target list shows
// "heard without a position" rows beside the placed one and the map does not draw them.
//
// It declares CASCADE_CAP_TRACK_SOURCE (display name "Track Probe") and CASCADE_CAP_HOST_CLIENT,
// and every poll it publishes:
//   PLACED    an aircraft at 51.0 N, 1.0 W, 3000 m, heard 0.5 s ago
//   HEARD1    heard without a position (flagged, NaN), 10668 m, labelled G-HEARD
//   HEARD2    heard without a position (flagged, NaN), altitude unknown, no label
//   HEARDOLD  heard without a position, last heard 61 s ago: past an aircraft's 60 s, so dropped
// but the last three ONLY if the host's state carries the bit. TRACK_PROBE_ASSUME_OLD_HOST=1
// makes it behave as if it never looked (it publishes them whatever the host says), and
// TRACK_PROBE_BARE_NAN=1 adds one more that an old or careless plugin would send: NaN
// coordinates with NO flag (HEARDBARE). TRACK_PROBE_ONLY_HEARD=1 leaves PLACED out, so the list
// holds nothing but aircraft heard without a position.
//
// TRACK_PROBE_COVERAGE=1 (0.99.58, tests/test_map_coverage_app.cpp) adds the three targets the map's
// coverage line must treat differently, around a receiver at 52.0 N, 0.0 E:
//   NEAR   an aircraft at 52.0 N, 1.5 E (about 103 km, due east): a reception, counted
//   FAR    an aircraft at 52.0 N, 40.0 E (about 2,740 km): past any line of sight, so a mis-decode,
//          and not counted
//   SAT    a satellite at 53.0 N, 0.5 E (about 115 km, north-north-east): its position is a
//          prediction, not a reception, and not counted
// (PLACED, 51.0 N 1.0 W, about 130 km south-west, is the fourth: the only other one counted.)
//
// TRACK_PROBE_ESTIMATED=1 (0.99.78, tests/test_unplaced_list_app.cpp) adds the two aircraft the staleness rule tells apart by the
// source's FLAG, both last fixed 100 s ago (past an aircraft's 60 s):
//   EST    flagged CASCADE_TRACK_FLAG_ESTIMATED: the source carried its last fix forward along its course, so the host keeps it
//          (up to 180 s) and the list says "estimated" beside its id
//   STALE  the same age and no flag: a real fix gone quiet, dropped
//
// Built by tests/CMakeLists.txt as a MODULE and including ONLY plugin_abi.h, as every
// third-party plugin must.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "plugin_abi.h"

namespace {

const CascadeHostApi* g_host = nullptr;

void hostAttach(const CascadeHostApi* h) { g_host = h; }

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &hostAttach,
};

bool envOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

// Does the host say it lists targets heard without a position?
bool hostListsUnplaced() {
    if (envOn("TRACK_PROBE_ASSUME_OLD_HOST")) { return true; }
    if (g_host == nullptr || !CASCADE_HOST_HAS(g_host, get_state)) { return false; }
    CascadeReceiverState s;
    std::memset(&s, 0, sizeof s);
    s.structSize = static_cast<uint32_t>(sizeof s);
    if (g_host->get_state(g_host->ctx, &s) != CASCADE_API_OK) { return false; }
    return (s.flags & CASCADE_STATE_LISTS_UNPLACED_TRACKS) != 0u;
}

void put(CascadeTrack* out, uint32_t& n, uint32_t cap, const char* id, const char* label,
         double lat, double lon, double altM, uint64_t ageMs, uint32_t flags,
         uint32_t kind = CASCADE_TRACK_AIRCRAFT) {
    if (n >= cap) { return; }
    CascadeTrack t;
    std::memset(&t, 0, sizeof t);
    std::snprintf(t.id, CASCADE_TRACK_ID_CHARS, "%s", id);
    std::snprintf(t.label, CASCADE_TRACK_LABEL_CHARS, "%s", label);
    t.latDeg = lat;
    t.lonDeg = lon;
    t.altM = altM;
    t.courseDeg = std::nan("");
    t.speedMps = std::nan("");
    t.ageMs = ageMs;
    t.kind = kind;
    t.flags = flags;
    out[n++] = t;
}

void* create() { return new (std::nothrow) int(0); }

int32_t pollTracks(void*, CascadeTrack* out, uint32_t cap) {
    const double nan = std::nan("");
    uint32_t n = 0;
    if (!envOn("TRACK_PROBE_ONLY_HEARD")) {
        put(out, n, cap, "PLACED", "G-PLACE", 51.0, -1.0, 3000.0, 500ull, 0u);
    }
    if (hostListsUnplaced()) {
        put(out, n, cap, "HEARD1", "G-HEARD", nan, nan, 10668.0, 1000ull,
            CASCADE_TRACK_FLAG_NO_POSITION);
        put(out, n, cap, "HEARD2", "", nan, nan, nan, 2000ull, CASCADE_TRACK_FLAG_NO_POSITION);
        put(out, n, cap, "HEARDOLD", "", nan, nan, nan, 61000ull, CASCADE_TRACK_FLAG_NO_POSITION);
    }
    if (envOn("TRACK_PROBE_BARE_NAN")) {
        put(out, n, cap, "HEARDBARE", "", nan, nan, nan, 1500ull, 0u);
    }
    if (envOn("TRACK_PROBE_ESTIMATED")) {
        put(out, n, cap, "EST", "G-EST", 51.5, -0.5, 9000.0, 100000ull, CASCADE_TRACK_FLAG_ESTIMATED);
        put(out, n, cap, "STALE", "G-STALE", 51.6, -0.4, 9000.0, 100000ull, 0u);
    }
    if (envOn("TRACK_PROBE_COVERAGE")) {
        put(out, n, cap, "NEAR", "G-NEAR", 52.0, 1.5, 3000.0, 500ull, 0u);
        put(out, n, cap, "FAR", "G-FAR", 52.0, 40.0, 11000.0, 500ull, 0u);
        put(out, n, cap, "SAT", "SAT-1", 53.0, 0.5, 400000.0, 500ull, 0u, CASCADE_TRACK_SATELLITE);
    }
    return static_cast<int32_t>(n);
}

void destroy(void* handle) { delete static_cast<int*>(handle); }

const CascadeTrackSourceApi kTrackSource = {
    static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)),
    &create,
    &pollTracks,
    nullptr,  // no paths
    &destroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_TRACK_SOURCE, static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &kTrackSource},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Track Probe",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}
