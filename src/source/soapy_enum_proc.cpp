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
                 const std::string& crashDir, EnumResult& out) {
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
    (void)helper;
    (void)timeoutMs;
    (void)crashDir;
    out.outcome = EnumOutcome::SpawnFailed;
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
    return std::string();
#endif
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
            core::reportAbsorbedFault(
                "SDR device enumeration child process died (contained: the parent "
                "survived and re-probed)",
                result.exitCode, nullptr, nullptr);

            if (i + 1 < maxAttempts) {
                core::diagWarnf(
                    "soapy: enumeration child died with exit 0x%08lX (attempt %d of %d) - "
                    "this is the known libusb fault, contained; retrying",
                    result.exitCode, i + 1, maxAttempts);
            }
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

    if (crashDir != nullptr && *crashDir != '\0') {
        // THE ORDINARY HANDLERS, into the directory the parent already armed.
        // exitAfterReport is what keeps the death fast and quiet: the filter
        // writes the report and TerminateProcesses with the exception code,
        // so the parent still reads 0xC0000005 and WER still never runs -
        // but a fault in cascade's own code on this path is once again a
        // symbolised, uploadable report instead of an exit code.
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
    // Diagnostics off. Nothing is written anywhere; the process just goes.
    ::SetUnhandledExceptionFilter(&quietDeath);
#else
    (void)crashDir;
#endif
}

std::string enumerationReportJson(bool runtimeAvailable,
                                  unsigned long long guardedCalls,
                                  bool captureArmed,
                                  const std::vector<SoapyDeviceInfo>& devices) {
    nlohmann::json j;
    j[kKeySchema] = kSchema;
    j[kKeyRuntime] = runtimeAvailable;
    j[kKeyGuarded] = guardedCalls;
    // REPORTED, not assumed. "The parent passed a directory" and "the child is
    // actually able to write a report into it" are different claims - the
    // second one is what a test has to be able to check, and a directory that
    // could not be created turns the first into a lie.
    j[kKeyCapture] = captureArmed;
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

int runEnumerateHelper(const char* crashDir) {
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
    const std::vector<SoapyDeviceInfo> devices = SoapySource::enumerateInProcess();

    const std::string line = enumerationReportJson(
        runtime, static_cast<unsigned long long>(vendorGuardCallCount() - before),
        captureArmed, devices);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    return 0;
}

}  // namespace cascade::source
