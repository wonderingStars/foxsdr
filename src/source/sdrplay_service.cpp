// SDRplay API Service state and restart - see sdrplay_service.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_service.hpp"

#include "core/diag_log.hpp"
#include "core/i18n.hpp"

#include <cctype>
#include <cstdio>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace cascade::source {

namespace {

std::string lowerTrimmed(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) { ++b; }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) { --e; }
    std::string out = s.substr(b, e - b);
    for (char& c : out) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return out;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

bool sdrPlayServiceFound(const SdrPlayServiceStatus& s) {
    switch (s.state) {
        case SdrPlayServiceState::NotApplicable:
        case SdrPlayServiceState::QueryFailed:
        case SdrPlayServiceState::NotInstalled: return false;
        default: break;
    }
    return !s.serviceName.empty();
}

bool serviceDisplayNameMatches(const std::string& displayName, const std::string& want) {
    return !lowerTrimmed(want).empty() && lowerTrimmed(displayName) == lowerTrimmed(want);
}

bool sdrPlayServiceDisplayNameMatches(const std::string& displayName) {
    return serviceDisplayNameMatches(displayName, kSdrPlayServiceDisplayName);
}

bool sdrPlayServiceBinaryMatches(const std::string& binaryPath) {
    return serviceBinaryMatches(binaryPath, kSdrPlayServiceBinary);
}

bool serviceBinaryMatches(const std::string& binaryPath, const std::string& exeFile) {
    // lpBinaryPathName is a COMMAND LINE, not a path: the program in quotes
    // when its path has spaces, possibly followed by arguments, or unquoted
    // with none. Take the executable part and compare its file name.
    std::string p = lowerTrimmed(binaryPath);
    if (p.empty()) { return false; }
    std::string exe;
    if (p[0] == '"') {
        const std::size_t close = p.find('"', 1);
        exe = p.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    } else {
        // Unquoted: the executable ends at the first ".exe" (a path without
        // quotes cannot hold a space the loader would not split on anyway).
        const std::size_t dotExe = p.find(".exe");
        exe = (dotExe == std::string::npos) ? p : p.substr(0, dotExe + 4);
    }
    const std::string want = lowerTrimmed(exeFile);
    if (want.empty()) { return false; }
    if (exe == want) { return true; }
    return endsWith(exe, "\\" + want) || endsWith(exe, "/" + want);
}

const char* sdrPlayServiceSentence(const SdrPlayServiceStatus& s) {
    // ENGLISH, deliberately - these are the translation keys. The screen
    // passes them through tr() (sdrPlayServiceAdvice); the log keeps them.
    // FOX_TR_NOOP marks each one for tools/i18n_keys.py and test_i18n.
    switch (s.state) {
        case SdrPlayServiceState::NotApplicable: return "";
        case SdrPlayServiceState::QueryFailed:
        case SdrPlayServiceState::Unknown:
            return FOX_TR_NOOP(
                "FoxSDR could not read the SDRplay API Service's state - restart it in Windows "
                "Services.");
        case SdrPlayServiceState::NotInstalled:
            return FOX_TR_NOOP(
                "The SDRplay API Service is not installed on this computer - install the SDRplay "
                "API from sdrplay.com, then restart FoxSDR.");
        default: break;
    }
    // DISABLED BEFORE STOPPED: net start refuses a disabled service, so the
    // restart key alone cannot help until the startup type is changed.
    if (s.startType == SdrPlayServiceStart::Disabled) {
        return FOX_TR_NOOP(
            "The SDRplay API Service is disabled in Windows Services - set its startup type to "
            "Automatic there, then press RESTART SDRPLAY SERVICE.");
    }
    switch (s.state) {
        case SdrPlayServiceState::Stopped:
            return FOX_TR_NOOP(
                "The SDRplay API Service is stopped - press RESTART SDRPLAY SERVICE to start it.");
        case SdrPlayServiceState::StartPending:
            return FOX_TR_NOOP(
                "The SDRplay API Service is still starting - wait a few seconds, then press "
                "Refresh.");
        case SdrPlayServiceState::Running:
            return FOX_TR_NOOP(
                "The SDRplay API Service is running but did not answer - press RESTART SDRPLAY "
                "SERVICE.");
        default: break;
    }
    // Stop pending, paused, pause/continue pending: a service that is neither
    // running nor stopped and did not answer - the restart's taskkill
    // fallback is for exactly the one stuck stopping.
    return FOX_TR_NOOP(
        "The SDRplay API Service is stuck or paused - press RESTART SDRPLAY SERVICE.");
}

std::string sdrPlayServiceAdvice(const SdrPlayServiceStatus& s) {
    const char* english = sdrPlayServiceSentence(s);
    if (english[0] == '\0') { return std::string(); }
    return cascade::i18n::tr(english);
}

namespace {

const char* stateWord(SdrPlayServiceState s) {
    switch (s) {
        case SdrPlayServiceState::NotApplicable: return "not applicable";
        case SdrPlayServiceState::QueryFailed: return "query failed";
        case SdrPlayServiceState::NotInstalled: return "not installed";
        case SdrPlayServiceState::Stopped: return "stopped";
        case SdrPlayServiceState::StartPending: return "start pending";
        case SdrPlayServiceState::StopPending: return "stop pending";
        case SdrPlayServiceState::Running: return "running";
        case SdrPlayServiceState::ContinuePending: return "continue pending";
        case SdrPlayServiceState::PausePending: return "pause pending";
        case SdrPlayServiceState::Paused: return "paused";
        case SdrPlayServiceState::Unknown: return "unknown";
    }
    return "unknown";
}

const char* startWord(SdrPlayServiceStart s) {
    switch (s) {
        case SdrPlayServiceStart::Unknown: return "start type unknown";
        case SdrPlayServiceStart::Boot: return "boot start";
        case SdrPlayServiceStart::System: return "system start";
        case SdrPlayServiceStart::Auto: return "auto start";
        case SdrPlayServiceStart::Manual: return "manual start";
        case SdrPlayServiceStart::Disabled: return "disabled";
    }
    return "start type unknown";
}

}  // namespace

std::string sdrPlayServiceSummary(const SdrPlayServiceStatus& s) {
    char buf[160];
    switch (s.state) {
        case SdrPlayServiceState::NotApplicable:
        case SdrPlayServiceState::NotInstalled: return stateWord(s.state);
        case SdrPlayServiceState::QueryFailed:
            std::snprintf(buf, sizeof(buf), "query failed (error %lu)", s.win32Error);
            return buf;
        default: break;
    }
    std::snprintf(buf, sizeof(buf), "%s, %s (%s%s)", stateWord(s.state), startWord(s.startType),
                  s.serviceName.c_str(), s.foundByFallback ? ", found by display name or binary" : "");
    return buf;
}

// --- the query ------------------------------------------------------------

#if defined(_WIN32)
namespace {

std::string narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return std::string(); }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) { return std::wstring(); }
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE v) : h(v) {}
    ~ScHandle() {
        if (h != nullptr) { ::CloseServiceHandle(h); }
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
};

// The binary path of `name`, or empty when it cannot be read.
std::string binaryPathOf(SC_HANDLE scm, const wchar_t* name) {
    ScHandle svc(::OpenServiceW(scm, name, SERVICE_QUERY_CONFIG));
    if (svc.h == nullptr) { return std::string(); }
    DWORD needed = 0;
    ::QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    if (needed == 0) { return std::string(); }
    std::vector<unsigned char> buf(needed);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    if (::QueryServiceConfigW(svc.h, cfg, needed, &needed) == 0) { return std::string(); }
    return narrow(cfg->lpBinaryPathName);
}

// THE FALLBACK: every Win32 service, by display name first (no service is
// opened for that pass), then by binary path. Empty when nothing matches.
std::wstring findByFallback(SC_HANDLE scm, const std::string& displayName,
                            const std::string& binaryFile, DWORD& error) {
    error = 0;
    std::vector<unsigned char> buf;
    std::vector<std::wstring> names;
    std::wstring hit;
    DWORD resume = 0;
    for (;;) {
        DWORD needed = 0;
        DWORD returned = 0;
        const BOOL ok = ::EnumServicesStatusExW(
            scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buf.empty() ? nullptr : buf.data(), static_cast<DWORD>(buf.size()), &needed,
            &returned, &resume, nullptr);
        const DWORD e = ok ? 0 : ::GetLastError();
        if (!ok && e != ERROR_MORE_DATA) {
            error = e;
            return std::wstring();
        }
        const auto* rows = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(
            buf.empty() ? nullptr : buf.data());
        for (DWORD i = 0; rows != nullptr && i < returned; ++i) {
            if (rows[i].lpServiceName == nullptr) { continue; }
            if (hit.empty() &&
                serviceDisplayNameMatches(narrow(rows[i].lpDisplayName), displayName)) {
                hit = rows[i].lpServiceName;
            }
            names.emplace_back(rows[i].lpServiceName);
        }
        if (ok) { break; }
        // ERROR_MORE_DATA: grow to what was asked for and go on from `resume`.
        if (needed > buf.size()) { buf.resize(needed); }
    }
    if (!hit.empty()) { return hit; }
    for (const std::wstring& n : names) {
        if (serviceBinaryMatches(binaryPathOf(scm, n.c_str()), binaryFile)) { return n; }
    }
    return std::wstring();
}

SdrPlayServiceState mapState(DWORD s) {
    switch (s) {
        case SERVICE_STOPPED: return SdrPlayServiceState::Stopped;
        case SERVICE_START_PENDING: return SdrPlayServiceState::StartPending;
        case SERVICE_STOP_PENDING: return SdrPlayServiceState::StopPending;
        case SERVICE_RUNNING: return SdrPlayServiceState::Running;
        case SERVICE_CONTINUE_PENDING: return SdrPlayServiceState::ContinuePending;
        case SERVICE_PAUSE_PENDING: return SdrPlayServiceState::PausePending;
        case SERVICE_PAUSED: return SdrPlayServiceState::Paused;
        default: break;
    }
    return SdrPlayServiceState::Unknown;
}

SdrPlayServiceStart mapStart(DWORD s) {
    switch (s) {
        case SERVICE_BOOT_START: return SdrPlayServiceStart::Boot;
        case SERVICE_SYSTEM_START: return SdrPlayServiceStart::System;
        case SERVICE_AUTO_START: return SdrPlayServiceStart::Auto;
        case SERVICE_DEMAND_START: return SdrPlayServiceStart::Manual;
        case SERVICE_DISABLED: return SdrPlayServiceStart::Disabled;
        default: break;
    }
    return SdrPlayServiceStart::Unknown;
}

}  // namespace
#endif

SdrPlayServiceStatus querySdrPlayServiceFromWindows() {
    return queryWindowsServiceByNames(kSdrPlayServiceName, kSdrPlayServiceDisplayName,
                                      kSdrPlayServiceBinary);
}

SdrPlayServiceStatus queryWindowsServiceByNames(const std::string& serviceName,
                                                const std::string& displayName,
                                                const std::string& binaryFile) {
    SdrPlayServiceStatus st;
#if defined(_WIN32)
    ScHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (scm.h == nullptr) {
        st.state = SdrPlayServiceState::QueryFailed;
        st.win32Error = ::GetLastError();
        return st;
    }
    std::wstring name = widen(serviceName);
    SC_HANDLE raw = ::OpenServiceW(scm.h, name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (raw == nullptr) {
        const DWORD e = ::GetLastError();
        if (e != ERROR_SERVICE_DOES_NOT_EXIST) {
            st.state = SdrPlayServiceState::QueryFailed;
            st.win32Error = e;
            return st;
        }
        DWORD enumError = 0;
        name = findByFallback(scm.h, displayName, binaryFile, enumError);
        if (name.empty()) {
            if (enumError != 0) {
                st.state = SdrPlayServiceState::QueryFailed;
                st.win32Error = enumError;
            } else {
                st.state = SdrPlayServiceState::NotInstalled;
            }
            return st;
        }
        st.foundByFallback = true;
        raw = ::OpenServiceW(scm.h, name.c_str(), SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
        if (raw == nullptr) {
            st.state = SdrPlayServiceState::QueryFailed;
            st.win32Error = ::GetLastError();
            return st;
        }
    }
    ScHandle svc(raw);
    st.serviceName = narrow(name.c_str());

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (::QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                               sizeof(ssp), &needed) == 0) {
        st.state = SdrPlayServiceState::QueryFailed;
        st.win32Error = ::GetLastError();
        return st;
    }
    st.state = mapState(ssp.dwCurrentState);

    needed = 0;
    ::QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    if (needed > 0) {
        std::vector<unsigned char> buf(needed);
        auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
        if (::QueryServiceConfigW(svc.h, cfg, needed, &needed) != 0) {
            st.startType = mapStart(cfg->dwStartType);
        }
    }
#else
    (void) serviceName;
    (void) displayName;
    (void) binaryFile;
#endif
    return st;
}

namespace {

// Never destroyed (a leaked singleton, not a function-local object): the
// restart's worker may be abandoned at exit and must not find this gone.
struct QueryOverride {
    std::mutex m;
    std::function<SdrPlayServiceStatus()> fn;
};
QueryOverride& queryOverride() {
    static QueryOverride* o = new QueryOverride();
    return *o;
}

struct LastNoted {
    std::mutex m;
    bool have = false;
    SdrPlayServiceStatus status;
};
LastNoted& lastNoted() {
    static LastNoted* l = new LastNoted();
    return *l;
}

}  // namespace

SdrPlayServiceStatus querySdrPlayService() {
    std::function<SdrPlayServiceStatus()> fn;
    {
        std::lock_guard<std::mutex> lk(queryOverride().m);
        fn = queryOverride().fn;
    }
    if (fn) { return fn(); }
    return querySdrPlayServiceFromWindows();
}

void setSdrPlayServiceQueryForTest(std::function<SdrPlayServiceStatus()> query) {
    std::lock_guard<std::mutex> lk(queryOverride().m);
    queryOverride().fn = std::move(query);
}

bool noteSdrPlayServiceStatus(const SdrPlayServiceStatus& s) {
    {
        std::lock_guard<std::mutex> lk(lastNoted().m);
        if (lastNoted().have && lastNoted().status == s) { return false; }
        lastNoted().have = true;
        lastNoted().status = s;
    }
    // Off Windows nothing was asked, so there is nothing to say - and a line
    // saying so on every Linux session with an RSP would be noise.
    if (s.state == SdrPlayServiceState::NotApplicable) { return false; }
    const std::string summary = sdrPlayServiceSummary(s);
    if (s.state == SdrPlayServiceState::Running) {
        core::diagLogf("source: SDRplay API Service - %s", summary.c_str());
    } else {
        core::diagWarnf("source: SDRplay API Service - %s", summary.c_str());
    }
    return true;
}

SdrPlayServiceStatus querySdrPlayServiceAndNote() {
    const SdrPlayServiceStatus s = querySdrPlayService();
    noteSdrPlayServiceStatus(s);
    return s;
}

bool sdrPlayLastServiceStatus(SdrPlayServiceStatus& out) {
    std::lock_guard<std::mutex> lk(lastNoted().m);
    if (!lastNoted().have) { return false; }
    out = lastNoted().status;
    return true;
}

void sdrPlayServiceResetNoteForTest() {
    std::lock_guard<std::mutex> lk(lastNoted().m);
    lastNoted().have = false;
    lastNoted().status = SdrPlayServiceStatus{};
}

// --- the restart ----------------------------------------------------------

bool sdrPlayServiceNameSafeForCommand(const std::string& serviceName) {
    // cmd.exe's metacharacters (& | < > ^ " % ( ) and blanks) must never reach
    // an elevated command line from a name a third party could have chosen.
    // Service names in practice are identifiers; this is the whole alphabet.
    if (serviceName.empty() || serviceName.size() > 256) { return false; }
    for (char c : serviceName) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                        (u >= '0' && u <= '9') || u == '_' || u == '-' || u == '.';
        if (!ok) { return false; }
    }
    return true;
}

std::wstring sdrPlayRestartProgram(const std::wstring& systemRoot) {
    std::wstring root = systemRoot.empty() ? std::wstring(L"C:\\Windows") : systemRoot;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) { root.pop_back(); }
    return root + L"\\System32\\cmd.exe";
}

std::wstring sdrPlayRestartParameters(const std::string& serviceName) {
    if (!sdrPlayServiceNameSafeForCommand(serviceName)) { return std::wstring(); }
    // The name is ASCII by the check above, so widening is byte for byte.
    const std::wstring svc(serviceName.begin(), serviceName.end());
    // cmd /c with the whole line in one pair of quotes: cmd strips the outer
    // pair and runs what is inside. "net stop" refuses a service that is not
    // running (exit 2) or that will not stop - either way taskkill makes sure
    // no wedged sdrplay_apiService.exe is left - and "net start" runs
    // whatever happened before it, so cmd's exit code is net start's.
    return L"/c \"(net stop " + svc + L" || taskkill /F /IM " +
           std::wstring(L"sdrplay_apiService.exe") + L") & net start " + svc + L"\"";
}

SdrPlayRestartOutcome runSdrPlayRestartElevated(const std::string& serviceName,
                                                std::chrono::milliseconds limit,
                                                const std::atomic<bool>& cancelled,
                                                void* ownerWindow) {
    SdrPlayRestartOutcome out;
    const std::wstring params = sdrPlayRestartParameters(serviceName);
    if (params.empty()) {
        out.phase = SdrPlayRestartPhase::NotStarted;
        out.win32Error = 87;  // ERROR_INVALID_PARAMETER: the name was refused
        return out;
    }
#if defined(_WIN32)
    wchar_t rootBuf[MAX_PATH] = {};
    const DWORD rootLen = ::GetEnvironmentVariableW(L"SystemRoot", rootBuf, MAX_PATH);
    const std::wstring program =
        sdrPlayRestartProgram((rootLen > 0 && rootLen < MAX_PATH) ? std::wstring(rootBuf, rootLen)
                                                                  : std::wstring());

    // ShellExecuteEx may hand the verb to a shell extension that needs COM on
    // this thread; the documented requirement is an STA with OLE1 DDE off.
    const HRESULT co = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = static_cast<HWND>(ownerWindow);
    sei.lpVerb = L"runas";
    sei.lpFile = program.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    // BLOCKS UNTIL THE UAC PROMPT IS ANSWERED - which is why this runs on the
    // restart's worker and never on the GUI thread.
    const BOOL launched = ::ShellExecuteExW(&sei);
    const DWORD launchError = launched ? 0 : ::GetLastError();
    if (SUCCEEDED(co)) { ::CoUninitialize(); }

    if (!launched) {
        out.phase = (launchError == ERROR_CANCELLED) ? SdrPlayRestartPhase::Cancelled
                                                     : SdrPlayRestartPhase::NotStarted;
        out.win32Error = launchError;
        return out;
    }
    if (sei.hProcess == nullptr) {
        // Elevated and started, but no handle to wait on: nothing to time.
        out.phase = SdrPlayRestartPhase::Done;
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    DWORD waited = WAIT_TIMEOUT;
    while (!cancelled.load(std::memory_order_acquire)) {
        waited = ::WaitForSingleObject(sei.hProcess, static_cast<DWORD>(kSdrPlayRestartPoll.count()));
        if (waited != WAIT_TIMEOUT) { break; }
        if (std::chrono::steady_clock::now() >= deadline) { break; }
    }
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 0;
        ::GetExitCodeProcess(sei.hProcess, &code);
        out.exitCode = code;
        out.phase = (code == 0) ? SdrPlayRestartPhase::Done : SdrPlayRestartPhase::Failed;
    } else {
        out.phase = SdrPlayRestartPhase::TimedOut;
    }
    ::CloseHandle(sei.hProcess);
#else
    (void) limit;
    (void) cancelled;
    (void) ownerWindow;
    out.phase = SdrPlayRestartPhase::NotStarted;
#endif
    return out;
}

SdrPlayServiceRestart::~SdrPlayServiceRestart() {
    shared_->cancelled.store(true, std::memory_order_release);
    if (!worker_.joinable()) { return; }
    // Joined when it has already finished, left otherwise - see the header.
    if (phase() != SdrPlayRestartPhase::Running) {
        worker_.join();
    } else {
        worker_.detach();
    }
}

void SdrPlayServiceRestart::setLauncherForTest(SdrPlayRestartLauncher launcher) {
    launcher_ = std::move(launcher);
}

bool SdrPlayServiceRestart::start(const std::string& serviceName, void* ownerWindow) {
    if (phase() == SdrPlayRestartPhase::Running) { return false; }
    // The previous worker has published its outcome and is returning (or has
    // returned); joining it costs nothing.
    if (worker_.joinable()) { worker_.join(); }
    {
        std::lock_guard<std::mutex> lk(shared_->m);
        shared_->finished = false;
        shared_->outcome = SdrPlayRestartOutcome{};
        shared_->outcome.phase = SdrPlayRestartPhase::Running;
    }
    shared_->cancelled.store(false, std::memory_order_release);
    shared_->phase.store(static_cast<int>(SdrPlayRestartPhase::Running), std::memory_order_release);

    std::shared_ptr<Shared> shared = shared_;
    SdrPlayRestartLauncher launcher = launcher_;
    worker_ = std::thread([shared, launcher, serviceName, ownerWindow]() {
        SdrPlayRestartOutcome o =
            launcher ? launcher(serviceName, kSdrPlayRestartLimit, shared->cancelled)
                     : runSdrPlayRestartElevated(serviceName, kSdrPlayRestartLimit,
                                                 shared->cancelled, ownerWindow);
        // WHAT WINDOWS SAYS NOW, whatever net's exit code was: the code says
        // what the command did, the state says where the service ended up.
        // Not after a cancelled prompt (nothing changed) or a give-up.
        if (o.phase != SdrPlayRestartPhase::Cancelled &&
            !shared->cancelled.load(std::memory_order_acquire)) {
            o.after = querySdrPlayService();
        }
        {
            std::lock_guard<std::mutex> lk(shared->m);
            shared->outcome = o;
            shared->finished = true;
        }
        shared->phase.store(static_cast<int>(o.phase), std::memory_order_release);
    });
    return true;
}

SdrPlayRestartPhase SdrPlayServiceRestart::phase() const {
    return static_cast<SdrPlayRestartPhase>(shared_->phase.load(std::memory_order_acquire));
}

SdrPlayRestartOutcome SdrPlayServiceRestart::outcome() const {
    std::lock_guard<std::mutex> lk(shared_->m);
    return shared_->outcome;
}

bool SdrPlayServiceRestart::takeFinished() {
    std::lock_guard<std::mutex> lk(shared_->m);
    if (!shared_->finished) { return false; }
    shared_->finished = false;
    return true;
}

}  // namespace cascade::source
