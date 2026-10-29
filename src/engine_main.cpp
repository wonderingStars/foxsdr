// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// foxsdr-engine: the receiver with no window (engine extraction stage 7b).
//
// WHAT THIS IS. A second executable on the engine library, beside `cascade`
// (the window application, src/main.cpp). It boots as an ENGINE process - the
// same sequence the window application boots through, with the kind's gates
// (src/engine/process_boot.hpp: no instance claim, no link activation, no
// stderr capture, no console handler, hermetic diagnostics and config) - and
// then runs the engine alone: no GLFW, no OpenGL, no ImGui, no display. It links
// cascade_engine only; the configure-time walk (CMakeLists.txt, "the line
// between them") covers it and so does tests/test_engine_main_links_headless.cpp.
//
// WHAT IT DOES TODAY (7b made the skeleton; 7c gave it a life-cycle and a config, 7d the signals and
// a bound on the stop, 7e the provisioning of the web and CAT servers):
//   * `--version`, the tool modes (`--selftest` and the hidden bench checks) and
//     the child-process helper modes the engine re-spawns itself for
//     (`--enumerate-json`, `--sentinel`, `--sdrplay-probe`, `--sdrplay-service-state`)
//     all answer inside bootProcess, exactly as they do in `cascade`;
//   * otherwise it runs the engine through an EngineService (src/engine/engine_service.hpp),
//     which is the window's life with the window left out: restore, start, keep, stop;
//   * HERMETIC by default, as the skeleton was: no settings file read, written or locked,
//     the built-in signal generator, MUTED (a service has no business playing a demo tone at a
//     sound card), nothing written to the disk at all (the diagnostics and config gates are
//     in boot);
//   * PERSISTENT when asked: `--config PATH` (that file) or `--use-config` (the per-user file the
//     window application uses). The run then takes the lock beside the file FIRST (a second
//     foxsdr-engine, or the window application, on the same file is refused with a sentence),
//     restores the settings the way the window does, keeps them saved while it runs and writes
//     the clean-exit marker last of all, in the window's stop order;
//   * `--seconds N` ends the run after N seconds: the stop is the window's order and the exit
//     code is 0. Without it the run goes on until it is ended from outside, and the engine says so
//     on its stderr line;
//   * a STOP SIGNAL ends it the same way (7d, core/stop_signal.hpp, armed by boot): SIGTERM (a service
//     manager's stop), SIGINT (Ctrl+C) and SIGHUP (the terminal closing) on Linux; Ctrl+C, Ctrl+Break and
//     a closing console on Windows (the window application's own console handler). The handler only asks;
//     the wait loop below reads the request and runs the orderly stop, exit 0, the clean-exit marker last.
//     A kill is not a stop: the marker stays FALSE and the next start counts the crash;
//   * PROVISIONED from the command line (7e, engine/provision.hpp): `--web-enable [--web-bind ADDR] [--web-port N]
//     [--web-user NAME] [--web-password-file FILE|-]` and `--cat-enable [--cat-bind-all]` are submitted as one
//     FOXAPI_OP_SERVER_CONFIG on a LOCAL ADMIN session after the restore and before the receiver starts, and settled
//     (the password hashed on the host's worker, the listeners started or refused by the bind policy) before the run
//     goes on. THE PASSWORD IS NEVER ON THE COMMAND LINE: the flag names a file, or `-` for standard input. A refusal
//     (the policy's sentence for an off-machine bind with no password, a port that is taken) is on stderr, the run
//     does not start, the exit code is 1 and a settings file is left as it was. With a settings file what was accepted
//     is kept and restored by a later run with no flags; hermetic, it applies for this run and nothing is written;
//   * EVERY STOP IS BOUNDED (7d, finding F5 of 7c, engine/shutdown_guard.hpp): armed around
//     EngineService::stop() whatever ended the run - a signal, `--seconds`, a run that failed to start - with
//     the window teardown's own budget (HangWatchdog::kShutdownThresholdMs, 53 s, a hang report when a report
//     directory is allowed) and ended after it, with exit code 3, if it has not returned.
//
// EXIT CODES (cascade's: 0 success, 1 a refused or failed run, 2 a refused
// helper argument):
//   0   the run was asked to end and did: `--seconds` elapsed, or a stop signal / console event arrived,
//       and the engine stopped in order; a tool mode that passed; `--version`
//   1   a refused argument; a tool mode that failed; the settings file is in use by another
//       program; the provisioning was refused or could not be read (a password file that is missing or
//       empty, a bind the policy will not allow, a port that is taken); the engine could not be started
//       (it threw, or the receiver did not come up) - the reason is on stderr
//   2   a helper mode refused its arguments (bootProcess)
//   3   the stop did not finish within its budget (53 s, then a 5 s grace for the hang report) and the
//       process was ended: the plugin unload, the pipeline join or a driver's close wedged. The clean-exit
//       marker is written last of all, so unless the stop had got to it the next start counts the run as a crash
//   (a process ended by a KILL has no code of its own: the operating system's - 137 for SIGKILL under a shell)
//
// stdout carries only what a mode prints (`--version`'s one line, a tool mode's
// verdict); the engine's own lines go to stderr, where a service manager keeps them.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include "core/diag_log.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/stop_signal.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"
#include "engine/engine_service.hpp"
#include "engine/process_boot.hpp"
#include "engine/provision.hpp"
#include "engine/shutdown_guard.hpp"

namespace {

using cascade::engine::Engine;
using cascade::engine::EngineService;
using cascade::engine::ShutdownGuard;

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;

// How long the receiver is given to come up before the run is called failed: the
// first pass has published well inside this on every machine measured; a slower
// one is told, not waited on for ever.
constexpr std::chrono::milliseconds kStartWait{5000};
// The sleep between looks at the clock, the stop request and the settings. The engine
// runs on its own thread; this thread only decides when to end it and keeps the settings
// saved (EngineService::keep, which looks at them a few times a second whatever this is).
constexpr std::chrono::milliseconds kWaitSlice{20};

// Has something ended the run from outside: a stop signal (SIGTERM, SIGINT, SIGHUP) or a console event (Ctrl+C,
// Ctrl+Break, a closing console), armed by boot (step 14). The handlers only set a flag; the wait loops below read it
// and the run stops in order, on this thread.
bool stopRequested() { return cascade::core::stopSignalRequested(); }

bool receiverRunning(const Engine& engine) {
    cascade::core::PublishedState ps = cascade::core::initialPublishedState();
    (void)engine.snapshot()->read(ps);
    return ps.app.published && (ps.rx.flags & FOXAPI_RX_RUNNING) != 0u;
}

// A positive whole number of milliseconds from the environment, or 0: the bounded-run test seam below.
int envMillis(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') { return 0; }
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return (end != v && *end == '\0' && n > 0 && n <= 600000L) ? static_cast<int>(n) : 0;
}

// The budget guard for one stop (shutdown_guard.hpp): the window teardown's budget, the hang report when this run may write
// one, and - a bounded run's TEST SEAM only (`--seconds`, as the unload stall above) - a smaller budget, so a wedged stop can
// be ended in seconds instead of a minute.
ShutdownGuard::Options guardOptions(const EngineService& service, const cascade::engine::BootResult& boot) {
    ShutdownGuard::Options o;
    o.reportDir = service.reportDir();
    if (boot.seconds >= 0.0) {
        const int ms = envMillis("FOXSDR_ENGINE_SHUTDOWN_BUDGET_MS");
        if (ms > 0) { o.budgetMs = static_cast<unsigned>(ms); }
    }
    return o;
}

// The whole run: lock, restore, start, wait (keeping the settings), stop. Returns the exit code.
int runEngine(const cascade::engine::BootResult& boot) {
    cascade::engine::ServiceOptions options;
    options.configPath = boot.configPath;
    options.safePlugins = boot.safePlugins;
    if (boot.safePlugins) { std::fprintf(stderr, "foxsdr-engine: safe mode - no plugin will be loaded in this run\n"); }
    // The hang watchdog's report directory when this run may write diagnostics at all (boot decided) and the
    // stored switch is on - the window's setDiagnosticsDir, handed over the same way (main.cpp).
    options.diagnosticsDir = boot.mayWrite ? cascade::core::diagCrashDir() : std::string();
    // THE TEST SEAM, a bounded run's only (`--seconds`; the window's CASCADE_DIAG_UNLOAD_STALL_MS is honoured
    // with `--frames` only, for the same reason: no interactive run can be wedged by a stray variable).
    if (boot.seconds >= 0.0) { options.unloadStallMs = envMillis("FOXSDR_ENGINE_UNLOAD_STALL_MS"); }
    const bool persistent = !options.configPath.empty();

    // THE PASSWORD, read before anything is locked, read or started (stage 7e): a file that is missing or empty refuses the
    // run while nothing has happened yet. It is in `password` - and in nothing that is logged - until the provisioning has
    // handed it to the engine, and is overwritten there (Secret). It is never an argument of this process: the flag names the
    // file (process_boot.cpp), so it is in no process list and no shell history.
    cascade::engine::Secret password;
    if (!boot.provision.passwordFile.empty()) {
        std::string why;
        if (!cascade::engine::readPassword(boot.provision.passwordFile, password, why)) {
            std::fprintf(stderr, "foxsdr-engine: %s\n", why.c_str());
            return kExitFailed;
        }
    }

    EngineService service(std::move(options));
    Engine& engine = service.engine();

    // THE LOCK FIRST, before the settings file is read: a program that is going to rewrite a file it read must
    // be the only one that may (core/config_lock.hpp). Refused: say so, in a sentence, and do not start.
    std::string sentence;
    if (!service.acquireConfig(sentence)) {
        std::fprintf(stderr, "%s\n", sentence.c_str());
        return kExitFailed;
    }

    const EngineService::Restored restored = service.restore();
    if (!restored.error.empty() && !restored.damaged) {
        std::fprintf(stderr, "foxsdr-engine: %s\n", restored.error.c_str());
        {
            ShutdownGuard bound(guardOptions(service, boot));  // a stop is bounded whatever ended the run
            bound.arm();
            service.stop();
        }
        return kExitFailed;
    }

    // THE PROVISIONING (7e): the web and CAT flags, after the restore (what the command line says replaces what the file said)
    // and before the receiver starts. A refusal ends the run here - nothing is receiving, and the service has told its writer
    // to save nothing, so a settings file keeps what it held.
    {
        std::string refused;
        std::string provisioned;
        const bool ok = service.provision(boot.provision, password.empty() ? nullptr : &password, refused, provisioned);
        cascade::engine::Secret::wipe(password.value);  // the engine has its own copy now, overwritten once hashed
        if (!ok) {
            std::fprintf(stderr, "%s\n", refused.c_str());
            {
                ShutdownGuard bound(guardOptions(service, boot));
                bound.arm();
                service.stop();
            }
            return kExitFailed;
        }
        if (!provisioned.empty()) {
            std::fprintf(stderr, "foxsdr-engine: %s - %s\n", provisioned.c_str(),
                         persistent ? "kept in the settings file" : "for this run only (no settings file: nothing is stored)");
        }
    }

    std::string why;
    if (!service.start(why)) {
        std::fprintf(stderr, "foxsdr-engine: %s\n", why.c_str());
        {
            ShutdownGuard bound(guardOptions(service, boot));
            bound.arm();
            service.stop();
        }
        return kExitFailed;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsedS = [&t0] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };

    bool up = false;
    while (!up && !stopRequested() &&
           std::chrono::steady_clock::now() - t0 < kStartWait) {
        up = receiverRunning(engine);
        if (!up) {
            service.keep();
            std::this_thread::sleep_for(kWaitSlice);
        }
    }
    int code = kExitOk;
    if (!up && !stopRequested()) {
        std::fprintf(stderr, "foxsdr-engine: the receiver did not report running within %lld ms\n",
                     static_cast<long long>(kStartWait.count()));
        code = kExitFailed;
    } else {
        const char* tail = boot.seconds >= 0.0 ? "" : " (until ended from outside)";
        if (persistent) {
            std::fprintf(stderr, "foxsdr-engine %s: running, settings file in use (%s)%s\n", cascade::versionString(),
                         restored.damaged ? "damaged, defaults in use" : "restored", tail);
        } else {
            std::fprintf(stderr, "foxsdr-engine %s: running, signal generator, muted, no config%s\n",
                         cascade::versionString(), tail);
        }
        std::fflush(stderr);
        while (!stopRequested() && (boot.seconds < 0.0 || elapsedS() < boot.seconds)) {
            service.keep();
            std::this_thread::sleep_for(kWaitSlice);
        }
    }

    // WHAT ENDED THE RUN, when it was asked from outside: said before the stop begins, so a journal reads "asked, then
    // stopping, then stopped" and a stop that wedges is known to have been the one that was asked for.
    if (stopRequested()) {
        const char* what = cascade::core::stopSignalName();
        std::fprintf(stderr, "foxsdr-engine: stop requested (%s), stopping\n", *what != '\0' ? what : "a stop signal");
        std::fflush(stderr);
    }

    // THE STOP, in the window's order (EngineService::stop: no sound-card close waited for, quiesce, the
    // final-state save, stop, release, the clean-exit marker, the last save waited for within its bound, the
    // drain), UNDER ITS BUDGET (ShutdownGuard: the hang watchdog's shutdown budget and, past it, the end of the
    // process with exit code 3). Armed whatever ended the run - a signal or `--seconds` - and disarmed when the stop
    // returns.
    const std::uint64_t passes = engine.passesEnded();
    {
        ShutdownGuard bound(guardOptions(service, boot));
        bound.arm();
        service.stop();
    }
    std::fprintf(stderr, "foxsdr-engine: stopped after %.1f s (%llu passes)\n", elapsedS(),
                 static_cast<unsigned long long>(passes));
    if (persistent) {
        std::fprintf(stderr, "foxsdr-engine: settings saved %llu time(s)\n",
                     static_cast<unsigned long long>(service.savesRequested()));
    }
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    // EVERYTHING before the engine: the console, the helper modes, the crash
    // handlers, the argument parse, the tool modes - in bootProcess, in its order.
    // A helper (`--enumerate-json`, `--sentinel`, ...), a tool mode, `--version`
    // or a refused argument ends the process here.
    const cascade::engine::BootResult boot =
        cascade::engine::bootProcess(cascade::engine::ProcessKind::Engine, argc, argv);
    if (boot.exit) { return boot.exitCode; }
    try {
        return runEngine(boot);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "foxsdr-engine: failed: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "foxsdr-engine: failed: unknown exception\n");
    }
    return kExitFailed;
}
