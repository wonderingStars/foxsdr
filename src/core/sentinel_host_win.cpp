// The Windows half of the sentinel: how the application starts it, and what the
// watcher process does. See sentinel.hpp for the design and the decision table.
//
// WHAT THE WATCHER IS GIVEN, and nothing else:
//
//   - a handle to the application's process with SYNCHRONIZE and
//     PROCESS_QUERY_LIMITED_INFORMATION and no other right: enough to wait for it
//     to end and to read its exit code and its start and exit times. Not enough to
//     read its memory, suspend it, debug it or end it.
//   - a READ-ONLY handle to the breadcrumb's mapping (FILE_MAP_READ).
//
// Both travel as INHERITED handles, by number on the command line. The mapping
// has no name, so there is nothing for another user or another session to open
// or to collide with, and the process handle is a handle to THIS process and not
// a lookup by process id - which could name another process if the id were
// reused. Exactly those two handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST),
// so the watcher never holds a pipe or a file of the application's, and a reader
// waiting for the application's output to end is never kept waiting by it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sentinel.hpp"

#if defined(_WIN32)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <windows.h>

#include "core/diag_log.hpp"

namespace cascade::core {

namespace {

// THE ANSI CODE PAGE, NOT UTF-8 (unlike source/soapy_enum_proc.cpp's helper, which
// reads the same kind of string as UTF-8): the two folders reach this process as
// narrow strings from getenv("LOCALAPPDATA"), which are in the active code page
// (the manifest does not select UTF-8), and the watcher's narrow argv is made from the
// wide command line by the same code page. Read as UTF-8, a profile folder with an
// accented letter in its name would become U+FFFD and the watcher would be handed a
// folder that does not exist - and write nothing.
std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) { return std::wstring(); }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring selfExePath() {
    std::wstring buf(1024, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return std::wstring(); }
    buf.resize(n);
    return buf;
}

// A trailing separator is stripped: inside a quoted field a backslash before the
// closing quote escapes it (the same care soapy_enum_proc.cpp takes).
std::string trimmedDir(std::string dir) {
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) { dir.pop_back(); }
    return dir;
}

// ---------------------------------------------------------------------------
// The application's side. GUI thread only: nothing here is locked.
// ---------------------------------------------------------------------------
struct Host {
    SentinelOptions opts;
    bool configured = false;
    HANDLE mapping = nullptr;
    breadcrumb::Block* view = nullptr;
    HANDLE proc = nullptr;
    unsigned long pid = 0;
    bool endedLogged = false;
    bool failedLogged = false;
    double lastStartMs = -1.0;
};

Host g;

bool ensureBlock() {
    if (g.view != nullptr) { return true; }
    // Pagefile-backed, UNNAMED, zero-filled by the system: see the header's
    // "NO NAME, NO COLLISION".
    HANDLE m = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    static_cast<DWORD>(sizeof(breadcrumb::Block)), nullptr);
    if (m == nullptr) { return false; }
    void* v = ::MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, sizeof(breadcrumb::Block));
    if (v == nullptr) {
        ::CloseHandle(m);
        return false;
    }
    g.mapping = m;
    g.view = static_cast<breadcrumb::Block*>(v);
    breadcrumb::attach(g.view);
    return true;
}

void sayStartFailed(const char* what, unsigned long error) {
    if (g.failedLogged) { return; }
    g.failedLogged = true;
    diagWarnf("sentinel: not started (%s, error %lu); an ending this session that the application "
              "cannot report itself will not be written up",
              what, error);
}

bool startProcess() {
    const auto t0 = std::chrono::steady_clock::now();

    std::wstring exe = g.opts.exePath.empty() ? selfExePath() : widen(g.opts.exePath);
    if (exe.empty()) {
        sayStartFailed("no executable path", 0);
        return false;
    }

    // Inheritable COPIES, made for this one spawn and closed straight after it,
    // so no other CreateProcess in this process can hand them to anything else.
    HANDLE procInh = nullptr;
    HANDLE mapInh = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(),
                          &procInh, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0) == 0) {
        sayStartFailed("could not duplicate the process handle", ::GetLastError());
        return false;
    }
    if (::DuplicateHandle(::GetCurrentProcess(), g.mapping, ::GetCurrentProcess(), &mapInh,
                          FILE_MAP_READ, TRUE, 0) == 0) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(procInh);
        sayStartFailed("could not duplicate the mapping handle", err);
        return false;
    }

    // EXACTLY TWO HANDLES. A list that cannot be built is a start that does not
    // happen: falling back to "inherit everything" would hand the watcher whatever
    // pipe another thread of this process has open at that instant.
    HANDLE allowed[2] = {procInh, mapInh};
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrStore(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        attrSize > 0 ? reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStore.data()) : nullptr;
    bool haveAttrs = false;
    if (attrs != nullptr && ::InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != 0) {
        haveAttrs = true;
        if (::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allowed,
                                        sizeof(allowed), nullptr, nullptr) == 0) {
            ::DeleteProcThreadAttributeList(attrs);
            haveAttrs = false;
        }
    }
    if (!haveAttrs) {
        ::CloseHandle(procInh);
        ::CloseHandle(mapInh);
        sayStartFailed("could not restrict the inherited handles", ::GetLastError());
        return false;
    }

    std::wstring cmd = L"\"" + exe + L"\" --sentinel --app-pid=" +
                       std::to_wstring(static_cast<unsigned long>(::GetCurrentProcessId())) +
                       L" --app-handle=" +
                       std::to_wstring(static_cast<unsigned long long>(
                           reinterpret_cast<std::uintptr_t>(procInh))) +
                       L" --map-handle=" +
                       std::to_wstring(static_cast<unsigned long long>(
                           reinterpret_cast<std::uintptr_t>(mapInh)));
    const std::string crashDir = trimmedDir(g.opts.crashDir);
    const std::string logDir = trimmedDir(g.opts.logDir);
    if (!crashDir.empty()) { cmd += L" \"--crash-dir=" + widen(crashDir) + L"\""; }
    if (!logDir.empty()) { cmd += L" \"--log-dir=" + widen(logDir) + L"\""; }
    cmd.push_back(L'\0');

    STARTUPINFOEXW six{};
    six.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    six.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    // NO WINDOW AND NO CONSOLE: the application's own console, if it has one, is
    // not shared and a new one is not made.
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                     nullptr, &six.StartupInfo, &pi);
    const DWORD err = ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);
    ::CloseHandle(procInh);
    ::CloseHandle(mapInh);
    if (ok == 0) {
        sayStartFailed("the process could not be created", err);
        return false;
    }
    ::CloseHandle(pi.hThread);
    g.proc = pi.hProcess;
    g.pid = static_cast<unsigned long>(pi.dwProcessId);
    g.lastStartMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // The watcher's process id is in the line, an operating-system number that
    // means nothing once it has ended (the same kind a report's thread line carries):
    // it is how a person, or a test, finds the process this line is about.
    diagLogf("sentinel: watching this session (process %lu, started in %.1f ms)", g.pid,
             g.lastStartMs);
    return true;
}

void dropProcessHandle() {
    if (g.proc != nullptr) { ::CloseHandle(g.proc); }
    g.proc = nullptr;
    g.pid = 0;
}

bool processEnded() {
    return g.proc != nullptr && ::WaitForSingleObject(g.proc, 0) == WAIT_OBJECT_0;
}

}  // namespace

void sentinelConfigure(const SentinelOptions& opts) {
    g.opts = opts;
    g.configured = true;
    // The page is made NOW, whether or not a watcher is started: it is only
    // memory in this process, and having it from here means the phase and the
    // heartbeat are right if the user switches Diagnostics on part-way through.
    // Failing to make it is not an error: nothing is tracked and no watcher starts.
    (void)ensureBlock();
}

bool sentinelSetEnabled(bool on) {
    if (!g.configured) { return false; }
    if (!on) {
        // OFF: the flag first, so that nothing is written whatever happens next,
        // then the watcher itself is ended. Not waited for: TerminateProcess is
        // asynchronous and the application has no business waiting on it.
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, true);
        if (g.proc != nullptr) {
            ::TerminateProcess(g.proc, 0);
            dropProcessHandle();
            g.endedLogged = true;  // ended on purpose: nothing to say about it later
        }
        return false;
    }
    if (g.proc != nullptr && !processEnded()) { return true; }
    dropProcessHandle();
    if (!ensureBlock()) {
        sayStartFailed("could not make the shared page", ::GetLastError());
        return false;
    }
    breadcrumb::setFlag(breadcrumb::kFlagReportsOff, false);
    g.endedLogged = false;
    return startProcess();
}

void sentinelPoll() {
    if (g.proc == nullptr || g.endedLogged || !processEnded()) { return; }
    DWORD code = 0;
    ::GetExitCodeProcess(g.proc, &code);
    g.endedLogged = true;
    dropProcessHandle();
    diagWarnf("sentinel: the watcher ended while the application was still running (exit code "
              "0x%08lX); an ending this session that the application cannot report itself will "
              "not be written up",
              static_cast<unsigned long>(code));
}

bool sentinelRunning() { return g.proc != nullptr && !processEnded(); }
unsigned long sentinelProcessId() { return g.proc != nullptr ? g.pid : 0ul; }
double sentinelLastStartMs() { return g.lastStartMs; }

// ---------------------------------------------------------------------------
// The watcher process: `cascade --sentinel --app-pid=N --app-handle=H --map-handle=M
// [--crash-dir=DIR] [--log-dir=DIR]`.
// ---------------------------------------------------------------------------
namespace {

bool takeValue(const char* arg, const char* flag, const char*& value) {
    const std::size_t n = std::strlen(flag);
    if (std::strncmp(arg, flag, n) != 0) { return false; }
    value = arg + n;
    return true;
}

std::chrono::system_clock::time_point fromFileTime(const FILETIME& ft) {
    // 100 ns ticks since 1601-01-01; the system clock counts from 1970-01-01.
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    constexpr unsigned long long kUnixEpochTicks = 116444736000000000ull;
    const unsigned long long ticks = u.QuadPart > kUnixEpochTicks ? u.QuadPart - kUnixEpochTicks : 0ull;
    return std::chrono::system_clock::from_time_t(static_cast<std::time_t>(ticks / 10000000ull)) +
           std::chrono::duration_cast<std::chrono::system_clock::duration>(
               std::chrono::microseconds((ticks % 10000000ull) / 10ull));
}

}  // namespace

int runSentinelMain(int argc, char** argv) {
    // No dialog of any kind, whatever happens to this process: it is hidden and
    // nobody could dismiss it.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // Among the LAST processes of a closing session to be ended (level 0x100, the
    // lowest an application may ask for; the application itself has the default
    // 0x280), so that it is still here to see the application go. Best effort.
    ::SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

    unsigned long appPid = 0;
    HANDLE app = nullptr;
    HANDLE map = nullptr;
    std::string crashDir;
    std::string logDir;
    for (int i = 2; i < argc; ++i) {  // argv[1] is "--sentinel"
        const char* v = nullptr;
        if (takeValue(argv[i], "--app-pid=", v)) {
            appPid = std::strtoul(v, nullptr, 10);
        } else if (takeValue(argv[i], "--app-handle=", v)) {
            app = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(v, nullptr, 10)));
        } else if (takeValue(argv[i], "--map-handle=", v)) {
            map = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::strtoull(v, nullptr, 10)));
        } else if (takeValue(argv[i], "--crash-dir=", v)) {
            crashDir = v;
        } else if (takeValue(argv[i], "--log-dir=", v)) {
            logDir = v;
        } else {
            return 2;  // an argument this watcher was not built to take
        }
    }
    if (app == nullptr || map == nullptr || appPid == 0) { return 2; }

    void* view = ::MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(breadcrumb::Block));

    // THE WAIT, and the only one this process makes: it blocks on the application's
    // process handle and uses no CPU until the application ends. It is not bounded
    // on purpose - a watcher that gave up waiting would be a watcher that missed
    // the ending it is for - and it is ended by the application ending, by the
    // application's Diagnostics switch (which ends this process), or by the
    // operating system.
    if (::WaitForSingleObject(app, INFINITE) != WAIT_OBJECT_0) { return 3; }
    // THE APPLICATION HAS ENDED: from this instant this process has one second to
    // be gone (kSentinelExitDeadline), so cascade.exe - which this process is a
    // running copy of - can be replaced by an installer.
    armSentinelExitDeadline();

    SentinelEnd end;
    end.appPid = appPid;
    end.exitKnown = true;
    DWORD code = 0;
    if (::GetExitCodeProcess(app, &code) == 0) { end.exitKnown = false; }
    end.exitCode = static_cast<unsigned long>(code);
    FILETIME created{}, exited{}, kernel{}, user{};
    if (::GetProcessTimes(app, &created, &exited, &kernel, &user) != 0) {
        end.appStartedKnown = true;
        end.appStarted = fromFileTime(created);
        // The same two instants, untouched, for matching Windows' crash record of this
        // death (core/os_crash_record.hpp): its ProcessCreationTime is this very value.
        end.appStartFileTime = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
                               static_cast<std::uint64_t>(created.dwLowDateTime);
        end.appEndFileTime = (static_cast<std::uint64_t>(exited.dwHighDateTime) << 32) |
                             static_cast<std::uint64_t>(exited.dwLowDateTime);
        ULARGE_INTEGER a, b;
        a.LowPart = created.dwLowDateTime;
        a.HighPart = created.dwHighDateTime;
        b.LowPart = exited.dwLowDateTime;
        b.HighPart = exited.dwHighDateTime;
        if (b.QuadPart >= a.QuadPart) {
            end.uptimeSec = static_cast<std::int64_t>((b.QuadPart - a.QuadPart) / 10000000ull);
        }
    }
    // What the operating system itself says: "Nonzero if the current session is
    // shutting down" (GetSystemMetrics, SM_SHUTTINGDOWN).
    end.osSessionEnding = ::GetSystemMetrics(SM_SHUTTINGDOWN) != 0;
    end.crumb = breadcrumb::read(static_cast<const breadcrumb::Block*>(view));
    end.crashDir = crashDir;
    end.logDir = logDir;
    (void)finishSentinelWatch(end);
    return 0;
}

}  // namespace cascade::core

#endif  // _WIN32
