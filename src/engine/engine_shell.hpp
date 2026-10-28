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
//              thread (HostPluginFeed), and the basemap plugin's tiles the browser's map asks the engine's web server for
//              (serveWebTiles). Their end: a feed over the engine API's table (a plugin UI and its basemap client are a front end's,
//              so they move with the host process).
//
//   (There was a fourth class, WEB / CAT, until stage 5f. The browser's and the CAT client's servers are the ENGINE'S since stage 5b
//   (engine/server_host.hpp); the window configures them (FOXAPI_OP_SERVER_CONFIG) and reads them (the SERVERS list and the facts).
//   Their CONTROL is sessions of the table since 5c and 5d, and since 5e so are their READS: the page's spectrum and audio come
//   from the table on the web server's own session and the receiver plugins' pictures are polled and served by the engine. The class's
//   last method was the basemap's tiles, which only a GUI-thread plugin client can produce: it is plugin-drawn content, so it lives
//   with the plugin feed (HOST) and the class itself is gone. The track list the page shows is not a door method: it is FrontEndFacts'
//   TrackFeed. docs/engine-stage4.md 6.14.)
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
    // THE TIMER TICK, NOW ONLY A FALLBACK (Windows only; 0 leaves it alone). A wait of 16 ms on the system's default 15.6 ms tick ends on the
    // SECOND tick, ~31 ms (a "62 passes a second" thread ran at 35, found by the interactive smoke of step P5), and a floor of 2 ms is ~15 ms.
    // The free-running loop no longer asks the process for a finer tick: it waits its period, its floor and its wake on its own
    // high-resolution waitable timer and an event, which need none (and a process-wide 1 ms request made the window's own frame-cap
    // wait overshoot by ~2 ms, 12x as often - docs/engine-stage4.md, the capped-tail record). This resolution (timeBeginPeriod, from the
    // thread's start to its stop) is asked for ONLY when that timer cannot be made (Windows 10 before 1803), where the loop falls back to a
    // condition variable's millisecond waits.
    unsigned timerResolutionMs = 1;
    // THE FRAME'S KICK (Engine::frameKick; docs/engine-stage4.md 6.9). A window kicks the free-running pass once a frame, so the period above
    // is the FALLBACK cadence: the loop's timer is what starts a pass when no frame does (a headless engine, a minimized or stalled
    // window). While kicks arrive the fallback waits `kickGrace` instead of `period` (a 60 Hz frame is 16.7 ms: a 16 ms timer would fire
    // just before every kick, which is the timed wake this exists to avoid), and a kick that arrives less than `kickGap` after a pass began is
    // dropped - the pass already running saw the frame's state, and a window that draws 1 000 frames a second must not ask for 500 passes.
    // When the frames stop the next pass is at most `kickGrace` after the last kick, then the period again. kickGrace 0 turns the extension
    // off (the fallback is always `period`); kickGap 0 turns the drop off (the floor alone bounds the kicks).
    std::chrono::milliseconds kickGrace{24};
    std::chrono::milliseconds kickGap{8};
};

// THE THREE PARTS OF A PASS (pump move P4/P5). A free-running pass runs them in this order as one; a stepped one runs each when
// the front end asks (EngineShell::stepPass), which is how the window keeps the order the pump had in its frame - the part up to and
// including the publish, then the audio mute (which the frame ran after its look, before it drew), then the workers and audio:
//   Front  the GPS fix, the command queues, the inputs, the remote controls, the plugins and the scanner, the patch, the publish
//   Mute   the audio mute (a decision about the receiver the state it just published describes)
//   Back   the workers (device scans and opens, the retune coalescer, downloads) and the audio sink's health, the config copy
enum class PassPart { Front, Mute, Back };

// THE TRACK LIST AND WHAT THE TRACK-INFO PLUGIN SAYS OF IT, the window's plugin poll as it hands it to the engine (pump move steps P2 and P3;
// named WebExtras until stage 5e, when the receiver plugins' pictures left it: they are the engine's own now, polled by its image feed
// - engine/image_feed.hpp - so what is left is the one thing only the window's plugin UI knows). The track-info plugin's answers for the
// tracks on screen used to be asked of the window while the engine published (the host hook enrichWebTrack), reading the window's
// TrackInfoCache - GUI-thread state - from the pass. The window builds this once a frame, immutable, and hands the pointer over inside
// FrontEndFacts (a copy of the facts is a refcount, so the publish's three copies stay free); the pass only looks the answer up. A
// headless engine has no window and so no feed: its status lists no tracks (whether it polls the plugin UI itself is stage 7's to decide).
struct TrackFeedInfo {
    std::string id;           // the track's id and kind: the key (the vector is sorted by id, then kind)
    unsigned kind = 0;
    unsigned infoState = 0;   // 1 = known, 2 = the source does not have it (RadioStatus::Track::infoState)
    std::string registration;
    std::string acType;
    std::string acOperator;
    std::string acCountry;
};
struct TrackFeed {
    std::vector<TrackFeedInfo> tracks;
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
// the counter's face (the web page mirrors it), the basemap plugin's figures, the track feed -
// handed to the engine once a frame (EngineShell::setFrontEndFacts) instead of the engine calling the window for them while it
// publishes.
struct FrontEndFacts {
    // (webListening was here until stage 5b: the web server is the engine's, and the receiver state's WEB_LISTENING bit is its host's answer.)
    // A front end that has handed nothing over (the headless engine) shows
    // the product's default face, as EngineHost's own answer always was.
    std::string tunerDisplayStyle = cascade::gui::tunerStyleName(cascade::gui::TunerStyle::Nixie);
    BasemapFacts basemap;
    // The track feed (above); null when there is none, which is every pass of a headless engine.
    std::shared_ptr<const TrackFeed> trackFeed;
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

// WHAT THE WINDOW ANSWERS A PAGE'S TILE REQUEST WITH (stage 5e; used by EngineShell::serveWebTiles). The page asks the engine's web server for
// a basemap tile, which records the want; the window - whose plugin client is the only thing that can produce one - drains the wants once a
// frame and publishes what its plugin has.
enum class WebTileAnswer { Pending, Missing, Ready };  // Pending: asked of the plugin, ask again later (nothing is published)
struct WebTileRequest {
    std::uint32_t z = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};
// Fills `bmp` (a 24-bit BMP) and answers Ready; Missing is the plugin saying the tile will never exist (published empty, so the page stops
// asking); Pending publishes nothing.
using WebTileSource = std::function<WebTileAnswer(const WebTileRequest&, std::vector<std::uint8_t>& bmp)>;

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

// (THE REMOTE CONTROLS' PULL - `RemoteControls` and `RemoteControlSource`, what a front-end-installed source handed the engine's pass - was
// here until stage 5d: the web server and CAT hold sessions on the table now, and the pass drains what they submit in the internal tier.
// docs/engine-stage4.md 6.12.)

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
    // One plugin woken or put to sleep, nothing rebuilt (stub campaign B2): the basemap and track-info tables are the engine's choice again,
    // but nothing was created that a window has to show.
    bool lifeOnly = false;
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

    // The nine host accessors (references into the plugin UI's vectors, valid for the frame: the directive applies at the head of
    // the frame only).
    virtual const std::vector<cascade::core::HostTrack>& tracks() const = 0;
    // THE TARGETS A TRACK SOURCE HEARS AND CANNOT PLACE YET (added 2026-10-27, stub campaign B16; END: stage 7h, when the plugin UI moves
    // with the host process and its vectors are no longer read through a feed). The ADS-B map lists an aircraft the receiver has heard and
    // not yet positioned (its position message has not arrived: 0.99.75), after every placed row and counted on the list's heading, and never
    // draws it. PluginUi keeps those targets in a vector of their own (PluginUi::unplacedTracks) precisely so that tracks() is only ever the
    // placed ones - every reader of tracks() (the map, the satellite panel, the fit, the radar scope, the coverage, the web snapshot) draws
    // or measures from a position - and this is the second read of the same object, valid for the frame like the other eight. The
    // alternative (merging them into tracks()) would have put a NaN coordinate in front of every one of those readers. One caller:
    // AppWindow::hostUnplacedTracks, for drawPluginWindows' per-page filter. Pure read; nothing is written through it.
    virtual const std::vector<cascade::core::HostTrack>& unplacedTracks() const = 0;
    virtual const std::vector<cascade::core::HostPath>& paths() const = 0;
    virtual const std::vector<cascade::core::HostPanel>& panels() const = 0;
    virtual const std::vector<cascade::core::HostInstrument>& instruments() const = 0;
    virtual const std::vector<std::string>& trackPluginNames() const = 0;
    virtual bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg,
                              double& outAltM) = 0;
    // Polls the plugins (the frame's one poll, after the head-of-frame directive).
    virtual void poll() = 0;
    // The receiver plugins' decoded pictures, brought up to date in `out` from the engine's one picture accumulator (ImageFeed::copyTo;
    // stage 5e: the engine polls the decoders and serves the pictures to the web page, this is the window's READ of them for its own image
    // windows - a poll made on the caller's thread, once a frame, exactly as before).
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
    // (The store's URL box is seeded from the table's fact.store.url, the status lines are read as fact.status.*, the frame's kick is the heartbeat of a
    // FOXAPI_SESSION_FRAME session and the two listeners' use is fact.web.served / fact.cat.accepted: shell retirement R3 retired
    // pluginCatalogueUrl, publishStatusText, statusText, frameKick and setServerUsageHooks from this door.)
    // The CASCADE_CONFIG_TEST printout's read-back: the demod mode the pipeline mirrors hold and the centre the source reports.
    struct ConfigReadback {
        int modeIndex = 0;
        double centreHz = 0.0;
    };
    ConfigReadback configReadback() const;
    cascade::core::AppConfig configSnapshot() const;
    void publishConfig();
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
    void shutdownDrain(std::chrono::steady_clock::time_point drainDeadline = std::chrono::steady_clock::time_point{});
    // The enforcement diagnostic, printed once after the first phase when the run asked (a bounded run's CASCADE_PLUGIN_STATUS).
    void reportPluginStatus(const std::vector<cascade::core::HostImage>& images, const PluginUiReport& ui);

    // --- the bounded-run seams: each is an environment-driven test or capture hook, honoured in a bounded run only ---
    // (R2 of the shell retirement, 2026-10-28: the eight seams of the window's ordinary hooks are CLIENT CODE now - the window sends the
    // table's own ops on its session: FOXAPP_OP_TEST_BIAS_STAND_IN, SET_CATALOGUE_URL + STORE_FETCH, SCAN_NATIVE_ONLY + SELECT_SOURCE read
    // from the DEVICES list, the state's FOXAPI_RX_DEVICE_OPENING and FOXAPI_RX_PATCH_RUNNING flags, PLUGIN_PRESET / PLUGIN_RUN_MODE read
    // from the PLUGINS and PRESETS lists, FOXAPP_OP_TEST_PATCH_RUNNING, and uiSubmit for the six commands the old seamSubmit carried.
    // What is left is the measurement run, which R9 moves to the host.)
    // The measurement run (FOXSDR_MEASURE): one step a frame over the pipeline, the pass's figures, and the generator's install.
    bool seamMeasureTick(cascade::core::EngineMeasure& measure, const cascade::core::MeasureHooks& hooks);
    void seamMarkPassStats();
    std::string seamPassStatsJson() const;
    void seamInstallSource(std::unique_ptr<cascade::source::IqSource> source);

    // ---------------------------------------------------------------------------------------------------------------------------
    // CLASS HOST (added 2026-10-21, stage 4 step 5; END: a feed over the table). The window's plugin UI, built over the engine's API
    // core and wired to the engine's directive link; and (2026-10-22, stage 5f) the basemap plugin's tiles for the browser's map.
    // ---------------------------------------------------------------------------------------------------------------------------
    std::unique_ptr<HostPluginFeed> makePluginFeed(HostPluginFeed::Steps steps);

    // THE BROWSER MAP'S TILES (moved here from the retired CLASS WEB / CAT, 2026-10-22, stage 5f; END: the basemap client over the table -
    // then the engine asks the plugin itself). The servers are the engine's, their control and their reads are sessions of the table and
    // the pictures are the engine's own; what is left is the basemap's TILES, which only the window's GUI-thread plugin client (basemap_)
    // can produce, so the window hands them over.
    //
    // ONE CALL A FRAME. `clearFirst` drops every stored tile and pending request (the basemap plugin changed or went away: tiles of the old
    // source must not be served under the new one). `serve` false (no basemap, or no web server listening) stops there. Otherwise the tiles
    // the page asked for since the last call are drained and the first `maxTiles` are asked of `source` and published; the rest are dropped
    // (the page asks again). Headless there is no caller: the status's basemap is not active (BasemapFacts' default) so the page asks for no
    // tile, and one asked for anyway is answered 202 "pending" - the route records the want (bounded) and nothing drains it.
    void serveWebTiles(bool clearFirst, bool serve, std::size_t maxTiles, const WebTileSource& source);

    // (THE BETA TESTER'S USAGE MARKS FOR THE TWO LISTENERS - setServerUsageHooks, added by stub campaign B15 - are gone, shell retirement R3: the servers keep two
    // counters, the table serves them as fact.web.served and fact.cat.accepted, and the window's recorder notes "browser" / "cat" when one has moved.)

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
