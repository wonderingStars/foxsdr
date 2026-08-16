// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_ui.hpp"

#include <algorithm>
#include <cstring>
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

}  // namespace

// Per-plugin bridge behind CascadeHostApi::ctx. One of these exists for every
// plugin that declares CASCADE_CAP_HOST_CLIENT, because the host has to know
// WHICH plugin is asking - the permission is per-plugin, and a single shared
// context could not tell them apart.
struct HostCtx {
    PluginUi* self = nullptr;
    std::string plugin;
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

        // Host services FIRST. The ABI says attach() runs before any other
        // capability's create(), so a tracker can read the receiver while
        // building its initial state instead of waiting a frame for it.
        if (lp.hostClient != nullptr && lp.hostClient->attach != nullptr) {
            auto bridge = std::make_unique<HostCtx>();
            bridge->self = this;
            bridge->plugin = lp.name;
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
                trackInstances_.push_back(std::move(ti));
            }
        }

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

bool PluginUi::tuneAllowed(const std::string& plugin) const {
    return std::find(tuneAllowed_.begin(), tuneAllowed_.end(), plugin) != tuneAllowed_.end();
}

void PluginUi::setTuneAllowed(const std::string& plugin, bool allowed) {
    const auto it = std::find(tuneAllowed_.begin(), tuneAllowed_.end(), plugin);
    if (allowed && it == tuneAllowed_.end()) {
        tuneAllowed_.push_back(plugin);
    } else if (!allowed && it != tuneAllowed_.end()) {
        tuneAllowed_.erase(it);
    }
}

std::int32_t PluginUi::tuneRequestFromPlugin(const std::string& plugin, double centreHz) {
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
