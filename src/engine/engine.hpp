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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/airspy_settings.hpp"
#include "core/app_commands.hpp"
#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "core/gps_reader.hpp"
#include "core/hang_watchdog.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/patch_recordings.hpp"
#include "core/patch_runner.hpp"
#include "core/pipeline.hpp"
#include "core/ppm_correction.hpp"
#include "core/phase_clock.hpp"
#include "core/freq_markers.hpp"
#include "core/background_saver.hpp"
#include "core/record_finish.hpp"
#include "engine/record_start.hpp"
#include "core/plugin_cleanup.hpp"
#include "core/plugin_directive.hpp"
#include "core/plugin_dir_signature.hpp"
#include "core/plugin_reload_phases.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/recorder.hpp"
#include "core/retune_coalescer.hpp"
#include "core/scanner.hpp"
#include "core/diag_history.hpp"
#include "core/foreign_modules.hpp"
#include "core/exit_cause.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"
#include "core/transmitter.hpp"
#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/device_scan_plan.hpp"
#include "engine/engine_host.hpp"
#include "engine/engine_shell.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/pass_stats.hpp"
#include "engine/running_view.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tune_control.hpp"
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/aor_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/hydrasdr_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/pluto_source.hpp"
#include "source/pluto_tx.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rtl_tcp_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"
#include "source/soapy_source.hpp"
#include "source/soundcard_source.hpp"
#include "usb/usb_device.hpp"

// The window's tests are named, never included: the engine does not depend on them. (The window itself reaches the engine only through
// EngineShell - engine/engine_shell.hpp - since the fence; stage 4 step 5.)
namespace cascade::gui {
struct AppWindowTestAccess;
}  // namespace cascade::gui

namespace cascade::engine {

// The vocabulary the engine shares with its front end (ControlThreadOptions, PassPart, WebTrackInfo, WebExtras, BasemapFacts, FrontEndFacts,
// PluginUiReport, EngineStatusText, RemoteControls) moved to engine/engine_shell.hpp with the fence (engine extraction stage 4, step 5): a
// name reachable only through this header is a name the interface cannot use. They are in namespace cascade::engine still.

// The function a front end installs to apply one plugin directive to the plugin UI it owns (Engine::setPluginUiApplier).
using PluginUiApplier = std::function<void(const cascade::core::PluginDirective&)>;

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
    // THE WINDOW IS NOT A FRIEND ANY MORE (stage 4, the fence): its door is EngineShell, the only class here besides the table's host
    // and the tests' accessor that reaches the members below.
    friend class EngineShell;
    friend struct cascade::gui::AppWindowTestAccess;
    // The engine API's host (engine/fox_engine_api.cpp) applies commands
    // through applyCommand and reads the snapshot, the source generation and
    // the pipeline's spectrum slot - nothing else of this class.
    friend class ApiHostImpl;

    // WHO THIS ENGINE ASKS (engine_host.hpp). Declared first, so it is there
    // before anything that could ask it is built.
    EngineHost ownHost_;
    // A POINTER, NOT A REFERENCE, on purpose (engine/stage3b-pre, the Low
    // fallback-teardown item): ~Engine's fallback path (below) must be able
    // to REDIRECT every remaining host_ call to ownHost_ before it runs, and
    // a reference cannot be reseated. Everywhere else this reads exactly as
    // the reference did (host_->x instead of host_.x); the pointer is never
    // null after construction (the constructor takes it from a reference
    // parameter) and is only ever repointed at ownHost_, never cleared.
    EngineHost* host_;

    // NO DEFAULT MEMBER INITIALISERS, on purpose: GCC refuses them on a
    // nested struct used by an inline static member of the enclosing class
    // ("required before the end of its enclosing class"). testHooks_ below is
    // value-initialised, which makes both pointers null all the same.
    //
    // WHAT scanNative's HOOKED WALK RETURNS (testHooks_.nativeScanDetail): the rows, and the rest of what the real
    // walk leaves behind - the dongles present but not bound to WinUSB, the SDRplay API's advice and where it
    // looked, and whether an API row was found - so the Source section's sentences about them are reachable with
    // no USB walk and no SDRplay install (API 0.3 8.10).
    struct NativeScanResult {
        std::vector<cascade::source::NativeDeviceInfo> devices;
        std::vector<cascade::usb::UsbDeviceInfo> unbound;
        std::string sdrPlayAdvice;
        std::string sdrPlayApiDetail;
        bool sdrPlayRowsFound = false;
    };
    // WHAT THE SOAPYSDR RUNTIME SAYS ABOUT ITSELF, and what this process knows of earlier scans
    // (testHooks_.scanDiagnostics): the modules loaded, the vendor installs and the module search paths, and the
    // drivers whose scan child died this session. The real answers cross into SoapySDR.dll (the first call edits
    // the process's environment and DLL search order), so a test must never reach them.
    struct ScanDiagnostics {
        std::vector<std::string> modules;
        std::vector<std::string> searchPaths;
        std::vector<cascade::source::VendorRoot> vendors;
        std::vector<std::string> faultedDrivers;
    };
    struct TestHooks {
        // Replaces core::readPluginDirSignature (the listing rescanPluginsIfChanged and the scan's own signature take): a test that has to
        // change the folder between two listings, or make one fail.
        bool (*pluginDirSignature)(const std::string& dir, cascade::core::PluginDirSignature& out);
        // Replaces makeDeviceSource's construction when set: the "radio" a
        // worker or the restore opens is the test's own recording fake.
        std::unique_ptr<cascade::source::DeviceSource> (*makeDevice)(const std::string& kind);
        // Replaces scanNative's USB walk when set: the list it returns IS the
        // native device list, so no test ever enumerates the desk's radios.
        std::vector<cascade::source::NativeDeviceInfo> (*nativeScan)();
        // The same stand-in with the whole of a walk's result (API 0.3 8.10); wins over nativeScan when both are
        // set. Called with `out` empty; whatever it leaves there is the walk's answer (the Pluto row is added to
        // it, as a real walk adds it, unless the hook listed one).
        void (*nativeScanDetail)(NativeScanResult& out);
        // Stands in for the four diagnostics a scan that found nothing reads (API 0.3 8.10). Set together with
        // soapyScan or alone; with soapyScan set and this null the engine reads NONE of them (a hermetic engine
        // never calls into the runtime).
        ScanDiagnostics (*scanDiagnostics)();
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
        // Called at the top of patchPublishSets when set, with the graph the
        // sets are about to be built from: tests/test_patch_graph_draft_input
        // checks that a node dragged THIS frame is already there.
        void (*patchPublishing)(const cascade::core::patch::Graph& graph);
        // Replaces the serial-port walk FOXAPI_OP_SCAN_SERIAL_PORTS makes when set (the registry on Windows, /dev on
        // Linux): a window test asks for the machine's ports and must get the ones it names, not the desk's.
        std::vector<std::string> (*serialPorts)();
        // Called on the unloading thread at the last moment before the modules are unmapped (detachAndUnloadPlugins), when
        // the interface has acknowledged its release: the instrumented fixture module counts the instances still alive there
        // (tests/test_plugin_unload_handshake.cpp). Never set in the product.
        void (*pluginsAboutToUnmap)();
        // Called in the pass between the state's publish and the status lines' (pumpPublish): a test that holds the pass there proves a
        // reader that takes the status lines and THEN the state never sees lines from a later pass than the state (the pump move's
        // step P4, the survey's 1.6 C). Never set in the product.
        void (*betweenPublishes)();
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
    //
    // RETURNS WHETHER THE MODULES WERE UNMAPPED. False means the interface (which holds the plugin UI) did not acknowledge the
    // release within kPluginUiHandshake - it is not framing - and nothing was unmapped; the caller does not go on to touch the
    // files or scan, and the operation is retried (pluginUnloadOp_) when the interface frames again.
    // `phases`: the caller's clock (rescanPlugins), whose steps - decoders, patch, panels and map, unload - are lapped here; every caller's
    // steps are PUBLISHED (0.99.84: the watchdog's `reload-phase:`), the ones that ask for no clock get a publish-only one.
    bool detachAndUnloadPlugins(cascade::core::PhaseClock* phases = nullptr);
    // THE ONE WAY A DIRECTIVE IS DELIVERED (see setPluginUiApplier): posted, and applied inline where the calling thread is the
    // interface's (or there is none), otherwise waited for - bounded - with the control thread's watchdog paused. Returns whether
    // it was applied before this returned.
    bool deliverPluginDirective(cascade::core::PluginDirective d);
    std::atomic<std::uint64_t> pluginUiSilentPolls_{~std::uint64_t{0}};
    // WHAT THE PASS HAS SPENT WAITING FOR THE INTERFACE'S ACKNOWLEDGEMENT (the pump move's step P5: the handshake and the attach-ordering
    // soft wait now carry live traffic, and are measured): how many waits were made, their total and longest in microseconds, how many ran
    // out, and how many were skipped by the silence rule. Written by the pass, read by anyone.
    struct PluginUiWaitCounters {
        std::atomic<std::uint64_t> waits{0};
        std::atomic<std::uint64_t> totalUs{0};
        std::atomic<std::uint64_t> maxUs{0};
        std::atomic<std::uint64_t> timeouts{0};
        std::atomic<std::uint64_t> silenceSkips{0};
    };
    PluginUiWaitCounters pluginUiWait_;
    // An unload that was refused because the interface was not framing, retried at the next pass in which it has been
    // (pumpPlugins): what was being done, the file it named, and the interface's poll count when it failed.
    // (Also: THE SILENCE RULE of deliverPluginDirective - a wait that ran out records the interface's poll count, and no wait is made
    // again until that count has moved, so a window that hangs costs the pass ONE bound, not one per refresh.)
    enum class PluginUnloadOp { None, Rescan, Remove, RemoveBlocked };
    // (An atomic so a test on another thread may watch for the deferral; the pass is the only writer.)
    std::atomic<PluginUnloadOp> pluginUnloadOp_{PluginUnloadOp::None};
    std::string pluginUnloadFile_;
    std::uint64_t pluginUnloadPolls_ = 0;
    void deferPluginUnload(PluginUnloadOp op, const std::string& file);
    void retryPluginUnload();
    // The last refusal's module (PLUGINS' lastDenied), copied from the core only when it moved - a settled pass allocates nothing.
    std::string apiLastDenied_;
    std::uint64_t apiLastDeniedSeq_ = ~std::uint64_t{0};
    const std::string& lastDeniedNow();
    // The applier the front end installed (null: none), and the thread that installed it.
    mutable std::mutex pluginApplierMutex_;
    std::shared_ptr<const PluginUiApplier> pluginApplier_;
    std::atomic<std::thread::id> pluginGuiThread_{};
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
    // Grants or revokes one plugin, updating both the live grant (the API core's, since the pump move's step P3) and the
    // persisted list. One function so the two can never disagree: a grant that
    // took effect but was not saved would come back revoked next launch.
    // `pluginKey` is a PluginUi::tuneKey() — the module file name, never the
    // display name, which the plugin itself chooses.
    void setPluginTuneAllowed(const std::string& pluginKey, bool allowed);
    // Pushes pluginTuneAllowed_ into the API core. Called before every plugin directive, because a release
    // (detachAndUnloadPlugins) drops the grants along with the live set — without this a rescan silently revoked
    // every permission the user had given.
    void applyPluginTuneGrants();

    // --- HOST API LEVEL 1 (0.99.31) - see core/plugin_api.hpp ---------------
    //
    // The SETTINGS grant, the level-1 twin of the tune grant: same keying,
    // same persistence, same re-application after every rebuild.
    void setPluginSettingsAllowed(const std::string& pluginKey, bool allowed);
    void applyPluginSettingsGrants();
    // ONCE A PASS, straight after pumpRemoteControls: applies what plugins
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
    // the interface target cannot name this class (engine/engine_shell.hpp is its door), so a widget that
    // changes the receiver any other way does not compile. THE PUMPING THREAD ONLY - the engine's pass, which is the
    // control thread since the pump move's step P5 (the same rule every setter
    // it calls has always had: the window never calls it - it submits). `longText`
    // is the command's text when it did not fit
    // FoxCommand::text (a long path); empty otherwise.
    FoxCommandResult applyCommand(const FoxCommand& c, const std::string& longText = {});
private:
public:
    // Queue a command for the next pass's drain (any thread): the front end's
    // own seams use it (the window's widgets go through its engine API session).
    // It WAKES the control thread, so the command lands within a pass's time of
    // the submit and not a period's (the pump move's step P4).
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
    // THE THREAD BOUNDARY (stage 3b, docs/engine-stage3.md section 10): a
    // front end submits from its own thread while the control side drains,
    // so the queue is behind its own lock - held for a push or a swap, never
    // while a command is applied.
    std::mutex localCommandsMutex_;
    std::vector<LocalCommand> localCommands_;
    // Every command a drain has taken, ever (tests; a diagnostic).
    std::atomic<std::uint64_t> localCommandsTaken_{0};
    std::size_t localCommandsQueuedForTest() {
        std::lock_guard<std::mutex> lk(localCommandsMutex_);
        return localCommands_.size();
    }

    // Helpers the commands call, each one the body a widget or a branch of
    // applyControlRequest used to hold inline (so both paths now share it).
    // GUI thread, like applyCommand.
    bool startIqRecording();
    void openIqFile(const std::string& path);
    void openPlutoAt(const std::string& args);
    // The rtl_tcp row's Open key (SELECT_SOURCE "open-rtltcp:rtltcp=<addr>"): the Pluto's recipe, step for step.
    void openRtlTcpAt(const std::string& args);
    void openRtlTcpFromBox();
    void tuneToBookmark(const cascade::core::Bookmark& b);
    std::uint64_t addBookmarkHere(const std::string& name);  // the new bookmark's id
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
    // The same with the plugin's key already in hand (the engine's cached one): the lists' fingerprint asks
    // for every plugin every pass and builds no string to do it.
    bool pluginMutesFor(const cascade::core::LoadedPlugin& p, const std::string& key) const;
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
    // THE RESCAN A CATALOGUE FETCH MAKES ONLY WHEN THE FOLDER IS NOT WHAT THE LAST SCAN LEFT (0.99.63): the full rescan - every decoder
    // destroyed, every module unmapped and mapped again - for a fetch that changed no floor and no file would be a freeze for nothing.
    // pluginScanSig_ is the folder as the last scan saw it, trusted (pluginScanSigValid_) only when that scan ran to its end with nothing
    // refused for a reason outside the folder.
    void rescanPluginsIfChanged();
    cascade::core::PluginDirSignature pluginScanSig_;
    bool pluginScanSigValid_ = false;
    // The old copies the last scan found superseded (a file the scan turned off as an older build of another): what the store's clean-up key
    // offers, judged against that scan and no older one (core/plugin_cleanup.hpp).
    std::vector<cascade::core::SupersededPlugin> pluginOldCopies_;
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
    // `who` is said in the log (0.99.68): the stop's cost with nothing to end was one report's whole story and its cause was nowhere in it.
    void stopReceiver(const char* who);
    // THE TEST SEAM OF FOXSDR_HOLD_AUDIO_LOCK (0.99.68, bounded runs only; FOXAPP_OP_TEST_HOLD_AUDIO_LOCK sets the milliseconds): the
    // first stop of a running receiver that a CONTROL REQUEST (the web remote or a plugin - not the window) makes first has holdAudioLockThread_
    // take the pipeline's audio mutex for that long (Pipeline::holdLockForTest), standing in for a plugin that stretched a DSP block that the
    // stop arrives in the middle of; the log says so once it is held. tests/test_stop_ends_recordings.cpp reads how long a stop with nothing
    // recording then spent on the recorders, from the line stopReceiver writes. Zero: no seam. Joined in teardown.
    void holdAudioLockForTest();
    int holdAudioLockMs_ = 0;
    bool holdAudioLockDone_ = false;
    std::atomic<bool> holdAudioLockAcquired_{false};
    std::thread holdAudioLockThread_;
    // Where the command being applied came from: the WINDOW's queue (drainLocalCommands) or a control request (the engine API's sessions,
    // the web remote, a plugin - the default). The one place the difference is visible is the stop's log line.
    bool commandFromWindow_ = false;
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
    // THE DISK IS NOT TOUCHED ON THE PASS (0.99.63 / 0.99.65; gui/record_start.hpp, core/record_finish.hpp). Recorder::start's create_directories
    // and fopen waited for the disk on the control thread - whose key the transmitter judges by its heartbeat - and a slow one froze the pass.
    // A start is PREPARED here (the cheap half: a refusal that needs no disk is reported at once), its file opened by RecordStart's worker and
    // ARMED when it arrives (pollRecordStarts); a stop detaches the file without touching the disk and RecordFinisher closes it on a worker.
    // A Record pressed while the previous take's file is still being closed is remembered (iqStartQueued_ / audioStartQueued_) and starts the
    // moment that file is closed, so a Stop followed by a Record in one breath still gives the take that was asked for and an open never
    // lands beside a file still being finalised (a file name has a one-second resolution).
    cascade::gui::RecordStart iqStart_;
    cascade::gui::RecordStart audioStart_;
    bool iqStartQueued_ = false;
    bool audioStartQueued_ = false;
    std::shared_ptr<cascade::core::RecordFinisher::Ticket> iqFinish_;
    std::shared_ptr<cascade::core::RecordFinisher::Ticket> audioFinish_;
    void pollRecordStarts(double nowS);
    void finishRecordStart(bool iq, cascade::gui::RecordStart::Result& r, double nowS);
    void finishTake(bool iq);
    bool takeFinishing(bool iq) const;
    void pollRecordFinishes();
    // At quit: a Record queued behind a finish is not started; the finishes still out are waited for until `deadline` (the shutdown budget's
    // one kSaveBound, shared with the settings and the lists - Engine::shutdownRelease's argument).
    bool drainRecordFinishes(std::chrono::steady_clock::time_point deadline);
    // The Radio section's own mode-button path, lifted out for the same
    // reason: a mode key must set the demodulator, its default bandwidth and
    // the log line identically to a click on the button beside it.
    void setModeIndex(int index);
    // The one assignment of modeIndex_ (the demodulator follows it, and the diagnostics context is rendered); see its definition.
    void commitModeIndex(int index);
    // THE BANDWIDTH LIST'S pick and its log line (0.99.61): "bandwidth: N Hz in MODE (how)", one per CHANGE, none for a pick of the step
    // already in force. vfoBandwidthDragged_: a drag on the spectrum moved it (the line is written when the drag settles).
    void setBandwidthIndex(int index);
    void logBandwidthChange(const char* how);
    bool vfoBandwidthDragged_ = false;
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
    // Writes the bookmarks whose ids the text names, whole and in the store's order, as SDR#'s
    // frequencies.xml under the record folder (FOXAPP_OP_BOOKMARK_EXPORT); sets the note.
    // FOXAPI_OK, BAD_ARGUMENT (no id named), NOT_FOUND (none of them exists) or FAILED (the file).
    std::int32_t exportBookmarks(const std::string& idsText);

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
    static constexpr int kSoundCardRow = cascade::engine::kSoundCardRow;
    static constexpr int kNativeRowBase = cascade::engine::kNativeRowBase;
    int soapyRowBase() const {
        return kNativeRowBase + static_cast<int>(nativeDevices_.size());
    }
    // THE ID A COMBO ROW SENDS (FOXAPI_OP_SELECT_SOURCE's text), WHOLE: "siggen"; "row:iqfile"; "row:soundcard";
    // "<driver>:<args>" for a native row; "soapy:<args>" for a SoapySDR row; "" for a row there is not. One
    // spelling for the combo, the bounded-run seam and the table's DEVICES list and fact.source.id.<n>
    // (API 0.3 8.10) - the list item's detail cuts it at 159 bytes, this does not.
    std::string sourceIdForRow(int row) const;

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
    // Drops the Soapy rows another route to the same chip owns, or that a lost SDRplay API session refuses (0.99.43+, master).
    void dropRefusedMiricsSoapyRows();

    // Fills every panel mirror (rates, gains and their ranges, AGC, antenna)
    // from an open DeviceSource, priming the hardware where the panel has to
    // push a value to agree with it. Shared by finishDeviceOpen and
    // openDeviceSync so the async and synchronous opens cannot drift apart -
    // they had two copies of this before, and they had already drifted.
    // `args` is what the device was opened with; the RTL-SDR's bias tee rule
    // needs it to know WHICH dongle this is.
    // `restoreRemembered` false (a recovery reopen after a driver fault) leaves the remembered manual gain and Auto gain alone: the
    // caller applies the state the user had a moment ago.
    void adoptDeviceMirrors(cascade::source::DeviceSource& dev, const std::string& kind,
                            const std::string& args, double requestRateHz, bool restoreRemembered = true);

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
    // THE AUDIO BUFFER (0.99.73 / 0.99.83, master): 0 = AUTOMATIC (starts at 120 ms, deepens itself when this computer falls behind
    // and eases back after a quiet quarter hour - closeAudioMinute), else a fixed lead that never steps. audioBufferAutoMs_ is the
    // depth AUTOMATIC reached, remembered for the next launch (AppConfig::audioBufferAutoMs). The note says what automatic last did.
    int audioBufferMs_ = 0;
    int audioBufferAutoMs_ = 0;
    enum class AudioLeadNote { kNone, kRaised, kRestored, kLowered };
    AudioLeadNote audioLeadNote_ = AudioLeadNote::kNone;
    int audioLeadNoteMs_ = 0;
    int audioLeadNoteCount_ = 0;
    int audioLeadQuietMinutes_ = 0;                                    // consecutive quiet, played minutes (the way back down)
    std::map<cascade::core::patch::NodeId, std::uint64_t> patchStarvedAtMinute_;  // each patch speaker's mark at its last minute
    std::map<cascade::core::patch::NodeId, int> patchQuietMinutes_;
    // Closes the minute (starved callbacks, the log line, the step up or down), for the receiver's sink and each patch speaker.
    void closeAudioMinute(std::size_t ringFramesNow);
    // The audio buffer setting (FOXAPP_OP_SET_AUDIO_BUFFER and the restore): an invalid value is AUTOMATIC.
    void setAudioBufferMs(int ms);
    // The lead the sink demands now, in milliseconds (what a patch speaker made afterwards is seeded with).
    int audioLeadNowMs();
    // The sentence the Sinks rail shows about what automatic last did ("kind ms count"; empty when nothing), as a fact.
    std::string audioLeadNoteFact() const;
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
    // API 0.3 8.10, what the Source section says when a SoapySDR scan has landed (the section used to ask the
    // runtime for these every frame it showed the "No radio hardware found" block; each was a call into
    // SoapySDR.dll). faultedDrivers_ is the drivers whose scan child died this session, taken at every landing;
    // scanEvidence_ is the runtime's own account of where it looked, taken only when a whole scan landed with
    // nothing found, and cleared when one finds something. scanEvidenceSeq_ moves with every write of either, and
    // is what the list fingerprint hashes (the texts are not hashed one by one).
    std::vector<std::string> faultedDrivers_;
    ScanDiagnostics scanEvidence_;
    std::uint64_t scanEvidenceSeq_ = 0;
    // Takes the diagnostics a landed scan reads (the hook's, the runtime's, or - with soapyScan hooked and no
    // scanDiagnostics - none), `empty` being whether the scan found nothing in a whole bus.
    void takeScanDiagnostics(bool wholeAndEmpty);
    // The source's own housekeeping, once a pass at the head of pumpWorkers (API 0.3 8.10): what the window used to
    // send as commands from its frame, every frame, because the draw was where it knew the section was showing.
    void pumpSourceHousekeeping();
    // The fault sentence the list block carries, WHOLE (the pipeline's, or the test stand-in's): the state's
    // faultMessage cuts it at 127 bytes. Kept so the per-pass fingerprint hashes it without a copy.
    std::string apiFaultText_;
    // A deferral has been logged for the radio currently open. The combo's
    // lazy scan asks on every frame the dropdown is open, so without this the
    // one diag line would be written sixty times a second; cleared the moment
    // the gate opens again (Engine::pumpSourceHousekeeping, at the head of the
    // pass: the Source section used to ask for it by a command every frame),
    // so the next radio gets its own line.
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
    // Atomic since stage 3b: the control side moves it, and a submit on the
    // front end's thread stamps each queued command with it.
    std::atomic<std::uint64_t> sourceGen_{0};
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
    // The open radio AS an Airspy, or null for any other radio (or none
    // open) - a query, like currentAbsoluteHz/carriedAirCentre.
    cascade::source::AirspySource* asAirspyDevice() const;
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

    // EACH RADIO'S AUTO GAIN SWITCH AND MANUAL GAIN (0.99.73 / 0.99.75, master): kept per radio under the bias tee's key (driver and
    // serial; the whole args for a radio with no serial), put back by adoptDeviceMirrors on that radio's own open, written by
    // changeAutoGain and rememberManualGain, persisted as AppConfig::autoGainByRadio / manualGainByRadio. Not an Airspy's: its gains
    // are part of its gain mode (airspyMemory_), and a second record would be a second witness that could disagree.
    // THE CRYSTAL CORRECTION (0.99.56, core/ppm_correction.hpp; engine_ppm.cpp): ONE switch for the application and a value per radio
    // (under core::ppmRadioKey), applied to the open radio - in the radio when it corrects its own crystal, otherwise by retuning.
    bool ppmCorrectionOn_ = false;
    std::map<std::string, double> ppmValues_;
    // What the open radio last ACCEPTED as its own correction (InRadio only), 0 after every install - a radio opens with none.
    double ppmRadioApplied_ = 0.0;
    // Why the open radio refused its own correction; "" when it did not.
    std::string ppmError_;
    // THE TWO TUNES MADE BEFORE A RADIO IS INSTALLED - the startup restore's and an RSP's pre-Init tune (launchDeviceOpen) - are told the
    // corrected frequency here, and the memo of that tune is left for applyPpmForSource to hand to the view, so the install moves nothing.
    struct PpmPending {
        std::string key;
        cascade::core::PpmMemo memo;
    };
    std::optional<PpmPending> ppmPending_;
    std::string ppmRadioKeyNow() const;
    // The correction in force for a radio key / a patch device key: its value while the switch is on, 0 otherwise.
    double ppmForKey(const std::string& key) const;
    double ppmForPatchDevice(const std::string& deviceKey) const;
    cascade::core::PpmMethod ppmMethodNow();
    // Puts the correction in force on the radio just installed (applyConverterForSource: every install, swap and close ends there).
    void applyPpmForSource();
    // Radio frequency in, the frequency to tell the radio out (see ppmPending_); the radio itself unchanged when it corrects its own
    // crystal or has no correction.
    double ppmPreTellHz(const std::string& kind, const std::string& args, bool radioCorrects, double radioHz);
    // FOXAPP_OP_SET_PPM: the switch and this radio's value; retunes so the station stays and the radio follows.
    void changePpm(bool on, double ppm);
    // The RECEIVER card's "PPM +1.5" value (number only) while the switch is on and the radio takes a correction; "" otherwise.
    std::string ppmCardValueText();
    // The diagnostics block's "ppm:" value ("off", "not applicable", "+1.5 in the radio", "+1.5 by retuning").
    std::string ppmDiagText();
    // What the Source section's controls draw from (fact.ppm.*).
    struct PpmFacts {
        bool on = false;
        std::string method;       // "na" | "radio" | "retune"
        double stored = 0.0;      // this radio's value (0 = none)
        bool wholeRadio = false;  // the radio takes whole ppm only
        int wholeValue = 0;
        std::string error;
        std::string cardValue;
        std::string model;
        std::string serial;
        std::string key;
    };
    PpmFacts ppmFacts();

    std::map<std::string, bool> autoGainMemory_;
    std::map<std::string, std::vector<std::pair<std::string, double>>> manualGainMemory_;
    // The manual numbers a radio had when Auto gain went ON, kept so that going OFF gives the stages their manual values back.
    std::vector<std::pair<std::string, float>> manualGainsBeforeAgc_;
    // The Auto gain box's own call (the SET_DEVICE_AGC op and the web control): asks the radio, mirrors an accepted change into
    // deviceAgc_, remembers it for this radio, and makes the gains follow the switch. False on a refusal (sourceError_ says why).
    bool changeAutoGain(bool want);
    // The radio's stage `stage`, as the radio holds it now, into manualGainMemory_ (not an Airspy's, not while the radio gains itself).
    void rememberManualGain(std::size_t stage);
    void rememberAutoGain(cascade::source::DeviceSource& dev, const std::string& args, bool on);
    // Puts the remembered manual gain back on `dev` after an ordinary open (adoptDeviceMirrors).
    void restoreManualGain(cascade::source::DeviceSource& dev, const std::string& args);

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
    // The radio's name as the confirmation dialog says it (API 0.3 8.3,
    // fact.biasTee.radioName): the stand-in's own words while it is active,
    // else the open radio's name; "" with none.
    std::string biasKeyRadioName() const;
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
    // WHAT THE CONVERTER ROWS READ OF THE ENGINE (API 0.3 8.3, window port
    // group 4): the facts fact.converter.alias / .modes / .upIgnored, and the
    // radio's own centre. The window's own rules, moved here.
    //   converterAliasState: "" (nothing to say), "notCarried" (a SoapySDR
    //     fallback opened without the native radio's converter, and none is set
    //     here yet) or "carried" (the fallback keeps it, and one is on). Not
    //     const: Pipeline::converter() takes the pipeline's lock.
    //   converterModesText: the FOXAPP_OP_SET_CONVERTER modes the source takes,
    //     comma-separated ("0,1,2"; "0,2" for a real-mode sound card; "" for an
    //     I/Q card, which takes no converter, so the rows are not drawn).
    //   converterUpIgnored: a stored Up for the open sound card is not used.
    //   converterRadioCentreHz: the radio's OWN centre readback, not the air
    //     figure (Pipeline::rawSource() is reached from the converter's file
    //     only - tests/test_converter_call_sites.cpp). Read once a pass into
    //     converterRadioCentre_, which the CONVERTERS list reads live.
    std::string converterAliasState();
    std::string converterModesText() const;
    bool converterUpIgnored() const;
    double converterRadioCentreHz();
    std::atomic<double> converterRadioCentre_{0.0};
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
    // WHERE THE RTL_TCP SERVER IS (0.99.70): the Pluto's box in every respect (an address, not a row; nothing contacted until Open; seeded
    // from AppConfig::rtlTcpAddr and written back by fillConfig; committed as typed by FOXAPP_OP_SET_RTLTCP_ADDR). Its row's args are "rtltcp=" + this.
    char rtlTcpAddr_[192] = "127.0.0.1:1234";

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
    // THE BOOKMARK LIST'S SAVE IS A WORKER'S (0.99.64, core/config_writer.hpp): the snapshot is taken on the pass, so the worker never reads the
    // list being edited; the write itself - the folder, the temp file, the flush, the rename - is the worker's. NEVER WAITS FOR THE DISK.
    // Its answer is collected on the pass it has come back (flushBookmarkSave), a failure in the words the synchronous save gave.
    cascade::gui::BackgroundSaver bookmarkSaver_{"bookmarks", &cascade::core::FreqManager::writeFile};
    // THE WATERFALL'S MARKERS (0.99.57; core/freq_markers.hpp), kept beside the bookmarks and saved the same way: markers.json, a save half a
    // second after a change is first seen (a note is typed a character at a time), the text taken on the pass and the write a worker's. The
    // list belongs to the engine so that the file's persistence is one thing with the bookmarks'; the markers' interface (the waterfall's
    // lines and the menu that drops one) is the window's, to be re-expressed (ledger MARKERS).
    cascade::core::FreqMarkers freqMarkers_;
    cascade::gui::BackgroundSaver markerSaver_{"markers", &cascade::core::FreqMarkers::writeFile};
    std::string markerPath_;
    std::string markerError_;
    unsigned markerSavedVersion_ = 0;
    double markerSaveDueS_ = -1.0;
    void flushMarkerSave(bool force);
    // At quit: the last saves waited for until `deadline` (the shutdown budget's one kSaveBound, shared - Engine::shutdownRelease's argument).
    bool drainListSaves(std::chrono::steady_clock::time_point deadline);
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
    // timestamp the dwell it is finishing), and with the plugin UI destroyed first
    // that call reached a dead host: an access violation on Windows and an
    // abort inside libc++ on Android, both reported from the field on
    // 2026-09-16 from the same plugin at shutdown. (Since the pump move's step P3 the
    // WINDOW'S PluginUi is destroyed before the engine, after ~AppWindow has taken the plugin system down in
    // that order; the bridges it leaves behind answer "detached" - plugin_ui.cpp.)
    //
    // THE ONE RECEIVER SNAPSHOT (engine stage 2, core/receiver_snapshot.hpp),
    // declared first so the API core can be built on it: publishReceiverState
    // fills it once a frame, and the plugin host API, the web server and CAT
    // all answer from it. Shared, because plugin bridges outlive this window.
    std::shared_ptr<cascade::core::ReceiverSnapshot> receiverSnapshot_ =
        std::make_shared<cascade::core::ReceiverSnapshot>();
    // THE PLUGIN SYSTEM'S ENGINE HALF (the pump move's step P3). The API core - the grants, the stop set, the plugins' request
    // queues, settings, command keys and the request_tune records, all thread-safe by their own lock - is the ENGINE'S, created
    // here; the plugin UI (the track sources, panels and instruments the window polls, the basemap and the track-info client)
    // is the WINDOW'S, built over this core, and the engine never touches it: it posts a directive through pluginLink_ and the
    // window applies it on its own thread (core/plugin_directive.hpp; docs/engine-stage4.md, step P3).
    std::shared_ptr<cascade::core::PluginApiCore> pluginApi_ =
        std::make_shared<cascade::core::PluginApiCore>(receiverSnapshot_);
    cascade::core::PluginDirectiveLink pluginLink_;
    // THE PLUGIN UI OF AN ENGINE WITH NO WINDOW (the table's headless engine, the tests that build an Engine alone): driven by
    // the engine itself, on the thread that posts, through the same apply() the window uses. Dormant - never rebuilt, never
    // polled - once a window has installed its applier (setPluginUiApplier), which is before its first scan.
    cascade::core::PluginUi headlessUi_{pluginApi_};
    // Drives the loaded decoders with real audio. Declared AFTER pluginHost_
    // and the API core / headlessUi_ so it is destroyed BEFORE them: the runner's destructor
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
    std::string telemetryFirstRun_;      // see AppConfig::telemetryFirstRun
    std::string telemetryFirstVersion_;
    std::uint64_t telemetryLaunches_ = 0;
    std::uint64_t telemetryCrashes_ = 0;
    // HOW THE UNCLEAN EXITS ENDED (0.99.69, core/exit_cause.hpp). Moves with telemetryCrashes_ at every instant: an unclean exit found at
    // start-up is `unknown` at once and is refined to its class when the evidence has been read (exitCausePoll).
    cascade::core::ExitCounts telemetryExits_;
    // When THIS session began (epoch seconds), journalled for the next start-up to use as the lower edge of "the previous session's
    // reports"; and, while an unclean exit found at this start-up is still waiting for its evidence, when the session that ended began.
    std::uint64_t telemetrySessionStartedEpoch_ = 0;
    std::uint64_t exitCausePrevStart_ = 0;
    bool exitCausePending_ = false;
    // The previous session's reports and log, read on a worker (never a disk read on the pass's thread); the evidence an exit is
    // classified from. The window keeps a cache of its own for the diagnostics bundle.
    cascade::core::DiagHistoryCache exitHistory_;
    // Looks, once per pass while an exit is waiting, for the evidence.
    void exitCausePoll();
    // Writes the verdict's one log line, refines the counters and stops waiting.
    void exitCauseFinish(const cascade::core::ExitVerdict& v);
    // DISPLAY STALLS the window's hang watchdog classified, and THE FAILURES THAT ARE NOT CRASHES (0.99.64): the process's two ledgers,
    // armed and disarmed here by the usage switch, files beside config.json (core/telemetry.hpp StallLedger; core/health_events.hpp).
    std::shared_ptr<cascade::core::StallLedger> stallLedger_ = cascade::core::globalStallLedger();
    std::string telemetryLedgerPath_;   // beside config.json; empty = memory only
    std::shared_ptr<cascade::core::health::HealthLedger> healthLedger_ = cascade::core::health::globalLedger();
    std::string healthLedgerPath_;
    // The samples watch healthPoll() keeps for the radio just installed: its driver kind, the newest spectrum frame the pipeline held
    // when it was installed (anything newer came from this radio), and the cursor the poll reads frames with. Off once seen to deliver.
    bool healthWatching_ = false;
    std::string healthWatchKind_;
    std::uint64_t healthWatchSeq_ = 0;
    cascade::core::SpectrumFrame healthProbe_;
    bool healthSoundNoted_ = false;     // the output has played this session
    // What moved in the counters healthPoll() reads for the recoveries a signal thread or a vendor call's worker meets.
    cascade::core::health::RecoveryWatch recoveryWatch_;
    // Once per pass: the output's first play, the watched radio's first samples, and the recoveries nothing else may count.
    void healthPoll();
    // Starts the samples watch for the radio just installed (kind: its driver key).
    void healthWatchSamples(const std::string& kind);
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
    // `configDir`: where config.json lives (the usage record's ledgers and send marker sit beside it); empty = memory only.
    void telemetryStartup(const cascade::core::AppConfig& cfg, const std::string& configDir = std::string());
    // Builds this session's report into `cfg` for the next start-up to send.
    void telemetryJournal(cascade::core::AppConfig& cfg);
    // Rebuilds the report context out of state the application already has -
    // mode, source, rate, radio model, loaded plugins with versions. Nothing
    // here is re-derived.
    void refreshDiagContext();
    // Renders the context on every way out of a function that changes what it says (a source installed, a rate followed, the patch
    // page's radios started or stopped): a fault in the first moment after such a change is reported with the NEW state, not the
    // old one the 1 Hz pass had last seen (0.99.62 - three reports said `source: siggen`, `device-open: no` beside a log line
    // saying a radio had opened).
    struct DiagContextOnExit {
        Engine& e;
        ~DiagContextOnExit() { e.refreshDiagContext(); }
    };
    // The other software's DLLs in this process (0.99.69): what the watch has seen at start and since. Started by the first pass,
    // polled by every one (it writes a line for each module that arrives after the start scan).
    cascade::core::ForeignModuleWatch foreignModules_;
    // THE PASS RENDERS IT (window port group 14, part c): once at the end of applyConfig and then, from pumpAudio, at most
    // once a second - and only when what the context is made of (mode, source, rate, radio model, loaded plugins with
    // versions) has changed, so a settled pass allocates nothing. The window no longer calls refreshDiagContext: the
    // diagnostics bundle takes the block the pass rendered, and the crash handler prints the same bytes.
    void pumpDiagContext();
    std::uint64_t diagContextInputs();
    std::chrono::steady_clock::time_point diagContextAt_{};
    std::uint64_t diagContextHash_ = 0;
    bool diagContextHave_ = false;
    std::uint64_t diagContextRenders_ = 0;  // times the context was rendered (a test reads it: the gate and the hash)
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
    // THE PATCH GRAPH the patch runtime runs. Replaced whole by
    // FOXAPP_OP_PATCH_SET_GRAPH (the Patch page's draft, a loaded document)
    // and edited in place only by the Engine's own patch runtime (a device
    // and centre named at the take-over, a centre or rate learnt from a
    // radio, START's and ALL OFF's switches). The window never writes it
    // (docs/engine-stage3.md OPEN 6).
    cascade::core::patch::Graph patchGraph_;
    // How many times patchGraph_ has been replaced by a NEW DOCUMENT
    // (FOXAPP_PATCH_GRAPH_DOCUMENT) - read by the window, whose draft edit of
    // the old document must not be put on top of the new one.
    std::uint64_t patchGraphEpoch_ = 0;
    // THE GRAPH'S VERSION (API 0.3 8.12): a counter that moves on every change
    // to patchGraph_ - a session's accepted edit, a document, and the patch
    // runtime's own (START's and ALL OFF's switches, a learnt centre, a device
    // named at the take-over). Starts at 1 for the empty graph, so 0 can mean
    // "no version" to a sender. It is published (fact.patch.version) in the
    // same block as the graph and the epoch, and a SET_GRAPH names the version
    // its draft was edited from (ival[1]), which is how the engine merges a
    // stale edit instead of overwriting what happened since.
    std::uint64_t patchGraphVersion_ = 1;
    // The last kPatchGraphHistory graphs, one per version they were noticed
    // at, newest last: what a stale base is merged against. Cleared by a
    // document (a new epoch's edits are never merged into the old one's).
    struct PatchGraphRecord {
        std::uint64_t version = 0;
        cascade::core::patch::Graph graph;
    };
    static constexpr std::size_t kPatchGraphHistory = 8;
    std::deque<PatchGraphRecord> patchGraphHistory_;
    // Notices a change to patchGraph_ made since the last notice (a graph
    // compare against the newest record, no allocation when nothing moved) and
    // gives it the next version. Called at every point a stale sender could be
    // merged against, and before every publish.
    void syncPatchGraphVersion();
    // THE PATCH RUNTIME'S STEP OF THE PASS (API 0.3 8.12): while the standard
    // window's patch view is showing (patchPageOpen_), the catalogue, the
    // transport, the radios, the compile and the sets; when the flag drops, the
    // close rule (the patch stops, the receiver gets its radio back). Between
    // pumpPlugins and pumpPublish, so the block published in the pass shows it.
    void pumpPatch();
    // The compile of the graph against the radios as they are, with the two
    // marks only an attempt can find (a radio that would not open, a plugin
    // that refused): Engine::patchPlan_.
    void compilePatchPlan();
    // THE PATCH VIEW IS SHOWING (FOXAPP_OP_SET_PATCH_PAGE_OPEN). True until a
    // window says otherwise: the view is the window's default face, so an
    // engine nobody has told must behave as the first frame of one that is
    // showing it.
    bool patchPageOpen_ = true;
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
    // THE PATCH DECODERS' PICTURES (API 0.3 8.12, part 3): the newest picture of each picture-decoder node, kept until
    // the node leaves the graph or a patch document replaces it (NOT until the patch stops: the face keeps the last
    // picture, as it always has). The pumping thread's own map; what an interface reads is the immutable set built from
    // it (ApiImageSet, read_image), so nothing here is ever read from another thread. Each picture is moved in whole from
    // the decoder's runner (pumpDecoderOutput -> takePatchImage) and never written again.
    struct PatchImage {
        std::uint64_t id = 0;
        std::string caption;
        std::shared_ptr<const cascade::core::HostImage> image;
    };
    std::map<cascade::core::patch::NodeId, PatchImage> patchImages_;
    std::uint64_t patchImageLastId_ = 0;       // the last id given (ids start at 1 and are never reused)
    std::uint64_t patchImagesEpoch_ = 0;       // the graph epoch the pictures belong to
    bool patchImagesDirty_ = false;            // a picture arrived or went since the set was last built
    std::vector<cascade::core::patch::NodeId> patchImagesFresh_;  // nodes whose picture arrived this pass (room kept)
    void takePatchImage(cascade::core::patch::NodeId node, cascade::core::HostImage&& img);
    // Once a pass, in publishApiLists: pictures of nodes the graph no longer holds go (all of them with a new document),
    // the set read_image serves is rebuilt when anything moved, and the pass's arrivals are announced.
    void syncPatchImages();
    std::shared_ptr<const ApiImageSet> apiImages_;
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
        // The crystal correction the worker applied before the first tune (0.99.56): in the radio (radioPpm) or by retuning
        // (softwarePpm, with the memo of that first tune so the view reads it back exactly).
        double radioPpm = 0.0;
        double softwarePpm = 0.0;
        cascade::core::PpmMemo ppmMemo{};
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
    // The slow-disk seams of a speaker's file (core/patch_audio.hpp). Empty in the application; a test binds ones that sleep.
    cascade::core::patch::DestSeams patchDestSeams_;
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
    // NOTHING HERE CAN KEY IT. The key is decided by the engine API's rules
    // (core/tx_key_rules.hpp, API 0.2's THE KEY) for the session that asked
    // - a PTT that is a 2 s hold its interface re-asserts, a latch only a
    // LOCAL session may close, a remote PTT only with a local session's
    // consent - and the API host drives the Transmitter from them once a
    // pass (engine/fox_engine_api.cpp). None of it is persisted;
    // core/config.hpp says why at length.
    cascade::core::Transmitter transmitter_;
    bool transmitOpen_ = false;    // is the PAGE open
    mutable std::mutex frontEndFactsMutex_;
    FrontEndFacts frontEndFacts_;
    mutable std::mutex statusTextMutex_;
    EngineStatusText statusText_;
    mutable std::mutex configSnapMutex_;
    cascade::core::AppConfig configSnap_;
    // THE CONFIG AS LOADED (0.99.84 merge, 2026-10-21): publishConfig starts from it, so a field no half of this tree writes yet - the
    // ones master's 0.99.43-0.99.84 added and the merge has not re-expressed (docs/engine-merge-0.99.84.md: PPM, patch presets, plugin
    // sites, the per-radio gain memory ...) - goes back into the file as it was read instead of as a default. Every field a half DOES
    // write is overwritten by it. Written by applyConfig (the control thread's own call), read by publishConfig on the same thread.
    cascade::core::AppConfig loadedCfg_;
    // TESTS ONLY (an engine created through the table with testHooks=1): the
    // fault test.fault stands in for (published as FAULTED until RUN 1), and
    // the dummy load TX_OPEN "test:tx:dummy" installs. Empty / null in the
    // product.
    std::string testFault_;
    std::unique_ptr<cascade::source::IqSink> (*txSinkForTest_)() = nullptr;
    // TESTS ONLY (test.rds, test.autoNotch, test.pilot; API 0.3 8.4): stand-ins
    // for what the DSP thread measures, which a hermetic engine on the signal
    // generator cannot produce - the RDS decoder's snapshot, the auto-notch's
    // readout, the stereo decoder's pilot lock. Read and written on the
    // pumping thread only. Off (the real pipeline's answer) in the product.
    bool testRdsOn_ = false;
    cascade::core::RdsSnapshot testRds_;
    bool testAutoNotchOn_ = false;
    bool testAutoNotchEngaged_ = false;
    double testAutoNotchHz_ = 0.0;
    bool testPilotOn_ = false;
    bool testPilotLocked_ = false;
    // TESTS ONLY (test.bandPlanDir): where the band plans are read from, when
    // not the folder beside the executable.
    std::string bandPlanDirForTest_;
    // The control thread (startControlThread).
    void controlLoop(std::chrono::milliseconds period, std::chrono::milliseconds floor, bool stepped);
    std::thread controlThread_;
    // Set by the control loop as its last act (after transmitter_.stop()),
    // so stopControlThread can bound its wait for the thread before joining.
    std::mutex controlExitMutex_;
    std::condition_variable controlExitCv_;
    bool controlExited_ = false;
    std::atomic<bool> controlRun_{false};
    std::atomic<std::thread::id> controlThreadId_{};
    cascade::core::HangWatchdog controlWatchdog_;
    std::atomic<std::uint64_t> controlPasses_{0};
    // TESTS ONLY: while true the control thread is parked at the top of its
    // loop - no pass, no tick, no heartbeat (tests/test_engine_control_thread).
    std::atomic<bool> controlParkForTest_{false};
    // TESTS ONLY: the next pass first sleeps this long, uninterruptibly (a
    // bounded wait the thread cannot leave early), with controlInStallForTest_
    // set meanwhile.
    std::atomic<int> controlStallForTestMs_{0};
    std::atomic<bool> controlInStallForTest_{false};
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
    static constexpr std::size_t kDecoderLogMax = cascade::engine::kDecoderLogMax;
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
    // the API core holds the live one: a release (every rescan) drops its set
    // with the live modules, so the permission has to survive somewhere
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
    // ADD ALL, planned HERE (engine/add_all_plan.hpp, docs/engine-stage3.md
    // OPEN 5) from the catalogue and inventory. `acknowledgedId` is the one
    // module whose own notice tick is on in the store window (empty: none).
    void startAddAll(bool noticesAcknowledged, const std::string& acknowledgedId = {});
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
    // How many times catalog_ has been replaced, cleared or pushed to (a fetch starting, a fetch landing or
    // failing, the test hook's entry): the list fingerprint hashes this one value for everything a catalogue entry
    // carries (API 0.3 8.9: author, licence, platforms, floor, size, the long texts), where hashing each entry's
    // texts every pass would cost far more than the lists are worth. Bumped at every write of catalog_.
    std::uint64_t catalogSeq_ = 0;
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
    static void applyWebTrackInfo(const WebExtras& x, cascade::net::RadioStatus::Track& w);
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
    // THE CONFIG'S RECEIVER HALF, HANDED OVER (engine/stage3b-pre, docs/
    // engine-stage3.md OPEN 2). publishConfig: the ENGINE's side - fills the
    // receiver half (fillConfig) into a copy behind a mutex; run by the
    // engine's own frame (pumpAudio, and so Engine::pump) and at the two
    // points a save must be current (the window's first snapshot and its save
    // at shutdown). configSnapshot: ANY thread - that copy. The window's
    // currentConfig() reads only this, never fillConfig, so no receiver field
    // is read across threads once the engine has one of its own.
    void publishConfig();
    cascade::core::AppConfig configSnapshot() const;
    // THE STATUS LINES, HANDED OVER (engine/stage3b-pre, docs/engine-stage3.md
    // OPEN 3, strings first): every sentence the engine writes for a panel to
    // letter - an error under the Source controls, a GPS refusal, the
    // catalogue's failure, and so on - as ONE copy behind a mutex. Published
    // with the frame's snapshot (pumpPublish, before anything is drawn) and
    // at the end of the frame (pumpAudio, after the workers' answers); the
    // window reads its own copy of it, refreshed once a frame, and never these
    // fields themselves (the interface cannot name them: engine/engine_shell.hpp).
    using StatusText = cascade::engine::EngineStatusText;
    void publishStatusText();
    StatusText statusText() const;
    // The front end's facts for the publish (FrontEndFacts above). Any
    // thread; the publish reads the newest handed over.
    void setFrontEndFacts(FrontEndFacts f);
    FrontEndFacts frontEndFacts() const;
    // Just the web extras of the newest facts (a pointer copy under the same lock), for the publish.
    std::shared_ptr<const WebExtras> webExtras() const;

    // THE REMOTE CONTROLS' PULL (pump move step P1; the review's F3, docs/engine-stage4.md). The web remote and CAT hand the
    // engine ControlRequests through queues of their own (WebServer/CatServer::takePendingControls - mutex-protected, any
    // thread, bounded, drop-oldest). The ENGINE pulls them, in its own pass, in a phase of its own (pumpRemoteControls)
    // where the window's applyWebControls used to run in the frame: after the local and API queues and the inputs, before
    // the plugins' requests and the scanner. Nothing else applies a remote request, so the key rules (ApiHostImpl::rules_,
    // single-threaded by contract) and the whole of applyCommand are touched by the pumping thread alone. A front end that
    // serves a browser or a CAT client installs the source once (before the pass runs on a thread of its own, or from it);
    // an engine with none - the table's headless engine - pulls nothing and judges nothing.
    //
    // WHY A PULL AND NOT A PUSH THROUGH submitCommand OR THE API QUEUE: both drain BEFORE this phase, so a remote request
    // that arrived in the same pass as a local one would be applied FIRST and could win the key (rule 6, first come); the
    // pull keeps today's order, the web after both.
    using RemoteControls = cascade::engine::RemoteControls;
    using RemoteControlSource = cascade::engine::RemoteControlSource;
    void setRemoteControlSource(RemoteControlSource source);
    void pumpRemoteControls();

    // THE PLUGIN UI'S OWNER (pump move step P3). The front end that holds the plugin UI installs a function that applies one
    // directive to it (core/plugin_directive.hpp); the engine never calls it from the control thread - it posts, and the front
    // end applies at the head of its frame (pluginDirectives().applyNewest). The engine runs it INLINE, on the calling thread,
    // only where that thread is the front end's own: the thread that installed it, or any thread while no control thread runs
    // (the pass is then in the front end's frame, as it is today). The calling thread is recorded as the front end's. An empty
    // function removes the front end: the engine drives its own plugin UI again. Install before the first scan.
    void setPluginUiApplier(PluginUiApplier applier);
    cascade::core::PluginDirectiveLink& pluginDirectives() { return pluginLink_; }
    // The engine-owned API core the window builds its PluginUi over.
    const std::shared_ptr<cascade::core::PluginApiCore>& pluginApiShared() const { return pluginApi_; }
    // THE BOUND ON THE INTERFACE ACKNOWLEDGING A DIRECTIVE (the unload handshake): the pass waits at most this long for the
    // front end's frame to apply the directive it posted, with the control thread's watchdog paused. On a release that runs
    // out, the modules are NOT unmapped (unmapping one the interface still holds is the failure the handshake exists to
    // prevent): the pass logs it and retries when the interface frames again.
    static constexpr std::chrono::milliseconds kPluginUiHandshake{250};

    // The per-frame pump (engine.cpp says what each phase runs, in order).
    void pumpFrameBegin();
    void pumpInputs();
    void pumpPlugins();
    double pumpPublish();  // returns the centre it published (publishReceiverState)
    void pumpAudioMute();
    // THE KEY (engine stage 4): no longer a slot the window writes. Every
    // interface - the standard window included - keys the transmitter through
    // its own engine API session (TX_PTT, TX_LATCH, TX_REMOTE_ARM), under the
    // rules in core/tx_key_rules.hpp, applied on the pass by the API host
    // (engine/fox_engine_api.cpp), which also ticks the Transmitter.
    // THE ENGINE'S TEARDOWN (engine/stage3b-pre, docs/engine-stage3.md OPEN
    // 9), in three ordered phases a front end calls between its own steps -
    // the window has a final config save that must read live state before
    // the pipeline stops, and a clean-exit marker that must wait for the join:
    //   shutdownQuiesce  the transmitter FIRST, the GPS reader and its last
    //                    poll, both recordings finalised. Stage 3b joins the
    //                    control thread AFTER this, so the transmitter is
    //                    always stopped before that join (OPEN 7 (d)).
    //   shutdownStop     the bookmark flush, the pipeline stopped and joined.
    //   shutdownRelease  the patch's radios, then the plugins unloaded - BEFORE the clean-exit marker (master 0.99.60: a plugin's destroy() is
    //                    third-party code that can wedge, and a marker written first reads a session killed inside it as a clean one).
    //   shutdownDrain    after the marker's save: the lists' last saves and the takes' files, waited for against the window's deadline.
    // shutdown() runs all of them, for a front end with nothing in between.
    // Every step is idempotent, so a phase called twice costs nothing.
    void shutdownQuiesce();
    void shutdownStop();
    void shutdownRelease();
    // `drainDeadline`: what is left of the window's drain deadline (taken before the settings' last save), against which the lists' saves and the
    // takes' files still being closed on workers are waited for. A default-constructed one waits for nothing and collects what has finished.
    void shutdownDrain(std::chrono::steady_clock::time_point drainDeadline = std::chrono::steady_clock::time_point{});
    void shutdown();
    // THE TWO BOUNDED-RUN PLUGIN DIAGNOSTICS (window port group 14, part e; engine_diagnostics.cpp). Test machinery that
    // prints what the engine decided to stdout; they read nothing but the engine's own state and were the window's until
    // then. A front end calls reportPluginStatus once, right after shutdownQuiesce (the state a bounded run finished in,
    // including any retirement a catalogue fetch brought into force) when CASCADE_PLUGIN_STATUS asked, handing over the
    // receiver plugins' pictures it holds (the plugin windows poll them into a buffer of their own); and
    // reportPluginTestResult from onCatalogueResult when CASCADE_PLUGIN_TEST asked, naming the frame the fetch resolved on.
    // Neither changes the engine (the status report is not const only because the pipeline's accessors are not).
    // THE PLUGIN UI IS THE FRONT END'S (pump move step P3), so it hands over what it polled for the "plugin ui:" lines.
    void reportPluginStatus(const std::vector<cascade::core::HostImage>& images, const PluginUiReport& ui);
    void reportPluginTestResult(int frame) const;
    void pumpWorkers();
    void pumpAudio();
    // THE SDRPLAY SERVICE'S TROUBLE, counted on the change to it (0.99.55 / 0.99.64; master's AppWindow::pollSdrPlayService). Only the COUNTING
    // is here: the service card, its restart key and the note are the window's (ledger SDRPLAY-SERVICE, stubbed).
    void pollSdrPlayTrouble();
    cascade::source::SdrPlayServiceTrouble sdrPlayTroubleSeen_ = cascade::source::SdrPlayServiceTrouble::None;
    void pump();

    // --- THE CONTROL THREAD (stage 3b, docs/engine-stage3.md section 10) -----
    // The engine pumping itself, on a thread of its own: one controlPass()
    // per period - pump()'s phases in pump()'s order, with the transmitter's
    // CONTROL half only (the front end, on its own thread, submits the page's
    // key through its API session) - and a heartbeat of the thread's own hang
    // watchdog. While it runs, a front end talks to the engine ONLY through
    // the thread-safe surface: the engine API's table, submitCommand, the
    // snapshot, statusText, configSnapshot, setFrontEndFacts - never
    // applyCommand, the pump phases or a field. THE WINDOW STARTS IT since the
    // pump move's step P5 (AppWindow::run, before its frame loop): free-running
    // in the product, STEPPED in a bounded run (ControlThreadOptions::stepped).
    // How it is paced and driven from another thread - the wake every submit
    // gives it, the floor between passes, stepPass, runPassNow, the work posted
    // to it - is declared below; docs/engine-stage4.md records the design.
    //
    // stopControlThread() lets go of every key FIRST, from the calling thread
    // (the front end's key-up stores, under the request slot's lock, and the
    // remote's release), asks the loop to end, and joins; the loop's own last
    // act is transmitter_.stop(), made by the thread that ticks it - so stop()
    // never runs beside a tick(). A control thread stuck inside a wait is
    // still no danger to the air: with no tick, the TX thread releases the key
    // itself within kKeyAliveWait. Idempotent; teardown and ~Engine call it.
    // THE BOUND ON THE CONTROL THREAD COMING BACK (stopControlThread). The
    // key is already up before this is spent (stopControlThread lets go of it
    // first, from the calling thread), so what it waits out is the rest of
    // one control pass. Everything a pass can wait on has its own bound (the
    // audio and microphone opens' grace, the plugin rescan, a vendor call),
    // the longest of which is the source's 3000 ms, so a thread not back by
    // then is logged as late - and then JOINED, not abandoned, because the
    // thread runs on this Engine and must not outlive it (the same trade as
    // Transmitter::kThreadJoinWait; tests/test_shutdown_budget.cpp charges it).
    static constexpr std::chrono::milliseconds kControlJoinWait{3000};
    // How often a STEPPED control thread that is idle (its front end is drawing) beats its watchdog: the wait is a heartbeat interval, not a
    // bound anyone depends on - it ends the moment a step is asked for or the thread is told to stop.
    static constexpr std::chrono::milliseconds kSteppedIdleBeat{250};
    // The safety net on a front-end thread's wait for the pass (stepPass, runPassNow): the wait ends on the pass's own notification, and this
    // is how often it looks again in case one was missed - never the way it is normally left.
    static constexpr std::chrono::milliseconds kPassWaitNet{20};
    void startControlThread();
    void startControlThread(const ControlThreadOptions& options);
    void stopControlThread();
    bool controlThreadRunning() const { return controlRun_.load(); }
    // Where the control thread's hang reports go (empty: none); the front end moves it with its own diagnostics switch.
    void setControlWatchdogReportDir(const std::string& dir) { controlWatchdog_.setReportDir(dir); }
    bool onControlThread() const { return controlThreadId_.load() == std::this_thread::get_id(); }
    // One pass of the control loop (the thread calls it; tests may).
    void controlPass();
    // One PART of a pass, in the pass's own order (controlPass is the three in turn). The control thread calls these.
    void runPassPart(PassPart part);
    // --- THE PASS, DRIVEN FROM ANOTHER THREAD (pump move P4/P5) ---------------
    // stepPass: for a STEPPED control thread, runs `part` on it and waits for it to finish; for a free-running one, does nothing
    // (the pass runs by itself); with no control thread at all, runs the part inline on the calling thread (the front end is
    // then the pump, as it was before the move). While it waits, a caller that is the front end's own thread (the one that
    // installed the plugin UI's applier) applies any plugin directive the part posts - a part that waits for the interface's
    // acknowledgement (the unload handshake) must find it "framing" even though the interface's frame is blocked in this call -
    // so a stepped run answers the handshake in microseconds, not by timing out.
    void stepPass(PassPart part);
    // runPassNow: wakes the control thread and waits for ONE WHOLE PASS THAT STARTED AFTER THIS CALL to finish, so everything
    // submitted before the call has been applied and published when it returns. Works on a free-running and a stepped thread (a
    // stepped one runs the three parts in turn); with no control thread it runs controlPass() inline. Returns false when the
    // pass did not come back within `bound` (a pass held by a test hook, or one stalled past it) or the control thread stopped.
    // This is the test seam for "the look after the pass" and the front end's way to say "I need a pass now".
    bool runPassNow(std::chrono::milliseconds bound = std::chrono::milliseconds(30000));
    bool controlStepped() const { return controlStepped_.load(); }
    // A function the front end's thread calls every kPassWaitNet while it waits inside stepPass or runPassNow (null: none). The window
    // installs its own watchdog's heartbeat: its frame is BLOCKED in the wait - not hung - and a pass part that really hangs is the control
    // thread's own watchdog's to report. (Set and cleared by the thread that waits; nothing else reads it.)
    void setFrontEndWaitBeat(std::function<void()> beat) { frontEndWaitBeat_ = std::move(beat); }
    // WHAT THE FREE-RUNNING PASS COSTS (the measurement run's pass-specific figures, the survey's section 4; written by the pass, read by anyone):
    // how many passes, the sum and the longest of their durations in microseconds, how many ran longer than the period, and how many were
    // started by a wake rather than by the period running out. A stepped thread keeps none (its parts are timed by the front end that asks).
    //
    // AND (the pump move's P7) THE TWO DISTRIBUTIONS the measurement run reports: a pass's duration, and the LAG from a submit's wake to the
    // publish of the pass that served it (the survey's "submit-to-publish"; a wake is any session's submit or close, Engine::submitCommand,
    // postToPass). The percentiles are the upper edge of a log-linear histogram bucket (engine/pass_stats.hpp: "at most", about 19 % wide).
    // Written by the control thread alone, with no lock and no allocation; read by anyone.
    struct PassStats {
        std::uint64_t passes = 0, sumUs = 0, maxUs = 0, overPeriod = 0, wakes = 0;
        std::uint64_t durationP50Us = 0, durationP99Us = 0;
        std::uint64_t lagCount = 0, lagSumUs = 0, lagMaxUs = 0, lagP50Us = 0, lagP99Us = 0;
    };
    PassStats passStats() const;
    // THE MEASUREMENT WINDOW: markPassStats() starts it (the harness calls it where its own warm-up ends), passStatsJson() reports the figures
    // since the mark (since the thread started when there was none) as one JSON object, for the measurement result.
    void markPassStats();
    std::string passStatsJson() const;
    // Passes begun and finished (whole passes; a stepped pass counts when its Back part ends).
    std::uint64_t passesBegun() const { return passesBegun_.load(); }
    std::uint64_t passesEnded() const { return passesEnded_.load(); }
    // WORK THE FRONT END MAY NOT DO ITSELF, run by the pass instead: queued here and run on the control thread at the head of the
    // next pass, after the command queues' drain (the same place a command lands), on the pumping thread with every engine field
    // legal to touch. A bounded run's seams (a device found by name, the measurement harness installing a source) use it, so
    // they never write an engine field from the window's thread while a pass runs. Wakes the control thread. Order is the order
    // of the posts.
    void postToPass(std::function<void()> work);
    // runOnPass: `work` is run with the pass out of the way, and has run when this returns (true). Where the control thread is
    // STEPPED or absent the pass is parked between its parts whenever the caller is not inside stepPass, so the work runs right here
    // on the caller's thread - exactly what a bounded run did before the move, which is what keeps its seams deterministic. Where the
    // thread is free-running the work is posted to it and awaited. Either way it never runs beside a pass, so it may read and write
    // any engine field; and it runs at most once and never after this returns (false: the pass stopped first and the work did not
    // run). The seams of a bounded run use this for every engine field they read or write.
    bool runOnPass(const std::function<void()>& work);
    // How many posted works have run (a test that needs to know one did).
    std::uint64_t passWorkRan() const { return passWorkRan_.load(); }
    // The engine's bounded waits (a plugin rescan, the audio and microphone
    // opens) bracket themselves with these: on the control thread they pause
    // the control thread's own watchdog, anywhere else the host's (the GUI's,
    // whose frame the wait then really is blocking) - OPEN 8.
    void pauseWaitWatchdog();
    void resumeWaitWatchdog();

    // --- THE ENGINE API (stage 4, engine/fox_engine_api.hpp) -----------------
    // The table's host for this engine: sessions, the command queue, results
    // and events. Built with the engine; its two pass calls run in
    // pumpFrameBegin and pumpPublish, on whichever thread pumps.
    ApiHost& api() { return *api_; }
    // The C handle an in-process interface opens its sessions on.
    FoxEngine* apiHandle() { return api_->handle(); }
    // Rebuilds the API host with engine options (FoxEngineParams::options) -
    // only before any session is open (the table's create()).
    void configureApi(const std::string& options);
    // The lists read_list serves (engine_api_lists.cpp): the pumping thread
    // builds a new block only when its fingerprint moves, and hands it to the
    // API host before the publish that carries the fingerprint (listSeq).
    std::uint64_t publishApiLists();
    // Wakes the control thread for a pass now (a submit, a close): a command
    // is applied within a pass's time of its submit, not a period's.
    void wakeControl();
    // A snapshot of the interface-wait counters (see PluginUiWaitCounters).
    struct PluginUiWaitStats {
        std::uint64_t waits = 0, totalUs = 0, maxUs = 0, timeouts = 0, silenceSkips = 0;
    };
    PluginUiWaitStats pluginUiWaitStats() const {
        return {pluginUiWait_.waits.load(), pluginUiWait_.totalUs.load(), pluginUiWait_.maxUs.load(), pluginUiWait_.timeouts.load(),
                pluginUiWait_.silenceSkips.load()};
    }
    // THE ENGINE'S OWN CLOCK (pump move P4, adopted decision 5): seconds on a steady clock since this engine was built, readable from
    // any thread and always current. The pass used to ask the front end for the frame's time (EngineHost::frameTimeS), which the
    // window answered from ImGui's clock - a clock that stands still when the window does (a drag of its title bar, a modal loop) and,
    // read from the control thread, a cache the window refreshed once a frame - so the scanner's dwell, the bookmark save's debounce,
    // the audio watchdog's cadence, the radio reopen's hold-off and the recordings' elapsed time all froze with the window. They are
    // the engine's business and run on the engine's clock now.
    double clockS() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - clockOrigin_).count(); }

    // What the receiver published last: the one receiver snapshot (read()
    // and readFull() are the reader side; core/receiver_snapshot.hpp).
    std::shared_ptr<const cascade::core::ReceiverSnapshot> snapshot() const {
        return receiverSnapshot_;
    }

private:
    bool tornDown_ = false;  // teardown() ran (so ~Engine does not run it again)
    // The origin of clockS(). Not const: tests/test_bookmark_save_async.cpp moves it (EngineTestAccess) to put the pass's clock where its debounce
    // cases need it, as the window's version of that test moved ImGui's.
    std::chrono::steady_clock::time_point clockOrigin_ = std::chrono::steady_clock::now();
    // The remote controls' source (setRemoteControlSource): the pass copies the pointer under the lock and calls it
    // outside it, so a front end may replace or clear it from any thread.
    std::mutex remoteSourceMutex_;
    std::shared_ptr<const RemoteControlSource> remoteSource_;
    // THE PASS'S PACING (everything below the mutex is guarded by it unless it says atomic). The wake a submit gives the control
    // loop (wakeControl); the wake runPassNow gives it, which is exempt from the floor; and, in a stepped run, the request a
    // stepPass makes and the count of steps the thread has finished.
    std::mutex controlWakeMutex_;
    std::mutex stepRequestMutex_;             // one stepPass at a time (the request slot holds one part)
    std::condition_variable controlWakeCv_;   // the control thread waits here
    std::condition_variable passDoneCv_;      // a front-end thread waiting for a pass or a step waits here
    bool controlWake_ = false;
    bool controlWakeNow_ = false;
    bool stepPending_ = false;
    PassPart stepPart_ = PassPart::Front;
    std::uint64_t stepsAsked_ = 0;
    std::uint64_t stepsDone_ = 0;
    std::chrono::milliseconds controlFloor_{0};
    std::atomic<bool> controlStepped_{false};
    unsigned timerResolutionApplied_ = 0;   // the resolution startControlThread asked the OS for (0: none), given back by stopControlThread
    struct PassStatsCounters {
        std::atomic<std::uint64_t> passes{0}, sumUs{0}, maxUs{0}, overPeriod{0}, wakes{0};
        std::atomic<std::uint64_t> lagCount{0}, lagSumUs{0}, lagMaxUs{0};
    };
    PassStatsCounters passStats_;
    PassHist passDurHist_;
    PassHist passLagHist_;
    // When the first wake since the last pass started was made (steady ns; 0 none) - set by wakeControl, taken by the pass that serves it,
    // both under controlWakeMutex_. And when the newest pass finished publishing (results included).
    std::int64_t controlWakeAtNs_ = 0;
    std::atomic<std::int64_t> lastPublishNs_{0};
    struct PassMark {
        bool have = false;
        std::int64_t atNs = 0;
        std::uint64_t passes = 0, sumUs = 0, overPeriod = 0, wakes = 0, lagCount = 0, lagSumUs = 0;
        std::uint64_t dur[PassHist::kBuckets] = {};
        std::uint64_t lag[PassHist::kBuckets] = {};
    };
    mutable std::mutex passMarkMutex_;
    PassMark passMark_;
    std::atomic<std::uint64_t> passesBegun_{0};
    std::atomic<std::uint64_t> passesEnded_{0};
    // The wait a front-end thread makes for a pass or a step; applies a plugin directive for it if it is the front end's thread.
    std::function<void()> frontEndWaitBeat_;
    bool waitForPassCounter(std::unique_lock<std::mutex>& lk, const std::function<bool()>& done, std::chrono::milliseconds bound);
    // WORK POSTED TO THE PASS (postToPass).
    std::mutex passWorkMutex_;
    std::vector<std::function<void()>> passWork_;
    std::atomic<std::uint64_t> passWorkRan_{0};
    void runPassWork();
    std::uint64_t apiListsFingerprint();
    std::shared_ptr<const ApiListBlock> buildApiLists(std::uint64_t fingerprint);
    void buildApiLists03(ApiListBlock& b);
    void buildApiListsSource(ApiListBlock& b);
    std::string transmitArgsSeed() const;
    std::shared_ptr<const ApiListBlock> apiLists_;  // the pumping thread's newest
    // PATCH_LIVE (API 0.3 8.12): the pass's copy of what the patch's running nodes are doing, in a block of its own
    // that moves no listSeq (ApiLiveBlock), rebuilt only when its own fingerprint moved. Once a pass, from
    // publishApiLists, on the pumping thread.
    void refreshPatchLive();
    std::shared_ptr<const ApiLiveBlock> apiLive_;
    // The pass's readings before they are text: kept between passes so that a pass which finds nothing new (the common
    // one) reuses the room and allocates nothing. `ref` points at what the strings of the entry come from (an
    // AudioDest, a PatchDecoderFace or a patchDestError_ text), valid for the pass that took it.
    struct PatchLiveRaw {
        std::uint64_t node = 0;
        int kind = 0;
        double v0 = 0.0, v1 = 0.0;
        bool open = false;
        const void* ref = nullptr;
    };
    std::vector<PatchLiveRaw> patchLiveRaw_;
    // The receiver's radio as a patch Radio node names its device (fact.patch.receiverRadio, API 0.3 8.12): the radio
    // lent to the patch, else the one the receiver has open, "" for the generator, a file or a card the receiver
    // has not opened as a radio.
    std::string patchReceiverRadioKey() const;
    unsigned apiListsBookmarkVersion_ = 0;
    // The plugins' keys (core::pluginKey), kept so the per-pass fingerprint
    // allocates nothing: rebuilt only when the plugins' paths change.
    std::vector<std::string> apiPluginKeys_;
    std::uint64_t apiPluginPathsHash_ = 0;
    void refreshApiPluginKeys();
    // How many times rescanPlugins has run: everything the host reads from the modules' files (a descriptor's
    // strings, a refusal, the file's size) can only have changed across one, so it is hashed into the list
    // fingerprint as one value (API 0.3 8.8).
    std::uint64_t pluginScans_ = 0;
    // FOXAPI_OP_SCAN_SERIAL_PORTS (API 0.3): the ports the last scan found.
    std::vector<std::string> serialPorts_;
    std::uint64_t serialPortsScans_ = 0;
    // LAST, so it is destroyed FIRST: nothing of the engine it names is gone
    // yet when it goes, and the control thread is stopped (teardown) before
    // any member is destroyed.
    std::unique_ptr<ApiHost> api_;
};

// The ops addressed to the open radio itself (the Source panel's device
// controls, the radar scope's gain knob and the bias tee key): a queued one
// is dropped when the radio it was asked of has gone since.
bool isDeviceScopedOp(std::uint32_t op);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_HPP
