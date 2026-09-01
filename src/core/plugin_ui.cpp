// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>

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
};

namespace {

// The C trampolines. Each recovers the bridge from ctx; a null ctx cannot
// happen through our own code but is checked anyway, because these pointers
// are handed to third-party code that may keep them longer than it should.
double hostCentre(void* ctx);
double hostRate(void* ctx);
std::int32_t hostTune(void* ctx, double centreHz);
std::int64_t hostTime(void* ctx);

}  // namespace

PluginUi::~PluginUi() { clear(); }

void PluginUi::setServices(HostServices services) { services_ = std::move(services); }

namespace {

// Storage for the bridges. Kept in a file-scope owner rather than in the class
// so the header does not have to expose HostCtx; cleared by PluginUi::clear.
std::vector<std::unique_ptr<HostCtx>>& ctxStore() {
    static std::vector<std::unique_ptr<HostCtx>> store;
    return store;
}

double hostCentre(void* ctx) {
    auto* c = static_cast<HostCtx*>(ctx);
    if (c == nullptr || c->self == nullptr || !c->self->hasServices()) { return 0.0; }
    return c->self->servicesCentreHz();
}

double hostRate(void* ctx) {
    auto* c = static_cast<HostCtx*>(ctx);
    if (c == nullptr || c->self == nullptr || !c->self->hasServices()) { return 0.0; }
    return c->self->servicesRateHz();
}

std::int32_t hostTune(void* ctx, double centreHz) {
    auto* c = static_cast<HostCtx*>(ctx);
    if (c == nullptr || c->self == nullptr) { return CASCADE_TUNE_FAILED; }
    return c->self->tuneRequestFromPlugin(c->plugin, centreHz);
}

std::int64_t hostTime(void* ctx) {
    auto* c = static_cast<HostCtx*>(ctx);
    if (c == nullptr || c->self == nullptr || !c->self->hasServices()) { return 0; }
    return c->self->servicesUnixTimeMs();
}

}  // namespace

void PluginUi::rebuild(const std::vector<LoadedPlugin>& plugins) {
    destroyInstances();

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
            auto bridge = std::make_unique<HostCtx>();
            bridge->self = this;
            // The permission key, NOT the display name: the name is the
            // plugin's own to choose, so keying on it would let any module
            // inherit a granted one's permission by adopting its name.
            bridge->plugin = tuneKey(lp);
            bridge->api.structSize = static_cast<std::uint32_t>(sizeof(CascadeHostApi));
            bridge->api.ctx = bridge.get();
            bridge->api.centre_hz = &hostCentre;
            bridge->api.sample_rate_hz = &hostRate;
            bridge->api.request_tune = &hostTune;
            bridge->api.unix_time_ms = &hostTime;
            const CascadeHostApi* apiPtr = &bridge->api;
            ctxStore().push_back(std::move(bridge));
            lp.hostClient->attach(apiPtr);
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
    }
}

void PluginUi::clear() {
    destroyInstances();
    // The bridges outlive the instances by design (a plugin may hold the
    // pointer), but not the unload: they are freed here, before the host
    // unmaps anything.
    ctxStore().clear();
    tuneRequesters_.clear();
    tuneAllowed_.clear();
    lastDenied_.clear();
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
    tracks_.clear();
    paths_.clear();
    panels_.clear();
}

void PluginUi::poll() {
    tracks_.clear();
    paths_.clear();

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
    if (!services_.tune) { return CASCADE_TUNE_NO_DEVICE; }
    // NaN and absurd frequencies are refused here rather than handed to a
    // driver: written as a positive test, because the negation would accept
    // NaN (every comparison with NaN is false).
    if (!(centreHz > 0.0 && centreHz < 1e12)) { return CASCADE_TUNE_OUT_OF_RANGE; }
    return services_.tune(centreHz);
}

bool PluginUi::hasServices() const { return static_cast<bool>(services_.centreHz); }

double PluginUi::servicesCentreHz() const {
    return services_.centreHz ? services_.centreHz() : 0.0;
}

double PluginUi::servicesRateHz() const {
    return services_.sampleRateHz ? services_.sampleRateHz() : 0.0;
}

std::int64_t PluginUi::servicesUnixTimeMs() const {
    return services_.unixTimeMs ? services_.unixTimeMs() : 0;
}

}  // namespace cascade::core
