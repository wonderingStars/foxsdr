// plugin_ui.hpp - drives the plugin capabilities that contribute to the GUI.
//
// The counterpart to PluginRunner, and deliberately a separate class. The
// runner owns decoder instances and is driven from the DSP thread under a
// lock; everything here is created, polled and destroyed on the GUI thread and
// needs no synchronisation at all. Merging them would have put a mutex around
// per-frame UI polling to protect state the DSP thread never touches, and
// would have tied the lifetime of a satellite tracker - which consumes no
// signal - to the audio chain.
//
// IMAGE DECODERS ARE NOT HERE, and that is the same rule applied honestly:
// they consume samples, so they belong to PluginRunner with the other
// decoders. They were created here while nothing routed samples to them, which
// made a capability that could never produce anything look implemented.
//
// THE TUNE PERMISSION, which is the one security-shaped decision here:
// a plugin that can move the VFO can also fight the user for it, or sit on a
// frequency they did not choose. So the host answers request_tune with
// CASCADE_TUNE_DENIED unless the user has explicitly granted that plugin
// control, the grant is per-plugin, and it defaults to off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_CORE_PLUGIN_UI_HPP
#define CASCADE_CORE_PLUGIN_UI_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"

namespace cascade::core {

// A track as the host holds it: the plugin's POD plus which plugin said it.
// Copied out of the plugin (see the ABI's note on why tracks are copied and
// images are borrowed), so nothing here has a lifetime tied to a module.
struct HostTrack {
    CascadeTrack t{};
    std::string plugin;
};

// A polyline. The plugin owns its vertices only until the next poll, so they
// are copied here too - the host draws on its own schedule and must not hold a
// pointer into a plugin across frames.
struct HostPath {
    std::string id;
    std::string plugin;
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    std::vector<CascadePathPoint> points;
};

// One plugin-declared window, resolved once at create time.
struct HostPanel {
    std::string plugin;
    std::string title;
    std::vector<std::string> headings;   // 1..CASCADE_PANEL_MAX_COLUMNS
    std::vector<CascadePanelRow> rows;   // refreshed each poll
};

// What the host lets a plugin do to the receiver. Supplied by the owner (the
// GUI), so this header stays free of the pipeline and the source stack.
struct HostServices {
    std::function<double()> centreHz;
    std::function<double()> sampleRateHz;
    // Returns a CASCADE_TUNE_* code. The permission check is applied by
    // PluginUi BEFORE this is called, so an implementation only has to do the
    // tuning and report device-level outcomes.
    std::function<std::int32_t(double)> tune;
    std::function<std::int64_t()> unixTimeMs;
};

class PluginUi {
public:
    PluginUi() = default;
    ~PluginUi();

    PluginUi(const PluginUi&) = delete;
    PluginUi& operator=(const PluginUi&) = delete;

    // Installs the callbacks the host offers plugins. Call before rebuild():
    // a plugin's attach() runs during rebuild and may read the receiver
    // immediately.
    void setServices(HostServices services);

    // Creates instances for every loaded plugin that declares a UI or
    // host-client capability. Destroys whatever existed before.
    void rebuild(const std::vector<LoadedPlugin>& plugins);

    // Destroys every instance. Must run BEFORE the plugin host unloads the
    // modules, for the same reason PluginRunner::clear must.
    void clear();

    // Per-frame. Refreshes tracks, paths and panel rows from the plugins.
    void poll();

    const std::vector<HostTrack>& tracks() const { return tracks_; }
    const std::vector<HostPath>& paths() const { return paths_; }
    const std::vector<HostPanel>& panels() const { return panels_; }

    // --- Tune permission -------------------------------------------------
    // WHAT A GRANT IS KEYED ON: the plugin's MODULE FILE NAME, which the host
    // reads off disk, and NOT its display name. The display name comes out of
    // the plugin's own descriptor, so keying on it lets any plugin inherit
    // another's permission simply by claiming its name; the file name it
    // cannot change without replacing the granted file itself, which needs
    // write access to the plugins directory and is already game over.
    //
    // The scan produces one record per file in one directory, so the file name
    // is unique across a scan. Empty when the record has no path, and an empty
    // key never matches a grant.
    static std::string tuneKey(const LoadedPlugin& p);

    // Which plugins have asked to control the receiver at least once, so the
    // GUI can offer the toggle only where it means something. A plugin that
    // never asks never appears. These are tuneKey() values, not display names.
    const std::vector<std::string>& tuneRequesters() const { return tuneRequesters_; }
    bool tuneAllowed(const std::string& pluginKey) const;
    void setTuneAllowed(const std::string& pluginKey, bool allowed);

    // The last refusal, for display: "X asked to tune and was not allowed" is
    // the message that turns a mysteriously idle tracker into an obvious
    // one-click fix.
    const std::string& lastDeniedPlugin() const { return lastDenied_; }

    std::size_t trackCount() const { return tracks_.size(); }

    // --- Called by the C trampolines behind CascadeHostApi ----------------
    // Public because the trampolines are free functions in the .cpp (they must
    // be, to have C linkage-compatible signatures) and cannot reach private
    // members. They are not part of the interface the GUI uses.
    bool hasServices() const;
    double servicesCentreHz() const;
    double servicesRateHz() const;
    std::int64_t servicesUnixTimeMs() const;
    // Applies the per-plugin permission, then forwards. Returns a
    // CASCADE_TUNE_* code.
    std::int32_t tuneRequestFromPlugin(const std::string& plugin, double centreHz);

private:
    struct TrackInstance {
        const CascadeTrackSourceApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
    };
    struct PanelInstance {
        const CascadePanelApi* api = nullptr;
        void* handle = nullptr;
        std::string name;
        std::size_t panelIndex = 0;  // into panels_
    };

    // Bounds on what one plugin may put on screen in a frame. A plugin is
    // third-party code; without a cap a buggy one could ask the host to draw
    // an unbounded number of targets and take the frame rate with it.
    static constexpr std::uint32_t kMaxTracksPerPlugin = 4000;
    static constexpr std::uint32_t kMaxPathsPerPlugin = 64;
    static constexpr std::uint32_t kMaxRowsPerPanel = 2000;
    static constexpr std::uint32_t kMaxPathPoints = 20000;

    void destroyInstances();

    std::vector<TrackInstance> trackInstances_;
    std::vector<PanelInstance> panelInstances_;
    std::vector<HostTrack> tracks_;
    std::vector<HostPath> paths_;
    std::vector<HostPanel> panels_;

    HostServices services_;
    std::vector<std::string> tuneRequesters_;
    std::vector<std::string> tuneAllowed_;
    std::string lastDenied_;

    // Scratch, reused so a per-frame poll allocates nothing.
    std::vector<CascadeTrack> trackScratch_;
    std::vector<CascadePath> pathScratch_;
    std::vector<CascadePanelRow> rowScratch_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_UI_HPP
