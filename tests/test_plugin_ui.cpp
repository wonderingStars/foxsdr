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

    return testSummary("test_plugin_ui");
}
