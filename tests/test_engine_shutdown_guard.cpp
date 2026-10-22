// The stop's budget guard in process (engine extraction stage 7d, finding F5 of 7c): engine/shutdown_guard.{hpp,cpp}.
// The real ending of a process is held by tests/test_engine_main_sigterm_wedged.cpp on the real executable; what is held
// here is the guard itself, with an ender of the test's own, and where engine_main.cpp puts it.
//
//   G1. THE BUDGET RUNS OUT: the ender is called once, with the guard's exit code (3), and not before the budget plus the
//       grace.
//   G2. DISARMED IN TIME: nothing is called, however long one waits afterwards; and disarm() is prompt - it does not wait
//       out the budget it interrupted (a clean stop must not cost the process its budget).
//   G3. THE DESTRUCTOR DISARMS (the scope the engine uses): the same.
//   G4. IDEMPOTENT: a second arm() starts nothing more; a disarmed guard is not armed again.
//   G5. NO ARMING, NO ENDING: a guard that was never armed never fires (the positive of "armed means bounded").
//   S.  WHERE IT IS (source scans of src/engine_main.cpp, the way tests/test_engine_service_order.cpp reads it): every
//       EngineService::stop() in runEngine is inside a block that arms a ShutdownGuard first (the signal-driven stop,
//       the `--seconds` stop and the two failed-start stops alike); the budget is the guard's default - the window
//       teardown's HangWatchdog::kShutdownThresholdMs - and the only thing that changes it is the bounded-run seam, read
//       only under `--seconds`.
//   C.  THE CONSTANTS: exit code 3 (beside 0, 1, 2), grace 5 s, budget 53 s.
//
// Break-it (docs/engine-stage4.md 6.18): ShutdownGuard::arm() returning at once -> G1 RED (never fires) and tests/
// test_engine_main_sigterm_wedged.cpp RED; a service.stop() with no guard before it -> S RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>

#include "core/hang_watchdog.hpp"
#include "engine/shutdown_guard.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::engine::ShutdownGuard;
using Clock = std::chrono::steady_clock;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::size_t countOf(const std::string& s, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) { ++n; }
    return n;
}

template <class T, class U>
bool same(T a, U b) { return a == b; }

double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Probe {
    std::atomic<int> calls{0};
    std::atomic<int> code{-1};
    std::atomic<long long> atMs{0};
    Clock::time_point t0 = Clock::now();
    std::function<void(int)> ender() {
        return [this](int c) {
            code = c;
            atMs = static_cast<long long>(ms(t0, Clock::now()));
            ++calls;
        };
    }
};

ShutdownGuard::Options options(unsigned budgetMs, unsigned graceMs, Probe& p) {
    ShutdownGuard::Options o;
    o.budgetMs = budgetMs;
    o.graceMs = graceMs;
    o.ender = p.ender();
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_engine_shutdown_guard\n");

    // C. the constants.
    // (read through a function, so a constant on both sides is not a "constant conditional" to the compiler)
    CHECK(same(ShutdownGuard::kExitWedged, 3));
    CHECK(same(ShutdownGuard::kEndGraceMs, 5000u));
    CHECK(ShutdownGuard::Options{}.budgetMs == cascade::core::HangWatchdog::kShutdownThresholdMs);
    CHECK(same(cascade::core::HangWatchdog::kShutdownThresholdMs, 53000u));
    CHECK(ShutdownGuard::Options{}.graceMs == ShutdownGuard::kEndGraceMs);
    CHECK(ShutdownGuard::Options{}.exitCode == 3);

    // G1. the budget runs out.
    {
        Probe p;
        ShutdownGuard g(options(300, 200, p));
        CHECK(!g.armed());
        p.t0 = Clock::now();
        g.arm();
        CHECK(g.armed());
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        CHECK(p.calls == 0);  // not yet: the budget alone (300) has not passed
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(p.calls == 0);  // the budget has, the grace (to 500) has not
        for (int i = 0; i < 100 && p.calls == 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        std::printf("  G1: the ender was called %d time(s), with code %d, at %lld ms (budget 300 + grace 200)\n", p.calls.load(),
                    p.code.load(), p.atMs.load());
        CHECK(p.calls == 1);
        CHECK(p.code == 3);
        CHECK(p.atMs >= 480 && p.atMs < 1500);
        CHECK(g.fired());
        g.disarm();
        CHECK(p.calls == 1);
    }

    // G2. disarmed in time: nothing is called, and the disarm is prompt.
    {
        Probe p;
        ShutdownGuard g(options(60000, 60000, p));  // a minute and a minute: a disarm that waited would show
        g.arm();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto t = Clock::now();
        g.disarm();
        const double took = ms(t, Clock::now());
        std::printf("  G2: disarm took %.1f ms of a 120 s budget\n", took);
        CHECK(took < 500.0);
        CHECK(!g.armed());
        CHECK(!g.fired());
        CHECK(p.calls == 0);
    }
    {
        Probe p;
        ShutdownGuard g(options(300, 100, p));
        g.arm();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g.disarm();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));  // well past budget + grace
        CHECK(p.calls == 0 && !g.fired());
    }

    // G3. the destructor disarms.
    {
        Probe p;
        const auto t = Clock::now();
        {
            ShutdownGuard g(options(60000, 60000, p));
            g.arm();
        }
        CHECK(ms(t, Clock::now()) < 1000.0);
        CHECK(p.calls == 0);
    }

    // G4. idempotent; a disarmed guard is not armed again.
    {
        Probe p;
        ShutdownGuard g(options(400, 100, p));
        g.arm();
        g.arm();
        g.disarm();
        g.disarm();
        g.arm();  // after a disarm: nothing
        CHECK(!g.armed());
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        CHECK(p.calls == 0 && !g.fired());
    }

    // G5. no arming, no ending.
    {
        Probe p;
        {
            ShutdownGuard g(options(50, 50, p));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(p.calls == 0 && !g.fired());
        }
    }

    // S. where it is: engine_main.cpp.
    {
        fs::path root;
#ifdef CASCADE_SOURCE_DIR
        root = CASCADE_SOURCE_DIR;
#endif
        if (argc > 1) { root = argv[1]; }
        if (root.empty()) {
            root = fs::current_path();
            for (int i = 0; i < 8 && !fs::exists(root / "src" / "engine_main.cpp"); ++i) { root = root.parent_path(); }
        }
        const std::string main = readFile(root / "src" / "engine_main.cpp");
        const std::string hdr = readFile(root / "src" / "engine" / "shutdown_guard.hpp");
        CHECK(!main.empty() && !hdr.empty());
        const std::size_t run = main.find("int runEngine(");
        CHECK(run != std::string::npos);
        const std::string body = run == std::string::npos ? std::string() : main.substr(run);
        const std::size_t stops = countOf(body, "service.stop()");
        std::printf("  S: %zu service.stop() call(s) in runEngine, %zu guard(s) armed\n", stops, countOf(body, "bound.arm();"));
        CHECK(stops >= 3);  // the two failed-start stops and the one that ends the run
        CHECK(countOf(body, "ShutdownGuard bound(guardOptions(service, boot))") == stops);
        CHECK(countOf(body, "bound.arm();") == stops);
        // ...each arm() directly before its stop (nothing between that could take the time the budget is for).
        for (std::size_t at = body.find("bound.arm();"); at != std::string::npos; at = body.find("bound.arm();", at + 1)) {
            const std::size_t stop = body.find("service.stop()", at);
            CHECK(stop != std::string::npos);
            if (stop != std::string::npos) {
                const std::string between = body.substr(at + 12, stop - at - 12);
                CHECK(between.find_first_not_of(" \t\r\n") == std::string::npos);
            }
        }
        // The budget: the guard's default is the window teardown's constant; engine_main touches it only in guardOptions,
        // behind `--seconds`, from the seam.
        CHECK(hdr.find("unsigned budgetMs = cascade::core::HangWatchdog::kShutdownThresholdMs;") != std::string::npos);
        CHECK(countOf(main, "budgetMs") == 1);
        const std::size_t seam = main.find("FOXSDR_ENGINE_SHUTDOWN_BUDGET_MS\")");
        const std::size_t gate = main.rfind("if (boot.seconds >= 0.0) {", seam);
        CHECK(seam != std::string::npos && gate != std::string::npos && seam - gate < 200);
        // The report directory follows the stored switch exactly as the control thread's watchdog's does.
        CHECK(main.find("o.reportDir = service.reportDir();") != std::string::npos);
    }

    return testSummary("test_engine_shutdown_guard");
}
