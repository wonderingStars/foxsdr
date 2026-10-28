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
//   (There was a third class, HOST, until shell retirement R4 (2026-10-28): plugin-drawn content - the plugin UI's tracks, paths, panels and
//   instruments, polled and drawn on the window's thread (HostPluginFeed), and the basemap plugin's tiles the browser's map asks the engine's web
//   server for (serveWebTiles). It is the PLUGIN UI PORT now (core/plugin_ui_port.hpp), which the window reaches through the table's one
//   in-process extension, get_local_extension(FOXAPI_EXT_PLUGIN_UI) - docs/api-0.3-additions.md 9.15. The facts only the window knows
//   (setFrontEndFacts) and the bounded run's plugin status report (reportPluginStatus) went with it: they ride the port's setFrontEnd.)
//
//   (There was another class, WEB / CAT, until stage 5f. The browser's and the CAT client's servers are the ENGINE'S since stage 5b
//   (engine/server_host.hpp); the window configures them (FOXAPI_OP_SERVER_CONFIG) and reads them (the SERVERS list and the facts).
//   Their CONTROL is sessions of the table since 5c and 5d, and since 5e so are their READS: the page's spectrum and audio come
//   from the table on the web server's own session and the receiver plugins' pictures are polled and served by the engine. The class's
//   last method was the basemap's tiles, which only a GUI-thread plugin client can produce: it is plugin-drawn content, so it lives
//   with the plugin feed (then HOST, now the plugin UI port) and the class itself is gone. The track list the page shows is not a door
//   method: it is the port's TrackFeed. docs/engine-stage4.md 6.14.)
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
#include "core/plugin_ui_port.hpp"
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
    // WHERE A HANG REPORT MAY GO AT ALL (shell retirement R5): the directory this run is allowed to write reports into, whatever the
    // Diagnostics switch says now - empty for a run that may not touch the disk (a bounded one). `watchdogReportDir` above is where the
    // watchdog STARTS; this is where it goes when the interface's half later says the switch is on (Engine::applyInterfaceConfig), so the
    // switch reaches the control thread's watchdog as part of the half and not as a call of its own. Empty (the default) leaves the
    // watchdog wherever `watchdogReportDir` put it and makes the half's switch a no-op for it.
    std::string diagnosticsDir;
};

// THE THREE PARTS OF A PASS (pump move P4/P5). A free-running pass runs them in this order as one; a stepped one runs each when
// the front end asks (EngineShell::stepPass), which is how the window keeps the order the pump had in its frame - the part up to and
// including the publish, then the audio mute (which the frame ran after its look, before it drew), then the workers and audio:
//   Front  the GPS fix, the command queues, the inputs, the remote controls, the plugins and the scanner, the patch, the publish
//   Mute   the audio mute (a decision about the receiver the state it just published describes)
//   Back   the workers (device scans and opens, the retune coalescer, downloads) and the audio sink's health, the config copy
enum class PassPart { Front, Mute, Back };

// THE TRACK FEED, THE BASEMAP'S FIGURES AND THE BOUNDED-RUN STATUS REPORT'S PLUGIN HALF moved to core/plugin_ui_port.hpp with the plugin UI plane
// (shell retirement R4, 2026-10-28), beside the tile vocabulary and what one directive says to the window: they are the port's data, which the
// interface includes. They are in namespace cascade::core there, and named back here for the engine's code (R10 deletes this header and names them
// where they are used).
using cascade::core::TrackFeedInfo;
using cascade::core::TrackFeed;
using cascade::core::BasemapFacts;
using cascade::core::PluginUiReport;

// THE PLUGIN HALF OF THE BOUNDED-RUN STATUS REPORT AND THE PICTURES IT PRINTS (FrontEndFacts::pluginStatus; Engine::reportPluginStatus prints it).
struct PluginStatusFacts {
    PluginUiReport ui;
    std::vector<cascade::core::HostImage> images;
};

// WHAT THE FRONT END HANDS OVER FOR THE PUBLISH (engine/stage3b-pre, docs/engine-stage3.md OPEN 4): the facts only the window knows -
// the counter's face (the web page mirrors it), the basemap plugin's figures, the track feed -
// handed to the engine once a frame (PluginUiPort::setFrontEnd, which copies the window's PluginUiFrontEnd into this) instead of the engine
// calling the window for them while it publishes.
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
    // A BOUNDED RUN'S DIAGNOSTIC (CASCADE_PLUGIN_STATUS; shell retirement R4): what the window's plugin poll and picture buffer hold, handed over
    // once at the end of a run that asked for the report (PluginUiFrontEnd::pluginStatusReport) and printed, once, by Engine::shutdownQuiesce
    // right after its first phase - where the window used to call reportPluginStatus. Null in every other pass.
    std::shared_ptr<const PluginStatusFacts> pluginStatus;
};

// THE CONFIG TEST'S READ-BACK (CASCADE_CONFIG_TEST's one printed line; shell retirement R6 made it Engine::configReadback, public): the demod mode the
// pipeline mirrors hold (an index into the engine's mode table) and the centre the active source reports - values read back from the engine, not an
// echo of the file.
struct ConfigReadback {
    int modeIndex = 0;
    double centreHz = 0.0;
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

// (THE REMOTE CONTROLS' PULL - `RemoteControls` and `RemoteControlSource`, what a front-end-installed source handed the engine's pass - was
// here until stage 5d: the web server and CAT hold sessions on the table now, and the pass drains what they submit in the internal tier.
// docs/engine-stage4.md 6.12.)

// ==================================================================================================================================
// WHAT THE ENGINE ASKS OF ITS FRONT END (replaces the interface's use of EngineHost, which names the web server and the plugin UI)
// ==================================================================================================================================

// The clocks and the watchdog - three calls, thread-safe by construction (engine/stage3b-pre OPEN 4). The shell adapts the two watchdog calls to the
// engine's own EngineHost; the window implements all three. (wallTimeS is NOT asked of the window by the engine since shell retirement R7: the engine
// started on its own host's clock, in the service's restore, and a host that replaced it with glfwGetTime would have put the usage report's session
// start and its reads on two clocks. The window's own tester session still reads it. It goes with the class, R9.)
class ShellHooks {
public:
    virtual ~ShellHooks() = default;
    // The wall-clock the window's own usage report measures a session with (glfwGetTime()).
    virtual double wallTimeS() const = 0;
    // Paused across deliberate blocking work on the calling thread (a plugin rescan, a bounded device open).
    virtual void pauseWatchdog() = 0;
    virtual void resumeWatchdog() = 0;
};

// ==================================================================================================================================
// THE SHELL
// ==================================================================================================================================

class EngineShell {
public:
    // ---------------------------------------------------------------------------------------------------------------------------
    // CLASS SHELL (added 2026-10-21, stage 4 step 5; END: stage 7, the host process split from the interface). The bounded-run
    // driver and the engine's start and stop. AppWindow::AppWindow, ~AppWindow and run are the callers.
    // (THE COMPOSITION ROOT is not here any more: shell retirement R7, 2026-10-28, moved the construction of the engine to the host
    // process. main - or the test host - builds an EngineService, restores the settings and hands the window the engine's API table, its
    // handle and the engine itself; the shell is a VIEW of that engine now, not its owner. The restore - initialise, the settings
    // file's load and the receiver's half of it, the usage journal's start-up, the bookmarks, the CASCADE_CONFIG_TEST read-back - is the
    // service's, and the table's api and handle are main's to pass: initialise, loadBookmarks, telemetryStartup, applyConfig,
    // configReadback, apiTable and apiHandle left this door.)
    // ---------------------------------------------------------------------------------------------------------------------------

    // A view of the engine the host built, for the window `hooks` fronts (`hooks` must outlive the shell, and so must `engine`).
    // It installs the window's answers to the engine's three questions of its front end (the hang watchdog's pause and resume) with
    // Engine::setHost, and the destructor puts the engine's own host back (Engine::resetHost): a window can come and go over an engine
    // that stays. NOTHING in the engine is built or destroyed by a shell. Throws std::runtime_error, with the sentence, when the engine
    // refuses the host (its control thread is already running - setHost answers false then and changes nothing): a window that cannot
    // become the engine's host must not carry on as if it were one.
    static std::unique_ptr<EngineShell> view(Engine& engine, ShellHooks& hooks);
    ~EngineShell();
    EngineShell(const EngineShell&) = delete;
    EngineShell& operator=(const EngineShell&) = delete;

    // --- the settings the window keeps (until R8 moves the keeper) ---
    // (The store's URL box is seeded from the table's fact.store.url, the status lines are read as fact.status.*, the frame's kick is the heartbeat of a
    // FOXAPI_SESSION_FRAME session and the two listeners' use is fact.web.served / fact.cat.accepted: shell retirement R3 retired
    // pluginCatalogueUrl, publishStatusText, statusText, frameKick and setServerUsageHooks from this door.)
    cascade::core::AppConfig configSnapshot() const;
    void publishConfig();
    void setFrontEndWaitBeat(std::function<void()> beat);
    // (The Diagnostics switch reaches the control thread's watchdog inside the interface's half of the configuration - FOXAPP_OP_INTERFACE_CONFIG,
    // Engine::applyInterfaceConfig - against the directory the host named in ControlThreadOptions::diagnosticsDir: shell retirement R5 retired
    // setControlWatchdogReportDir from this door.)
    // (The engine API's table and the handle its LOCAL session opens on are main's to pass - AppWindow(WindowHost) - and not the door's: shell
    // retirement R7 retired apiTable and apiHandle.)

    // --- the run ---
    void stepPass(PassPart part);
    // (SHELL RETIREMENT R8, 2026-10-28: the engine's START and STOP left this door for EngineService, which main.cpp - or the test host - calls: the receiver
    // started, the control thread's first pass and the thread, the usage heartbeat (startReceiver, controlPass, startControlThread, configureHeartbeat), the
    // clean-exit marker (markCleanExit), the engine's transfers and teardown (stopTransfers, teardown) and its four shutdown phases (shutdownQuiesce,
    // shutdownStop, shutdownRelease, shutdownDrain). The window's own part of its end is the prelude of run() and closeWindow().)
    // (The enforcement diagnostic - a bounded run's CASCADE_PLUGIN_STATUS - is printed by the engine at the end of shutdownQuiesce, from what the
    // window handed over in its last setFrontEnd; shell retirement R4 retired reportPluginStatus from this door.)

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

private:
    EngineShell();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend Engine& rawEngine(EngineShell& shell);
};

// The Engine behind a shell, for the code that is entitled to it: the tests (through AppWindowTestAccess) and engine_shell.cpp. It
// returns an INCOMPLETE type to any translation unit that has not included engine/engine.hpp - which the interface cannot - so
// naming it from src/gui is a compile error at the first use, and tests/test_engine_shell_guard.cpp holds that nobody there names it.
Engine& rawEngine(EngineShell& shell);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_SHELL_HPP
