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

// How stale a target is allowed to get before the host stops showing it.
//
// THIS IS THE HOST'S JOB, and the ABI says so: CascadeTrack::ageMs exists
// precisely because "the host fades and eventually drops stale targets". A
// plugin that never evicts - and the shipped ADS-B decoder never does - would
// otherwise keep every aircraft it has ever heard on the map and in the list
// for the whole session. Putting the rule here rather than in the drawing code
// means the map, the flight list, the target counts and the web snapshot all
// answer the same question the same way, instead of three of them disagreeing.
//
// THE THRESHOLDS ARE PER KIND because the sources report at wildly different
// cadences, and one number would be wrong for all but one of them:
//
//   AIRCRAFT (ADS-B)  positions arrive about twice a second; dump1090 and
//                     tar1090 drop an aircraft at 60 s of silence, so an
//                     aircraft quiet for 30 s is already anomalous and one
//                     quiet for 60 s has gone - out of range, or landed.
//   VESSEL (AIS)      2-10 s under way, up to 3 minutes at anchor, and Class B
//                     transmits as slowly as every 3 minutes by design. Fading
//                     at 5 minutes is the first point at which silence is not
//                     simply a slow reporting class; 10 minutes is gone.
//   STATION (APRS)    beacons are every 10-30 minutes, and a fixed digipeater
//                     is not "stale" for being quiet - it has not moved. 30
//                     and 60 minutes are one and two missed beacon slots at
//                     the slow end.
//   SATELLITE         a tracker PROPAGATES a position rather than hearing one,
//                     so it should update every frame. Two minutes of no new
//                     position means the propagator has stopped, not that the
//                     satellite went quiet; 10 minutes before dropping, because
//                     an orbit that reappears is far more useful than a gap.
//   UNKNOWN           the host cannot know the cadence of a kind it does not
//                     recognise, so it uses the MOST forgiving rule it has.
//                     Dropping an unfamiliar source on an aircraft's schedule
//                     would erase a target that was behaving perfectly.
//
// Times in milliseconds, to match CascadeTrack::ageMs.
constexpr std::uint64_t kTrackFadeMsAircraft = 30ull * 1000ull;
constexpr std::uint64_t kTrackDropMsAircraft = 60ull * 1000ull;
constexpr std::uint64_t kTrackFadeMsVessel = 5ull * 60ull * 1000ull;
constexpr std::uint64_t kTrackDropMsVessel = 10ull * 60ull * 1000ull;
constexpr std::uint64_t kTrackFadeMsStation = 30ull * 60ull * 1000ull;
constexpr std::uint64_t kTrackDropMsStation = 60ull * 60ull * 1000ull;
constexpr std::uint64_t kTrackFadeMsSatellite = 2ull * 60ull * 1000ull;
constexpr std::uint64_t kTrackDropMsSatellite = 10ull * 60ull * 1000ull;

// How faint a target may get before it is dropped. Not zero: a marker that
// reaches full transparency and only THEN disappears has already been invisible
// for a while, so the user sees a target vanish with no warning at all - which
// is the behaviour fading exists to avoid.
constexpr float kTrackMinAlpha = 0.30f;

// The single visibility rule. Pure: same answer for the same inputs, no state,
// no clock - which is what makes it testable and what makes re-acquisition
// automatic. A dropped target whose plugin hears it again reports a small
// ageMs on the very next poll and is visible on that frame; the host keeps no
// "I dropped this one" memory that would have to be undone.
struct TrackPresentation {
    bool visible = true;
    float alpha = 1.0f;  // 1.0 fresh, ramping to kTrackMinAlpha, then invisible
};
TrackPresentation trackPresentation(std::uint64_t ageMs, std::uint32_t kind);

// How many of `tracks` the rule says to show. The counts beside the map and
// above the flight list must agree with what is drawn: "30 targets" over a
// list of twelve is worse than no count at all.
std::size_t visibleTrackCount(const std::vector<HostTrack>& tracks);

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

// THE SAME RULE, FOR A TRAIL. A CascadePath carries no age of its own - see
// the ABI - so the only honest answer about a trail's staleness is its OWNER's:
// the track with the same id, from the same plugin. Without this the marker
// obeys the rule and the line under it does not, and a dropped target leaves a
// trail starting where its marker would have been and running off into empty
// space - which says "something is here" about the one thing the host has just
// decided is not.
//
// A path with NO matching track keeps today's behaviour (visible, full
// strength): a source may plot a line that is not a target at all - a
// footprint, a predicted ground track, a boundary - and the host has no age
// for it and no business hiding it.
TrackPresentation pathPresentation(const HostPath& path,
                                   const std::vector<HostTrack>& tracks);

// Whether there is anything the map would actually DRAW - which is the only
// honest reason to open the map window on the user's behalf.
//
// Asking "are there any tracks at all" instead is a trap, and was one: a
// source that never evicts (the shipped ADS-B decoder does not) keeps
// reporting targets the staleness rule has dropped, so the window was demanded
// again on every single frame, opened itself over whatever the user was doing,
// showed "0 targets", and could not be closed - the close button cleared the
// flag and the next frame set it straight back. Measured on the application
// with a probe reporting one aircraft at ageMs = 3600000.
//
// Paths count too, and by the same rule: an orphan path with no owning track
// is drawn (see pathPresentation), so it is a real reason to open the window,
// while a trail whose owner has been dropped is not.
bool anyVisibleTarget(const std::vector<HostTrack>& tracks,
                      const std::vector<HostPath>& paths);

// Whether the host may open the map ON THE USER'S BEHALF this frame.
//
// A TRANSITION, not a state, and that is the whole of it: asking
// anyVisibleTarget() every frame and opening the window whenever it says yes
// makes the close button useless in the ORDINARY case, not just the stale one.
// A source that keeps hearing its targets - which is what an ADS-B receiver
// does all day - answers yes on every frame, so the click cleared mapOpen_ and
// the next frame set it straight back. Measured on the application with a
// probe reporting one aircraft at ageMs = 0: the map was still there 10 s
// after a close whose hover highlight was confirmed on the button.
//
// Firing only on nothing -> something keeps what the self-open is for (the
// first target of a session brings the map up without the user hunting for a
// menu) and gives up nothing else: a user who closes it stays closed until the
// air genuinely goes quiet and something new arrives.
inline bool mapSelfOpens(bool hadVisibleLastFrame, bool haveVisibleNow) {
    return haveVisibleNow && !hadVisibleLastFrame;
}

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
