// The SDRplay API Service: what Windows says it is doing, the sentence that
// turns that into an instruction, and the RESTART SDRPLAY SERVICE key's
// command line, worker and decisions (0.99.55, src/source/sdrplay_service.hpp).
//
// THE FIELD REPORT. A UK RSP2 Pro owner on 0.99.52: sdrplay_api_Open answered
// sdrplay_api_Fail in 0 ms on seven launches, and FoxSDR's only instruction
// was "Check that the SDRplay API service is running". His service was
// stopped, crashed or wedged; he never found it.
//
// WHAT IS PROVED HERE, and what is not. Every pure piece (the sentences, the
// summary, the name matchers, the elevated command line, the key's and the
// restart's decisions), the restart worker against a fake launcher, and the
// REAL Service Control Manager path against services every Windows machine
// has - by name, by display name and by binary path - because no machine
// this runs on has SDRplay's. What is NOT proved: that SDRplay's service is
// really called SDRplayAPIService (third-party listings say so; the fallback
// exists because that is not SDRplay's own word), and the elevated restart
// itself, which would put a UAC prompt in front of whoever runs the suite.
// The fake-API half - an Open that fails, the log once per change, the lost
// session kept lost - is in test_sdrplay_source.cpp beside the other
// sessionAcquire tests.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "core/diag_log.hpp"
#include "source/sdrplay_service.hpp"
#include "source/sdrplay_source.hpp"
#include "test_check.hpp"

namespace src = cascade::source;
using src::SdrPlayAfterRestart;
using src::SdrPlayRestartOutcome;
using src::SdrPlayRestartPhase;
using src::SdrPlayServiceStart;
using src::SdrPlayServiceState;
using src::SdrPlayServiceStatus;
using src::SdrPlayServiceTrouble;

namespace {

SdrPlayServiceStatus status(SdrPlayServiceState state,
                            SdrPlayServiceStart start = SdrPlayServiceStart::Manual,
                            const std::string& name = "SDRplayAPIService") {
    SdrPlayServiceStatus s;
    s.state = state;
    s.startType = start;
    if (state != SdrPlayServiceState::NotApplicable && state != SdrPlayServiceState::NotInstalled &&
        state != SdrPlayServiceState::QueryFailed) {
        s.serviceName = name;
    }
    return s;
}

std::string sentence(const SdrPlayServiceStatus& s) { return src::sdrPlayServiceSentence(s); }

int logLinesContaining(const char* needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// --- 1. every state has its sentence, pinned verbatim ----------------------

// Pinned word for word because each is the whole of what an RSP owner is told
// about the service, and each is a translation key: a rewording here is a
// missing string in 33 catalogues (test_i18n) as well as a changed instruction.
void testEveryStateHasItsSentence() {
    CHECK(sentence(status(SdrPlayServiceState::NotApplicable)).empty());
    CHECK(src::sdrPlayServiceAdvice(status(SdrPlayServiceState::NotApplicable)).empty());

    CHECK(sentence(status(SdrPlayServiceState::NotInstalled)) ==
          "The SDRplay API Service is not installed on this computer - install the SDRplay API "
          "from sdrplay.com, then restart FoxSDR.");
    CHECK(sentence(status(SdrPlayServiceState::Stopped)) ==
          "The SDRplay API Service is stopped - press RESTART SDRPLAY SERVICE to start it.");
    CHECK(sentence(status(SdrPlayServiceState::StartPending)) ==
          "The SDRplay API Service is still starting - wait a few seconds, then press Refresh.");
    CHECK(sentence(status(SdrPlayServiceState::Running, SdrPlayServiceStart::Auto)) ==
          "The SDRplay API Service is running but did not answer - press RESTART SDRPLAY "
          "SERVICE.");
    const std::string stuck =
        "The SDRplay API Service is stuck or paused - press RESTART SDRPLAY SERVICE.";
    CHECK(sentence(status(SdrPlayServiceState::StopPending)) == stuck);
    CHECK(sentence(status(SdrPlayServiceState::Paused)) == stuck);
    CHECK(sentence(status(SdrPlayServiceState::PausePending)) == stuck);
    CHECK(sentence(status(SdrPlayServiceState::ContinuePending)) == stuck);
    const std::string unreadable =
        "FoxSDR could not read the SDRplay API Service's state - restart it in Windows Services.";
    CHECK(sentence(status(SdrPlayServiceState::QueryFailed)) == unreadable);
    CHECK(sentence(status(SdrPlayServiceState::Unknown)) == unreadable);

    // DISABLED BEATS STOPPED: net start refuses a disabled service, so
    // "press RESTART" alone would send the user into a failure.
    const std::string disabled =
        "The SDRplay API Service is disabled in Windows Services - set its startup type to "
        "Automatic there, then press RESTART SDRPLAY SERVICE.";
    CHECK(sentence(status(SdrPlayServiceState::Stopped, SdrPlayServiceStart::Disabled)) == disabled);
    CHECK(sentence(status(SdrPlayServiceState::Running, SdrPlayServiceStart::Disabled)) == disabled);
    // ...but "not installed" is not a startup-type question.
    CHECK(sentence(status(SdrPlayServiceState::NotInstalled, SdrPlayServiceStart::Disabled))
              .find("not installed") != std::string::npos);

    // With English in force the screen's copy is the key itself.
    CHECK(src::sdrPlayServiceAdvice(status(SdrPlayServiceState::Stopped)) ==
          sentence(status(SdrPlayServiceState::Stopped)));
}

// --- 2. the one-line summary the log and the bundle carry ------------------

void testTheSummaryLine() {
    CHECK(src::sdrPlayServiceSummary(status(SdrPlayServiceState::NotApplicable)) == "not applicable");
    CHECK(src::sdrPlayServiceSummary(status(SdrPlayServiceState::NotInstalled)) == "not installed");
    SdrPlayServiceStatus failed = status(SdrPlayServiceState::QueryFailed);
    failed.win32Error = 5;
    CHECK(src::sdrPlayServiceSummary(failed) == "query failed (error 5)");
    CHECK(src::sdrPlayServiceSummary(status(SdrPlayServiceState::Stopped)) ==
          "stopped, manual start (SDRplayAPIService)");
    CHECK(src::sdrPlayServiceSummary(status(SdrPlayServiceState::Running, SdrPlayServiceStart::Auto)) ==
          "running, auto start (SDRplayAPIService)");
    CHECK(src::sdrPlayServiceSummary(
              status(SdrPlayServiceState::StopPending, SdrPlayServiceStart::Disabled)) ==
          "stop pending, disabled (SDRplayAPIService)");
    // Found under ANOTHER name: the summary says so, because it means the
    // third-party listings had the name wrong - worth knowing from a report.
    SdrPlayServiceStatus other = status(SdrPlayServiceState::Running, SdrPlayServiceStart::Auto,
                                        "SDRplayAPI");
    other.foundByFallback = true;
    CHECK(src::sdrPlayServiceSummary(other) ==
          "running, auto start (SDRplayAPI, found by display name or binary)");
}

// --- 3. the fallback's matchers --------------------------------------------

void testTheFallbackMatchers() {
    CHECK(src::sdrPlayServiceDisplayNameMatches("SDRplay API Service"));
    CHECK(src::sdrPlayServiceDisplayNameMatches("  sdrplay api service "));
    CHECK(!src::sdrPlayServiceDisplayNameMatches("SDRplay API Service 2"));
    CHECK(!src::sdrPlayServiceDisplayNameMatches(""));

    // lpBinaryPathName is a command line: quoted with spaces, unquoted, with
    // arguments after it - and only the executable's FILE NAME counts.
    CHECK(src::sdrPlayServiceBinaryMatches(
        "\"C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_apiService.exe\""));
    CHECK(src::sdrPlayServiceBinaryMatches(
        "\"C:\\Program Files\\SDRplay\\API\\x64\\SDRPLAY_APISERVICE.EXE\" -run"));
    CHECK(src::sdrPlayServiceBinaryMatches(
        "C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_apiService.exe"));
    CHECK(src::sdrPlayServiceBinaryMatches("sdrplay_apiService.exe"));
    CHECK(!src::sdrPlayServiceBinaryMatches("C:\\Windows\\system32\\svchost.exe -k netsvcs"));
    CHECK(!src::sdrPlayServiceBinaryMatches("C:\\x\\notsdrplay_apiService.exe"));
    CHECK(!src::sdrPlayServiceBinaryMatches("\"C:\\x\\sdrplay_apiService.exe.bak\""));
    CHECK(!src::sdrPlayServiceBinaryMatches(""));
    CHECK(src::serviceBinaryMatches("C:\\WINDOWS\\system32\\svchost.exe -k LocalService -p",
                                    "svchost.exe"));
    CHECK(!src::serviceBinaryMatches("C:\\WINDOWS\\system32\\svchost.exe", ""));
}

// --- 4. the elevated command line -------------------------------------------

void testTheRestartCommandLine() {
    // cmd.exe from %SystemRoot%, trailing separators or not, and a default
    // when the variable is missing.
    CHECK(src::sdrPlayRestartProgram(L"C:\\Windows") == L"C:\\Windows\\System32\\cmd.exe");
    CHECK(src::sdrPlayRestartProgram(L"D:\\WIN\\") == L"D:\\WIN\\System32\\cmd.exe");
    CHECK(src::sdrPlayRestartProgram(L"") == L"C:\\Windows\\System32\\cmd.exe");

    // The whole line, pinned: the service name found, substituted twice; the
    // taskkill fallback when the stop fails; the start after either.
    CHECK(src::sdrPlayRestartParameters("SDRplayAPIService") ==
          L"/c \"(net stop SDRplayAPIService || taskkill /F /IM sdrplay_apiService.exe) & "
          L"net start SDRplayAPIService\"");
    CHECK(src::sdrPlayRestartParameters("SDRplay_API.2-x") ==
          L"/c \"(net stop SDRplay_API.2-x || taskkill /F /IM sdrplay_apiService.exe) & "
          L"net start SDRplay_API.2-x\"");

    // THE NAME IS CHECKED BEFORE IT REACHES AN ELEVATED cmd.exe. It comes
    // from the Service Control Manager - through the display-name fallback,
    // from whichever service claims that display name - so every cmd.exe
    // metacharacter is refused, and the command is then empty.
    const char* unsafe[] = {"",           "SDRplay API Service", "x&calc",   "x|y",
                            "x>y",        "x<y",                 "x^y",      "x\"y",
                            "%COMSPEC%",  "x(y)",                "x y",      "x\ty",
                            "x\ny",       "caf\xc3\xa9"};
    for (const char* n : unsafe) {
        CHECK(!src::sdrPlayServiceNameSafeForCommand(n));
        CHECK(src::sdrPlayRestartParameters(n).empty());
    }
    CHECK(src::sdrPlayServiceNameSafeForCommand(std::string(256, 'a')));
    CHECK(!src::sdrPlayServiceNameSafeForCommand(std::string(257, 'a')));

    // And the real launcher refuses an unsafe name BEFORE it asks Windows for
    // anything - no UAC prompt, no process, NotStarted with the reason.
    std::atomic<bool> cancelled{false};
    const SdrPlayRestartOutcome o = src::runSdrPlayRestartElevated(
        "x & calc", std::chrono::milliseconds(10), cancelled, nullptr);
    CHECK(o.phase == SdrPlayRestartPhase::NotStarted);
    CHECK(o.win32Error == 87u);
}

// --- 5. the real Service Control Manager --------------------------------------

// No machine this runs on has SDRplay's service, so the real query is run
// against services every Windows install has, through the same function with
// other names: by name, by display name when the name is wrong, and by binary
// path when both are. EventLog cannot be stopped on a running Windows.
void testTheRealServiceControlManager() {
#if defined(_WIN32)
    const SdrPlayServiceStatus byName = src::queryWindowsServiceByNames("EventLog", "", "");
    std::printf("EventLog by name: %s\n", src::sdrPlayServiceSummary(byName).c_str());
    CHECK(byName.state == SdrPlayServiceState::Running);
    CHECK(byName.startType == SdrPlayServiceStart::Auto);
    CHECK(byName.serviceName == "EventLog");
    CHECK(!byName.foundByFallback);

    const SdrPlayServiceStatus byDisplay =
        src::queryWindowsServiceByNames("NoSuchService_FoxSDR_1", "windows event log", "");
    std::printf("EventLog by display name: %s\n", src::sdrPlayServiceSummary(byDisplay).c_str());
    CHECK(byDisplay.state == SdrPlayServiceState::Running);
    CHECK(byDisplay.foundByFallback);
    CHECK(byDisplay.serviceName == "EventLog");

    // svchost.exe hosts dozens of services and its paths carry arguments
    // ("...\svchost.exe -k LocalServiceNetworkRestricted -p"): this proves the
    // binary pass parses a real lpBinaryPathName and reports what it found.
    const SdrPlayServiceStatus byBinary = src::queryWindowsServiceByNames(
        "NoSuchService_FoxSDR_2", "No Such Display Name FoxSDR", "svchost.exe");
    std::printf("svchost by binary: %s\n", src::sdrPlayServiceSummary(byBinary).c_str());
    CHECK(src::sdrPlayServiceFound(byBinary));
    CHECK(byBinary.foundByFallback);
    CHECK(!byBinary.serviceName.empty());

    const SdrPlayServiceStatus none = src::queryWindowsServiceByNames(
        "NoSuchService_FoxSDR_3", "No Such Display Name FoxSDR", "no_such_binary_foxsdr.exe");
    CHECK(none.state == SdrPlayServiceState::NotInstalled);
    CHECK(!src::sdrPlayServiceFound(none));

    // And the SDRplay query itself answers without failing, whatever this
    // machine has (printed, not assumed: an owner's bench may have it).
    const SdrPlayServiceStatus sdr = src::querySdrPlayServiceFromWindows();
    std::printf("SDRplay API Service on this machine: %s\n", src::sdrPlayServiceSummary(sdr).c_str());
    CHECK(sdr.state != SdrPlayServiceState::QueryFailed);
    CHECK(sdr.state != SdrPlayServiceState::NotApplicable);
#else
    const SdrPlayServiceStatus sdr = src::querySdrPlayServiceFromWindows();
    CHECK(sdr.state == SdrPlayServiceState::NotApplicable);
    CHECK(src::sdrPlayServiceSentence(sdr)[0] == '\0');
#endif
}

// --- 6. logged once per change, not once per scan ---------------------------

void testTheStateIsLoggedOncePerChange() {
    cascade::core::DiagLog::instance().resetForTest();
    src::sdrPlayServiceResetNoteForTest();
    const SdrPlayServiceStatus stopped = status(SdrPlayServiceState::Stopped);
    const SdrPlayServiceStatus running = status(SdrPlayServiceState::Running, SdrPlayServiceStart::Auto);

    CHECK(src::noteSdrPlayServiceStatus(stopped));
    // A scan every few seconds, the service still stopped: silence.
    for (int i = 0; i < 5; ++i) { CHECK(!src::noteSdrPlayServiceStatus(stopped)); }
    CHECK(logLinesContaining("SDRplay API Service - stopped, manual start (SDRplayAPIService)") == 1);
    CHECK(src::noteSdrPlayServiceStatus(running));
    CHECK(!src::noteSdrPlayServiceStatus(running));
    CHECK(src::noteSdrPlayServiceStatus(stopped));
    CHECK(logLinesContaining("SDRplay API Service - stopped") == 2);
    CHECK(logLinesContaining("SDRplay API Service - running, auto start") == 1);
    // The stopped line is a warning, the running one is not.
    int warned = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find("SDRplay API Service - stopped") != std::string::npos &&
            l.find("warn") != std::string::npos) {
            ++warned;
        }
    }
    CHECK(warned == 2);

    SdrPlayServiceStatus last;
    CHECK(src::sdrPlayLastServiceStatus(last));
    CHECK(last == stopped);
    // Off Windows nothing was asked, so nothing is said.
    CHECK(!src::noteSdrPlayServiceStatus(status(SdrPlayServiceState::NotApplicable)));
    CHECK(logLinesContaining("not applicable") == 0);
    src::sdrPlayServiceResetNoteForTest();
    CHECK(!src::sdrPlayLastServiceStatus(last));
}

// --- 7. the key's own decision ------------------------------------------------

void testWhenTheKeyIsShown() {
    const SdrPlayServiceStatus stopped = status(SdrPlayServiceState::Stopped);
    // The tester's case: Windows, a service found, Open failed.
    CHECK(src::sdrPlayRestartKeyShown(true, stopped, SdrPlayServiceTrouble::OpenFailed,
                                      SdrPlayRestartPhase::Idle));
    CHECK(src::sdrPlayRestartKeyShown(true, stopped, SdrPlayServiceTrouble::EnumerationHung,
                                      SdrPlayRestartPhase::Idle));
    CHECK(src::sdrPlayRestartKeyShown(true, stopped, SdrPlayServiceTrouble::SessionLost,
                                      SdrPlayRestartPhase::Cancelled));
    // Never off Windows.
    CHECK(!src::sdrPlayRestartKeyShown(false, stopped, SdrPlayServiceTrouble::OpenFailed,
                                       SdrPlayRestartPhase::Idle));
    // Never for a service Windows did not find: there is nothing to name.
    for (SdrPlayServiceState s : {SdrPlayServiceState::NotInstalled, SdrPlayServiceState::QueryFailed,
                                  SdrPlayServiceState::NotApplicable}) {
        CHECK(!src::sdrPlayRestartKeyShown(true, status(s), SdrPlayServiceTrouble::OpenFailed,
                                           SdrPlayRestartPhase::Idle));
    }
    // Nothing wrong: no key - unless a restart is still running, whose
    // progress must stay in view.
    CHECK(!src::sdrPlayRestartKeyShown(true, stopped, SdrPlayServiceTrouble::None,
                                       SdrPlayRestartPhase::Idle));
    CHECK(!src::sdrPlayRestartKeyShown(true, stopped, SdrPlayServiceTrouble::None,
                                       SdrPlayRestartPhase::Done));
    CHECK(src::sdrPlayRestartKeyShown(true, stopped, SdrPlayServiceTrouble::None,
                                      SdrPlayRestartPhase::Running));
}

// --- 8. what a finished restart leads to --------------------------------------

void testWhatARestartLeadsTo() {
    // (serviceRunning, sessionStateKnown, sessions, sessionLost, controlsInFlight)
    // THE TESTER'S CASE: the service is back and this process never had a
    // session - reopen here, no FoxSDR restart.
    CHECK(src::sdrPlayAfterServiceRestart(true, true, 0, false, 0) ==
          SdrPlayAfterRestart::ReopenInProcess);
    // A LOST SESSION IS NEVER REOPENED IN THIS PROCESS.
    CHECK(src::sdrPlayAfterServiceRestart(true, true, 0, true, 0) ==
          SdrPlayAfterRestart::RestartFoxSdr);
    CHECK(src::sdrPlayAfterServiceRestart(true, true, 1, true, 0) ==
          SdrPlayAfterRestart::RestartFoxSdr);
    // Not provably clear: a session still counted, a control still waiting,
    // or a state that could not be read without waiting.
    CHECK(src::sdrPlayAfterServiceRestart(true, true, 1, false, 0) ==
          SdrPlayAfterRestart::RestartFoxSdr);
    CHECK(src::sdrPlayAfterServiceRestart(true, true, 0, false, 1) ==
          SdrPlayAfterRestart::RestartFoxSdr);
    CHECK(src::sdrPlayAfterServiceRestart(true, false, 0, false, 0) ==
          SdrPlayAfterRestart::RestartFoxSdr);
    // The service still not running beats everything: say its state.
    CHECK(src::sdrPlayAfterServiceRestart(false, true, 0, false, 0) ==
          SdrPlayAfterRestart::ServiceNotRunning);
    CHECK(src::sdrPlayAfterServiceRestart(false, true, 0, true, 0) ==
          SdrPlayAfterRestart::ServiceNotRunning);
}

// --- 9. the restart's worker ------------------------------------------------

// The launcher is faked - the real one is ShellExecuteExW "runas" and would
// put a UAC prompt in front of whoever runs the suite. What is proved is the
// part FoxSDR owns: one at a time, off the calling thread, the state asked of
// Windows afterwards (not after a cancel), the outcome handed over once, and
// a destructor that never waits for a worker still inside the prompt.
void testTheRestartWorker() {
    std::atomic<int> queries{0};
    src::setSdrPlayServiceQueryForTest([&queries]() {
        ++queries;
        return status(SdrPlayServiceState::Running, SdrPlayServiceStart::Auto);
    });

    {  // Done: the command exited 0, the state is re-read.
        src::SdrPlayServiceRestart r;
        std::string seenName;
        std::atomic<bool> release{false};
        r.setLauncherForTest([&](const std::string& name, std::chrono::milliseconds limit,
                                 const std::atomic<bool>&) {
            seenName = name;
            CHECK(limit == src::kSdrPlayRestartLimit);
            while (!release.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            SdrPlayRestartOutcome o;
            o.phase = SdrPlayRestartPhase::Done;
            return o;
        });
        CHECK(r.phase() == SdrPlayRestartPhase::Idle);
        CHECK(!r.takeFinished());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(r.start("SDRplayAPIService"));
        // start() RETURNS while the launcher is still in progress - the GUI
        // thread never waits on the prompt or on net.
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
        CHECK(r.running());
        // One at a time.
        CHECK(!r.start("SDRplayAPIService"));
        release.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (r.running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(r.phase() == SdrPlayRestartPhase::Done);
        CHECK(seenName == "SDRplayAPIService");
        CHECK(r.takeFinished());
        CHECK(!r.takeFinished());  // once
        const SdrPlayRestartOutcome o = r.outcome();
        CHECK(o.after.state == SdrPlayServiceState::Running);
        CHECK(queries.load() == 1);
    }

    auto runOnce = [](src::SdrPlayServiceRestart& r) {
        CHECK(r.start("SDRplayAPIService"));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!r.takeFinished() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return r.outcome();
    };

    {  // Cancelled at the UAC prompt: nothing else is done - not even a query.
        queries.store(0);
        src::SdrPlayServiceRestart r;
        r.setLauncherForTest([](const std::string&, std::chrono::milliseconds,
                                const std::atomic<bool>&) {
            SdrPlayRestartOutcome o;
            o.phase = SdrPlayRestartPhase::Cancelled;
            o.win32Error = 1223;  // ERROR_CANCELLED
            return o;
        });
        const SdrPlayRestartOutcome o = runOnce(r);
        CHECK(o.phase == SdrPlayRestartPhase::Cancelled);
        CHECK(queries.load() == 0);
        CHECK(o.after.state == SdrPlayServiceState::NotApplicable);  // untouched
        // ...and the key can be pressed again.
        const SdrPlayRestartOutcome again = runOnce(r);
        CHECK(again.phase == SdrPlayRestartPhase::Cancelled);
    }

    {  // Failed with net's exit code: the code is kept and the state re-read.
        queries.store(0);
        src::SdrPlayServiceRestart r;
        r.setLauncherForTest([](const std::string&, std::chrono::milliseconds,
                                const std::atomic<bool>&) {
            SdrPlayRestartOutcome o;
            o.phase = SdrPlayRestartPhase::Failed;
            o.exitCode = 2;
            return o;
        });
        const SdrPlayRestartOutcome o = runOnce(r);
        CHECK(o.phase == SdrPlayRestartPhase::Failed);
        CHECK(o.exitCode == 2u);
        CHECK(queries.load() == 1);
    }

    {  // THE DESTRUCTOR DOES NOT WAIT for a worker still in the prompt; the
       // worker is told, and skips the query once it comes back.
        queries.store(0);
        std::atomic<bool> sawCancel{false};
        std::atomic<bool> finished{false};
        const auto t0 = std::chrono::steady_clock::now();
        {
            src::SdrPlayServiceRestart r;
            r.setLauncherForTest([&](const std::string&, std::chrono::milliseconds,
                                     const std::atomic<bool>& cancelled) {
                while (!cancelled.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
                sawCancel.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                finished.store(true);
                SdrPlayRestartOutcome o;
                o.phase = SdrPlayRestartPhase::TimedOut;
                return o;
            });
            CHECK(r.start("SDRplayAPIService"));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto destroyed = std::chrono::steady_clock::now() - t0;
        std::printf("restart destroyed mid-run in %lld ms\n",
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(destroyed).count()));
        CHECK(destroyed < std::chrono::milliseconds(150));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(sawCancel.load());
        CHECK(finished.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(queries.load() == 0);
    }

    src::setSdrPlayServiceQueryForTest(nullptr);
}

}  // namespace

int main() {
    testEveryStateHasItsSentence();
    testTheSummaryLine();
    testTheFallbackMatchers();
    testTheRestartCommandLine();
    testTheRealServiceControlManager();
    testTheStateIsLoggedOncePerChange();
    testWhenTheKeyIsShown();
    testWhatARestartLeadsTo();
    testTheRestartWorker();
    return testSummary("test_sdrplay_service");
}
