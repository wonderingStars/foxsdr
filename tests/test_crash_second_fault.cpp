// A SECOND FAULT WHILE THE CRASH HANDLER IS RUNNING must not throw away the
// first fault's report - and the handler must not need anything a faulting
// process may no longer be able to give it.
//
// WHAT THIS IS FOR. Field report F204602B5329B268 (0.99.35, Windows
// 10.0.28000): the SoapySDR enumeration child probing driver=uhd died with
// 0xE0000002 - the code the handler's re-entry guard kills the process with
// when a handler is entered while one is already running. The child had its
// crash capture armed (exitAfterReport), so a single fault would have left a
// report and died with the fault's own code; 0xE0000002 means the handler was
// entered TWICE, and the parent could say nothing about what the child died of.
//
// UHD's discovery runs every device family's find function on a thread of its
// own at once, and the libusb fault this product already knows about corrupts
// memory that every one of those threads then touches. So the two ways a
// second entry happens, and both are staged here in a child process of this
// binary with the enumeration child's own handler configuration:
//
//   1. ANOTHER THREAD FAULTS while the first report is being written. Before
//      this fix the second thread TerminateProcess'd with 0xE0000002 at once,
//      killing the first thread mid-report: the report was truncated or never
//      created and the exit code named nothing.
//   2. THE HANDLER ITSELF CANNOT PROCEED because of state the fault left
//      behind. Creating the report file through CreateFileA converts the path
//      on the PROCESS HEAP (RtlDosPathNameToRelativeNtPathName allocates), so
//      a thread that dies holding the heap lock - or a heap the vendor fault
//      corrupted - stalls or faults the handler itself. Staged by holding the
//      heap lock on another thread and by trashing freed heap blocks before
//      faulting.
//
// Plus the two cases the owner asked about that turned out NOT to double
// fault, kept so they stay that way: the faulting thread holding the heap lock
// itself (the lock is recursive), and a C++ exception thrown out of a detached
// thread (the abort net reports it once).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "source/soapy_enum_proc.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

enum Mode {
    kConcurrent = 1,     // a second thread faults while the first report is written
    kHeapLockOther = 2,  // another thread holds the process heap lock
    kHeapLockSelf = 3,   // the faulting thread holds the process heap lock
    kDetachedThrow = 4,  // a C++ exception escapes a detached thread
    kHeapTrashed = 5,    // freed heap blocks overwritten, then a fault
    kHeldSameThread = 6, // the fault path is held by THIS thread: a fault inside the handler
    kHeldOtherThread = 7,  // held by another thread that never finishes
    kNoisyStdout = 8,    // another thread writing to stdout the whole time
};

const char* modeName(int m) {
    switch (m) {
        case kConcurrent: return "concurrent";
        case kHeapLockOther: return "heaplock-other";
        case kHeapLockSelf: return "heaplock-self";
        case kDetachedThrow: return "detached-throw";
        case kHeapTrashed: return "heap-trashed";
        case kHeldSameThread: return "held-same-thread";
        case kHeldOtherThread: return "held-other-thread";
        case kNoisyStdout: return "noisy-stdout";
        default: return "?";
    }
}

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path("/tmp");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return base / (std::string("cascade-secondfault-") + tag + "-" + std::to_string(pid));
}

std::string selfExePath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return std::string(buf, buf + n);
#else
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return (n > 0) ? std::string(buf, buf + n) : std::string();
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<fs::path> reportsIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().filename().string().rfind("crash-", 0) == 0) {
            out.push_back(e.path());
        }
    }
    return out;
}

// A null store the optimiser cannot fold away. Out of line so each thread
// faults in a frame of its own. The ADDRESS is read through a volatile, not
// written as a constant: GCC at -O3 proved a store through a constant null
// pointer undefined and removed it, and the Linux children returned 7 ("the
// fault did not happen") instead of dying.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void faultNow() {
    volatile std::uintptr_t address = 0;
    volatile int* p = reinterpret_cast<volatile int*>(static_cast<std::uintptr_t>(address));
    *p = 1;
}

// --- the child -------------------------------------------------------------

int secondFaultChild(int mode, const std::string& dir) {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    DiagLog::instance().configure(std::string(), false);
    for (int i = 0; i < 400; ++i) { DiagLog::instance().writef("info", "step %d", i); }
    DiagLog::instance().write("warn", "about to fault on purpose");
    refreshModuleTable();

    // EXACTLY THE ENUMERATION CHILD'S CONFIGURATION
    // (source/soapy_enum_proc.cpp, armEnumerateHelperProcess): a report, no
    // minidump, and a fast death with the fault's own code.
    CrashHandlerConfig cfg;
    cfg.crashDir = dir;
    cfg.enabled = true;
    cfg.minidump = false;
    cfg.exitAfterReport = true;
    cfg.faultLineToStdout = true;
    installCrashHandlers(cfg);

    switch (mode) {
        case kNoisyStdout: {
            // ANOTHER THREAD WRITING TO THE SAME PIPE the whole time - a
            // vendor module's printf, in the field - in small raw writes
            // with no newline, so anything of it that lands inside the
            // handler's line is carried into the parent's reason. '#' is a
            // byte the line itself never contains.
            std::atomic<bool> going{false};
            std::thread noisy([&going] {
                static const char kNoise[] = "########";
                going.store(true);
                for (;;) {
#if defined(_WIN32)
                    DWORD w = 0;
                    ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), kNoise, 8, &w, nullptr);
#else
                    (void)!::write(STDOUT_FILENO, kNoise, 8);
#endif
                }
            });
            noisy.detach();
            while (!going.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            faultNow();
            break;
        }
        case kHeldSameThread: {
            holdFaultPathForTest();
            faultNow();
            break;
        }
        case kHeldOtherThread: {
            std::atomic<bool> held{false};
            std::thread holder([&held] {
                holdFaultPathForTest();
                held.store(true);
                std::this_thread::sleep_for(std::chrono::hours(1));
            });
            holder.detach();
            while (!held.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            faultNow();
            break;
        }
        case kConcurrent: {
            // The second thread faults the moment the first report FILE
            // exists - that is, while the first handler is still writing it.
            std::thread second([dir] {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (std::chrono::steady_clock::now() < until) {
                    if (!reportsIn(dir).empty()) { faultNow(); }
                }
            });
            second.detach();
            faultNow();
            break;
        }
        case kHeapLockOther: {
#if defined(_WIN32)
            std::atomic<bool> held{false};
            std::thread holder([&held] {
                ::HeapLock(::GetProcessHeap());
                held.store(true);
                ::Sleep(INFINITE);
            });
            holder.detach();
            while (!held.load()) { ::Sleep(1); }
            faultNow();
#endif
            break;
        }
        case kHeapLockSelf: {
#if defined(_WIN32)
            ::HeapLock(::GetProcessHeap());
            faultNow();
#endif
            break;
        }
        case kDetachedThrow: {
            std::thread([] { throw std::runtime_error("deliberate escape from a detached thread"); })
                .detach();
            std::this_thread::sleep_for(std::chrono::seconds(20));
            break;
        }
        case kHeapTrashed: {
#if defined(_WIN32)
            // USE AFTER FREE ACROSS THE SIZE CLASSES a path conversion could
            // land in: allocate, free, then overwrite what the heap keeps in
            // its free blocks. Nothing in THIS thread touches the heap again
            // before the fault, so the first allocation after it is the
            // handler's own.
            const HANDLE heap = ::GetProcessHeap();
            std::vector<void*> blocks;
            for (SIZE_T sz = 32; sz <= 4096; sz += 16) {
                for (int k = 0; k < 4; ++k) { blocks.push_back(::HeapAlloc(heap, 0, sz)); }
            }
            for (void* b : blocks) { ::HeapFree(heap, 0, b); }
            SIZE_T sz = 32;
            int k = 0;
            for (void* b : blocks) {
                if (b != nullptr) { std::memset(b, 0x41, sz); }
                if (++k == 4) {
                    k = 0;
                    sz += 16;
                }
            }
            faultNow();
#endif
            break;
        }
        default: break;
    }
    return 7;  // a fault that did not happen
}

// --- the parent ------------------------------------------------------------

struct Outcome {
    unsigned long exitCode = 0;
    bool timedOut = false;
    std::vector<std::string> reports;
    std::string stdoutText;  // the child's fault line(s), as its parent reads them
};

Outcome runChild(int mode, const fs::path& dir) {
    Outcome out;
#if defined(_WIN32)
    std::string cmd = "\"" + selfExePath() + "\" --second-fault " + std::to_string(mode) + " \"" +
                      dir.string() + "\"";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    ::CreatePipe(&rd, &wr, &sa, 0);
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = wr;
    si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started = ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(wr);
    if (started == 0) {
        ::CloseHandle(rd);
        out.exitCode = 0xFFFFFFFFul;
        return out;
    }
    std::string text;
    std::thread drain([rd, &text] {
        char buf[1024];
        DWORD got = 0;
        while (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) != 0 && got > 0) {
            text.append(buf, got);
        }
    });
    if (::WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) {
        out.timedOut = true;
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    drain.join();
    ::CloseHandle(rd);
    out.stdoutText = text;
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    out.exitCode = code;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        out.exitCode = 0xFFFFFFFFul;
        return out;
    }
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        const std::string exe = selfExePath();
        const std::string modeStr = std::to_string(mode);
        const std::string dirStr = dir.string();
        std::vector<char*> argv = {const_cast<char*>(exe.c_str()),
                                   const_cast<char*>("--second-fault"),
                                   const_cast<char*>(modeStr.c_str()),
                                   const_cast<char*>(dirStr.c_str()), nullptr};
        ::execv(exe.c_str(), argv.data());
        ::_exit(0x7F);
    }
    ::close(fds[1]);
    std::string text;
    std::thread drain([rd = fds[0], &text] {
        char buf[1024];
        for (;;) {
            const ssize_t got = ::read(rd, buf, sizeof(buf));
            if (got <= 0) { break; }
            text.append(buf, static_cast<std::size_t>(got));
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    for (;;) {
        if (::waitpid(pid, &status, WNOHANG) == pid) { break; }
        if (std::chrono::steady_clock::now() >= deadline) {
            out.timedOut = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (WIFEXITED(status)) { out.exitCode = static_cast<unsigned long>(WEXITSTATUS(status)); }
    if (WIFSIGNALED(status)) { out.exitCode = 128ul + static_cast<unsigned long>(WTERMSIG(status)); }
    drain.join();
    ::close(fds[0]);
    out.stdoutText = text;
#endif
    for (const fs::path& p : reportsIn(dir)) { out.reports.push_back(readFile(p)); }
    return out;
}

// A report that got all the way to its last section: the ring copy is the
// last thing writeReport writes, and "about to fault on purpose" is the last
// line the child logged.
bool complete(const std::string& text) {
    return text.find("kind: crash") != std::string::npos &&
           text.find("--- modules ---") != std::string::npos &&
           text.find("about to fault on purpose") != std::string::npos;
}

#if defined(_WIN32)
constexpr unsigned long kAccessViolation = 0xC0000005ul;
constexpr unsigned long kSecondEntry = 0xE0000002ul;
#else
constexpr unsigned long kAccessViolation = 11ul;  // finish() _exits with the signal number
constexpr unsigned long kSecondEntry = 0xE2ul;
#endif

int g_runs = 1;

// Runs one mode `g_runs` times and requires every run to die with
// `wantCode`, leave one complete report, and tell its parent on stdout what it
// died of (`wantLine`, which begins with kFaultLinePrefix).
void expectOneCompleteReport(int mode, unsigned long wantCode, const std::string& wantLine) {
    int good = 0;
    int secondEntries = 0;
    for (int run = 0; run < g_runs; ++run) {
        const fs::path dir = scratchDir(std::string(modeName(mode)) + "-" + std::to_string(run));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const Outcome o = runChild(mode, dir);
        const bool lineOk = o.stdoutText.find(wantLine) != std::string::npos;
        if (!lineOk) { std::printf("  %s run %d: stdout was [%s]\n", modeName(mode), run,
                                   o.stdoutText.c_str()); }
        const bool ok = !o.timedOut && o.exitCode == wantCode && o.reports.size() == 1u &&
                        complete(o.reports.front()) && lineOk;
        if (o.exitCode == kSecondEntry) { ++secondEntries; }
        if (ok) {
            ++good;
            fs::remove_all(dir, ec);
        } else {
            std::printf("  %s run %d: exit 0x%08lX%s, %zu report(s)%s\n", modeName(mode), run,
                        o.exitCode, o.timedOut ? " (TIMED OUT - handler stalled)" : "",
                        o.reports.size(),
                        (!o.reports.empty() && !complete(o.reports.front())) ? ", INCOMPLETE"
                                                                             : "");
        }
    }
    std::printf("%s: %d of %d runs died with 0x%08lX and one complete report "
                "(%d died with the second-entry code)\n",
                modeName(mode), good, g_runs, wantCode, secondEntries);
    CHECK(good == g_runs);
    CHECK(secondEntries == 0);
}

// THE HANDLER CANNOT RUN: it still dies with the second-entry code - that is
// correct, retrying would recurse - but its parent is told why, and what this
// thread was about to report.
void expectCannotRun(int mode, const std::string& why) {
    const fs::path dir = scratchDir(modeName(mode));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const Outcome o = runChild(mode, dir);
    const std::string want = std::string(kFaultLinePrefix) + why + ": access violation";
    std::printf("%s: exit 0x%08lX%s, stdout [%s]\n", modeName(mode), o.exitCode,
                o.timedOut ? " (TIMED OUT)" : "", o.stdoutText.c_str());
    CHECK(!o.timedOut);
    CHECK(o.exitCode == kSecondEntry);
    CHECK(o.stdoutText.find(want) != std::string::npos);
    // Said ONCE, however the thread got here.
    CHECK(o.stdoutText.find(kFaultLinePrefix) == o.stdoutText.rfind(kFaultLinePrefix));
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

// THE LINE ARRIVES WHOLE with another thread writing to the same pipe (review
// of 5e7b968): the parent's reader (source::childFaultLineFrom, the production
// one) must read the fault line and nothing of the other writer's bytes.
void expectCleanFaultLine() {
    int clean = 0;
    for (int run = 0; run < g_runs; ++run) {
        const fs::path dir = scratchDir(std::string("noisy-") + std::to_string(run));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        const Outcome o = runChild(kNoisyStdout, dir);
        const std::string line = cascade::source::childFaultLineFrom(o.stdoutText);
        const bool ok = !o.timedOut && o.exitCode == kAccessViolation &&
                        line.rfind("access violation", 0) == 0 &&
                        line.find('#') == std::string::npos && line.find(" at ") != std::string::npos;
        if (ok) {
            ++clean;
        } else {
            std::printf("  noisy-stdout run %d: exit 0x%08lX, %zu stdout bytes, line [%s]\n", run,
                        o.exitCode, o.stdoutText.size(), line.c_str());
        }
        fs::remove_all(dir, ec);
    }
    std::printf("noisy-stdout: %d of %d runs gave the parent a clean fault line\n", clean, g_runs);
    CHECK(clean == g_runs);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::strcmp(argv[1], "--second-fault") == 0) {
        return secondFaultChild(std::atoi(argv[2]), argv[3]);
    }
    if (const char* runs = std::getenv("FOXSDR_SECOND_FAULT_RUNS")) {
        g_runs = std::max(1, std::atoi(runs));
    } else {
        g_runs = 8;
    }

    const std::string av = std::string(kFaultLinePrefix) + "access violation";

    // THE FIELD SHAPE: two UHD discovery threads dying together.
    expectOneCompleteReport(kConcurrent, kAccessViolation, av);
    // A C++ exception out of a detached thread: the abort net, once.
#if defined(_WIN32)
    expectOneCompleteReport(kDetachedThrow, 0xE0000006ul,
                            std::string(kFaultLinePrefix) + "abort 0xE0000006");
#else
    // std::set_terminate is process-wide here, so it is onTerminatePosix:
    // kCodeTerminate 0xE1000001, which finish() masks to its low 7 bits.
    expectOneCompleteReport(kDetachedThrow, 1ul,
                            std::string(kFaultLinePrefix) + "std::terminate 0xE1000001");
#endif

#if defined(_WIN32)
    expectOneCompleteReport(kHeapLockSelf, kAccessViolation, av);
    expectOneCompleteReport(kHeapLockOther, kAccessViolation, av);
    expectOneCompleteReport(kHeapTrashed, kAccessViolation, av);
#else
    SKIP_LINUX("the process-heap cases are Windows heap-manager behaviour; the POSIX handler "
               "never allocates (open/write only)");
#endif

    // The two ways the handler genuinely cannot run, staged with the hook.
    expectCannotRun(kHeldSameThread, "second fault inside the crash handler");
    expectCannotRun(kHeldOtherThread, "crash handler did not finish on another thread");
    expectCleanFaultLine();
    return testSummary("test_crash_second_fault");
}
