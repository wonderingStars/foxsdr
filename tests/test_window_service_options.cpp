// test_window_service_options.cpp - WHAT THE WINDOW APPLICATION'S SERVICE IS BUILT WITH, held by value (shell retirement R8; docs/engine-stage4.md 6.25).
//
// Since R8 main() starts and stops the engine through EngineService and the window's run() no longer does; every decision run() used to make about HOW the
// engine is run - whether the receiver starts, whether the usage heartbeat is armed, whether the pass is stepped by the frames, which wedges a bounded run
// asks for - is one function, cascade::engine::windowServiceOptions (src/engine/window_service_options.cpp), which main.cpp and the test host both call.
// tests/test_engine_service_order.cpp holds its TEXT (and mutates it); this holds the VALUES, by calling it with the environments the decisions depend on:
//
//   A  A REAL SESSION (frames < 0): the receiver starts, the heartbeat is armed, the pass is free-running whatever the environment says, the wedges and
//      CASCADE_DECODE_TEST are NOT read (an interactive session can never be wedged or steered by a stray variable).
//   B  A BOUNDED RUN (frames >= 0): stays hermetic - no receiver (unless CASCADE_DECODE_TEST asks), no heartbeat, stepped (unless FOXSDR_PASS=free or a
//      measurement); the two wedges are read, a negative or empty one is none.
//   C  WHAT EVERY RUN HAS: the best-effort lock, the window's hermetic default, the label "cascade", the stall lines on stdout, the sentinel's breadcrumbs, a
//      front end that follows the restore, the keeper thread, the final pass - and the settings path and the diagnostics directory handed through.
//   D  THE HEADLESS DEFAULT IS UNMOVED: a default-constructed ServiceOptions has none of the window's new options (finalPass false), so foxsdr-engine is
//      what it was.
//
// Break-it: any line of the function changed (the order test's options mutants say which text; here the value goes RED): a real session not receiving,
// stepped, or reading a wedge; a bounded run arming the heartbeat; the keeper thread or the final pass off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>

#include "engine/engine_service.hpp"
#include "engine/window_service_options.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {

namespace ce = cascade::engine;

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value, 1);
    }
#endif
}

void clearEnv() {
    for (const char* n : {"CASCADE_DECODE_TEST", "FOXSDR_PASS", "CASCADE_DIAG_SHUTDOWN_STALL_MS", "CASCADE_DIAG_UNLOAD_STALL_MS"}) { setEnv(n, nullptr); }
}

ce::ServiceOptions optionsFor(int frames, bool measuring = false) {
    ce::WindowServiceRun run;
    run.configPath = "C:/scratch/settings.json";
    run.diagnosticsDir = "C:/scratch/diag";
    run.frames = frames;
    run.measuring = measuring;
    return ce::windowServiceOptions(run);
}

void commonToEveryRun(const ce::ServiceOptions& o) {
    CHECK(o.lock == ce::ServiceLock::BestEffort);
    CHECK(o.hermetic == ce::ServiceHermetic::Window);
    CHECK_EQ(o.label, std::string("cascade"));
    CHECK(o.diagToStdout);
    CHECK(o.breadcrumbs);
    CHECK(o.frontEndFollows);
    CHECK(o.keeperThread);
    CHECK(o.finalPass);
    CHECK_EQ(o.configPath, std::string("C:/scratch/settings.json"));
    CHECK_EQ(o.diagnosticsDir, std::string("C:/scratch/diag"));
}

void testRealSession() {
    std::printf("  A: a real session starts the receiver and the heartbeat, is free-running, and reads no test variable\n");
    clearEnv();
    // Every variable a bounded run honours, set: none of them may reach an interactive session.
    setEnv("CASCADE_DECODE_TEST", "1");
    setEnv("FOXSDR_PASS", "free");
    setEnv("CASCADE_DIAG_SHUTDOWN_STALL_MS", "900");
    setEnv("CASCADE_DIAG_UNLOAD_STALL_MS", "800");
    for (const int frames : {-1, -5}) {
        const ce::ServiceOptions o = optionsFor(frames);
        commonToEveryRun(o);
        CHECK(o.receive);
        CHECK(o.heartbeat);
        CHECK(!o.stepped);
        CHECK_EQ(o.shutdownStallMs, 0);
        CHECK_EQ(o.unloadStallMs, 0);
    }
    // Even a measurement flag does not step an interactive run.
    CHECK(!optionsFor(-1, true).stepped);
    clearEnv();
}

void testBoundedRun() {
    std::printf("  B: a bounded run stays hermetic - no receiver, no heartbeat, stepped - and reads the two wedges\n");
    clearEnv();
    for (const int frames : {0, 3, 240}) {
        const ce::ServiceOptions o = optionsFor(frames);
        commonToEveryRun(o);
        CHECK(!o.receive);
        CHECK(!o.heartbeat);
        CHECK(o.stepped);
        CHECK_EQ(o.shutdownStallMs, 0);
        CHECK_EQ(o.unloadStallMs, 0);
    }
    // CASCADE_DECODE_TEST starts the receiver in a bounded run (the decoder runner cannot be verified any other way) and nothing else changes.
    setEnv("CASCADE_DECODE_TEST", "1");
    {
        const ce::ServiceOptions o = optionsFor(3);
        CHECK(o.receive);
        CHECK(!o.heartbeat);
        CHECK(o.stepped);
    }
    setEnv("CASCADE_DECODE_TEST", "");   // set and empty: not asked for
    CHECK(!optionsFor(3).receive);
    clearEnv();
    // FOXSDR_PASS=free, and a measurement, make the pass free-running; any other value does not.
    setEnv("FOXSDR_PASS", "free");
    CHECK(!optionsFor(3).stepped);
    setEnv("FOXSDR_PASS", "stepped");
    CHECK(optionsFor(3).stepped);
    clearEnv();
    CHECK(!optionsFor(3, /*measuring=*/true).stepped);
    // The wedges: the variable's number; empty, unset and negative are none.
    setEnv("CASCADE_DIAG_SHUTDOWN_STALL_MS", "7000");
    setEnv("CASCADE_DIAG_UNLOAD_STALL_MS", "1500");
    {
        const ce::ServiceOptions o = optionsFor(3);
        CHECK_EQ(o.shutdownStallMs, 7000);
        CHECK_EQ(o.unloadStallMs, 1500);
    }
    setEnv("CASCADE_DIAG_SHUTDOWN_STALL_MS", "-40");
    setEnv("CASCADE_DIAG_UNLOAD_STALL_MS", "");
    {
        const ce::ServiceOptions o = optionsFor(3);
        CHECK_EQ(o.shutdownStallMs, 0);
        CHECK_EQ(o.unloadStallMs, 0);
    }
    clearEnv();
}

void testHeadlessDefaultUnmoved() {
    std::printf("  D: a default-constructed ServiceOptions is foxsdr-engine: none of the window's options on\n");
    const ce::ServiceOptions d;
    CHECK(!d.finalPass);
    CHECK(!d.keeperThread);
    CHECK(!d.heartbeat);
    CHECK(!d.breadcrumbs);
    CHECK(!d.frontEndFollows);
    CHECK(!d.diagToStdout);
    CHECK(d.receive);
    CHECK(!d.stepped);
    CHECK(d.lock == ce::ServiceLock::Strict);
    CHECK(d.hermetic == ce::ServiceHermetic::Headless);
    CHECK_EQ(d.label, std::string("foxsdr-engine"));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_window_service_options\n");
    testRealSession();
    testBoundedRun();
    testHeadlessDefaultUnmoved();
    return testSummary("test_window_service_options");
}
