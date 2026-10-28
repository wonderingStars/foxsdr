// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// The engine's life without a window (engine extraction stage 7c). See engine_service.hpp for what each
// phase is. Each phase below is the window's own sequence with the window left out, written in the
// window's ORDER and with its comments where the order has a reason; the line numbers in the margin of
// each statement's comment are the places in src/gui/app_window.cpp it was read from (as of the stage
// 7c tree). tests/test_engine_service_order.cpp holds this file's order, and the window's, in one table:
// moving a statement here without moving it there - or the other way - is a red test, not a review
// comment.
#include "engine/engine_service.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

#include "core/app_commands.hpp"
#include "core/config_halves.hpp"
#include "core/damaged_file.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "engine/receiver_tables.hpp"
#include "source/soundcard_source.hpp"

namespace cascade::engine {

namespace {

namespace cmd = cascade::core::cmd;

// How long a change must sit still before it is saved, and how long a failed save is waited out at most:
// the window's kConfigDebounceS and kConfigRetryCapS (app_window.cpp), seconds as doubles, as there.
constexpr double kConfigDebounceS = 2.0;
constexpr double kConfigRetryCapS = 300.0;
// The keeper looks at the published snapshot this often, not on every wake of the thread that calls it
// (the window looks once a frame, and compares fields; this compares text, which is dearer).
constexpr double kLookEveryS = 0.25;
// The longest the provisioning flags may take to settle: a password's hash is the long part, and a slow board's
// is seconds, not a minute.
constexpr std::chrono::milliseconds kProvisionBound{60000};
// How often the keeper THREAD (ServiceOptions::keeperThread) looks at the settings: its wait between looks, ended at once by stop() (a notify), so the
// join at the head of stop() costs one look at most and a look never blocks.
constexpr std::chrono::milliseconds kKeeperPeriod{100};
// The longest stop() waits for the pass ServiceOptions::finalPass asks for: a pass is a few milliseconds, and a stop that cannot get one in half a
// second goes on (what the interface submitted last is then lost, which is what a crash would have lost).
constexpr std::chrono::milliseconds kFinalPassBound{500};

}  // namespace

EngineService::EngineService(ServiceOptions options) : options_(std::move(options)) {
    // Before anything can scan: the engine is built and restored for a front end that is not here yet (ServiceOptions::frontEndFollows).
    if (options_.frontEndFollows) { engine_.expectFrontEnd(true); }
}

EngineService::~EngineService() { stopKeeper(); }

double EngineService::nowS() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - born_).count();
}

// The text the keeper compares. The window's configsEqual leaves out the same families and says why:
// the pending usage report grows a second every second, the clean-exit flag is bookkeeping (false while
// running, true only on the marker's own rewrite), and the session's start is constant for the session (it
// rides the forced first save). Comparing the report would keep the debounce from ever settling (it moves every
// second), so no change would ever be saved; comparing the marker would read as a change at the first look.
// The same holds for the INTERFACE'S half, which the engine now holds as well (shell retirement R5): the beta tester's
// running report, its retry queue and invalid-token flag, and the crash-upload limiter's memory move by themselves.
// core/config_halves.hpp names all ten once (`moves`), and clearBookkeeping blanks them - the mask the survey's trap T-a
// asks for (tests/test_engine_service_options.cpp holds it: an idle service whose half carries a report that moves every
// 100 ms makes no save).
//
// This is a compare of the WHOLE serialised file, not of a hand-kept list of fields as configsEqual is:
// a field a later build adds to AppConfig is compared by being written, so it cannot be one that "reaches
// the file only when something else happens to change" - the failure configsEqual's own header warns of.
// The cost is that a field that moves by itself and is not listed here would keep the debounce from settling, and
// nothing would be saved; tests/test_engine_service_keep.cpp (an idle stretch, then a change that must land) says so.
std::string EngineService::comparable(cascade::core::AppConfig cfg) {
    cascade::core::clearBookkeeping(cfg);
    return cascade::core::ConfigStore::serialize(cfg);
}

void EngineService::wedge(const char* seam, const char* what, int ms) const {
    std::FILE* const to = options_.diagToStdout ? stdout : stderr;
    std::fprintf(to, "%s: --diag-%s wedging %s for %d ms\n", options_.label.c_str(), seam, what, ms);
    std::fflush(to);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void EngineService::phase(cascade::core::breadcrumb::Phase p) const {
    if (options_.breadcrumbs) { cascade::core::breadcrumb::setPhase(p); }
}

std::string EngineService::refusal(const cascade::core::ConfigLock& lock, const std::string& configPath) {
    using State = cascade::core::ConfigLock::State;
    if (lock.state() == State::Unavailable) {
        return "foxsdr-engine: not started - the lock that keeps two programs from saving one settings file at once "
               "could not be taken beside \"" + configPath + "\" (" + lock.error() +
               "), and without it nothing would stop a second program overwriting this one's settings";
    }
    std::string who = "another FoxSDR program";
    if (lock.holder().role == "cascade") {
        who = "the FoxSDR window application";
    } else if (lock.holder().role == "foxsdr-engine") {
        who = "another foxsdr-engine";
    }
    if (lock.holder().pid != 0) { who += " (process " + std::to_string(lock.holder().pid) + ")"; }
    return "foxsdr-engine: not started - " + who + " is already using the settings file \"" + configPath +
           "\". Two programs saving one settings file would overwrite each other's changes, so only one may use it "
           "at a time: close that program, or run this one on a settings file of its own with --config PATH";
}

// ---------------------------------------------------------------------------------------------------
// 1. THE LOCK.
//
// POSITION: before anything reads the file. A program that is going to read a settings file and later
// rewrite it from what it read must be the only one that may: a window that was open beside it would
// have its own changes put back by this one's next save. Hermetic: nothing to lock.
// ---------------------------------------------------------------------------------------------------
bool EngineService::acquireConfig(std::string& sentence) {
    if (!persistent()) { return true; }
    const cascade::core::ConfigLock::State state = lock_.acquire(options_.configPath, options_.label);
    if (state == cascade::core::ConfigLock::State::Held) { return true; }
    if (options_.lock == ServiceLock::BestEffort) {
        // The window application's way (src/main.cpp, stage 7c): a second window has always been allowed to start, and one that cannot get the lock
        // behaves exactly as it did before the lock existed. It says so in its log and carries on.
        cascade::core::diagLogf("settings: the lock beside the settings file is %s; carrying on",
                                state == cascade::core::ConfigLock::State::Contended ? "held by another FoxSDR program" : "unavailable");
        return true;
    }
    sentence = refusal(lock_, options_.configPath);
    return false;
}

// ---------------------------------------------------------------------------------------------------
// 2. THE RESTORE: the window's constructor (app_window.cpp, AppWindow::AppWindow), minus the window.
//
// ORDER (each step's reason is the window's):
//   initialise     the receiver's own start-up first (the recording folder, the demo signal, the band
//                  plan, the plugin scan, the output device list), so a config restore has something to
//                  restore onto                                                             [:808]
//   writer bound   before anything can ask it to write                                       [:827]
//   load           missing file: defaults and "loaded"; a damaged one: defaults and "not loaded", the
//                  construction defaults KEPT and nothing applied                            [:837]
//   start-up state the file records what was showing at the last exit; the application starts on the
//                  bench whatever that was (core::startupState: every open flag cleared), and EVERYTHING
//                  below reads that, never the file as saved                                [:21200]
//   apply          the receiver's half, all of it (Engine::applyConfig)                      [:21210]
//   usage journal  AFTER the apply, so the crash count and the pending report are read from the file
//                  rather than from freshly defaulted members                                [:849]
//   damaged        a file that failed to load is KEPT ASIDE before anything can save over it  [:856]
//   first snapshot the engine hands its half over first - no pass has run yet - and that is the
//                  baseline of the debounce: what the file holds (or would)                  [:873-875]
//   bookmarks      under the same gate as the settings: a hermetic run never reads them      [:884]
//   status lines   with or without a settings file, so the first read says what the restore said [:890]
//
// WHAT THE WINDOW ALSO DOES HERE AND THIS DOES NOT: the plugin feed, the engine API session, the
// window's own half of the restore (view state, the diagnostics and update switches, the crash-upload
// memory). None of it is the engine's; the fields it owns ride back into the file untouched, because the
// engine publishes from the config as it LOADED it (Engine::publishConfig starts from loadedCfg_).
// ---------------------------------------------------------------------------------------------------
EngineService::Restored EngineService::restore() {
    Restored out;
    engine_.initialise();
    writer_.bind(cascade::core::ConfigStore::writeFile);

    if (persistent()) {
        cascade::core::AppConfig cfg;
        std::string err;
        const bool loaded = cascade::core::ConfigStore::load(options_.configPath, cfg, err);
        if (loaded) {
            // THE START-UP STATE, then the receiver's half from it (the window's applyConfig, in its order).
            const cascade::core::AppConfig startup = cascade::core::startupState(cfg);
            engine_.applyConfig(startup);
            diagnosticsEnabled_ = cfg.diagnosticsEnabled;
            out.lastRunUnclean = !cfg.telemetryCleanExit;
            out.sourceKind = cfg.sourceKind;
            out.iqFilePath = cfg.iqFilePath;
            {
                std::string configDir;
                const std::size_t cut = options_.configPath.find_last_of("/\\");
                if (cut != std::string::npos) { configDir = options_.configPath.substr(0, cut); }
                engine_.telemetryStartup(cfg, configDir);
            }
            out.loaded = true;
            out.readback = engine_.configReadback();
        } else {
            std::fprintf(stderr, "%s: %s\n", options_.label.c_str(), err.c_str());
            // KEPT ASIDE BEFORE ANYTHING CAN SAVE OVER IT (core/damaged_file.hpp).
            setAsideDamaged();
            out.damaged = true;
            out.error = err;
            out.writesForbidden = writer_.writesForbidden();
        }
        // Baseline for the debounce: what the file holds (or would hold).
        engine_.publishConfig();
        const cascade::core::AppConfig first = engine_.configSnapshot();
        savedKey_ = comparable(first);
        pendingKey_ = savedKey_;
        lastRequested_ = first;
        engine_.loadBookmarks();
    } else if (options_.hermetic == ServiceHermetic::Headless) {
        // HERMETIC (the 7b skeleton's defaults, which a restored config replaces): the built-in signal
        // generator, chosen explicitly, MUTED - the receiver opens its audio sink on RUN whatever the
        // level, and a service has no business playing the demo tones at a sound card. (ServiceHermetic::Window leaves the engine as built -
        // volume 0.5, the generator as the list's first row - which is what the window's hermetic runs have always started from.)
        (void)engine_.applyCommand(cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.0));
        const cmd::QueuedCommand generator = cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "siggen");
        const FoxCommandResult selected = engine_.applyCommand(generator.c, generator.longText);
        if (selected.status != FOXAPI_OK && selected.status != FOXAPI_NO_CHANGE) {
            out.error = "the signal generator could not be selected (status " +
                        std::to_string(static_cast<int>(selected.status)) + ")";
        }
    }
    engine_.publishStatusText();
    return out;
}

void EngineService::announce(const Restored& restored) {
    if (restored.loaded) {
        std::printf("config applied: mode=%s center=%.0f\n", cascade::gui::kModeNames[restored.readback.modeIndex], restored.readback.centreHz);
    } else {
        std::printf("config applied: defaults (%s)\n", restored.error.c_str());
    }
}

// A damaged settings file is kept, not overwritten (0.99.65): the window's setAsideDamagedFile, for the one
// file this program has. If the rename is refused (held by another program, a read-only folder) the file stays
// exactly as it is and this service's writer writes nothing for the rest of the run.
void EngineService::setAsideDamaged() {
    const cascade::core::SetAsideResult r = cascade::core::setDamagedFileAside(options_.configPath);
    const unsigned long long bytes = static_cast<unsigned long long>(r.bytes);
    switch (r.outcome) {
        case cascade::core::SetAsideResult::Outcome::NothingToKeep:
            return;  // a first run, an empty file, or a folder: nothing to lose, nothing said
        case cascade::core::SetAsideResult::Outcome::KeptAside:
            // WHICH FILE AND HOW BIG, and no path: a log line never names one.
            cascade::core::diagLogf(
                "settings: a damaged file (%llu bytes) was kept aside beside the live one; the next save writes a new file", bytes);
            return;
        case cascade::core::SetAsideResult::Outcome::CouldNotKeep:
            break;
    }
    writer_.forbidWrites("config: not saved - the damaged settings file could not be kept aside");
    cascade::core::diagWarnf(
        "settings: a damaged file (%llu bytes) could not be kept aside, so nothing will be saved over it in this session", bytes);
    std::fprintf(stderr, "%s: the damaged settings file could not be kept aside, so nothing will be saved over it in this run\n",
                 options_.label.c_str());
}

// ---------------------------------------------------------------------------------------------------
// 2b. THE PROVISION (stage 7e): the window has no counterpart - its settings panels submit the same
// FOXAPI_OP_SERVER_CONFIG, one click at a time, while it runs. Here the command line says it all at once,
// so it is submitted once, after the restore (what the flags say replaces what the file said) and before
// the receiver starts (the configuration is settled - the password hashed, the listeners started or
// refused - before anything is receiving, and a refusal costs nothing but the run).
// ---------------------------------------------------------------------------------------------------
bool EngineService::provision(const ProvisionRequest& request, const Secret* password, std::string& error,
                              std::string& summary) {
    if (!request.any()) { return true; }
    const ProvisionOutcome outcome = provisionServers(engine_, request, password, kProvisionBound);
    if (!outcome.ok) {
        error = outcome.error;
        // A REFUSED PROVISION SAVES NOTHING: the file keeps what it held (the clean-exit marker included - this run
        // never started, so it did not end unclean either). The writer is told, as for a damaged file that could not
        // be kept aside; its sentence is what the stop then says.
        writer_.forbidWrites("the settings file was left as it was: the web/CAT provisioning was refused");
        return false;
    }
    summary = outcome.summary;
    return true;
}

// ---------------------------------------------------------------------------------------------------
// 3. THE START: the window's run() from the receiver's start to the control thread.
//
// ORDER:
//   receiver       interactive runs start receiving at once (a radio that opens with dead panels reads as
//                  broken); the window calls Engine::startReceiver, which is all the RUN command does
//                  beyond clearing a test fault                                              [:1225]
//   (heartbeat)    the window arms the update check and the usage heartbeat HERE, interactive runs
//                  only. A headless service arms NEITHER for now: a service on the desktop's install id
//                  would double-count one machine (the 0.65.0 note, app_window.cpp:1372-1378), and the
//                  heartbeat needs a "kind" on the Worker that has not been decided (survey O8). The
//                  start-up report of the PREVIOUS session is still sent, by the restore.   [:1371-1380]
//   diagnostics    the hang watchdog's report directory follows the stored switch             [:1393-1402]
//   first save     THE UNCLEAN-EXIT MARKER, FORCED TO DISK ONCE, HERE. telemetryCleanExit = false on
//                  disk is what tells the NEXT start that this run never shut down; it reaches the file
//                  only when the settings are saved, the save is debounced behind a CHANGE, and the
//                  debounce baseline was taken after the launch counter moved - so without this a run
//                  that died before anything changed would look, next start, like a clean exit [:1412-1427]
//   control thread the pass on its own thread, AFTER everything this thread does to the engine before
//                  the loop (the receiver, the first save), with the window's period and floor; the first
//                  pass made here so the first read is a real state                            [:1439-1458]
// ---------------------------------------------------------------------------------------------------
bool EngineService::start(std::string& why) {
    // The test seam of FOXSDR_HOLD_AUDIO_LOCK (ServiceOptions::holdAudioLockMs), FIRST: before the receiver starts and before any pass, as the window's submit was
    // applied by the first pass it made before it started the thread.
    if (options_.holdAudioLockMs > 0) { (void)engine_.applyCommand(cmd::makeInt(FOXAPP_OP_TEST_HOLD_AUDIO_LOCK, options_.holdAudioLockMs)); }
    if (options_.receive) {
        const FoxCommandResult started = engine_.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
        if (started.status != FOXAPI_OK || (started.flags & FOXAPI_RESULT_REFUSED) != 0u) {
            why = "the receiver could not be started (status " + std::to_string(static_cast<int>(started.status)) + ")";
            return false;
        }
    }

    // THE USAGE HEARTBEAT, for a host that asked (the window's interactive run): armed right after the receiver, before the diagnostics and the first save,
    // where run() arms it. configure() refuses an empty install id, so a run that opted out arms nothing.
    if (options_.heartbeat) { engine_.configureHeartbeat(); }

    const std::string reportDir = diagnosticsEnabled_ ? options_.diagnosticsDir : std::string();

    if (persistent()) { saveNow(); }

    cascade::engine::ControlThreadOptions opt;
    opt.period = std::chrono::milliseconds(16);
    opt.floor = std::chrono::milliseconds(2);
    opt.stepped = options_.stepped;
    opt.watchdogReportDir = reportDir;
    opt.diagnosticsDir = options_.diagnosticsDir;   // where the interface's half moves the control watchdog to when it switches Diagnostics on (shell retirement R5)
    if (!options_.stepped) { engine_.controlPass(); }   // a stepped run's first frame runs the pass's front part: that IS the first pass
    engine_.startControlThread(opt);
    // WHICH PASS THIS RUN HAS, in the log (the window's run() wrote it when it started the thread; tests/test_stop_ends_recordings.cpp reads it back to
    // prove the arrangement under test is the product's).
    cascade::core::diagLogf("engine pass: %s on its own thread (period %d ms, floor %d ms)",
                            options_.stepped ? "STEPPED by the frame" : "free-running", static_cast<int>(opt.period.count()),
                            static_cast<int>(opt.floor.count()));
    if (options_.keeperThread && persistent()) { startKeeper(); }
    return true;
}

// ---------------------------------------------------------------------------------------------------
// 4. THE KEEP: what the window's frame loop does about the settings, once a frame
// (pollConfigWriter, then maybeSaveConfig, app_window.cpp:1828-1837).
//
// The write is on the writer's worker (core::ConfigWriter: the 0.96.3 field report that put the window's
// config save off its thread is the same argument for a thread that must keep answering); this thread only
// reads the engine's PUBLISHED snapshot (Engine::configSnapshot is a copy behind a mutex, the engine's pass
// publishes it once a pass) and decides. It never calls Engine::publishConfig while the control thread
// runs: that is the pass's own call.
// ---------------------------------------------------------------------------------------------------
void EngineService::keep() {
    if (!persistent()) { return; }
    // With a keeper thread the thread keeps, and nobody else: the keeper's memory (the debounce, the back-off, the writer's poll) is its alone while it runs.
    if (keeper_.joinable()) { return; }
    keepOnce();
}

void EngineService::keepOnce() {
    pollWriter();
    const double now = nowS();
    if (lastLookS_ >= 0.0 && now - lastLookS_ < kLookEveryS) { return; }
    lastLookS_ = now;
    maybeSave(now);
}

// THE KEEPER THREAD (ServiceOptions::keeperThread): keepOnce a few times a second until it is told to stop. Started after the first save and the
// control thread, joined at the head of stop() - so everything stop() does with the writer and the keeper's memory (the final saves, the marker,
// the waits) is on one thread again - and by the destructor for a service that is never stopped.
void EngineService::startKeeper() {
    {
        std::lock_guard<std::mutex> lk(keeperMutex_);
        keeperStop_ = false;
    }
    keeper_ = std::thread([this] {
        std::unique_lock<std::mutex> lk(keeperMutex_);
        while (!keeperStop_) {
            lk.unlock();
            keepOnce();
            lk.lock();
            keeperCv_.wait_for(lk, kKeeperPeriod, [this] { return keeperStop_; });
        }
    });
}

void EngineService::stopKeeper() {
    if (!keeper_.joinable()) { return; }
    {
        std::lock_guard<std::mutex> lk(keeperMutex_);
        keeperStop_ = true;
    }
    keeperCv_.notify_all();
    keeper_.join();
}

void EngineService::pollWriter() {
    if (!writer_.poll()) { return; }
    if (writer_.lastOk()) {
        // Only when fully drained: a write that just finished but has another coalesced behind it is not yet
        // the LAST requested content.
        if (!writer_.inFlight()) { savedKey_ = comparable(lastRequested_); }
        noteWrite(true, std::string());
    } else {
        std::fprintf(stderr, "%s: %s\n", options_.label.c_str(), writer_.lastError().c_str());
        noteWrite(false, writer_.lastError());
        // THE SETTINGS FILE COULD NOT BE WRITTEN (a read-only profile, a full disk, a file another program holds)
        // and the service carries on with what it has: counted, once a session.
        cascade::core::health::noteRecovered(cascade::core::health::Recovered::CfgSave);
    }
}

void EngineService::noteWrite(bool ok, const std::string& error) {
    if (ok) {
        failures_ = 0;
        failureFresh_ = false;
        retryAtS_ = 0.0;
        failureLogged_ = false;  // the next failure starts a new run
        return;
    }
    ++failures_;
    failureFresh_ = true;  // the next look gives it the clock
    if (!failureLogged_) {
        failureLogged_ = true;
        const char* cause = "the write failed";
        if (error.find("cannot create temp file") != std::string::npos) {
            cause = "the settings folder would not take a new file";
        } else if (error.find("atomic replace") != std::string::npos) {
            cause = "the settings file is held by another program or could not be replaced";
        } else if (error.find("write to temp file") != std::string::npos) {
            cause = "the write failed part way";
        } else if (error.find("cannot create directory") != std::string::npos) {
            cause = "the settings folder could not be made";
        }
        cascade::core::diagWarnf("config: the settings could not be saved (%s); trying again, with a longer wait each time", cause);
    }
}

bool EngineService::retryHeld(double nowS) {
    if (failureFresh_) {
        failureFresh_ = false;
        double wait = kConfigDebounceS;
        for (int i = 0; i < failures_ && wait < kConfigRetryCapS; ++i) { wait *= 2.0; }
        if (wait > kConfigRetryCapS) { wait = kConfigRetryCapS; }
        retryAtS_ = nowS + wait;
    }
    return nowS < retryAtS_;
}

void EngineService::maybeSave(double nowS) {
    // A DAMAGED SETTINGS FILE THAT COULD NOT BE KEPT ASIDE is never saved over: the writer was told so at
    // start-up, and there is nothing to ask for.
    if (writer_.writesForbidden()) { return; }
    const bool held = retryHeld(nowS);
    const cascade::core::AppConfig cur = engine_.configSnapshot();
    const std::string curKey = comparable(cur);
    if (curKey == savedKey_) {
        lastChangeS_ = -1.0;  // clean again (e.g. change was undone)
        return;
    }
    if (lastChangeS_ < 0.0 || curKey != pendingKey_) {
        // First difference, or the state moved again: restart the debounce window so a burst of changes
        // writes once, when it stops.
        pendingKey_ = curKey;
        lastChangeS_ = nowS;
        return;
    }
    if (!held && nowS - lastChangeS_ >= kConfigDebounceS) {
        // THE REQUEST NEVER BLOCKS (core/config_writer.hpp). lastChangeS_ resets optimistically: if the
        // write goes on to fail, savedKey_ stays stale (pollWriter advances it on success only), so the very
        // next look reads "different" again and starts a fresh debounce window.
        requestSave(cur);
        lastChangeS_ = -1.0;
    }
}

void EngineService::saveNow() {
    // A save that must be CURRENT (the first, the final): the engine hands over its half as it is now, not as
    // the last pass left it. Only ever called with the control thread not running (before it starts, after
    // the quiesce joined it).
    engine_.publishConfig();
    requestSave(engine_.configSnapshot());
}

void EngineService::requestSave(const cascade::core::AppConfig& cfg) {
    lastRequested_ = cfg;
    // A writer told not to write (a damaged file that could not be kept aside) drops the request: not counted
    // as a save, so the run's last line never claims one that was refused.
    if (!writer_.writesForbidden()) { saves_.fetch_add(1); }
    writer_.requestAsync(options_.configPath, cascade::core::ConfigStore::serialize(cfg));
}

// ---------------------------------------------------------------------------------------------------
// 5. THE STOP: the window's teardown (app_window.cpp, run() from "beginShutdown" to the drain,
// :2149-2301), statement for statement. This order has been broken before - the 0.99.84 merge lost whole
// statements from it and found them only by running tests - so each line says why it is where it is.
//
//   close wait off   NO SOUND CARD CLOSE IS WAITED FOR FROM HERE ON: each is handed to its own thread and
//                    left there, so a card whose close hangs cannot spend the budget the radios' destroy
//                    below is inside                                                          [:2161]
//   quiesce          the servers first of all, the control thread joined, the TRANSMITTER (a keyed radio
//                    cannot wait), the GPS reader and its last poll, both recordings finalised [:2170]
//   (window only)    the plugin report, the crash-report sweep's stop, the feature-request senders:
//                    the window's, none here                                                  [:2177-2202]
//   final save       unconditional: guarantees the file matches the final state even when the debounce
//                    never fired. BEFORE the pipeline stops, so the snapshot reads live state; and the
//                    clean-exit flag is deliberately still FALSE - a death during the pipeline join is a
//                    death                                                                    [:2203]
//   stop             the bookmark flush, the pipeline stopped and joined - where the bounded driver waits
//                    are spent and the 120 s CAT freeze happened                              [:2222]
//   release          THE PLUGINS COME DOWN HERE, BEFORE THE CLEAN-EXIT MARKER (master 0.99.60; a merge
//                    once put them after it and test_clean_exit_marker found out): a plugin's destroy()
//                    is third-party code that can wedge, and a marker written before it already said
//                    "clean". The patch's radios first, the plugins destroyed, the modules unmapped last
//                                                                                             [:2234]
//   marker           AFTER the last of the third-party code. It REWRITES THE SNAPSHOT THE FINAL SAVE JUST
//                    ASKED FOR with one field changed, rather than publishing again: a second snapshot
//                    would re-derive every value at a moment when the session is half torn down (the
//                    usage report's clock, the source's live state)                           [:2260-2272]
//   last save        THE LAST SAVE MUST BE GIVEN A REAL CHANCE TO LAND, OR BE SEEN NOT TO: the one place
//                    that WAITS, bounded (ConfigWriter::kSaveBound, charged in the shutdown budget) and
//                    abandoned, with a line in the log, past the bound; the deadline is taken BEFORE this
//                    wait so what it leaves is what the drain gets                            [:2284-2297]
//   drain            the two lists' last saves and the takes' files, against the SAME deadline [:2301]
// ---------------------------------------------------------------------------------------------------
void EngineService::stop() {
    if (stopped_) { return; }
    stopped_ = true;

    // THE KEEPER THREAD, if there is one, first of all: from here the writer and the keeper's memory are this thread's alone.
    stopKeeper();

    // ONE WHOLE PASS (ServiceOptions::finalPass), while the control thread still runs and nothing has been quiesced: what an interface submitted last
    // - the window's last half of the settings - is applied by it, and the final-state save below holds it. Bounded.
    if (options_.finalPass) { (void)engine_.runPassNow(kFinalPassBound); }

    cascade::source::SoundCardSource::setCloseWaitEnabled(false);

    // A bounded run that asked for the enforcement diagnostic and has no interface to hand over its plugin half (ServiceOptions::pluginStatusReport).
    if (options_.pluginStatusReport) { engine_.requestPluginStatusReport(); }

    engine_.shutdownQuiesce();

    if (persistent()) { saveNow(); }

    // The deliberate wedge in the place the real one lives: the stretch around the receiver's stop, where the bounded driver waits are spent. A bounded
    // run's test seam only (ServiceOptions::shutdownStallMs); printed so a test can prove the process is inside it.
    if (options_.shutdownStallMs > 0) { wedge("shutdown-stall", "the teardown", options_.shutdownStallMs); }

    phase(cascade::core::breadcrumb::Phase::ShutdownStoppingReceiver);
    engine_.shutdownStop();

    phase(cascade::core::breadcrumb::Phase::ShutdownUnloadingPlugins);
    engine_.shutdownRelease();

    // The deliberate wedge in THIS stage - the one the plugins' own code runs in. A bounded run's test seam
    // only (ServiceOptions::unloadStallMs); printed so a test can prove the process is inside it before it
    // kills it.
    if (options_.unloadStallMs > 0) { wedge("unload-stall", "the plugin unload", options_.unloadStallMs); }

    phase(cascade::core::breadcrumb::Phase::ShutdownWritingMarker);
    engine_.markCleanExit();
    if (persistent()) {
        cascade::core::AppConfig marked = lastRequested_;
        marked.telemetryCleanExit = true;
        requestSave(marked);
    }

    const auto drainDeadline = std::chrono::steady_clock::now() + cascade::gui::ConfigWriter::kSaveBound;
    if (persistent()) {
        if (writer_.finishOrAbandon(cascade::gui::ConfigWriter::kSaveBound)) {
            if (writer_.lastOk()) {
                savedKey_ = comparable(lastRequested_);
            } else {
                std::fprintf(stderr, "%s: %s\n", options_.label.c_str(), writer_.lastError().c_str());
            }
        } else {
            cascade::core::diagLogf("config: final save abandoned - the disk did not answer within %lld ms",
                                    static_cast<long long>(cascade::gui::ConfigWriter::kSaveBound.count()));
        }
    }

    engine_.shutdownDrain(drainDeadline);
}

}  // namespace cascade::engine
