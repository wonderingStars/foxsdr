// The Linux half of the sentinel: how the application starts it, and what the
// watcher process does. See sentinel.hpp for the design and the decision table,
// and sentinel_host_win.cpp for the Windows half this mirrors.
//
// WHAT IS DIFFERENT HERE, and it is a limit of the platform and not a choice: a
// watcher that is not the parent of a process cannot learn how it ended. The
// application starts the sentinel itself, so the sentinel is the application's
// CHILD, and the kernel gives the exit status of a child to its parent only. So
// the watcher is told when the application has GONE and nothing about HOW: the
// application keeps the write end of a pipe open for as long as it lives, the
// sentinel holds the read end, and end-of-file is the application ending by any
// route, including SIGKILL (the OOM killer, `kill -9`). The in-process POSIX
// handler already catches the fatal signals and writes its report; what is left
// for this watcher is exactly the endings it cannot see - SIGKILL, and a window
// that froze and was then killed - and every report it writes says in words that
// the cause is not available on this platform. The process tree is NOT
// restructured: the application is still nobody's child but the shell's.
//
// WHAT IT IS GIVEN: the read end of the pipe and a memfd holding the breadcrumb
// (unnamed, so nothing for another user to open), both by descriptor number on
// the command line, and every other descriptor the application had is closed in
// the child before the exec - a listening socket must not outlive the
// application inside a watcher.
//
// UNVERIFIED: this file was written on a machine that cannot run Linux. It is
// compiled for syntax against stand-in headers only (docs/DIAGNOSTICS.md, "The
// sentinel", lists what was and was not checked).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sentinel.hpp"

#if defined(__linux__)

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/diag_log.hpp"

namespace cascade::core {

namespace {

struct Host {
    SentinelOptions opts;
    bool configured = false;
    int mapFd = -1;
    breadcrumb::Block* view = nullptr;
    // BOTH ends of the liveness pipe are kept for the life of the process. The
    // write end is what "the application is alive" is made of: closing it while the
    // application lives would tell a running sentinel the application had gone.
    int pipeRead = -1;
    int pipeWrite = -1;
    pid_t pid = 0;
    pid_t reapPid = 0;  // a sentinel that was told to go and has not been collected yet
    bool endedLogged = false;
    bool failedLogged = false;
    double lastStartMs = -1.0;
};

Host g;

void sayStartFailed(const char* what, int error) {
    if (g.failedLogged) { return; }
    g.failedLogged = true;
    diagWarnf("sentinel: not started (%s, error %d); an ending this session that the application "
              "cannot report itself will not be written up",
              what, error);
}

bool ensureBlock() {
    if (g.view != nullptr) { return true; }
#ifdef SYS_memfd_create
    // UNNAMED: a memfd has no name in any namespace another process could open.
    // The flag is MFD_CLOEXEC (1); the one descriptor the sentinel needs has the
    // flag cleared in the child, between the fork and the exec.
    const int fd = static_cast<int>(::syscall(SYS_memfd_create, "foxsdr-breadcrumb", 1u));
    if (fd < 0) { return false; }
    if (::ftruncate(fd, static_cast<off_t>(sizeof(breadcrumb::Block))) != 0) {
        ::close(fd);
        return false;
    }
    void* v = ::mmap(nullptr, sizeof(breadcrumb::Block), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (v == MAP_FAILED) {
        ::close(fd);
        return false;
    }
    g.mapFd = fd;
    g.view = static_cast<breadcrumb::Block*>(v);
    breadcrumb::attach(g.view);
    return true;
#else
    return false;
#endif
}

bool ensurePipe() {
    if (g.pipeWrite >= 0) { return true; }
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) { return false; }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    g.pipeRead = fds[0];
    g.pipeWrite = fds[1];
    return true;
}

std::string selfExePath() {
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

void reapQuietly() {
    if (g.reapPid > 0) {
        int status = 0;
        if (::waitpid(g.reapPid, &status, WNOHANG) == g.reapPid) { g.reapPid = 0; }
    }
}

bool startProcess() {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string exe = g.opts.exePath.empty() ? selfExePath() : g.opts.exePath;
    // Checked BEFORE the fork: fork() succeeds whether or not the path exists.
    if (exe.empty() || ::access(exe.c_str(), X_OK) != 0) {
        sayStartFailed("no executable to start", errno);
        return false;
    }

    std::vector<std::string> store;
    store.push_back(exe);
    store.push_back("--sentinel");
    store.push_back("--app-pid=" + std::to_string(static_cast<long>(::getpid())));
    store.push_back("--pipe-fd=" + std::to_string(g.pipeRead));
    store.push_back("--map-fd=" + std::to_string(g.mapFd));
    if (!g.opts.crashDir.empty()) { store.push_back("--crash-dir=" + g.opts.crashDir); }
    if (!g.opts.logDir.empty()) { store.push_back("--log-dir=" + g.opts.logDir); }
    std::vector<char*> argv;
    for (std::string& s : store) { argv.push_back(s.data()); }
    argv.push_back(nullptr);

    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 16) { maxFd = 1024; }
    if (maxFd > 4096) { maxFd = 4096; }
    const int keepA = g.pipeRead;
    const int keepB = g.mapFd;

    const pid_t pid = ::fork();
    if (pid < 0) {
        sayStartFailed("fork failed", errno);
        return false;
    }
    if (pid == 0) {
        // CHILD. Only async-signal-safe calls from here to execv.
        //
        // Its own session: a Ctrl-C in the terminal the application runs in goes to
        // the foreground process group, and a watcher in that group would die with
        // the very ending it is there to see.
        ::setsid();
        ::fcntl(keepA, F_SETFD, 0);
        ::fcntl(keepB, F_SETFD, 0);
        for (int fd = 3; fd < maxFd; ++fd) {
            if (fd != keepA && fd != keepB) { ::close(fd); }
        }
        const int nul = ::open("/dev/null", O_RDWR);
        if (nul >= 0) {
            ::dup2(nul, 0);
            ::dup2(nul, 1);
            ::dup2(nul, 2);
            if (nul > 2) { ::close(nul); }
        }
        ::execv(argv[0], argv.data());
        ::_exit(127);  // exec failed; unreachable otherwise
    }

    g.pid = pid;
    g.lastStartMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    diagLogf("sentinel: watching this session (process %ld, started in %.1f ms)",
             static_cast<long>(g.pid), g.lastStartMs);
    return true;
}

// Has the sentinel ended? A child must be collected by its parent, so this is also
// what keeps a watcher that died while the application lives from lingering.
bool processEnded(int* status) {
    if (g.pid <= 0) { return false; }
    int st = 0;
    const pid_t r = ::waitpid(g.pid, &st, WNOHANG);
    if (r == g.pid) {
        if (status != nullptr) { *status = st; }
        return true;
    }
    return false;
}

}  // namespace

void sentinelConfigure(const SentinelOptions& opts) {
    g.opts = opts;
    g.configured = true;
    // The page is made NOW, whether or not a watcher is started: see the Windows
    // half for why.
    (void)ensureBlock();
}

bool sentinelSetEnabled(bool on) {
    if (!g.configured) { return false; }
    if (!on) {
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, true);
        if (g.pid > 0) {
            ::kill(g.pid, SIGKILL);
            g.reapPid = g.pid;  // collected without waiting, by sentinelPoll()
            g.pid = 0;
            g.endedLogged = true;
            reapQuietly();
        }
        return false;
    }
    if (g.pid > 0 && !processEnded(nullptr)) { return true; }
    g.pid = 0;
    if (!ensureBlock()) {
        sayStartFailed("could not make the shared page", errno);
        return false;
    }
    if (!ensurePipe()) {
        sayStartFailed("could not make the pipe", errno);
        return false;
    }
    breadcrumb::setFlag(breadcrumb::kFlagReportsOff, false);
    g.endedLogged = false;
    return startProcess();
}

void sentinelPoll() {
    reapQuietly();
    int status = 0;
    if (g.pid <= 0 || g.endedLogged || !processEnded(&status)) { return; }
    g.endedLogged = true;
    g.pid = 0;
    if (WIFEXITED(status)) {
        diagWarnf("sentinel: the watcher ended while the application was still running (exit "
                  "status %d); an ending this session that the application cannot report itself "
                  "will not be written up",
                  WEXITSTATUS(status));
    } else {
        diagWarnf("sentinel: the watcher ended while the application was still running (signal "
                  "%d); an ending this session that the application cannot report itself will "
                  "not be written up",
                  WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
}

bool sentinelRunning() { return g.pid > 0 && !processEnded(nullptr); }
unsigned long sentinelProcessId() { return g.pid > 0 ? static_cast<unsigned long>(g.pid) : 0ul; }
double sentinelLastStartMs() { return g.lastStartMs; }

// ---------------------------------------------------------------------------
// The watcher process: `cascade --sentinel --app-pid=N --pipe-fd=R --map-fd=M
// [--crash-dir=DIR] [--log-dir=DIR]`.
// ---------------------------------------------------------------------------
namespace {

bool takeValue(const char* arg, const char* flag, const char*& value) {
    const std::size_t n = std::strlen(flag);
    if (std::strncmp(arg, flag, n) != 0) { return false; }
    value = arg + n;
    return true;
}

}  // namespace

int runSentinelMain(int argc, char** argv) {
    unsigned long appPid = 0;
    int pipeFd = -1;
    int mapFd = -1;
    std::string crashDir;
    std::string logDir;
    for (int i = 2; i < argc; ++i) {  // argv[1] is "--sentinel"
        const char* v = nullptr;
        if (takeValue(argv[i], "--app-pid=", v)) {
            appPid = std::strtoul(v, nullptr, 10);
        } else if (takeValue(argv[i], "--pipe-fd=", v)) {
            pipeFd = std::atoi(v);
        } else if (takeValue(argv[i], "--map-fd=", v)) {
            mapFd = std::atoi(v);
        } else if (takeValue(argv[i], "--crash-dir=", v)) {
            crashDir = v;
        } else if (takeValue(argv[i], "--log-dir=", v)) {
            logDir = v;
        } else {
            return 2;
        }
    }
    if (pipeFd < 0 || mapFd < 0 || appPid == 0) { return 2; }

    void* view = ::mmap(nullptr, sizeof(breadcrumb::Block), PROT_READ, MAP_SHARED, mapFd, 0);
    if (view == MAP_FAILED) { view = nullptr; }

    // THE WAIT, and the only one this process makes: a blocking read on the pipe
    // whose write end the application holds. It returns 0 when every write end is
    // closed - the application has ended, however it ended. Nothing is ever
    // written to the pipe, so any data is ignored. Not bounded, for the reason the
    // Windows side gives.
    for (;;) {
        char c = 0;
        const ssize_t n = ::read(pipeFd, &c, 1);
        if (n == 0) { break; }
        if (n < 0 && errno != EINTR) { return 3; }
    }
    // THE APPLICATION HAS GONE: one second to be gone too (kSentinelExitDeadline).
    armSentinelExitDeadline();

    SentinelEnd end;
    end.appPid = appPid;
    end.exitKnown = false;  // a watcher that is not the parent cannot learn it
    end.osSessionEnding = false;
    end.crumb = breadcrumb::read(static_cast<const breadcrumb::Block*>(view));
    end.crashDir = crashDir;
    end.logDir = logDir;
    (void)finishSentinelWatch(end);
    return 0;
}

}  // namespace cascade::core

#elif !defined(_WIN32)

// Any other platform: the application never starts a watcher, and says so by
// returning false from the one call that would.
namespace cascade::core {
void sentinelConfigure(const SentinelOptions&) {}
bool sentinelSetEnabled(bool) { return false; }
void sentinelPoll() {}
bool sentinelRunning() { return false; }
unsigned long sentinelProcessId() { return 0ul; }
double sentinelLastStartMs() { return -1.0; }
int runSentinelMain(int, char**) { return 2; }
}  // namespace cascade::core

#endif
