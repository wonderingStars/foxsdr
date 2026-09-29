// test_command_queue_threads.cpp - the engine's command queue is the boundary
// between a front end's thread and the control thread (stage 3b,
// docs/engine-stage3.md section 10): submitCommand may be called from any
// thread while drainLocalCommands runs on another, and no command is lost,
// doubled or torn.
//
// In 3a both ran on the GUI thread and the queue was a plain vector; a submit
// from any other thread was a data race on it (and on sourceGen_, which the
// submit stamps each command with and the control side moves).
//   A  four threads submit 5000 commands each at once, nothing draining: all
//      20000 are queued
//   B  the same while a fifth thread drains without pause: every one of the
//      20000 is taken exactly once, and applied (the last volume each thread
//      asked for is the one a final read finds, when every thread's last
//      command is the same value)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

using cascade::engine::Engine;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    const std::string tmp = std::string(buf) + "foxsdr_cmd_queue_" + std::to_string(pid);
#else
    const int pid = static_cast<int>(getpid());
    const std::string tmp = "/tmp/foxsdr_cmd_queue_" + std::to_string(pid);
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

constexpr int kThreads = 4;
constexpr int kEach = 5000;

void submitAll(Engine& e, int thread) {
    for (int i = 0; i < kEach; ++i) {
        // The volume a thread asks for last is 0.5 for every thread, so the
        // final value does not depend on how the threads interleave.
        const double v = i == kEach - 1 ? 0.5 : 0.01 * static_cast<double>((thread * kEach + i) % 90 + 1);
        e.submitCommand(cascade::core::cmd::makeNum(FOXAPI_OP_SET_VOLUME, v));
    }
}

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static std::size_t queued(Engine& e) { return e.localCommandsQueuedForTest(); }
    static std::uint64_t taken(Engine& e) { return e.localCommandsTaken_.load(); }
    static float volume(Engine& e) { return e.volume_; }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_command_queue_threads\n");
    isolate();

    Engine e;
    e.initialise();

    // A: submits only.
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) { ts.emplace_back([&e, t] { submitAll(e, t); }); }
        for (auto& t : ts) { t.join(); }
        std::printf("A: %d threads x %d submits, nothing draining: %zu queued\n", kThreads, kEach, A::queued(e));
        CHECK(A::queued(e) == static_cast<std::size_t>(kThreads * kEach));
        const std::uint64_t before = A::taken(e);
        e.drainLocalCommands();
        CHECK(A::queued(e) == 0u);
        CHECK(A::taken(e) - before == static_cast<std::uint64_t>(kThreads * kEach));
    }

    // B: submits against a drain that never stops.
    {
        const std::uint64_t before = A::taken(e);
        std::atomic<bool> stop{false};
        std::thread drainer([&] {
            while (!stop.load()) { e.drainLocalCommands(); }
        });
        std::vector<std::thread> ts;
        for (int t = 0; t < kThreads; ++t) { ts.emplace_back([&e, t] { submitAll(e, t); }); }
        for (auto& t : ts) { t.join(); }
        stop.store(true);
        drainer.join();
        e.drainLocalCommands();
        const std::uint64_t taken = A::taken(e) - before;
        std::printf("B: %d threads x %d submits against a running drain: %llu taken, %zu left, volume %.2f\n",
                    kThreads, kEach, static_cast<unsigned long long>(taken), A::queued(e),
                    static_cast<double>(A::volume(e)));
        CHECK(taken == static_cast<std::uint64_t>(kThreads * kEach));
        CHECK(A::queued(e) == 0u);
        CHECK(A::volume(e) == 0.5f);
    }

    e.teardown();
    return testSummary("test_command_queue_threads");
}
