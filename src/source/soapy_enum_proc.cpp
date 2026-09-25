// See source/soapy_enum_proc.hpp for why the device walk runs in a child
// process at all, and for the measurements behind the timeout and the retry.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_enum_proc.hpp"

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "source/vendor_guard.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
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

// Defined with the probe log's writer, below.
std::vector<std::string> probesStillRunning(const std::string& text);

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

    if (out.outcome == EnumOutcome::ChildTimedOut) {
        out.inFlightDrivers = probesStillRunning(text);
        out.childFaultLine = childFaultLineFrom(text);
        return;
    }
    if (exitCode != 0) {
        out.outcome = EnumOutcome::ChildDied;
        out.inFlightDrivers = probesStillRunning(text);
        out.childFaultLine = childFaultLineFrom(text);
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

    if (out.outcome == EnumOutcome::ChildTimedOut) {
        out.inFlightDrivers = probesStillRunning(text);
        out.childFaultLine = childFaultLineFrom(text);
        return;
    }
    if (exitCode != 0) {
        out.outcome = EnumOutcome::ChildDied;
        out.inFlightDrivers = probesStillRunning(text);
        out.childFaultLine = childFaultLineFrom(text);
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

std::string lowerAscii(std::string s);

namespace {

// THE SESSION'S DO-NOT-ASK LIST. Filled only by a PER-DRIVER child dying, so
// every name on it has already killed a process of its own with no other
// driver running beside it; a whole-bus death blames nobody (every driver
// probes at once there). Read by every scan and by the device panel's note.
std::mutex& sessionMutex() {
    static std::mutex m;
    return m;
}

std::vector<FaultedDriver>& sessionList() {
    static std::vector<FaultedDriver> list;
    return list;
}

void rememberFaultedDriver(const std::string& driver, unsigned long exitCode) {
    const std::string name = lowerAscii(driver);
    std::lock_guard<std::mutex> lk(sessionMutex());
    for (const FaultedDriver& f : sessionList()) {
        if (f.driver == name) { return; }
    }
    sessionList().push_back(FaultedDriver{name, exitCode});
}

std::vector<std::string> sessionFaultedNames() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(sessionMutex());
    for (const FaultedDriver& f : sessionList()) { out.push_back(f.driver); }
    return out;
}

// THE SESSION'S TOO-SLOW-BESIDE-A-RADIO LIST (bug hunt 2026-09-24). Filled
// only by a per-driver child in a scan beside an open radio that ran out its
// WHOLE budget; read only by later such scans, so a wedged driver is waited
// out once rather than on every Refresh while the radio stays open. Not a
// crash, so it is not on the list above (whose panel note says "crashed"), and
// the whole-bus scan still asks it - and a whole-bus scan that answers in time
// clears it, because every driver answered that one.
std::vector<std::string>& sessionSlowList() {
    static std::vector<std::string> list;
    return list;
}

void rememberSlowDriver(const std::string& driver) {
    const std::string name = lowerAscii(driver);
    std::lock_guard<std::mutex> lk(sessionMutex());
    auto& list = sessionSlowList();
    if (std::find(list.begin(), list.end(), name) == list.end()) { list.push_back(name); }
}

std::vector<std::string> sessionSlowNames() {
    std::lock_guard<std::mutex> lk(sessionMutex());
    return sessionSlowList();
}

void forgetSlowDrivers() {
    std::lock_guard<std::mutex> lk(sessionMutex());
    sessionSlowList().clear();
}

// The --skip= argument: only names a command line can carry unquoted and
// unsplit, which every SoapySDR driver key is. Anything else is left off the
// list rather than risking a mangled argument - it is then asked, which is
// what every build before this one did.
bool skippableName(const std::string& s) {
    if (s.empty()) { return false; }
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) { return false; }
    }
    return true;
}

// A driver name as it may appear in a REPORT LINE: the name comes from a
// third-party module, and a report is "name: value" lines - one newline in a
// registry key would split the reason and forge a field. Lower-cased, with
// anything outside the driver-key alphabet shown as '?'.
std::string reportSafeName(const std::string& driver) {
    std::string out = lowerAscii(driver);
    for (char& c : out) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) { c = '?'; }
    }
    if (out.size() > 48) { out.resize(48); }
    return out;
}

constexpr const char* kProbeMarker = "cascade-probe: ";

// WHICH PROBES WERE STILL RUNNING, read off the child's probe log (see
// probeMarkerLine) - begun and never ended, in the order they began. The
// marker is looked for ANYWHERE in a line, not only at its start: a vendor
// module printing to stdout without a newline would otherwise glue its text
// to the front of ours and hide it.
std::vector<std::string> probesStillRunning(const std::string& text) {
    std::vector<std::string> running;
    std::size_t pos = 0;
    const std::string marker(kProbeMarker);
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        pos += marker.size();
        const std::size_t eol = text.find_first_of("\r\n", pos);
        const std::string rest =
            text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
        const std::size_t sp = rest.find(' ');
        if (sp == std::string::npos) { continue; }
        const std::string verb = rest.substr(0, sp);
        const std::string name = reportSafeName(rest.substr(sp + 1));
        if (name.empty()) { continue; }
        const auto it = std::find(running.begin(), running.end(), name);
        if (verb == "begin" && it == running.end()) {
            running.push_back(name);
        } else if (verb == "end" && it != running.end()) {
            running.erase(it);
        }
    }
    return running;
}

// The child's fault line as a reason suffix. Capped harder than the field
// itself: the site keeps 200 characters of a reason, and the base sentence
// (which names the driver) is the half that must survive.
std::string childSaid(const std::string& line) {
    if (line.empty()) { return std::string(); }
    constexpr std::size_t kMostInReason = 72;
    return " - child: " + line.substr(0, kMostInReason);
}

std::string joinNames(const std::vector<std::string>& names, std::size_t most) {
    std::string out;
    for (std::size_t i = 0; i < names.size() && i < most; ++i) {
        out += (i == 0 ? "" : ", ") + names[i];
    }
    if (names.size() > most) { out += " and " + std::to_string(names.size() - most) + " more"; }
    return out;
}

}  // namespace

std::vector<FaultedDriver> sessionFaultedDrivers() {
    std::lock_guard<std::mutex> lk(sessionMutex());
    return sessionList();
}

void clearSessionFaultedDriversForTest() {
    std::lock_guard<std::mutex> lk(sessionMutex());
    sessionList().clear();
    sessionSlowList().clear();
}

std::string childFaultSignatureTag(const std::string& driver) {
    if (driver.empty()) { return "enumerate-child:whole-bus"; }
    return "enumerate-child:driver=" + reportSafeName(driver);
}

std::string probeMarkerLine(bool begin, const std::string& driver) {
    return std::string(kProbeMarker) + (begin ? "begin " : "end ") + driver + "\n";
}

std::string childFaultLineFrom(const std::string& childStdout) {
    // Found ANYWHERE in a line, like the probe markers: a vendor printf with
    // no newline must not hide it. At most two lines (the fault, then why the
    // handler could not finish), printable ASCII only - this text goes into a
    // report line, where a newline would forge a field - and capped, because
    // the site clips a reason at 200 characters and the driver name must not
    // be what gets cut.
    constexpr std::size_t kMostLines = 2;
    constexpr std::size_t kMostChars = 160;
    const std::string prefix(core::kFaultLinePrefix);
    std::string out;
    std::size_t lines = 0;
    std::size_t pos = 0;
    while (lines < kMostLines && (pos = childStdout.find(prefix, pos)) != std::string::npos) {
        pos += prefix.size();
        const std::size_t eol = childStdout.find_first_of("\r\n", pos);
        const std::size_t end = (eol == std::string::npos) ? childStdout.size() : eol;
        std::string line;
        for (std::size_t i = pos; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(childStdout[i]);
            line.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
        }
        pos = end;
        if (line.empty()) { continue; }
        out += (out.empty() ? "" : "; ") + line;
        ++lines;
    }
    if (out.size() > kMostChars) { out.resize(kMostChars); }
    return out;
}

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
std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

// THE SAME WALK SERVES TWO CALLERS. After a whole-bus death (skipDrivers
// empty, `restricted` false) it asks every driver. While radios are open
// (`restricted` true) it is the ONLY walk: options.skipDrivers are left out,
// because their probe would open a device this process is streaming from.
//
// ONE BUDGET FOR THE WALK BESIDE AN OPEN RADIO (bug hunt 2026-09-24). The
// children run one after another, and each used to get the full timeoutMs of
// its own, so N wedged drivers cost N budgets back to back while the panel
// said "Scanning...". Beside a radio the whole walk - the listing included -
// is now bounded by the ordinary scan's own worst case, attempts x timeoutMs;
// a child gets what is left of it (never more than timeoutMs), and a driver
// whose turn comes after it is spent is named in outOfTimeDrivers, not asked.
// A driver that ran out its WHOLE timeoutMs is remembered (sessionSlowList)
// and not asked beside a radio again this session. The sweep after a
// whole-bus death is unchanged: it exists to find the culprit, and cutting it
// short could leave the culprit unnamed.
void sweepEachDriver(const std::string& helper, const EnumOptions& options,
                     const std::string& crashDir, EnumResult& result, bool restricted) {
    using Clock = std::chrono::steady_clock;
    const unsigned long budgetMs =
        options.timeoutMs * static_cast<unsigned long>(options.attempts > 0 ? options.attempts : 1);
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(budgetMs);
    // A child needs time to start and load the modules before it can answer
    // at all; one given less than this would only be killed.
    constexpr unsigned long kMinChildMs = 250;

    EnumResult listing;
    runOneChild(helper, options.timeoutMs, crashDir, listing, "--list-drivers");
    result.sweepChildren += listing.attempts;
    if (listing.outcome != EnumOutcome::Ok || listing.drivers.empty()) {
        if (restricted) {
            // Nothing was probed, so nothing was risked: say so and give the
            // listing's own outcome, which is the honest answer.
            result.outcome = listing.outcome == EnumOutcome::Ok ? EnumOutcome::Ok
                                                                : listing.outcome;
            result.exitCode = listing.exitCode;
            core::diagWarnf("soapy: the driver list could not be read for a scan beside an "
                            "open radio - no devices listed this scan");
            return;
        }
        core::diagWarnf(
            "soapy: every whole-bus probe died and the driver list could not be read "
            "either - no devices listed this scan");
        return;
    }

    std::vector<std::string> skip;
    for (const std::string& s : options.skipDrivers) { skip.push_back(lowerAscii(s)); }
    std::vector<std::string> absent;
    for (const std::string& s : options.absentDrivers) { absent.push_back(lowerAscii(s)); }
    // A driver that already killed a child of its own this session is not
    // asked again, beside an open radio or after a whole-bus death alike.
    const std::vector<std::string> faultedBefore = sessionFaultedNames();
    const std::vector<std::string> slowBefore =
        restricted ? sessionSlowNames() : std::vector<std::string>();
    std::vector<std::string> asked;
    for (const std::string& d : listing.drivers) {
        const std::string low = lowerAscii(d);
        if (std::find(skip.begin(), skip.end(), low) != skip.end()) {
            result.skippedDrivers.push_back(low);
        } else if (std::find(absent.begin(), absent.end(), low) != absent.end()) {
            // Nothing of its family is here to find (EnumOptions::absentDrivers).
            if (std::find(result.absentDrivers.begin(), result.absentDrivers.end(), low) ==
                result.absentDrivers.end()) {
                result.absentDrivers.push_back(low);
            }
        } else if (std::find(faultedBefore.begin(), faultedBefore.end(), low) !=
                   faultedBefore.end()) {
            // Already listed when the whole-bus child was told to skip it.
            if (std::find(result.sessionSkippedDrivers.begin(), result.sessionSkippedDrivers.end(),
                          low) == result.sessionSkippedDrivers.end()) {
                result.sessionSkippedDrivers.push_back(low);
            }
        } else if (std::find(slowBefore.begin(), slowBefore.end(), low) != slowBefore.end()) {
            result.sessionSlowDrivers.push_back(low);
        } else {
            asked.push_back(d);
        }
    }
    if (!result.sessionSkippedDrivers.empty()) {
        core::diagWarnf(
            "soapy: not asking %s - it crashed a device scan earlier in this session",
            joinNames(result.sessionSkippedDrivers, 8).c_str());
    }
    if (!result.sessionSlowDrivers.empty()) {
        core::diagWarnf(
            "soapy: not asking %s beside an open radio - it did not answer within %lu ms "
            "earlier in this session",
            joinNames(result.sessionSlowDrivers, 8).c_str(), options.timeoutMs);
    }

    result.sweptPerDriver = true;
    std::vector<SoapyDeviceInfo> found;
    for (std::size_t i = 0; i < asked.size(); ++i) {
        const std::string& driver = asked[i];
        unsigned long childMs = options.timeoutMs;
        if (restricted) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now())
                    .count();
            if (left < static_cast<long long>(kMinChildMs)) {
                for (std::size_t j = i; j < asked.size(); ++j) {
                    result.outOfTimeDrivers.push_back(asked[j]);
                }
                core::diagWarnf(
                    "soapy: the scan beside an open radio spent its %lu ms before asking %s - "
                    "not asked this scan",
                    budgetMs, joinNames(result.outOfTimeDrivers, 8).c_str());
                break;
            }
            if (static_cast<unsigned long long>(left) < childMs) {
                childMs = static_cast<unsigned long>(left);
            }
        }
        result.sweptDrivers.push_back(driver);
        EnumResult one;
        runOneChild(helper, childMs, crashDir, one, "--driver=" + driver);
        result.sweepChildren += one.attempts;
        if (restricted && one.outcome == EnumOutcome::ChildTimedOut &&
            childMs == options.timeoutMs) {
            // Its WHOLE budget, and no answer: waited out once is enough.
            rememberSlowDriver(driver);
        }
        if (one.outcome == EnumOutcome::Ok) {
            for (SoapyDeviceInfo& d : one.devices) { found.push_back(std::move(d)); }
            continue;
        }
        // The most recent death's or kill's own words, as the header promises.
        result.childFaultLine = one.childFaultLine;
        if (one.outcome == EnumOutcome::ChildDied) {
            result.childDeaths += 1;
            result.deathExitCode = one.exitCode;
            // FILED PER DRIVER, NAMED, AND UNDER ITS OWN SIGNATURE, because
            // the driver name is the one thing the whole-bus death could never
            // say and the only thing that tells a user which install to fix.
            // Until 0.99.33 this report said "one driver" without saying
            // which, and hashed to the same signature as the whole-bus death
            // filed moments before it - so the uploader dropped it as a
            // duplicate and the name never left the machine.
            // AND WHAT THE CHILD SAID IT DIED OF (F204602B5329B268), when its
            // handler got that far: the code and module of the fault itself,
            // which the exit code alone cannot give when the handler could
            // not finish.
            const std::string reason =
                "SDR device enumeration child process died probing driver=" +
                reportSafeName(driver) + " (contained: every other driver was still probed)" +
                childSaid(one.childFaultLine);
            core::reportAbsorbedChildFault(reason.c_str(), one.exitCode, 1,
                                           childFaultSignatureTag(driver).c_str());
            // Deterministic by now: the driver died with nothing else running
            // in its process. Asking it again on every Refresh only costs a
            // crash each time.
            rememberFaultedDriver(driver, one.exitCode);
        }
        result.faultedDrivers.push_back(driver);
        if (one.outcome == EnumOutcome::ChildTimedOut) {
            core::diagWarnf(
                "soapy: the '%s' driver did not answer within %lu ms and was stopped - it is "
                "skipped for this scan",
                driver.c_str(), childMs);
        } else {
            core::diagWarnf(
                "soapy: the '%s' driver faulted during discovery (exit 0x%08lX) - it is "
                "skipped for this scan; every other driver was still asked%s%s",
                driver.c_str(), one.exitCode, one.childFaultLine.empty() ? "" : " - the child said: ",
                one.childFaultLine.c_str());
        }
    }

    result.devices = std::move(found);
    if (restricted) {
        // Ok unless every driver asked died: the list is the honest answer for
        // the drivers that could be asked. Leaving every driver out (only the
        // open radios' families are installed) is Ok and empty, not a death.
        const bool allDied = !result.sweptDrivers.empty() &&
                             result.faultedDrivers.size() == result.sweptDrivers.size();
        result.outcome = allDied ? EnumOutcome::ChildDied : EnumOutcome::Ok;
        if (!allDied) { result.exitCode = 0; }
        std::string left;
        for (const std::string& s : result.skippedDrivers) { left += (left.empty() ? "" : ", ") + s; }
        core::diagLogf(
            "soapy: scanned beside an open radio - %d driver(s) asked, %d left out (%s: their "
            "probe would reset the open radio), %d faulted, %d device(s) listed",
            static_cast<int>(result.sweptDrivers.size()),
            static_cast<int>(result.skippedDrivers.size()),
            left.empty() ? "none" : left.c_str(), static_cast<int>(result.faultedDrivers.size()),
            static_cast<int>(result.devices.size()));
        return;
    }
    // Ok even when some drivers faulted: the list is the honest answer for the
    // drivers that worked, and the ones that did not are named in the log and
    // in faultedDrivers. Only a sweep that produced nothing at all stays a
    // death, so the caller's "no devices" message is still reached.
    if (!result.devices.empty() || result.faultedDrivers.size() < result.sweptDrivers.size()) {
        result.outcome = EnumOutcome::Ok;
        result.exitCode = 0;
        core::diagWarnf(
            "soapy: the whole-bus scan died, so each driver was asked separately - "
            "%d driver(s) asked, %d faulted, %d device(s) listed",
            static_cast<int>(result.sweptDrivers.size()),
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

    if (!helper.empty() && !options.skipDrivers.empty()) {
        // BESIDE AN OPEN RADIO: never the whole bus, only the drivers that
        // cannot touch it, each in its own child. See EnumOptions::skipDrivers.
        sweepEachDriver(helper, options, childCrashDir, result, true);
    } else if (!helper.empty()) {
        // THE SESSION'S FAULTED DRIVERS ARE LEFT OUT OF THE WHOLE BUS TOO, by
        // the child itself (runEnumerateHelper's `skip`): one child, as ever,
        // and no Refresh that re-runs a probe known to kill it.
        std::vector<std::string> sessionSkip;
        for (const std::string& d : sessionFaultedNames()) {
            if (skippableName(d)) { sessionSkip.push_back(d); }
        }
        // ...AND SO ARE THE DRIVERS WITH NOTHING HERE TO FIND, through the
        // same argument (EnumOptions::absentDrivers).
        std::vector<std::string> absentSkip;
        for (const std::string& d : options.absentDrivers) {
            const std::string low = lowerAscii(d);
            if (skippableName(low) &&
                std::find(sessionSkip.begin(), sessionSkip.end(), low) == sessionSkip.end() &&
                std::find(absentSkip.begin(), absentSkip.end(), low) == absentSkip.end()) {
                absentSkip.push_back(low);
            }
        }
        std::string skipArg;
        for (const std::string& d : sessionSkip) { skipArg += (skipArg.empty() ? "" : ",") + d; }
        for (const std::string& d : absentSkip) { skipArg += (skipArg.empty() ? "" : ",") + d; }
        if (!sessionSkip.empty()) {
            core::diagWarnf(
                "soapy: not asking %s - it crashed a device scan earlier in this session",
                joinNames(sessionSkip, 8).c_str());
        }
        if (!absentSkip.empty()) {
            core::diagLogf("soapy: not asking %s - no hardware of theirs is on this machine",
                           joinNames(absentSkip, 8).c_str());
        }
        if (!skipArg.empty()) { skipArg = "--skip=" + skipArg; }

        const int maxAttempts = (options.attempts > 0) ? options.attempts : 1;
        for (int i = 0; i < maxAttempts; ++i) {
            EnumResult attempt;
            attempt.attempts = result.attempts;
            // Carried across attempts on purpose: a retry that SUCCEEDS must
            // not erase the death that made it necessary.
            attempt.childDeaths = result.childDeaths;
            attempt.deathExitCode = result.deathExitCode;
            runOneChild(helper, options.timeoutMs, childCrashDir, attempt, skipArg);
            // ...and neither must it erase which probes that death interrupted.
            if (attempt.outcome == EnumOutcome::Ok || attempt.outcome == EnumOutcome::Malformed) {
                attempt.inFlightDrivers = result.inFlightDrivers;
            }
            result = attempt;
            result.sessionSkippedDrivers = sessionSkip;
            result.absentDrivers = absentSkip;
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
            //
            // WHAT IT WAS DOING, since 0.99.33 (field report 91965660116CF497,
            // which could say nothing). Every driver probes at once in this
            // walk, so no single one can be blamed from here - the reason says
            // which probes were still running, and the per-driver sweep that
            // follows a second death is what names a culprit. Its own
            // signature, so it is not one group with every other child death
            // of the same exit code.
            const std::string running =
                result.inFlightDrivers.empty()
                    ? std::string(" - no driver's probe had begun (it died while the driver "
                                  "modules were loading)")
                    : " - every driver probes at once in this walk; still probing when it "
                      "died: " +
                          joinNames(result.inFlightDrivers, 8);
            const std::string reason =
                "SDR device enumeration child process died (contained: the parent "
                "survived and re-probed)" +
                childSaid(result.childFaultLine) + running;
            core::reportAbsorbedChildFault(reason.c_str(), result.exitCode, i + 1,
                                           childFaultSignatureTag(std::string()).c_str());

            if (i + 1 < maxAttempts) {
                core::diagWarnf(
                    "soapy: enumeration child died with exit 0x%08lX (attempt %d of %d)%s - "
                    "contained; retrying",
                    result.exitCode, i + 1, maxAttempts, running.c_str());
            }
        }
        // THE WHOLE BUS ANSWERED IN TIME, every driver on it included (the
        // too-slow-beside-a-radio list is not handed to --skip), so none of
        // them is too slow any more.
        if (result.outcome == EnumOutcome::Ok) { forgetSlowDrivers(); }
        // EVERY ATTEMPT DIED. One bad driver must not hide the rest.
        if (result.outcome == EnumOutcome::ChildDied && options.perDriverSweep) {
            sweepEachDriver(helper, options, childCrashDir, result, false);
        }
    } else {
        result.outcome = EnumOutcome::SpawnFailed;
    }

    // NEVER IN-PROCESS BESIDE AN OPEN RADIO. The in-process walk refuses only
    // while a SoapySDR device is open, and a scan with skipDrivers exists
    // precisely because the open radio may be a NATIVE one it cannot see - the
    // whole-bus walk here would probe that dongle from inside this process.
    if (result.outcome == EnumOutcome::SpawnFailed && options.allowInProcessFallback &&
        options.skipDrivers.empty()) {
        core::diagWarnf(
            "soapy: no enumeration helper could be started ('%s') - walking the bus "
            "in-process instead, which is not crash-isolated",
            helper.c_str());
        // In THIS process a driver that killed a child would kill the session,
        // so the session's faulted drivers are certainly not asked here.
        const std::vector<std::string> faulted = sessionFaultedNames();
        std::vector<std::string> leaveOut = faulted;
        for (const std::string& d : options.absentDrivers) {
            const std::string low = lowerAscii(d);
            if (std::find(leaveOut.begin(), leaveOut.end(), low) == leaveOut.end()) {
                leaveOut.push_back(low);
                result.absentDrivers.push_back(low);
            }
        }
        result.devices = leaveOut.empty() ? SoapySource::enumerateInProcess()
                                          : SoapySource::enumerateInProcessEach(leaveOut, {});
        result.sessionSkippedDrivers = faulted;
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
                "a device on this machine is not answering its probe (still probing: %s)",
                options.timeoutMs,
                result.inFlightDrivers.empty() ? "no driver reported"
                                               : joinNames(result.inFlightDrivers, 8).c_str());
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
        // AND A LINE ON STDOUT, which is the parent's pipe, before the report
        // (F204602B5329B268): the parent's report then says what the child
        // died of even when the child's own report never gets written. See
        // EnumResult::childFaultLine.
        cfg.faultLineToStdout = true;
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

int runEnumerateHelper(const char* crashDir, const char* driver, bool listDrivers,
                       const char* skip) {
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

    // THE WHOLE BUS WRITES ITS PROBE LOG to the parent as it goes (0.99.33):
    // one line as each driver's probe begins and one as it ends, flushed
    // straight into the pipe, so a death leaves the parent the list of probes
    // that were still running. Serialised here because the probes run on a
    // thread each.
    std::vector<std::string> skipList;
    if (skip != nullptr) {
        std::string csv(skip);
        std::size_t start = 0;
        while (start <= csv.size()) {
            const std::size_t comma = csv.find(',', start);
            const std::size_t end = (comma == std::string::npos) ? csv.size() : comma;
            if (end > start) { skipList.push_back(csv.substr(start, end - start)); }
            if (comma == std::string::npos) { break; }
            start = comma + 1;
        }
    }
    std::mutex logMutex;
    const auto onProbe = [&logMutex](bool begin, const std::string& name) {
        const std::string line = probeMarkerLine(begin, name);
        std::lock_guard<std::mutex> lk(logMutex);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    };
    const bool oneDriver = driver != nullptr && *driver != '\0';
    const std::vector<SoapyDeviceInfo> devices =
        listDrivers  ? std::vector<SoapyDeviceInfo>()
        : oneDriver  ? SoapySource::enumerateInProcess(std::string(driver))
                     : SoapySource::enumerateInProcessEach(skipList, onProbe);

    const std::string line = enumerationReportJson(
        runtime, static_cast<unsigned long long>(vendorGuardCallCount() - before),
        captureArmed, devices, names);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

}  // namespace cascade::source
