// sdrplay_service.hpp - what state Windows says the SDRplay API Service is
// in, the sentence that turns that state into an instruction, and the key
// that restarts it.
//
// WHY THIS EXISTS (0.99.55, a UK RSP2 Pro owner on 0.99.52). An RSP is driven
// through sdrplay_api.dll, and that DLL talks to a Windows SERVICE which holds
// the radio's USB handle (see sdrplay_source.hpp). When the service is
// stopped, crashed or wedged, sdrplay_api_Open answers sdrplay_api_Fail in
// 0 ms and all FoxSDR could say was "Check that the SDRplay API service is
// running" - an instruction the tester did not manage to act on in seven
// launches. Windows knows whether the service is running; asking it costs a
// millisecond and needs no administrator rights. So the sentence now says
// WHICH state it is in, and a key does the restart for him.
//
// WHERE THE NAMES COME FROM, said plainly because it is weaker evidence than
// the rest of the driver rests on: the service name "SDRplayAPIService", the
// display name "SDRplay API Service" and the binary "sdrplay_apiService.exe"
// come from third-party listings (file.net, glarysoft), NOT from SDRplay's own
// documentation, and there is no SDRplay install on the machine this was
// written on. So the query does not trust the name alone: when OpenServiceW
// answers ERROR_SERVICE_DOES_NOT_EXIST it walks every Win32 service and takes
// the one whose display name is "SDRplay API Service" (any case) or whose
// binary path ends in sdrplay_apiService.exe, and it reports the name it
// actually found - which is the name the restart then uses.
//
// READ-ONLY, AND NEVER ON A HOT PATH. The query opens the Service Control
// Manager with SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE and the
// service with SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG - rights every
// interactive user has. It is made where an Open has just failed (inside the
// enumeration's bounded worker, or open()'s caller), when a scan finds the
// process in trouble, and when a diagnostics bundle is built; never inside a
// stream callback, never per frame.
//
// THE RESTART IS ELEVATED THROUGH cmd.exe, NOT THROUGH FoxSDR. Stopping and
// starting a service needs an administrator, and a Microsoft Store (MSIX)
// copy of FoxSDR cannot relaunch its own packaged exe elevated without a
// restricted capability. So the key hands Windows' own cmd.exe to
// ShellExecuteExW with the "runas" verb - the ordinary UAC prompt - and the
// command it runs is built by one pure, tested function.
//
// LINUX. SDRplay's Linux daemon is out of scope: every function here answers
// "not applicable" off Windows and the sentence is empty, so nothing changes
// on that platform.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cascade::source {

// The names the query looks for first - see the file header for where they
// come from and why a display-name and binary fallback stands behind them.
inline constexpr const char* kSdrPlayServiceName = "SDRplayAPIService";
inline constexpr const char* kSdrPlayServiceDisplayName = "SDRplay API Service";
inline constexpr const char* kSdrPlayServiceBinary = "sdrplay_apiService.exe";

enum class SdrPlayServiceState {
    NotApplicable,  // not Windows: nothing was asked
    QueryFailed,    // the Service Control Manager could not be asked
    NotInstalled,   // asked, and no service matches
    Stopped,
    StartPending,
    StopPending,
    Running,
    ContinuePending,
    PausePending,
    Paused,
    Unknown  // a state Windows reported that none of the above names
};

enum class SdrPlayServiceStart { Unknown, Boot, System, Auto, Manual, Disabled };

struct SdrPlayServiceStatus {
    SdrPlayServiceState state = SdrPlayServiceState::NotApplicable;
    SdrPlayServiceStart startType = SdrPlayServiceStart::Unknown;
    // The name the Service Control Manager knows it by, as FOUND - empty when
    // it was not found. This, not kSdrPlayServiceName, is what a restart uses.
    std::string serviceName;
    // True when it was found by display name or binary path rather than by
    // kSdrPlayServiceName - i.e. the third-party listings had the name wrong.
    bool foundByFallback = false;
    // GetLastError() of the call that failed, for QueryFailed.
    unsigned long win32Error = 0;

    bool operator==(const SdrPlayServiceStatus& o) const {
        return state == o.state && startType == o.startType && serviceName == o.serviceName &&
               foundByFallback == o.foundByFallback && win32Error == o.win32Error;
    }
    bool operator!=(const SdrPlayServiceStatus& o) const { return !(*this == o); }
};

// True when a service was found at all - the only case a restart can name.
bool sdrPlayServiceFound(const SdrPlayServiceStatus& s);

// --- the pure halves --------------------------------------------------------

// A display name equal to `want`, ignoring case and surrounding blanks; the
// SDRplay form compares with kSdrPlayServiceDisplayName.
bool serviceDisplayNameMatches(const std::string& displayName, const std::string& want);
bool sdrPlayServiceDisplayNameMatches(const std::string& displayName);

// A service binary path (QueryServiceConfigW's lpBinaryPathName, which may be
// quoted and may carry arguments) whose executable's file name is `exeFile`,
// ignoring case; the SDRplay form compares with kSdrPlayServiceBinary.
bool serviceBinaryMatches(const std::string& binaryPath, const std::string& exeFile);
bool sdrPlayServiceBinaryMatches(const std::string& binaryPath);

// THE SENTENCE, IN ENGLISH - the translation KEY. Callers that put it on the
// screen pass it through i18n::tr(); the log and the diagnostics keep the
// English. Empty ("") when there is nothing worth saying: not Windows.
const char* sdrPlayServiceSentence(const SdrPlayServiceStatus& s);

// The same sentence, translated - what the Source section shows. Empty when
// sdrPlayServiceSentence is.
std::string sdrPlayServiceAdvice(const SdrPlayServiceStatus& s);

// One short English line for the log and the diagnostics bundle's
// `sdrplay-service:` field: "running, auto start (SDRplayAPIService)",
// "not installed", "not applicable", "query failed (error 5)". No personal
// data - a Windows service's name and state.
std::string sdrPlayServiceSummary(const SdrPlayServiceStatus& s);

// --- the query --------------------------------------------------------------

// Asks Windows, now: the Service Control Manager, read-only (see the file
// header). Off Windows, NotApplicable without asking anything.
SdrPlayServiceStatus querySdrPlayServiceFromWindows();

// THE SAME QUERY FOR ANY NAMES - what the one above calls with the SDRplay
// names. Exposed so a test can run the real Service Control Manager path
// (the name, the display-name fallback and the binary-path fallback) against
// a service every Windows machine has, since none of them has SDRplay's.
SdrPlayServiceStatus queryWindowsServiceByNames(const std::string& serviceName,
                                                const std::string& displayName,
                                                const std::string& binaryFile);

// THE SEAM every caller goes through: the Windows answer, or the test's.
SdrPlayServiceStatus querySdrPlayService();

// TESTS ONLY: answer querySdrPlayService() with this instead of asking
// Windows. An empty function restores the real query.
void setSdrPlayServiceQueryForTest(std::function<SdrPlayServiceStatus()> query);

// ONCE PER STATE CHANGE, NOT ONCE PER SCAN. Records `s` as the last state
// seen and, when it differs from the one before, writes one log line
// (a warning unless the service is running). Returns true when it logged.
// NotApplicable (not Windows) is recorded but never logged.
// Scans happen every time the source list is opened; a line per scan would
// bury everything else in a report.
bool noteSdrPlayServiceStatus(const SdrPlayServiceStatus& s);

// querySdrPlayService() then noteSdrPlayServiceStatus() - the one call the
// driver makes where an Open has failed.
SdrPlayServiceStatus querySdrPlayServiceAndNote();

// The last state noted, and whether there has been one.
bool sdrPlayLastServiceStatus(SdrPlayServiceStatus& out);

// TESTS ONLY: forget the last state noted, so the next note logs.
void sdrPlayServiceResetNoteForTest();

// --- the restart ------------------------------------------------------------

// HOW LONG THE RESTART'S WORKER WAITS FOR THE ELEVATED cmd.exe TO FINISH, once
// Windows has started it (the UAC prompt itself is not counted: ShellExecuteExW
// does not return until it has been answered). A net stop that hangs is what
// the taskkill fallback is for; forty-five seconds covers net's own ~20 s
// wait for a stop plus a start. Spent on the worker, never the GUI thread.
inline constexpr std::chrono::milliseconds kSdrPlayRestartLimit{45000};

// How often that worker looks up from the process to see whether it has been
// told to give up (the key's owner is being destroyed). Not a bound on
// anything the user waits for.
inline constexpr std::chrono::milliseconds kSdrPlayRestartPoll{100};

// A service name the restart will put on an elevated command line: letters,
// digits, '_', '-' and '.', 1 to 256 of them. The name comes from the
// Service Control Manager - and, through the display-name fallback, from
// whichever service claims that display name - so it is checked before it
// is ever given to cmd.exe as administrator.
bool sdrPlayServiceNameSafeForCommand(const std::string& serviceName);

// "<systemRoot>\System32\cmd.exe" - the program the elevated restart runs.
// `systemRoot` is %SystemRoot% as the environment has it; "C:\Windows" when it
// is empty.
std::wstring sdrPlayRestartProgram(const std::wstring& systemRoot);

// THE COMMAND LINE handed to that cmd.exe:
//   /c "(net stop <svc> || taskkill /F /IM sdrplay_apiService.exe) & net start <svc>"
// Empty when the name fails sdrPlayServiceNameSafeForCommand.
std::wstring sdrPlayRestartParameters(const std::string& serviceName);

enum class SdrPlayRestartPhase {
    Idle,       // never pressed
    Running,    // the UAC prompt or the command is in progress
    Done,       // the command ran and exited 0
    Failed,     // the command ran and exited non-zero
    Cancelled,  // the user said no to the UAC prompt
    TimedOut,   // the command did not finish within kSdrPlayRestartLimit
    NotStarted  // Windows would not start it (win32Error), or the name was unsafe
};

struct SdrPlayRestartOutcome {
    SdrPlayRestartPhase phase = SdrPlayRestartPhase::Idle;
    unsigned long exitCode = 0;    // for Done / Failed
    unsigned long win32Error = 0;  // for NotStarted
    SdrPlayServiceStatus after;    // the state queried once the command ended
};

// What actually runs the elevated command and waits for it - the seam the
// tests replace. Given the service name, the bound, and a flag that becomes
// true when the caller has given up; answers phase / exitCode / win32Error
// (`after` is filled by the caller).
using SdrPlayRestartLauncher = std::function<SdrPlayRestartOutcome(
    const std::string& serviceName, std::chrono::milliseconds limit,
    const std::atomic<bool>& cancelled)>;

// The real launcher: ShellExecuteExW("runas", cmd.exe, SW_HIDE,
// SEE_MASK_NOCLOSEPROCESS) and a bounded wait. Off Windows, NotStarted.
// `ownerWindow` is the HWND the UAC prompt belongs to (may be null).
SdrPlayRestartOutcome runSdrPlayRestartElevated(const std::string& serviceName,
                                                std::chrono::milliseconds limit,
                                                const std::atomic<bool>& cancelled,
                                                void* ownerWindow);

// THE KEY'S ENGINE: one restart at a time, on a worker thread, so the GUI
// thread never waits on the UAC prompt or on net. The GUI polls phase() each
// frame and calls takeFinished() once to act on the result.
class SdrPlayServiceRestart {
public:
    SdrPlayServiceRestart() = default;
    // Tells a running worker to give up and DOES NOT WAIT for it: the worker
    // may be inside ShellExecuteExW with the UAC prompt up, which nothing here
    // can shorten. It owns everything it touches through a shared_ptr.
    ~SdrPlayServiceRestart();
    SdrPlayServiceRestart(const SdrPlayServiceRestart&) = delete;
    SdrPlayServiceRestart& operator=(const SdrPlayServiceRestart&) = delete;

    // Starts a restart of `serviceName`. False when one is already running.
    // An unsafe name is not refused here but finishes at once as NotStarted,
    // so the panel says so rather than nothing happening.
    bool start(const std::string& serviceName, void* ownerWindow = nullptr);

    SdrPlayRestartPhase phase() const;
    bool running() const { return phase() == SdrPlayRestartPhase::Running; }
    // The last finished restart's outcome (phase Idle before any).
    SdrPlayRestartOutcome outcome() const;
    // True exactly once per finished restart - the GUI's cue to act on it.
    bool takeFinished();

    // TESTS ONLY: run this instead of the elevated command.
    void setLauncherForTest(SdrPlayRestartLauncher launcher);

private:
    struct Shared {
        mutable std::mutex m;
        SdrPlayRestartOutcome outcome;
        bool finished = false;  // not yet taken
        std::atomic<bool> cancelled{false};
        std::atomic<int> phase{static_cast<int>(SdrPlayRestartPhase::Idle)};
    };
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    SdrPlayRestartLauncher launcher_;
    std::thread worker_;
};

}  // namespace cascade::source
