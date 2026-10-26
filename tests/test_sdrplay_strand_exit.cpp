// A stranded SDRplay Link must outlive the process's own static destruction:
// the 0.99.84 RSPduo crash report of 2026-10-23 (signature 929BF825C4993CB3).
//
// WHAT THE REPORT SHOWS. Windows 10, RSPduo at 4 MS/s, a Debug build. At
// 14:01:19 the reader declared the stream stalled and the radio released; the
// stream-health line written 29 s later says the service never stopped
// calling - 115296 blocks in that window, 111135 of them into a ring nobody
// was reading any more. At 14:01:48 the window closed: stop() ran its
// unreachable branch ("stopped without Uninit"), so the Link was STRANDED
// rather than freed, the device was "closed without ReleaseDevice", and main
// returned. The access violation is on the vendor's own thread, two frames of
// cascade.exe above sdrplay_api.dll, 107 s into the session - the moment the
// C runtime, running the executable's static destructors on the main thread
// while every other thread is still alive, destroyed the graveyard that was
// keeping the stranded Link alive. The service's thread, which nothing had
// told to stop, took its next block into the freed Link.
//
// THE RULE THIS FILE HOLDS: "for the life of the process" means past the
// static destructors too. ExitProcess ends the other threads only AFTER
// they have run, so anything a vendor thread may still be inside has to be
// a heap object that nothing, not even exit, ever deletes.
//
// HOW IT IS PROVED. The test runs itself as a child. The child opens a fake
// SDRplay whose service thread delivers a block every millisecond, starts the
// stream, then has the fake REFUSE stop()'s Uninit (sdrplay_api_
// ServiceNotResponding, the 0.97.1 shape): the Link is stranded, the service
// keeps calling, the source is destroyed, and the child returns from main
// with the fake's thread still running - exactly the field sequence. A
// namespace-scope object of this translation unit holds a weak_ptr to the
// stranded Link and is destroyed after the library's own statics (MSVC and
// GCC both destroy in reverse order of construction, and the executable's
// own objects are initialised before the static library's), so its
// destructor is the last word: an expired weak_ptr means the graveyard was
// destroyed, and the child ends with exit code 7. A child that faults on the
// vendor thread in the meantime ends with the fault's code instead. The
// parent requires 0.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/leak_on_purpose.hpp"
#include "sdrplay_fake_api.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

using cascade::source::SdrPlaySource;
namespace abi = cascade::source::sdrplay_abi;
using fakesdrplay::FakeSdrPlayApi;

namespace {

using Clock = std::chrono::steady_clock;

// The same stand-ins test_sdrplay_stall.cpp uses: a block a millisecond, and
// a service that gives up on a callback held longer than 100 ms.
constexpr unsigned int kBlock = 2016;
constexpr auto kPeriod = std::chrono::microseconds(1000);
constexpr auto kWedgeAfter = std::chrono::milliseconds(100);

constexpr int kChildFailedExit = 7;
constexpr unsigned kChildTimeoutMs = 60000;

bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// THE LAST WORD (see the file header). Armed by the child once the Link is
// stranded; its destructor runs after the library's statics have been
// destroyed, and says whether the Link survived them.
struct LastWord {
    std::weak_ptr<void> stranded;
    bool armed = false;

    ~LastWord() {
        if (!armed) { return; }
        if (stranded.expired()) {
            std::fputs("FAIL: the stranded Link was destroyed with the executable's statics while "
                       "the service thread was still calling it\n",
                       stderr);
            std::fflush(stderr);
            // No further destructors, no atexit: the verdict is the exit code.
            std::_Exit(kChildFailedExit);
        }
        std::fputs("child: the stranded Link outlived the executable's static destructors\n",
                   stdout);
        std::fflush(stdout);
    }
};

LastWord g_lastWord;

// --- the child --------------------------------------------------------------

int childMain() {
    cascade::core::DiagLog::instance().resetForTest();

    // THE SERVICE OUTLIVES main, as the real one does: the fake is never
    // destroyed, so its thread is still delivering blocks while the C runtime
    // runs the static destructors. (Its destructor would join that thread,
    // which is the one thing the field process never got to do.)
    FakeSdrPlayApi* fake = cascade::core::leakOnPurpose(new FakeSdrPlayApi());
    fake->addDevice("1706012347", abi::kRsp2);

    auto src = std::make_unique<SdrPlaySource>();
    src->setApiForTest(&fake->table);
    CHECK(src->open(""));
    CHECK(src->start());
    fake->startService(kBlock, kPeriod, kWedgeAfter);
    CHECK(waitFor([&] { return fake->serviceBlocks.load() > 20; }, 3000));

    // A REFUSED UNINIT HAS STOPPED NOTHING: the service keeps calling, and
    // stop() strands the Link for whatever is still inside it.
    const unsigned long long strandedBefore = SdrPlaySource::linksStranded();
    fake->uninitResult = abi::ServiceNotResponding;
    src->stop();
    CHECK(SdrPlaySource::linksStranded() == strandedBefore + 1);

    g_lastWord.stranded = SdrPlaySource::lastStrandedLinkForTest();
    CHECK(!g_lastWord.stranded.expired());

    // The source goes the way it goes at shutdown - closed with its session
    // orphaned - and the stranding alone must already be holding the Link.
    src.reset();
    CHECK(!g_lastWord.stranded.expired());

    // ...and the service is still calling into it as main returns.
    const unsigned long long blocksAtReset = fake->serviceBlocks.load();
    CHECK(waitFor([&] { return fake->serviceBlocks.load() > blocksAtReset + 20; }, 3000));

    g_lastWord.armed = true;
    return testSummary("test_sdrplay_strand_exit (child)");
}

// --- the parent -------------------------------------------------------------

std::string selfPath(const char* argv0) {
#if defined(_WIN32)
    char buf[MAX_PATH * 2];
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { return std::string(buf, static_cast<std::size_t>(n)); }
#endif
    return argv0 != nullptr ? std::string(argv0) : std::string();
}

int parentMain(const char* argv0) {
    const std::string self = selfPath(argv0);
    CHECK(!self.empty());

#if defined(_WIN32)
    // A faulting child must end with its exit code, not wait on a dialog: the
    // error mode is inherited.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::string cmd = "\"" + self + "\" --child";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0,
                                     nullptr, nullptr, &si, &pi);
    CHECK(ok);
    if (ok) {
        const DWORD waited = ::WaitForSingleObject(pi.hProcess, kChildTimeoutMs);
        CHECK(waited == WAIT_OBJECT_0);
        DWORD exitCode = 1;
        if (waited == WAIT_OBJECT_0) {
            ::GetExitCodeProcess(pi.hProcess, &exitCode);
        } else {
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 5000);
        }
        std::printf("child exit code 0x%08lX (%lu)\n", static_cast<unsigned long>(exitCode),
                    static_cast<unsigned long>(exitCode));
        CHECK(exitCode == 0);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
#else
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        ::execl(self.c_str(), self.c_str(), "--child", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    if (pid > 0) {
        int status = 0;
        const pid_t got = ::waitpid(pid, &status, 0);
        CHECK(got == pid);
        if (WIFEXITED(status)) {
            std::printf("child exit status %d\n", WEXITSTATUS(status));
            CHECK(WEXITSTATUS(status) == 0);
        } else if (WIFSIGNALED(status)) {
            std::printf("child ended by signal %d\n", WTERMSIG(status));
            CHECK(false);
        } else {
            CHECK(false);
        }
    }
#endif
    return testSummary("test_sdrplay_strand_exit");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--child") == 0) { return childMain(); }
    return parentMain(argc >= 1 ? argv[0] : nullptr);
}
