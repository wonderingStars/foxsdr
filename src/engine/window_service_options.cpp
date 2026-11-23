// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// The options of the window application's service (engine/window_service_options.hpp). Every line is a decision the window's run() used to make for
// itself, moved here with its reason; the line numbers of the old places are in docs/engine-stage4.md 6.25 (R8).
#include "engine/window_service_options.hpp"

#include <cstdlib>
#include <string>

namespace cascade::engine {

namespace {

// A bounded run's wedge, in milliseconds: the variable's number, or 0 when unset, empty or negative (the window read them with atoi and clamped).
int stallFromEnvironment(const char* name) {
    const char* const v = std::getenv(name);
    if (v == nullptr || *v == '\0') { return 0; }
    const int ms = std::atoi(v);
    return ms < 0 ? 0 : ms;
}

}  // namespace

ServiceOptions windowServiceOptions(const WindowServiceRun& run) {
    const bool bounded = run.frames >= 0;
    ServiceOptions o;
    o.configPath = run.configPath;
    o.safePlugins = run.safePlugins;
    // Empty unless this run is ALLOWED to write - which is not the same as whether the user wants it: the Settings toggle works in a session that
    // started with diagnostics off, so the directory is named whenever boot allows it (the service follows the stored switch itself).
    o.diagnosticsDir = run.diagnosticsDir;

    // THE LOCK is BEST EFFORT and never a refusal: a second window has always been allowed to start, and the window application behaves exactly as it
    // did when it cannot get the lock (another window holds it, or an engine does - the one case the engine's own refusal cannot cover, recorded in
    // docs/engine-stage4.md 6.17). The service logs the line and answers true. (Whether to TAKE the lock at all - a real session only - is main's.)
    o.lock = ServiceLock::BestEffort;
    // THE ENGINE OF A RUN WITH NO SETTINGS FILE is left exactly as it was built (volume 0.5, the generator as the list's first row): what every
    // --frames run and every test that builds a window has always started from.
    o.hermetic = ServiceHermetic::Window;
    // The lines the service writes to the user are "cascade: ..." as the window's have always been; the stall lines go where the window wrote them,
    // to stdout (tests/test_clean_exit_marker.cpp and test_shutdown_budget.cpp read them there).
    o.label = "cascade";
    o.diagToStdout = true;
    // The window attaches its plugin UI AFTER the restore: the scan is the window's engine's scan (Engine::expectFrontEnd).
    o.frontEndFollows = true;
    // The sentinel's shutdown steps (core/breadcrumb.hpp): the service sets the three it owns, the window keeps ShutdownBegun and ShutdownClosingWindow.
    o.breadcrumbs = true;
    // The settings are kept by a thread of the service's own: the window's frame loop has gone to the engine's side of the line, and the window's
    // copy of the keeper is gone with it (R8).
    o.keeperThread = true;
    // The window hands its last half of the settings over as its frame loop ends; the stop makes one whole pass to take it before the final save.
    o.finalPass = true;

    if (!bounded) {
        // A REAL SESSION. Interactive runs start receiving immediately: a radio that opens with dead black panels and no hint that a button must be
        // pressed reads as broken, the single biggest first-run complaint. The usage heartbeat is interactive-only too, and that is measured, not
        // hypothetical: 0.65.0 armed it in telemetryStartup, so every app-level ctest run minted a fresh isolated config (whose default is reporting
        // on), got a brand-new install id and fired its first-poll beat at the LIVE endpoint - one machine running one copy read as four "running right
        // now". The pass is free-running: an interactive run is never stepped, whatever the environment says.
        o.receive = true;
        o.heartbeat = true;
        o.stepped = false;
        return o;
    }

    // A BOUNDED RUN stays stopped so CI keeps its old timing and never spawns DSP threads it does not need - except CASCADE_DECODE_TEST, which exists
    // because a decoder that is never fed a sample looks exactly like a decoder that is fed and finds nothing. Opt-in, bounded-run only.
    const char* const decodeHook = std::getenv("CASCADE_DECODE_TEST");
    o.receive = decodeHook != nullptr && *decodeHook != '\0';
    o.heartbeat = false;
    // WHICH PASS: a bounded run is STEPPED unless FOXSDR_PASS=free asks otherwise (adopted decision 3), so N frames is N frames and every test and
    // picture that holds one stays reproducible. The engine measurement is the one bounded run that is free-running by nature.
    const char* const passEnv = std::getenv("FOXSDR_PASS");
    const bool wantFree = passEnv != nullptr && std::string(passEnv) == "free";
    o.stepped = !wantFree && !run.measuring;
    // The two deliberate wedges of the stop (the only way to hold the shutdown budget against the real teardown of the real binary) - bounded runs
    // only, so an interactive session can never be wedged by a stray environment variable.
    o.shutdownStallMs = stallFromEnvironment("CASCADE_DIAG_SHUTDOWN_STALL_MS");
    o.unloadStallMs = stallFromEnvironment("CASCADE_DIAG_UNLOAD_STALL_MS");
    // FOXSDR_HOLD_AUDIO_LOCK (0.99.68): the audio mutex held for this long inside the first control request's stop. The engine is the host's to start, and it answers
    // a browser before the window exists, so the seam is in force from the start and not submitted by the window once its frames are running.
    o.holdAudioLockMs = stallFromEnvironment("FOXSDR_HOLD_AUDIO_LOCK");
    return o;
}

}  // namespace cascade::engine
