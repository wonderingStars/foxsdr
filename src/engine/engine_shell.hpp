// engine_shell.hpp - THE FENCE: the one door the interface (src/gui, the cascade_interface target) has onto the engine
// (engine extraction stage 4, step 5 - "the fence"; the record is docs/engine-stage4.md, the section that closes the stage).
//
// WHAT THIS IS. The interface is built as its own CMake target, cascade_interface, and the compiler is told what it may and
// may not include (cmake/interface_fence.cmake: a deny-by-stub include root, one list). engine/engine.hpp, engine_host.hpp,
// fox_engine_api.hpp, the pipeline, the plugin host and runner, the recorders, the transmitter ... are not on that list's
// allow side: an interface translation unit that names one of them does not compile. This header is what is left on the
// allow side - the whole of what the interface may ask of the engine that is NOT the engine API's table (foxsdr_api.h through
// the window's session, and app_commands.hpp's command vocabulary, which is how every control and read has gone since stages
// 1 to 4).
//
// WHY IT EXISTS AT ALL. The port (window port groups 1 to 14) took the window's reads and writes of the engine down to a
// CLASSIFIED RESIDUE, not to zero. Each class has its reason and its end, and each is a section below, dated:
//
//   SHELL      the composition root and the bounded-run driver: construct, restore the config, start and stop the pass,
//              tear down in the engine's three ordered phases, and the environment-driven seams a test or a capture drives
//              the run with. Their end: the shell leaves src/gui when the host process is split from the interface (stage 7,
//              the headless foxsdr-engine and its own front-end binary).
//   HOST       plugin-drawn content: the plugin UI's tracks, paths, panels and instruments, polled and drawn on the window's
//              thread (HostPluginFeed). Their end: a feed over the engine API's table (a plugin UI is a front end's, so it
//              moves with the host process).
//   WEB / CAT  the browser's and the CAT client's servers still live in the window until stage 5, and read the engine's
//              published snapshot and the audio tap directly. Their end: stage 5 makes them sessions of the table and this
//              whole section is DELETED, not moved.
//
// THE SURFACE DOES NOT GROW SILENTLY. tests/test_engine_shell_guard.cpp counts the declarations of each section and holds the
// budgets; a method added here takes a dated comment naming its class and its end, and the budget moves in the same change
// (the successor of the retired tests/test_command_path_guard.cpp ratchet - the compiler now does the ratchet's job on the
// rest of the window).
//
// THE IMPLEMENTATION is src/engine/engine_shell.cpp (the cascade_shell target), the only file that sees both this header and
// the Engine. The interface target links it and cannot include engine.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_ENGINE_SHELL_HPP
#define CASCADE_ENGINE_ENGINE_SHELL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/engine_measure.hpp"
#include "core/hang_watchdog.hpp"
#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_ui_data.hpp"
#include "core/receiver_snapshot.hpp"
#include "engine/bias_tee.hpp"
#include "engine/tune_control.hpp"
#include "net/web_control.hpp"
#include "net/web_server.hpp"
#include "source/iq_source.hpp"

namespace cascade::core {
class PluginUi;  // named, never defined here: only engine_shell.cpp and the tests see it
}

namespace cascade::engine {

class Engine;  // named, never defined here: only engine_shell.cpp and the tests see it
class EngineShell;
Engine& rawEngine(EngineShell& shell);

// ==================================================================================================================================
// THE VOCABULARY - plain data the engine and its front end both speak. Moved here verbatim from engine.hpp (stage 4, the fence),
// which names each back (Engine::StatusText, EngineHost::BasemapFacts ...): a name that was reachable only through the Engine's
// header is a name the interface could not use, and none of these has a reason to be inside it.
// ==================================================================================================================================

// WHERE EACH FAMILY'S ROWS START IN THE SOURCE COMBO. Row 0 is the generator, row 1 the IQ file and row 2 the sound card; the native
// radios come next, and the SoapySDR devices after them (Engine::kSoundCardRow and kNativeRowBase are these).
inline constexpr int kSoundCardRow = 2;
inline constexpr int kNativeRowBase = 3;
// The decoder output's bound: the panel is a tail, not an archive (Engine::kDecoderLogMax is this).
inline constexpr std::size_t kDecoderLogMax = 500;

// How the engine's control thread runs (Engine::startControlThread; stage 3b, docs/engine-stage3.md section 10).
struct ControlThreadOptions {
    // One pass of the control loop per period (the GUI's frame, by default).
    std::chrono::milliseconds period{16};
    // The control thread's own hang watchdog (OPEN 8): where a report goes
    // (empty: logged to the ring, no file) and after how long a pass that has
    // not come back is a hang.
    std::string watchdogReportDir;
    unsigned watchdogThresholdMs = cascade::core::HangWatchdog::kDefaultThresholdMs;
    // THE PASS FLOOR (pump move P4, adopted decision 6; the survey's section 3.4): the least time between the STARTS of two passes. A
    // submit wakes the loop at once (that is what keeps a tune-to-audio inside one pass of its submit), so without a floor a flood
    // of submits - a drag at the frame rate, a script, a misbehaving client - becomes a flood of passes, each paying the whole
    // pass's cost (the publish allocates a status block). The floor turns a flood into at most 1000/floor passes a second and
    // costs a burst nothing it was not going to wait for: a wake that arrives after the floor has passed (any submit after an
    // idle spell) is served at once. 0 turns it off. A wake from runPassNow() is exempt (the caller asked for exactly one pass).
    std::chrono::milliseconds floor{2};
    // STEPPED MODE (adopted decision 3): the thread exists - so onControlThread(), the control watchdog, the table's thread
    // gating and every hook's real call path are all real - but runs NO pass of its own. It runs one PART of a pass (PassPart)
    // for each stepPass() the front end makes and the front end waits for it, so a bounded run is deterministic: the parts run
    // at the same places in the frame the pump ran them before the move. Stepped mode orders threads; it cannot find a race (the
    // front end is parked while a part runs) - the free-running mode and ThreadSanitizer do that.
    bool stepped = false;
    // THE TIMER TICK (Windows only; 0 leaves it alone): a wait of 16 ms on this system's default 15.6 ms tick ends on the SECOND tick, ~31 ms
    // - so a "62 passes a second" thread ran at 35, found by the interactive smoke of step P5 - and a floor of 2 ms is ~15 ms. The thread asks for
    // this resolution (timeBeginPeriod) from its start to its stop, as the audio stream's WASAPI host already does whenever one is open; the
    // operating system keeps the finest request any process makes, per process since Windows 10 2004.
    unsigned timerResolutionMs = 1;
};

// THE THREE PARTS OF A PASS (pump move P4/P5). A free-running pass runs them in this order as one; a stepped one runs each when
// the front end asks (EngineShell::stepPass), which is how the window keeps the order the pump had in its frame - the part up to and
// including the publish, then the audio mute (which the frame ran after its look, before it drew), then the workers and audio:
//   Front  the GPS fix, the command queues, the inputs, the remote controls, the plugins and the scanner, the patch, the publish
//   Mute   the audio mute (a decision about the receiver the state it just published describes)
//   Back   the workers (device scans and opens, the retune coalescer, downloads) and the audio sink's health, the config copy
enum class PassPart { Front, Mute, Back };

// WHAT THE WEB STATUS NEEDS FROM THE WINDOW'S PLUGIN POLL (pump move step P2): the track-info plugin's answers for the tracks on
// screen, and the headers of the receiver plugins' decoded pictures. Both used to be asked of the window while the engine published
// (the host hooks enrichWebTrack and fillWebImages), reading the window's TrackInfoCache and picture buffer - GUI-thread state - from
// the pass. The window builds this once a frame, immutable, and hands the pointer over inside FrontEndFacts (a copy of the facts is
// a refcount, so the publish's three copies stay free); the pass only looks the answer up.
struct WebTrackInfo {
    std::string id;           // the track's id and kind: the key (the vector is sorted by id, then kind)
    unsigned kind = 0;
    unsigned infoState = 0;   // 1 = known, 2 = the source does not have it (RadioStatus::Track::infoState)
    std::string registration;
    std::string acType;
    std::string acOperator;
    std::string acCountry;
};
struct WebExtras {
    std::vector<WebTrackInfo> tracks;
    std::vector<cascade::net::RadioStatus::Image> images;
    // THE TRACKS THE LAST PLUGIN POLL FOUND, as the window drew them (the pump move's step P3): the pass used to walk the plugin
    // UI's own vector for the browser's target list - a vector the window's poll clears and refills every frame - and walks this
    // immutable copy now. The window carries the VISIBLE ones (the staleness rule the desktop applies); the pass applies it again.
    std::vector<cascade::core::HostTrack> polled;
};

// The basemap plugin's figures, handed over once a frame inside FrontEndFacts (engine/stage3b-pre OPEN 4; EngineHost::BasemapFacts
// is this).
struct BasemapFacts {
    bool active = false;
    std::uint32_t minZoom = 0;
    std::uint32_t maxZoom = 0;
    std::uint32_t tileSize = 0;
    std::string attribution;
};

// WHAT THE FRONT END HANDS OVER FOR THE PUBLISH (engine/stage3b-pre, docs/engine-stage3.md OPEN 4): the facts only the window knows -
// its web listener's state, the counter's face (the web page mirrors it), the basemap plugin's figures, the web status' extras -
// handed to the engine once a frame (EngineShell::setFrontEndFacts) instead of the engine calling the window for them while it
// publishes.
struct FrontEndFacts {
    bool webListening = false;
    // A front end that has handed nothing over (the headless engine) shows
    // the product's default face, as EngineHost's own answer always was.
    std::string tunerDisplayStyle = cascade::gui::tunerStyleName(cascade::gui::TunerStyle::Nixie);
    BasemapFacts basemap;
    // The web status' extras (above); null when there are none, which is every pass of a headless engine.
    std::shared_ptr<const WebExtras> webExtras;
    // A BOUNDED RUN'S DIAGNOSTIC (CASCADE_PLUGIN_TEST): whether to print the catalogue report when a fetch resolves, and the
    // window's frame count to name in it - the window's onCatalogueResult hook used to read its own counter from the pass.
    bool pluginTestReport = false;
    int frame = 0;
};

// WHAT THE BOUNDED-RUN STATUS REPORT PRINTS OF THE PLUGIN UI (pump move step P3): the engine no longer holds the plugin UI - it is
// the front end's - so the front end hands over what it polled when it asks for the report (EngineShell::reportPluginStatus).
struct PluginUiReport {
    std::vector<cascade::core::HostTrack> tracks;
    std::size_t paths = 0;
    std::vector<cascade::core::HostPanel> panels;
};

// The status sentences the panels letter (Engine::StatusText is this) - see Engine::publishStatusText.
struct EngineStatusText {
    std::string sourceError;
    std::string gpsRefusal;
    std::string catalogError;
    std::string bandPlanError;
    std::string tuneMismatchNote;
    std::string transmitError;
    std::string soundCardMissing;
    std::string sdrPlayApiDetail;
    std::string sdrPlayAdvice;
    std::string recordNotice;
    std::string recordError;
    std::string presetNote;
    std::string pluginEnforceError;
    std::string restoreKeepLabel;
    // API 0.3 8.4: the Sinks section's watchdog note (STATUS_LINES key
    // audioHealth).
    std::string audioHealth;
    // API 0.3 8.6: the Bookmarks section's note (what the last import, export or group
    // removal did: bookmarkImportNote_) and its red line (bookmarkError_).
    std::string bookmarkNote;
    std::string bookmarkError;
    // API 0.3 8.8: what the last install or removal of a plugin did (installReport_, green: STATUS_LINES key
    // installReport, level 1) and why the last one failed (installError_, red, verbatim: installError, level 3).
    // The store window and the fitted-modules window show the same pair.
    std::string installReport;
    std::string installError;
    // API 0.3 8.9, what the plugin store window draws beside them: the catalogue status line ("N plugins in the
    // catalogue", catalogStatus_; STATUS_LINES key catalogStatus, level 1) and what the last ADD ALL run did
    // (addAllSummary_; key addAllSummary, level 1, or 3 when the run had failures: addAllFailed_, which is the
    // level's own source and is handed over with it).
    std::string catalogStatus;
    std::string addAllSummary;
    bool addAllFailed = false;
    // API 0.3 8.10: the radio's fault sentence WHOLE (STATUS_LINES key deviceFault, level 3): the pipeline's, or the
    // test stand-in's. FoxReceiverState::faultMessage is the same text cut at 127 bytes, and the zombie-source
    // sentence is 134 - a drawing that looks for the end of it ("restart FoxSDR") must read this, not that.
    std::string deviceFault;
};

// THE REMOTE CONTROLS' PULL (pump move step P1; the review's F3, docs/engine-stage4.md): what the front end's source hands the
// engine's pass when it asks (Engine::RemoteControls is this). The web remote and CAT hand the engine ControlRequests through
// queues of their own (WebServer/CatServer::takePendingControls - mutex-protected, any thread, bounded, drop-oldest); the ENGINE
// pulls them, in its own pass, after the local and API queues and the inputs, before the plugins' requests and the scanner.
struct RemoteControls {
    std::vector<cascade::net::ControlRequest> requests;  // web first, then CAT (the order applyWebControls had)
    // Whether the server that carries the browser's key is running AT THE MOMENT OF THE PULL (the standing condition
    // of the remote key, docs/engine-stage3.md OPEN 7 (c)): when it is not, a key the web remote holds ends in the same
    // phase (FOXAPP_OP_WEB_CONTROL_STOPPED's rule), since no browser can ask again.
    bool serverRunning = true;
};
using RemoteControlSource = std::function<RemoteControls()>;

// ==================================================================================================================================
// WHAT THE ENGINE ASKS OF ITS FRONT END (replaces the interface's use of EngineHost, which names the web server and the plugin UI)
// ==================================================================================================================================

// The clocks and the watchdog - three calls, thread-safe by construction (engine/stage3b-pre OPEN 4). The shell adapts these to the
// engine's own EngineHost; the window implements them.
class ShellHooks {
public:
    virtual ~ShellHooks() = default;
    // The wall-clock the usage report measures a session with (glfwGetTime() in the window).
    virtual double wallTimeS() const = 0;
    // Paused across deliberate blocking work on the calling thread (a plugin rescan, a bounded device open).
    virtual void pauseWatchdog() = 0;
    virtual void resumeWatchdog() = 0;
};

// ==================================================================================================================================
// CLASS HOST - plugin-drawn content (added 2026-10-21, stage 4 step 5; END: a feed over the table when the plugin UI moves with the
// host process). The plugin UI is the window's (pump move step P3): its instances are polled and drawn on the window's thread, over
// the API core the engine owns. This is the one object the window holds for it. It owns the PluginUi and the directive link's
// front-end side; the window names neither.
// ==================================================================================================================================

// What one plugin directive says to the WINDOW (the rest of it - the plugins, their services - is the plugin UI's, applied inside
// the feed): a release (the engine is about to unmap the modules), and the basemap and track-info tables to attach.
struct PluginDirectiveFacts {
    bool release = false;
    const CascadeBasemapApi* basemap = nullptr;
    const CascadeTrackInfoApi* trackInfo = nullptr;
};

class HostPluginFeed {
public:
    // The window's two steps around the plugin UI's own: BEFORE it rebuilds (a release folds the map pages' geometry into the saved
    // list while the instances still stand) and AFTER (attach the basemap and the track-info client, show a demonstration instrument).
    struct Steps {
        std::function<void(const PluginDirectiveFacts&)> before;
        std::function<void(const PluginDirectiveFacts&)> after;
    };
    virtual ~HostPluginFeed() = default;

    // The eight host accessors (references into the plugin UI's vectors, valid for the frame: the directive applies at the head of
    // the frame only).
    virtual const std::vector<cascade::core::HostTrack>& tracks() const = 0;
    virtual const std::vector<cascade::core::HostPath>& paths() const = 0;
    virtual const std::vector<cascade::core::HostPanel>& panels() const = 0;
    virtual const std::vector<cascade::core::HostInstrument>& instruments() const = 0;
    virtual const std::vector<std::string>& trackPluginNames() const = 0;
    virtual bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg,
                              double& outAltM) = 0;
    // Polls the plugins (the frame's one poll, after the head-of-frame directive).
    virtual void poll() = 0;
    // The receiver plugins' decoded pictures, polled into `out` (PluginRunner::pollImages).
    virtual void pollImages(std::vector<cascade::core::HostImage>& out) = 0;
    // Applies the newest directive, if one is waiting, on this thread and acknowledges it to the engine (the head of the frame).
    virtual void applyNewestDirective() = 0;
    // What the bounded-run status report prints of the plugin UI.
    virtual PluginUiReport report() const = 0;
    // The engine no longer applies a directive through this feed (the window is about to go): after this the engine drives its own
    // plugin UI again.
    virtual void disconnect() = 0;
};

// ==================================================================================================================================
// THE SHELL
// ==================================================================================================================================

class EngineShell {
public:
    // ---------------------------------------------------------------------------------------------------------------------------
    // CLASS SHELL (added 2026-10-21, stage 4 step 5; END: stage 7, the host process split from the interface). The composition
    // root and the bounded-run driver. AppWindow::AppWindow, ~AppWindow, run and applyConfig are the callers.
    // ---------------------------------------------------------------------------------------------------------------------------

    // Builds the engine this shell fronts. `hooks` must outlive the shell.
    static std::unique_ptr<EngineShell> create(ShellHooks& hooks);
    ~EngineShell();
    EngineShell(const EngineShell&) = delete;
    EngineShell& operator=(const EngineShell&) = delete;

    // --- construction and restore (the constructor, applyConfig) ---
    void initialise();
    void loadBookmarks();
    void telemetryStartup(const cascade::core::AppConfig& cfg, const std::string& configDir);
    void applyConfig(const cascade::core::AppConfig& cfg);
    // applyConfig's one read, and the constructor's: the store's URL box is seeded from the engine's catalogue URL.
    std::string pluginCatalogueUrl() const;
    // The CASCADE_CONFIG_TEST printout's read-back: the demod mode the pipeline mirrors hold and the centre the source reports.
    struct ConfigReadback {
        int modeIndex = 0;
        double centreHz = 0.0;
    };
    ConfigReadback configReadback() const;
    cascade::core::AppConfig configSnapshot() const;
    void publishConfig();
    void publishStatusText();
    EngineStatusText statusText() const;
    void setFrontEndFacts(FrontEndFacts facts);
    void setFrontEndWaitBeat(std::function<void()> beat);
    void setControlWatchdogReportDir(const std::string& dir);
    // The engine API's table and the handle of the engine the window's LOCAL session opens on.
    static const FoxEngineApi* apiTable();
    FoxEngine* apiHandle();

    // --- the run ---
    void startReceiver();
    void controlPass();
    void startControlThread(const ControlThreadOptions& options);
    void stepPass(PassPart part);
    // The usage heartbeat, armed in an interactive run only (the endpoint, the install id and the version are the engine's to name).
    void configureHeartbeat();
    // The clean-exit marker, set after the pipeline join.
    void markCleanExit();

    // --- the teardown, in the engine's three ordered phases between the window's own steps ---
    void stopTransfers();
    void teardown();
    void shutdownQuiesce();
    void shutdownStop();
    void shutdownRelease();
    // The enforcement diagnostic, printed once after the first phase when the run asked (a bounded run's CASCADE_PLUGIN_STATUS).
    void reportPluginStatus(const std::vector<cascade::core::HostImage>& images, const PluginUiReport& ui);

    // --- the bounded-run seams: each is an environment-driven test or capture hook, honoured in a bounded run only ---
    // FOXSDR_FORCE_BIAS_KEY: the deck's bias tee stand-in.
    void seamBiasStandIn(cascade::gui::BiasStandIn standIn);
    // CASCADE_PLUGIN_TEST: point the store at a catalogue and fetch it once.
    void seamFetchCatalogue(const std::string& url);
    // FOXSDR_SOURCE_DEVICE: scan the native radios and select the one whose patch key is `deviceKey`, by the row's own command.
    void seamSelectNativeDevice(const std::string& deviceKey);
    // Whether a radio is still opening (the first capture sweep: a tune before the open lands is overwritten by it).
    bool seamDeviceOpenPending();
    // FOXSDR_PRESS_PRESET: the first preset of the plugin called `name`, by its key's own command, or - for a plugin with none - its
    // windows are the window's to open (the result says so).
    struct PresetPress {
        bool found = false;
        bool openWindows = false;
        std::string name;
        std::uint32_t capabilities = 0;
    };
    PresetPress seamPressPreset(const std::string& name);
    // FOXSDR_PATCH_START / FOXSDR_PATCH_RUN_AT: the patch's run flag, set and read on the pass.
    void seamPatchStart();
    bool seamPatchRunning();
    // A command from a bounded-run driver (FOXSDR_TUNE_HZ, FOXSDR_PATCH_ALLOFF_AT, the measurement's hooks).
    void seamSubmit(const FoxCommand& command);
    void seamSubmit(cascade::core::cmd::QueuedCommand command);
    // The measurement run (FOXSDR_MEASURE): one step a frame over the pipeline, the pass's figures, and the generator's install.
    bool seamMeasureTick(cascade::core::EngineMeasure& measure, const cascade::core::MeasureHooks& hooks);
    void seamMarkPassStats();
    std::string seamPassStatsJson() const;
    void seamInstallSource(std::unique_ptr<cascade::source::IqSource> source);

    // ---------------------------------------------------------------------------------------------------------------------------
    // CLASS HOST (added 2026-10-21, stage 4 step 5; END: a feed over the table). The window's plugin UI, built over the engine's API
    // core and wired to the engine's directive link.
    // ---------------------------------------------------------------------------------------------------------------------------
    std::unique_ptr<HostPluginFeed> makePluginFeed(HostPluginFeed::Steps steps);

    // ---------------------------------------------------------------------------------------------------------------------------
    // CLASS WEB / CAT (added 2026-10-21, stage 4 step 5; END: DELETED by stage 5, when the browser and the CAT client are sessions
    // of the table). The servers live in the window until then; what they read of the engine is here.
    // ---------------------------------------------------------------------------------------------------------------------------
    void setRemoteControlSource(RemoteControlSource source);
    // The browser page's own poll is the web remote's keep-alive (API 0.2: only an interface's heartbeat counts).
    void webRemoteBeat();
    // The whole block of one publish (the browser's status) and the figures alone (CAT's, lock-free).
    std::shared_ptr<const cascade::core::ReceiverSnapshot::Full> readFullSnapshot() const;
    bool readSnapshot(cascade::core::PublishedState& out) const;
    // The audio tap and the input rate the web publishes read straight off the pipeline.
    std::uint64_t audioSamplesProduced() const;
    std::size_t audioTap(float* out, std::size_t frames) const;
    double inputRateHz() const;

private:
    EngineShell();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend Engine& rawEngine(EngineShell& shell);
};

// The plugin UI inside a feed, for the same two (core/plugin_ui.hpp defines it): the tests drive grants and injected tracks through it.
cascade::core::PluginUi& rawPluginUi(HostPluginFeed& feed);

// The Engine behind a shell, for the code that is entitled to it: the tests (through AppWindowTestAccess) and engine_shell.cpp. It
// returns an INCOMPLETE type to any translation unit that has not included engine/engine.hpp - which the interface cannot - so
// naming it from src/gui is a compile error at the first use, and tests/test_engine_shell_guard.cpp holds that nobody there names it.
Engine& rawEngine(EngineShell& shell);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_SHELL_HPP
