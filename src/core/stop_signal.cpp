// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// See stop_signal.hpp for what this is and how it composes with the exit recorder.
//
// UNVERIFIED AS A RUNNING ENGINE ON LINUX: on a Windows build the POSIX half below is dead text (guarded out). It has
// been compiled and run as a stand-alone harness under WSL2 (tests/test_stop_signal.cpp, built by hand with g++; the
// stage's record says what it showed); it has never run inside foxsdr-engine on Linux or on arm64 - the Linux jobs of
// stage 7g are where it first does.
#include "core/stop_signal.hpp"

#include <atomic>

#if defined(_WIN32)

#include "core/console_close.hpp"

namespace cascade::core {

bool installStopSignals() { return installConsoleCloseHandler(); }

bool stopSignalRequested() { return consoleCloseRequested(); }

int stopSignalNumber() { return 0; }

const char* stopSignalName() { return consoleCloseRequested() ? "a console event" : ""; }

}  // namespace cascade::core

#else

#include <errno.h>
#include <signal.h>

namespace cascade::core {

namespace {

constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};
constexpr int kCount = static_cast<int>(sizeof(kSignals) / sizeof(kSignals[0]));

static_assert(std::atomic<int>::is_always_lock_free, "the handler's flag must be lock-free to be async-signal-safe");

std::atomic<bool> g_installed{false};
// The signal that asked first; 0 until one has. The handler's only write on the first signal.
std::atomic<int> g_first{0};
// What each signal's disposition was before this installed (the exit recorder's handler, the default, ...). Written
// before the handler exists, read only by it.
struct sigaction g_previous[kCount];

int indexOf(int sig) {
    for (int i = 0; i < kCount; ++i) {
        if (kSignals[i] == sig) { return i; }
    }
    return -1;
}

// Async-signal-safe: an atomic store, sigaction and raise, and errno put back.
void onStopSignal(int sig, siginfo_t* /*info*/, void* /*context*/) {
    const int savedErrno = errno;
    int none = 0;
    if (!g_first.compare_exchange_strong(none, sig, std::memory_order_acq_rel)) {
        // NOT THE FIRST: whoever asked twice means it. Hand the signal back to what was there before this handler (the
        // recorder's, the default, anybody's): put that disposition back and raise the signal again. It is blocked
        // while this runs, so it is delivered when the handler returns - to the previous disposition.
        const int i = indexOf(sig);
        if (i >= 0) { ::sigaction(sig, &g_previous[i], nullptr); }
        ::raise(sig);
    }
    errno = savedErrno;
}

}  // namespace

bool installStopSignals() {
    if (g_installed.exchange(true)) { return true; }
    bool ok = true;
    for (int i = 0; i < kCount; ++i) {
        const int sig = kSignals[i];
        struct sigaction current {};
        if (::sigaction(sig, nullptr, &current) != 0) {
            ok = false;
            continue;
        }
        // A signal somebody chose to ignore stays ignored (nohup, a background job).
        if ((current.sa_flags & SA_SIGINFO) == 0 && current.sa_handler == SIG_IGN) {
            g_previous[i] = current;
            continue;
        }
        struct sigaction sa {};
        sa.sa_sigaction = &onStopSignal;
        ::sigemptyset(&sa.sa_mask);
        for (const int other : kSignals) { ::sigaddset(&sa.sa_mask, other); }
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        // The previous disposition is saved by the call that replaces it, before any signal can reach the new one.
        if (::sigaction(sig, &sa, &g_previous[i]) != 0) { ok = false; }
    }
    return ok;
}

bool stopSignalRequested() { return g_first.load(std::memory_order_acquire) != 0; }

int stopSignalNumber() { return g_first.load(std::memory_order_acquire); }

const char* stopSignalName() {
    switch (g_first.load(std::memory_order_acquire)) {
        case SIGTERM: return "SIGTERM";
        case SIGINT: return "SIGINT";
        case SIGHUP: return "SIGHUP";
        default: return "";
    }
}

}  // namespace cascade::core

#endif
