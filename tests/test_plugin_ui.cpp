// PluginUi: the GUI-side half of the plugin system - map targets, plugin
// windows, and the host services a plugin may call.
//
// No DLL: LoadedPlugin holds plain table pointers, so a fake plugin is a
// static table plus counters. That lets every case assert what the host DID
// rather than inferring it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_ui.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginUi;

// --- fake track source ----------------------------------------------------

struct FakeTrack {
    int created = 0;
    int destroyed = 0;
    std::vector<CascadeTrack> emit;
    std::vector<CascadePathPoint> pathPts;
    bool emitPath = false;
};
FakeTrack g_tr;

void* trCreate() { ++g_tr.created; return &g_tr; }
void trDestroy(void*) { ++g_tr.destroyed; }

int32_t trPoll(void*, CascadeTrack* out, uint32_t cap) {
    const uint32_t n = static_cast<uint32_t>(g_tr.emit.size()) < cap
                           ? static_cast<uint32_t>(g_tr.emit.size())
                           : cap;
    for (uint32_t i = 0; i < n; ++i) { out[i] = g_tr.emit[i]; }
    return static_cast<int32_t>(n);
}

int32_t trPollPaths(void*, CascadePath* out, uint32_t cap) {
    if (!g_tr.emitPath || cap == 0) { return 0; }
    std::snprintf(out[0].id, CASCADE_TRACK_ID_CHARS, "orbit");
    out[0].kind = CASCADE_TRACK_SATELLITE;
    out[0].flags = 0;
    out[0].points = g_tr.pathPts.data();
    out[0].count = static_cast<uint32_t>(g_tr.pathPts.size());
    return 1;
}

CascadeTrackSourceApi makeTrackApi(bool withPaths) {
    CascadeTrackSourceApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeTrackSourceApi));
    a.create = &trCreate;
    a.poll_tracks = &trPoll;
    a.poll_paths = withPaths ? &trPollPaths : nullptr;
    a.destroy = &trDestroy;
    return a;
}

CascadeTrack track(const char* id, double lat, double lon) {
    CascadeTrack t{};
    std::snprintf(t.id, CASCADE_TRACK_ID_CHARS, "%s", id);
    std::snprintf(t.label, CASCADE_TRACK_LABEL_CHARS, "L-%s", id);
    t.latDeg = lat;
    t.lonDeg = lon;
    t.altM = std::nan("");
    t.courseDeg = std::nan("");
    t.speedMps = std::nan("");
    t.kind = CASCADE_TRACK_AIRCRAFT;
    return t;
}

// --- fake panel -----------------------------------------------------------

struct FakePanel {
    int created = 0, destroyed = 0, columnCalls = 0;
    std::vector<CascadePanelRow> rows;
};
FakePanel g_pn;

void* pnCreate() { ++g_pn.created; return &g_pn; }
void pnDestroy(void*) { ++g_pn.destroyed; }

uint32_t pnColumns(void*, char h[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    ++g_pn.columnCalls;
    std::snprintf(h[0], CASCADE_PANEL_CELL_CHARS, "Satellite");
    std::snprintf(h[1], CASCADE_PANEL_CELL_CHARS, "AOS");
    return 2u;
}

int32_t pnRows(void*, CascadePanelRow* out, uint32_t cap) {
    const uint32_t n =
        static_cast<uint32_t>(g_pn.rows.size()) < cap ? static_cast<uint32_t>(g_pn.rows.size())
                                                      : cap;
    for (uint32_t i = 0; i < n; ++i) { out[i] = g_pn.rows[i]; }
    return static_cast<int32_t>(n);
}

CascadePanelApi makePanelApi() {
    CascadePanelApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadePanelApi));
    a.title = "Satellite passes";
    a.create = &pnCreate;
    a.columns = &pnColumns;
    a.poll_rows = &pnRows;
    a.destroy = &pnDestroy;
    return a;
}

// --- fake host client -----------------------------------------------------

struct FakeHostClient {
    int attaches = 0;
    const CascadeHostApi* api = nullptr;
    int attachOrderMarker = 0;  // value of a global counter when attach ran
};
FakeHostClient g_hc;
int g_orderCounter = 0;
// Every bridge handed out, in rebuild order. One plugin overwrites g_hc.api,
// so a case with two plugins needs each one's own handle.
std::vector<const CascadeHostApi*> g_attached;

void hcAttach(const CascadeHostApi* h) {
    ++g_hc.attaches;
    g_hc.api = h;
    g_hc.attachOrderMarker = ++g_orderCounter;
    g_attached.push_back(h);
}

// Bounds-safe: a CHECK on the size records and continues, so indexing behind
// one would read past the end in exactly the run that has something to report.
int32_t requestTuneFrom(std::size_t i, double hz) {
    if (i >= g_attached.size() || g_attached[i] == nullptr) { return CASCADE_TUNE_FAILED; }
    return g_attached[i]->request_tune(g_attached[i]->ctx, hz);
}

std::string requesterAt(const std::vector<std::string>& v, std::size_t i) {
    return i < v.size() ? v[i] : std::string();
}

CascadeHostClientApi makeHostClientApi() {
    CascadeHostClientApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeHostClientApi));
    a.attach = &hcAttach;
    return a;
}

LoadedPlugin plug(const char* name) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    // The host finds every plugin as a FILE, and that file is what a tune
    // grant is keyed on, so a fake plugin without one could never be granted
    // anything. The display name above is deliberately not the key.
    p.path = std::string("C:/plugins/") + name + ".dll";
    return p;
}

// The same, with the module file the host found it in. A tune grant is keyed
// on that file, never on the display name above, so a case about permissions
// has to say which file each plugin came from.
LoadedPlugin plugAt(const char* name, const char* path) {
    LoadedPlugin p = plug(name);
    p.path = path;
    return p;
}

void resetAll() {
    g_tr = FakeTrack{};
    g_pn = FakePanel{};
    g_hc = FakeHostClient{};
    g_orderCounter = 0;
    g_attached.clear();
}

HostServices workingServices(int* tuneCalls, double* lastTuned) {
    HostServices s;
    s.centreHz = [] { return 1090000000.0; };
    s.sampleRateHz = [] { return 2400000.0; };
    s.unixTimeMs = [] { return static_cast<int64_t>(1700000000000LL); };
    s.tune = [tuneCalls, lastTuned](double hz) {
        ++(*tuneCalls);
        *lastTuned = hz;
        return static_cast<int32_t>(CASCADE_TUNE_OK);
    };
    return s;
}

}  // namespace

int main() {
    // --- tracks are polled, copied and tagged ----------------------------
    {
        resetAll();
        const CascadeTrackSourceApi api = makeTrackApi(false);
        g_tr.emit = {track("406135", 53.98, -2.27), track("4CAE5A", 53.75, -2.03)};
        LoadedPlugin p = plug("ADS-B");
        p.trackSource = &api;
        PluginUi ui;
        ui.rebuild({p});
        CHECK(g_tr.created == 1);
        ui.poll();
        CHECK(ui.tracks().size() == 2u);
        CHECK(std::string(ui.tracks()[0].t.id) == "406135");
        // Tagged, so a map showing aircraft and ships can tell them apart.
        CHECK(ui.tracks()[0].plugin == "ADS-B");
        // NaN unknowns survive the copy rather than becoming 0.
        CHECK(std::isnan(ui.tracks()[0].t.altM));
    }

    // --- a track with no valid position is DROPPED, not plotted ----------
    {
        // (0,0) is a real place in the Atlantic. A plugin that emits an
        // out-of-range or NaN position must not put a phantom target there.
        resetAll();
        const CascadeTrackSourceApi api = makeTrackApi(false);
        g_tr.emit = {track("good", 51.5, -0.12), track("bad", 91.0, 0.0),
                     track("nan", std::nan(""), 0.0), track("lon", 0.0, 999.0)};
        LoadedPlugin p = plug("Src");
        p.trackSource = &api;
        PluginUi ui;
        ui.rebuild({p});
        ui.poll();
        CHECK(ui.tracks().size() == 1u);
        CHECK(std::string(ui.tracks()[0].t.id) == "good");
    }

    // --- paths are COPIED out of the plugin's buffer ---------------------
    {
        // The ABI only guarantees a path's vertices until the next poll, and
        // the host draws on its own schedule. If this were a borrow, mutating
        // the plugin's buffer afterwards would change what is on screen.
        resetAll();
        const CascadeTrackSourceApi api = makeTrackApi(true);
        g_tr.emitPath = true;
        g_tr.pathPts = {{10.0, 20.0}, {11.0, 21.0}, {12.0, 22.0}};
        LoadedPlugin p = plug("Sat");
        p.trackSource = &api;
        PluginUi ui;
        ui.rebuild({p});
        ui.poll();
        CHECK(ui.paths().size() == 1u);
        CHECK(ui.paths()[0].points.size() == 3u);
        CHECK(ui.paths()[0].points[1].latDeg == 11.0);
        // Scribble over the plugin's storage; the host's copy must not move.
        g_tr.pathPts[1] = {99.0, 99.0};
        CHECK(ui.paths()[0].points[1].latDeg == 11.0);
    }

    // --- a source with no poll_paths is fine -----------------------------
    {
        resetAll();
        const CascadeTrackSourceApi api = makeTrackApi(false);  // poll_paths NULL
        g_tr.emit = {track("a", 1.0, 2.0)};
        LoadedPlugin p = plug("NoPaths");
        p.trackSource = &api;
        PluginUi ui;
        ui.rebuild({p});
        ui.poll();
        CHECK(ui.tracks().size() == 1u);
        CHECK(ui.paths().empty());
    }

    // --- panel: columns read once, rows refreshed every poll -------------
    {
        resetAll();
        const CascadePanelApi api = makePanelApi();
        LoadedPlugin p = plug("Tracker");
        p.panel = &api;
        PluginUi ui;
        ui.rebuild({p});
        CHECK(ui.panels().size() == 1u);
        CHECK(ui.panels()[0].title == "Satellite passes");
        CHECK(ui.panels()[0].headings.size() == 2u);
        CHECK(ui.panels()[0].headings[1] == "AOS");
        CHECK(g_pn.columnCalls == 1);

        CascadePanelRow r{};
        r.kind = CASCADE_ROW_CELLS;
        std::snprintf(r.cells[0], CASCADE_PANEL_CELL_CHARS, "NOAA 19");
        g_pn.rows = {r};
        ui.poll();
        CHECK(ui.panels()[0].rows.size() == 1u);
        // A panel's shape is fixed for its lifetime, so polling must not
        // re-ask for columns every frame.
        ui.poll();
        CHECK(g_pn.columnCalls == 1);
        // Rows REPLACE rather than accumulate.
        CHECK(ui.panels()[0].rows.size() == 1u);
        g_pn.rows.clear();
        ui.poll();
        CHECK(ui.panels()[0].rows.empty());
    }

    // --- host services: attach happens, and BEFORE create() --------------
    {
        resetAll();
        const CascadeTrackSourceApi tapi = makeTrackApi(false);
        const CascadeHostClientApi hapi = makeHostClientApi();
        LoadedPlugin p = plug("Tracker");
        p.trackSource = &tapi;
        p.hostClient = &hapi;
        int tuneCalls = 0;
        double lastTuned = 0.0;
        PluginUi ui;
        ui.setServices(workingServices(&tuneCalls, &lastTuned));
        ui.rebuild({p});
        CHECK(g_hc.attaches == 1);
        CHECK(g_hc.api != nullptr);
        // The ABI promises attach() runs before any capability's create(), so
        // a tracker can read the receiver while building its initial state.
        CHECK(g_hc.attachOrderMarker == 1);
        // The receiver is readable through the table.
        CHECK(g_hc.api->centre_hz(g_hc.api->ctx) == 1090000000.0);
        CHECK(g_hc.api->sample_rate_hz(g_hc.api->ctx) == 2400000.0);
        CHECK(g_hc.api->unix_time_ms(g_hc.api->ctx) == 1700000000000LL);
    }

    // --- THE PERMISSION GATE ---------------------------------------------
    {
        resetAll();
        const CascadeHostClientApi hapi = makeHostClientApi();
        const CascadeTrackSourceApi tapi = makeTrackApi(false);
        LoadedPlugin p = plug("Tracker");
        p.hostClient = &hapi;
        p.trackSource = &tapi;
        int tuneCalls = 0;
        double lastTuned = 0.0;
        PluginUi ui;
        ui.setServices(workingServices(&tuneCalls, &lastTuned));
        ui.rebuild({p});

        // DENIED BY DEFAULT. A freshly installed plugin must not be able to
        // move the user's receiver.
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, 137100000.0) == CASCADE_TUNE_DENIED);
        CHECK(tuneCalls == 0);
        // Reported by MODULE, which is what the permission is keyed on.
        CHECK(ui.lastDeniedPlugin() == "Tracker.dll");

        // ...but the attempt is RECORDED, so the GUI can offer the toggle for
        // exactly the plugins that want it. A denied tracker must be
        // discoverable, not invisible.
        CHECK(ui.tuneRequesters() == std::vector<std::string>{"Tracker.dll"});
        CHECK(!ui.tuneAllowed("Tracker.dll"));

        // Granted: the request reaches the receiver.
        ui.setTuneAllowed("Tracker.dll", true);
        CHECK(ui.tuneAllowed("Tracker.dll"));
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, 137100000.0) == CASCADE_TUNE_OK);
        CHECK(tuneCalls == 1);
        CHECK(lastTuned == 137100000.0);

        // Revoked again mid-session.
        ui.setTuneAllowed("Tracker.dll", false);
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, 137100000.0) == CASCADE_TUNE_DENIED);
        CHECK(tuneCalls == 1);

        // Nonsense frequencies are refused by the host, never handed to a
        // driver. Written as a positive range test because the negation would
        // let NaN straight through.
        ui.setTuneAllowed("Tracker.dll", true);
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, std::nan("")) == CASCADE_TUNE_OUT_OF_RANGE);
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, -1.0) == CASCADE_TUNE_OUT_OF_RANGE);
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, 0.0) == CASCADE_TUNE_OUT_OF_RANGE);
        CHECK(tuneCalls == 1);
    }

    // --- permission is PER PLUGIN ----------------------------------------
    {
        resetAll();
        const CascadeHostClientApi hapi = makeHostClientApi();
        const CascadeTrackSourceApi tapi = makeTrackApi(false);
        LoadedPlugin a = plug("Allowed");
        a.hostClient = &hapi;
        a.trackSource = &tapi;
        PluginUi ui;
        int tuneCalls = 0;
        double lastTuned = 0.0;
        ui.setServices(workingServices(&tuneCalls, &lastTuned));
        ui.rebuild({a});
        ui.setTuneAllowed("Allowed.dll", true);
        CHECK(ui.tuneAllowed("Allowed.dll"));
        // A different module is NOT covered by that grant — nor is the display
        // name, which is not the key at all.
        CHECK(!ui.tuneAllowed("Other.dll"));
        CHECK(!ui.tuneAllowed("Allowed"));
    }

    // --- a grant belongs to the MODULE, not to the name it prints --------
    {
        // A plugin's display name comes out of its own descriptor, so a
        // hostile plugin can call itself whatever a granted one calls itself.
        // Keyed on that name, dropping such a file into the plugins directory
        // would inherit permission to move the user's receiver.
        resetAll();
        const CascadeHostClientApi hapi = makeHostClientApi();
        LoadedPlugin good = plugAt("Tracker", "C:/plugins/sattrack.dll");
        good.hostClient = &hapi;
        LoadedPlugin impostor = plugAt("Tracker", "C:/plugins/impostor.dll");
        impostor.hostClient = &hapi;

        int tuneCalls = 0;
        double lastTuned = 0.0;
        PluginUi ui;
        ui.setServices(workingServices(&tuneCalls, &lastTuned));
        ui.rebuild({good, impostor});
        CHECK(g_attached.size() == 2u);

        // The real tracker asks and is refused, which is what puts it on the
        // settings row; the user then grants exactly the identity the host
        // recorded, as the checkbox does.
        CHECK(requestTuneFrom(0, 137100000.0) == CASCADE_TUNE_DENIED);
        CHECK(ui.tuneRequesters().size() == 1u);
        ui.setTuneAllowed(requesterAt(ui.tuneRequesters(), 0), true);

        // The granted module tunes...
        CHECK(requestTuneFrom(0, 137100000.0) == CASCADE_TUNE_OK);
        // ...and the one that merely shares its name does not.
        CHECK(requestTuneFrom(1, 145800000.0) == CASCADE_TUNE_DENIED);
        CHECK(tuneCalls == 1);
        CHECK(lastTuned == 137100000.0);

        // The key is the module file name, which the plugin cannot change by
        // renaming itself.
        CHECK(PluginUi::tuneKey(good) == "sattrack.dll");
        CHECK(PluginUi::tuneKey(impostor) == "impostor.dll");
        CHECK(ui.tuneAllowed("sattrack.dll"));
        CHECK(!ui.tuneAllowed("impostor.dll"));
    }

    // --- with no services installed, tuning fails cleanly ----------------
    {
        resetAll();
        const CascadeHostClientApi hapi = makeHostClientApi();
        const CascadeTrackSourceApi tapi = makeTrackApi(false);
        LoadedPlugin p = plug("Tracker");
        p.hostClient = &hapi;
        p.trackSource = &tapi;
        PluginUi ui;  // setServices never called
        ui.rebuild({p});
        ui.setTuneAllowed("Tracker.dll", true);
        CHECK(g_hc.api->request_tune(g_hc.api->ctx, 137100000.0) == CASCADE_TUNE_NO_DEVICE);
        CHECK(g_hc.api->centre_hz(g_hc.api->ctx) == 0.0);
    }

    // --- lifetime: everything destroyed exactly once ---------------------
    {
        resetAll();
        const CascadeTrackSourceApi tapi = makeTrackApi(false);
        const CascadePanelApi papi = makePanelApi();
        LoadedPlugin p = plug("Both");
        p.trackSource = &tapi;
        p.panel = &papi;
        {
            PluginUi ui;
            ui.rebuild({p});
            CHECK(g_tr.created == 1);
            CHECK(g_pn.created == 1);
            ui.rebuild({p});  // a rescan
            CHECK(g_tr.destroyed == 1);
            CHECK(g_pn.destroyed == 1);
            ui.clear();
            CHECK(g_tr.destroyed == 2);
            CHECK(g_pn.destroyed == 2);
            ui.clear();  // idempotent
            CHECK(g_tr.destroyed == 2);
        }
        CHECK(g_tr.created == g_tr.destroyed);
        CHECK(g_pn.created == g_pn.destroyed);
    }

    // --- polling with nothing loaded is safe -----------------------------
    {
        resetAll();
        PluginUi ui;
        ui.poll();
        CHECK(ui.tracks().empty());
        CHECK(ui.panels().empty());
    }

    // --- the staleness rule ----------------------------------------------
    // The ABI promises "the host fades and eventually drops stale targets".
    // Nothing did, so a plugin that never evicts - the shipped ADS-B decoder
    // never does - kept every aircraft it had ever heard on the map and in the
    // list forever. These cases pin the promise.
    using cascade::core::trackPresentation;
    using cascade::core::visibleTrackCount;

    // Fresh is fresh, for every kind the ABI names and for one it does not.
    {
        const std::uint32_t kinds[] = {CASCADE_TRACK_AIRCRAFT, CASCADE_TRACK_VESSEL,
                                       CASCADE_TRACK_STATION,  CASCADE_TRACK_SATELLITE,
                                       CASCADE_TRACK_UNKNOWN,  9999u};
        for (std::uint32_t k : kinds) {
            const auto p = trackPresentation(0ull, k);
            CHECK(p.visible);
            CHECK_NEAR(p.alpha, 1.0f, 1e-6);
        }
    }

    // AIRCRAFT: ADS-B positions arrive twice a second, so 30 s of silence is
    // already anomalous and 60 s is what dump1090 itself calls gone.
    {
        CHECK(trackPresentation(29999ull, CASCADE_TRACK_AIRCRAFT).visible);
        CHECK_NEAR(trackPresentation(29999ull, CASCADE_TRACK_AIRCRAFT).alpha, 1.0f, 1e-6);
        // Exactly at the fade threshold the ramp has not moved yet: the fade
        // BEGINS here, it does not jump.
        CHECK_NEAR(trackPresentation(30000ull, CASCADE_TRACK_AIRCRAFT).alpha, 1.0f, 1e-6);
        // Half way between fade and drop, half way down the ramp.
        CHECK_NEAR(trackPresentation(45000ull, CASCADE_TRACK_AIRCRAFT).alpha, 0.65f, 1e-3);
        CHECK(trackPresentation(59999ull, CASCADE_TRACK_AIRCRAFT).visible);
        CHECK_NEAR(trackPresentation(59999ull, CASCADE_TRACK_AIRCRAFT).alpha, 0.30f, 1e-3);
        // The boundary is inclusive: at the drop threshold it is gone.
        CHECK(!trackPresentation(60000ull, CASCADE_TRACK_AIRCRAFT).visible);
        CHECK(!trackPresentation(60001ull, CASCADE_TRACK_AIRCRAFT).visible);
        CHECK(!trackPresentation(3600000ull, CASCADE_TRACK_AIRCRAFT).visible);
    }

    // VESSEL: AIS Class B may legitimately transmit only every 3 minutes, so
    // the aircraft rule applied to a ship would delete a ship behaving
    // normally. This case is what proves the thresholds really are per kind.
    {
        CHECK(trackPresentation(60000ull, CASCADE_TRACK_VESSEL).visible);
        CHECK_NEAR(trackPresentation(60000ull, CASCADE_TRACK_VESSEL).alpha, 1.0f, 1e-6);
        CHECK_NEAR(trackPresentation(299999ull, CASCADE_TRACK_VESSEL).alpha, 1.0f, 1e-6);
        CHECK(trackPresentation(599999ull, CASCADE_TRACK_VESSEL).visible);
        CHECK(!trackPresentation(600000ull, CASCADE_TRACK_VESSEL).visible);
    }

    // STATION: an APRS digipeater beacons every 10-30 minutes and has not
    // moved in between; being quiet is its normal condition.
    {
        CHECK_NEAR(trackPresentation(1799999ull, CASCADE_TRACK_STATION).alpha, 1.0f, 1e-6);
        CHECK(trackPresentation(3599999ull, CASCADE_TRACK_STATION).visible);
        CHECK(!trackPresentation(3600000ull, CASCADE_TRACK_STATION).visible);
    }

    // SATELLITE: propagated, not heard, so it should update every frame.
    {
        CHECK_NEAR(trackPresentation(119999ull, CASCADE_TRACK_SATELLITE).alpha, 1.0f, 1e-6);
        CHECK(trackPresentation(599999ull, CASCADE_TRACK_SATELLITE).visible);
        CHECK(!trackPresentation(600000ull, CASCADE_TRACK_SATELLITE).visible);
    }

    // AN UNKNOWN KIND GETS THE MOST FORGIVING RULE, never the strictest: the
    // host cannot know a future kind's cadence, and erasing a target that was
    // reporting perfectly is the worse of the two mistakes.
    {
        CHECK(trackPresentation(60000ull, CASCADE_TRACK_UNKNOWN).visible);
        CHECK_NEAR(trackPresentation(60000ull, CASCADE_TRACK_UNKNOWN).alpha, 1.0f, 1e-6);
        CHECK(trackPresentation(3599999ull, 9999u).visible);
        CHECK(!trackPresentation(3600000ull, 9999u).visible);
    }

    // ALPHA IS MONOTONIC NON-INCREASING and stays in range. A fade that
    // brightened again as a target went quieter would say the opposite of what
    // it means, and one that reached zero before the drop would make targets
    // vanish with no warning.
    {
        const std::uint32_t kinds[] = {CASCADE_TRACK_AIRCRAFT, CASCADE_TRACK_VESSEL,
                                       CASCADE_TRACK_STATION, CASCADE_TRACK_SATELLITE};
        for (std::uint32_t k : kinds) {
            float prev = 2.0f;
            int stepsVisible = 0;
            for (std::uint64_t ms = 0ull; ms <= 3600000ull; ms += 1000ull) {
                const auto p = trackPresentation(ms, k);
                if (!p.visible) { continue; }
                ++stepsVisible;
                CHECK(p.alpha <= prev + 1e-6f);
                CHECK(p.alpha >= 0.30f - 1e-6f);
                CHECK(p.alpha <= 1.0f + 1e-6f);
                prev = p.alpha;
            }
            CHECK(stepsVisible > 0);
        }
    }

    // RE-ACQUISITION IS AUTOMATIC. The rule is a pure function of the age the
    // plugin reports, so a target that was dropped reappears on the very frame
    // its plugin hears it again - the host keeps no "dropped" memory to undo.
    {
        std::vector<cascade::core::HostTrack> v(1);
        std::snprintf(v[0].t.id, CASCADE_TRACK_ID_CHARS, "ABC123");
        v[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        v[0].t.ageMs = 120000ull;  // two minutes silent: dropped
        CHECK(visibleTrackCount(v) == 0u);
        v[0].t.ageMs = 250ull;  // a new message arrives
        CHECK(visibleTrackCount(v) == 1u);
        CHECK(trackPresentation(v[0].t.ageMs, v[0].t.kind).visible);
        CHECK_NEAR(trackPresentation(v[0].t.ageMs, v[0].t.kind).alpha, 1.0f, 1e-6);
    }

    // The count the UI shows must be the count of what it draws.
    {
        std::vector<cascade::core::HostTrack> v(4);
        v[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        v[0].t.ageMs = 0ull;  // shown
        v[1].t.kind = CASCADE_TRACK_AIRCRAFT;
        v[1].t.ageMs = 45000ull;  // faded but shown
        v[2].t.kind = CASCADE_TRACK_AIRCRAFT;
        v[2].t.ageMs = 90000ull;  // dropped
        v[3].t.kind = CASCADE_TRACK_VESSEL;
        v[3].t.ageMs = 90000ull;  // a ship at the same age is fine
        CHECK(visibleTrackCount(v) == 3u);
        CHECK(visibleTrackCount({}) == 0u);
    }

    // --- the same rule, applied to a TRAIL --------------------------------
    // A path has no age of its own, so it inherits its owner's. Without this
    // the marker obeys the staleness rule and the line under it does not: the
    // dropped target's trail was still drawn, at full strength, starting where
    // the missing marker would have been. Reproduced on the running app with a
    // probe plugin publishing a four-point trail owned by an aircraft at
    // ageMs 61000.
    using cascade::core::HostPath;
    using cascade::core::pathPresentation;
    {
        std::vector<cascade::core::HostTrack> tr(2);
        std::snprintf(tr[0].t.id, CASCADE_TRACK_ID_CHARS, "A00");
        tr[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        tr[0].t.ageMs = 0ull;  // fresh
        tr[0].plugin = "probe";
        std::snprintf(tr[1].t.id, CASCADE_TRACK_ID_CHARS, "A61");
        tr[1].t.kind = CASCADE_TRACK_AIRCRAFT;
        tr[1].t.ageMs = 61000ull;  // dropped
        tr[1].plugin = "probe";

        // The owner is fresh: the trail is drawn exactly as before.
        HostPath fresh;
        fresh.id = "A00";
        fresh.plugin = "probe";
        fresh.kind = CASCADE_TRACK_AIRCRAFT;
        CHECK(pathPresentation(fresh, tr).visible);
        CHECK_NEAR(pathPresentation(fresh, tr).alpha, 1.0f, 1e-6);

        // The owner has been dropped: so has the trail.
        HostPath dropped;
        dropped.id = "A61";
        dropped.plugin = "probe";
        dropped.kind = CASCADE_TRACK_AIRCRAFT;
        CHECK(!pathPresentation(dropped, tr).visible);

        // NO OWNER AT ALL keeps today's behaviour. A source may plot a line
        // that is not a target - a footprint, a predicted track - and the host
        // has no age for it and no business hiding it.
        HostPath orphan;
        orphan.id = "NOSUCH";
        orphan.plugin = "probe";
        orphan.kind = CASCADE_TRACK_SATELLITE;
        CHECK(pathPresentation(orphan, tr).visible);
        CHECK_NEAR(pathPresentation(orphan, tr).alpha, 1.0f, 1e-6);

        // THE PLUGIN IS PART OF THE IDENTITY. Two sources may both use an
        // ICAO address or an MMSI as an id, and one plugin's silence must not
        // erase another plugin's trail.
        HostPath otherPlugin;
        otherPlugin.id = "A61";
        otherPlugin.plugin = "somebody-else";
        otherPlugin.kind = CASCADE_TRACK_AIRCRAFT;
        CHECK(pathPresentation(otherPlugin, tr).visible);

        // A merely QUIET owner fades its trail by the same fraction, so the
        // line and the marker dim together rather than the line staying bright
        // over a marker that is nearly gone.
        std::vector<cascade::core::HostTrack> quiet(1);
        std::snprintf(quiet[0].t.id, CASCADE_TRACK_ID_CHARS, "A45");
        quiet[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        quiet[0].t.ageMs = 45000ull;
        quiet[0].plugin = "probe";
        HostPath faded;
        faded.id = "A45";
        faded.plugin = "probe";
        faded.kind = CASCADE_TRACK_AIRCRAFT;
        CHECK(pathPresentation(faded, quiet).visible);
        CHECK_NEAR(pathPresentation(faded, quiet).alpha,
                   trackPresentation(45000ull, CASCADE_TRACK_AIRCRAFT).alpha, 1e-6);

        // THE OWNER'S KIND DECIDES, not the path's. A plugin that mislabels a
        // trail's kind must not thereby buy its owner a more forgiving rule:
        // a vessel quiet for 61 s is fine even where an aircraft would be
        // faded, and the trail follows the vessel.
        std::vector<cascade::core::HostTrack> ship(1);
        std::snprintf(ship[0].t.id, CASCADE_TRACK_ID_CHARS, "V61");
        ship[0].t.kind = CASCADE_TRACK_VESSEL;
        ship[0].t.ageMs = 61000ull;
        ship[0].plugin = "probe";
        HostPath shipTrail;
        shipTrail.id = "V61";
        shipTrail.plugin = "probe";
        shipTrail.kind = CASCADE_TRACK_AIRCRAFT;  // wrong on purpose
        CHECK(pathPresentation(shipTrail, ship).visible);
        CHECK_NEAR(pathPresentation(shipTrail, ship).alpha, 1.0f, 1e-6);

        // No tracks at all: nothing to inherit from, so nothing is hidden.
        CHECK(pathPresentation(dropped, {}).visible);
    }

    // --- what the map window may OPEN ITSELF for ---------------------------
    // The map opens on its own the first time a plugin has something to show.
    // Deciding that from the RAW track list rather than the visible one made
    // the window impossible to close: a source that never evicts (the shipped
    // ADS-B decoder does not) keeps reporting dropped targets, so the demand
    // was re-asserted on the very next frame after the user pressed the close
    // button. Reproduced on the running application with a probe reporting one
    // aircraft at ageMs 3600000: the map opened itself, read "0 targets", and
    // the close button - confirmed to highlight under the cursor - did nothing.
    using cascade::core::anyVisibleTarget;
    {
        std::vector<cascade::core::HostTrack> stale(2);
        std::snprintf(stale[0].t.id, CASCADE_TRACK_ID_CHARS, "A1");
        stale[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        stale[0].t.ageMs = 3600000ull;  // the probe's value
        stale[0].plugin = "probe";
        std::snprintf(stale[1].t.id, CASCADE_TRACK_ID_CHARS, "A2");
        stale[1].t.kind = CASCADE_TRACK_AIRCRAFT;
        stale[1].t.ageMs = 60000ull;  // exactly at the drop threshold
        stale[1].plugin = "probe";

        // Nothing at all: no demand, which is the state at start-up.
        CHECK(!anyVisibleTarget({}, {}));
        // THE DEFECT: reported targets that will not be drawn are not a
        // reason to open, or to re-open, the window.
        CHECK(!anyVisibleTarget(stale, {}));
        // One fresh target among them is.
        std::vector<cascade::core::HostTrack> mixed = stale;
        mixed.push_back(cascade::core::HostTrack{});
        std::snprintf(mixed[2].t.id, CASCADE_TRACK_ID_CHARS, "A3");
        mixed[2].t.kind = CASCADE_TRACK_AIRCRAFT;
        mixed[2].t.ageMs = 0ull;
        mixed[2].plugin = "probe";
        CHECK(anyVisibleTarget(mixed, {}));
        // And a merely faded one still counts: it is still drawn.
        std::vector<cascade::core::HostTrack> faded1(1);
        std::snprintf(faded1[0].t.id, CASCADE_TRACK_ID_CHARS, "A4");
        faded1[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        faded1[0].t.ageMs = 45000ull;
        faded1[0].plugin = "probe";
        CHECK(anyVisibleTarget(faded1, {}));

        // PATHS OBEY THE SAME RULE THEY ARE DRAWN BY. A trail whose owner has
        // been dropped is not drawn, so it is not a reason to open the window
        // either - otherwise the paths half of the test reintroduces exactly
        // the defect the tracks half just fixed.
        HostPath ownedByStale;
        ownedByStale.id = "A1";
        ownedByStale.plugin = "probe";
        ownedByStale.kind = CASCADE_TRACK_AIRCRAFT;
        CHECK(!anyVisibleTarget(stale, {ownedByStale}));

        // An ORPHAN path is drawn (pathPresentation says so), so it is a
        // reason to open: a footprint or a predicted ground track with no
        // owning target is still something on the map.
        HostPath orphanPath;
        orphanPath.id = "NOSUCH";
        orphanPath.plugin = "probe";
        orphanPath.kind = CASCADE_TRACK_SATELLITE;
        CHECK(anyVisibleTarget(stale, {orphanPath}));
        CHECK(anyVisibleTarget({}, {orphanPath}));

        // A path owned by a FRESH track opens the window through either half.
        HostPath ownedByFresh;
        ownedByFresh.id = "A3";
        ownedByFresh.plugin = "probe";
        ownedByFresh.kind = CASCADE_TRACK_AIRCRAFT;
        CHECK(anyVisibleTarget(mixed, {ownedByFresh}));
    }

    // --- WHEN the map may open itself: the transition, not the state ---------
    // Counting only what is drawn fixed the stale case and left the ORDINARY
    // one broken. A receiver that keeps hearing its targets - which is what an
    // ADS-B receiver does all day - reports something visible on every frame,
    // so a rule that asks "is anything visible" each frame re-opened the window
    // the frame after the close button cleared it. Reproduced on the running
    // application with a probe reporting one aircraft at ageMs 0: the close
    // button highlighted under the cursor (152 blue pixels), the click landed,
    // and the map was still on screen at +2 s and +10 s.
    using cascade::core::mapSelfOpens;
    {
        // Nothing, and still nothing: the start-up state, no demand.
        CHECK(!mapSelfOpens(false, false));
        // The first target of a session - the one moment the host is entitled
        // to put the window up on the user's behalf.
        CHECK(mapSelfOpens(false, true));
        // THE DEFECT: a target that was already visible last frame is not a
        // new reason to open anything.
        CHECK(!mapSelfOpens(true, true));
        // Everything going quiet does not open the window either - and it is
        // what re-arms the next open, so a map closed by the user comes back
        // when the air has genuinely fallen silent and something new arrives.
        CHECK(!mapSelfOpens(true, false));

        // THE RULE AS THE WINDOW USES IT, over a run of frames with the same
        // live target arriving on every one of them. This is the shape of the
        // live reproduction: open by itself, then a user close that has to
        // survive every later frame.
        std::vector<cascade::core::HostTrack> live(1);
        std::snprintf(live[0].t.id, CASCADE_TRACK_ID_CHARS, "LIVE1");
        live[0].t.kind = CASCADE_TRACK_AIRCRAFT;
        live[0].t.ageMs = 0ull;
        live[0].plugin = "probe";

        bool open = false;
        bool had = false;
        for (int frame = 0; frame < 3; ++frame) {
            const bool now = anyVisibleTarget(live, {});
            if (mapSelfOpens(had, now)) { open = true; }
            had = now;
        }
        CHECK(open);  // it opened itself on the frame the target appeared

        open = false;  // the user presses the close button
        for (int frame = 0; frame < 100; ++frame) {
            const bool now = anyVisibleTarget(live, {});
            if (mapSelfOpens(had, now)) { open = true; }
            had = now;
        }
        CHECK(!open);  // and it stays closed while that target keeps arriving

        // The air goes quiet for a frame and a NEW target arrives: that is a
        // fresh transition, and the map is allowed back.
        const bool quiet = anyVisibleTarget({}, {});
        CHECK(!mapSelfOpens(had, quiet));
        had = quiet;
        const bool again = anyVisibleTarget(live, {});
        CHECK(mapSelfOpens(had, again));
    }

    // --- STOP: a stopped plugin has no instances and puts nothing on screen -
    //
    // The GUI half of the same property the runner test asserts. A stopped
    // plugin's targets, trails and panel rows have to leave with the instances
    // that produced them: a marker that stays behind is a target the host is
    // still claiming to hear from a plugin it has switched off.
    {
        resetAll();
        const CascadeTrackSourceApi trApi = makeTrackApi(true);
        const CascadePanelApi pnApi = makePanelApi();
        g_tr.emit = {track("406135", 53.98, -2.27)};
        g_tr.emitPath = true;
        g_tr.pathPts = {{10.0, 20.0}, {11.0, 21.0}};
        g_pn.rows.resize(2);

        // The module file is the key, and it is deliberately not the display
        // name - a plugin picks its own name and must not inherit another's
        // state by claiming it.
        LoadedPlugin p = plugAt("ADS-B", "C:/plugins/adsb-decoder.dll");
        p.trackSource = &trApi;
        p.panel = &pnApi;

        PluginUi ui;
        ui.rebuild({p});
        ui.poll();
        CHECK(g_tr.created == 1);
        CHECK(g_pn.created == 1);
        CHECK(ui.tracks().size() == 1u);
        CHECK(ui.paths().size() == 1u);
        CHECK(ui.panels().size() == 1u);

        ui.setStopped({"adsb-decoder.dll"});
        CHECK(ui.isStopped("adsb-decoder.dll"));
        ui.rebuild({p});
        // Both handles were handed back to the plugin, and neither was
        // recreated.
        CHECK(g_tr.destroyed == 1);
        CHECK(g_pn.destroyed == 1);
        CHECK(g_tr.created == 1);
        CHECK(g_pn.created == 1);
        // ...and polling a stopped plugin yields nothing, even though the fake
        // is still offering a target, a trail and two rows.
        ui.poll();
        CHECK(ui.tracks().empty());
        CHECK(ui.paths().empty());
        CHECK(ui.panels().empty());
        CHECK(ui.trackCount() == 0u);

        // START: the same rebuild path recreates them and the target is back.
        ui.setStopped({});
        ui.rebuild({p});
        ui.poll();
        CHECK(g_tr.created == 2);
        CHECK(g_pn.created == 2);
        CHECK(ui.tracks().size() == 1u);
        CHECK(ui.panels().size() == 1u);
    }

    // --- A STOPPED PLUGIN CANNOT MOVE THE RECEIVER -------------------------
    //
    // The half that is not obvious, and the one worth a test of its own. A
    // host bridge outlives the instance it was handed to - PluginUi frees the
    // bridges at clear(), not at rebuild(), and the ABI lets a plugin keep the
    // pointer - so a stopped plugin holding a stale bridge can still CALL
    // request_tune. With the grant ticked, that would move the radio on behalf
    // of a plugin the user switched off.
    {
        resetAll();
        const CascadeHostClientApi hcApi = makeHostClientApi();
        LoadedPlugin p = plugAt("Tracker", "C:/plugins/tracker.dll");
        p.hostClient = &hcApi;

        int tuneCalls = 0;
        double lastTuned = 0.0;
        PluginUi ui;
        ui.setServices(workingServices(&tuneCalls, &lastTuned));
        // Granted, so nothing below can pass for the permission working.
        ui.setTuneAllowed("tracker.dll", true);
        ui.rebuild({p});
        CHECK(g_hc.attaches == 1);
        CHECK(requestTuneFrom(0, 137.1e6) == CASCADE_TUNE_OK);
        CHECK(tuneCalls == 1);

        // Stopped. The bridge the plugin was given at attach is still valid
        // memory, and the plugin is free to call through it.
        ui.setStopped({"tracker.dll"});
        ui.rebuild({p});
        // Not re-attached: a stopped plugin is not told about the host at all.
        CHECK(g_hc.attaches == 1);
        CHECK(requestTuneFrom(0, 145.8e6) == CASCADE_TUNE_DENIED);
        CHECK(tuneCalls == 1);          // the receiver did not move
        CHECK(lastTuned == 137.1e6);    // still where the allowed call left it
        // The grant itself is untouched, so starting the plugin restores the
        // permission the user gave rather than making them tick it again.
        CHECK(ui.tuneAllowed("tracker.dll"));

        ui.setStopped({});
        ui.rebuild({p});
        CHECK(g_hc.attaches == 2);
        CHECK(requestTuneFrom(1, 145.8e6) == CASCADE_TUNE_OK);
        CHECK(tuneCalls == 2);
        CHECK(lastTuned == 145.8e6);
    }

    // --- The stopped set survives a rescan ---------------------------------
    {
        // rescanPlugins() calls PluginUi::clear() before unloading the
        // modules. clear() drops the tune grants on purpose (they are
        // re-applied from the config afterwards), but a stop dropped there
        // would start the plugin for the whole rebuild that follows - and a
        // stop that does not survive a rescan does not survive a launch.
        resetAll();
        const CascadeTrackSourceApi trApi = makeTrackApi(false);
        g_tr.emit = {track("406135", 53.98, -2.27)};
        LoadedPlugin p = plugAt("ADS-B", "C:/plugins/adsb-decoder.dll");
        p.trackSource = &trApi;

        PluginUi ui;
        ui.setStopped({"adsb-decoder.dll"});
        ui.clear();
        ui.rebuild({p});
        ui.poll();
        CHECK(ui.isStopped("adsb-decoder.dll"));
        CHECK(g_tr.created == 0);
        CHECK(ui.tracks().empty());
    }

    // --- An empty stop entry stops nothing ---------------------------------
    {
        // Same rule as the tune grant's empty key: a record with no path
        // yields "", and if "" matched, one stray entry would stop every
        // path-less plugin at once.
        resetAll();
        const CascadeTrackSourceApi trApi = makeTrackApi(false);
        g_tr.emit = {track("406135", 53.98, -2.27)};
        LoadedPlugin p = plugAt("Pathless", "");
        p.trackSource = &trApi;

        PluginUi ui;
        ui.setStopped({""});
        CHECK(!ui.isStopped(""));
        ui.rebuild({p});
        ui.poll();
        CHECK(g_tr.created == 1);
        CHECK(ui.tracks().size() == 1u);
    }

    // --- Audio mute policy -------------------------------------------------
    //
    // Pure functions over snapshots: no plugin is loaded, no radio exists, and
    // that is the point - the rule that decides whether the speakers go quiet
    // has to be checkable without either.
    {
        using cascade::core::kPresetToleranceHz;
        using cascade::core::MuteDecision;
        using cascade::core::MutePlugin;
        using cascade::core::MutePreset;
        using cascade::core::anyStillRunning;
        using cascade::core::muteActive;
        using cascade::core::muteDefaultForCaps;
        using cascade::core::onPreset;
        using cascade::core::TunePoint;
        using cascade::core::tuneAwayEdge;

        // The default comes from what the plugin CONSUMES, nothing else.
        CHECK(muteDefaultForCaps(CASCADE_CAP_IQ_DECODER));
        CHECK(muteDefaultForCaps(CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET |
                                 CASCADE_CAP_TRACK_SOURCE));
        CHECK(!muteDefaultForCaps(CASCADE_CAP_DECODER));
        CHECK(!muteDefaultForCaps(CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PRESET));
        CHECK(!muteDefaultForCaps(0u));

        // --- onPreset: the tolerance edges, both kinds of preset ------------
        MutePreset adsb;
        adsb.frequencyHz = 1090.0e6;
        adsb.deviceCentre = true;  // an I/Q decoder wants the BAND, not the VFO

        TunePoint t;
        t.deviceCentreHz = 1090.0e6;
        t.tunedHz = 1090.0e6;
        CHECK(onPreset(adsb, t));

        // Exactly on the tolerance is ON (<=, not <), and one hertz beyond it
        // is off. Asserted from both sides so a change to either the bound or
        // the comparison shows up here rather than as a mute that will not let
        // go.
        t.deviceCentreHz = 1090.0e6 + kPresetToleranceHz;
        CHECK(onPreset(adsb, t));
        t.deviceCentreHz = 1090.0e6 - kPresetToleranceHz;
        CHECK(onPreset(adsb, t));
        t.deviceCentreHz = 1090.0e6 + kPresetToleranceHz + 1.0;
        CHECK(!onPreset(adsb, t));
        t.deviceCentreHz = 1090.0e6 - kPresetToleranceHz - 1.0;
        CHECK(!onPreset(adsb, t));

        // A DEVICE-CENTRE preset ignores the VFO offset. This is the case that
        // would silently break the feature for anyone who had dragged the VFO:
        // pressing the preset lands the device exactly on it, and reading the
        // tuned frequency instead would answer "not on the preset" the instant
        // the button was pressed.
        t.deviceCentreHz = 1090.0e6;
        t.tunedHz = 1090.0e6 + 90000.0;  // 90 kHz of VFO offset
        CHECK(onPreset(adsb, t));

        // ...and a VFO preset is the mirror image: the offset is exactly what
        // it is about.
        MutePreset aprs;
        aprs.frequencyHz = 144.8e6;
        aprs.deviceCentre = false;
        t.deviceCentreHz = 144.7e6;
        t.tunedHz = 144.8e6;
        CHECK(onPreset(aprs, t));
        t.tunedHz = 144.7e6;
        CHECK(!onPreset(aprs, t));

        // A declared passband WIDENS the window; it never narrows it below the
        // floor, or a narrow-channel preset would be impossible to sit on.
        MutePreset wide;
        wide.frequencyHz = 137.5e6;
        wide.bandwidthHz = 40000.0;  // half-width 20 kHz > the 5 kHz floor
        t.deviceCentreHz = 137.5e6;
        t.tunedHz = 137.5e6 + 19000.0;
        CHECK(onPreset(wide, t));
        t.tunedHz = 137.5e6 + 21000.0;
        CHECK(!onPreset(wide, t));

        MutePreset narrow;
        narrow.frequencyHz = 137.5e6;
        narrow.bandwidthHz = 2000.0;  // half-width 1 kHz < the floor
        t.tunedHz = 137.5e6 + 3000.0;
        CHECK(onPreset(narrow, t));   // the FLOOR still applies

        // A preset with no frequency is not a place. Without this a plugin
        // that filled a CascadePreset badly would mute the radio wherever it
        // was pointed, which is the worst failure this feature can have.
        MutePreset empty;
        t.tunedHz = 0.0;
        t.deviceCentreHz = 0.0;
        CHECK(!onPreset(empty, t));

        // --- muteActive: running, setting, preset - all three required -----
        const auto plug = [](const char* key, const char* name, bool running,
                             bool mutes, double presetHz, bool devCentre) {
            MutePlugin m;
            m.key = key;
            m.name = name;
            m.running = running;
            m.mutes = mutes;
            MutePreset ps;
            ps.frequencyHz = presetHz;
            ps.deviceCentre = devCentre;
            m.presets.push_back(ps);
            return m;
        };

        TunePoint on1090;
        on1090.deviceCentreHz = 1090.0e6;
        on1090.tunedHz = 1090.0e6;
        TunePoint onFm;
        onFm.deviceCentreHz = 100.1e6;
        onFm.tunedHz = 100.1e6;

        {
            const std::vector<MutePlugin> ps = {
                plug("adsb.dll", "ADS-B decoder", true, true, 1090.0e6, true)};
            const MuteDecision d = muteActive(ps, on1090);
            CHECK(d.active);
            CHECK(d.names == std::vector<std::string>({"ADS-B decoder"}));
            CHECK(d.keys == std::vector<std::string>({"adsb.dll"}));

            // Tuned away: nothing is muting, but the plugin that WAS muting is
            // still running - which is the pair of facts the popup needs.
            const MuteDecision away = muteActive(ps, onFm);
            CHECK(!away.active);
            CHECK(away.names.empty());
            CHECK(anyStillRunning(ps, d.keys));
        }
        {
            // STOPPED: on its own preset and silent about it.
            const std::vector<MutePlugin> ps = {
                plug("adsb.dll", "ADS-B decoder", false, true, 1090.0e6, true)};
            const MuteDecision d = muteActive(ps, on1090);
            CHECK(!d.active);
            // ...and nothing to ask about: a stop is not a tune-away.
            CHECK(!anyStillRunning(ps, std::vector<std::string>({"adsb.dll"})));
        }
        {
            // OVERRIDDEN OFF: running on its preset and still audible.
            const std::vector<MutePlugin> ps = {
                plug("adsb.dll", "ADS-B decoder", true, false, 1090.0e6, true)};
            const MuteDecision d = muteActive(ps, on1090);
            CHECK(!d.active);
            CHECK(!anyStillRunning(ps, std::vector<std::string>({"adsb.dll"})));
        }
        {
            // A NON-I/Q plugin at its default (off) - SSTV on 14.230 MHz, the
            // case where the tones are the point.
            const std::vector<MutePlugin> ps = {
                plug("sstv.dll", "SSTV decoder", true,
                     muteDefaultForCaps(CASCADE_CAP_IMAGE_DECODER), 14.230e6, false)};
            TunePoint sstv;
            sstv.deviceCentreHz = 14.230e6;
            sstv.tunedHz = 14.230e6;
            CHECK(!muteActive(ps, sstv).active);
        }
        {
            // SEVERAL muting plugins: every one that is actually on a preset
            // is named, in list order, and one that is not does not creep in.
            std::vector<MutePlugin> ps = {
                plug("adsb.dll", "ADS-B decoder", true, true, 1090.0e6, true),
                plug("ais.dll", "AIS decoder", true, true, 1090.0e6, true),
                plug("pocsag.dll", "POCSAG decoder", true, true, 153.35e6, false)};
            const MuteDecision d = muteActive(ps, on1090);
            CHECK(d.active);
            CHECK(d.names == std::vector<std::string>({"ADS-B decoder", "AIS decoder"}));
            CHECK(d.keys == std::vector<std::string>({"adsb.dll", "ais.dll"}));
        }
        {
            // A plugin with SEVERAL presets is on preset if it is on any of
            // them - APRS is a different frequency in every region.
            MutePlugin m;
            m.key = "aprs.dll";
            m.name = "APRS decoder";
            m.running = true;
            m.mutes = true;
            MutePreset eu;
            eu.frequencyHz = 144.8e6;
            MutePreset na;
            na.frequencyHz = 144.39e6;
            m.presets = {eu, na};
            TunePoint na1;
            na1.deviceCentreHz = 144.39e6;
            na1.tunedHz = 144.39e6;
            CHECK(muteActive({m}, na1).active);
            TunePoint eu1;
            eu1.deviceCentreHz = 144.8e6;
            eu1.tunedHz = 144.8e6;
            CHECK(muteActive({m}, eu1).active);
            TunePoint neither;
            neither.deviceCentreHz = 145.5e6;
            neither.tunedHz = 145.5e6;
            CHECK(!muteActive({m}, neither).active);
        }
        {
            // A muting plugin with NO presets can never mute anything: there
            // is no frequency it is "on", so the audio is never taken away by
            // a plugin the user cannot tune to.
            MutePlugin m;
            m.key = "nopreset.dll";
            m.name = "Something";
            m.running = true;
            m.mutes = true;
            CHECK(!muteActive({m}, on1090).active);
        }
        // Nothing installed at all.
        CHECK(!muteActive({}, on1090).active);
        CHECK(muteActive({}, on1090).names.empty());

        // --- anyStillRunning: a tune-away is not a stop --------------------
        //
        // The case measured on the running application: two muting plugins,
        // one of them stopped from its own row while parked on its preset. The
        // OTHER one is running and mutes, so a question phrased as "is any
        // muting plugin running" answers yes and raises a dialog about the
        // plugin that has just been switched off.
        {
            const std::vector<MutePlugin> both = {
                plug("adsb.dll", "ADS-B decoder", false, true, 1090.0e6, true),
                plug("ais.dll", "AIS decoder", true, true, 162.0e6, true)};
            CHECK(!anyStillRunning(both, std::vector<std::string>({"adsb.dll"})));
            CHECK(anyStillRunning(both, std::vector<std::string>({"ais.dll"})));
            CHECK(anyStillRunning(both,
                                  std::vector<std::string>({"adsb.dll", "ais.dll"})));
        }
        // An empty list asks about nothing, and an empty KEY must match
        // nothing - the same rule the stop set applies to a path-less record.
        {
            const std::vector<MutePlugin> ps = {
                plug("", "Pathless", true, true, 1090.0e6, true)};
            CHECK(!anyStillRunning(ps, {}));
            CHECK(!anyStillRunning(ps, std::vector<std::string>({""})));
        }
        // A key that matches no installed plugin - the plugin was removed
        // while it was muting - is not "still running".
        CHECK(!anyStillRunning(
            std::vector<MutePlugin>({plug("ais.dll", "AIS decoder", true, true, 162.0e6,
                                          true)}),
            std::vector<std::string>({"gone.dll"})));

        // --- tuneAwayEdge --------------------------------------------------
        // Only the falling edge, which is what stops a modal being reopened on
        // every frame of a tune that stays off-preset.
        CHECK(tuneAwayEdge(true, false));
        CHECK(!tuneAwayEdge(false, false));
        CHECK(!tuneAwayEdge(true, true));
        CHECK(!tuneAwayEdge(false, true));
    }

    // --- The tune-away popup's SUBJECT is captured, not re-read ------------
    //
    // MEASURED ON THE RUNNING APPLICATION, which is why this is a test and not
    // a comment. With AIS and ADS-B both running, tuning off 162 MHz opened the
    // dialog saying "AIS is still running and is muting the audio"; tuning from
    // there to 1090 MHz left the SAME open dialog re-labelled "ADS-B is still
    // running..." while the receiver sat exactly on ADS-B's own preset -
    // nagging in the one place the design says it must never nag, and offering
    // to switch off the decoder the user had just tuned to. The desktop's
    // controls are behind the modal, but the web API, CAT, a plugin's
    // request_tune and the scanner all move the radio from outside it, and the
    // scanner does it unattended.
    {
        using cascade::core::advanceMutePopup;
        using cascade::core::MutePopupSubject;

        const std::vector<std::string> aisName = {"AIS"};
        const std::vector<std::string> aisKey = {"ais.dll"};
        const std::vector<std::string> adsbName = {"ADS-B"};
        const std::vector<std::string> adsbKey = {"adsb.dll"};

        // The edge opens it, naming what was muting.
        const MutePopupSubject opened =
            advanceMutePopup(MutePopupSubject{}, false, true, true, aisName, aisKey);
        CHECK(opened.open);
        CHECK(opened.names == aisName);
        CHECK(opened.keys == aisKey);

        // A LATER FRAME OFF-PRESET, with a different plugin now the live
        // answer, must not move the question onto it. This is the defect
        // itself.
        const MutePopupSubject still =
            advanceMutePopup(opened, false, false, true, adsbName, adsbKey);
        CHECK(still.open);
        CHECK(still.names == aisName);
        CHECK(still.keys == aisKey);

        // ...and ARRIVING ON A PRESET withdraws it outright, whatever is muting
        // there. On a preset the mute is explained by where the radio is
        // pointed and the Sinks panel says so; there is nothing left to ask.
        const MutePopupSubject onPresetNow =
            advanceMutePopup(opened, true, false, true, adsbName, adsbKey);
        CHECK(!onPresetNow.open);
        CHECK(onPresetNow.names.empty());
        CHECK(onPresetNow.keys.empty());

        // The plugin it named being stopped from somewhere else - its own row,
        // the browser - also withdraws it: a dialog offering to stop something
        // already stopped is a button that does nothing.
        const MutePopupSubject gone =
            advanceMutePopup(opened, false, false, false, aisName, aisKey);
        CHECK(!gone.open);
        CHECK(gone.names.empty());

        // An edge with nothing still running opens nothing - that is a STOP,
        // not a tune-away, and the two are what anyStillRunning tells apart.
        const MutePopupSubject notOpened =
            advanceMutePopup(MutePopupSubject{}, false, true, false, aisName, aisKey);
        CHECK(!notOpened.open);
        CHECK(notOpened.names.empty());

        // A closed popup stays closed on an ordinary off-preset frame: no edge,
        // no popup, however long the radio sits away from every preset.
        const MutePopupSubject closedStays =
            advanceMutePopup(MutePopupSubject{}, false, false, true, aisName, aisKey);
        CHECK(!closedStays.open);

        // RE-ARMING, end to end: withdrawn on the preset, and the next tune
        // away asks again - about whatever is muting THEN.
        const MutePopupSubject rearmed =
            advanceMutePopup(onPresetNow, false, true, true, adsbName, adsbKey);
        CHECK(rearmed.open);
        CHECK(rearmed.names == adsbName);
        CHECK(rearmed.keys == adsbKey);

        // Several plugins muting at once: all of them are captured, and the
        // capture is still frozen against a later frame that names one.
        const std::vector<std::string> bothNames = {"ADS-B", "AIS"};
        const std::vector<std::string> bothKeys = {"adsb.dll", "ais.dll"};
        const MutePopupSubject two =
            advanceMutePopup(MutePopupSubject{}, false, true, true, bothNames, bothKeys);
        CHECK(two.names == bothNames);
        CHECK(two.keys == bothKeys);
        CHECK(advanceMutePopup(two, false, false, true, aisName, aisKey).names ==
              bothNames);
    }

    return testSummary("test_plugin_ui");
}
