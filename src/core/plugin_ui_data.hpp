// plugin_ui_data.hpp - the DATA the plugin windows draw, with none of the machinery that fills it.
//
// SPLIT OUT OF plugin_ui.hpp (window port group 14, part d; engine-stage4.md section 3 item 3 and the fence ledger). The
// views - the map, the radar scope, the target details, the instrument faces, the decoder surfaces - DRAW a track, a
// path, a panel's rows, an instrument's state and a preset; none of them needs the object that polls the plugins
// (PluginUi), the host table behind it (HostServices, the bridges) or the mute policy. Those stay in plugin_ui.hpp,
// which includes this header, so every file that included plugin_ui.hpp still sees every name here; the views include
// THIS header and the interface target the fence builds will see this one and never plugin_ui.hpp.
//
// A PURE MOVE: not a field, a default or a word of the structs changed. What is here:
//   - HostTrack, HostPath and the visibility rule they are drawn by (trackPresentation, pathPresentation,
//     visibleTrackCount; the fade and drop ages);
//   - HostPanel and HostInstrument, the plugin-declared windows' feed;
//   - PluginWindows, the window's own record of which plugin windows are on screen (pure state);
//   - MutePreset, one preset as the decoder surfaces read it.
// The three functions are DEFINED in plugin_ui.cpp as before (pure, no state); the fence step may give them a file of their
// own beside this header.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_CORE_PLUGIN_UI_DATA_HPP
#define CASCADE_CORE_PLUGIN_UI_DATA_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::core {

// A track as the host holds it: the plugin's POD plus which plugin said it.
// Copied out of the plugin (see the ABI's note on why tracks are copied and
// images are borrowed), so nothing here has a lifetime tied to a module.
struct HostTrack {
    CascadeTrack t{};
    std::string plugin;
};

// WHETHER THE HOST CAN PUT A TRACK ON A MAP (0.99.75).
//
// A source may publish a target it HEARS and cannot place yet - an ADS-B
// aircraft whose position message has not arrived - by setting
// CASCADE_TRACK_FLAG_NO_POSITION and leaving the coordinates NaN, and it may do
// so only when the host has said (CASCADE_STATE_LISTS_UNPLACED_TRACKS) that it
// understands. Such a target is LISTED and never drawn, fitted to, followed,
// counted into the coverage map or called "on screen".
//
// TWO TESTS, AND EITHER ONE IS ENOUGH TO REFUSE. The flag is the contract. The
// finiteness of both coordinates is the defence: an old plugin or a buggy one
// that publishes NaN (or an infinity) without the flag must not put it into a
// renderer, where a NaN projected to the screen compares false against every
// edge and so is not skipped but drawn - at NaN. And a flagged track whose
// coordinates a careless plugin filled in anyway is still unplaced, because the
// flag is the plugin saying so. Written positively (isfinite) because the
// negation of a comparison would accept NaN, which is every value refused here.
inline bool trackHasPosition(const CascadeTrack& t) {
    return (t.flags & CASCADE_TRACK_FLAG_NO_POSITION) == 0u && std::isfinite(t.latDeg) &&
           std::isfinite(t.lonDeg);
}

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
// AN ESTIMATED AIRCRAFT POSITION (0.99.78, CASCADE_TRACK_FLAG_ESTIMATED) is the
// source's last fix carried forward along the aircraft's last course, published
// so that an aircraft at the fringe, heard in bursts tens of seconds apart, does
// not blink off the map between them (a 31,000 ft airliner at 42 nm did, on
// 2026-10-13). It fades from the same 30 s - the age is still the fix's - and is
// dropped at 180 s, which is where Flightradar24 and tar1090 give up on a
// carried-forward position too. The flag is the source's to set; the host never
// invents it, and for every other kind it changes nothing.
constexpr std::uint64_t kTrackDropMsAircraftEstimated = 180ull * 1000ull;
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
// `flags` are the track's CASCADE_TRACK_FLAG_* bits: an aircraft carrying
// CASCADE_TRACK_FLAG_ESTIMATED is dropped at kTrackDropMsAircraftEstimated.
TrackPresentation trackPresentation(std::uint64_t ageMs, std::uint32_t kind,
                                    std::uint32_t flags = 0u);
// The same rule from the track itself - age, kind AND flags - which is the form
// every caller that draws, lists or counts a track uses, so none can forget the
// flags.
inline TrackPresentation trackPresentation(const CascadeTrack& t) {
    return trackPresentation(t.ageMs, t.kind, t.flags);
}
// Whether a track's position is an estimate (0.99.78): the flag, on a track that
// has a position at all.
inline bool trackIsEstimated(const CascadeTrack& t) {
    return (t.flags & CASCADE_TRACK_FLAG_ESTIMATED) != 0u && trackHasPosition(t);
}

// How many of `tracks` the rule says to show. The counts beside the map and
// above the flight list must agree with what is drawn: "30 targets" over a
// list of twelve is worse than no count at all.
//
// PLACED TARGETS ONLY (0.99.75): a target the source hears and cannot place
// (trackHasPosition false) is not a marker on the map, so it is not in this
// number whatever vector it is found in; visibleUnplacedTrackCount counts those.
std::size_t visibleTrackCount(const std::vector<HostTrack>& tracks);

// How many of `tracks` are heard without a position and still inside their
// kind's staleness rule - the second number on the list's heading, "heard
// without a position". The same rule as visibleTrackCount for the same reason
// (the number must be the rows), applied to the other half: an aircraft that
// has been silent for a minute is not "heard" any more, placed or not, and a
// vessel at ninety seconds is fine.
std::size_t visibleUnplacedTrackCount(const std::vector<HostTrack>& tracks);

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
//
// AN OWNER WITH NO POSITION HIDES ITS TRAIL (0.99.75): a target heard and not
// placed (trackHasPosition false) has no marker, and a line leading to nowhere
// says "something is here" about the one thing the host has decided not to draw.
// PluginUi::poll also drops such a path before it is stored; this is the same
// rule for a caller that holds both kinds of track in one vector.
TrackPresentation pathPresentation(const HostPath& path,
                                   const std::vector<HostTrack>& tracks);

// One plugin-declared window, resolved once at create time.
struct HostPanel {
    std::string plugin;
    std::string title;
    std::vector<std::string> headings;   // 1..CASCADE_PANEL_MAX_COLUMNS
    std::vector<CascadePanelRow> rows;   // refreshed each poll
};

// One plugin-declared INSTRUMENT: a face the host draws as equipment, fed
// by the plugin's state (see CASCADE_CAP_INSTRUMENT). `have` is false until
// the plugin has answered poll_state with 1 at least once, and the face is
// then drawn with no reading rather than with zeroes - the distinction this
// product has been bitten by before. The memory rows are optional and follow
// the panel contract exactly.
struct HostInstrument {
    std::string plugin;
    std::string title;
    std::uint32_t kind = 0;
    bool have = false;
    CascadeInstrumentState state{};
    std::vector<std::string> headings;   // empty when the plugin has no memory feed
    std::vector<CascadePanelRow> rows;   // refreshed each poll
};

// WHICH PLUGIN WINDOWS ARE ON SCREEN - a decoder's picture, a plugin's own
// panel - by ImGui window identity. EMPTY AT EVERY LAUNCH AND NEVER SAVED:
// the application starts on the main screen alone (the user's instruction,
// 0.79.1: "if you close the software, that is it for the day"), and a
// plugin's window opens when the user presses its row on the rail and
// closes from its own key. Until 0.79.1 the rule ran the other way - every
// published window appeared by itself and a close was remembered across
// launches in AppConfig::closedWindows, which the host no longer applies.
//
// Pure state, no drawing, so the rule "nothing is shown until asked" can be
// tested without a window: a fresh instance shows nothing, show/hide/toggle
// do what they say, and an identity nobody has asked for stays hidden
// however often a plugin publishes it.
class PluginWindows {
public:
    bool shown(const std::string& id) const { return open_.count(id) != 0u; }
    void show(const std::string& id) { open_.insert(id); }
    void hide(const std::string& id) { open_.erase(id); }
    void toggle(const std::string& id) {
        if (open_.erase(id) == 0u) { open_.insert(id); }
    }
    std::size_t count() const { return open_.size(); }
    // Every identity shown (the window reports which plugins it is showing to the engine: FOXAPP_OP_PLUGIN_USE).
    const std::set<std::string>& ids() const { return open_; }

private:
    std::set<std::string> open_;
};

// One preset, reduced to what the mute decision needs - PLUS what the 0.99.0
// preset bars need to draw and re-apply the same preset, added rather than
// kept as a second cache: this snapshot is already rebuilt on every plugin-
// set change (AppWindow::rebuildMuteStates), which is exactly the cadence the
// bars are not allowed to call count()/get() at any faster than.
struct MutePreset {
    double frequencyHz = 0.0;
    // The preset's channel bandwidth, or 0 when it declares none (most do).
    double bandwidthHz = 0.0;
    // CASCADE_PRESET_DEVICE_CENTRE: the preset moves the DEVICE centre rather
    // than the VFO, so that is the number it must be compared against. Getting
    // this wrong is not cosmetic - a user with a VFO offset dialled in from
    // the last station would press "ADS-B 1090 MHz", land the device exactly
    // on the preset, and be told they are not on it.
    bool deviceCentre = false;
    // The RAW index this preset was read from (get(index, ...)), not its
    // position in this filtered vector - the two disagree the moment any
    // earlier index failed validation. A bar's key press records this index
    // so the deferred apply can re-fetch the same preset from the plugin
    // itself rather than trusting anything carried across frames.
    std::uint32_t index = 0;
    // Bounded already (see cascade::gui::presetLabel): the plugin's own label,
    // or this plugin's display name when it left the label empty. Never the
    // raw ABI bytes, so nothing downstream of this cache has to bound them
    // again.
    std::string label;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_UI_DATA_HPP
