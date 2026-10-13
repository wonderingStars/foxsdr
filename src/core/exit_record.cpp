// See exit_record.hpp for what this is for and what it records.
//
// UNVERIFIED: written on a machine that cannot run Linux. Compiled for syntax only, against
// the real glibc and Linux headers' declarations (docs/DIAGNOSTICS.md, "The sentinel", lists
// what was and was not checked).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/exit_record.hpp"

#if defined(__linux__)

#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "core/breadcrumb.hpp"

namespace cascade::core {

namespace {

std::atomic<bool> g_installed{false};
// The process that installed the hooks. A fork() child that has not yet exec'd shares the page
// (it is a MAP_SHARED memfd) and inherits the hooks: it must not write the application's record.
std::atomic<pid_t> g_ownerPid{0};

bool isOwner() { return ::getpid() == g_ownerPid.load(std::memory_order_relaxed); }

#if defined(__GLIBC__)
// Run by exit() with the status exit() was given, and by a return from main (which is exit()).
// The status the process ends with is the low eight bits.
void onExitStatus(int status, void* /*arg*/) {
    if (isOwner()) {
        breadcrumb::noteExit(breadcrumb::ExitHow::Exited, static_cast<std::uint32_t>(status) & 0xFFu);
    }
}
#endif

// A signal whose default action ends the process. Records it, and puts the default back
// (SA_RESETHAND did so on entry) and raises it again: the signal is blocked while this runs,
// so it is held until the handler returns and is then delivered to the default disposition,
// which ends the process with the same status the signal would have ended it with. Only
// async-signal-safe calls: getpid(), a relaxed store, and raise().
void onEndingSignal(int sig) {
    if (isOwner()) { breadcrumb::noteExit(breadcrumb::ExitHow::Signalled, static_cast<std::uint32_t>(sig)); }
    ::raise(sig);
}

}  // namespace

void installExitRecording() {
    if (g_installed.exchange(true)) { return; }
    g_ownerPid.store(::getpid(), std::memory_order_relaxed);
#if defined(__GLIBC__)
    ::on_exit(&onExitStatus, nullptr);
#endif
    for (const int sig : {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE}) {
        struct sigaction current {};
        if (::sigaction(sig, nullptr, &current) != 0) { continue; }
        // Only a signal that would end the process: one that was ignored, or that something
        // else handles, stays as it is.
        if ((current.sa_flags & SA_SIGINFO) != 0 || current.sa_handler != SIG_DFL) { continue; }
        struct sigaction sa {};
        sa.sa_handler = &onEndingSignal;
        ::sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESETHAND;
        ::sigaction(sig, &sa, nullptr);
    }
}

}  // namespace cascade::core

#else

namespace cascade::core {
void installExitRecording() {}
}  // namespace cascade::core

#endif
