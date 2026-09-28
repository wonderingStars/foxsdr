// engine.hpp - THE RECEIVER'S ENGINE: its state and, from stage 3a on, its
// machinery, moved out of the window (engine extraction stage 3a; the plan is
// docs/ENGINE-EXTRACTION.md in the foxsdr-api repository, the record of this
// stage docs/engine-stage3.md).
//
// WHAT IT OWNS. The Pipeline, the open radio and the workers that open and
// scan radios, the recorders, the transmitter, the plugin host, runner and
// host services, the patch page's runtime and its radios, the scanner, the
// bookmarks, the receiver part of the configuration, the telemetry sender,
// and the one receiver snapshot every reader answers from. The window
// (gui::AppWindow) holds a reference to one and asks it; the window keeps
// what is drawn and how.
//
// THE LINE IS ENFORCED, NOT REMEMBERED: this file and everything in
// src/engine compile into cascade_engine, which may not include a gui/
// header, imgui, GLFW or OpenGL (checked at configure time and by
// cascade_engine_link_check).
//
// THREADING. Stage 3a changes none: everything here is still called on the
// GUI thread, exactly where the window called it before (the per-frame order
// is in docs/engine-stage3.md). Stage 3b moves it onto the engine's own
// control thread; every GUI-THREAD-ONLY note in the code below is a checklist
// item for that (docs/engine-stage3.md lists them).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_ENGINE_HPP
#define CASCADE_ENGINE_ENGINE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/airspy_settings.hpp"
#include "core/app_commands.hpp"
#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "core/gps_reader.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/patch_recordings.hpp"
#include "core/patch_runner.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/recorder.hpp"
#include "core/retune_coalescer.hpp"
#include "core/scanner.hpp"
#include "core/telemetry.hpp"
#include "core/transmitter.hpp"
#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/engine_host.hpp"
#include "engine/running_view.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tune_control.hpp"
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/pluto_source.hpp"
#include "source/pluto_tx.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"
#include "source/soapy_source.hpp"
#include "source/soundcard_source.hpp"
#include "usb/usb_device.hpp"

// The window and its tests are named, never included: the engine does not
// depend on them. Stage 3a's window still reaches the engine's members
// directly (as friends); stage 4 replaces that with the engine API's table.
namespace cascade::gui {
class AppWindow;
struct AppWindowTestAccess;
}  // namespace cascade::gui

namespace cascade::engine {

class Engine {
public:
    // A HEADLESS ENGINE: its host is its own EngineHost, whose answers are
    // the defaults in engine_host.cpp (clocks from std::chrono, no views).
    Engine();
    // An engine held by a front end that answers the host's questions itself
    // (gui::AppWindow). `host` must outlive the engine.
    explicit Engine(EngineHost& host);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

private:
    friend class cascade::gui::AppWindow;
    friend struct cascade::gui::AppWindowTestAccess;

    // WHO THIS ENGINE ASKS (engine_host.hpp). Declared first, so it is there
    // before anything that could ask it is built.
    EngineHost ownHost_;
    EngineHost& host_;

    // NO DEFAULT MEMBER INITIALISERS, on purpose: GCC refuses them on a
    // nested struct used by an inline static member of the enclosing class
    // ("required before the end of its enclosing class"). testHooks_ below is
    // value-initialised, which makes both pointers null all the same.
    struct TestHooks {
        // Replaces makeDeviceSource's construction when set: the "radio" a
        // worker or the restore opens is the test's own recording fake.
        std::unique_ptr<cascade::source::DeviceSource> (*makeDevice)(const std::string& kind);
        // Replaces scanNative's USB walk when set: the list it returns IS the
        // native device list, so no test ever enumerates the desk's radios.
        std::vector<cascade::source::NativeDeviceInfo> (*nativeScan)();
        // Replaces PortAudio for the Source section's SOUND CARD when set:
        // the list scanSoundCards asks for and every backend an open makes
        // (tests/test_soundcard_app_paths.cpp), so no test lists or opens
        // the desk's audio inputs.
        std::shared_ptr<cascade::source::SoundCardBackend> (*soundCardBackend)();
        // Replaces PluginHost::defaultPluginDir() in rescanPlugins when set:
        // tests/test_apply_command.cpp loads its fixture plugin from its own
        // scratch directory, never from beside the test binaries, where every
        // other test that builds an AppWindow would load it too.
        std::string (*pluginDir)();
        // Replaces SoapySource::enumerate on scanSoapy's worker when set: the
        // device-scan commands are applied in tests without a vendor probe
        // ever touching the desk's radios (it opens and resets what it finds).
        std::vector<cascade::source::SoapyDeviceInfo> (*soapyScan)();
    };
    // Set by the test before any AppWindow exists and never changed while one
    // does, so the worker threads that read makeDevice race with nothing.
    inline static TestHooks testHooks_{};
    // Once-a-second check that the output stream is still alive, reopening it
    // if it is not. See AudioOut::streamAlive() for what kills one; the short
    // version is that a dead sink is invisible from inside the app, so the
    // only fix is to keep asking.
    void pollAudioHealth();
    // Collects a finished asynchronous audio-device open. Called once per
    // frame, BEFORE pollAudioHealth: the watchdog must not judge a sink that
    // an open has just handed back.
    void pollAudioOpen();
    // The same collect for the microphone's gate (micOpen_). Once per frame,
    // whether or not the TX page is drawn, so an open that finishes while the
    // page is closed is still collected and its worker released.
    void pollMicOpen();
    // Asks for an output device through audioOpen_, and applies the result
    // immediately when the device answered inside the bound. `recovery` marks
    // a request the audio watchdog made rather than the user, so only those
    // are counted as recoveries. Returns true when the open completed here.
    bool requestAudioOpen(int deviceIndex, bool recovery);
    // The GUI-thread half of an open, wherever it completed: republishes the
    // channel layout for the DSP thread, re-enumerates, and puts the Sinks
    // combo back on the device that is actually playing.
    void applyAudioOpenResult();
    // Takes every live plugin handle off the pipeline and the UI, in the one
    // order that is safe, then unmaps the modules. The ONLY way any code here
    // may call PluginHost::unloadAll() — see the note in its body.
    void detachAndUnloadPlugins();
    // Runs one SoapySDR enumeration into soapyDevices_ and re-points the
    // combo selection at the active device by args (labels can repeat; a
    // device that vanished from the scan leaves sourceSel_ = -1 and the
    // preview falls back to the live source name). Called from the combo's
    // first open, from Refresh and from the web interface's scanDevices —
    // deliberately never from the constructor (see soapyDevices_ below for
    // why).
    //
    // NEVER WHILE A RADIO IS OPEN (0.90.1). The scan's child-process probe
    // opens and resets every dongle it finds - the streaming one included -
    // and the 0.90.0 field report (NESDR SMArt v5, 2026-09-09) is our next
    // control call dying twelve seconds after exactly that. While a device is
    // open the scan is deferred instead: the list stays as it is, the open
    // device is given a row if it has none, one diag line says why, and
    // soapyScanned_ is left false so the next draw after the radio closes
    // scans as before. The decision itself is gui::deviceScanAllowed.
    void scanSoapy();
    // True while scanSoapy() is refusing to run (see deviceScanAllowed): the
    // Refresh key is disabled with the reason as its caption. Read every
    // frame the Source section draws, so it is never stale.
    bool soapyScanGated() const;
    // The name a deferral names the open radio by - the sanitised model of
    // deviceArgs_, the label of an open in flight, or a radio this session
    // could not release.
    std::string soapyScanGateDevice() const;
    // What scanSoapy() would do right now (2026-09-23): a whole scan, one that
    // leaves the open radios' drivers out, or nothing - built from the
    // receiver's radio and every patch radio. See engine/device_scan_plan.hpp.
    cascade::gui::SoapyScanPlan soapyScanPlan() const;
    // Combo-row click handler: 0 = generator, 1 = IQ file (panel only — the
    // pipeline switches on a successful Open), 2+i = soapyDevices_[i]
    // (opens immediately; on failure the combo selection is left unchanged).
    // A device row carries the receiver's air centre across to the radio it
    // opens (carriedAirCentre); `carryAirHz`, when given, is carried instead -
    // the patch page's hand-back, whose frequency belongs to the radio it
    // took, not to the generator standing in for it.
    void selectSource(int idx, std::optional<double> carryAirHz = std::nullopt);
    // The Pluto row's Open key: closes the radio in use and opens the board at
    // the address typed in plutoUri_, carrying the air frequency read BEFORE
    // the close. Its own member so the converter test can press it.
    void openPlutoFromBox();
    // ONCE A FRAME, and once more after the last frame: takes the fix the
    // reader accepted, if there is one, and hands it to applyReceiverPosition
    // - the only door a position enters by. The single-shot takeFix() is
    // what keeps a 60 Hz poll from re-applying it (and resetting the
    // coverage map) sixty times a second.
    void pollGpsReader();
    // WHAT "SET RX HERE" ACTUALLY DOES, as a function, because there are now
    // THREE ways to say where the antenna is: the toolbar's fields, the
    // satellites window's coordinate cells, and a click on that window's map
    // while SET FROM MAP CLICK is armed. Every one of them has to move every
    // page's home, tell the scope, discard the coverage accumulated from the
    // old origin and put the toolbar's fields back in step - and a second
    // hand-written copy would sooner or later do only some of that.
    //
    // The pair is REFUSED, not clamped, outside -90..90 / -180..180: a typo
    // must not be able to install a receiver at the pole and quietly make
    // every distance on the window wrong. Answers whether it applied.
    bool applyReceiverPosition(double latDeg, double lonDeg);
    // Tunes to a preset, sets the mode/bandwidth/device rate it asks for,
    // rebuilds the decoders against the new receiver state, and opens what
    // that plugin contributes. The ONLY callers are a button and the deferred
    // preset-bar/web-remote apply paths, each of which has already re-read
    // and re-validated `ps` from the plugin itself: a preset is a plugin
    // publishing where it listens, never a plugin retuning the radio — that
    // still needs the separate per-plugin permission.
    void applyPluginPreset(const cascade::core::LoadedPlugin& p, const CascadePreset& ps);
    // THE ONE ENUMERATION, used by drawPluginPresets, maybeAutoPreset,
    // rebuildMuteStates and the web status snapshot: walks `p`'s preset table
    // (capped at kMaxPresetsPerPlugin, exactly as every consumer has always
    // capped it) and keeps only what cascade::gui::presetIsValid accepts,
    // paired with the RAW index get() was called with — the index a deferred
    // apply must record, because it is not the same number as this preset's
    // position in the returned (filtered) vector. Empty when p.preset is
    // null. This is a real call into third-party code and must only be
    // called on a plugin-set change, never once per frame per window — see
    // muteStates_ and the long comment on rebuildMuteStates for why.
    std::vector<cascade::gui::IndexedPreset> validatedPresets(
        const cascade::core::LoadedPlugin& p) const;
    // Grants or revokes one plugin, updating both the live PluginUi and the
    // persisted list. One function so the two can never disagree: a grant that
    // took effect but was not saved would come back revoked next launch.
    // `pluginKey` is a PluginUi::tuneKey() — the module file name, never the
    // display name, which the plugin itself chooses.
    void setPluginTuneAllowed(const std::string& pluginKey, bool allowed);
    // Pushes pluginTuneAllowed_ into pluginUi_. Called after every
    // PluginUi::rebuild, because rebuild follows a clear() that drops the
    // grants along with the instances — without this a rescan silently revoked
    // every permission the user had given.
    void applyPluginTuneGrants();

    // --- HOST API LEVEL 1 (0.99.31) - see core/plugin_api.hpp ---------------
    //
    // The SETTINGS grant, the level-1 twin of the tune grant: same keying,
    // same persistence, same re-application after every rebuild.
    void setPluginSettingsAllowed(const std::string& pluginKey, bool allowed);
    void applyPluginSettingsGrants();
    // ONCE A FRAME, straight after applyWebControls: applies what plugins
    // asked of the receiver (through applyControlRequest, the code a click or
    // a browser goes through), turns their log lines into decoder-output lines
    // and plate notices, folds their settings into the config, and collects
    // their marks. GUI thread - the only thread that may touch the receiver,
    // which is the whole reason plugin requests are queued. What plugins READ
    // is the one receiver snapshot publishReceiverState publishes later in the
    // same frame, before anything is drawn.
    void applyPluginApi();
    // ONE control request (the web remote, CAT, a plugin): a TRANSLATION, and
    // nothing else - net::controlRequestToCommands turns its fields into
    // commands in the order they were always applied, and each goes through
    // applyCommand below.
    void applyControlRequest(const cascade::net::ControlRequest& r);

public:
    // --- ONE CONTROL PATH (engine extraction, stage 1) ----------------------
    //
    // THE ONE PLACE THE RECEIVER'S STATE CHANGES. Every desktop widget, key
    // binding and gesture that changes the receiver, the web remote, CAT and
    // plugins all end here as a FoxCommand (the engine API's vocabulary,
    // third_party/foxsdr_api; app-internal extensions in
    // core/app_commands.hpp). docs/engine-stage1.md has the op table, the
    // rule for when a widget's command is queued and when it is applied at
    // once, and the line between receiver state and view state; tests/
    // test_command_path_guard.cpp fails a widget that changes the receiver
    // any other way. GUI thread only - the same rule every setter it calls
    // has always had. `longText` is the command's text when it did not fit
    // FoxCommand::text (a long path); empty otherwise.
    FoxCommandResult applyCommand(const FoxCommand& c, const std::string& longText = {});
private:
public:
    // What a widget or key does: queue a command for the top of the next
    // drain. Drained twice at the top of every frame (drawUi): once before the
    // keyboard is read - what the widgets asked for last frame - and once
    // after, so a key still acts in the frame it is pressed.
    void submitCommand(const FoxCommand& c);
private:
public:
    void submitCommand(cascade::core::cmd::QueuedCommand q);
private:
public:
    void drainLocalCommands();
private:
    // Each queued command carries the sourceGen_ it was asked under. A
    // command addressed to THE RADIO (rate, gain, AGC, antenna, bias tee,
    // device switch) whose radio has since been closed or replaced is
    // dropped at the drain rather than landing on whatever is open now:
    // every assignment of device_ bumps sourceGen_, so an unchanged
    // generation means the very device the widget was drawn for
    // (docs/engine-stage1.md, "queued device commands").
    struct LocalCommand {
        cascade::core::cmd::QueuedCommand q;
        std::uint64_t sourceGen = 0;
    };
    std::vector<LocalCommand> localCommands_;

    // Helpers the commands call, each one the body a widget or a branch of
    // applyControlRequest used to hold inline (so both paths now share it).
    // GUI thread, like applyCommand.
    bool startIqRecording();
    void openIqFile(const std::string& path);
    void openPlutoAt(const std::string& args);
    void tuneToBookmark(const cascade::core::Bookmark& b);
    void addBookmarkHere(const std::string& name);
    cascade::core::Scanner::Params scannerParams() const;
    bool selectSourceById(const std::string& id);
    // HOW MANY LOADED MODULES ARE DECODERS AT ALL - the denominator under the
    // word DECODERS, and under the rail's fed-of-fitted chip.
    //
    // It is the RUNNER'S OWN TEST, not a new opinion: PluginRunner creates an
    // instance for a module that supplies a decoder, an I/Q decoder or an
    // image decoder table, and for nothing else (see its rebuild). Counting
    // every loaded module instead put a basemap and a track-info provider -
    // neither of which can ever be fed a signal - permanently in the
    // denominator of "of N installed, M not fed", so a perfectly healthy
    // receiver read as two decoders broken.
    std::size_t loadedDecoderCount() const;
    // HOW MANY OF THOSE ARE BEING FED - the numerator over the same
    // population, counted the same way: per MODULE, from
    // PluginRunner::isFeeding, which answers from the very status list the
    // fitted window's rows are drawn from.
    //
    // NOT PluginRunner::activeCount, which counts INSTANCES. A module may
    // declare both an audio decoder and an I/Q one and get an instance for
    // each, so an instance count over a module count is two populations in one
    // chip - and can read 3/2. It also does NOT test the receiver's run state:
    // "matched to the rate the pipeline is configured for" is a different
    // question from "the DSP threads are turning", and the callers that need
    // both check pipeline_.running() beside this, exactly as they always did.
    std::size_t fedDecoderCount() const;
    // Whether the user has stopped this plugin. `pluginKey` is a module file
    // name (cascade::core::pluginKey), the same identity the tune grant uses.
    bool pluginIsStopped(const std::string& pluginKey) const;
    // Records a stop or a start WITHOUT rebuilding: updates the durable list
    // and pushes it into the runner and the UI half, so the next rebuild sees
    // it. Split from the button's action below because applyPluginPreset has
    // to start a plugin and then rebuild ONCE, having also moved the receiver.
    void recordPluginStopped(const std::string& pluginKey, bool stopped);
    // The Stop/Start button's action: record it, then rebuild through the one
    // lifecycle path everything else uses, so a stop tears the plugin's
    // instances down and a start builds them against the CURRENT receiver.
    void setPluginStopped(const std::string& pluginKey, bool stopped);
    // "WE WANT THE USER TO HAVE TO DO NOTHING" (the owner's words). Called
    // from setPluginStopped's own START branch only: looks the plugin back up
    // by key, and if it carries presets and the receiver is not already
    // sitting inside one of them (engine/tune_control.hpp's
    // autoPresetIndexOnStart), applies the first exactly as if its own button
    // had been pressed. A no-op for a plugin with no preset table, and never
    // called on a stop or from config load — see the call site in
    // setPluginStopped and core::startupState for why neither reaches here.
    void maybeAutoPresetOnStart(const std::string& pluginKey);
    // THE SAME RULE, for the gesture the DECODE rail actually offers: opening
    // a plugin's own window. Since 0.79.1 a window is shown ONLY by a row's
    // click (never restored at start-up, never self-opened - PluginWindows
    // starts empty every launch and MapPage::open is cleared by
    // core::startupState), so that click is exactly as deliberate an "I want
    // this plugin now" as pressing START. Call ONLY when
    // cascade::gui::autoPresetTriggersOnWindowClick says this frame's click
    // just turned a window from hidden to shown - never on a click that hides
    // one. Shares its decision and apply path with maybeAutoPresetOnStart
    // through the private maybeAutoPreset() below; only the log line's verb
    // differs ("window opened" here, "started" there).
    void maybeAutoPresetOnShow(const std::string& pluginKey);
    // The body both of the above call: find the plugin by key, decide via
    // autoPresetIndexOnStart, apply through applyPluginPreset, and log with
    // `verb` standing in for what just happened ("started" / "window
    // opened"). `verb` is a string literal from the two call sites, never
    // plugin-supplied text.
    void maybeAutoPreset(const std::string& pluginKey, const char* verb);
    // THE REVERSE OF cascade::core::pluginKey(): HostImage/HostPanel/
    // HostInstrument/MapPage all carry a plugin's DISPLAY name (LoadedPlugin::
    // name), the same identity drawPluginWindowRows and drawMapPageSections
    // build their window ids from - never the module FILE NAME
    // maybeAutoPresetOnShow needs to look the plugin back up by, the same
    // key setPluginStopped/recordPluginStopped use. Empty when no loaded
    // plugin answers to `displayName` (an unloaded or since-removed module),
    // which the two callers below treat as "nothing to auto-preset".
    std::string pluginKeyForDisplayName(const std::string& displayName) const;
    // The rows a STOP ALL key or DECODER_STOP_ALL judges: every loaded
    // decoder, with whether the user stopped it and whether it is fed.
    std::vector<cascade::gui::RunnableDecoder> runnableDecoders() const;
    // The user's own presets for one loaded plugin, in saved order (see
    // core/user_presets.hpp). Keyed by the version-stripped module id, so a
    // plugin update keeps them.
    std::vector<cascade::core::UserPreset> userPresetsForPlugin(
        const cascade::core::LoadedPlugin& p) const;

    // --- Audio mute while a data decoder is running (see plugin_ui.hpp) -------
    // The EFFECTIVE "mute audio while running" setting for one plugin: the
    // default its capabilities imply, flipped if the user has overridden it.
    bool pluginMutes(const cascade::core::LoadedPlugin& p) const;
    // Records the user's choice as an override of the capability default, so
    // ticking the box back to the default REMOVES the entry rather than
    // recording a second kind of "yes". Rebuilds the mute snapshot.
    void setPluginMutes(const cascade::core::LoadedPlugin& p, bool mutes);
    // Rebuilds muteStates_ from the loaded plugins: identity, running state,
    // effective setting, and the plugin's presets.
    //
    // A SNAPSHOT REBUILT ON CHANGE, not read per frame, because reading the
    // presets means CALLING the plugin - count() and get() are its own code -
    // and doing that once per plugin per frame to decide whether to be quiet
    // would put third-party code on the frame path for no gain. Presets are a
    // property of the plugin and not of a running instance (the ABI says so),
    // so they cannot change without a rescan.
    void rebuildMuteStates();
    // Once per frame: evaluate the policy against where the receiver actually
    // is, push the result into the pipeline, and arm the popup on the edge.
    void updateAudioMute();
    // Stops exactly the plugins named by `keys`, through the ordinary stop
    // path, in one rebuild. The caller passes the keys its own message named -
    // the banner passes mutedByKeys_, the popup passes what it captured - so a
    // button can never stop something other than what the words above it said.
    void stopMutingPlugins(const std::vector<std::string>& keys);
    // "ADS-B decoder", or "ADS-B decoder and AIS decoder", or a comma list.
    // One place, because the popup, the banner and the Sinks panel all have to
    // name the same plugins the same way.
    static std::string muteNameList(const std::vector<std::string>& names);
    std::string muteSubjectText() const;
    // Opens or closes the transmit radio. Bounded, and on the GUI thread -
    // opening a Pluto is a TCP connect with iiod::kConnectWait on it, not a
    // USB enumeration, so it is fast enough not to need a worker.
    void openTransmitRadio();
    void closeTransmitRadio();
    // Pushes the receiver's centre into the transmitter when SPLIT is off,
    // and nothing when it is on. Called once a frame.
    void followTransmitFrequency();
    // Moves decoded lines out of the runner into decoderLog_. Called from
    // drawUi unconditionally, because the runner's buffer is bounded and
    // draining only when the panel is visible would drop output silently.
    void pumpDecoderOutput();
    // Rebuilds every decoder instance against the CURRENT source rate and
    // centre frequency. Called after any source change, because both are
    // passed to a decoder's create() and cannot be changed afterwards.
    void refreshPluginRunner();
    // Band plan (optional program data) and plugins (optional user
    // installs) — both silently absent when their directory does not exist.
    void loadBandPlan();
    void rescanPlugins();
    // Every tune that moves the SOURCE centre has to tell the pipeline, which
    // cannot see it: the RDS/stereo decoders must forget the old station.
    //
    // For a hardware (Soapy) source this is a REQUEST, paced through
    // retuneCoalescer_: bursts (one wheel notch per frame is 60-144 tunes a
    // second) collapse to at most one device call per ~50 ms, latest value
    // winning — the gesture that produced the most frequent 0.62.0 field
    // crash. A single tune still applies immediately. The generator and IQ
    // file sources apply immediately always (no USB to pace).
    //
    // isPluginPreset: true only from applyPluginPreset, and only so a
    // mismatch this retune produces (see noteTuneMismatch) can say the
    // PRESET needs a receiver that covers that band, rather than leaving an
    // unexplained tune. Every other caller takes the default.
    void retuneSourceHz(double centerHz, bool isPluginPreset = false);
    // The unpaced apply: setCenterFrequencyHz + decoder resets + readback.
    // Call directly only where the readback must be valid on return (the
    // carry-across on a fresh device open); everything else goes through
    // retuneSourceHz.
    void applyRetuneNow(double centerHz, bool isPluginPreset = false);
    // Frame-loop poll releasing a held retune once its interval has passed.
    void pollPendingRetune();
    // Compares what applyRetuneNow asked for against what the source actually
    // landed on (SoapySDR devices coerce; the generator and IQ file never
    // do) and, past kTuneMismatchToleranceHz, sets tuneMismatchNote_ (shown
    // in the Source section) and logs once per distinct request. Pulled out
    // of applyRetuneNow only so its one non-trivial decision — the wording,
    // in engine/tune_control.hpp's tuneMismatchMessage — stays testable without
    // a device.
    void noteTuneMismatch(double requestHz, double answeredHz, bool isPluginPreset);
    // The refusal's counterpart: a tune the source would not make at all,
    // reported only when the request lies outside the range the radio itself
    // publishes (engine/tune_control.hpp, tuneRefusedMessage). Same note line,
    // logged once per distinct request.
    void noteTuneRefused(double requestHz, bool isPluginPreset);
    double lastRefusedRequestHz_ = -1.0;

    // Uninstalls the matching pipeline tap, THEN stops the recorder — the
    // order the Recorder contract requires (see Pipeline::set*Recorder).
    // Both are harmless no-ops when nothing is recording, so the toolbar
    // Stop path calls them unconditionally.
    void stopIqRecording();
    void stopAudioRecording();
    // A TAKE BELONGS TO ONE UNBROKEN RUN OF ONE SOURCE. Four things end it
    // besides the Stop buttons in the Recorder section, and each is one
    // routine below so no path can forget:
    //
    // stopReceiver - THE ONE WAY A USER STOPS THE RECEIVER. The STOP dome,
    // the Start/Stop key, the radar scope's POWER button and
    // applyControlRequest (the web remote and plugins through the host API;
    // CAT can read the run state but has no command that sets it) all call
    // it. Through 0.99.35 only the dome and the key ended the takes: a stop
    // from the remote or the POWER button left both recorders open with
    // zero-length headers on disk, and the next start appended to the same
    // files across the gap. The takes end when the receiver is running or
    // FAULTED (a latched fault is proof it was running): a take armed on a
    // cleanly stopped receiver ("press Play to feed the recorders") is not
    // ended by a stop that stops nothing, exactly as the dome, which reads
    // START there, cannot end it either.
    //
    // endTakesOnFault - once per frame. The frame that first sees the
    // pipeline's fault latch ends both takes and says why on screen and in
    // the log: the fault drops the run flag, so nothing else would, and a
    // START or the automatic reopen of a SoapySDR radio would otherwise
    // restart the receiver with the recorders still hooked in and splice
    // the same take across the gap.
    //
    // startReceiver - THE ONE WAY THE RECEIVER IS STARTED (dome, key, POWER,
    // web remote and plugins, the SoapySDR recovery restart, and the startup
    // start). A start clears the fault latch, so it asks endTakesOnFault
    // first, while the latch is still up (a fault that landed after this
    // frame's check would otherwise vanish unseen), and then forgets the old
    // edge, so a second fault within a frame of the START is a new one.
    //
    // installSource - every pipeline_.setSource() goes through it, and it
    // ends the I/Q take BEFORE the swap: an I/Q recording is one source's
    // baseband, and a switch at the same rate (the generator standing in for
    // a radio the patch page borrowed, say) kept writing the new source's
    // samples into it. The AUDIO take carries on: it records what the
    // speaker plays, whose 48 kHz format no source change touches, and a
    // retune - which changes what it hears just as much - never ended it.
    //
    // endTakes - the shared end: taps out, headers patched, the reason in
    // the diagnostic log and on screen and the web page - as an ERROR
    // (recordError_, which lights the web FAIL lamp) for a fault, kept beside
    // any error already showing, or as a NOTICE (recordNotice_, which does
    // not) for a source the user chose to change. Returns whether anything
    // was recording.
    //
    // tests/test_stop_ends_recordings.cpp holds every pipeline_.stop() in
    // src/gui to stopReceiver and run()'s teardown, every
    // pipeline_.start() to startReceiver, every pipeline_.setSource() to
    // installSource, and drives the stop, the faults and the same-rate
    // switch through the real application.
    void stopReceiver();
    void startReceiver();
    void endTakesOnFault();
    void installSource(std::unique_ptr<cascade::source::IqSource> src);
    bool endTakes(bool iq, bool audio, const char* why, bool asError);
    bool faultSeen_ = false;  // endTakesOnFault's edge: the latch as last frame saw it
    // The Recorder section's own "Record audio" path, lifted out of the button
    // so the keyboard presses the SAME button rather than a second copy of it
    // that could drift from the one on screen. Returns whether a take started;
    // the error, when it did not, is in recordError_ exactly as before.
    bool startAudioRecording();
    // The Radio section's own mode-button path, lifted out for the same
    // reason: a mode key must set the demodulator, its default bandwidth and
    // the log line identically to a click on the button beside it.
    void setModeIndex(int index);
    // THE USER'S OWN MUTE, kept apart from the plugin mute (mutedBy_ and the
    // rest). They are different things with different lifetimes: a plugin's
    // mute is recomputed from the tuning every frame in updateAudioMute, and
    // one that also cleared a user's mute would make the Mute key stop working
    // the moment a decoder was running. The pipeline is told the OR of the two.
    bool userMuted_ = false;

    // ONE absolute-tune path shared by bookmark click-to-tune and scanner
    // retunes: commands the SOURCE center to (absHz - VFO offset) through
    // activeSource().setCenterFrequencyHz — the same setter + readback path
    // the toolbar digit wheel uses — so the VFO band (whose offset is
    // preserved) lands on absHz and the display follows the readback.
    void tuneAbsoluteHz(double absHz, bool isPluginPreset = false);
    // The tuned station: source center readback + VFO offset (what the VFO
    // band marks on the spectrum). This is what a bookmark captures and what
    // the scanner's user-tune detection compares.
    double currentAbsoluteHz();

    // Once-per-GUI-frame scanner driver (called at the end of drawUi):
    // detects manual tunes (user wins -> stop), feeds tick() with ImGui's
    // clock and the squelch-open state, applies returned retunes.
    void scannerFrame();

    // Persists the bookmark list after a mutation; failures land in
    // bookmarkError_ (red text). No-op in hermetic mode (empty path).
    // DEBOUNCED since 0.99.19: it marks the list dirty and the write happens
    // about a second after the last change (flushBookmarkSave, every frame and
    // at exit) - a 33 000-entry list takes ~40 ms to write, and a hitch on
    // every star clicked is exactly the slowdown an imported list must not add.
    void saveBookmarks();
    void flushBookmarkSave(bool force);
    // Imports an SDR# frequencies.xml or a CSV into the bookmarks.
    void importBookmarkFile(const std::string& path);

    // Opens a radio of `kind` ("soapy" or one of the eight native driver keys)
    // by its args on
    // THIS thread, pushes the requested rate and the default gains, and fills
    // the panel mirrors. Null (with sourceError_ set) when the open fails.
    // Used by the config restore, which happens before there is a frame to
    // draw and therefore has nothing to keep responsive; the dropdown's own
    // path goes through launchDeviceOpen onto a worker.
    std::unique_ptr<cascade::source::DeviceSource> openDeviceSync(const std::string& kind,
                                                                  const std::string& args,
                                                                  double requestRateHz);

    // Constructs an unopened driver of `kind`; null for a kind this build
    // does not know. One place decides what a kind name means, so the
    // worker, the synchronous restore and any future caller cannot disagree.
    static std::unique_ptr<cascade::source::DeviceSource> makeDeviceSource(
        const std::string& kind);

    // Re-reads nativeDevices_ and nativeUnbound_ from the transport. Cheap,
    // ungated and safe at any time - see nativeDevices_ for why a native
    // enumeration is nothing like a Soapy scan.
    void scanNative();

    // WHERE EACH FAMILY'S ROWS START IN THE SOURCE COMBO. Row 0 is the
    // generator, row 1 the IQ file and row 2 the sound card; the native radios
    // come next, and the SoapySDR devices after them. Named rather than written
    // as "3" at the dozen sites that index this list, because one of those
    // sites forgetting that the native block exists is an off-by-N that opens
    // the wrong radio.
    static constexpr int kSoundCardRow = 2;
    static constexpr int kNativeRowBase = 3;
    int soapyRowBase() const {
        return kNativeRowBase + static_cast<int>(nativeDevices_.size());
    }

    // The label of the native row whose args are `args`, or the args
    // themselves when no row matches (a device that has since been
    // unplugged). Used for the model string a log line names the radio by.
    //
    // THE DRIVER IS PART OF THE MATCH (0.99.36): an RSP's native Mirics row
    // and its SDRplay API row carry the SAME args ("serial=..."), and matching
    // args alone named an API-opened RSP1 "Mirics MSi2500" in the 0.99.27 log.
    std::string nativeLabelFor(const std::string& kind, const std::string& args) const;

    // The Source combo, one key per row, index-aligned with it - see
    // gui::refindSourceRow, which keeps the selection on the same radio when
    // a re-scan moves the rows (0.99.36).
    std::vector<cascade::gui::SourceRowKey> sourceRowKeys() const;
    // The end of every native scan: the selection found again by what it
    // names (rowsBefore = sourceRowKeys() before the list changed).
    void followSourceRowAfterRescan(const std::vector<cascade::gui::SourceRowKey>& rowsBefore);

    // Fills every panel mirror (rates, gains and their ranges, AGC, antenna)
    // from an open DeviceSource, priming the hardware where the panel has to
    // push a value to agree with it. Shared by finishDeviceOpen and
    // openDeviceSync so the async and synchronous opens cannot drift apart -
    // they had two copies of this before, and they had already drifted.
    // `args` is what the device was opened with; the RTL-SDR's bias tee rule
    // needs it to know WHICH dongle this is.
    void adoptDeviceMirrors(cascade::source::DeviceSource& dev, const std::string& kind,
                            const std::string& args, double requestRateHz);

    // Makes the DSP chain follow activeSource().sampleRateHz() (rate-follow).
    // A pipeline refusal — fractional channel rate — keeps the old chain and
    // surfaces the reason in sourceError_.
    void followInputRate();

    // DSP pipeline plus the two live display widgets it feeds. The views are
    // held by unique_ptr for two reasons: the forward declarations above, and
    // the waterfall's GL texture, whose deletion needs the creating GL context
    // current — run() tears the view down explicitly before destroying the
    // context, because AppWindow itself outlives it (destroyed in main()).
    cascade::core::Pipeline pipeline_;

    // Display range for both the spectrum axis and the waterfall colormap.
    float dbMin_ = -110.0f;
    float dbMax_ = 0.0f;

    // Radio/Sinks control state. The pipeline owns the live DSP values; these
    // mirrors exist because ImGui widgets edit by pointer. Defaults match the
    // pipeline's own defaults (WFM, 150 kHz bandwidth, -50 dB squelch) except
    // the VFO offset, which the constructor pushes to +300 kHz so the demo
    // tone 0 sits on the VFO — near-silent in WFM (an unmodulated carrier
    // demodulates to DC), a clean 700 Hz sidetone in CW.
    float volume_ = 0.5f;
    int modeIndex_ = 1;                              // WFM
    float vfoOffsetKhz_ = 300.0f;
    // WHICH OF THE OFFERED BANDWIDTH STEPS THE VFO IS ON, or -1 for none of
    // them: a plugin preset may ask for a width the list does not carry (the
    // NOAA APT one asks for 40 kHz), and the combo then letters the real
    // figure and ticks nothing. It used to be the NEAREST step, which made the
    // control show 12.5k over a 40 kHz VFO and apply that 12.5k on the next
    // click. Never used to index kBwHz without a literal step beside it.
    int bandwidthIndex_ = 1;                         // 150k
    float squelchDb_ = -50.0f;
    // Output devices, enumerated once at construction (a hot-plug refresh can
    // come with the settings work in P5); index into devices_, -1 when empty.
    std::vector<cascade::sink::AudioDevice> devices_;
    int deviceIndex_ = -1;
    // Output-stream watchdog (see pollAudioHealth). The note is shown in the
    // Sinks panel: a stream that had to be restarted is something the user
    // should be told about, because the alternative reading of the same
    // event — audio that stopped and came back on its own — is indistinguish-
    // able from a fault in their radio.
    double lastAudioProbeSec_ = 0.0;
    int audioRecoveries_ = 0;
    std::string audioHealthNote_;
    // THE DEVICE OPEN, OFF THIS THREAD. Field report "hang ntdll.dll @
    // InitializeWaveHandles" (0.96.4): picking an output device put the GUI
    // thread inside waveOutOpen for 57 seconds. See engine/audio_open.hpp for the
    // whole argument; what matters here is that nothing on this thread may
    // query the sink while inFlight() is true.
    cascade::gui::AudioOpen audioOpen_;
    // The AudioOpen::Result tag that says which of the two asked: the audio
    // watchdog reopening a dead stream (counted as a recovery, and the only
    // one that writes the health note) or the user picking a device. Carried
    // by the request rather than kept in a member here, because a click queued
    // behind a reopen would otherwise relabel the open already in flight.
    static constexpr int kAudioOpenByUser = 0;
    static constexpr int kAudioOpenByWatchdog = 1;
    // Once-a-minute starvation digest (see pollAudioHealth). Sampled every
    // frame — not gated behind the 1 Hz watchdog above — because a ring can
    // dip and recover well inside a second at 48 kHz, and a low-water mark
    // read only once a second would miss most of the dips it exists to
    // report. SIZE_MAX so the very first frame's real reading always beats
    // the sentinel instead of needing a separate "have we sampled yet" flag.
    double lastAudioLogSec_ = 0.0;
    std::size_t audioRingLowWaterFrames_ = SIZE_MAX;
    std::uint64_t audioUnderrunsAtLogStart_ = 0;
    std::uint64_t audioPrimingAtLogStart_ = 0;

    // Moves the VFO so the tuned frequency lands on wantAbsHz, snapping to the
    // mode's raster unless the caller says otherwise, and clamping the band
    // inside the baseband span. Shared by click-to-tune and the drag path.
    void setVfoToAbsoluteHz(double wantAbsHz, bool snap);

    int deemphIndex_ = 0;  // index into kDeemphUs; 0 = 50 us (global default)

    std::vector<cascade::source::SoapyDeviceInfo> soapyDevices_;
    bool soapyScanned_ = false;  // one lazy scan done (scanSoapy())
    // AppConfig::lookForNetworkUsrps, and the Soapy args the config named at
    // start-up - both read by scanSoapy() for the UHD rule
    // (gui::soapyDriversWithNoHardware).
    bool lookForNetworkUsrps_ = false;
    std::string startupSoapyArgs_;
    // The drivers the scan in flight left out for having nothing to find,
    // written by the scan's own thread before its future is ready (so read
    // safely once it is) - and the last completed scan's, which is what
    // gui::networkUsrpHint draws from under the Source list.
    std::shared_ptr<std::vector<std::string>> soapyScanAbsent_;
    std::vector<std::string> soapyAbsentDrivers_;
    // A deferral has been logged for the radio currently open. The combo's
    // lazy scan asks on every frame the dropdown is open, so without this the
    // one diag line would be written sixty times a second; cleared the moment
    // the gate opens again (drawSourceSection), so the next radio gets its
    // own line.
    bool soapyScanDeferredLogged_ = false;
    // THE SCAN BESIDE AN OPEN RADIO (2026-09-23; engine/device_scan_plan.hpp).
    // soapyScanSkip_ is the drivers the scan in flight left out - the open
    // radios' own families - and pollSourceAsync keeps the rows of those
    // drivers from the old list, since that scan could not have seen them.
    // soapyScanPartial_ says the last scan was one of those, so the next
    // chance with no radio open does a whole one.
    std::vector<std::string> soapyScanSkip_;
    bool soapyScanPartial_ = false;
    // The patch page asked for a SoapySDR scan when it opened and has not had
    // one yet (the plan was deferring - a radio still opening). See patchReconcile.
    // Only ever set once patchListsWanted_ allows it (0.99.40).
    bool patchScanWanted_ = false;
    // THE DEVICE LISTS A PATCH RADIO IS CHOSEN FROM MAY BE ASKED FOR: the
    // SoapySDR scan (whose vendor probe opens and resets USB radios, and loads
    // modules that have faulted in-process - see the constructor) and the sound
    // card listing. Asked for when the user opens a Radio's device list or
    // presses "Look for radios" - never by showing the patch view, which the
    // application opens on and which is switched to and fro all day: that
    // would run the probe at every launch and every switch. Once asked, for
    // the session. The native list needs no permission: it opens nothing.
    // Written in place by the window (kWindowMayWrite) when the user opens a
    // Radio's device list or presses "Look for radios".
    bool patchListsWanted_ = false;

    // --- Reopening after an absorbed driver fault (0.90.1) -----------------
    // When the automatic reopen was last attempted, in ImGui::GetTime()
    // seconds; negative = never. gui::autoReopenDue holds the next attempt
    // off for kSoapyReopenHoldoffSec after this, so a radio that is really
    // gone is tried once, not in a loop.
    double soapyReopenAttemptSec_ = -1.0;

    // --- Off-thread SoapySDR discovery and open --------------------------
    // SoapySDR::Device::enumerate()/make() do USB bus discovery and, for a
    // B200, an FPGA/firmware load: seconds of blocking work. Run inline they
    // froze the GUI for ~3 s on every source click. Both now run on a worker
    // thread; the GUI polls each frame and applies the result. The device
    // itself is only ever touched by the GUI thread once the future resolves,
    // so no locking is needed beyond the future's own synchronization.
    struct DeviceOpenResult {
        std::unique_ptr<cascade::source::DeviceSource> dev;  // null on failure
        // WHICH DRIVER THE WORKER SHOULD CONSTRUCT, and afterwards which one
        // it did: "soapy", or one of the eight native driver keys - the
        // same spellings
        // AppConfig::sourceKind uses. The kind has to travel with the request
        // because the worker is what decides the concrete type, and it has to
        // come back with the answer because sourceKind_ is set from it.
        std::string kind = "soapy";
        std::string args;
        // THE SOAPY ARGS THE PREFER-NATIVE DECISION WAS MADE FROM, carried
        // along so the worker can fall back to them. Empty for an open the
        // user asked for directly. See launchDeviceOpen: a dongle whose tuner
        // the native driver does not support (E4000, FC0012/13) must still
        // open the way it always did, and by the time that is known the
        // worker is the only thing still holding the request.
        std::string fallbackSoapyArgs;
        std::string error;
        int row = -1;
        double requestRateHz = 0.0;
    // The frequency the user was listening to when they changed device.
    //
    // A newly opened radio sits wherever its driver defaults to - an RTL-SDR
    // comes up at 100 MHz - so without carrying this across, changing device
    // silently retunes the receiver and the audio stops. Captured before the
    // switch because by the time the open finishes, the old source is gone.
    //
    // AN AIR FREQUENCY, AND IT MAY BE NEGATIVE: with the VFO parked above a
    // VLF station the band centre sits below 0 Hz on the air, which through an
    // up-converter is an ordinary tune (core::airReachable). So "nothing to
    // carry" is its own state - no value - and never a zero or a sign
    // (carriedAirCentre decides which). The new radio's converter judges
    // whether it can be delivered.
    std::optional<double> keepCenterHz;
    // THE RADIO FREQUENCY AN RSP IS TUNED TO BEFORE ITS STREAM STARTS (the
    // 0.99.36 pre-Init tune), already converted through that radio's
    // converter on the GUI thread by launchDeviceOpen - the worker has no
    // converter state, and keepCenterHz is an AIR frequency. No value: no
    // pre-tune (nothing to carry, not an RSP, or not deliverable).
    std::optional<double> preTuneRadioHz;
    // AN AIRSPY's REMEMBERED GAIN MODE, GAINS AND DECIMATION (0.99.41), copied
    // from airspyMemory_ on the GUI thread by launchDeviceOpen so the worker
    // can put them on the radio straight after open() and BEFORE it asks for
    // the rate: the saved rate is a decimated one, and asked for first it is
    // matched against undecimated rates and "coerced" to the wrong one.
    std::optional<cascade::core::AirspySetting> airspyAtOpen;
    // THE NATIVE RADIO THIS OPEN FELL BACK FROM (converter key), when the
    // worker opened the dongle through SoapySDR because the native driver
    // refused its tuner; empty otherwise. finishDeviceOpen uses it so the
    // converter set for that radio still applies (converterKeyAlias_).
    std::string fellBackFromKey;
    // WHAT AN AUTOMATIC REOPEN HAS TO PUT BACK (pollSoapyRecovery, 0.90.1).
    // The ordinary open primes every gain to its default and leaves AGC off;
    // a reopen after a driver fault is not a new radio to the user, so the
    // gains, the gain mode and - if the receiver was running when the driver
    // faulted - the running state are restored once the device is up. The
    // antenna needs nothing here: deviceAntenna_ is applied by every open.
    // Carried INSIDE the result rather than in a member so an answer the
    // user has moved on from (asyncOpenStillWanted) drops it with the rest.
    bool recovery = false;
    std::vector<std::string> recoveryGainNames;
    std::vector<float> recoveryGainsDb;
    bool recoveryAgc = false;
    bool recoveryRestart = false;
    // THE RADIO CLOSED TO MAKE THIS ATTEMPT (0.99.36), as the config would
    // have named it, and what it was called. Invalid when nothing was closed.
    // A failed open remembers it (gui::rememberAfterFailedSwitch) so the exit
    // save does not write the generator in its place.
    cascade::gui::RememberedSource closedRadio;
    std::string closedLabel;
    };
    std::future<std::vector<cascade::source::SoapyDeviceInfo>> soapyScanFuture_;
    std::future<DeviceOpenResult> deviceOpenFuture_;
    bool soapyScanPending_ = false;
    bool deviceOpenPending_ = false;
    std::string deviceBusyLabel_;  // device name shown while an open is in flight

    // Paces hardware retunes — see retuneSourceHz. 50 ms: invisible against
    // the wheel gesture, one apply per notch burst instead of one per frame.
    cascade::core::RetuneCoalescer retuneCoalescer_{50.0};
    // The isPluginPreset a retune was requested with, carried across the
    // coalescer alongside its frequency — see retuneSourceHz. Overwritten on
    // every request, so a deferred apply reads the LATEST caller's context,
    // never a stale one from an earlier request the coalescer already
    // superseded.
    bool pendingRetuneIsPreset_ = false;

    // Set by noteTuneMismatch when a retune's readback disagreed with what it
    // asked for by more than kTuneMismatchToleranceHz; drawn in warning
    // colour under the Source controls, "" = the last retune landed where it
    // was asked. NaN so the very first mismatch this session sees is always
    // logged (NaN != NaN), never suppressed by an uninitialised zero that
    // happens to equal a real request.
    std::string tuneMismatchNote_;
    double lastMismatchRequestHz_ = std::numeric_limits<double>::quiet_NaN();
    double lastMismatchAnswerHz_ = std::numeric_limits<double>::quiet_NaN();

    // Source-selection sequence number, incremented by EVERY install of a
    // source into the pipeline (generator, IQ file, or a resolved device).
    // deviceOpenReqGen_ records the value an in-flight open was requested at;
    // asyncOpenStillWanted() compares the two when it resolves. See the
    // predicate's comment above for why a counter and not a flag.
    std::uint64_t sourceGen_ = 0;
    std::uint64_t deviceOpenReqGen_ = 0;

    // Drains a pending device open OFF the GUI thread at shutdown. See the
    // definition for the semantics chosen and what they cost.
    void reapPendingDeviceOpen();
    // The same for a pending device SCAN. Separate because the futures are
    // separate and either may be in flight alone; the definition explains why
    // this reaper has nothing to release where the open reaper has a handle.
    void reapPendingSoapyScan();

    // --- THE SOUND CARD SOURCE (app_window_soundcard.cpp) ---------------------
    // Row kSoundCardRow of the Source combo. Like the IQ file and the Pluto,
    // choosing the row shows its controls and opens nothing; Open does, on a
    // worker, because enumerating and opening an audio device are calls into
    // the host API that can take as long as it likes. See
    // source/soundcard_source.hpp for the source itself.
    struct SoundCardOpenResult {
        std::unique_ptr<cascade::source::SoundCardSource> src;  // null on failure
        std::string error;    // why it failed, or a coerced rate on success
        std::vector<cascade::source::SoundCardDevice> devices;  // the list it opened from
        std::uint64_t gen = 0;  // sourceGen_ when it was asked for
        bool restore = false;   // the startup restore asked, not the user
        cascade::source::SoundCardSettings wanted;  // what was asked for
        // THE SAME CARD WAS RELEASED FIRST (gui::soundCardReopenReleasesFirst):
        // the running card was closed before this open, with `previous` - its
        // settings - to fall back on.
        bool released = false;
        cascade::source::SoundCardSettings previous;
        // `wanted` IS `previous` (gui::soundCardSameSettings): released and
        // tried once, with nothing different to fall back to.
        bool sameAsPrevious = false;
        bool restoredPrevious = false;  // src runs `previous`: `wanted` was refused
        std::string previousRefused;    // ...and why `previous` did not come back either
    };
    // What the controls show, whether or not the card is the source in use -
    // the same rule as the I/Q file's path. The config saves it only when no
    // card is running, lent to the patch page or remembered
    // (gui::soundCardToSave): these are edits until Open is pressed.
    cascade::source::SoundCardSettings soundCard_;
    // The card restoreKeep_ names when its kind is "soundcard" - the saved
    // card a startup restore could not open, the card released for a re-Open,
    // the card handed back by the patch page - as it last RAN (or as the
    // config had it). Meaningful only while restoreKeep_ names a card.
    cascade::source::SoundCardSettings soundCardRemembered_;
    // The settings of the card as it is RUNNING (set when one is installed;
    // meaningful only while sourceKind_ is "soundcard"). The centre box reads
    // its format: a real-mode card has no centre to move. The patch page's
    // loan of the card takes these, never soundCard_ (which may have been
    // edited and not Opened), and so does a re-Open's fallback.
    cascade::source::SoundCardSettings soundCardLive_;
    std::vector<cascade::source::SoundCardDevice> soundCardDevices_;
    bool soundCardListed_ = false;  // soundCardDevices_ holds a finished enumeration
    std::future<std::vector<cascade::source::SoundCardDevice>> soundCardScanFuture_;
    bool soundCardScanPending_ = false;
    std::future<SoundCardOpenResult> soundCardOpenFuture_;
    bool soundCardOpenPending_ = false;
    // The saved card was not in the list at startup: say so under the
    // controls until the user opens something (never replaced by another).
    std::string soundCardMissing_;
    // Enumerate on a worker; the list arrives through pollSoundCard().
    void scanSoundCards();
    // Where a sound card backend comes from: PortAudio, or the test's fake
    // (testHooks_.soundCardBackend). An empty factory means PortAudio.
    static cascade::source::SoundCardSource::BackendFactory soundCardBackendFactory();
    // Open `settings` on a worker; installed by pollSoundCard() on success.
    // When they name the card that is RUNNING, it is released first (see
    // gui::soundCardReopenReleasesFirst) and reopened as it was if the new
    // settings are refused.
    void launchSoundCardOpen(bool restore, const cascade::source::SoundCardSettings& settings);
    // Once per frame: collect a finished enumeration or open.
    void pollSoundCard();
    // At quit, the same grace-then-abandon as reapPendingDeviceOpen.
    void reapSoundCardWorkers();
    // The installed source is a sound card that has stopped (unplugged,
    // taken away, silent past its stall timer) - its own latch or the
    // pipeline's. False for any other source.
    bool installedSoundCardDead();
    // A tune asked of a source whose centre cannot move (a sound card): the
    // VFO moves inside the span instead. True when it handled the request.
    bool retuneFixedCentre(double centerHz);
    // The open args for a patch radio on a sound card, from its key's
    // "device=...,api=...": the Source section's settings when it is the same
    // card, real mono on the left channel otherwise.
    std::string soundCardPatchArgs(const std::string& keyArgs) const;

    // Consumes finished scan/open futures; called once per frame.
    void pollSourceAsync();
    // ONE AUTOMATIC REOPEN AFTER AN ABSORBED DRIVER FAULT (0.90.1); called
    // once per frame after pollSourceAsync. The 0.90.0 field report (NESDR
    // SMArt v5, 2026-09-09): a rate change faulted inside rtlsdr.dll, the
    // guard absorbed it, the device was condemned, and the radio stayed dead
    // - deck reading FAIL - until FoxSDR was restarted, though the fault was
    // on our own call frame and every thread of ours was out of the module.
    // When the open device is dead by such a fault (SoapySource::deadReason
    // == VendorFault - never Abandoned, whose driver still has a thread of
    // ours parked inside it), nothing is in flight, and no attempt was made
    // in the last kSoapyReopenHoldoffSec (gui::autoReopenDue), this closes
    // the dead source exactly as selectSource does and reopens the same args
    // at the same rate through launchDeviceOpen, with the state to restore in
    // the result. A reopen that fails leaves the ordinary failed-open state
    // and message, and nothing tries again.
    void pollSoapyRecovery();
    // The reopen itself, once pollSoapyRecovery has judged it due: reads the
    // rate, gains, running state and AIR centre off the dead radio (device_),
    // closes it and launches the open. Split from the gate so the carry-across
    // it starts is testable without a real SoapySDR fault.
    void reopenAfterDriverFault();
    // The worker-thread open shared by selectSource and pollSoapyRecovery:
    // closes nothing (the caller has), stamps the request with sourceGen_,
    // and sets deviceOpenPending_/deviceBusyLabel_. `r` carries the args, the
    // row, the rate, the centre to carry across and any recovery payload.
    void launchDeviceOpen(DeviceOpenResult r, const std::string& busyLabel);
    // Applies a resolved open on the GUI thread (panel mirrors, gain priming,
    // pipeline install). Takes ownership of r.dev.
    void finishDeviceOpen(DeviceOpenResult r);
    // Combo selection. -1 means "active device no longer in the list" (a
    // Refresh dropped it); the preview then falls back to the active source
    // name. Distinct from the ACTIVE source: selecting "IQ file" only shows
    // the path controls — the pipeline keeps its source until Open succeeds.
    int sourceSel_ = 0;
    std::string sourceError_;   // red text under the Source controls; "" = none
    // THE RADIO THE PANEL DRIVES, whatever kind it is. Non-owning view of the
    // DeviceSource installed in the pipeline (the pipeline owns it via
    // setSource); null whenever the active source is the generator or a file,
    // and must be nulled BEFORE any setSource that destroys the object.
    //
    // This was a SoapySource* until 0.91.0 and everything the Source section
    // did went through the concrete class. It is the interface now because
    // there are two more kinds of radio behind it - RtlSdrSource and
    // HackRfSource, which speak WinUSB and need no vendor module at all - and
    // a panel written against one of the three would have had to be written
    // three times. Rate, gains, AGC, antenna, tuning range, dead/faulted: all
    // of it is DeviceSource now, and a Soapy device answers exactly as it did.
    cascade::source::DeviceSource* device_ = nullptr;

    // ...AND THE SAME OBJECT AS A SoapySource WHEN IT IS ONE, for the four
    // things that are genuinely Soapy-specific and have no meaning for a
    // native driver: the child-process scan gate (a native enumeration opens
    // nothing and needs no gate), the module/vendor diagnostics under "no
    // radio hardware found", the "close the radio to look for other devices"
    // caption, and the automatic reopen after an absorbed vendor fault
    // (deadReason() distinguishes a faulted driver from a wedged one, which
    // only SoapySource has). Null whenever device_ is not a SoapySource -
    // including when it is a native radio - and set and cleared with it.
    cascade::source::SoapySource* soapyView_ = nullptr;

    std::string deviceArgs_;     // args of the open device (re-find on Refresh)
    // WHICH ROW OF supportedSampleRatesHz() the Rate combo is on. The list
    // used to be the fixed 1/2/4/8 MS/s table for every radio on every
    // driver; it is now the DEVICE's own list - the RTL-SDR's twelve standard
    // rates, the HackRF's 2..20 MS/s menu, whatever a Soapy driver reports -
    // so the index only means anything alongside deviceRatesHz_.
    int deviceRateIndex_ = 1;
    std::vector<double> deviceRatesHz_;         // supportedSampleRatesHz() at open
    std::vector<std::string> deviceRateLabels_;  // "2.400 MS/s", one per rate
    std::vector<std::string> deviceGainNames_;  // gains() at open, names only
    // The RANGE each of those gains will accept, parallel to the names. The
    // sliders were drawn 0..60 dB for every stage of every radio before this,
    // which is right for none of them: a B200's PGA goes to 76 dB and an
    // RTL-SDR's VGA starts at -4.7, and SoapySDR clamps silently so nothing
    // ever said so.
    std::vector<cascade::source::GainInfo> deviceGainRanges_;
    std::vector<float> deviceGainsDb_;          // slider mirrors, one per name

    // WHETHER GAIN i IS DECIBELS OR THE HARDWARE'S OWN STEPS, for the four
    // places that letter a gain (the sliders, the RECEIVER card, the scope
    // deck's knob, the browser status). Decibels for anything the driver did
    // not describe - an out-of-range index, or a mirror that outlived the
    // ranges it was filled beside - because that is what every gain in
    // FoxSDR was before the native Airspy and what every other driver still
    // reports.
    cascade::source::GainUnit gainUnitAt(std::size_t i) const {
        return i < deviceGainRanges_.size() ? deviceGainRanges_[i].unit
                                            : cascade::source::GainUnit::Decibels;
    }
    cascade::source::GainUnit firstGainUnit() const { return gainUnitAt(0); }
    bool deviceAgcSupported_ = false;
    bool deviceAgc_ = false;

    // THE AIRSPY R2 / MINI's OWN CONTROLS (0.99.41, engine/engine_airspy.cpp):
    // one gain mode at a time - Sensitive, Linear or Free, the reference
    // Airspy application's three - with only that mode's sliders, Free mode's
    // two AGC switches, and the software decimation. The window's
    // drawAirspyControls draws them and answers true when the open radio is
    // an Airspy, in which case the generic Auto gain switch and gain sliders
    // are not drawn; false and draws nothing for every other radio.
    //
    // Re-reads the gain list, values and AGC state from device_: an Airspy's
    // list changes with its mode, and a gain set by name from the browser can
    // change the mode.
    void refreshDeviceGainMirrors();
    // The open radio's Airspy state into airspyMemory_ (a no-op for any other
    // radio), after every change the user makes to it.
    void airspyRememberOpen();
    // What the panel's controls DO, called by the window's drawAirspyControls
    // (kControlMayCall - the same reviewed pattern as scanSoundCards) so
    // tests/test_airspy_app.cpp drives the same code the buttons do: the
    // decimation (the radio, then the Rate combo's delivered rates, then the
    // whole chain follows the new rate), the gain mode, and Free mode's two
    // AGCs - each re-reading the radio and remembering it. False, with
    // sourceError_ set, on a refusal.
    bool chooseAirspyDecimation(unsigned factor);
    bool chooseAirspyGainMode(cascade::source::AirspySource::GainMode mode);
    bool chooseAirspyAgc(bool lna, bool on);
    // Each Airspy's gain mode, gains and decimation - AppConfig::airspy, per
    // radio (core/airspy_settings.hpp); put back by adoptDeviceMirrors.
    std::map<std::string, cascade::core::AirspySetting> airspyMemory_;

    // THE BIAS TEE. Present only when the OPEN device is one of the native
    // drivers that has one and can say so (see withBiasTee in
    // engine/bias_tee.hpp for which, and for why this is not a DeviceSource
    // method). `shown` is the checkbox and is always the driver's READBACK;
    // `remembered` is AppConfig::biasTee, the memory PER RADIO (driver and
    // serial) for every family. It is seeded at restore, applied by
    // adoptDeviceMirrors after every open through biasTeeAfterOpen - to that
    // radio only - and changed only when the user switches a radio, with the
    // checkbox or the deck's key (biasTeeTicked).
    cascade::gui::BiasTeePanel biasTeePanel_;
    // THE SWITCH BOTH CONTROLS USE: biasTeeTicked on the open radio, and its
    // refusal (the driver's own lastError) into sourceError_, where every
    // other Source-panel refusal is shown.
    void switchBiasTee(bool want);
    // A radio is open NOW and has a bias tee this application reaches -
    // biasTeePanel_.present on its own outlives a close.
    bool biasTeeReachable() const;
    // "kind|args" of the radio the key speaks for (gui::biasKeyRadio), and
    // whether an "on" switched now will be kept and restored at its next open
    // (gui::biasTeeWillRestoreOn) - what the dialog may promise, and when a
    // confirmation may be kept for the session.
    std::string biasKeyRadioNow() const;
    bool biasKeyMayRememberNow() const;
    // THE CENSUS AND CAPTURE SEAM, like FOXSDR_FORCE_MUTE_BANNER: with
    // FOXSDR_FORCE_BIAS_KEY=accept (or =refuse) and no radio with a bias tee
    // open, the key is drawn over a STAND-IN bias tee that takes (or refuses)
    // every change, so tests/test_theme_census.cpp can place the key in every
    // theme and a capture can show it off, on and refused off the generator.
    // It never touches a radio: the moment a real one with a bias tee is open,
    // that radio's readback is what the key shows. READ ONLY BY run() FOR A
    // BOUNDED (--frames) RUN (gui::biasStandInFor), like FOXSDR_INPUT_SCRIPT:
    // an interactive launch never has a stand-in.
    cascade::gui::BiasStandIn biasStandIn_ = cascade::gui::BiasStandIn::None;
    bool biasStandInActive() const {
        return biasStandIn_ != cascade::gui::BiasStandIn::None && !biasTeeReachable();
    }
    bool biasStandInOn_ = false;
    // The installed source's converter key before any alias (a sound card
    // by its card - gui::soundCardConverterKey).
    std::string converterRawKeyNow() const;
    std::string converterRadioKeyNow() const;
    cascade::core::ConverterSetting converterForKey(const std::string& radioKey) const;
    void applyConverterForSource();
    double radioHzForSource(const std::string& kind, const std::string& args, double airHz) const;
    // The user changed the converter for the radio in use: remember it, apply
    // it, and keep the AIR frequency - the radio is retuned to what the new
    // setting makes of it. Only when the radio cannot go there (0 Hz or below,
    // or outside its published range) does it stay put, the counter relabel
    // and the note say what the radio reaches. An I/Q file always relabels.
    void changeConverter(const cascade::core::ConverterSetting& s);
    // "125 MHz up-converter" and its three siblings, translated.
    std::string converterName(const cascade::core::ConverterSetting& s) const;
    // A tune the converter could not deliver, or the radio refused or moved,
    // stated in AIR terms. "" when no converter is on (the plain sentences in
    // engine/tune_control.hpp speak then).
    std::string converterTuneNote(double requestAirHz, bool refused, double answeredAirHz,
                                  bool isPluginPreset);
    // The AIR centre a source switch carries to the next radio, or no value
    // when the installed source has none to give. Judged at the RADIO: a
    // readback at or below 0 Hz is a device that was never tuned, while the
    // air figure itself may legitimately be negative (see
    // DeviceOpenResult::keepCenterHz).
    std::optional<double> carriedAirCentre();
    // THE SAME RADIO UNDER ANOTHER KEY FOR THIS SESSION. When a native open
    // falls back to SoapySDR the dongle is the one the user chose, but its key
    // is the Soapy one; the alias maps that key to the native one so the
    // converter set for the radio still applies - and changes made while it is
    // open are kept under the native key, where the next native open looks.
    // Not saved: the next session makes the same decision again. Installed
    // only when the Soapy key has no ACTIVE converter of its own (an Off
    // record is none) and both keys name the dongle by the same serial
    // (gui::fallbackNamesTheSameDongle).
    std::map<std::string, std::string> converterKeyAlias_;
    // Soapy keys a fallback opened WITHOUT carrying the native radio's
    // converter, because nothing showed it was the same dongle; the Source
    // section says so (converterAliasNote). This session only.
    std::set<std::string> converterNotCarried_;
    // The air centre a converter change could NOT keep (the radio could not
    // follow it, so the counter relabelled), with the radio it was held for and
    // where that radio sat. The next change uses it instead of the relabelled
    // figure while the radio is still there, so off-then-on and a typo-then-
    // fix come back to the same station. Cleared by every source install.
    // (No default member initialisers: GCC refuses them on a nested struct
    // an std::optional member instantiates inside the class - see TestHooks.)
    struct ConverterHeldAir {
        std::string key;
        double airHz;
        double radioHz;
    };
    std::optional<ConverterHeldAir> converterHeldAir_;
    std::string resolveConverterKey(const std::string& radioKey) const;
    void noteConverterFallback(const std::string& nativeKey, const std::string& fallbackKey);
    std::map<std::string, cascade::core::ConverterSetting> converters_;

    // THE SWITCHES THAT BELONG TO ONE RADIO EACH, and are NOT persisted.
    //
    // The bias tee above is saved because leaving it off silently costs a
    // user an evening of a dead band. None of these does: an RSP's notches
    // and HDR mode and an RX888's dither and output randomiser change what is
    // heard, are visible in the spectrum the moment they move, and each
    // driver deliberately puts the radio into a known state at open. So these
    // mirror the DRIVER'S READBACK for the session and nothing more - which
    // also means there is no stale saved value to reconcile against a
    // driver's open-time policy, the reconciliation the RTL-SDR's bias tee
    // needed a rule of its own for (engine/bias_tee.hpp, rtlBiasTeeAtOpen).
    //
    // "Present" is asked of the CONCRETE TYPE once per open, because these
    // are per-model even within one driver: an RSP1A has no HDR mode, an
    // RSPdx has no DAB notch, and a panel that offered either would be
    // offering a control the API answers with an error.
    bool deviceRfNotchPresent_ = false;
    bool deviceRfNotch_ = false;
    bool deviceDabNotchPresent_ = false;
    bool deviceDabNotch_ = false;
    bool deviceHdrPresent_ = false;
    bool deviceHdr_ = false;
    bool deviceAdcSwitchesPresent_ = false;  // the RX888's pair, together
    bool deviceDither_ = false;
    bool deviceRandomiser_ = false;

    // --- Native radios ----------------------------------------------------
    // Every radio one of our own drivers can open, from the eight enumerate*
    // functions. UNGATED and refreshed freely, unlike soapyDevices_: a native
    // enumeration reads SetupAPI properties and NEVER OPENS A DEVICE
    // (src/usb/usb_device.hpp rule 1), which is the exact rule the vendor
    // probe breaks and the whole reason scanSoapy() has a gate. It is cheap
    // enough to run on the GUI thread.
    //
    // TWO OF THE EIGHT ARE NOT USB AND ARE STILL IN HERE. enumerateSdrPlay()
    // asks the SDRplay service for its list, which is safe at any time for
    // the same reason rule 1 exists - it does not touch the bus. The Pluto's
    // row is not a discovery at all: a network cannot be walked, so
    // scanNative appends ONE row for it unconditionally, at the end, and that
    // row opens nothing until the user presses Open on an address. See
    // plutoUri_ and kPlutoDriverKey.
    std::vector<cascade::source::NativeDeviceInfo> nativeDevices_;
    // Their combo captions, composed once by scanNative(): the row label with
    // " (native)" appended. Stored rather than built per frame because the
    // combo hands ImGui a const char* that has to outlive the call.
    std::vector<std::string> nativeRowLabels_;
    // Dongles that are PRESENT but not bound to WinUSB - the DVB-T driver, or
    // no driver at all (problem code 28). They cannot be opened by anything,
    // so they are not offered as rows; the Source section says so in one
    // sentence instead, because "my dongle is not in the list" with no
    // explanation was the single worst thing this panel used to do.
    std::vector<cascade::usb::UsbDeviceInfo> nativeUnbound_;

    // WHERE THE PLUTO IS. An InputText buffer rather than a std::string
    // because that is what ImGui edits, seeded from AppConfig::plutoUri at
    // restore and written back by currentConfig().
    //
    // It is an ADDRESS AND NOT A ROW, which is the whole difference between
    // this radio and the other seven: nothing is contacted until the user
    // presses Open, so choosing the Pluto row costs no network traffic, no
    // timeout and no wait - a user who has never owned one can select it,
    // read what it wants, and select something else.
    char plutoUri_[192] = "ip:192.168.2.1";

    // WHAT THE SOURCE SECTION SAYS ABOUT THE SDRPLAY API WHEN THERE IS NO RSP
    // ROW TO SHOW. Composed by scanNative() from the driver's own pure
    // sdrPlayPanelAdvice(), so the sentence the user is given is the one a
    // test pins; empty when the API is installed and new enough, which is when
    // there is nothing to say. It covers BOTH ways an RSP goes missing - no
    // API and an API too old - which took the enumeration's recorded reason,
    // because the table's own version field is written only by a session that
    // got past the version gate. sdrPlayApiDetail_ is the loader's own account
    // of where it looked, shown dimmed underneath for whoever is helping.
    std::string sdrPlayAdvice_;
    std::string sdrPlayApiDetail_;
    bool sdrPlayRowsFound_ = false;
    // The label each SDRplay API row last showed, by its args (0.99.36). A
    // radio this process has selected is not in the API's list, so its row is
    // put back from here (source::withClaimedSdrPlayRows) under the name the
    // user picked it by.
    std::map<std::string, std::string> sdrPlaySeenLabels_;
    // Last REQUESTED VFO bandwidth (Hz): combo presets and band-edge drags
    // both land here, and this is what the overlay and the config store use.
    // (The combo keeps showing its last preset after an edge drag — the combo
    // is a preset picker, not a readback; the overlay is the truth.)
    double vfoBandwidthHz_ = 150000.0;
    // The ACTIVE source's kind as the config store spells it. Tracked at each
    // successful switch because the pipeline does not expose source identity.
    // "siggen"|"file"|"soapy"|"rtlsdr"|"hackrf"|"airspy"|"airspyhf"|
    // "sdrplay"|"mirisdr"|"rx888"|"pluto"|"soundcard"
    std::string sourceKind_ = "siggen";

    // WHAT THE CONFIG REMEMBERS, ONE SLOT PER FAMILY, and they are separate
    // on purpose. deviceArgs_ above is the LIVE device's args; these two are
    // the last Soapy device's kwargs and the last native device's args, kept
    // apart because the prefer-native rule needs both at once: it reads the
    // SAVED SOAPY args to learn "driver=rtlsdr, serial=00000001" and decide
    // whether a native row is the same dongle, and it must still have those
    // Soapy args to fall back to when the native open refuses the tuner. One
    // shared field would be overwritten by whichever family opened last, and
    // the fallback would then have nothing to fall back to.
    std::string cfgSoapyArgs_;
    std::string cfgNativeArgs_;

    // THE SAVED RADIO A RESTORE COULD NOT OPEN, held so the exit save can
    // write it back instead of the generator that stood in for it. Set only
    // by the startup restore's failure path and cleared the moment the user
    // deliberately chooses any other source; see
    // gui::rememberedSourceAfterFailedOpen for why one session with the
    // dongle unplugged used to lose the radio permanently. The label is what
    // the Source section shows in the combo while this is set - a radio that
    // is saved but not open, rather than a generator presented as if it had
    // been chosen.
    cascade::gui::RememberedSource restoreKeep_;
    std::string restoreKeepLabel_;

    // The open radio's MODEL, with no serial in it - what every diagnostic
    // line, the crash context and the scan-gate caption name it by. Kept as a
    // member because it cannot be derived from the args for a native device:
    // core::sanitiseDevice's allow list is driver/product/type, and a native
    // row's args are nothing but a serial, so sanitising them yields "".
    std::string deviceModel_;
    // RX antenna ports the open device offers, and the one selected. Empty
    // until a Soapy device is opened. Persisted, because which port carries
    // the antenna is a property of the user's cabling, not of a session.
    std::vector<std::string> deviceAntennas_;
    std::string deviceAntenna_;
    std::string iqOpenPath_;  // last successfully opened IQ file (persisted;

    // --- Recorder state (P6) --------------------------------------------------
    // Two independent Recorder instances so IQ and audio takes can run
    // simultaneously (each records ONE kind at a time by its contract). The
    // pipeline holds non-owning pointers to them only while a take is live;
    // stop*Recording clears the pointer before stopping the recorder.
    cascade::core::Recorder iqRecorder_;
    cascade::core::Recorder audioRecorder_;
    std::string recordDir_;    // %USERPROFILE%/Documents/SDR-recordings
    std::string recordError_;  // red text in the Recorder section; "" = none
    // Why a take ended when nothing went WRONG (the user changed source):
    // muted text in the Recorder section and its own field on the web page,
    // so it never lights FAIL there. Cleared with recordError_ by a Record.
    std::string recordNotice_;
    double iqRecordStartS_ = 0.0;     // ImGui::GetTime() at take start, for
    double audioRecordStartS_ = 0.0;  // the elapsed-wall-time readout
    // Input rate the live IQ take's WAV header was written for. A rate-follow
    // change (source switch, Soapy rate change) finalizes the take: a WAV
    // whose header rate disagrees with its samples would replay detuned.
    double iqRecordRateHz_ = 0.0;

    // --- Bookmarks state (P6) --------------------------------------------------
    // Loaded at startup from FreqManager::defaultPath() and saved after every
    // mutation — but ONLY when config persistence is enabled: hermetic runs
    // (empty configPath_, i.e. every --frames/--selftest CI run) leave
    // bookmarkPath_ empty and never read or write the user's bookmark file.
    cascade::core::FreqManager freqMgr_;
    std::string bookmarkPath_;   // empty = bookmark persistence disabled
    std::string bookmarkError_;  // red text in the Bookmarks section
    std::string bookmarkImportNote_;     // what the last import did
    bool bookmarkSaveDirty_ = false;
    double bookmarkSaveDueS_ = 0.0;

    // --- Scanner state (P6) -----------------------------------------------------
    // The Scanner itself is a pure state machine (core/scanner.hpp); these
    // mirrors exist because ImGui edits by pointer. Defaults come from
    // Scanner::Params's own member initializers so the two can never drift.
    // --- P7 feature state -----------------------------------------------------
    // Panel mirrors for the pipeline's stereo / NR / notch settings (ImGui
    // edits by pointer). Defaults match AppConfig's, which match the
    // pipeline's own construction defaults, so the three can never disagree
    // before the first user click.
    bool stereoEnabled_ = true;
    bool nrEnabled_ = false;
    float nrStrength_ = 0.5f;
    bool notchEnabled_ = false;
    float notchFreqHz_ = 1000.0f;
    float notchQ_ = 30.0f;
    bool autoNotch_ = false;

    // Band plan: OPTIONAL display data merged from
    // BandPlan::defaultDir() at construction. A missing directory is the
    // normal case for a run-from-build-tree session and is silent — there is
    // simply no overlay. A directory that EXISTS but fails to parse keeps its
    // reason here and shows it in the Display section, because that one is a
    // user-visible mistake worth reporting.
    cascade::core::BandPlan bandPlan_;
    std::string bandPlanError_;
    // The active plan's id and the menu of installed plans. The list is built
    // ONCE in loadBandPlan() rather than per frame: available() opens and
    // parses every file in the directory, which is fine at startup and at a
    // deliberate re-scan, and is not something to do sixty times a second
    // inside a combo box.
    std::string bandPlanSelection_ = "world";
    std::vector<cascade::core::PlanInfo> bandPlanChoices_;

    // Plugin host: scanned once at construction and on Rescan. Owns the
    // loaded modules, so it must outlive nothing in particular here — but it
    // is declared before the pipeline-dependent members so it unloads last.
    cascade::core::PluginHost pluginHost_;
    // GUI-side plugin capabilities: map targets, plugin windows, and THE HOST
    // SERVICES a plugin calls back through.
    //
    // DECLARED BEFORE pluginRunner_, so it is destroyed AFTER it, and the
    // three are torn down host-services-last: runner, then UI, then host. That
    // is the order detachAndUnloadPlugins() has always performed and the order
    // ~AppWindow now performs explicitly; this declaration is the net under
    // that, because reverse declaration order is what the destructor falls
    // back on and it used to have these two the wrong way round. A decoder's
    // destroy() may ask the host for the time (Survey Engine 0.1.0 does, to
    // timestamp the dwell it is finishing), and with pluginUi_ destroyed first
    // that call reached a dead host: an access violation on Windows and an
    // abort inside libc++ on Android, both reported from the field on
    // 2026-09-16 from the same plugin at shutdown.
    //
    // THE ONE RECEIVER SNAPSHOT (engine stage 2, core/receiver_snapshot.hpp),
    // declared first so pluginUi_ can be built on it: publishReceiverState
    // fills it once a frame, and the plugin host API, the web server and CAT
    // all answer from it. Shared, because plugin bridges outlive this window.
    std::shared_ptr<cascade::core::ReceiverSnapshot> receiverSnapshot_ =
        std::make_shared<cascade::core::ReceiverSnapshot>();
    cascade::core::PluginUi pluginUi_{receiverSnapshot_};
    // Drives the loaded decoders with real audio. Declared AFTER pluginHost_
    // and pluginUi_ so it is destroyed BEFORE both: the runner's destructor
    // calls each plugin's destroy(), which is code inside a module the host
    // unmaps and which may call a host service on its way out.
    cascade::core::PluginRunner pluginRunner_;
    // --- Anonymous usage reporting (opt-in; see PRIVACY.md) ----------------
    // Counters for the session in progress, journalled to the config at exit
    // and sent at the NEXT start-up. Nothing here is transmitted unless the
    // user has turned reporting on.
    cascade::core::TelemetryReporter telemetryReporter_;
    // "Running now" beats: a minimal ping every five minutes while the app is
    // open, only while reporting is on. See HeartbeatSender in telemetry.hpp.
    cascade::core::HeartbeatSender telemetryHeartbeat_;
    bool telemetryEnabled_ = false;
    std::string telemetryInstallId_;
    std::uint64_t telemetryLaunches_ = 0;
    std::uint64_t telemetryCrashes_ = 0;
    bool telemetryCleanExit_ = false;   // true only on the normal shutdown path
    double telemetrySessionStart_ = 0.0;                  // glfwGetTime at start
    // Time in the current mode. A plain "since" mark cannot be used here: the
    // accrual runs once per frame, and a per-frame delta truncated to whole
    // seconds is always zero, which is what kept modeSeconds empty.
    cascade::core::SecondAccrual telemetryModeAccrual_;
    std::map<std::string, std::uint64_t> telemetryModeSeconds_;
    std::vector<std::string> telemetryPanels_;
    // Called once per frame, so the seconds land against the mode that was
    // actually running rather than the last one selected.
    void telemetryAccrueMode();
    void telemetryNotePanel(const char* name);
    // Reads the previous session out of the config, counts a crash if it
    // never finished, and sends its report. Start-up only.
    void telemetryStartup(const cascade::core::AppConfig& cfg);
    // Builds this session's report into `cfg` for the next start-up to send.
    void telemetryJournal(cascade::core::AppConfig& cfg);
    // Rebuilds the report context out of state the application already has -
    // mode, source, rate, radio model, loaded plugins with versions. Nothing
    // here is re-derived.
    void refreshDiagContext();
    std::size_t diagPluginCount_ = static_cast<std::size_t>(-1);

    bool rxSet_ = false;
    double rxLat_ = 0.0;
    double rxLon_ = 0.0;
    // --- the GPS the position can be read from (0.86.0) ---------------------
    // The reader owns its thread and joins in its destructor; run() also
    // stops it at the top of the shutdown path so the join precedes the GL
    // teardown. gpsPort_ and gpsBaud_ are the persisted choice
    // (AppConfig::gpsPort / gpsBaud); gpsPortInput_ is the text field's own
    // buffer, one byte over the port layer's limit for the terminator, so
    // the field cannot hold a name the sanitiser would cut. gpsPorts_ is the
    // last enumeration, taken when the drop-down opened. gpsRefusal_ is the
    // one sentence shown when a fix the reader accepted was refused by
    // applyReceiverPosition - which cannot happen while both apply the same
    // predicate, and is shown rather than swallowed precisely so a day it
    // does happen is visible.
    cascade::core::GpsReader gpsReader_;
    std::string gpsRefusal_;
    cascade::core::patch::Graph patchGraph_;
    // THE PATCH'S OWN TRANSPORT (0.99.18). Opening the page no longer starts
    // anything: START on the page opens the radios that are switched on and
    // takes the receiver's radio; STOP or ALL OFF closes them and gives it
    // back. Session-only - a patch never starts itself at launch.
    bool patchRunning_ = false;
    bool patchWasRunning_ = false;
    // This frame's compile() of the patch: what would be built, and every
    // reason it could not be. Recomputed while the page is open.
    cascade::core::patch::Plan patchPlan_;
    // WHAT THE RUNNING SET WAS BUILT FROM, as core::patch::dspSignature()
    // spells it. A new set is published only when this changes. It used to
    // be the rate and centre plus "anything was edited", which rebuilt the
    // set on every frame of a node drag; now that a set holds plugin
    // instances, that would restart every decoder sixty times a second.
    std::string patchDspSig_;
    bool patchWasOpen_ = false;
    // The installed decoder plugins as the patch sees them, parallel lists.
    // Rebuilt from the plugin host each frame the page is open (a dozen
    // records), and EMPTIED in detachAndUnloadPlugins() before any module is
    // unmapped - the API pointers point into those modules.
    std::vector<cascade::core::patch::DecoderInfo> patchCatalogue_;
    std::vector<cascade::core::patch::PluginApis> patchApis_;
    // Decoder nodes whose plugin returned no instance from create() in the
    // last build, so the node can say so rather than look ready.
    std::vector<cascade::core::patch::NodeId> patchRefused_;
    // Decoder nodes whose FIRST line since the last build has been logged.
    // One line per decoder per build goes to the application log - enough to
    // prove from a user's log that a patch decoder was fed and produced
    // output, without copying a busy decoder's whole stream into it.
    std::set<cascade::core::patch::NodeId> patchFirstLineLogged_;
    // Each decoder node's latest line and running count since the last
    // build, for its face on the canvas. Kept whether or not the node is
    // wired to a Text out, because a decoder working with nowhere to send its
    // text is still working, and the face is where that is visible.
    struct PatchDecoderFace {
        std::string last;
        std::uint64_t lines = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchDecoderFace> patchDecoderFaces_;
    // Each Text out node's recent lines, newest last, for its face - the
    // patch's own decoder log. Bounded per node.
    std::map<cascade::core::patch::NodeId, std::deque<std::string>> patchSinkLines_;
    // GUI thread: rebuilds the two lists above from pluginHost_.
    void rebuildPatchCatalogue();
    // True when the node's Text output reaches a Text sink - the only way
    // its lines are shown in the Decoder output window.
    bool patchDecoderIsShown(cascade::core::patch::NodeId node) const;

    // --- the patch's own radios (0.99.17, app_window_patch_radios.cpp) --------
    // UP TO FIVE, EACH ITS OWN DEVICE. Every Radio node with a device chosen
    // runs a core::patch::PatchRadio while the page is open: its own device
    // open, reader thread, runner and spectrum. Keyed by node.
    std::map<cascade::core::patch::NodeId, std::unique_ptr<cascade::core::patch::PatchRadio>>
        patchRadios_;
    // What each running radio was opened AS ("<device key>@<rate>"), so a
    // changed device or rate reopens it and nothing else does.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioOpenedAs_;
    // A hardware open in flight, per node. The open is a multi-second USB
    // walk, so it runs on a worker exactly as the receiver's own does.
    struct PatchRadioOpen {
        std::unique_ptr<cascade::source::IqSource> src;
        std::string label;
        std::string error;
    };
    std::map<cascade::core::patch::NodeId, std::future<PatchRadioOpen>> patchRadioPending_;
    std::map<cascade::core::patch::NodeId, std::string> patchRadioPendingAs_;
    // Why a radio is not running, when it tried and failed; drawn on its face
    // and turned into Problem::RadioFailed for the plan.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioError_;
    // The "<key>@<rate>" that failed, so a radio that would not open is not
    // retried every frame - only when its device or rate is changed.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioFailedAs_;
    // Decoder nodes that refused to start, per radio, merged into
    // patchRefused_ for the plan.
    std::map<cascade::core::patch::NodeId, std::vector<cascade::core::patch::NodeId>>
        patchRefusedBy_;
    // After the plan is compiled: make and drop speaker outputs, and publish
    // each running radio's set when it has changed.
    void patchPublishSets();
    // Per radio: what its running set was built from (radioSignature).
    std::map<cascade::core::patch::NodeId, std::string> patchRadioSig_;
    // Each speaker's output, and the output key it was made for.
    cascade::core::patch::DestTable patchDests_;
    std::map<cascade::core::patch::NodeId, std::string> patchDestMadeFor_;
    std::map<cascade::core::patch::NodeId, std::string> patchDestError_;
    // Each radio's newest spectrum, for its Spectrum parts and channel levels.
    struct PatchSpectrum {
        std::vector<float> db;
        std::uint64_t seq = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchSpectrum> patchSpectra_;
    // THE RECEIVER'S RADIO, HANDED TO THE PATCH (owner: "when the patch panel
    // is running unpopulate the sdr from the main sdr software so it show
    // signal generator"). Opening the page switches the receiver to the
    // generator and remembers the radio here; closing the page opens it again.
    struct PatchMainKeep {
        bool valid = false;
        std::string kind;
        std::string args;
        std::string label;
        double rateHz = 0.0;
        // The receiver's AIR centre when the patch took the radio; no value
        // when it had none to give. May be below 0 Hz (see
        // DeviceOpenResult::keepCenterHz).
        std::optional<double> centreHz;
        // A lent SOUND CARD, as it was running when the patch took it - what
        // the hand-back reopens (gui::receiverSourceForPatch).
        cascade::source::SoundCardSettings card;
    };
    PatchMainKeep patchMainKeep_;
    // One device a patch Radio can be set to, for the inspector's list.
    struct PatchDeviceChoice {
        std::string key;     // core/patch_devices.hpp
        std::string label;
    };
    std::vector<PatchDeviceChoice> patchDeviceChoices() const;
    // THE I/Q RECORDINGS a Radio can play (0.99.40): every playable WAV in the
    // recordings folder and in FOXSDR_PATCH_SAMPLES's, read by
    // core::patch::listIqRecordings when a Radio's device list is opened or
    // "Look for radios" is pressed - never at launch, and never per frame.
    std::vector<cascade::core::patch::RecordingInfo> patchRecordings_;
    // What each file's header said, so reopening the list reads only files
    // that are new or changed (core::patch::RecordingProbeCache).
    cascade::core::patch::RecordingProbeCache patchRecordingCache_;
    void patchListRecordings();
    std::string patchDeviceLabel(const std::string& key) const;
    // Per frame while the page is open: take the receiver's radio, open and
    // close radios to match the nodes, make and drop speaker outputs, and
    // publish each radio's set when it has changed.
    void patchReconcile();
    std::vector<cascade::core::patch::RadioInfo> patchRadioInfos() const;
    // Stops every patch radio and drops every output. `restoreMain` then hands
    // the receiver its radio back.
    void patchStopAll(bool restoreMain);
    // The patch transport (0.99.18): the START/STOP key, the red ALL OFF,
    // what a change of running does, and each radio's own switch.
    void patchPressStart();
    void patchAllOff();
    void patchApplyRunning();
    // Every demodulator's squelch setting, handed to the runner of the radio
    // its channel is on - every frame, lock-free, so a drag is live.
    void patchPushSquelch();

    // --- THE TRANSMITTER (0.95.0) --------------------------------------------
    //
    // The TX thread, the modulator, the microphone and the radio all live
    // inside Transmitter; this is everything the PAGE needs and nothing more.
    //
    // NOTHING HERE CAN KEY IT. transmitPttHeld_ is written from the page's
    // own key and from the spacebar while the page has focus, and it is reset
    // to false at the top of every frame - so a page that stops being drawn
    // stops asking, which is the behaviour a window losing focus should have.
    // The LATCH is a deliberate switch whose state lives in Transmitter (with
    // its own failsafe); the page only records a PRESS. All three per-frame
    // flags reach the transmitter together, in drawUi just before its tick,
    // through gui::txPageKey - so a page that is not live releases the key.
    // None is persisted; core/config.hpp says why at length.
    cascade::core::Transmitter transmitter_;
    bool transmitOpen_ = false;    // is the PAGE open
    bool transmitSplit_ = false;
    double transmitSplitHz_ = 145.5e6;
    int transmitModeIndex_ = 0;    // dsp::TxMode
    int transmitInputIndex_ = 1;   // core::TxInput; 1 is TONE, and deliberately
    double transmitPowerDb_ = -89.75;
    double transmitToneHz_ = cascade::dsp::kToneDefaultHz;
    // LISTEN WHILE TRANSMITTING. Off by default: a receiver left unmuted on
    // the frequency it is transmitting on is a howl, and on a full-duplex
    // board like the Pluto the receiver really is still running. On is what
    // somebody working split, or listening to their own signal through a
    // second radio, actually wants.
    bool transmitMonitor_ = false;
    // The address the transmit radio is opened at. Seeded from the Source
    // section's own args when that is a Pluto, because the overwhelmingly
    // common case is one board doing both.
    std::string transmitArgs_;
    std::string transmitError_;
    // THE MICROPHONE'S OPEN, OFF THIS THREAD. Pa_OpenStream on an input is
    // waveInOpen, with the same missing timeout as the output side's
    // waveOutOpen that held this thread 57 s in the field, so the MIC key goes
    // through the same gate as audioOpen_ (bug hunt 2026-09-24,
    // audio-sink-02). Bound to Transmitter::microphoneOpener(), which owns the
    // microphone, so the worker can be abandoned at quit. Nothing on this
    // thread may query the microphone while inFlight() is true.
    cascade::gui::AudioOpen micOpen_;
    // Decoded output, newest last, bounded. The panel is a tail, not an
    // archive; the recorder is where a permanent copy belongs.
    std::deque<cascade::core::DecodedLine> decoderLog_;
    static constexpr std::size_t kDecoderLogMax = 500;
    // THE ONLY MESSAGE COUNT THIS APPLICATION HAS. No decoder plugin reports a
    // message tally over the ABI and the runner keeps none, so the one
    // countable thing a decoder produces is a LINE of output, counted here as
    // pumpDecoderOutput drains it. Cumulative and never reset, so the status
    // column can difference it over a window; decoderLog_ itself cannot serve
    // (it is a bounded tail and drops its oldest entries).
    std::uint64_t decoderLinesTotal_ = 0;
    std::string pluginDir_;

    // --- Retirement enforcement (P11) -----------------------------------------
    //
    // WHY THERE IS A QUARANTINE STEP AND NOT JUST A RED ROW. A retired plugin
    // that is merely painted red is still mapped into this process: its
    // DllMain has run, its static initialisers are live, and its decoder
    // callbacks are one click away. The whole point of the feature is that the
    // stale CODE does not execute, so enforcement has to happen BEFORE
    // LoadLibrary, not after.
    //
    // PluginHost::scan() takes a DIRECTORY and loads every ".dll" in it (there
    // is no per-file load entry point, and plugin_host is fixed), so the only
    // way to keep one file out of a scan is to make it not look like a plugin
    // for the duration. rescanPlugins() therefore runs one ordered sequence:
    //
    //   unloadAll -> un-quarantine everything -> loadInventory (the disk is
    //   now complete, so reconciliation and planUpdates see the truth) ->
    //   blockedPlugins -> rename each blocked file to "<name>.dll.disabled"
    //   -> scan.
    //
    // The renamed file has no plugin extension, so PluginHost never sees it
    // and LoadLibrary is never called on it - not even once, not even at
    // startup. The rename is reversible and local: drop the floor (or update
    // the plugin) and the next rescan puts the name back.
    //
    // FAIL CLOSED on a failed rename. If a blocked file cannot be moved aside
    // (locked, read-only), the scan is SKIPPED entirely and the reason is
    // shown: loading every other plugin while the retired one loads with them
    // would be exactly the state this exists to prevent.
    static const char* pluginQuarantineSuffix();  // ".disabled"
    // Renames every "<name>.dll.disabled" back to "<name>.dll". A leftover
    // whose live name already exists (an update landed while it was aside) is
    // DELETED instead - it is a copy of a file this code renamed, and keeping
    // stale bytes around under a hidden name helps nobody.
    bool restoreQuarantinedPlugins(std::string& error);
    // Renames every currently blocked plugin file out of the scan's way.
    bool quarantineBlockedPlugins(std::string& error);
    // The manifest + cached policy, as of the last rescan and with the whole
    // directory present (see the ordering above). Everything downstream -
    // the red rows, the badge, planUpdates - reads THIS, not a fresh
    // loadInventory, so nothing ever plans against a quarantined file.
    cascade::core::PluginInventory pluginInventory_;
    std::vector<cascade::core::BlockedPlugin> pluginBlocked_;
    // Why enforcement could not be completed (red, above the list). Empty in
    // the normal case, which is every case where nothing is retired.
    std::string pluginEnforceError_;
    // What the catalogue currently offers over what is installed. Pure, cheap,
    // and empty until the user has fetched a catalogue this session - there is
    // no startup fetch, so an update can only ever be offered on request.
    std::vector<cascade::core::PluginUpdate> plannedPluginUpdates() const;
    // AppConfig::pluginLastUpdateCheck, stamped when a catalogue fetch
    // succeeds and persisted with the rest of the config.
    std::int64_t pluginLastUpdateCheck_ = 0;
    // AppConfig::pluginTuneAllowed. THIS is the durable copy of the grant, and
    // PluginUi holds the live one: PluginUi::clear() drops its set with the
    // instances on every rescan, so the permission has to survive somewhere
    // that a rescan does not touch.
    std::vector<std::string> pluginTuneAllowed_;
    // AppConfig::pluginSettingsAllowed - the durable copy of the level-1
    // SETTINGS grant, for exactly the reason pluginTuneAllowed_ is one.
    std::vector<std::string> pluginSettingsAllowed_;
    // AppConfig::pluginSettings - the durable copy of every plugin's own
    // settings. PluginApiCore holds the live one; applyPluginApi copies it
    // here whenever its generation moves, and currentConfig() saves this.
    std::map<std::string, std::map<std::string, std::string>> pluginSettings_;
    std::uint64_t pluginSettingsGen_ = 0;
    // The newest WARN or ERROR each plugin logged, by module file name, for
    // the notice on its plate. Session only, like the decoder output.
    struct PluginNotice {
        std::uint32_t level = 0;
        std::string text;
    };
    std::map<std::string, PluginNotice> pluginNotices_;
    // The plugins' marks, copied out of the core only when its sequence moves.
    std::vector<cascade::core::HostMarker> pluginMarkers_;
    std::uint64_t pluginMarkersSeq_ = 0;
    // AppConfig::pluginsStopped. The durable copy of "the user stopped this
    // plugin", by module file name, held here for the same reason the grants
    // are: PluginRunner and PluginUi are rebuilt on every source change and
    // cleared on every rescan, so the decision has to live somewhere neither
    // touches.
    std::vector<std::string> pluginsStopped_;
    // AppConfig::pluginMuteOverride. Held here for the same reason: the value
    // is a decision about a plugin, and the objects that act on it are torn
    // down and rebuilt underneath it.
    std::vector<std::string> pluginMuteOverride_;
    // What the mute policy is evaluated against, rebuilt whenever the plugin
    // set, a stop, or an override changes (see rebuildMuteStates).
    std::vector<cascade::core::MutePlugin> muteStates_;
    // Display names of the plugins currently holding the audio down. Empty
    // when nothing is. Read by the Sinks panel, the banner, the popup and the
    // web snapshot, so all four say the same thing.
    std::vector<std::string> mutedBy_;
    // Their module file names, in the same order. Kept beside the names rather
    // than looked up from them because the popup's Stop button must act on
    // EXACTLY the plugins the sentence above it named.
    //
    // Measured on the running application: stopping "every running plugin that
    // mutes" instead stopped AIS as well, which was running on a band 900 MHz
    // away and muting nothing - a dialog that said "ADS-B" and switched off
    // two decoders.
    std::vector<std::string> mutedByKeys_;
    // Was the receiver on a muting plugin's preset LAST frame. The popup fires
    // on the true -> false edge (cascade::core::tuneAwayEdge) and re-arms only
    // when this goes true again, which is what stops a modal reappearing on
    // every frame of a slow tune.
    bool mutePrevOnPreset_ = false;
    // The user tuned away and chose "Keep it running". The audio stays muted -
    // that is the model they were offered, sound comes back when the plugin
    // stops - and the banner stays up until it does, or until the tune returns
    // to a preset.
    bool muteKeptRunning_ = false;
    // WHAT THE POPUP IS ASKING ABOUT: the plugin names and keys captured when
    // the edge opened it, and never re-read from the live decision afterwards.
    // See cascade::core::advanceMutePopup for the measured failure that made
    // this a captured value rather than a call to muteSubjectText().
    cascade::core::MutePopupSubject mutePopup_;
    // Set when mutePopup_ goes open, consumed by the next drawMutePopup().
    // Deferred rather than calling ImGui::OpenPopup from the tune path because
    // the evaluation runs before the frame's windows exist, and a popup opened
    // against no ID stack is a popup that never appears.
    bool mutePopupQueued_ = false;
    // Bound on how many one-click presets a single plugin may put on the
    // panel. A plugin is third-party code and a list this long is not a menu.
    static constexpr std::uint32_t kMaxPresetsPerPlugin = 16u;
    // What the last preset click did, shown under the list — a receiver that
    // moved with no acknowledgement reads as a button that did nothing.
    std::string presetNote_;

    // --- The user's own presets (0.99.4) ---------------------------------------
    // AppConfig::userPresets, for every plugin (see core/user_presets.hpp for
    // what they are and why). Held here, like the plugin lists above, because
    // everything that acts on them is rebuilt underneath them.
    std::vector<cascade::core::UserPreset> userPresets_;
    // A SAVE OR A FORGET. Both end in rebuildMuteStates(), which replaces the
    // muteStates_ entry a preset bar is reading its keys from while it draws
    // them - so a press only submits a command (USER_PRESET_SAVE,
    // APP_USER_PRESET_FORGET_AT) and the edit happens at the top of the next
    // frame. `pluginFileKey` is the module file name (core::pluginKey); the
    // version-stripped key the list is stored under is derived from it when
    // the edit is applied.
    struct PendingUserPresetEdit {
        enum class Op { None, Save, Forget };
        Op op = Op::None;
        std::string pluginFileKey;
        std::size_t ordinal = 0;  // Forget only: which of that plugin's presets
    };
    // A save or a forget of the user's own presets, applied: a Save stores
    // the receiver's CURRENT tuning (absolute frequency, mode, channel
    // bandwidth) against that plugin, a Forget removes one. Reports what
    // happened in presetNote_. Reached only through applyCommand
    // (USER_PRESET_SAVE, APP_USER_PRESET_FORGET_AT), which the drain runs at
    // the top of a frame - the safe point these edits always waited for.
    void applyUserPresetEdit(const PendingUserPresetEdit& edit);

    // --- Plugin browser (P9) --------------------------------------------------
    //
    // THE PRIVACY PROMISE. Nothing here contacts the catalogue origin until
    // the user presses Browse. Not at startup, not when the section is
    // expanded, not on a config restore that remembers the browser was open.
    // The published catalogue's README makes that promise to plugin authors
    // and users, and a paid product has to keep it, so the ONLY caller of
    // startCatalogFetch() is a button.
    //
    // THREADING. Identical in shape to the Soapy scan/open pair above and for
    // the same reason: a catalogue fetch is a TLS handshake plus an HTTP
    // round trip, and an install is a multi-megabyte download — seconds of
    // blocking work that would otherwise freeze the window and stall the
    // radio. Both run on a worker via std::async; the GUI polls the future
    // once per frame (pollPluginAsync) and applies the result on the GUI
    // thread. The worker touches pluginRepo_ and nothing else the GUI reads,
    // except progress()/cancel(), which are atomics for exactly this.
    cascade::core::PluginRepo pluginRepo_;

    // Result carriers. The catalogue entries are COPIED out of the repo on
    // the worker thread so the GUI never reads pluginRepo_.entries() while a
    // transfer could be rewriting it.
    struct CatalogFetchResult {
        bool ok = false;
        std::vector<cascade::core::PluginCatalogEntry> entries;
        std::string error;
        // cacheCataloguePolicies() runs on the SAME worker, immediately after
        // a successful fetch: that call is the only moment a retirement floor
        // is ever written to this machine, and deferring it to "some later
        // fetch" would mean a user who browses once and never again is never
        // protected. It is done off the GUI thread because it re-hashes every
        // installed plugin. A failure here is reported, never swallowed - the
        // catalogue still loaded, but the policy the user just saw was not
        // remembered.
        std::string policyError;
    };
    struct PluginInstallResult {
        bool ok = false;
        std::string name;           // display name, for the report
        std::string installedPath;  // set on success
        std::string error;          // verbatim from PluginRepo on failure
        bool isUpdate = false;      // applyUpdate (records itself) vs install
        // recordInstall's failure, for a PLAIN install only. The file is
        // installed and verified either way; what failed is the manifest
        // write, which leaves the plugin unmanaged (and therefore fail-open,
        // never retired) until an install or a catalogue fetch repairs it.
        std::string recordError;
    };
    std::future<CatalogFetchResult> catalogFuture_;
    std::future<PluginInstallResult> installFuture_;
    bool catalogPending_ = false;
    bool installPending_ = false;
    std::string installBusyName_;  // shown in "Downloading <name>..."

    // Starts the catalogue fetch on a worker. https:// goes through
    // PluginRepo::fetchIndex; a path with no "://" scheme is read from disk
    // and parsed with the same parseIndex — see AppConfig::pluginCatalogueUrl
    // for why the local form exists and why it grants nothing extra.
    void startCatalogFetch();
    // Starts one install on a worker. Takes the entry BY VALUE: the worker
    // outlives the frame that spawned it, and catalog_ can be replaced by a
    // Refresh in the meantime.
    void startInstall(cascade::core::PluginCatalogEntry entry);
    // Starts ONE user-requested update on the same worker slot as an install
    // (PluginRepo has a single progress/cancel pair, so only one transfer runs
    // at a time). The plan's `entry` aliases catalog_, which a Refresh can
    // replace mid-transfer, so the worker takes its own COPY of the entry and
    // re-points the plan at it before calling applyUpdate.
    void startUpdate(const cascade::core::PluginUpdate& u);

    // --- ADD ALL PLUGINS (0.96.0) -------------------------------------------
    //
    // ONE RUN, MANY TRANSFERS, AND NOT ONE NEW CODE PATH FOR THE BYTES. The
    // store's key hands back a list of catalogue rows; this queue starts them
    // through startInstall / startUpdate one at a time, because PluginRepo has
    // a single progress/cancel pair and applies exactly one transfer at a
    // time. Everything a single FIT gets - the https rule, the byte cap, the
    // ABI test, the sha256, the manifest record, the rescan afterwards - a
    // module in this queue gets, because it IS a single FIT.
    //
    // IDENTIFIED BY CATALOGUE ID, NOT BY INDEX. A run outlives many frames and
    // a Refresh can replace catalog_ under it; an index would then name a
    // different module. An id that is no longer in the catalogue is recorded
    // as a failure with that reason rather than silently dropped.
    struct AddAllRun {
        bool active = false;
        std::vector<std::string> ids;
        std::vector<bool> isUpdate;
        std::size_t next = 0;  // the next id to start
        int installed = 0;
        int failed = 0;
        // {name, reason}, the reason verbatim and in English from whatever
        // refused it. Kept apart so the log gets the English and the panel a
        // translation of the same words (gui::trStoredReason).
        std::vector<std::pair<std::string, std::string>> failures;
        std::string currentName;  // what is moving right now
        std::size_t total = 0;
    };
    AddAllRun addAllRun_;
    // Builds the queue from the store's own plan and starts it. Re-plans from
    // live state rather than trusting the plan the key was drawn from.
    void startAddAll(bool noticesAcknowledged);
    // Starts the next module in the queue when the slot is free, and writes
    // the summary when the queue empties. Called once per frame, after
    // pollPluginAsync has had its chance to clear installPending_.
    void pumpAddAll();
    // What a finished run left behind: "23 installed, 0 failed", or the names
    // that failed with the reason each gave. Kept until the next run starts.
    std::string addAllSummary_;
    bool addAllFailed_ = false;

    // Consumes finished catalogue/install futures; called once per frame from
    // drawUi, right beside pollSourceAsync.
    void pollPluginAsync();
    // True if the catalogue entry's file name already exists in the plugins
    // directory, comparing against every record PluginHost produced — loaded
    // AND refused — and against the manifest's own records. A refused DLL
    // still occupies the name, so treating it as "not installed" would offer
    // an install that could not replace it; and a RETIRED plugin has been
    // renamed out of the scan, so the host has no record of it at all — the
    // manifest is what keeps it from looking uninstalled and sending the user
    // down an Install path when Update is the remedy.
    bool catalogEntryInstalled(const cascade::core::PluginCatalogEntry& e) const;
    // The HOST RECORD for a catalogue entry, or null when the host has none.
    //
    // Narrower than catalogEntryInstalled on purpose, and the difference is
    // the point: this answers "did the loader see this file", which is what
    // lets the store's data plate report loaded, refused, running and the tune
    // grant from the same record the fitted window uses. A RETIRED module is
    // installed and has no host record - it was renamed out of the scan - so
    // this returns null for one while catalogEntryInstalled still says true,
    // and neither answer is wrong.
    //
    // The pointer is into PluginHost's own vector and is valid only until the
    // next rescan; every caller uses it inside the frame that asked.
    const cascade::core::LoadedPlugin* installedPluginRecord(
        const cascade::core::PluginCatalogEntry& e) const;

    // THE INSTALL GATE, as one named predicate so the button, the tooltip and
    // the --frames diagnostic can never disagree about it. Returns the reason
    // Install must stay disabled for catalog_[idx], or an EMPTY string when it
    // may be clicked. `acknowledged` is the state of the legal-notice
    // checkbox; it is a parameter rather than a member read so the same
    // function answers "would this be installable if the box were ticked",
    // which is what makes the gate observable in a headless run.
    std::string pluginInstallBlockedReason(int idx, bool acknowledged) const;

    // Deletes one installed plugin (see drawPluginsSection for the
    // unload-first rationale) and rescans.
    void removeInstalledPlugin(const std::string& fileName);

    // Deletes one BLOCKED (retired or ABI-mismatched) plugin. Separate from
    // removeInstalledPlugin because a blocked plugin is not on disk under its
    // own name: it has been renamed aside with pluginQuarantineSuffix(), so it
    // needs PluginRepo::removeQuarantined rather than remove().
    void removeBlockedPlugin(const std::string& fileName);
    std::string pluginCatalogueUrl_;  // committed value (persisted)
    std::vector<cascade::core::PluginCatalogEntry> catalog_;
    std::string catalogError_;   // red: fetch/parse failure, verbatim
    std::string catalogStatus_;  // neutral: "N plugins in the catalogue"

    // WHICH CATALOGUE ROW IS SELECTED, AND WHETHER ITS NOTICE WAS TICKED, both
    // live in PluginStoreDeck now (selected, legalAck) - the store window owns
    // its own selection and clears the tick whenever that selection moves, so
    // consent given for one plugin can never be carried to the next. The two
    // members that used to hold them are gone rather than kept in step with
    // the deck: two copies of a consent flag is one copy too many.
    std::string installReport_;  // green: last successful install/remove
    std::string installError_;   // red: last failed install/remove, verbatim
public:
    // THE ONE PUBLISH (engine stage 2, docs/engine-stage2.md): fills a
    // PublishedState - FoxReceiverState and the app's extension - and the
    // /api/status text and lists from this window's members and the
    // pipeline, and publishes them to receiverSnapshot_, which the plugin host
    // API, the web server and CAT all read. Also copies the newest spectrum
    // frame for the browser (the members below). Called once per frame from
    // pumpPublish (drawUi's phase), unconditionally, after every command of the frame has landed
    // and before anything is drawn: the panel being collapsed must not stop
    // anyone being served.
    double publishReceiverState();
private:
    // Its two halves. Every read is a GUI-thread read, which is the contract
    // activeSource() and its readbacks require. `faultMessage` and `rds` are
    // read once by the caller and handed to both.
    void fillPublishedState(cascade::core::PublishedState& ps, const std::string& faultMessage,
                            const cascade::core::RdsSnapshot& rds);
    void fillStatusLists(cascade::net::RadioStatus& s, const std::string& faultMessage,
                         const cascade::core::RdsSnapshot& rds, std::vector<std::uint64_t>& bookmarkIds);
    // The runner's feeding keys, reused every frame (fillPublishedState), and
    // the last sink name a try-locked read answered (keepLastGoodName).
    std::vector<std::string> feedingKeysScratch_;
    std::string sinkNameLastGood_;

    cascade::core::Scanner scanner_;
    double scanStartMhz_ = cascade::core::Scanner::Params{}.startHz / 1.0e6;
    double scanStopMhz_ = cascade::core::Scanner::Params{}.stopHz / 1.0e6;
    double scanStepKhz_ = cascade::core::Scanner::Params{}.stepHz / 1.0e3;
    double scanDwellMs_ = cascade::core::Scanner::Params{}.dwellMs;
    double scanHoldMs_ = cascade::core::Scanner::Params{}.holdMs;
    double scanResumeMs_ = cascade::core::Scanner::Params{}.resumeMs;
    double scanListenMs_ = cascade::core::Scanner::Params{}.listenMs;
    // Readback (center + offset) right after the last scanner-commanded
    // retune. Any later frame where the live readback differs is a tune the
    // scanner did not make — a manual tune, and the user wins (scan stops).
    double scannerExpectedAbsHz_ = 0.0;
    bool scannerHasExpected_ = false;  // false until the scan's first retune

public:
    // --- THE ENGINE'S LIFE (engine stage 3a) ---------------------------------
    // A front end constructs the engine, calls initialise() once, restores
    // its config (applyConfig) and bookmarks (loadBookmarks) if its run is
    // persistent, then runs the pump every pass - the phases one by one at
    // the points of its own frame (gui::AppWindow::drawUi), or pump() for
    // all of them at once. stopTransfers() then teardown() take it down;
    // ~Engine does both for a front end that did not. GUI thread in 3a.
    void initialise();
    void loadBookmarks();
    void stopTransfers();
    void teardown();

    // The receiver's half of the config (AppConfig): restore it from the
    // start-up state (core::startupState) and write it into a snapshot.
    // The front end owns the file and the view state beside it.
    void applyConfig(const cascade::core::AppConfig& cfg);
    void fillConfig(cascade::core::AppConfig& cfg);

    // The per-frame pump (engine.cpp says what each phase runs, in order).
    void pumpFrameBegin();
    void pumpInputs();
    void pumpPlugins();
    double pumpPublish();  // returns the centre it published (publishReceiverState)
    void pumpAudioMute();
    void pumpTransmitter(bool pageLive, bool latchPressed, bool pttHeld);
    void pumpWorkers();
    void pumpAudio();
    void pump();

    // What the receiver published last: the one receiver snapshot (read()
    // and readFull() are the reader side; core/receiver_snapshot.hpp).
    std::shared_ptr<const cascade::core::ReceiverSnapshot> snapshot() const {
        return receiverSnapshot_;
    }

private:
    bool tornDown_ = false;  // teardown() ran (so ~Engine does not run it again)
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_HPP
