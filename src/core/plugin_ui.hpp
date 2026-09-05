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
#include <mutex>
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

// --- Observed altitudes along a trail ---------------------------------------
//
// WHY THE HOST KEEPS THIS AT ALL. A CascadePathPoint carries a latitude and a
// longitude and nothing else (see the ABI), so the altitude at a trail vertex
// is genuinely not in the data a plugin hands over, and colouring a trail per
// segment from the path alone would mean inventing it. The way out is not to
// change the ABI - plugins are shipped binaries and the vertex type is fixed -
// but to notice that the host ALREADY SEES the missing number: poll() reads
// every track's latDeg, lonDeg and altM together on every frame. So the host
// records what it saw, and later answers "what altitude did this aircraft have
// when it was HERE" from its own observations.
//
// THAT IS RECORDED OBSERVATION, NOT INTERPOLATION, and the distinction is the
// whole justification. A vertex the host watched the aircraft pass through
// gets the altitude that was reported at that moment; a vertex it never
// observed - a trail that predates this session, a plugin that plots a
// predicted track - is reported honestly as "no altitude here" and draws in
// the owner's single colour, rather than borrowing a neighbouring vertex's
// number and inventing a climb that never happened.
//
// KEYED ON (PLUGIN, TRACK ID), which is the pairing pathPresentation already
// uses to match a trail to its owner and for the same reason: two sources may
// key on the same ICAO address or MMSI, and an id alone is not an identity
// across plugins.

// How many observations one track keeps. An ADS-B trail is 48 vertices spaced
// 200 m or more apart, so 256 covers five times the longest trail the shipped
// decoder can draw and still costs 6 KB for an aircraft - which is the point
// of a bound rather than a growing list: a target heard all day cannot turn
// into unbounded memory, and the oldest observation to fall off the end is by
// construction older than any vertex still being drawn.
inline constexpr std::size_t kAltObservationsPerTrack = 256;

// AND THE AGGREGATE, which the per-track bound alone does not state: the store
// holds one ring per LIVE track, and kMaxTracksPerPlugin is 4000, so a single
// plugin saturating its own cap costs 4000 x 256 x 24 bytes, about 24 MB. That
// is the worst case a deliberately abusive plugin could reach, not anything
// real traffic produces (a busy ADS-B site tracks a few hundred aircraft, some
// 1.5 MB), and it is bounded and reclaimed - the per-frame eviction below
// releases a track's ring as soon as the track stops being published.

// How far a track must have moved since its last observation before another
// one is recorded, in metres.
//
// A CHURN GUARD, not a measurement. Without it a parked aircraft, or one whose
// source re-reports the same position twice a second, would push an identical
// observation every frame and flush every real position out of the ring within
// five seconds - so a trail with genuine history would answer "no altitude
// here" for every vertex of it. 50 m is comfortably below the 200 m minimum
// spacing of a trail vertex, so nothing a trail can draw is ever skipped, and
// comfortably above the metre-scale jitter a reported position carries.
inline constexpr double kAltObservationMinMoveM = 50.0;

// How close an observation has to be to a queried position to answer for it,
// in metres.
//
// BOUNDED FROM BOTH SIDES. It must be UNDER half the 200 m minimum spacing
// between trail vertices, or a vertex could be handed its NEIGHBOUR's altitude
// - and a climbing aircraft's neighbouring vertices are deliberately different
// colours, so that error would draw a band boundary in the wrong place. It
// must also be OVER the difference between a trail vertex and the track
// position it was recorded from, which is zero when the plugin plots the
// positions it reported and a few tens of metres when it resamples them.
//
// 80 m, not 100 m, and the correction is worth recording: 100 m is EXACTLY
// half of 200 m, so it had no margin at all on the side that matters, and a
// vertex landing midway between two observations was decided by which was
// nearer by fractions of a metre. 80 m leaves a real 20 m of it.
//
// AND THE 200 m IS THE SHIPPED DECODER'S, not a rule the host enforces. A
// plugin is free to publish vertices closer together than that, and one that
// does can hand a vertex an observation from just beyond its neighbour. The
// failure is a segment drawn in an adjacent band's colour - a picture slightly
// wrong about where a climb crossed a boundary, never a wrong position and
// never invented data.
inline constexpr double kAltObservationToleranceM = 80.0;

// How much altitude change is worth an observation on its own, in metres.
// Roughly a hundred feet - below the height of any band boundary this store
// feeds, so a climb cannot cross a colour boundary without being recorded, and
// far above the metre-scale wobble a barometric altitude reports while parked.
inline constexpr double kAltObservationMinClimbM = 30.0;

// Slack on the cached bounding box used to reject a query before scanning the
// ring, in degrees. About 120 m of latitude - comfortably more than the
// tolerance above, so the cheap test can only ever admit a query the real
// distance test would then judge properly; it must never reject one the scan
// would have answered.
inline constexpr double kAltObservationBoundsPadDeg = 0.0011;

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

// ---------------------------------------------------------------------------
// Audio mute while a data decoder is running on its own frequency
// ---------------------------------------------------------------------------
//
// WHY THIS EXISTS. An I/Q decoder is handed the whole raw device band and does
// its own tuning inside it; the audio chain, meanwhile, keeps demodulating
// whatever the VFO happens to sit on. On ADS-B at 1090 MHz there is no
// modulation an FM discriminator can make sense of, so the speakers get full-
// scale hiss for as long as the decoder runs - measured on the real
// application at a WASAPI session peak of 0.35-0.44 while the map was tracking
// an aircraft. Turning the volume down is not the answer, because the volume
// is a setting the user has to remember to put back.
//
// WHY IT IS A POLICY AND NOT A FLAG. Three separate things have to agree about
// it - the pipeline (which silences), the Sinks panel and the browser (which
// have to SAY why), and the popup that offers to stop the plugin - and they
// have to agree frame by frame as the receiver moves. Computing it in three
// places is how they come to disagree, so it is computed once, here, from
// values with no ImGui, no pipeline and no plugin handles in them, which is
// also what makes it testable without a radio.
//
// EVERYTHING IN THESE TYPES IS A SNAPSHOT the caller has already gathered.
// Nothing here calls into a plugin: the preset API is the plugin's own code
// and must not be run once per frame per plugin from inside a mute decision.

// One preset, reduced to what the mute decision needs.
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
};

// One plugin's contribution to the decision.
struct MutePlugin {
    std::string key;   // module file name (pluginKey)
    std::string name;  // display name - this is what the popup and banner say
    // ACTUALLY DECODING: not stopped (see PluginStopSet) AND being fed by the
    // runner (see PluginRunner::isFeeding). Both, because a plugin that
    // decodes nothing has no claim on the audio however it came to be idle -
    // the user switched it off, or the receiver is not producing the rate it
    // asked for and its own row says so in orange.
    bool running = false;
    // The EFFECTIVE per-plugin setting: the capability-derived default, with
    // the user's override applied. See muteDefaultForCaps.
    bool mutes = false;
    std::vector<MutePreset> presets;
};

// Where the receiver is, in the two senses a preset can mean.
struct TunePoint {
    double deviceCentreHz = 0.0;  // what the tuner is on
    double tunedHz = 0.0;         // device centre + VFO offset: what is heard
};

// What the frame needs to know, computed in one pass.
struct MuteDecision {
    // At least one running, muting plugin is sitting on one of its presets.
    bool active = false;
    // Display names of exactly those plugins, in the order they were given.
    // Plural because several data decoders can legitimately run at once.
    std::vector<std::string> names;
    // Their module file names, same order - the popup's "stop them" button
    // needs the identity, not the label.
    std::vector<std::string> keys;
};

// How far off a preset still counts as being on it, when the preset declares
// no bandwidth of its own.
//
// THE NUMBER IS BOUNDED FROM BOTH SIDES, which is the only reason to prefer it
// to any other round number. It has to be WIDER than the error between the
// frequency asked for and the frequency a tuner actually lands on - the host
// reads the device back after every retune (see AppWindow::retuneSourceHz) and
// an R820T's synthesiser resolves well under 1 kHz - or the mute would drop
// out the instant it engaged. And it has to be NARROWER than the closest
// channel spacing a user could deliberately tune to, which is 12.5 kHz
// narrowband FM, or stepping one channel off the preset would leave them
// muted on a different signal with no explanation.
inline constexpr double kPresetToleranceHz = 5000.0;

// Whether `tune` is on `preset`.
//
// THE RULE: |f - preset| <= max(bandwidth/2, kPresetToleranceHz), where f is
// the device centre for a device-centre preset and the tuned frequency
// otherwise. The bandwidth half-width is what makes a preset that declares a
// passband mean its passband; the floor is what makes the common case (a
// preset with bandwidthHz == 0) mean anything at all.
//
// Deliberately NOT "can the decoder still work from here". An I/Q decoder
// handed 2.4 MHz of band would still hear 1090 MHz from 1 MHz away, but the
// user's model - the one the popup is written to - is the frequency in the
// readout, not the edge of the device's band.
bool onPreset(const MutePreset& preset, const TunePoint& tune);

// The default for a plugin that has never been overridden: ON for a plugin
// that consumes I/Q, OFF for everything else.
//
// I/Q IS THE LINE, and it is a statement about what the audio chain can
// possibly be doing. A CASCADE_CAP_IQ_DECODER plugin takes the raw band and
// tunes inside it, so the demodulated channel it leaves behind is by
// construction not the signal being decoded. An AUDIO decoder is the opposite
// case: it is fed the very audio the speakers get, so SSTV's warble and RTTY's
// diddle are the sound of it working, and some people tune by ear. Defaulting
// those to muted would take away a diagnostic.
//
// THE CAPABILITY BIT IS THE WHOLE RULE, and no list of plugin names belongs
// here beside it. An earlier draft of this comment and of the README named
// APRS and POCSAG among the I/Q decoders; measured on the running application
// with no overrides set, both come up CLEAR - they decode the demodulated
// channel, and at 2 MS/s it was AIS alone that complained about the raw I/Q
// rate. A user who read that list and then looked at the APRS row found the
// opposite of what it promised. What a plugin consumes is the plugin's own
// declaration, it changes with every release, and the checkbox on its row is
// the only statement of it that cannot go stale.
bool muteDefaultForCaps(std::uint32_t capabilities);

// The whole decision for one frame.
MuteDecision muteActive(const std::vector<MutePlugin>& plugins,
                        const TunePoint& tune);

// Whether ANY of `keys` is still a running, muting plugin.
//
// THIS IS WHAT TELLS A TUNE FROM A STOP, and it is not a detail. Both make the
// mute go away, and only one of them is worth a dialog. Asking instead whether
// any muting plugin is running at all gets it wrong the moment a second one
// exists: measured on the running application, stopping ADS-B from its own row
// while parked on 1090 MHz raised the tune-away dialog - "ADS-B is still
// running and is muting the audio" - about a plugin that had just been stopped,
// because AIS happened to be running 900 MHz from ITS preset and answered
// "yes, something mutes". The question has to be about the plugins that were
// actually holding the audio down.
bool anyStillRunning(const std::vector<MutePlugin>& plugins,
                     const std::vector<std::string>& keys);

// The EDGE the popup fires on: on-preset last frame, off-preset this frame.
//
// An edge and not a state, for the reason the map self-open is an edge (see
// mapSelfOpens): a modal re-opened from a level test would be re-opened every
// frame, which is not a dialog, it is a lock-up. Re-arming happens by the same
// token - the user has to come back to a preset before leaving one can ask
// again.
inline bool tuneAwayEdge(bool prevOnPreset, bool nowOnPreset) {
    return prevOnPreset && !nowOnPreset;
}

// WHAT THE TUNE-AWAY DIALOG IS ABOUT, captured when it opens.
//
// A dialog that re-reads the live decision every frame is a dialog that can
// change its own question after the user has read it, and this one did.
// Measured on the running application with AIS and ADS-B both loaded: tuning
// off 162 MHz opened it saying "AIS is still running and is muting the audio",
// and tuning on from there to 1090 MHz left the SAME open dialog saying
// "ADS-B is still running..." with a button offering to stop ADS-B - while the
// receiver sat exactly on ADS-B's own preset, the one place the design
// promises never to ask. The desktop's controls are behind the modal, but the
// web API, CAT, a plugin's request_tune and the scanner all move the receiver
// from outside it, and the scanner does it with nobody watching.
//
// So the names and the keys are COPIED here on the edge and the popup reads
// nothing else. Everything that can end the question - arriving on a preset,
// or the plugins it named being stopped from somewhere else - withdraws it,
// because a question whose answer no longer applies has to disappear rather
// than be re-aimed at a new target.
struct MutePopupSubject {
    bool open = false;
    std::vector<std::string> names;  // what the sentence says
    std::vector<std::string> keys;   // what the button acts on
};

// One frame of that state machine.
//
//   onPreset       - the receiver is on a muting plugin's preset right now
//                    (MuteDecision::active). Withdraws, always: on a preset
//                    the mute is explained by where the radio is pointed, the
//                    Sinks panel says so, and offering to stop the decoder the
//                    user has just tuned to would be worse than saying nothing.
//   tuneAway       - the falling edge (tuneAwayEdge). Captures `names`/`keys`.
//   subjectRunning - are the plugins the question is about still running and
//                    still muting (anyStillRunning over the CAPTURED keys).
//                    False withdraws: a stop is not a tune-away, and a button
//                    offering to stop something already stopped does nothing.
//
// Anything else leaves the state exactly as it was, which is what makes the
// captured names outlive a live decision that has moved on.
MutePopupSubject advanceMutePopup(const MutePopupSubject& prev, bool onPreset,
                                  bool tuneAway, bool subjectRunning,
                                  const std::vector<std::string>& names,
                                  const std::vector<std::string>& keys);

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

    // The plugins the user has STOPPED, by module file name (see
    // PluginStopSet). Applied by the next rebuild(): a stopped plugin is not
    // attached, gets no track source and no panel, and its targets, trails and
    // rows leave the map and the list with the instances that produced them.
    //
    // IT ALSO CLOSES THE TUNE PATH, which is the half that is not obvious. A
    // host bridge outlives the instance that was handed it — a plugin may keep
    // the pointer, and this class only frees the bridges at clear() — so a
    // stopped plugin with a stale bridge could still call request_tune and
    // move the receiver. tuneRequestFromPlugin refuses a stopped plugin
    // outright for that reason.
    //
    // The set SURVIVES clear(), unlike the tune grants beside it, and the
    // difference is deliberate: a grant is re-applied from the config after
    // every rescan (see AppWindow::applyPluginTuneGrants), whereas a stop must
    // hold from the moment the module is scanned, before anything can be
    // re-applied. A stop that evaporated in clear() would let a stopped plugin
    // run for one rebuild after every rescan.
    void setStopped(std::vector<std::string> keys) { stopped_.set(std::move(keys)); }
    bool isStopped(const std::string& pluginKey) const {
        return stopped_.contains(pluginKey);
    }

    // Destroys every instance. Must run BEFORE the plugin host unloads the
    // modules, for the same reason PluginRunner::clear must.
    void clear();

    // Per-frame. Refreshes tracks, paths and panel rows from the plugins.
    void poll();

    const std::vector<HostTrack>& tracks() const { return tracks_; }
    const std::vector<HostPath>& paths() const { return paths_; }
    const std::vector<HostPanel>& panels() const { return panels_; }

    // THE ALTITUDE THIS HOST OBSERVED AT A POSITION, for the track `id`
    // published by `plugin`. True and `outAltM` filled when an observation
    // within kAltObservationToleranceM exists (the NEAREST one, so a query
    // that lands between two takes the closer rather than the first found);
    // false, and `outAltM` untouched, when there is none.
    //
    // FALSE IS AN ANSWER, not a failure. It means "this host never saw this
    // aircraft here" - the vertex predates the session, or belongs to a
    // predicted track that was never flown - and the caller must draw it as
    // unknown rather than reach for a neighbouring vertex's number.
    //
    // Const and cheap, because it is called once per trail vertex per frame:
    // no allocation, no transcendental per observation, and a binary search
    // for the track rather than a scan - a linear one would have made drawing
    // the trails quadratic in the number of live tracks, and the host's own
    // cap allows four thousand of them per plugin.
    bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg,
                      double lonDeg, double& outAltM) const;

    // The plugins that currently HAVE a track instance, by display name (the
    // same name every HostTrack::plugin carries), in load order. This is what
    // gives each track-capable plugin a map page of its own: CAPABILITY, not
    // content — an ADS-B page with no aircraft decoded yet still exists,
    // because "the page is there but empty" answers "where is my map" and
    // "the page is missing" does not.
    const std::vector<std::string>& trackPluginNames() const {
        return trackPluginNames_;
    }

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

    // How many tracks the altitude store is holding rings for. Exposed for
    // one reason: a test that only checks "the lookup stops answering" cannot
    // tell an entry that was RELEASED from one whose ring was merely emptied,
    // and the second still leaks a little memory for every track ever heard.
    // An adversarial reviewer demonstrated exactly that mutant surviving.
    std::size_t altitudeTrackCount() const { return altTrails_.size(); }

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

    // One thing the host saw, exactly as the plugin reported it.
    struct AltObservation {
        double latDeg = 0.0;
        double lonDeg = 0.0;
        double altM = 0.0;
    };
    // One track's history. The ring is a vector rather than a fixed array so
    // an entry stays cheap to move when the store is compacted: a live-track
    // list is erased from every frame, and 6 KB of by-value ring per element
    // would make that shuffle the whole store around.
    struct AltTrail {
        std::string plugin;
        std::string id;
        std::vector<AltObservation> ring;  // grows to the cap, then overwrites
        std::size_t next = 0;              // oldest slot, once the ring is full
        // The ring's bounding box, so a vertex nowhere near this track can be
        // refused without walking 256 observations - see altitudeNear. Grown
        // on append and never shrunk: too large merely costs a scan that finds
        // nothing, too small would refuse a vertex that has an answer.
        double minLat = 0.0, maxLat = 0.0, minLon = 0.0, maxLon = 0.0;
        // Set by poll() for every track the plugins reported THIS frame; the
        // entries still clear at the end of it are tracks that have gone, and
        // they are dropped. That is what bounds the store by LIVE tracks
        // rather than by everything ever heard.
        bool seenThisPoll = false;
    };

    // Records one observation for (plugin, id), applying the NaN rule and the
    // movement guard. Called from poll() only.
    void noteAltitude(const std::string& plugin, const CascadeTrack& t);

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
    // Mirror of trackInstances_'s names, kept so trackPluginNames() can hand
    // out a reference every frame instead of building a vector per call.
    std::vector<std::string> trackPluginNames_;
    std::vector<HostTrack> tracks_;
    std::vector<HostPath> paths_;
    std::vector<HostPanel> panels_;

    // GUARDED, because a plugin does not only call these from the GUI thread.
    // The ABI documents the host table for the GUI thread, but Satellites
    // 1.0.1 read the clock from its own background worker, and the GUI thread
    // replaces this struct on every rescan and source change
    // (refreshPluginRunner). A std::function torn mid-move on one thread and
    // called on another is undefined behaviour, and undefined behaviour
    // inside a plugin's noexcept thread entry ends in abort(). The trampolines
    // copy what they need under this lock and call it after releasing it.
    mutable std::mutex servicesMutex_;
    HostServices services_;
    PluginStopSet stopped_;
    std::vector<std::string> tuneRequesters_;
    std::vector<std::string> tuneAllowed_;
    std::string lastDenied_;

    // What the host has watched each live track do. Bounded twice over: by the
    // number of tracks the plugins are reporting right now (see
    // AltTrail::seenThisPoll), and by kAltObservationsPerTrack within each.
    //
    // KEPT SORTED BY (plugin, id) - see altKeyLess in the .cpp, which also
    // says why, and which every mutation of this vector has to preserve.
    std::vector<AltTrail> altTrails_;

    // Scratch, reused so a per-frame poll allocates nothing.
    std::vector<CascadeTrack> trackScratch_;
    std::vector<CascadePath> pathScratch_;
    std::vector<CascadePanelRow> rowScratch_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_UI_HPP
