// See source/soapy_enum_proc.hpp for why the device walk runs in a child
// process at all, and for the measurements behind the timeout and the retry.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_enum_proc.hpp"

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "source/vendor_guard.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace cascade::source {

namespace {

constexpr int kSchema = 1;

// The child's whole vocabulary, in one place so the writer and the reader
// cannot drift apart.
constexpr const char* kKeySchema = "schema";
constexpr const char* kKeyRuntime = "runtime";
constexpr const char* kKeyGuarded = "guardedCalls";
constexpr const char* kKeyCapture = "capture";
constexpr const char* kKeyDevices = "devices";
constexpr const char* kKeyLabel = "label";
constexpr const char* kKeyArgs = "args";
constexpr const char* kKeyDrivers = "drivers";

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                        nullptr, 0);
    if (n <= 0) { return std::wstring(); }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) { return std::string(); }
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n,
                          nullptr, nullptr);
    return out;
}

// The running executable's directory, with the trailing separator.
std::wstring exeDirW() {
    std::wstring buf(1024, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return std::wstring(); }
    buf.resize(n);
    const std::size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { return std::wstring(); }
    return buf.substr(0, slash + 1);
}

// A handle onto NUL, for the child's stdin and for its stderr when this
// process has none. CreateProcess with STARTF_USESTDHANDLES wants real
// handles, and a GUI application's std handles are frequently null.
HANDLE openNul() {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

#endif  // _WIN32

// Parses ONE candidate line. Returns false if it is not the answer this
// version of the protocol expects - a mismatched schema is a version skew
// between a parent and a helper from different installs, and guessing at it
// would be worse than saying so.
bool parseOneLine(const std::string& text, EnumResult& out) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return false; }
    if (!j.contains(kKeySchema) || !j[kKeySchema].is_number_integer()) { return false; }
    if (j[kKeySchema].get<int>() != kSchema) { return false; }
    if (!j.contains(kKeyDevices) || !j[kKeyDevices].is_array()) { return false; }

    out.childRuntimeAvailable =
        j.contains(kKeyRuntime) && j[kKeyRuntime].is_boolean() && j[kKeyRuntime].get<bool>();
    out.guardedCalls = (j.contains(kKeyGuarded) && j[kKeyGuarded].is_number_unsigned())
                           ? j[kKeyGuarded].get<unsigned long long>()
                           : 0ull;
    out.childCaptureArmed =
        j.contains(kKeyCapture) && j[kKeyCapture].is_boolean() && j[kKeyCapture].get<bool>();

    if (j.contains(kKeyDrivers) && j[kKeyDrivers].is_array()) {
        for (const auto& d : j[kKeyDrivers]) {
            if (d.is_string() && !d.get<std::string>().empty()) {
                out.drivers.push_back(d.get<std::string>());
            }
        }
    }

    for (const auto& e : j[kKeyDevices]) {
        if (!e.is_object()) { return false; }
        SoapyDeviceInfo info;
        if (e.contains(kKeyLabel) && e[kKeyLabel].is_string()) {
            info.label = e[kKeyLabel].get<std::string>();
        }
        if (e.contains(kKeyArgs) && e[kKeyArgs].is_string()) {
            info.args = e[kKeyArgs].get<std::string>();
        }
        // A row the Source menu could not show or reopen is not a device.
        // Dropping it here keeps that invariant out of every consumer.
        if (info.label.empty() || info.args.empty()) { continue; }
        out.devices.push_back(std::move(info));
    }
    return true;
}

// LINE BY LINE, LAST FIRST, rather than parsing the whole capture.
//
// The child shares its stdout with whatever the SoapySDR runtime and the
// vendor modules decide to print. Today they all log to stderr - checked, not
// assumed - but "today" is doing a lot of work in that sentence: one vendor
// module with a printf in its find function would otherwise turn every scan on
// that machine into Malformed, which reads to a user as "no devices" for a
// reason no log explains. Scanning backwards for the first line that IS this
// protocol costs nothing and removes the whole class.
bool parseChildOutput(const std::string& text, EnumResult& out) {
    std::size_t end = text.size();
    while (end > 0) {
        std::size_t begin = text.find_last_of('\n', end - 1);
        begin = (begin == std::string::npos) ? 0 : begin + 1;
        const std::string line = text.substr(begin, end - begin);
        if (!line.empty() && line.find_first_not_of(" \t\r") != std::string::npos) {
            EnumResult candidate;
            if (parseOneLine(line, candidate)) {
                out.devices = std::move(candidate.devices);
                out.drivers = std::move(candidate.drivers);
                out.guardedCalls = candidate.guardedCalls;
                out.childRuntimeAvailable = candidate.childRuntimeAvailable;
                out.childCaptureArmed = candidate.childCaptureArmed;
                return true;
            }
        }
        if (begin == 0) { break; }
        end = begin - 1;  // step over the '\n' that ended the previous line
    }
    return false;
}

// One child, start to finish. Fills `out` and returns.
//
// `crashDir` is handed to the child on its command line when it is non-empty,
// and is the parent's already-armed capture directory - see CRASH CAPTURE IN
// THE CHILD in the header.
void runOneChild(const std::string& helper, unsigned long timeoutMs,
                 const std::string& crashDir, EnumResult& out,
                 const std::string& extraArg = std::string()) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (::CreatePipe(&rd, &wr, &sa, 0) == 0) {
        out.outcome = EnumOutcome::SpawnFailed;
        return;
    }
    // The READ end must not be inheritable, or the child holds a copy of it
    // and the pipe never reaches end-of-file when the child exits.
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = openNul();

    // THE PARENT'S stderr, deliberately: UHD prints its discovery errors there
    // and always has, so a console run keeps showing exactly what it showed
    // when the walk was in-process, and a windowed run discards them exactly
    // as it did. Duplicated INHERITABLE because the handle list below refuses
    // any handle that is not - a std handle usually is not, and the original
    // must not have its flags changed under the rest of the process.
    HANDLE errDup = nullptr;
    const HANDLE errRaw = ::GetStdHandle(STD_ERROR_HANDLE);
    if (errRaw != nullptr && errRaw != INVALID_HANDLE_VALUE) {
        if (::DuplicateHandle(::GetCurrentProcess(), errRaw, ::GetCurrentProcess(), &errDup, 0,
                              TRUE, DUPLICATE_SAME_ACCESS) == 0) {
            errDup = nullptr;
        }
    }
    HANDLE errH = (errDup != nullptr) ? errDup : nul;

    STARTUPINFOEXW six{};
    STARTUPINFOW& si = six.StartupInfo;
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = errH;

    // EXACTLY THREE HANDLES, and nothing else the parent happens to hold.
    //
    // bInheritHandles TRUE with no list hands the child EVERY inheritable
    // handle in the process - and the write end of a CONCURRENT enumeration's
    // pipe is one of them. That child would then keep the other scan's pipe
    // open past its own exit, its drain thread would never see end-of-file,
    // and a good answer would become a 20 s timeout and a kill. See SAFE TO
    // CALL CONCURRENTLY in the header. Duplicates are collapsed because
    // hStdError is hStdInput when this process has no stderr of its own.
    HANDLE allowed[3];
    DWORD nAllowed = 0;
    for (HANDLE h : {nul, wr, errH}) {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) { continue; }
        bool seen = false;
        for (DWORD i = 0; i < nAllowed; ++i) {
            if (allowed[i] == h) { seen = true; }
        }
        if (!seen) { allowed[nAllowed++] = h; }
    }

    // A list that cannot be built is not fatal: the spawn falls back to plain
    // inheritance, which is what every build before this one did. Losing the
    // scoping costs nothing unless two scans overlap.
    SIZE_T attrSize = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrStore(attrSize);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        (attrSize > 0) ? reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrStore.data())
                       : nullptr;
    bool haveAttrs = false;
    if (attrs != nullptr && ::InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) != 0) {
        haveAttrs = true;
        if (nAllowed == 0 ||
            ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, allowed,
                                        nAllowed * sizeof(HANDLE), nullptr, nullptr) == 0) {
            ::DeleteProcThreadAttributeList(attrs);
            haveAttrs = false;
        }
    }
    if (haveAttrs) {
        six.lpAttributeList = attrs;
        si.cb = sizeof(STARTUPINFOEXW);
    }

    // CommandLineToArgvW quoting: both fields are quoted, so spaces in an
    // install directory - or in the crash directory, which lives under a user
    // name that frequently has one - are handled. CreateProcessW mutates its
    // command-line argument, hence the writable buffer.
    std::wstring cmd = L"\"" + widen(helper) + L"\" --enumerate-json";
    if (!crashDir.empty()) {
        // A TRAILING SEPARATOR IS STRIPPED, because inside a quoted field a
        // backslash immediately before the closing quote escapes it and
        // CommandLineToArgvW hands the child one argument made of two.
        std::string dir = crashDir;
        while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) { dir.pop_back(); }
        if (!dir.empty()) { cmd += L" \"--crash-dir=" + widen(dir) + L"\""; }
    }
    // One more flag, quoted the same way: --list-drivers, or --driver=<name>
    // where the name comes from a child of ours and never from a user.
    if (!extraArg.empty()) { cmd += L" \"" + widen(extraArg) + L"\""; }
    cmd.push_back(L'\0');

    // A JOB THE CHILD CANNOT OUTLIVE, and this is not belt-and-braces.
    //
    // The parent can disappear while a probe is still running: AppWindow's quit
    // path waits 250 ms for an in-flight scan and then DETACHES it (see
    // reapPendingSoapyScan), so a user who quits during a scan leaves nobody
    // holding the timeout. Without a job, a child wedged inside a USB probe -
    // exactly the case the timeout exists for - would be orphaned mid-probe,
    // with no parent left to kill it, sitting on the bus indefinitely and
    // poisoning every later run against a device that "can wedge". A job with
    // KILL_ON_JOB_CLOSE makes the operating system do it instead: the last
    // handle to the job goes when this process does, whatever way it goes.
    //
    // Started SUSPENDED so the assignment cannot lose a race against a child
    // that has already begun work. A job that cannot be created or assigned is
    // not fatal - the child still runs, it just loses this protection.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                      sizeof(limits)) == 0) {
            ::CloseHandle(job);
            job = nullptr;
        }
    }

    PROCESS_INFORMATION pi{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED |
                        (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0ul);
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags,
                                     nullptr, nullptr, &si, &pi);
    // The parent's copy of the WRITE end must go now, whether or not the spawn
    // worked: while it is open the pipe has a writer and the drain below would
    // wait for end-of-file for ever.
    ::CloseHandle(wr);
    if (haveAttrs) { ::DeleteProcThreadAttributeList(attrs); }
    if (errDup != nullptr) { ::CloseHandle(errDup); }
    if (ok == 0) {
        out.outcome = EnumOutcome::SpawnFailed;
        ::CloseHandle(rd);
        if (job != nullptr) { ::CloseHandle(job); }
        if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
        return;
    }
    if (job != nullptr) { ::AssignProcessToJobObject(job, pi.hProcess); }
    ::ResumeThread(pi.hThread);
    out.attempts += 1;

    // DRAINED ON ITS OWN THREAD, not after the wait. A pipe holds 64 KiB by
    // default; a child that filled it would block in WriteFile while this
    // thread blocked in WaitForSingleObject, and the only thing that would end
    // that is the timeout - turning a perfectly good answer into a kill.
    std::string text;
    std::thread drain([rd, &text] {
        char buf[4096];
        for (;;) {
            DWORD got = 0;
            if (::ReadFile(rd, buf, sizeof(buf), &got, nullptr) == 0 || got == 0) { break; }
            text.append(buf, got);
        }
    });

    const DWORD waited = ::WaitForSingleObject(pi.hProcess, timeoutMs);
    if (waited == WAIT_TIMEOUT) {
        // Kill it, then let the wait below collect it: terminating closes the
        // child's handles, which is what gives the drain thread its EOF.
        ::TerminateProcess(pi.hProcess, 0xE0454E55ul);  // 'ENU' + kill marker
        ::WaitForSingleObject(pi.hProcess, 5000);
        out.outcome = EnumOutcome::ChildTimedOut;
    }

    drain.join();
    ::CloseHandle(rd);
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }

    DWORD exitCode = 0;
    ::GetExitCodeProcess(pi.hProcess, &exitCode);
    out.exitCode = static_cast<unsigned long>(exitCode);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    // Safe here and only here: the child has already been waited on, so closing
    // the job kills nothing that is still wanted.
    if (job != nullptr) { ::CloseHandle(job); }

    if (out.outcome == EnumOutcome::ChildTimedOut) { return; }
    if (exitCode != 0) {
        out.outcome = EnumOutcome::ChildDied;
        return;
    }
    if (!parseChildOutput(text, out)) {
        out.devices.clear();
        out.outcome = EnumOutcome::Malformed;
        return;
    }
    out.outcome = EnumOutcome::Ok;
#else
    // THE POSIX SIDE. fork()+exec() rather than posix_spawn(), because the
    // parent-side protections below - the process's own group, and the
    // kernel killing it if this process dies first - both need code to run
    // in the child BETWEEN the fork and the exec, and posix_spawn has no seam
    // for that. Everything run there is async-signal-safe (setpgid, prctl,
    // dup2, close, execv) - fork() in a process with other threads only
    // duplicates the calling thread, and nothing more than that is safe to
    // call before the exec replaces this image.
    //
    // CHECKED BEFORE FORKING, unlike Windows's CreateProcess-does-the-check:
    // fork() itself always succeeds regardless of whether `helper` exists, so
    // without this a bad path would still count as an attempt and only fail
    // once reaped - breaking the SpawnFailed contract ("nothing was ever
    // started", attempts == 0) that a missing helper is supposed to keep.
    if (::access(helper.c_str(), X_OK) != 0) {
        out.outcome = EnumOutcome::SpawnFailed;
        return;
    }

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        out.outcome = EnumOutcome::SpawnFailed;
        return;
    }
    // Close-on-exec on both ends by default. The child clears it on exactly
    // the descriptor it dup2()s into place - see SAFE TO CALL CONCURRENTLY in
    // the header: two overlapping scans must not hand a child the OTHER
    // scan's pipe, or its drain thread never sees end-of-file.
    ::fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);

    const int nullFd = ::open("/dev/null", O_RDWR | O_CLOEXEC);

    std::vector<std::string> argStorage;
    argStorage.push_back(helper);
    argStorage.push_back("--enumerate-json");
    if (!crashDir.empty()) { argStorage.push_back("--crash-dir=" + crashDir); }
    if (!extraArg.empty()) { argStorage.push_back(extraArg); }
    std::vector<char*> argv;
    argv.reserve(argStorage.size() + 1);
    for (std::string& s : argStorage) { argv.push_back(s.data()); }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        out.outcome = EnumOutcome::SpawnFailed;
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        if (nullFd >= 0) { ::close(nullFd); }
        return;
    }
    if (pid == 0) {
        // CHILD. Only async-signal-safe calls from here to execv.
        //
        // ITS OWN PROCESS GROUP, so a timeout below can kill it and anything
        // it spawned with one signal to the group rather than one process.
        ::setpgid(0, 0);
#ifdef __linux__
        // THE CHILD CANNOT OUTLIVE THE PARENT - see the header. This is the
        // Linux equivalent of the Windows job object with
        // KILL_ON_JOB_CLOSE: if the thread that called fork() exits (process
        // death or otherwise) before this child does, the kernel SIGKILLs
        // it. PR_SET_PDEATHSIG has no portable POSIX equivalent, which is why
        // this is guarded by __linux__ rather than the general #else.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        if (nullFd >= 0) { ::dup2(nullFd, STDIN_FILENO); }
        ::dup2(pipeFds[1], STDOUT_FILENO);
        // stderr is left exactly as inherited - deliberately, matching the
        // Windows side: UHD's discovery errors keep reaching the same place
        // they always did (a console, or nowhere in a windowed run).
        ::execv(argv[0], argv.data());
        ::_exit(127);  // exec failed; unreachable otherwise
    }

    // PARENT. The write end must close now, whether or not anything below
    // succeeds: while it stays open the pipe has a writer and the drain
    // thread would wait for end-of-file forever.
    ::close(pipeFds[1]);
    if (nullFd >= 0) { ::close(nullFd); }
    out.attempts += 1;

    // DRAINED ON ITS OWN THREAD, not after the wait - see the Windows side
    // for why: a full pipe would otherwise deadlock against a child that is
    // itself blocked in write() while this thread sits in waitpid().
    std::string text;
    std::thread drain([rd = pipeFds[0], &text] {
        char buf[4096];
        for (;;) {
            const ssize_t got = ::read(rd, buf, sizeof(buf));
            if (got <= 0) { break; }
            text.append(buf, static_cast<std::size_t>(got));
        }
    });

    // BOUNDED WITH A POLLING WAIT rather than a blocking waitpid(), because
    // POSIX has no "wait with a timeout" call: sigtimedwait() needs SIGCHLD
    // blocked process-wide (this is a library function other threads share),
    // and a self-pipe adds a second file descriptor and signal handler for
    // the same result this loop gets directly. 5 ms polling on a 20 s budget
    // costs nothing a caller would notice.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int status = 0;
    bool exited = false;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exited = true;
            break;
        }
        if (r < 0 && errno != EINTR) { break; }  // ECHILD or similar: give up
        if (std::chrono::steady_clock::now() >= deadline) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!exited) {
        // Kill the whole group - see THE CHILD CANNOT OUTLIVE THE PARENT and
        // ITS OWN PROCESS GROUP above - then reap it. SIGKILL cannot be
        // caught or blocked, so this wait is bounded even against a wedged
        // vendor driver.
        ::killpg(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        out.outcome = EnumOutcome::ChildTimedOut;
    }

    drain.join();
    ::close(pipeFds[0]);

    unsigned long exitCode = 0;
    if (WIFEXITED(status)) {
        exitCode = static_cast<unsigned long>(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        // THE POSIX READING of the exit code Windows reports as an NTSTATUS
        // exception (0xC0000005 and friends): there is no equivalent numeric
        // space on Linux, so this uses the convention every POSIX shell
        // already reports a signal death as - 128 + the signal number - by
        // which SIGSEGV (11) is 139. It is a real, well-known number rather
        // than one invented for this file.
        exitCode = 128ul + static_cast<unsigned long>(WTERMSIG(status));
    }
    out.exitCode = exitCode;

    if (out.outcome == EnumOutcome::ChildTimedOut) { return; }
    if (exitCode != 0) {
        out.outcome = EnumOutcome::ChildDied;
        return;
    }
    if (!parseChildOutput(text, out)) {
        out.devices.clear();
        out.outcome = EnumOutcome::Malformed;
        return;
    }
    out.outcome = EnumOutcome::Ok;
#endif
}

}  // namespace

const char* enumOutcomeName(EnumOutcome outcome) noexcept {
    switch (outcome) {
        case EnumOutcome::Ok: return "ok";
        case EnumOutcome::ChildDied: return "child-died";
        case EnumOutcome::ChildTimedOut: return "child-timed-out";
        case EnumOutcome::SpawnFailed: return "spawn-failed";
        case EnumOutcome::Malformed: return "malformed";
    }
    return "?";
}

std::string enumerateHelperPath() {
    if (const char* over = std::getenv("CASCADE_ENUM_HELPER")) {
        if (*over != '\0') { return std::string(over); }
    }
#ifdef _WIN32
    const std::wstring dir = exeDirW();
    if (dir.empty()) { return std::string(); }
    return narrow(dir + L"cascade.exe");
#else
    // THE RUNNING EXECUTABLE'S DIRECTORY, Linux-style: /proc/self/exe is
    // always a symlink to the binary that is actually running, argv[0]
    // notwithstanding (a relative path, a PATH lookup, an exec*() that lied).
    // The shipping case is "cascade" beside itself, exactly as the Windows
    // side is "cascade.exe" beside itself - see CMakeLists.txt's
    // add_executable(cascade ...), which carries no suffix on this platform.
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) { return std::string(); }
    const std::string exe(buf, static_cast<std::size_t>(n));
    const std::size_t slash = exe.find_last_of('/');
    if (slash == std::string::npos) { return std::string(); }
    return exe.substr(0, slash + 1) + "cascade";
#endif
}

// ONE CHILD PER DRIVER, and only once the whole-bus probe has died every time.
//
// Field report 650B88A1 (0.99.6 on Windows 10.0.26200, and 0.96.3 before it):
// a libusb-based module faulted during discovery on EVERY probe of one
// machine, so both children died, the scan listed nothing, and the Source
// menu told a user with a radio plugged in that there were no radios. The
// retry cannot help a fault that is deterministic - only asking the other
// drivers separately can.
//
// The list of drivers comes from a child too (loading a module can fault as
// surely as probing with it), so a machine where even that dies is left
// exactly where it was before: no devices, and a logged reason.
//
// Each driver gets ONE child: a driver that faults faults every time here by
// construction, and this path is already the slow one.
void sweepEachDriver(const std::string& helper, const EnumOptions& options,
                     const std::string& crashDir, EnumResult& result) {
    EnumResult listing;
    runOneChild(helper, options.timeoutMs, crashDir, listing, "--list-drivers");
    result.sweepChildren += listing.attempts;
    if (listing.outcome != EnumOutcome::Ok || listing.drivers.empty()) {
        core::diagWarnf(
            "soapy: every whole-bus probe died and the driver list could not be read "
            "either - no devices listed this scan");
        return;
    }

    result.sweptPerDriver = true;
    result.sweptDrivers = listing.drivers;
    std::vector<SoapyDeviceInfo> found;
    for (const std::string& driver : listing.drivers) {
        EnumResult one;
        runOneChild(helper, options.timeoutMs, crashDir, one, "--driver=" + driver);
        result.sweepChildren += one.attempts;
        if (one.outcome == EnumOutcome::Ok) {
            for (SoapyDeviceInfo& d : one.devices) { found.push_back(std::move(d)); }
            continue;
        }
        if (one.outcome == EnumOutcome::ChildDied) {
            result.childDeaths += 1;
            result.deathExitCode = one.exitCode;
            // FILED PER DRIVER, because the driver NAME is the one thing the
            // whole-bus death could never say and the only thing that tells a
            // user which install to fix.
            core::reportAbsorbedChildFault(
                "SDR device enumeration child process died probing one driver "
                "(contained: every other driver was still probed)",
                one.exitCode, 1);
        }
        result.faultedDrivers.push_back(driver);
        core::diagWarnf(
            "soapy: the '%s' driver faulted during discovery (exit 0x%08lX) - it is "
            "skipped for this scan; every other driver was still asked",
            driver.c_str(), one.exitCode);
    }

    result.devices = std::move(found);
    // Ok even when some drivers faulted: the list is the honest answer for the
    // drivers that worked, and the ones that did not are named in the log and
    // in faultedDrivers. Only a sweep that produced nothing at all stays a
    // death, so the caller's "no devices" message is still reached.
    if (!result.devices.empty() ||
        result.faultedDrivers.size() < listing.drivers.size()) {
        result.outcome = EnumOutcome::Ok;
        result.exitCode = 0;
        core::diagWarnf(
            "soapy: the whole-bus scan died, so each driver was asked separately - "
            "%d driver(s) asked, %d faulted, %d device(s) listed",
            static_cast<int>(listing.drivers.size()),
            static_cast<int>(result.faultedDrivers.size()),
            static_cast<int>(result.devices.size()));
    }
}

EnumResult enumerateIsolated(const EnumOptions& options) {
    const auto t0 = std::chrono::steady_clock::now();
    EnumResult result;

    const std::string helper =
        options.helperPath.empty() ? enumerateHelperPath() : options.helperPath;

    // Only ever the parent's own armed directory, and empty when the user has
    // diagnostics off - so the child's capture is exactly the parent's consent.
    const std::string childCrashDir = core::activeCrashDir();

    if (!helper.empty()) {
        const int maxAttempts = (options.attempts > 0) ? options.attempts : 1;
        for (int i = 0; i < maxAttempts; ++i) {
            EnumResult attempt;
            attempt.attempts = result.attempts;
            // Carried across attempts on purpose: a retry that SUCCEEDS must
            // not erase the death that made it necessary.
            attempt.childDeaths = result.childDeaths;
            attempt.deathExitCode = result.deathExitCode;
            runOneChild(helper, options.timeoutMs, childCrashDir, attempt);
            result = attempt;
            // Only a DEATH is worth another child; see EnumOptions::attempts
            // for why a timeout is not.
            if (result.outcome != EnumOutcome::ChildDied) { break; }
            result.childDeaths += 1;
            result.deathExitCode = result.exitCode;

            // FILED HERE, AT THE DEATH, and not once at the end - which is
            // where it used to be and where it was very nearly useless.
            //
            // The overwhelmingly common shape of this fault is "first child
            // died, the retry worked": at the 2.5%-per-child rate measured on
            // this bench, roughly forty of every forty-one occurrences end
            // that way. Reporting only when EVERY attempt died meant those
            // forty left a diagnostics line and nothing else - and
            // core/crash_upload.cpp forwards report FILES only, never the
            // diagnostics log, so they reached nobody. The success of the
            // containment is exactly what made the fault invisible.
            // THE CHILD ENTRY POINT, and the distinction is load bearing. This
            // call happens on whatever thread ran the scan - a std::async/PPL
            // worker, in the reports - and the general reportAbsorbedFault
            // would walk THAT thread's stack for want of an exception context.
            // It did, twice, and the second time (0.96.3) it killed the parent
            // the containment had just saved: a stack overflow on a nearly
            // spent worker stack, which no __try can catch. The frames of the
            // thread that NOTICED a child die describe the noticing; the exit
            // code and the attempt number are the fault.
            core::reportAbsorbedChildFault(
                "SDR device enumeration child process died (contained: the parent "
                "survived and re-probed)",
                result.exitCode, i + 1);

            if (i + 1 < maxAttempts) {
                core::diagWarnf(
                    "soapy: enumeration child died with exit 0x%08lX (attempt %d of %d) - "
                    "this is the known libusb fault, contained; retrying",
                    result.exitCode, i + 1, maxAttempts);
            }
        }
        // EVERY ATTEMPT DIED. One bad driver must not hide the rest.
        if (result.outcome == EnumOutcome::ChildDied && options.perDriverSweep) {
            sweepEachDriver(helper, options, childCrashDir, result);
        }
    } else {
        result.outcome = EnumOutcome::SpawnFailed;
    }

    if (result.outcome == EnumOutcome::SpawnFailed && options.allowInProcessFallback) {
        core::diagWarnf(
            "soapy: no enumeration helper could be started ('%s') - walking the bus "
            "in-process instead, which is not crash-isolated",
            helper.c_str());
        result.devices = SoapySource::enumerateInProcess();
        result.fellBackInProcess = true;
    }

    // EACH OUTCOME LOGGED DIFFERENTLY, because to a user they mean opposite
    // things and used to be one silent "no devices".
    switch (result.outcome) {
        case EnumOutcome::Ok:
            // LOGGED EVEN WHEN THE ANSWER IS GOOD. A retry that worked is
            // still a driver fault that happened, and a machine losing one
            // scan in twenty to one is worth a line - otherwise the only trace
            // of the whole problem is that it stopped being a crash.
            if (result.childDeaths > 0) {
                core::diagWarnf(
                    "soapy: enumeration succeeded after %d child death(s) (last exit "
                    "0x%08lX) - a driver on this machine is faulting during device "
                    "discovery",
                    result.childDeaths, result.deathExitCode);
            }
            break;
        case EnumOutcome::ChildDied:
            // The report for each death was already filed in the retry loop
            // above, at the moment it happened - deliberately, because the
            // case that reaches HERE (every attempt died) is the rare one.
            core::diagWarnf(
                "soapy: enumeration failed - every child died (last exit 0x%08lX, %d "
                "attempts); no devices listed this scan",
                result.exitCode, result.attempts);
            break;
        case EnumOutcome::ChildTimedOut:
            core::diagWarnf(
                "soapy: enumeration timed out after %lu ms and the child was killed - "
                "a device on this machine is not answering its probe",
                options.timeoutMs);
            break;
        case EnumOutcome::Malformed:
            core::diagWarnf(
                "soapy: enumeration child exited cleanly but its answer did not parse - "
                "helper '%s' does not match this build",
                helper.c_str());
            break;
        case EnumOutcome::SpawnFailed:
            // Already logged above if it fell back; a SpawnFailed with the
            // fallback disabled is a caller that asked for exactly this.
            break;
    }

    result.elapsedMs = static_cast<unsigned long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              t0)
            .count());
    return result;
}

#ifdef _WIN32
namespace {
// The helper's own last resort. It must die FAST and QUIETLY: this process is
// expected to fault occasionally by design, and a Windows Error Reporting
// round trip on every one would cost seconds and leave dumps behind. The exit
// code is the exception code, which is what the parent logs.
LONG WINAPI quietDeath(EXCEPTION_POINTERS* ep) {
    const DWORD code = (ep != nullptr && ep->ExceptionRecord != nullptr)
                           ? ep->ExceptionRecord->ExceptionCode
                           : 0xE0000001ul;
    ::TerminateProcess(::GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace
#endif

void armEnumerateHelperProcess(const char* crashDir) {
#ifdef _WIN32
    // FIRST, and unconditionally: whichever way this process dies, it must do
    // it without a Windows Error Reporting round trip. A helper is expected to
    // fault occasionally by design, and a dialog on a hidden process is a hang
    // that nobody can see to dismiss.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif

    if (crashDir != nullptr && *crashDir != '\0') {
        // THE ORDINARY HANDLERS, into the directory the parent already armed.
        // exitAfterReport is what keeps the death fast and quiet: the filter
        // writes the report and TerminateProcesses with the exception code,
        // so the parent still reads 0xC0000005 and WER still never runs -
        // but a fault in cascade's own code on this path is once again a
        // symbolised, uploadable report instead of an exit code. On Linux
        // the same config arms crash_handler_posix.cpp's signal handlers:
        // the report is written, the process _exits with the signal number
        // and the parent reads 128 + signal, exactly as an unhandled death
        // would have read. This block was Windows-only until 0.97.0, which
        // is why a Linux helper reported capture as never armed.
        //
        // No minidump, ever, from here: a dump is process memory and the user
        // consented to reports, not to memory, on a process they cannot see.
        core::CrashHandlerConfig cfg;
        cfg.crashDir = crashDir;
        cfg.enabled = true;
        cfg.minidump = false;
        cfg.exitAfterReport = true;
        core::installCrashHandlers(cfg);
        return;
    }
#ifdef _WIN32
    // Diagnostics off. Nothing is written anywhere; the process just goes.
    ::SetUnhandledExceptionFilter(&quietDeath);
#endif
    // On Linux with diagnostics off there is nothing to install: the default
    // signal disposition already dies quietly and the parent reads the signal.
}

std::string enumerationReportJson(bool runtimeAvailable,
                                  unsigned long long guardedCalls,
                                  bool captureArmed,
                                  const std::vector<SoapyDeviceInfo>& devices,
                                  const std::vector<std::string>& drivers) {
    nlohmann::json j;
    j[kKeySchema] = kSchema;
    j[kKeyRuntime] = runtimeAvailable;
    j[kKeyGuarded] = guardedCalls;
    // REPORTED, not assumed. "The parent passed a directory" and "the child is
    // actually able to write a report into it" are different claims - the
    // second one is what a test has to be able to check, and a directory that
    // could not be created turns the first into a lie.
    j[kKeyCapture] = captureArmed;
    if (!drivers.empty()) {
        j[kKeyDrivers] = nlohmann::json::array();
        for (const std::string& d : drivers) { j[kKeyDrivers].push_back(d); }
    }
    j[kKeyDevices] = nlohmann::json::array();
    for (const SoapyDeviceInfo& d : devices) {
        nlohmann::json e;
        e[kKeyLabel] = d.label;
        e[kKeyArgs] = d.args;
        j[kKeyDevices].push_back(std::move(e));
    }
    // REPLACE, NEVER THROW, and this is not defensive decoration: it is the
    // fix for a crash that was misdiagnosed for weeks.
    //
    // dump() validates UTF-8 and throws type_error.316 on a byte sequence it
    // cannot encode. Everything in this object is THIRD-PARTY TEXT - a device
    // label, driver name and argument string handed over by a vendor SoapySDR
    // module - so a radio whose descriptor carries one non-UTF-8 byte made
    // this throw, and nothing in the child catches it. The child then died
    // with 0xE06D7363, and the parent logged that exit as "this is the known
    // libusb fault, contained", which is what hid it: an ordinary encoding
    // fault in our own serialiser, reported as somebody else's memory bug.
    //
    // The rest of this application already learned this. telemetry.cpp and
    // web_server.cpp both pass error_handler_t::replace, the latter because
    // an unserialisable plugin name once took the whole browser UI down. This
    // was the third site and the only one still throwing.
    const std::string line = j.dump(-1, ' ', false,
                                    nlohmann::json::error_handler_t::replace);
    return line;
}

int runEnumerateHelper(const char* crashDir, const char* driver, bool listDrivers) {
    armEnumerateHelperProcess(crashDir);
    const bool captureArmed = !core::activeCrashDir().empty();
#ifdef _WIN32
    // Binary stdout: the one line below must reach the parent byte for byte,
    // with no CRLF translation between a writer and a reader that both count
    // on exactly what was written.
    ::_setmode(::_fileno(stdout), _O_BINARY);
#endif

    const std::uint64_t before = vendorGuardCallCount();
    const bool runtime = SoapySource::runtimeAvailable();
    // THREE JOBS, ONE CHILD. The names-only answer exists so the parent can
    // sweep drivers separately after a whole-bus death; the restricted walk is
    // that sweep. Both are otherwise the ordinary walk, guard and all.
    const std::vector<std::string> names =
        listDrivers ? SoapySource::driverNames() : std::vector<std::string>();
    const std::vector<SoapyDeviceInfo> devices =
        listDrivers ? std::vector<SoapyDeviceInfo>()
                    : SoapySource::enumerateInProcess(
                          (driver != nullptr) ? std::string(driver) : std::string());

    const std::string line = enumerationReportJson(
        runtime, static_cast<unsigned long long>(vendorGuardCallCount() - before),
        captureArmed, devices, names);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

}  // namespace cascade::source
