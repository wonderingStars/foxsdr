// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// See shutdown_guard.hpp for what this is, why the headless engine needs it and what ending the process leaves behind.
#include "engine/shutdown_guard.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "core/breadcrumb.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cascade::engine {

namespace {

// One line on stderr by the platform's raw write: stdio's stderr lock may be held by the thread that is wedged.
void writeStderrRaw(const char* text, std::size_t size) {
#if defined(_WIN32)
    const HANDLE h = ::GetStdHandle(STD_ERROR_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) { return; }
    DWORD written = 0;
    (void)::WriteFile(h, text, static_cast<DWORD>(size), &written, nullptr);
#else
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(2, text + done, size - done);
        if (n <= 0) { break; }
        done += static_cast<std::size_t>(n);
    }
#endif
}

}  // namespace

ShutdownGuard::ShutdownGuard(Options options) : options_(std::move(options)) {}

ShutdownGuard::~ShutdownGuard() { disarm(); }

void ShutdownGuard::endProcess(int exitCode) {
#if defined(_WIN32)
    // Not ExitProcess: it runs every module's detach routine, and the wedge may be inside one of those.
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exitCode));
    for (;;) { ::Sleep(1000); }  // not reached: TerminateProcess on oneself does not return
#else
    // _exit runs no on_exit hook, so the status is written where the sentinel reads it first (a no-op with no watcher).
    cascade::core::breadcrumb::noteExit(cascade::core::breadcrumb::ExitHow::Exited, static_cast<std::uint32_t>(exitCode) & 0xFFu);
    ::_exit(exitCode);
#endif
}

void ShutdownGuard::arm() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (armed_ || disarmed_) { return; }
        armed_ = true;
    }
    // THE SENTINEL'S VIEW: from here the process is in its shutdown (a watcher that exists judges silence against the long
    // budget, as it does for the window's teardown).
    cascade::core::breadcrumb::setPhase(cascade::core::breadcrumb::Phase::ShutdownBegun);
    // THE WINDOW'S OWN PROTECTION, when a report may be written: the hang watchdog with the shutdown budget. Started with
    // the budget as its threshold (not the 5 s default raised a moment later) so no poll can judge the stop against the
    // frame threshold; beginShutdown() restarts its stall clock and logs the budget, as the window's teardown does.
    if (!options_.reportDir.empty()) {
        watchdog_.start(options_.reportDir, options_.budgetMs);
        watchdog_.beginShutdown(options_.budgetMs);
    }
    thread_ = std::thread(&ShutdownGuard::threadMain, this);
}

void ShutdownGuard::disarm() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        disarmed_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
    watchdog_.stop();
}

bool ShutdownGuard::armed() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return armed_ && !disarmed_;
}

bool ShutdownGuard::fired() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return fired_;
}

void ShutdownGuard::threadMain() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.budgetMs) +
                          std::chrono::milliseconds(options_.graceMs);
    std::unique_lock<std::mutex> lk(mutex_);
    // Woken early by disarm(); a spurious wake re-checks the clock, and only the budget ending (not the flag) fires.
    while (!disarmed_) {
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout && !disarmed_) { break; }
    }
    if (disarmed_) { return; }
    fired_ = true;
    lk.unlock();

    // THE BUDGET RAN OUT. One line, by the raw write; then the ending.
    char line[384];
    const int n = std::snprintf(
        line, sizeof(line),
        "foxsdr-engine: the shutdown did not finish within its budget of %u ms - ending the process with exit code %d. "
        "The clean-exit marker is written last of all, so unless the stop had reached it the next start counts this run "
        "as a crash\n",
        options_.budgetMs, options_.exitCode);
    if (n > 0) { writeStderrRaw(line, static_cast<std::size_t>(n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1)); }
    if (options_.ender) {
        options_.ender(options_.exitCode);
        return;
    }
    endProcess(options_.exitCode);
}

}  // namespace cascade::engine
