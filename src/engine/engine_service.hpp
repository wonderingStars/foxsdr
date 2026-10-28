// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// THE ENGINE'S LIFE WITHOUT A WINDOW (engine extraction stage 7c): who restores the settings, starts the
// receiver, keeps the settings saved while it runs, and stops it in order - for the headless binary
// (src/engine_main.cpp). The window application did all of this inside AppWindow (its constructor, run()
// and the teardown at the end of run()); this is the same life with the window's own parts left out, and
// "the same" is a contract, not a hope: the window's order has been broken once already (the 0.99.84
// merge lost whole statements from the teardown and found them only by running tests), so the order of
// each phase here is held against the order of the window's by tests/test_engine_service_order.cpp, which
// reads both files and keeps them in ONE table.
//
// FROM SHELL RETIREMENT R7b (2026-10-28) THE WINDOW APPLICATION IS A SECOND HOST OF THIS CLASS, and since R8 (2026-10-28) it is a COMPLETE one: src/main.cpp
// builds the service with the window application's options (engine/window_service_options.hpp), takes the lock, calls restore() BEFORE the window exists,
// constructs the window on the restored engine, calls start() (after the window's plugin UI has attached, before its GL window exists), runs the window,
// calls stop() with the window still on screen, and only then lets the window close its own GL context. The window no longer starts or stops the engine, and
// the window's copy of the keeper (the debounced save) is gone: the keeper is this class's, and for a host with no loop of its own a thread of its own
// (ServiceOptions::keeperThread).
//
// THE PHASES, in the order a run takes them (engine_service.cpp has each one's statements and the window's
// line numbers they come from):
//
//   acquireConfig  take the lock beside the settings file (core/config_lock.hpp) - BEFORE the file is read,
//                  because a program that reads a file it is about to rewrite must be the only one that may
//   restore        the window's constructor, minus the window: the engine's start-up, the writer, the file's
//                  load, the start-up state, the receiver's half applied, the usage journal's start-up, a
//                  damaged file kept aside, the first snapshot, the bookmarks, the status lines
//   start          the window's run() from the receiver start to the control thread: the receiver, the
//                  diagnostics directory, the settings forced to disk ONCE (the unclean-exit marker),
//                  the first pass, the control thread with the window's options
//   keep           what the window's frame loop does about the settings: collect a finished write, and
//                  save ~2 s after the last change (a compare of the engine's published snapshot, an
//                  asynchronous write through the same core::ConfigWriter, the same back-off)
//   stop           the window's teardown, statement for statement: no sound-card close waited for, the
//                  quiesce, the final-state save, the stop, the release, the clean-exit marker, the last
//                  save waited for within its bound, the drain
//
// HERMETIC OR PERSISTENT. A service built with no config path is hermetic, as the 7b skeleton was: no file
// read, written or locked; the built-in generator, muted. Given a path (foxsdr-engine --config PATH or
// --use-config) it owns that file for the life of the process. The survey's decision O3 - one shared file,
// a lock, a second program refused - is acquireConfig.
//
// WHAT IS DELIBERATELY NOT HERE, because the window's run() does it and a headless service does not (yet):
// the update check and the usage heartbeat (interactive window runs only; a service on the desktop's
// install id would double-count one machine - the 0.65.0 note in app_window.cpp), the crash-report upload
// sweep (survey O6: no UI has consented), the frame watchdog and the diagnostics switch's mid-session
// toggle, the plugin windows, the front end's facts. Stage 7d adds the signals that end a run, 7e the
// provisioning of the servers (provision(), between restore and start); both go through the phases above.
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "core/breadcrumb.hpp"
#include "core/config.hpp"
#include "core/config_lock.hpp"
#include "core/config_writer.hpp"
#include "engine/engine.hpp"
#include "engine/provision.hpp"

namespace cascade::engine {

// HOW THE SERVICE TREATS THE SETTINGS FILE'S LOCK (ServiceOptions::lock).
//   Strict       a held file REFUSES the run (acquireConfig answers false with the sentence): the headless engine, which shares the
//                file with the window by design and must never run beside it.
//   BestEffort   a held file is carried on from: the window application, which has always been allowed to start beside another (a
//                second window, an engine) and says so in its log (src/main.cpp, stage 7c; docs/engine-stage4.md 6.17).
enum class ServiceLock { Strict, BestEffort };

// WHAT A SERVICE WITH NO SETTINGS FILE IS (ServiceOptions::hermetic).
//   Headless     the built-in signal generator chosen explicitly and MUTED (a service has no business playing the demo tones at a sound
//                card): foxsdr-engine with no --config.
//   Window       the engine left exactly as it was built - volume 0.5, the generator as the source list's first row - which is what the
//                window application's hermetic runs (every --frames run and every test that builds a window) have always started from.
enum class ServiceHermetic { Headless, Window };

// EVERY OPTION'S DEFAULT IS WHAT THE HEADLESS BINARY HAS ALWAYS DONE (shell retirement R6): a default-constructed ServiceOptions is
// foxsdr-engine, byte for byte, and each default is pinned by a test on a default-constructed service (tests/test_engine_service_options.cpp)
// beside the 13 test_engine_main_* files that run the real binary. The window application will be a SECOND HOST of this class (R7, R8): it
// sets the options below to what it does today, and nothing about the engine's life is then written twice.
struct ServiceOptions {
    // The settings file this run owns, or empty: HERMETIC (nothing read, written or locked).
    std::string configPath;
    // Where hang reports may go when the stored Diagnostics switch is on: the window's setDiagnosticsDir
    // (BootResult::mayWrite ? diagCrashDir() : empty). Empty for a run that may not touch the disk.
    std::string diagnosticsDir;
    // TEST SEAM, bounded runs only (engine_main reads FOXSDR_ENGINE_UNLOAD_STALL_MS beside --seconds, as the
    // window reads CASCADE_DIAG_UNLOAD_STALL_MS beside --frames): wedge the stop for this many ms right
    // after the plugins are unloaded and before the clean-exit marker, and say so (on stderr; stdout with diagToStdout) - the place a
    // plugin's destroy() runs, where tests/test_clean_exit_marker.cpp holds the window's marker.
    int unloadStallMs = 0;

    // --- shell retirement R6: what the window application needs of the same life ---------------------------------------------------------
    // The lock beside the settings file: refuse the run when it is held (Strict, the headless engine) or carry on without it (BestEffort, the
    // window; the line it logs is the one main.cpp has always logged).
    ServiceLock lock = ServiceLock::Strict;
    // The engine of a run with no settings file (see ServiceHermetic).
    ServiceHermetic hermetic = ServiceHermetic::Headless;
    // start() starts the receiver (the RUN command). False for a window's bounded run, which stays stopped so CI keeps its timing and spawns
    // no DSP threads it does not need (the one exception, CASCADE_DECODE_TEST, is the host's to say).
    bool receive = true;
    // start() runs the control thread STEPPED (the interface's frame runs each part of the pass: a bounded window run) and makes no first pass
    // of its own - the first frame's front part is the first pass. False: free-running, with the first pass made here so the first read is real.
    bool stepped = false;
    // start() arms the usage heartbeat, right after the receiver is started and before anything else, as the window's run() does for an
    // interactive run. False: a service on the desktop's install id would double-count one machine, and every bounded run stays hermetic.
    bool heartbeat = false;
    // The settings are kept by a thread of the service's own (keep() every ~100 ms, a look at the snapshot a few times a second), started at the
    // end of start() and joined at the HEAD of stop(), instead of by the caller's wait loop. True for a host with no loop of its own to call keep()
    // from (the window: its frame loop has gone to the engine's side). False: the caller calls keep() as foxsdr-engine's wait loop does.
    bool keeperThread = false;
    // TEST SEAM, bounded runs only (the window's CASCADE_DIAG_SHUTDOWN_STALL_MS): wedge the stop for this many ms between the final-state save
    // and the receiver's stop - the stretch where the bounded driver waits are spent - and say so.
    int shutdownStallMs = 0;
    // TEST SEAM, bounded runs only (the window's FOXSDR_HOLD_AUDIO_LOCK, docs/engine-stage4.md 6.25 R8): start() sends FOXAPP_OP_TEST_HOLD_AUDIO_LOCK with this many
    // milliseconds, before the receiver starts and before the first pass, so the hold is in force before ANY other command can reach the engine. The window used to
    // submit it from run(); the engine runs, and its web server answers, from before the window exists now, and a browser's stop could arrive first.
    int holdAudioLockMs = 0;
    // Who the run says it is: the lock's role ("cascade" for the window application, "foxsdr-engine"), and the prefix of the lines the service
    // writes to the user ("<label>: ..."). Tests grep them (tests/test_clean_exit_marker.cpp, test_engine_main_clean_exit_marker.cpp).
    std::string label = "foxsdr-engine";
    // A bounded run that asked for the enforcement diagnostic (CASCADE_PLUGIN_STATUS): stop() makes sure the engine prints it at the end of
    // its first phase even when no interface handed over a plugin half (the window hands its own over first). False: no report.
    bool pluginStatusReport = false;
    // stop() sets the process's breadcrumb phase (core/breadcrumb.hpp) at the stages it owns - the receiver's stop, the plugins' unload, the
    // marker - for a host whose sentinel watches them. The window keeps ShutdownBegun and ShutdownClosingWindow, which bracket the service's three.
    // False: foxsdr-engine, which has never set one.
    bool breadcrumbs = false;
    // The two stall lines (and nothing else) go to stdout, where the window application writes them, instead of stderr, where
    // foxsdr-engine's tests read them.
    bool diagToStdout = false;
    // A FRONT END WILL ATTACH ITS PLUGIN UI after restore() (shell retirement R7: the window application, whose main.cpp restores first and constructs the
    // window on the restored engine). The engine then behaves, through the restore, as the engine a window built did when the window attached before the
    // scan - AUTO plugins left asleep, the plugin UI's directives kept for the attach to replay and built for nobody (Engine::expectFrontEnd). False:
    // the headless engine, whose window-less host treats AUTO as pinned and drives its own plugin UI.
    bool frontEndFollows = false;
    // stop() makes ONE WHOLE PASS before it does anything else (after the keeper thread is joined): everything an interface submitted last - the window's
    // last half of the settings, handed over as the frame loop ended - is applied by it, so the final-state save holds it. A stepped run's pass only runs
    // when someone steps it and the frames are over, so nothing else would; a free-running one is woken by the submit but not waited for. False:
    // foxsdr-engine, which has no interface to hand anything over and has never made one.
    bool finalPass = false;
};

class EngineService {
public:
    explicit EngineService(ServiceOptions options);
    ~EngineService();
    EngineService(const EngineService&) = delete;
    EngineService& operator=(const EngineService&) = delete;

    Engine& engine() { return engine_; }
    bool persistent() const { return !options_.configPath.empty(); }

    // Where a hang report may go right now: the diagnostics directory when boot let this run write at all AND the
    // stored Diagnostics switch (read by restore()) is on, else empty - the window's setReportDir rule, the one start()
    // gives the control thread's watchdog. What the stop's budget guard (engine/shutdown_guard.hpp) is handed.
    std::string reportDir() const { return diagnosticsEnabled_ ? options_.diagnosticsDir : std::string(); }

    // 1. THE LOCK. Strict (the default): false when another program holds the settings file (the window application, or another
    // foxsdr-engine), or the lock cannot be asked for: `sentence` is then the whole refusal, ready to print. BestEffort: true whatever
    // happened (what could not be taken is logged, as the window has always logged it) and `sentence` untouched. A hermetic service holds
    // nothing and always answers true. Call before restore(). releaseConfig lets the lock go before the service is destroyed - the window's
    // software-GL relaunch does so before the second process starts, so it can take it (src/main.cpp).
    bool acquireConfig(std::string& sentence);
    void releaseConfig() { lock_.release(); }

    // 2. THE RESTORE.
    struct Restored {
        bool loaded = false;    // the file was read (a missing file is a first run: defaults, loaded)
        bool damaged = false;   // the file failed to load: kept aside (or left alone, writes forbidden)
        std::string error;      // the loader's sentence when damaged; a refused built-in source when hermetic
        // --- WHAT A WINDOW HOST NEEDS OF THE RESTORE (shell retirement R7) -------------------------------------------------------------------
        // The window application (R7b) no longer reads the settings file: the engine took it (above), the interface's half comes back as fact.config.interface, and
        // what is left is what only the file said and neither the table nor the engine's snapshot carries.
        // Loaded: the clean-exit marker of the LAST run was false - the window's trigger to offer a crash report (AppWindow::diagnosticsStartup). The
        // engine counts the same observation as a crash; it does not publish it, and the snapshot's marker is this run's (false while it runs).
        bool lastRunUnclean = false;
        // Damaged and the file could not be kept aside: nothing may be saved over it, by anyone. The service's own writer is told; a host that
        // keeps a writer of its own (the window, until R8 gives the keeper to the service) must tell that one.
        bool writesForbidden = false;
        // Loaded: the saved source as the file named it - what the window's I/Q file path box shows whenever the saved source was a file,
        // opened or not (the engine's snapshot names what is open, which for a file that would not open is the keep-aside's business).
        std::string sourceKind;
        std::string iqFilePath;
        // Loaded: what CASCADE_CONFIG_TEST prints (Engine::configReadback, taken once the restore is applied).
        ConfigReadback readback;
    };
    Restored restore();
    // The CASCADE_CONFIG_TEST line, "config applied: mode=... center=..." or "config applied: defaults (<why>)", to stdout, from what restore()
    // returned (the window printed it from its constructor until R7; main and the test host call this now, when the run asked for it).
    static void announce(const Restored& restored);

    // 2b. THE PROVISION (stage 7e): the web and CAT flags of the command line, submitted as FOXAPI_OP_SERVER_CONFIG on a
    // LOCAL ADMIN session and settled (engine/provision.hpp) - after the restore, so what the flags say replaces what
    // the file said, and BEFORE the receiver starts. Nothing to do when the request asks for nothing. False with the
    // whole refusal in `error` (the engine's or the bind policy's sentence): the caller then stops. A refused provision
    // FORBIDS THE SAVES of this run, so the settings file keeps exactly what it held (a refused flag is not a setting;
    // the panels store what the operator chose, but nobody is choosing here, and a half-applied command line must not
    // become the next start's configuration). With a settings file the flags that were accepted ride the forced first
    // save of start() and the keeper; hermetic, they apply for this run and are written nowhere. `summary` is the
    // journal line on success.
    bool provision(const ProvisionRequest& request, const Secret* password, std::string& error, std::string& summary);

    // 3. THE START. False (with `why`) when the receiver could not be started; the caller then calls stop().
    bool start(std::string& why);

    // 4. THE KEEP: call it regularly (every few tens of ms) from the thread that waits for the end of the
    // run. Cheap when nothing changed; never blocks (the write is on the writer's own worker). With ServiceOptions::keeperThread a thread of the
    // service's own does this, and a call from anywhere else does nothing; keeperRunning is whether that thread is running.
    void keep();
    bool keeperRunning() const { return keeper_.joinable(); }

    // 5. THE STOP. Once; a second call does nothing.
    void stop();

    // Writes asked of the writer so far (the forced first save, each debounced one, the final two): what a
    // run's last stderr line reports, so a test can tell a quiet service from one that rewrites its file.
    std::uint64_t savesRequested() const { return saves_.load(); }

    // THE SETTINGS THE KEEPER COMPARES, as text: the whole serialised document without the fields that move on their own (the beta tester's running report,
    // its retry queue, the crash-upload limiter's memory, the exit marker, the session's start - core/config_halves.hpp, clearBookkeeping). Two configs the keeper
    // would NOT save a difference between have the same text. Public since shell retirement R8 so a test can ask "does the debounce see this change?" of the
    // production compare (tests/config_compare.hpp) instead of the window's hand-kept field list, which was deleted with the window's keeper.
    static std::string comparable(cascade::core::AppConfig cfg);

    // The sentence a refused lock prints (public so a test can hold its wording without running a process).
    static std::string refusal(const cascade::core::ConfigLock& lock, const std::string& configPath);

private:
    void keepOnce();
    void startKeeper();
    void stopKeeper();
    // A bounded run's deliberate wedge: says so as "<label>: --diag-<seam> wedging <what> for <ms> ms" (stderr, or stdout with diagToStdout), then waits.
    void wedge(const char* seam, const char* what, int ms) const;
    // Sets the breadcrumb phase when the host asked for them (ServiceOptions::breadcrumbs).
    void phase(cascade::core::breadcrumb::Phase p) const;
    double nowS() const;
    void setAsideDamaged();
    void saveNow();
    void requestSave(const cascade::core::AppConfig& cfg);
    void pollWriter();
    void maybeSave(double nowS);
    void noteWrite(bool ok, const std::string& error);
    bool retryHeld(double nowS);

    friend struct EngineServiceTestAccess;   // the keeper driven by hand (tests/service_keeper_access.hpp: the read-only folder, the refused write)
    ServiceOptions options_;
    // Declared so that they are destroyed in the order the window's are: the engine first, the writer
    // (which abandons a write still in flight after its short grace), the lock last - held to the end.
    cascade::core::ConfigLock lock_;
    cascade::gui::ConfigWriter writer_;
    Engine engine_;

    bool diagnosticsEnabled_ = true;   // the restored switch: the hang-report directory follows it
    bool stopped_ = false;
    std::chrono::steady_clock::time_point born_ = std::chrono::steady_clock::now();

    // The keeper's memory: what the file holds (or will), what was last asked to be written, and the
    // debounce - the window's savedCfg_, lastRequestedConfig_, pendingCfg_ and lastChangeTimeS_.
    std::string savedKey_;
    std::string pendingKey_;
    cascade::core::AppConfig lastRequested_;
    double lastChangeS_ = -1.0;
    double lastLookS_ = -1.0;   // the keeper looks a few times a second, not on every wake of the waiting thread
    std::atomic<std::uint64_t> saves_{0};   // (the keeper thread counts them, the host reads them)
    // The back-off after a failed write: doubles from the debounce window to five minutes.
    int failures_ = 0;
    bool failureFresh_ = false;
    double retryAtS_ = 0.0;
    bool failureLogged_ = false;
    // The keeper thread (ServiceOptions::keeperThread): started at the end of start(), joined at the head of stop() and by the destructor.
    std::thread keeper_;
    std::mutex keeperMutex_;
    std::condition_variable keeperCv_;
    bool keeperStop_ = false;
};

}  // namespace cascade::engine
