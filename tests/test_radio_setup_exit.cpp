// The radio-setup probe must not find its tables destroyed under it at exit:
// the 0.99.88 access violation, signature 74EEC22DE4163D64 (2026-10-24).
//
// WHAT WAS SEEN. tests/test_sentinel_app.cpp's clean scripted run
// (`cascade.exe --frames 3`) failed about one run in three, on a branch and on
// the released 0.99.88 build alike: the in-process handler wrote an access
// violation at cascade.exe+0x2C5F5B, uptime 0 s, right after "frame loop ended
// after 3 frames; shutting down". Symbolised against the build's PDB, the
// faulting thread was the radio-setup probe's own detached worker:
//
//   radio_setup.cpp   Probe::start's thread -> collectInventory()
//   radio_setup.cpp   enumerateRtlSdr()
//   rtlsdr_source.cpp usb::enumerateWinUsb(rtlSdrUsbIds())
//   winusb_device.cpp ids[matched].vid
//
// THE CAUSE. The worker is detached on purpose - a probe wedged inside
// SetupAPI must never hold up the window - so it can still be scanning when
// main returns. The C runtime then runs the static destructors on the main
// thread while every other thread is still alive, and two tables the worker
// reads were function-local statics with destructors: the RTL-SDR id list
// (source::rtlSdrUsbIds) and the check's own known-radio table
// (radiosetup::knownDevices). Destroyed, a vector's pointers are null; the
// worker's next ids[matched] read address zero plus an offset.
//
// THE RULE THIS FILE HOLDS, the same one test_sdrplay_strand_exit.cpp holds
// for the SDRplay Link: anything a thread that may outlive main reads must be
// something not even exit destroys.
//
// HOW IT IS PROVED. The test runs itself as a child. The child reads both
// tables (so they are constructed, as the real worker constructs them), starts
// the REAL Probe with a collector that walks both tables without pause for as
// long as the process lives - the field worker's reads, minus the bus - waits
// until the worker is in its loop, and returns from main. Two verdicts:
//   - a namespace-scope object of this translation unit is constructed before
//     main, so it is destroyed AFTER the library's function-local statics; its
//     destructor checks that both tables still hold what they held in main,
//     and ends the child with exit code 7 if not. Deterministic: no race.
//   - a worker that faults on a destroyed table ends the child with the
//     fault's own code first.
// The parent requires exit code 0.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "core/radio_setup.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"

namespace rs = cascade::core::radiosetup;

namespace {

constexpr int kChildFailedExit = 7;
constexpr unsigned kChildTimeoutMs = 60000;

// What the tables held in main, for the last word to compare against.
struct TableFacts {
    std::size_t idCount = 0;
    std::uint16_t firstIdVid = 0;
    std::uint16_t firstIdPid = 0;
    std::size_t knownCount = 0;
    std::uint16_t firstKnownVid = 0;
};

TableFacts tableFactsNow() {
    TableFacts t;
    const std::vector<cascade::usb::UsbId>& ids = cascade::source::rtlSdrUsbIds();
    t.idCount = ids.size();
    if (!ids.empty()) {
        t.firstIdVid = ids.front().vid;
        t.firstIdPid = ids.front().pid;
    }
    const std::vector<rs::KnownDevice>& known = rs::knownDevices();
    t.knownCount = known.size();
    if (!known.empty()) { t.firstKnownVid = known.front().vid; }
    return t;
}

// THE LAST WORD (see the file header). Armed by the child once the worker is
// running; its destructor runs after the library's function-local statics have
// been destroyed, and says whether the two tables survived them.
struct LastWord {
    TableFacts inMain;
    bool armed = false;

    ~LastWord() {
        if (!armed) { return; }
        const TableFacts now = tableFactsNow();
        const bool idsIntact = now.idCount == inMain.idCount && now.idCount > 0 &&
                               now.firstIdVid == inMain.firstIdVid &&
                               now.firstIdPid == inMain.firstIdPid;
        const bool knownIntact = now.knownCount == inMain.knownCount && now.knownCount > 0 &&
                                 now.firstKnownVid == inMain.firstKnownVid;
        if (!idsIntact || !knownIntact) {
            std::fprintf(stderr,
                         "FAIL: destroyed with the statics while the probe was still reading "
                         "them:%s%s (ids %zu -> %zu, known %zu -> %zu)\n",
                         idsIntact ? "" : " the RTL-SDR id table",
                         knownIntact ? "" : " the known-radio table", inMain.idCount, now.idCount,
                         inMain.knownCount, now.knownCount);
            std::fflush(stderr);
            // No further destructors, no atexit: the verdict is the exit code.
            std::_Exit(kChildFailedExit);
        }
        std::fputs("child: both tables outlived the executable's static destructors\n", stdout);
        std::fflush(stdout);
    }
};

LastWord g_lastWord;

// The worker's loop, and proof that it is in it.
std::atomic<unsigned long long> g_passes{0};

// --- the child --------------------------------------------------------------

int childMain() {
    // Constructed here, on the main thread, before the worker exists - as in
    // the application, where the Source list and the check both reach them.
    const TableFacts facts = tableFactsNow();
    CHECK(facts.idCount > 0);
    CHECK(facts.knownCount > facts.idCount);  // every RTL id is a known radio, and more

    // THE REAL PROBE, DETACHED AS IN THE APPLICATION. Its collector walks both
    // tables exactly as the bus walk and enumerateWinUsb do - an index taken
    // from the size, then an element read - for as long as the process lives.
    rs::Probe* probe = new rs::Probe();  // never deleted: its thread outlives main
    CHECK(probe->start([]() {
        volatile unsigned sink = 0;
        for (;;) {
            const std::vector<cascade::usb::UsbId>& ids = cascade::source::rtlSdrUsbIds();
            const std::size_t n = ids.size();
            for (std::size_t i = 0; i < n; ++i) { sink = sink + ids[i].vid + ids[i].pid; }
            for (const cascade::usb::UsbId& id : ids) {
                if (rs::findKnownDevice(id.vid, id.pid) != nullptr) { sink = sink + 1; }
            }
            g_passes.fetch_add(1, std::memory_order_relaxed);
        }
        return rs::Inventory{};
    }));
    for (int i = 0; i < 3000 && g_passes.load() < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(g_passes.load() >= 100);
    std::printf("child: probe worker in its loop (%llu passes); returning from main\n",
                g_passes.load());
    std::fflush(stdout);

    g_lastWord.inMain = facts;
    g_lastWord.armed = true;
    return testSummary("test_radio_setup_exit (child)");
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
    return testSummary("test_radio_setup_exit");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--child") == 0) { return childMain(); }
    return parentMain(argc >= 1 ? argv[0] : nullptr);
}
