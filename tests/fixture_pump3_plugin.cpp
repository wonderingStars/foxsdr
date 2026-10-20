// fixture_pump3_plugin.cpp - an INSTRUMENTED track source that is also a host client, for the tests of the plugin UI's thread
// ownership (the pump move, step P3: tests/test_plugin_directive.cpp's siblings test_plugin_ui_threaded.cpp and
// test_plugin_unload_handshake.cpp).
//
// WHAT IT IS FOR. The plugin UI is the window's: its instances are created, polled and destroyed on the window's thread, and the
// engine's control thread never reaches them - it posts a directive, and unmaps the module only after the window says it has let
// go. A test of that needs a module that can SAY what was done to it, and from which thread:
//
//   - every handle it hands out is REGISTERED, and each poll or destroy of a handle that is not live is counted (a poll or a
//     destroy through a handle the host has already destroyed: the stale reference the design exists to prevent);
//   - destroy() entered while any poll is in progress on ANOTHER thread is counted (the rebuild-under-a-poll the old design
//     allowed), and so is create() or destroy() running on a thread other than the one that polls;
//   - poll_tracks can be HELD inside the module (a gate), so a test has the interface thread demonstrably inside plugin code
//     while the engine does what it does;
//   - poll_tracks can Doppler-churn: each poll calls the host's request_tune with a frequency that moves, from the polling
//     thread, and records what the host answered (the legacy tune, queued since the step);
//   - when the module is UNMAPPED (its static destructor runs) it writes how many handles were still alive to the file named by
//     FIXTURE_P3_UNMAP_LOG - the only observation of "live handles at FreeLibrary" that is not made from inside the module's own
//     image, because after the unmap nothing of it can be asked.
//
// It also declares a BASEMAP and a TRACK_INFO table (nothing of either does any work), so a test can see WHICH THREAD created and
// destroyed them: the basemap's tiles are GL textures, and the window's release of both must happen on the window's thread (the
// engine's pass used to let go of them through host hooks).
//
// The track source reports three aircraft whose positions move with the poll count, so a reader that holds a reference into the
// host's track vector across a rebuild reads different data, not merely the same data twice. Descriptor name "Pump3 Fixture".
//
// It is not a test (the tests/ glob builds only test_*.cpp as tests) and it ships nowhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_abi.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace {

std::atomic<long> g_creates{0};
std::atomic<long> g_destroys{0};
std::atomic<long> g_polls{0};
std::atomic<long> g_attaches{0};
std::atomic<long> g_staleCalls{0};         // a poll or a destroy through a handle that is not live
std::atomic<long> g_destroyDuringPoll{0};  // destroy() entered while a poll was running on another thread
std::atomic<long> g_wrongThread{0};        // create/destroy on a thread other than the one that polls
std::atomic<int> g_inPoll{0};              // polls in progress right now
std::atomic<bool> g_gate{false};           // hold poll_tracks until released
std::atomic<bool> g_churn{false};
std::atomic<long> g_tuneCalls{0};
std::atomic<int> g_lastTuneRc{12345};
std::atomic<std::uint64_t> g_pollerThread{0};  // the thread that polled most recently
double g_churnBaseHz = 100.0e6;
double g_churnStepHz = 1000.0;
std::atomic<const CascadeHostApi*> g_host{nullptr};

std::atomic<long> g_basemapLive{0};
std::atomic<long> g_infoLive{0};
std::atomic<std::uint64_t> g_basemapCreateThread{0};
std::atomic<std::uint64_t> g_basemapDestroyThread{0};
std::atomic<std::uint64_t> g_infoCreateThread{0};
std::atomic<std::uint64_t> g_infoDestroyThread{0};

std::mutex g_liveMutex;
std::set<void*> g_live;

std::uint64_t thisThread() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

struct Source {
    long seq = 0;
    CascadePathPoint trail[2] = {{51.0, -1.0}, {51.1, -1.1}};
};

CascadeTrack make(const char* id, double lat, double lon) {
    CascadeTrack t{};
    std::snprintf(t.id, CASCADE_TRACK_ID_CHARS, "%s", id);
    std::snprintf(t.label, CASCADE_TRACK_LABEL_CHARS, "P3-%s", id);
    t.latDeg = lat;
    t.lonDeg = lon;
    t.altM = 5000.0;
    t.courseDeg = 90.0;
    t.speedMps = 200.0;
    t.ageMs = 1000u;
    t.kind = CASCADE_TRACK_AIRCRAFT;
    return t;
}

void* create() {
    // The first poll may not have happened yet, so the thread rule is judged against the poller once there is one.
    auto* s = new Source();
    {
        std::lock_guard<std::mutex> lk(g_liveMutex);
        g_live.insert(s);
    }
    g_creates.fetch_add(1);
    const std::uint64_t poller = g_pollerThread.load();
    if (poller != 0u && poller != thisThread()) { g_wrongThread.fetch_add(1); }
    return s;
}

void destroy(void* handle) {
    bool live = false;
    {
        std::lock_guard<std::mutex> lk(g_liveMutex);
        live = g_live.erase(handle) != 0u;
    }
    if (!live) {
        g_staleCalls.fetch_add(1);
        return;  // never touch a handle that is not ours any more
    }
    if (g_inPoll.load() != 0) { g_destroyDuringPoll.fetch_add(1); }
    const std::uint64_t poller = g_pollerThread.load();
    if (poller != 0u && poller != thisThread()) { g_wrongThread.fetch_add(1); }
    g_destroys.fetch_add(1);
    delete static_cast<Source*>(handle);
}

int32_t pollTracks(void* handle, CascadeTrack* out, uint32_t cap) {
    {
        std::lock_guard<std::mutex> lk(g_liveMutex);
        if (g_live.count(handle) == 0u) {
            g_staleCalls.fetch_add(1);
            return -1;
        }
    }
    g_inPoll.fetch_add(1);
    g_pollerThread.store(thisThread());
    g_polls.fetch_add(1);
    Source* s = static_cast<Source*>(handle);
    const long n = ++s->seq;
    // HELD INSIDE THE MODULE until released: the interface thread is, for as long as the test wants, in plugin code.
    while (g_gate.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    if (g_churn.load()) {
        const CascadeHostApi* host = g_host.load();
        if (host != nullptr && host->request_tune != nullptr) {
            const long k = g_tuneCalls.fetch_add(1) + 1;
            g_lastTuneRc.store(host->request_tune(host->ctx, g_churnBaseHz + static_cast<double>(k) * g_churnStepHz));
        }
    }
    const double d = static_cast<double>(n % 1000) * 0.0001;
    const CascadeTrack all[3] = {make("P3A1", 51.0 + d, -1.0), make("P3A2", 52.0 + d, -2.0), make("P3A3", 53.0 + d, -3.0)};
    uint32_t k = 0;
    for (; k < 3u && k < cap; ++k) { out[k] = all[k]; }
    g_inPoll.fetch_sub(1);
    return static_cast<int32_t>(k);
}

int32_t pollPaths(void* handle, CascadePath* out, uint32_t cap) {
    Source* s = static_cast<Source*>(handle);
    if (s == nullptr || cap == 0u) { return 0; }
    std::snprintf(out[0].id, CASCADE_TRACK_ID_CHARS, "P3-trail");
    out[0].kind = CASCADE_TRACK_AIRCRAFT;
    out[0].flags = 0;
    out[0].points = s->trail;
    out[0].count = 2u;
    return 1;
}

// ---- the basemap and the track-info client: counted, and the thread of each create and destroy remembered -----------------------
std::uint64_t thisThread();
void* basemapCreate() {
    g_basemapLive.fetch_add(1);
    g_basemapCreateThread.store(thisThread());
    return new int(0);
}
int32_t basemapTile(void*, uint32_t, uint32_t, uint32_t, CascadeTile*) { return CASCADE_TILE_MISSING; }
void basemapRelease(void*, const CascadeTile*) {}
int32_t basemapText(void*, char*, size_t) { return 0; }
void basemapDestroy(void* handle) {
    g_basemapLive.fetch_sub(1);
    g_basemapDestroyThread.store(thisThread());
    delete static_cast<int*>(handle);
}
const CascadeBasemapApi kBasemap = {
    static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
    "Pump3 test tiles",
    0u,
    3u,
    256u,
    &basemapCreate,
    &basemapTile,
    &basemapRelease,
    &basemapText,
    &basemapDestroy,
};

void* infoCreate() {
    g_infoLive.fetch_add(1);
    g_infoCreateThread.store(thisThread());
    return new int(0);
}
int32_t infoGet(void*, const char*, uint32_t, CascadeTrackInfo*) { return CASCADE_INFO_MISSING; }
void infoRelease(void*, const CascadeTrackInfo*) {}
int32_t infoText(void*, char*, size_t) { return 0; }
void infoDestroy(void* handle) {
    g_infoLive.fetch_sub(1);
    g_infoDestroyThread.store(thisThread());
    delete static_cast<int*>(handle);
}
const CascadeTrackInfoApi kInfo = {
    static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)),
    &infoCreate,
    &infoGet,
    &infoRelease,
    &infoText,
    &infoDestroy,
};

const CascadeTrackSourceApi kTracks = {
    static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)),
    &create,
    &pollTracks,
    &pollPaths,
    &destroy,
};

void attach(const CascadeHostApi* host) {
    g_host.store(host);
    g_attaches.fetch_add(1);
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &attach,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_TRACK_SOURCE, static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &kTracks},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
    {CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)), &kBasemap},
    {CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)), &kInfo},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Pump3 Fixture",
    "1.0.0",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

// WHAT WAS STILL ALIVE WHEN THE MODULE WAS UNMAPPED. A static object's destructor runs when the image is released (FreeLibrary,
// dlclose, process exit): the one moment that is "at the unmap" and not before it.
struct UnmapWitness {
    ~UnmapWitness() {
        const char* path = std::getenv("FIXTURE_P3_UNMAP_LOG");
        if (path == nullptr || path[0] == '\0') { return; }
        std::size_t alive = 0;
        {
            std::lock_guard<std::mutex> lk(g_liveMutex);
            alive = g_live.size();
        }
        if (std::FILE* f = std::fopen(path, "ab")) {
            std::fprintf(f, "unmapped live=%zu creates=%ld destroys=%ld\n", alive, g_creates.load(), g_destroys.load());
            std::fclose(f);
        }
    }
} g_witness;

}  // namespace

extern "C" {

CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}

// ---- what a test asks of the module (looked up by name in the loaded image) -----------------------------------------------------
CASCADE_PLUGIN_EXPORT long fixture_p3_live() {
    std::lock_guard<std::mutex> lk(g_liveMutex);
    return static_cast<long>(g_live.size());
}
CASCADE_PLUGIN_EXPORT long fixture_p3_creates() { return g_creates.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_destroys() { return g_destroys.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_polls() { return g_polls.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_attaches() { return g_attaches.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_stale_calls() { return g_staleCalls.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_destroy_during_poll() { return g_destroyDuringPoll.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_wrong_thread() { return g_wrongThread.load(); }
CASCADE_PLUGIN_EXPORT int fixture_p3_in_poll() { return g_inPoll.load(); }
CASCADE_PLUGIN_EXPORT void fixture_p3_set_gate(int on) { g_gate.store(on != 0); }
CASCADE_PLUGIN_EXPORT void fixture_p3_set_churn(int on, double baseHz, double stepHz) {
    g_churnBaseHz = baseHz;
    g_churnStepHz = stepHz;
    g_churn.store(on != 0);
}
CASCADE_PLUGIN_EXPORT long fixture_p3_tune_calls() { return g_tuneCalls.load(); }
CASCADE_PLUGIN_EXPORT int fixture_p3_last_tune_rc() { return g_lastTuneRc.load(); }
// A request_tune from the CALLER'S thread (a test thread standing for the plugin's own worker), through the table attach() was
// handed: what the host answers, or a sentinel when no table has been handed over.
CASCADE_PLUGIN_EXPORT int fixture_p3_request_tune(double hz) {
    const CascadeHostApi* host = g_host.load();
    if (host == nullptr || host->request_tune == nullptr) { return 12345; }
    return host->request_tune(host->ctx, hz);
}
// The centre the host reports, through the same table.
CASCADE_PLUGIN_EXPORT double fixture_p3_centre_hz() {
    const CascadeHostApi* host = g_host.load();
    if (host == nullptr || host->centre_hz == nullptr) { return -1.0; }
    return host->centre_hz(host->ctx);
}
// The basemap and the track-info client: how many instances exist, and the thread (a hash; compare with fixture_p3_thread()) of the
// last create and the last destroy of each.
CASCADE_PLUGIN_EXPORT long fixture_p3_basemap_live() { return g_basemapLive.load(); }
CASCADE_PLUGIN_EXPORT long fixture_p3_info_live() { return g_infoLive.load(); }
CASCADE_PLUGIN_EXPORT unsigned long long fixture_p3_basemap_create_thread() { return g_basemapCreateThread.load(); }
CASCADE_PLUGIN_EXPORT unsigned long long fixture_p3_basemap_destroy_thread() { return g_basemapDestroyThread.load(); }
CASCADE_PLUGIN_EXPORT unsigned long long fixture_p3_info_create_thread() { return g_infoCreateThread.load(); }
CASCADE_PLUGIN_EXPORT unsigned long long fixture_p3_info_destroy_thread() { return g_infoDestroyThread.load(); }
CASCADE_PLUGIN_EXPORT unsigned long long fixture_p3_thread() { return thisThread(); }
// Zeroes every counter (the live set is not touched: it is what the host really holds).
CASCADE_PLUGIN_EXPORT void fixture_p3_reset() {
    g_creates.store(0);
    g_destroys.store(0);
    g_polls.store(0);
    g_attaches.store(0);
    g_staleCalls.store(0);
    g_destroyDuringPoll.store(0);
    g_wrongThread.store(0);
    g_tuneCalls.store(0);
}

}  // extern "C"
