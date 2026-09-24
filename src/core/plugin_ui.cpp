// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_ui.hpp"

#include "core/version.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>

namespace cascade::core {
namespace {

// Copies a fixed-size ABI char array out as a std::string without trusting it
// to be NUL-terminated. A plugin that fills every byte is not malformed - the
// ABI says "NUL-terminated" but a host that walks off the end because one
// plugin got it wrong is a host bug, not a plugin bug.
std::string bounded(const char* p, std::size_t cap) {
    const std::size_t n = ::strnlen(p, cap);
    return std::string(p, n);
}

// The fade and drop thresholds for one kind. Written as a lookup rather than a
// chain of ifs at each call site so there is exactly one place a cadence can be
// wrong, and so an unrecognised kind lands on the forgiving default by
// construction instead of by someone remembering to write the else branch.
void staleLimits(std::uint32_t kind, std::uint64_t& fadeMs, std::uint64_t& dropMs) {
    switch (kind) {
        case CASCADE_TRACK_AIRCRAFT:
            fadeMs = kTrackFadeMsAircraft;
            dropMs = kTrackDropMsAircraft;
            break;
        case CASCADE_TRACK_VESSEL:
            fadeMs = kTrackFadeMsVessel;
            dropMs = kTrackDropMsVessel;
            break;
        case CASCADE_TRACK_STATION:
            fadeMs = kTrackFadeMsStation;
            dropMs = kTrackDropMsStation;
            break;
        case CASCADE_TRACK_SATELLITE:
            fadeMs = kTrackFadeMsSatellite;
            dropMs = kTrackDropMsSatellite;
            break;
        default:
            // See the header: the most forgiving rule the host has, because it
            // does not know what this kind's reporting cadence is.
            fadeMs = kTrackFadeMsStation;
            dropMs = kTrackDropMsStation;
            break;
    }
}

// Metres per degree of latitude, and the constant that turns degrees of
// longitude into metres once multiplied by the cosine of the latitude. A
// spherical earth is the right model here: every distance this file measures
// is a few hundred metres compared against a fifty or hundred metre threshold,
// where the difference between a sphere and the WGS84 ellipsoid is centimetres
// - and the alternative, a great-circle formula per observation per vertex per
// frame, would pay for accuracy nothing here can use.
constexpr double kMetresPerDegree = 111320.0;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// The squared distance between two nearby positions, in square metres, with
// the cosine of the latitude supplied by the caller.
//
// SQUARED, so nothing here calls sqrt: every use is a comparison against a
// threshold, and comparing squares answers the same question. The cosine is a
// PARAMETER because both callers compare one position against many, and the
// only honest reason to recompute a transcendental for each of them would be a
// latitude difference this function is never given - a few hundred metres.
double metresSqBetween(double lat1, double lon1, double lat2, double lon2,
                       double cosLat) {
    double dLon = lon2 - lon1;
    // The antimeridian, which two positions a hundred metres apart can sit
    // either side of: their longitudes then differ by very nearly 360 degrees
    // and the naive difference would call them half a world apart.
    if (dLon > 180.0) { dLon -= 360.0; }
    if (dLon < -180.0) { dLon += 360.0; }
    const double dy = (lat2 - lat1) * kMetresPerDegree;
    const double dx = dLon * kMetresPerDegree * cosLat;
    return dx * dx + dy * dy;
}

// The order the observation store is kept in: plugin first, then track id.
//
// SORTED SO THE LOOKUP IS A BINARY SEARCH, and that is not premature. Both
// callers ask per item over a whole collection - noteAltitude once per track
// per frame, altitudeNear once per trail VERTEX per frame - so a linear scan
// would make the store quadratic in the number of live tracks, and the host's
// own cap allows four thousand of them per plugin. Sorted, the same work is
// logarithmic, and the two mutations that exist keep the order by
// construction: an insert goes at its lower_bound, and the per-frame eviction
// is a stable remove_if.
//
// The id is a string_view so a LOOKUP never has to materialise a key: the ABI
// hands over a fixed char array, and building a std::string from it once per
// track per frame would allocate for any id long enough to escape the
// small-string buffer.
bool altKeyLess(const std::string& aPlugin, std::string_view aId,
                const std::string& bPlugin, std::string_view bId) {
    if (aPlugin != bPlugin) { return aPlugin < bPlugin; }
    return aId < bId;
}

}  // namespace

TrackPresentation trackPresentation(std::uint64_t ageMs, std::uint32_t kind) {
    std::uint64_t fadeMs = 0;
    std::uint64_t dropMs = 0;
    staleLimits(kind, fadeMs, dropMs);

    TrackPresentation p;
    if (ageMs >= dropMs) {
        // GONE, not merely faint. The list row and the marker both disappear,
        // and the target counts stop counting it.
        p.visible = false;
        p.alpha = 0.0f;
        return p;
    }
    if (ageMs < fadeMs) { return p; }  // fresh: full strength

    // Linear from full strength at the fade threshold down to kTrackMinAlpha
    // just before the drop threshold. Linear rather than anything cleverer
    // because the value being conveyed is "how long since we last heard it",
    // and a curve would make equal silences look unequal.
    //
    // dropMs > fadeMs holds for every kind above, and ageMs < dropMs here, so
    // the denominator is non-zero and t stays inside [0,1).
    const double t = static_cast<double>(ageMs - fadeMs) /
                     static_cast<double>(dropMs - fadeMs);
    p.alpha = static_cast<float>(1.0 - t * (1.0 - static_cast<double>(kTrackMinAlpha)));
    return p;
}

TrackPresentation pathPresentation(const HostPath& path,
                                   const std::vector<HostTrack>& tracks) {
    for (const HostTrack& ht : tracks) {
        // PLUGIN AND ID BOTH, because neither alone is an identity: two
        // sources may key on the same ICAO address or MMSI, and one plugin
        // going quiet must not erase another plugin's trail. Both fields are
        // filled from the same instance name in PluginUi::poll, so the
        // comparison is exact rather than approximate.
        if (ht.plugin != path.plugin) { continue; }
        if (path.id != ht.t.id) { continue; }
        // The OWNER'S kind decides, not the path's: the age being judged is
        // the owner's, and judging it against a cadence the owner does not
        // report at would be the same mistake one global threshold was.
        return trackPresentation(ht.t.ageMs, ht.t.kind);
    }
    // Unowned: see the header. Not everything a source draws is a target.
    return TrackPresentation{};
}

std::size_t visibleTrackCount(const std::vector<HostTrack>& tracks) {
    std::size_t n = 0;
    for (const HostTrack& ht : tracks) {
        if (trackPresentation(ht.t.ageMs, ht.t.kind).visible) { ++n; }
    }
    return n;
}

bool anyVisibleTarget(const std::vector<HostTrack>& tracks,
                      const std::vector<HostPath>& paths) {
    // Tracks first, because it is the cheap half and the common one.
    if (visibleTrackCount(tracks) > 0u) { return true; }
    for (const HostPath& p : paths) {
        if (pathPresentation(p, tracks).visible) { return true; }
    }
    return false;
}

// --- Audio mute policy (see the header) -------------------------------------

bool onPreset(const MutePreset& preset, const TunePoint& tune) {
    // A preset with no frequency is not a place, and matching it would make
    // every plugin that fills a CascadePreset badly mute the radio wherever it
    // happened to be pointed. The ABI already says frequencyHz must be > 0.
    if (!(preset.frequencyHz > 0.0)) { return false; }
    const double f = preset.deviceCentre ? tune.deviceCentreHz : tune.tunedHz;
    // std::max would be the obvious spelling, but a plugin is free to hand us
    // a negative bandwidth and half of it would then WIN a max against the
    // floor only if the floor were also negative - which it is not. Written
    // out so the guard is visible rather than inferred.
    double tol = kPresetToleranceHz;
    if (preset.bandwidthHz > 0.0 && 0.5 * preset.bandwidthHz > tol) {
        tol = 0.5 * preset.bandwidthHz;
    }
    return std::fabs(f - preset.frequencyHz) <= tol;
}

bool muteDefaultForCaps(std::uint32_t capabilities) {
    return (capabilities & CASCADE_CAP_IQ_DECODER) != 0u;
}

MuteDecision muteActive(const std::vector<MutePlugin>& plugins,
                        const TunePoint& tune) {
    MuteDecision d;
    for (const MutePlugin& p : plugins) {
        // Both halves are required and neither implies the other: a stopped
        // plugin decodes nothing whatever its setting says, and a plugin the
        // user has told not to mute stays audible however hard it is working.
        if (!p.running || !p.mutes) { continue; }
        bool on = false;
        for (const MutePreset& ps : p.presets) {
            if (onPreset(ps, tune)) {
                on = true;
                break;
            }
        }
        if (!on) { continue; }
        d.active = true;
        d.names.push_back(p.name);
        d.keys.push_back(p.key);
    }
    return d;
}

bool anyStillRunning(const std::vector<MutePlugin>& plugins,
                     const std::vector<std::string>& keys) {
    for (const std::string& k : keys) {
        // An empty key matches nothing, the same rule PluginStopSet applies and
        // for the same reason: a record with no path yields "", and one stray
        // empty must not stand for every path-less plugin at once.
        if (k.empty()) { continue; }
        for (const MutePlugin& p : plugins) {
            if (p.key == k && p.running && p.mutes) { return true; }
        }
    }
    return false;
}

MutePopupSubject advanceMutePopup(const MutePopupSubject& prev, bool onPreset,
                                  bool tuneAway, bool subjectRunning,
                                  const std::vector<std::string>& names,
                                  const std::vector<std::string>& keys) {
    // ON A PRESET FIRST, before the edge is even looked at. The two cannot both
    // be true today (an edge means off-preset this frame), and ordering it this
    // way is the guarantee that no future caller can make the dialog appear
    // while the radio is sitting on the preset of the plugin it names.
    if (onPreset) { return MutePopupSubject{}; }
    if (tuneAway) {
        if (!subjectRunning) { return MutePopupSubject{}; }
        MutePopupSubject s;
        s.open = true;
        s.names = names;  // COPIED, deliberately: see the header
        s.keys = keys;
        return s;
    }
    if (prev.open && !subjectRunning) { return MutePopupSubject{}; }
    return prev;
}

// Per-plugin bridge behind CascadeHostApi::ctx. One of these exists for every
// plugin that declares CASCADE_CAP_HOST_CLIENT, because the host has to know
// WHICH plugin is asking - the permission is per-plugin, and a single shared
// context could not tell them apart.
struct HostCtx {
    PluginUi* self = nullptr;
    std::string plugin;  // PluginUi::tuneKey(), not the display name
    CascadeHostApi api{};
    // HOST API LEVEL 1. The core is SHARED with the PluginUi that issued this
    // bridge and outlives it, so a level-1 call through a bridge whose owner
    // is gone still lands in live memory and is answered DETACHED - unlike
    // the four original functions, which have to test `self` for that.
    std::shared_ptr<PluginApiCore> core;
    PluginApiClient* client = nullptr;  // owned by `core`, never freed before it
};

namespace {

// The C trampolines. Each recovers the bridge from ctx; a null ctx cannot
// happen through our own code but is checked anyway, because these pointers
// are handed to third-party code that may keep them longer than it should.
double hostCentre(void* ctx);
double hostRate(void* ctx);
std::int32_t hostTune(void* ctx, double centreHz);
std::int64_t hostTime(void* ctx);

// Points every bridge issued on behalf of `owner` at no host at all, leaving
// the storage in place. Declared here for ~PluginUi, defined with the store.
void detachBridges(const PluginUi* owner);

// Fills the level-1 members of a bridge's table. Defined with the level-1
// trampolines below.
void fillLevel1(HostCtx& c);

}  // namespace

PluginUi::~PluginUi() {
    clear();
    // Every level-1 call from here on is answered DETACHED, whatever bridge
    // it arrives through - the core itself lives on in the bridges.
    api_->setAttached(false);
    // DETACHED, NOT FREED. A plugin may still be loaded and still holding a
    // bridge that points here; once this object is gone, a call through it
    // would lock a destroyed mutex and read a destroyed std::function - which
    // is what aborted inside libc++ on Android on every exit of the app. The
    // storage stays (the module may still read it) and answers the ABI's
    // "nothing" from here on. See detachBridges below for the reason this is
    // a net rather than the main defence: AppWindow now clears the runner,
    // and so destroys every decoder instance, while this object is alive.
    detachBridges(this);
}

void PluginUi::setServices(HostServices services) {
    std::lock_guard<std::mutex> lk(servicesMutex_);
    services_ = std::move(services);
}

namespace {

// Storage for the bridges. Kept in a file-scope owner rather than in the class
// so the header does not have to expose HostCtx.
//
// NOTHING SHORTER THAN THE PROCESS FREES ONE, and that is the fix for two
// crash reports (see hostBridgeCount in the header). plugin_abi.h promises a
// plugin a table "valid for as long as the plugin is loaded" and tells it to
// store the pointer; Survey Engine 0.1.0 reads that table from inside its
// decoder's destroy(). PluginUi::clear() used to free the bridges while every
// module was still mapped, so the plugin read freed memory - 0xC0000005 on
// Windows, an abort inside libc++ on Android. A static vector is reclaimed
// after main() returns, which is after ~PluginHost has unmapped every module:
// the last moment any plugin code could run. One bridge per plugin, reused
// across rebuilds, so this is bounded by the number of distinct plugins that
// have ever attached rather than by how often the user changes source.
std::vector<std::unique_ptr<HostCtx>>& ctxStore() {
    static std::vector<std::unique_ptr<HostCtx>> store;
    return store;
}

// The bridge already issued to `plugin` on behalf of `owner`, or null.
HostCtx* findBridge(const PluginUi* owner, const std::string& plugin) {
    for (const std::unique_ptr<HostCtx>& c : ctxStore()) {
        if (c != nullptr && c->self == owner && c->plugin == plugin) { return c.get(); }
    }
    return nullptr;
}

void detachBridges(const PluginUi* owner) {
    for (const std::unique_ptr<HostCtx>& c : ctxStore()) {
        if (c != nullptr && c->self == owner) { c->self = nullptr; }
    }
}

// NOTHING THROWS ACROSS THE BOUNDARY IN THIS DIRECTION EITHER. The ABI makes
// plugins promise that no exception reaches the host; the host owes the same
// promise back, because the frames above these trampolines belong to a third
// party's compiler and may well be noexcept - the standard library's thread
// entry is - and an exception arriving in one of those is std::terminate in
// the plugin's own CRT, i.e. an abort() this application cannot see or
// report. Each trampoline therefore answers a failure with the ABI's own
// "nothing": zero, or CASCADE_TUNE_FAILED.
double hostCentre(void* ctx) {
    try {
        auto* c = static_cast<HostCtx*>(ctx);
        if (c == nullptr || c->self == nullptr || !c->self->hasServices()) { return 0.0; }
        return c->self->servicesCentreHz();
    } catch (...) {
        return 0.0;
    }
}

double hostRate(void* ctx) {
    try {
        auto* c = static_cast<HostCtx*>(ctx);
        if (c == nullptr || c->self == nullptr || !c->self->hasServices()) { return 0.0; }
        return c->self->servicesRateHz();
    } catch (...) {
        return 0.0;
    }
}

std::int32_t hostTune(void* ctx, double centreHz) {
    try {
        auto* c = static_cast<HostCtx*>(ctx);
        if (c == nullptr || c->self == nullptr) { return CASCADE_TUNE_FAILED; }
        return c->self->tuneRequestFromPlugin(c->plugin, centreHz);
    } catch (...) {
        return CASCADE_TUNE_FAILED;
    }
}

std::int64_t hostTime(void* ctx) {
    try {
        auto* c = static_cast<HostCtx*>(ctx);
        if (c == nullptr || c->self == nullptr || !c->self->hasServices()) { return 0; }
        return c->self->servicesUnixTimeMs();
    } catch (...) {
        return 0;
    }
}

// --- HOST API LEVEL 1 trampolines --------------------------------------------
//
// Each one recovers the bridge, hands the call to the shared PluginApiCore
// with the bridge's own client, and answers CASCADE_API_FAILED if anything in
// the host threw - the same no-exception promise the four above keep, for the
// same reason. None of them reads `self`: the core outlives the PluginUi, and
// that is what lets a call after the owner has gone be answered rather than
// be a use-after-free.
template <class F>
std::int32_t level1(void* ctx, F&& f) {
    try {
        auto* c = static_cast<HostCtx*>(ctx);
        if (c == nullptr || !c->core || c->client == nullptr) { return CASCADE_API_DETACHED; }
        return f(*c->core, *c->client);
    } catch (...) {
        return CASCADE_API_FAILED;
    }
}

std::int32_t l1GetState(void* ctx, CascadeReceiverState* out) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) { return a.getState(c, out); });
}
std::int32_t l1GetGain(void* ctx, std::uint32_t index, CascadeGainInfo* out) {
    return level1(ctx,
                  [&](PluginApiCore& a, PluginApiClient& c) { return a.getGain(c, index, out); });
}
std::int32_t l1GetRates(void* ctx, double* out, std::uint32_t cap) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) {
        return a.getSampleRates(c, out, cap);
    });
}
std::int32_t l1GetStream(void* ctx, CascadeStreamInfo* out) {
    return level1(ctx,
                  [&](PluginApiCore& a, PluginApiClient& c) { return a.getStreamInfo(c, out); });
}

std::int32_t l1Control(void* ctx, PluginControl::Kind kind, double value, std::uint32_t mode,
                       bool flag, const char* gainName) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) {
        PluginControl r;
        r.kind = kind;
        r.value = value;
        r.mode = mode;
        r.flag = flag;
        if (gainName != nullptr) {
            // Copied here, bounded: the plugin's string is never read again.
            std::size_t n = 0;
            while (n + 1 < sizeof(r.gainName) && gainName[n] != '\0') {
                r.gainName[n] = gainName[n];
                ++n;
            }
            r.gainName[n] = '\0';
            // A name longer than any the host publishes cannot match one.
            if (gainName[n] != '\0') { return static_cast<std::int32_t>(CASCADE_API_UNSUPPORTED); }
        }
        return a.requestControl(c, r);
    });
}

using K = PluginControl::Kind;
std::int32_t l1SetFrequency(void* ctx, double hz) {
    return l1Control(ctx, K::Frequency, hz, 0, false, nullptr);
}
std::int32_t l1SetVfoOffset(void* ctx, double hz) {
    return l1Control(ctx, K::VfoOffset, hz, 0, false, nullptr);
}
std::int32_t l1SetMode(void* ctx, std::uint32_t mode) {
    return l1Control(ctx, K::Mode, 0.0, mode, false, nullptr);
}
std::int32_t l1SetBandwidth(void* ctx, double hz) {
    return l1Control(ctx, K::Bandwidth, hz, 0, false, nullptr);
}
std::int32_t l1SetSquelch(void* ctx, double db) {
    return l1Control(ctx, K::Squelch, db, 0, false, nullptr);
}
std::int32_t l1SetSampleRate(void* ctx, double hz) {
    return l1Control(ctx, K::SampleRate, hz, 0, false, nullptr);
}
std::int32_t l1SetGain(void* ctx, const char* name, double db) {
    if (name == nullptr) { return CASCADE_API_BAD_ARGUMENT; }
    return l1Control(ctx, K::Gain, db, 0, false, name);
}
std::int32_t l1SetDeviceAgc(void* ctx, std::int32_t on) {
    return l1Control(ctx, K::DeviceAgc, 0.0, 0, on != 0, nullptr);
}
std::int32_t l1SetRunning(void* ctx, std::int32_t on) {
    return l1Control(ctx, K::Running, 0.0, 0, on != 0, nullptr);
}
std::int32_t l1SetVolume(void* ctx, double v) {
    return l1Control(ctx, K::Volume, v, 0, false, nullptr);
}
std::int32_t l1SetMuted(void* ctx, std::int32_t on) {
    return l1Control(ctx, K::Muted, 0.0, 0, on != 0, nullptr);
}

std::int32_t l1SetMarker(void* ctx, const CascadeMarker* m) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) { return a.setMarker(c, m); });
}
std::int32_t l1RemoveMarker(void* ctx, std::uint32_t id) {
    return level1(ctx,
                  [&](PluginApiCore& a, PluginApiClient& c) { return a.removeMarker(c, id); });
}
std::int32_t l1ClearMarkers(void* ctx) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) { return a.clearMarkers(c); });
}
std::int32_t l1SettingsGet(void* ctx, const char* key, char* buf, std::size_t cap) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) {
        return a.settingsGet(c, key, buf, cap);
    });
}
std::int32_t l1SettingsSet(void* ctx, const char* key, const char* value) {
    return level1(ctx,
                  [&](PluginApiCore& a, PluginApiClient& c) { return a.settingsSet(c, key, value); });
}
std::int32_t l1Log(void* ctx, std::uint32_t level, const char* text) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) { return a.log(c, level, text); });
}
std::int32_t l1AddCommand(void* ctx, std::uint32_t id, const char* label) {
    return level1(ctx,
                  [&](PluginApiCore& a, PluginApiClient& c) { return a.addCommand(c, id, label); });
}
std::int32_t l1RemoveCommand(void* ctx, std::uint32_t id) {
    return level1(ctx,
                  [&](PluginApiCore& a, PluginApiClient& c) { return a.removeCommand(c, id); });
}
std::int32_t l1PollCommand(void* ctx, std::uint32_t* id) {
    return level1(ctx, [&](PluginApiCore& a, PluginApiClient& c) { return a.pollCommand(c, id); });
}

void fillLevel1(HostCtx& c) {
    CascadeHostApi& t = c.api;
    t.apiLevel = CASCADE_HOST_API_LEVEL;
    t.knownCapabilities = CASCADE_CAP_ALL_KNOWN;
    t.hostName = "FoxSDR";
    t.hostVersion = cascade::versionString();
    t.get_state = &l1GetState;
    t.get_gain = &l1GetGain;
    t.get_sample_rates = &l1GetRates;
    t.get_stream_info = &l1GetStream;
    t.set_frequency = &l1SetFrequency;
    t.set_vfo_offset = &l1SetVfoOffset;
    t.set_mode = &l1SetMode;
    t.set_bandwidth = &l1SetBandwidth;
    t.set_squelch = &l1SetSquelch;
    t.set_sample_rate = &l1SetSampleRate;
    t.set_gain = &l1SetGain;
    t.set_device_agc = &l1SetDeviceAgc;
    t.set_running = &l1SetRunning;
    t.set_volume = &l1SetVolume;
    t.set_muted = &l1SetMuted;
    t.set_marker = &l1SetMarker;
    t.remove_marker = &l1RemoveMarker;
    t.clear_markers = &l1ClearMarkers;
    t.settings_get = &l1SettingsGet;
    t.settings_set = &l1SettingsSet;
    t.log = &l1Log;
    t.add_command = &l1AddCommand;
    t.remove_command = &l1RemoveCommand;
    t.poll_command = &l1PollCommand;
}

}  // namespace

std::size_t hostBridgeCount() { return ctxStore().size(); }

std::size_t attachedHostBridgeCount() {
    std::size_t n = 0;
    for (const std::unique_ptr<HostCtx>& c : ctxStore()) {
        if (c != nullptr && c->self != nullptr) { ++n; }
    }
    return n;
}

void PluginUi::rebuild(const std::vector<LoadedPlugin>& plugins) {
    destroyInstances();

    // WHICH PLUGINS THE LEVEL-1 API ANSWERS FOR, decided BEFORE any attach():
    // a plugin that reads its settings or the receiver from inside attach()
    // - the natural place to - must find itself live, not DETACHED. The same
    // rule as the loop below (loaded, not stopped, declares the host table),
    // and one call, so a plugin that stays attached across this rebuild never
    // sees a transient DETACHED either. Everything else loses its marks,
    // commands and queued requests here (PluginApiCore::setLiveSet).
    {
        std::vector<std::string> live;
        for (const LoadedPlugin& lp : plugins) {
            if (!lp.loaded || stopped_.contains(lp)) { continue; }
            if (lp.hostClient == nullptr || lp.hostClient->attach == nullptr) { continue; }
            const std::string key = tuneKey(lp);
            if (key.empty()) { continue; }
            api_->client(key, lp.name);
            live.push_back(key);
        }
        api_->setLiveSet(live);
    }

    for (const LoadedPlugin& lp : plugins) {
        if (!lp.loaded) { continue; }
        // STOPPED BY THE USER: no attach, no track source, no panel. Skipped
        // before the host bridge in particular - handing a stopped plugin the
        // host API would give it a fresh way to ask for the receiver, and the
        // one thing a stopped plugin must not do is act.
        if (stopped_.contains(lp)) { continue; }

        // Host services FIRST. The ABI says attach() runs before any other
        // capability's create(), so a tracker can read the receiver while
        // building its initial state instead of waiting a frame for it.
        if (lp.hostClient != nullptr && lp.hostClient->attach != nullptr) {
            // The permission key, NOT the display name: the name is the
            // plugin's own to choose, so keying on it would let any module
            // inherit a granted one's permission by adopting its name.
            const std::string key = tuneKey(lp);
            // ONE BRIDGE PER PLUGIN, REUSED, not a fresh one per rebuild.
            //
            // rebuild() runs on every source change, so a bridge per call grew
            // the store without limit and - worse - left the plugin holding
            // the table from its FIRST attach while the host considered a
            // later one current. The ABI says attach() is called once with a
            // table valid for as long as the plugin is loaded; handing back
            // the same table makes the host's repeated call the no-op the
            // plugin is entitled to assume it is.
            HostCtx* bridge = findBridge(this, key);
            if (bridge == nullptr) {
                auto owned = std::make_unique<HostCtx>();
                owned->self = this;
                owned->plugin = key;
                owned->api.structSize = static_cast<std::uint32_t>(sizeof(CascadeHostApi));
                owned->api.ctx = owned.get();
                owned->api.centre_hz = &hostCentre;
                owned->api.sample_rate_hz = &hostRate;
                owned->api.request_tune = &hostTune;
                owned->api.unix_time_ms = &hostTime;
                // HOST API LEVEL 1: the same table, grown at the end. A plugin
                // built before level 1 reads only the four members above at
                // the offsets they have always had (plugin_abi.h, VERSIONING).
                owned->core = api_;
                if (!key.empty()) { owned->client = &api_->client(key, lp.name); }
                fillLevel1(*owned);
                bridge = owned.get();
                ctxStore().push_back(std::move(owned));
            }
            lp.hostClient->attach(&bridge->api);
        }

        if (lp.trackSource != nullptr) {
            void* h = lp.trackSource->create();
            if (h != nullptr) {
                TrackInstance ti;
                ti.api = lp.trackSource;
                ti.handle = h;
                ti.name = lp.name;
                // The name mirror rides with the instance, so the two can
                // never disagree about which plugins have a track source.
                trackPluginNames_.push_back(ti.name);
                trackInstances_.push_back(std::move(ti));
            }
        }

        // NOTE: no image-decoder instance is created here. An image decoder
        // consumes samples, so PluginRunner owns it - see the header.

        if (lp.panel != nullptr) {
            void* h = lp.panel->create();
            if (h != nullptr) {
                HostPanel hp;
                hp.plugin = lp.name;
                hp.title = lp.panel->title != nullptr ? lp.panel->title : lp.name;

                // Columns are read ONCE: the ABI fixes a panel's shape for its
                // lifetime, so rediscovering it every frame would be work that
                // can only ever return the same answer.
                char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS] = {};
                std::uint32_t cols = lp.panel->columns(h, headings);
                if (cols == 0u) { cols = 1u; }
                if (cols > CASCADE_PANEL_MAX_COLUMNS) { cols = CASCADE_PANEL_MAX_COLUMNS; }
                for (std::uint32_t c = 0; c < cols; ++c) {
                    hp.headings.push_back(bounded(headings[c], CASCADE_PANEL_CELL_CHARS));
                }

                PanelInstance pi;
                pi.api = lp.panel;
                pi.handle = h;
                pi.name = lp.name;
                pi.panelIndex = panels_.size();
                panels_.push_back(std::move(hp));
                panelInstances_.push_back(std::move(pi));
            }
        }

        if (lp.instrument != nullptr) {
            void* h = lp.instrument->create();
            if (h != nullptr) {
                HostInstrument hi;
                hi.plugin = lp.name;
                hi.title = lp.instrument->title != nullptr ? lp.instrument->title : lp.name;
                hi.kind = lp.instrument->kind;
                // The memory feed is a pair (the loader refuses half of one),
                // and its columns are read once, as a panel's are.
                if (lp.instrument->columns != nullptr && lp.instrument->poll_rows != nullptr) {
                    char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS] = {};
                    std::uint32_t cols = lp.instrument->columns(h, headings);
                    if (cols == 0u) { cols = 1u; }
                    if (cols > CASCADE_PANEL_MAX_COLUMNS) { cols = CASCADE_PANEL_MAX_COLUMNS; }
                    for (std::uint32_t c = 0; c < cols; ++c) {
                        hi.headings.push_back(bounded(headings[c], CASCADE_PANEL_CELL_CHARS));
                    }
                }
                InstrumentInstance ii;
                ii.api = lp.instrument;
                ii.handle = h;
                ii.name = lp.name;
                ii.index = instruments_.size();
                instruments_.push_back(std::move(hi));
                instrumentInstances_.push_back(std::move(ii));
            }
        }
    }

    // Demonstration faces LAST, so every real instance's index stays valid.
    if (const char* demo = std::getenv("FOXSDR_DEMO_INSTRUMENT");
        demo != nullptr && demo[0] != '\0') {
        addDemoInstruments(demo);
    }
}

// ---------------------------------------------------------------------------
// Demonstration instruments. Everything below is sample content, labelled as
// such in the window title, and exists so a face can be drawn and looked at
// with no radio and no plugin. The figures are plausible for the kind - a
// pager message, a radial, a meter reading - so a face is designed against
// the shape of real data rather than against zeroes.

std::uint32_t PluginUi::demoKindByName(const std::string& name) {
    struct Entry {
        const char* n;
        std::uint32_t k;
    };
    static const Entry table[] = {
        {"generic", CASCADE_INSTRUMENT_GENERIC},
        {"pager", CASCADE_INSTRUMENT_PAGER},
        {"teleprinter", CASCADE_INSTRUMENT_TELEPRINTER},
        {"tone", CASCADE_INSTRUMENT_TONE_ALERT},
        {"bearing", CASCADE_INSTRUMENT_NAV_BEARING},
        {"fax", CASCADE_INSTRUMENT_FAX},
        {"beacon", CASCADE_INSTRUMENT_BEACON},
        {"meter", CASCADE_INSTRUMENT_METER},
        {"weather", CASCADE_INSTRUMENT_WEATHER_CONSOLE},
    };
    for (const Entry& e : table) {
        if (name == e.n) { return e.k; }
    }
    return ~0u;
}

namespace {

void setText(CascadeInstrumentState& s, int slot, const char* v) {
    std::snprintf(s.text[slot], CASCADE_INSTRUMENT_TEXT_CHARS, "%s", v);
}

void addRow(std::vector<CascadePanelRow>& rows, std::uint32_t flags, const char* a,
            const char* b = nullptr, const char* c = nullptr, const char* d = nullptr) {
    CascadePanelRow r{};
    r.kind = CASCADE_ROW_CELLS;
    r.flags = flags;
    const char* cells[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        if (cells[i] != nullptr) {
            std::snprintf(r.cells[i], CASCADE_PANEL_CELL_CHARS, "%s", cells[i]);
        }
    }
    rows.push_back(r);
}

HostInstrument demoInstrument(std::uint32_t kind) {
    HostInstrument h;
    h.plugin = "Demonstration";
    h.kind = kind;
    h.have = true;
    h.state.structSize = static_cast<std::uint32_t>(sizeof(CascadeInstrumentState));
    h.state.seq = 1u;
    CascadeInstrumentState& s = h.state;
    switch (kind) {
        case CASCADE_INSTRUMENT_PAGER:
            h.title = "Pager DEMO";
            // LONGER THAN THE SCREEN ON PURPOSE. The face wraps a page into
            // four rows of twenty characters and flashes an arrow when it runs
            // past them, and a sample that fitted comfortably would leave both
            // of those untested by eye.
            setText(s, 0, "CALL DISPATCH RE UNIT 4 ETA 20 MIN BRING SPARE ANTENNA AND LOG");
            setText(s, 1, "1234567");
            setText(s, 2, "14:32");
            setText(s, 3, "ALPHA");
            s.values[0] = 3.0;
            // Ringing, and in frame sync: the two lamps a paging receiver has
            // to be able to show at once.
            s.flags = CASCADE_INSTRUMENT_FLAG_ALERT | CASCADE_INSTRUMENT_FLAG_LOCK;
            h.headings = {"Time", "Capcode", "Message"};
            addRow(h.rows, CASCADE_ROW_FLAG_GOOD, "14:32", "1234567",
                   "CALL DISPATCH RE UNIT 4 ETA 20 MIN BRING THE SPARE ANTENNA");
            addRow(h.rows, 0u, "14:29", "0891122", "MTG MOVED TO RM 3B");
            addRow(h.rows, 0u, "14:11", "1234567", "PLS CALL 0113 496 0111");
            addRow(h.rows, 0u, "14:04", "0000077", "5551234");
            addRow(h.rows, CASCADE_ROW_FLAG_MUTED, "13:58", "2200450", "TEST PAGE");
            break;
        case CASCADE_INSTRUMENT_TELEPRINTER:
            // Sample ACARS traffic in the OLDEST-FIRST order this kind's slot
            // comment gives for its rows - the paper, in the order it came
            // off the roll - so the face's "newest is the last row" reading
            // is exercised by the demonstration rather than only by the
            // plugin. The text slots carry the newest block's header: the
            // registration, flight, label, mode and block identifier a real
            // ACARS downlink block carries, which is what a cockpit printer
            // prints at the head of a message.
            h.title = "ACARS printer DEMO";
            setText(s, 0, "G-EZBX");
            setText(s, 1, "EZY83U");
            setText(s, 2, "H1");
            setText(s, 3, "2");
            setText(s, 4, "4");
            s.values[0] = 137.0;
            h.headings = {"Time", "Reg", "Flight", "Text"};
            addRow(h.rows, 0u, "14:29:41", "G-XLEA", "BAW1A", "WX REQ EGLL");
            addRow(h.rows, 0u, "14:30:12", "EI-DWA", "RYR4MK", "OUT 1428 OFF 1440 ETA 1602");
            addRow(h.rows, 0u, "14:30:40", "G-EUUU", "BAW817", "REQ PDC EGLL RWY 27R");
            addRow(h.rows, 0u, "14:31:05", "EI-DWA", "RYR4MK", "POS N5340 W00145 FL360 M78");
            addRow(h.rows, CASCADE_ROW_FLAG_GOOD, "14:31:50", "G-EZBX", "EZY83U",
                   "ETA EGNM 1455 GATE 12 FUEL 4.1T");
            break;
        case CASCADE_INSTRUMENT_TONE_ALERT:
            // A REAL PAGE OUT OF THE PUBLISHED CHART, not plausible-looking
            // numbers. 746.8 Hz and 879.0 Hz are Motorola Quick Call II reeds
            // 125 and 128, both in group 2, which the general encoding plan
            // makes cap code 258; the 1 s / 3 s timing is the chart's own.
            // Designing a face against invented figures is how a cell ends up
            // too narrow for the widest thing that can land in it.
            h.title = "Tone alert DEMO";
            s.values[0] = 746.8;
            s.values[1] = 879.0;
            s.values[2] = 1.02;
            s.values[3] = 2.98;
            setText(s, 0, "258");
            setText(s, 1, "Motorola Quick Call II group 2");
            setText(s, 2, "MATCHED");
            s.flags = CASCADE_INSTRUMENT_FLAG_ALERT;
            h.headings = {"Time", "Tones", "Code", "Result"};
            addRow(h.rows, CASCADE_ROW_FLAG_GOOD, "14:32:07", "746.8 / 879.0 Hz", "258",
                   "Motorola Quick Call II");
            addRow(h.rows, CASCADE_ROW_FLAG_WARN, "13:05:44", "912.0 / 1011.0 Hz", "-",
                   "UNMATCHED");
            addRow(h.rows, CASCADE_ROW_FLAG_MUTED, "12:55:02", "1050.0 Hz single", "-",
                   "single tone");
            addRow(h.rows, 0u, "11:48:19", "746.8 / 879.0 Hz", "258",
                   "Motorola Quick Call II");
            break;
        case CASCADE_INSTRUMENT_NAV_BEARING:
            // Pole Hill (POL, 112.10 MHz), a real Lancashire VOR, on a radial
            // whose reciprocal is worth reading: 094 out, 274 back. The three
            // levels are consistent with each other rather than picked to look
            // busy - the plugin derives confidence as the geometric mean of the
            // reference and variable qualities, so sqrt(0.91 * 0.97) is the
            // 0.94 beside them and the face's three bays agree.
            h.title = "VOR DEMO";
            s.values[0] = 94.0;
            s.values[1] = 0.94;
            s.values[2] = 0.91;
            s.values[3] = 0.97;
            setText(s, 0, "POL");
            s.flags = CASCADE_INSTRUMENT_FLAG_LOCK;
            break;
        case CASCADE_INSTRUMENT_FAX:
            // A chart part way through: the working standard everywhere in
            // NOAA's schedule is IOC 576 at 120 lines per minute, a receiver
            // 23 Hz low is realistically mistuned rather than comically so,
            // and 412 lines is about a third of a ten-minute transmission -
            // enough paper out of the slot to see it feeding.
            h.title = "Radiofax DEMO";
            s.values[0] = 576.0;
            s.values[1] = 120.0;
            s.values[2] = 412.0;
            s.values[3] = -23.0;
            setText(s, 0, "PICTURE");
            s.flags = CASCADE_INSTRUMENT_FLAG_LOCK;
            h.headings = {"Time", "Event"};
            addRow(h.rows, CASCADE_ROW_FLAG_GOOD, "+03:26", "Line 400");
            addRow(h.rows, 0u, "+02:41", "Line 300");
            addRow(h.rows, CASCADE_ROW_FLAG_GOOD, "+00:36",
                   "Phased, 120 lpm, tuning -23 Hz");
            addRow(h.rows, 0u, "+00:06", "Start tone, IOC 576");
            addRow(h.rows, CASCADE_ROW_FLAG_MUTED, "+00:00", "Listening");
            break;
        case CASCADE_INSTRUMENT_BEACON:
            // The identity, the country and the protocol wording are C/S T.001
            // Annex B's OWN worked example, which is also what the 406 MHz
            // beacon plugin's tests decode - so the demonstration face shows
            // the same strings a real burst produces rather than invented ones.
            // The carrier error is a plausible +430 Hz against a five kilohertz
            // scale, and the age is twelve seconds into a fifty second burst
            // period.
            h.title = "406 MHz beacon DEMO";
            setText(s, 0, "ADCD00800440401");
            setText(s, 1, "366 United States of America");
            setText(s, 2, "Serial User Protocol - float-free EPIRB");
            setText(s, 3, "none (user protocol)");
            s.values[0] = 430.0;
            s.values[1] = 12.0;
            s.flags = CASCADE_INSTRUMENT_FLAG_ALERT;
            h.headings = {"Time", "Hex ID", "Country", "Channel"};
            addRow(h.rows, CASCADE_ROW_FLAG_WARN, "14:31:58", "ADCD00800440401", "366 USA",
                   "406.0250 MHz (ch B)");
            addRow(h.rows, 0u, "14:31:08", "ADCD00800440401", "366 USA",
                   "406.0250 MHz (ch B)");
            addRow(h.rows, 0u, "14:30:16", "ADCD00800440401", "366 USA",
                   "406.0250 MHz (ch B)");
            break;
        case CASCADE_INSTRUMENT_METER:
            // A neighbourhood as an ERT receiver actually sees one: several
            // meters of three commodities, the newest on the face and the
            // rest on the roster beneath it, and a tamper count standing on
            // one of them so the face's flag can be seen doing its job
            // (values[1] = physical 1, encoder 2 -> 1 | (2 << 2) = 9).
            h.title = "ERT meter DEMO";
            setText(s, 0, "28394712");
            setText(s, 1, "ELECTRIC");
            s.values[0] = 48213.0;
            s.values[1] = 9.0;
            h.headings = {"Meter", "Type", "Reading", "Heard"};
            addRow(h.rows, CASCADE_ROW_FLAG_GOOD, "28394712", "ELECTRIC", "48213", "2 s");
            addRow(h.rows, 0u, "19002231", "GAS", "3308", "41 s");
            addRow(h.rows, 0u, "28394881", "ELECTRIC", "10557", "3 min");
            addRow(h.rows, CASCADE_ROW_FLAG_WARN, "51120044", "ELECTRIC", "722901", "6 min");
            addRow(h.rows, CASCADE_ROW_FLAG_MUTED, "40011923", "WATER", "9921", "12 min");
            break;
        case CASCADE_INSTRUMENT_WEATHER_CONSOLE:
            // THREE CHANNELS AND ONLY TWO SENSORS, deliberately. The face's
            // hardest case is the empty compartment - the one that must show
            // the equipment's dashes and not a zero - and a demonstration that
            // filled all three would never draw it. Channel 2 is a THN132N,
            // which is a TEMPERATURE-ONLY sensor: its humidity slot is left at
            // zero, which the slot map defines as "this sensor does not
            // measure humidity", so the face's second no-reading case is on
            // screen as well. The negative reading exercises the minus bar and
            // the blanked tens digit in one figure.
            h.title = "Weather station DEMO";
            s.values[0] = 21.4;
            s.values[1] = -2.6;
            s.values[3] = 48.0;
            s.values[6] = 3.0;  // channels 1 and 2 have a reading; 3 has none
            s.values[7] = 2.0;  // channel 2's sensor reports a low battery
            setText(s, 0, "THGR122NX/THGN123N");
            setText(s, 1, "THN132N/THR238NF");
            s.flags = CASCADE_INSTRUMENT_FLAG_LOW_BATT;
            h.headings = {"Time", "Ch", "Sensor", "Reading"};
            addRow(h.rows, 0u, "14:32:06", "1", "THGR122NX/THGN123N", "21.4 C  48 %RH");
            addRow(h.rows, CASCADE_ROW_FLAG_WARN, "14:31:48", "2", "THN132N/THR238NF",
                   "-2.6 C  BATT LOW");
            addRow(h.rows, 0u, "14:31:26", "1", "THGR122NX/THGN123N", "21.3 C  48 %RH");
            addRow(h.rows, CASCADE_ROW_FLAG_MUTED, "14:30:52", "?8", "id 9A70",
                   "heard, not decoded");
            break;
        default:
            h.title = "Instrument DEMO";
            h.kind = CASCADE_INSTRUMENT_GENERIC;
            setText(s, 0, "sample text in slot zero");
            setText(s, 1, "and slot one");
            s.values[0] = 42.0;
            s.values[3] = 3.14159;
            s.flags = CASCADE_INSTRUMENT_FLAG_LOCK;
            h.headings = {"Column A", "Column B"};
            addRow(h.rows, 0u, "one", "two");
            break;
    }
    return h;
}

}  // namespace

void PluginUi::addDemoInstruments(const std::string& spec) {
    std::size_t start = 0;
    while (start <= spec.size()) {
        std::size_t comma = spec.find(',', start);
        if (comma == std::string::npos) { comma = spec.size(); }
        std::string name = spec.substr(start, comma - start);
        // Trim and lower-case, so "Pager, VOR" is accepted as typed.
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) { name.erase(0, 1); }
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) { name.pop_back(); }
        for (char& c : name) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        if (name == "all") {
            for (std::uint32_t k = CASCADE_INSTRUMENT_GENERIC; k <= CASCADE_INSTRUMENT_WEATHER_CONSOLE; ++k) {
                instruments_.push_back(demoInstrument(k));
                ++demoCount_;
            }
        } else if (!name.empty()) {
            const std::uint32_t k = demoKindByName(name);
            if (k != ~0u) {
                instruments_.push_back(demoInstrument(k));
                ++demoCount_;
            }
        }
        start = comma + 1;
    }
    demoLastStepSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PluginUi::stepDemos(double nowSec) {
    if (demoCount_ == 0u) { return; }
    // Every twelve seconds something "arrives": the sequence advances, the
    // alert toggles, and a figure moves, so the NEW lamp, the ALERT lamp and
    // a live readout can each be watched doing their job.
    if (nowSec - demoLastStepSec_ < 12.0) { return; }
    demoLastStepSec_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (std::size_t i = instruments_.size() - demoCount_; i < instruments_.size(); ++i) {
        HostInstrument& h = instruments_[i];
        ++h.state.seq;
        h.state.flags ^= CASCADE_INSTRUMENT_FLAG_ALERT;
        switch (h.kind) {
            case CASCADE_INSTRUMENT_PAGER: h.state.values[0] += 1.0; break;
            case CASCADE_INSTRUMENT_TELEPRINTER: h.state.values[0] += 1.0; break;
            case CASCADE_INSTRUMENT_NAV_BEARING:
                h.state.values[0] = std::fmod(h.state.values[0] + 7.0, 360.0);
                break;
            case CASCADE_INSTRUMENT_FAX: h.state.values[2] += 60.0; break;
            case CASCADE_INSTRUMENT_METER: h.state.values[0] += 1.0; break;
            case CASCADE_INSTRUMENT_WEATHER_CONSOLE: h.state.values[0] += 0.1; break;
            default: break;
        }
    }
}

void PluginUi::clear() {
    destroyInstances();
    // THE BRIDGES ARE NOT TOUCHED HERE, and that is the whole of the fix for
    // the Survey Engine crash. They used to be freed on this line, which runs
    // BEFORE PluginHost::unloadAll() and therefore while every plugin is still
    // loaded and still holding the table the ABI promised it - a table read
    // moments later by a decoder's destroy(). They are reused by the next
    // rebuild and reclaimed at process exit, after the modules are unmapped.
    tuneRequesters_.clear();
    tuneAllowed_.clear();
    lastDenied_.clear();
    // The level-1 half of the same reset: no plugin is live until the next
    // rebuild attaches it (a call in between is answered DETACHED and its
    // marks and commands are gone), and the grants go with the tune grants,
    // to be re-applied by the owner after the rescan exactly as those are.
    // AFTER destroyInstances, so a plugin saving its settings from destroy()
    // still finds itself live.
    api_->setLiveSet({});
    api_->clearGrants();
}

void PluginUi::destroyInstances() {
    for (TrackInstance& t : trackInstances_) {
        if (t.api != nullptr && t.handle != nullptr) { t.api->destroy(t.handle); }
    }
    trackInstances_.clear();
    trackPluginNames_.clear();
    for (PanelInstance& p : panelInstances_) {
        if (p.api != nullptr && p.handle != nullptr) { p.api->destroy(p.handle); }
    }
    panelInstances_.clear();
    for (InstrumentInstance& in : instrumentInstances_) {
        if (in.api != nullptr && in.handle != nullptr) { in.api->destroy(in.handle); }
    }
    instrumentInstances_.clear();
    instruments_.clear();
    demoCount_ = 0;
    tracks_.clear();
    paths_.clear();
    panels_.clear();
    // THE OBSERVATIONS GO WITH THE INSTANCES THAT PRODUCED THEM. Every entry
    // is keyed on a plugin's display name and a track id it published; once
    // the instances are gone those keys refer to nothing, and a rebuild that
    // brought the same plugin back would inherit a history recorded by a
    // different instance - possibly a different VERSION of the module.
    altTrails_.clear();
}

void PluginUi::poll() {
    if (demoCount_ != 0u) {
        stepDemos(std::chrono::duration<double>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
    }
    tracks_.clear();
    paths_.clear();
    // ARMED HERE, cleared as each track is seen below, and read after every
    // instance has been polled: an entry still unseen belongs to a track no
    // plugin is reporting any more, and goes. Doing it per frame is what keeps
    // the store bounded by what is LIVE rather than by everything ever heard.
    for (AltTrail& a : altTrails_) { a.seenThisPoll = false; }

    for (TrackInstance& ti : trackInstances_) {
        trackScratch_.assign(kMaxTracksPerPlugin, CascadeTrack{});
        const std::int32_t n =
            ti.api->poll_tracks(ti.handle, trackScratch_.data(), kMaxTracksPerPlugin);
        if (n > 0) {
            const std::uint32_t count =
                std::min(static_cast<std::uint32_t>(n), kMaxTracksPerPlugin);
            for (std::uint32_t i = 0; i < count; ++i) {
                const CascadeTrack& t = trackScratch_[i];
                // A track with no position is not a track. Silently dropping
                // beats plotting something at (0,0), which is a real place in
                // the Atlantic and would look like a target.
                if (!(t.latDeg >= -90.0 && t.latDeg <= 90.0) ||
                    !(t.lonDeg >= -180.0 && t.lonDeg <= 180.0)) {
                    continue;
                }
                // WATCHED AS IT IS COPIED, which is the whole of the altitude
                // store: this is the one place in the host where a position
                // and the altitude reported at it are in the same object.
                noteAltitude(ti.name, t);
                HostTrack ht;
                ht.t = t;
                ht.plugin = ti.name;
                tracks_.push_back(std::move(ht));
            }
        }

        if (ti.api->poll_paths == nullptr) { continue; }
        pathScratch_.assign(kMaxPathsPerPlugin, CascadePath{});
        const std::int32_t np =
            ti.api->poll_paths(ti.handle, pathScratch_.data(), kMaxPathsPerPlugin);
        if (np <= 0) { continue; }
        const std::uint32_t pcount =
            std::min(static_cast<std::uint32_t>(np), kMaxPathsPerPlugin);
        for (std::uint32_t i = 0; i < pcount; ++i) {
            const CascadePath& p = pathScratch_[i];
            if (p.points == nullptr || p.count == 0u) { continue; }
            HostPath hp;
            hp.id = bounded(p.id, CASCADE_TRACK_ID_CHARS);
            hp.plugin = ti.name;
            hp.kind = p.kind;
            hp.flags = p.flags;
            // COPIED here: the ABI only guarantees the vertices until the next
            // poll, and the host draws on its own schedule.
            const std::uint32_t n2 = std::min(p.count, kMaxPathPoints);
            hp.points.assign(p.points, p.points + n2);
            paths_.push_back(std::move(hp));
        }
    }

    // THE TRACKS THAT DID NOT REPORT THIS FRAME ARE GONE FROM THE STORE. Not
    // faded, not kept for a while: a trail whose owner has stopped reporting
    // is not drawn at all (see pathPresentation), so its observations can
    // never be asked for again and holding them would be a leak with a
    // plausible-sounding excuse.
    //
    // remove_if IS STABLE, so what survives is still in (plugin, id) order and
    // the binary search in altitudeNear stays valid - which is the one thing
    // this compaction could have quietly broken.
    altTrails_.erase(std::remove_if(altTrails_.begin(), altTrails_.end(),
                                    [](const AltTrail& a) { return !a.seenThisPoll; }),
                     altTrails_.end());

    for (PanelInstance& pi : panelInstances_) {
        rowScratch_.assign(kMaxRowsPerPanel, CascadePanelRow{});
        const std::int32_t n =
            pi.api->poll_rows(pi.handle, rowScratch_.data(), kMaxRowsPerPanel);
        HostPanel& hp = panels_[pi.panelIndex];
        hp.rows.clear();
        if (n > 0) {
            const std::uint32_t count =
                std::min(static_cast<std::uint32_t>(n), kMaxRowsPerPanel);
            hp.rows.assign(rowScratch_.begin(),
                           rowScratch_.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }

    for (InstrumentInstance& ii : instrumentInstances_) {
        HostInstrument& hi = instruments_[ii.index];
        // The host sets the size and ZEROES the rest before every call, so a
        // plugin that fills three slots leaves the other five empty rather
        // than holding last frame's figures.
        CascadeInstrumentState st{};
        st.structSize = static_cast<std::uint32_t>(sizeof(CascadeInstrumentState));
        const std::int32_t r = ii.api->poll_state(ii.handle, &st);
        if (r > 0) {
            hi.state = st;
            hi.have = true;
        } else if (r == 0) {
            // Nothing yet: keep `have` as it was. A face that had a reading
            // and momentarily has none keeps showing the last one, which is
            // what a real instrument does between updates.
        }
        // Text slots are NUL-terminated by contract; enforce it so no drawer
        // can walk off the end of a slot a plugin filled to the brim.
        for (auto& t : hi.state.text) { t[CASCADE_INSTRUMENT_TEXT_CHARS - 1] = '\0'; }
        hi.rows.clear();
        if (ii.api->poll_rows != nullptr) {
            rowScratch_.assign(kMaxRowsPerPanel, CascadePanelRow{});
            const std::int32_t n =
                ii.api->poll_rows(ii.handle, rowScratch_.data(), kMaxRowsPerPanel);
            if (n > 0) {
                const std::uint32_t count =
                    std::min(static_cast<std::uint32_t>(n), kMaxRowsPerPanel);
                hi.rows.assign(rowScratch_.begin(),
                               rowScratch_.begin() + static_cast<std::ptrdiff_t>(count));
            }
        }
    }
}

void PluginUi::noteAltitude(const std::string& plugin, const CascadeTrack& t) {
    // Looked up without building a key (see altKeyLess). bounded() is still
    // what CREATES one below, where it happens exactly once per track ever
    // seen rather than once per track per frame.
    const std::string_view idView(t.id, ::strnlen(t.id, CASCADE_TRACK_ID_CHARS));
    const auto at = std::lower_bound(altTrails_.begin(), altTrails_.end(), idView,
                                     [&plugin](const AltTrail& a, std::string_view key) {
                                         return altKeyLess(a.plugin, a.id, plugin, key);
                                     });
    AltTrail* trail = nullptr;
    if (at != altTrails_.end() && at->plugin == plugin &&
        std::string_view(at->id) == idView) {
        trail = &*at;
    }
    // SEEN IS SET WHATEVER THE ALTITUDE IS, and that ordering is deliberate: a
    // live aircraft whose source has stopped reporting an altitude is still
    // live, and letting this frame's NaN drop the entry would throw away every
    // real observation behind it and blank the colour of a trail that is still
    // on screen.
    if (trail != nullptr) { trail->seenThisPoll = true; }

    // NaN MEANS "NOT REPORTED" per the ABI, so there is nothing to record.
    // Written as a positive test because the negation would accept NaN - every
    // comparison with NaN is false - which is exactly the value being refused.
    if (!std::isfinite(t.altM)) { return; }

    if (trail == nullptr) {
        AltTrail fresh;
        fresh.plugin = plugin;
        fresh.id = bounded(t.id, CASCADE_TRACK_ID_CHARS);
        fresh.seenThisPoll = true;
        // AT ITS lower_bound, which is what keeps the store sorted without a
        // sort: `at` is exactly where this key belongs, and nothing above has
        // invalidated it - the NaN return is the only thing between the search
        // and here, and it touches nothing.
        trail = &*altTrails_.insert(at, std::move(fresh));
    }

    if (!trail->ring.empty()) {
        // The newest entry: the last one appended while the ring is still
        // filling, and the slot before the write cursor once it has wrapped.
        const std::size_t cap = kAltObservationsPerTrack;
        const std::size_t newest = trail->ring.size() >= cap
                                       ? (trail->next + cap - 1u) % cap
                                       : trail->ring.size() - 1u;
        const AltObservation& last = trail->ring[newest];
        const double cosLat = std::cos(t.latDeg * kDegToRad);
        const double movedSq =
            metresSqBetween(last.latDeg, last.lonDeg, t.latDeg, t.lonDeg, cosLat);
        // HORIZONTAL MOVEMENT IS NOT THE ONLY THING WORTH RECORDING. This
        // guard exists to stop a stationary track flushing the ring, but a
        // track can be stationary on the ground and moving in the one
        // dimension this store is about: a helicopter climbing over a pad, an
        // aircraft holding while it descends. Judged on position alone, those
        // froze at one altitude for the whole session. A material altitude
        // change is therefore its own reason to record, and it cannot
        // reintroduce the churn the guard prevents, because a repeated
        // identical report changes neither position nor altitude.
        const bool climbed =
            std::isfinite(last.altM) &&
            std::fabs(t.altM - last.altM) >= kAltObservationMinClimbM;
        if (movedSq < kAltObservationMinMoveM * kAltObservationMinMoveM && !climbed) {
            return;
        }
    }

    AltObservation o;
    o.latDeg = t.latDeg;
    o.lonDeg = t.lonDeg;
    o.altM = t.altM;
    // The bounding box grows with the ring and is deliberately never shrunk
    // when an entry is overwritten: a box that is too LARGE only costs a scan
    // that then finds nothing within tolerance, while one recomputed wrongly
    // could reject a vertex that really does have an observation. Conservative
    // in the one direction that cannot lose data.
    if (trail->ring.empty()) {
        trail->minLat = trail->maxLat = o.latDeg;
        trail->minLon = trail->maxLon = o.lonDeg;
    } else {
        trail->minLat = std::min(trail->minLat, o.latDeg);
        trail->maxLat = std::max(trail->maxLat, o.latDeg);
        trail->minLon = std::min(trail->minLon, o.lonDeg);
        trail->maxLon = std::max(trail->maxLon, o.lonDeg);
    }
    if (trail->ring.size() < kAltObservationsPerTrack) {
        // Still filling, so `next` stays 0 - which is where the oldest entry
        // will be the moment the ring is full and starts overwriting.
        trail->ring.push_back(o);
    } else {
        trail->ring[trail->next] = o;
        trail->next = (trail->next + 1u) % kAltObservationsPerTrack;
    }
}

bool PluginUi::altitudeNear(const std::string& plugin, const std::string& id,
                            double latDeg, double lonDeg, double& outAltM) const {
    const auto at = std::lower_bound(altTrails_.begin(), altTrails_.end(),
                                     std::string_view(id),
                                     [&plugin](const AltTrail& a, std::string_view key) {
                                         return altKeyLess(a.plugin, a.id, plugin, key);
                                     });
    // NOT AN ERROR: this host has never watched that track, so it has nothing
    // to say about it. The caller draws the vertex in the owner's colour.
    if (at == altTrails_.end() || at->plugin != plugin || at->id != id) { return false; }
    const AltTrail* trail = &*at;

    // A NON-FINITE QUERY IS ANSWERED "NO", NEVER "YES". A path vertex is
    // third-party data and may be NaN or infinite; metresSqBetween then
    // returns NaN, and the `d2 >= bestSq` rejection below is FALSE for NaN, so
    // every observation would be accepted in turn and the function would
    // return the last one in ring order as though it had been measured. An
    // adversarial reviewer compiled this file to demonstrate exactly that: a
    // NaN latitude returned true carrying an altitude 2.4 km from the query.
    if (!std::isfinite(latDeg) || !std::isfinite(lonDeg)) { return false; }

    // O(1) REJECT BEFORE AN O(256) SCAN. altitudeNear is called once per trail
    // vertex per path per frame, and the host lets a plugin publish up to
    // kMaxPathPoints vertices - so this scan is the one part of the feature a
    // hostile or merely enthusiastic plugin could turn into real frame time.
    // Almost every rejected vertex is nowhere near this track's observations,
    // and the cached bounding box answers those without touching the ring.
    if (latDeg < trail->minLat - kAltObservationBoundsPadDeg ||
        latDeg > trail->maxLat + kAltObservationBoundsPadDeg ||
        lonDeg < trail->minLon - kAltObservationBoundsPadDeg ||
        lonDeg > trail->maxLon + kAltObservationBoundsPadDeg) {
        return false;
    }

    const double cosLat = std::cos(latDeg * kDegToRad);
    // NEAREST WITHIN THE TOLERANCE, not the first inside it. A trail vertex
    // that falls between two observations belongs to the closer one, and
    // taking whichever happened to be scanned first would make the answer
    // depend on the order the aircraft was heard in.
    double bestSq = kAltObservationToleranceM * kAltObservationToleranceM;
    const AltObservation* best = nullptr;
    for (const AltObservation& o : trail->ring) {
        const double d2 = metresSqBetween(o.latDeg, o.lonDeg, latDeg, lonDeg, cosLat);
        // A POSITIVE test, for the reason noteAltitude's sibling comment gives:
        // `d2 >= bestSq` is false for a NaN d2, so the negation would accept an
        // unmeasurable distance as the new best.
        if (!(d2 < bestSq)) { continue; }
        bestSq = d2;
        best = &o;
        // A metre is the same place by any measure this store can resolve, and
        // it is the ORDINARY case - a trail vertex is usually the very
        // position that was observed. Stopping there is what keeps a full ring
        // from being walked for every vertex of every trail, every frame.
        if (d2 <= 1.0) { break; }
    }
    if (best == nullptr) { return false; }
    outAltM = best->altM;
    return true;
}

std::string PluginUi::tuneKey(const LoadedPlugin& p) {
    // ONE definition of a plugin's identity, in plugin_host, because the stop
    // list is keyed on the same string as the tune grant and two copies of
    // "which file is this" would eventually disagree about a path.
    return pluginKey(p);
}

bool PluginUi::tuneAllowed(const std::string& pluginKey) const {
    // An empty key is what a record with no path produces, and it must never
    // match: otherwise every path-less plugin would share one grant.
    if (pluginKey.empty()) { return false; }
    return std::find(tuneAllowed_.begin(), tuneAllowed_.end(), pluginKey) !=
           tuneAllowed_.end();
}

void PluginUi::setTuneAllowed(const std::string& pluginKey, bool allowed) {
    if (pluginKey.empty()) { return; }
    // Mirrored into the level-1 core, whose copy is the one a plugin thread
    // reads (lock-free, per client) - set_frequency and set_vfo_offset need
    // the same grant request_tune does.
    api_->setTuneGranted(pluginKey, allowed);
    const auto it = std::find(tuneAllowed_.begin(), tuneAllowed_.end(), pluginKey);
    if (allowed && it == tuneAllowed_.end()) {
        tuneAllowed_.push_back(pluginKey);
    } else if (!allowed && it != tuneAllowed_.end()) {
        tuneAllowed_.erase(it);
    }
}

std::int32_t PluginUi::tuneRequestFromPlugin(const std::string& plugin, double centreHz) {
    // A STOPPED PLUGIN IS REFUSED BEFORE ANYTHING ELSE, grant or no grant.
    //
    // It should not be able to get here at all - a stopped plugin is never
    // attached, so it is never handed a bridge - but the bridges outlive the
    // instances by design (see the header: a plugin may keep the pointer, and
    // they are only freed at clear()), so a plugin stopped after it attached
    // still holds a working one. Refusing here rather than trusting that no
    // plugin keeps its pointer is the difference between "stopped" meaning
    // something and meaning "we asked nicely".
    //
    // Not recorded as a requester: that list drives a permission row the user
    // is invited to tick, and inviting them to grant the receiver to a plugin
    // they have switched off would be an odd thing to offer.
    if (stopped_.contains(plugin)) {
        lastDenied_ = plugin;
        return CASCADE_TUNE_DENIED;
    }

    // Recorded whether or not it is allowed, so the GUI can offer the toggle
    // exactly for the plugins that actually want it. A tracker that is denied
    // must be discoverable, not invisible.
    if (std::find(tuneRequesters_.begin(), tuneRequesters_.end(), plugin) ==
        tuneRequesters_.end()) {
        tuneRequesters_.push_back(plugin);
    }
    if (!tuneAllowed(plugin)) {
        lastDenied_ = plugin;
        return CASCADE_TUNE_DENIED;
    }
    // Copied under the lock and called outside it - see servicesMutex_ - so
    // a rescan replacing the services on the GUI thread can neither tear the
    // callable nor wait on a tune that is inside the driver.
    std::function<std::int32_t(double)> tune;
    {
        std::lock_guard<std::mutex> lk(servicesMutex_);
        tune = services_.tune;
    }
    if (!tune) { return CASCADE_TUNE_NO_DEVICE; }
    // NaN and absurd frequencies are refused here rather than handed to a
    // driver: written as a positive test, because the negation would accept
    // NaN (every comparison with NaN is false).
    if (!(centreHz > 0.0 && centreHz < 1e12)) { return CASCADE_TUNE_OUT_OF_RANGE; }
    return tune(centreHz);
}

bool PluginUi::hasServices() const {
    std::lock_guard<std::mutex> lk(servicesMutex_);
    return static_cast<bool>(services_.centreHz);
}

double PluginUi::servicesCentreHz() const {
    std::function<double()> f;
    {
        std::lock_guard<std::mutex> lk(servicesMutex_);
        f = services_.centreHz;
    }
    return f ? f() : 0.0;
}

double PluginUi::servicesRateHz() const {
    std::function<double()> f;
    {
        std::lock_guard<std::mutex> lk(servicesMutex_);
        f = services_.sampleRateHz;
    }
    return f ? f() : 0.0;
}

std::int64_t PluginUi::servicesUnixTimeMs() const {
    std::function<std::int64_t()> f;
    {
        std::lock_guard<std::mutex> lk(servicesMutex_);
        f = services_.unixTimeMs;
    }
    return f ? f() : 0;
}

}  // namespace cascade::core
