// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// THE ENGINE'S LIFE WITHOUT A WINDOW (engine extraction stage 7c): who restores the settings, starts the
// receiver, keeps the settings saved while it runs, and stops it in order - for the headless binary
// (src/engine_main.cpp). The window application does all of this inside AppWindow (its constructor, run()
// and the teardown at the end of run()); this is the same life with the window's own parts left out, and
// "the same" is a contract, not a hope: the window's order has been broken once already (the 0.99.84
// merge lost whole statements from the teardown and found them only by running tests), so the order of
// each phase here is held against the order of the window's by tests/test_engine_service_order.cpp, which
// reads both files and keeps them in ONE table.
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
// provisioning of the servers; both go through the phases above.
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "core/config.hpp"
#include "core/config_lock.hpp"
#include "core/config_writer.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

struct ServiceOptions {
    // The settings file this run owns, or empty: HERMETIC (nothing read, written or locked).
    std::string configPath;
    // Where hang reports may go when the stored Diagnostics switch is on: the window's setDiagnosticsDir
    // (BootResult::mayWrite ? diagCrashDir() : empty). Empty for a run that may not touch the disk.
    std::string diagnosticsDir;
    // TEST SEAM, bounded runs only (engine_main reads FOXSDR_ENGINE_UNLOAD_STALL_MS beside --seconds, as the
    // window reads CASCADE_DIAG_UNLOAD_STALL_MS beside --frames): wedge the stop for this many ms right
    // after the plugins are unloaded and before the clean-exit marker, and say so on stderr - the place a
    // plugin's destroy() runs, where tests/test_clean_exit_marker.cpp holds the window's marker.
    int unloadStallMs = 0;
};

class EngineService {
public:
    explicit EngineService(ServiceOptions options);
    ~EngineService();
    EngineService(const EngineService&) = delete;
    EngineService& operator=(const EngineService&) = delete;

    Engine& engine() { return engine_; }
    bool persistent() const { return !options_.configPath.empty(); }

    // 1. THE LOCK. False when another program holds the settings file (the window application, or another
    // foxsdr-engine), or the lock cannot be asked for: `sentence` is then the whole refusal, ready to print.
    // A hermetic service holds nothing and always answers true. Call before restore().
    bool acquireConfig(std::string& sentence);

    // 2. THE RESTORE.
    struct Restored {
        bool loaded = false;    // the file was read (a missing file is a first run: defaults, loaded)
        bool damaged = false;   // the file failed to load: kept aside (or left alone, writes forbidden)
        std::string error;      // the loader's sentence when damaged; a refused built-in source when hermetic
    };
    Restored restore();

    // 3. THE START. False (with `why`) when the receiver could not be started; the caller then calls stop().
    bool start(std::string& why);

    // 4. THE KEEP: call it regularly (every few tens of ms) from the thread that waits for the end of the
    // run. Cheap when nothing changed; never blocks (the write is on the writer's own worker).
    void keep();

    // 5. THE STOP. Once; a second call does nothing.
    void stop();

    // Writes asked of the writer so far (the forced first save, each debounced one, the final two): what a
    // run's last stderr line reports, so a test can tell a quiet service from one that rewrites its file.
    std::uint64_t savesRequested() const { return saves_; }

    // The sentence a refused lock prints (public so a test can hold its wording without running a process).
    static std::string refusal(const cascade::core::ConfigLock& lock, const std::string& configPath);

private:
    // The settings the window compares, as text: the snapshot without the three fields that move on their
    // own (the usage report that grows a second every second, the exit marker, the session's start).
    static std::string comparable(cascade::core::AppConfig cfg);
    double nowS() const;
    void setAsideDamaged();
    void saveNow();
    void requestSave(const cascade::core::AppConfig& cfg);
    void pollWriter();
    void maybeSave(double nowS);
    void noteWrite(bool ok, const std::string& error);
    bool retryHeld(double nowS);

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
    std::uint64_t saves_ = 0;
    // The back-off after a failed write: doubles from the debounce window to five minutes.
    int failures_ = 0;
    bool failureFresh_ = false;
    double retryAtS_ = 0.0;
    bool failureLogged_ = false;
};

}  // namespace cascade::engine
