// The stop signals and how they compose with the exit recorder (engine extraction stage 7d): core/stop_signal.{hpp,cpp}
// against core/exit_record.{hpp,cpp}, the other owner of SIGTERM/SIGINT/SIGHUP on Linux.
//
// WHY THIS IS ITS OWN TEST. exit_record.cpp says of itself "UNVERIFIED: written on a machine that cannot run Linux", and
// the survey of stage 7 named handler composition the risk of 7d. The recorder installs a handler on a stop signal only
// when it is still at the default; the engine's stop path installs AFTER it (boot's step 14) and has to keep it. What is
// held here, each case in a child of its own (the installation is once per process), against a MAP_SHARED breadcrumb page
// the parent reads afterwards - the very page a sentinel reads:
//   S1. alone: a signal sets the request and ends nothing; the number is the signal's; the child goes on and exits 0.
//   S2. beneath the recorder, ONE signal: the request is set, the child is NOT ended, and the ordinary exit that follows
//       is what the recorder writes down (Exited 0) - the first signal leaves no "signalled" record, because the
//       process was not signalled to death.
//   S3. beneath the recorder, a SECOND signal: handed back to the recorder, which records Signalled(SIGTERM) and lets the
//       default end the child - the parent sees the child die of SIGTERM AND the record says so. (A recorder that was
//       displaced would leave the record empty: this is the case that says it is not.)
//   S4. a second signal with no recorder: the child dies of it (the default is what was beneath).
//   S5. a different second signal (SIGINT, then SIGTERM) escalates the same way.
//   S6. a signal that was IGNORED before the install stays ignored: nothing is requested, nothing ends.
//   S7. a handler somebody else had installed gets the second signal (and the child goes on): composition is not
//       limited to the recorder.
//
// POSIX ONLY. On Windows the stop path is the console handler of core/console_close.hpp (held by
// tests/test_engine_main_sigterm.cpp with a real Ctrl+Break), and this prints that it did not run. UNVERIFIED on Windows by
// construction; run under WSL2 by hand for stage 7d (the record says what it showed); the Linux jobs of stage 7g are where
// it runs in CI.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "test_check.hpp"

#if defined(_WIN32)

#include "core/stop_signal.hpp"

int main() {
    std::printf("test_stop_signal\n");
    // The one thing checkable without a signal: nothing is requested before anything happens, and installing works.
    CHECK(!cascade::core::stopSignalRequested());
    CHECK(cascade::core::stopSignalNumber() == 0);
    CHECK(std::string(cascade::core::stopSignalName()).empty());
    CHECK(cascade::core::installStopSignals());
    CHECK(cascade::core::installStopSignals());  // idempotent
    CHECK(!cascade::core::stopSignalRequested());
    ++g_checksSkipped;
    std::printf("SKIP (windows): the POSIX signal composition cases S1-S7 (SIGTERM/SIGINT/SIGHUP, the exit recorder) - "
                "run them on Linux\n");
    return testSummary("test_stop_signal");
}

#else

#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <string>

#include "core/breadcrumb.hpp"
#include "core/exit_record.hpp"
#include "core/stop_signal.hpp"

namespace {

namespace bc = cascade::core::breadcrumb;

struct Outcome {
    bool exited = false;
    int status = -1;     // the exit status, when exited
    bool signalled = false;
    int signal = 0;      // the signal that ended it, when signalled
    bc::Snapshot record;  // what the child wrote into the shared page
};

// Run `body` in a forked child that has the shared breadcrumb page attached; wait for it (bounded) and report how it ended
// and what the page says. `body` returns the child's exit status, or never returns (a signal ends it).
Outcome runChild(const std::function<int(volatile int* progress)>& body) {
    void* mem = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    Outcome out;
    if (mem == MAP_FAILED) { return out; }
    auto* page = static_cast<bc::Block*>(mem);
    auto* progress = reinterpret_cast<volatile int*>(static_cast<char*>(mem) + 1024);
    *progress = 0;
    std::fflush(stdout);
    const pid_t pid = ::fork();
    if (pid == 0) {
        bc::attach(page);
        const int code = body(progress);
        std::exit(code);  // a return from main: the on_exit hook runs, as it does in the engine
    }
    int status = 0;
    bool done = false;
    for (int i = 0; i < 1000 && !done; ++i) {  // up to ten seconds
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) { done = true; break; }
        ::usleep(10000);
    }
    if (!done) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    } else {
        out.exited = WIFEXITED(status);
        out.status = out.exited ? WEXITSTATUS(status) : -1;
        out.signalled = WIFSIGNALED(status);
        out.signal = out.signalled ? WTERMSIG(status) : 0;
    }
    out.record = bc::read(page);
    ::munmap(mem, 4096);
    return out;
}

std::atomic<int> g_custom{0};
void customHandler(int) { g_custom.fetch_add(1); }

}  // namespace

int main() {
    std::printf("test_stop_signal\n");

    // S1. alone: the request is set, nothing ends, the child exits 0 by itself.
    {
        const Outcome o = runChild([](volatile int*) {
            if (!cascade::core::installStopSignals()) { return 10; }
            if (cascade::core::stopSignalRequested()) { return 11; }
            ::raise(SIGTERM);
            if (!cascade::core::stopSignalRequested()) { return 12; }
            if (cascade::core::stopSignalNumber() != SIGTERM) { return 13; }
            if (std::string(cascade::core::stopSignalName()) != "SIGTERM") { return 14; }
            return 0;
        });
        std::printf("  S1: exited %d status %d, signalled %d (%d)\n", o.exited, o.status, o.signalled, o.signal);
        CHECK(o.exited && o.status == 0);
    }
    // ...and each of the three signals names itself.
    for (const auto& [sig, name] : {std::pair<int, const char*>{SIGINT, "SIGINT"}, std::pair<int, const char*>{SIGHUP, "SIGHUP"}}) {
        const Outcome o = runChild([sig = sig, name = name](volatile int*) {
            if (!cascade::core::installStopSignals()) { return 10; }
            ::raise(sig);
            if (cascade::core::stopSignalNumber() != sig) { return 13; }
            if (std::string(cascade::core::stopSignalName()) != name) { return 14; }
            return 0;
        });
        CHECK(o.exited && o.status == 0);
    }

    // S2. beneath the recorder, one signal: requested, not ended; the exit that follows is what is recorded.
    {
        const Outcome o = runChild([](volatile int*) {
            cascade::core::installExitRecording();   // boot's step 13
            if (!cascade::core::installStopSignals()) { return 10; }  // boot's step 14
            ::raise(SIGTERM);
            return cascade::core::stopSignalRequested() ? 0 : 12;
        });
        std::printf("  S2: exited %d status %d; record how=%u value=%u\n", o.exited, o.status,
                    static_cast<unsigned>(o.record.exitHow), o.record.exitValue);
        CHECK(o.exited && o.status == 0);
        CHECK(o.record.valid);
#if defined(__GLIBC__)
        CHECK(o.record.exitHow == bc::ExitHow::Exited && o.record.exitValue == 0);  // the on_exit hook: glibc only
#endif
    }

    // S3. beneath the recorder, a second signal: handed to the recorder, which records it and lets the default end the child.
    {
        const Outcome o = runChild([](volatile int* progress) {
            cascade::core::installExitRecording();
            if (!cascade::core::installStopSignals()) { return 10; }
            ::raise(SIGTERM);
            *progress = 1;  // the first signal did not end it
            ::raise(SIGTERM);
            *progress = 2;  // not reached
            return 99;
        });
        std::printf("  S3: signalled %d (%d); record how=%u value=%u\n", o.signalled, o.signal,
                    static_cast<unsigned>(o.record.exitHow), o.record.exitValue);
        CHECK(o.signalled && o.signal == SIGTERM);
        CHECK(o.record.valid);
        CHECK(o.record.exitHow == bc::ExitHow::Signalled && o.record.exitValue == static_cast<std::uint32_t>(SIGTERM));
    }

    // S4. a second signal with no recorder beneath: the default ends the child.
    {
        const Outcome o = runChild([](volatile int*) {
            if (!cascade::core::installStopSignals()) { return 10; }
            ::raise(SIGTERM);
            ::raise(SIGTERM);
            return 99;
        });
        CHECK(o.signalled && o.signal == SIGTERM);
    }

    // S5. a different second signal escalates the same way: SIGINT asked, then SIGTERM.
    {
        const Outcome o = runChild([](volatile int*) {
            cascade::core::installExitRecording();
            if (!cascade::core::installStopSignals()) { return 10; }
            ::raise(SIGINT);
            ::raise(SIGTERM);
            return 99;
        });
        CHECK(o.signalled && o.signal == SIGTERM);
        CHECK(o.record.exitHow == bc::ExitHow::Signalled && o.record.exitValue == static_cast<std::uint32_t>(SIGTERM));
    }

    // S6. a signal ignored before the install stays ignored.
    {
        const Outcome o = runChild([](volatile int*) {
            ::signal(SIGHUP, SIG_IGN);
            if (!cascade::core::installStopSignals()) { return 10; }
            ::raise(SIGHUP);
            ::raise(SIGHUP);
            if (cascade::core::stopSignalRequested()) { return 12; }  // nothing was asked
            return 0;
        });
        CHECK(o.exited && o.status == 0);
    }

    // S7. somebody else's handler gets the second signal.
    {
        const Outcome o = runChild([](volatile int*) {
            struct sigaction sa {};
            sa.sa_handler = &customHandler;
            ::sigemptyset(&sa.sa_mask);
            ::sigaction(SIGTERM, &sa, nullptr);
            if (!cascade::core::installStopSignals()) { return 10; }
            ::raise(SIGTERM);                                         // the first: ours, a request
            if (g_custom.load() != 0) { return 11; }
            ::raise(SIGTERM);                                         // the second: handed back to the custom handler
            if (g_custom.load() != 1) { return 12; }
            return cascade::core::stopSignalRequested() ? 0 : 13;     // and the child is still here
        });
        CHECK(o.exited && o.status == 0);
    }

    return testSummary("test_stop_signal");
}

#endif
