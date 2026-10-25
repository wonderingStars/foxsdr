// A shell call made from the GUI thread is entered under a watchdog pause.
//
// WHAT THIS IS FOR. Field report "hang ntdll.dll @
// cascade::gui::AppWindow::launchInstaller" (0.96.2, Windows 11 26200, 28 s
// uptime): the user pressed the in-app update key, ShellExecuteW blocked inside
// Windows.Storage.dll waiting for the elevation / SmartScreen prompt to be
// answered, and the hang watchdog filed a report against the dialog the user
// was reading.
//
// core/hang_watchdog.hpp's false-positive rule 2b already names the mechanism -
// "a native modal dialog ... MUST take one too" - so the fix is a WatchdogPause
// around every shell call. What a test can reach is the bracket itself:
// gui::runShellOpen is the one place the call sites go through, and the
// properties below are the ones that rot silently.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/hang_watchdog.hpp"
#include "gui/shell_open.hpp"

#include "test_check.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
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
#include <objbase.h>
#include <shellapi.h>
#endif

namespace {

// THE ORDER IS THE PROPERTY. "pause was called" and "the call happened" are
// both true of an implementation that pauses AFTER the shell has already
// blocked, which would fix nothing at all.
void checkBracketOrder() {
    std::vector<std::string> log;
    cascade::gui::ShellPauseHooks hooks;
    hooks.pause = [&log] { log.push_back("pause"); };
    hooks.resume = [&log] { log.push_back("resume"); };

    const bool ok = cascade::gui::runShellOpen(hooks, [&log] {
        log.push_back("shell");
        return true;
    });

    CHECK(ok);
    const std::vector<std::string> want{"pause", "shell", "resume"};
    CHECK(log == want);
    if (log != want) {
        std::string got;
        for (const std::string& s : log) { got += s + " "; }
        std::printf("FAIL bracket order was: %s\n", got.c_str());
    }
}

// The return value is the shell's, not the bracket's: launchInstaller's caller
// shows "could not start the installer" off exactly this boolean.
void checkResultPassesThrough() {
    cascade::gui::ShellPauseHooks hooks;
    CHECK(cascade::gui::runShellOpen(hooks, [] { return true; }));
    CHECK(!cascade::gui::runShellOpen(hooks, [] { return false; }));
    // An empty call is a failure, not a crash.
    CHECK(!cascade::gui::runShellOpen(hooks, nullptr));
}

// A PAUSE THAT IS NEVER RELEASED DISARMS THE WATCHDOG FOR THE REST OF THE
// SESSION, because HangWatchdog counts its pauses and only re-arms when the
// count returns to zero. So the resume has to survive the throwing path.
void checkResumeOnThrow() {
    int paused = 0;
    int resumed = 0;
    cascade::gui::ShellPauseHooks hooks;
    hooks.pause = [&paused] { ++paused; };
    hooks.resume = [&resumed] { ++resumed; };

    bool threw = false;
    try {
        (void)cascade::gui::runShellOpen(hooks, []() -> bool {
            throw std::runtime_error("the shell threw");
        });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(paused == 1);
    CHECK(resumed == 1);
}

// AND THE COUNTING RULE THE BRACKET RELIES ON. HangWatchdog::pause/resume nest,
// so an inner pause cannot un-pause an outer one - a shell call made from
// inside a plugin rescan (which already holds one) must leave the rescan's
// pause standing. pausesTaken() counts from process start and never decreases,
// which is what `cascade --frames N` prints.
void checkWatchdogPauseCounting() {
    cascade::core::HangWatchdog w;
    const unsigned before = w.pausesTaken();

    {
        cascade::core::WatchdogPause outer(w);
        CHECK(w.pausesTaken() == before + 1u);
        {
            cascade::core::WatchdogPause inner(w);
            CHECK(w.pausesTaken() == before + 2u);
        }
        // The inner guard is gone; the outer one is not, and the count of
        // pauses TAKEN does not go backwards when one is released.
        CHECK(w.pausesTaken() == before + 2u);
    }
    CHECK(w.pausesTaken() == before + 2u);

    // The same through the hooks the shell path uses, so the seam and the
    // watchdog are proved against each other rather than each against a mock.
    cascade::gui::ShellPauseHooks hooks;
    hooks.pause = [&w] { w.pause(); };
    hooks.resume = [&w] { w.resume(); };
    CHECK(cascade::gui::runShellOpen(hooks, [] { return true; }));
    CHECK(w.pausesTaken() == before + 3u);
}

#if defined(_WIN32)
// THE INSTALLER'S PATH MUST NAME THE FILE THAT WAS DOWNLOADED (bug hunt
// 2026-09-24, updater-installer-2). core::downloadUpdate returns
// fs::path::string() - narrowed through the ANSI code page - and
// launchInstaller used to widen it by copying each byte into a wchar_t, so any
// non-ASCII character in %TEMP% (the profile folder is named after the
// account: "Jose" with an acute e, "Muller" with an umlaut) became a garbage
// code unit and ShellExecuteW was handed a path that names nothing.
//
// A REAL FILE, narrowed exactly the way downloadUpdate narrows it, so the check
// is "does the widened path open the file that is there" rather than a
// comparison against a string this test invented.
void checkInstallerPathNonAscii() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) /
                         (L"foxsdr_shellpath_Jos\u00e9_M\u00fcller_" +
                          std::to_wstring(::GetCurrentProcessId()));
    fs::create_directories(dir, ec);
    const fs::path file = dir / L"foxsdr-setup-9.9.9.exe";
    { std::ofstream(file, std::ios::binary) << "not really an installer"; }
    CHECK(fs::exists(file, ec));

    // What downloadUpdate hands back in outPath (updater.cpp: dest.string()).
    const std::string narrow = file.string();
    const std::wstring wide = cascade::gui::installerPathWide(narrow);
    CHECK(wide == file.wstring());
    const bool found = ::GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
    CHECK(found);
    if (!found) { std::printf("  the widened installer path names no file on disk\n"); }

    // ASCII is unchanged, and an empty path is empty - which launchInstaller
    // answers with "could not start" rather than a ShellExecuteW of nothing.
    CHECK(cascade::gui::installerPathWide("C:\\Temp\\foxsdr-setup-1.0.0.exe") ==
          L"C:\\Temp\\foxsdr-setup-1.0.0.exe");
    CHECK(cascade::gui::installerPathWide("").empty());

    fs::remove_all(dir, ec);
}
#endif

// ---------------------------------------------------------------------------
// runShellOpenResponsive (0.99.91): the same contract, the call on a thread of its own.
// Whether the window stays responsive meanwhile is measured with a real window and a real
// watchdog in test_hung_window_excuse.cpp (cases 5 and 6); this holds everything else.
// ---------------------------------------------------------------------------

// The bracket is taken and released on the CALLING thread - the watchdog's GUI thread, whose
// frame timer the hooks also tell - and the call runs between them on another thread.
void checkResponsiveBracketAndThreads() {
    std::mutex m;
    std::vector<std::string> log;
    std::thread::id pauseOn, callOn, resumeOn;
    cascade::gui::ShellPauseHooks hooks;
    hooks.pause = [&] {
        std::lock_guard<std::mutex> lock(m);
        log.push_back("pause");
        pauseOn = std::this_thread::get_id();
    };
    hooks.resume = [&] {
        std::lock_guard<std::mutex> lock(m);
        log.push_back("resume");
        resumeOn = std::this_thread::get_id();
    };
    const bool ok = cascade::gui::runShellOpenResponsive(hooks, [&] {
        std::lock_guard<std::mutex> lock(m);
        log.push_back("shell");
        callOn = std::this_thread::get_id();
        return true;
    });
    CHECK(ok);
    CHECK((log == std::vector<std::string>{"pause", "shell", "resume"}));
    CHECK(pauseOn == std::this_thread::get_id());
    CHECK(resumeOn == std::this_thread::get_id());
#if defined(_WIN32)
    CHECK(callOn != std::this_thread::get_id());
#else
    CHECK(callOn == std::this_thread::get_id());  // off Windows it is runShellOpen
#endif
}

void checkResponsiveResultAndThrow() {
    cascade::gui::ShellPauseHooks none;
    CHECK(cascade::gui::runShellOpenResponsive(none, [] { return true; }));
    CHECK(!cascade::gui::runShellOpenResponsive(none, [] { return false; }));
    CHECK(!cascade::gui::runShellOpenResponsive(none, nullptr));

    // An exception thrown on the worker comes back as itself, and the pause is released.
    int paused = 0;
    int resumed = 0;
    cascade::gui::ShellPauseHooks hooks;
    hooks.pause = [&paused] { ++paused; };
    hooks.resume = [&resumed] { ++resumed; };
    std::string what;
    try {
        (void)cascade::gui::runShellOpenResponsive(hooks, []() -> bool {
            throw std::runtime_error("the shell threw, off the GUI thread");
        });
    } catch (const std::runtime_error& e) {
        what = e.what();
    }
    CHECK(what == "the shell threw, off the GUI thread");
    CHECK(paused == 1 && resumed == 1);
}

#if defined(_WIN32)
// Posted input and quit requests belong to the outer frame loop.
void checkResponsivePostedMessages() {
    MSG message{};
    ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE); // create this thread's queue
    const DWORD caller = ::GetCurrentThreadId();
    cascade::gui::ShellPauseHooks none;
    CHECK(cascade::gui::runShellOpenResponsive(none, [caller] {
        const bool posted = ::PostThreadMessageW(caller, WM_APP + 42, 123, 456) != FALSE;
        ::Sleep(300); // allow several iterations of the waiting pump
        return posted;
    }));
    CHECK(::PeekMessageW(&message, nullptr, WM_APP + 42, WM_APP + 42, PM_REMOVE) != FALSE);
    CHECK(message.message == WM_APP + 42 && message.wParam == 123 && message.lParam == 456);
    ::PostQuitMessage(17);
    CHECK(cascade::gui::runShellOpenResponsive(none, [] { ::Sleep(300); return true; }));
    message = {};
    CHECK(::PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE);
    CHECK(message.message == WM_QUIT && message.wParam == 17);
}

// THE WORKER IS AN STA, as ShellExecute's documentation asks ("CoInitializeEx(NULL,
// COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)"): some shell extensions need one.
void checkResponsiveApartment() {
    APTTYPE type = APTTYPE_MTA;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    HRESULT hr = E_FAIL;
    cascade::gui::ShellPauseHooks none;
    (void)cascade::gui::runShellOpenResponsive(none, [&] {
        hr = ::CoGetApartmentType(&type, &qualifier);
        return true;
    });
    CHECK(SUCCEEDED(hr));
    CHECK(type == APTTYPE_STA || type == APTTYPE_MAINSTA);
}

// REAL SHELL CALLS through it, shaped like the two in the product: a refusal whose reason is
// read with GetLastError inside the call (the radio setup's INSTALL DRIVER reads it there), and
// a launch that starts a process (the installer key). Neither shows anything: the refusal is a
// file that does not exist with SEE_MASK_FLAG_NO_UI, the launch a hidden cmd.exe that exits 7.
void checkResponsiveRealShellExecute() {
    cascade::gui::ShellPauseHooks none;
    DWORD error = 0;
    const bool refused = !cascade::gui::runShellOpenResponsive(none, [&]() -> bool {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = L"C:\\foxsdr-no-such-folder\\no-such-installer.exe";
        sei.nShow = SW_HIDE;
        const bool ok = ::ShellExecuteExW(&sei) != FALSE;
        if (!ok) { error = ::GetLastError(); }
        return ok;
    });
    CHECK(refused);
    CHECK(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
    if (!refused || (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)) {
        std::printf("  refusal: Windows error %lu\n", static_cast<unsigned long>(error));
    }

    wchar_t cmd[MAX_PATH] = {0};
    ::GetSystemDirectoryW(cmd, MAX_PATH);
    const std::wstring cmdExe = std::wstring(cmd) + L"\\cmd.exe";
    HANDLE process = nullptr;
    const bool started = cascade::gui::runShellOpenResponsive(none, [&]() -> bool {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = cmdExe.c_str();
        sei.lpParameters = L"/c exit 7";
        sei.nShow = SW_HIDE;
        const bool ok = ::ShellExecuteExW(&sei) != FALSE;
        process = sei.hProcess;
        return ok;
    });
    CHECK(started && process != nullptr);
    if (process != nullptr) {
        CHECK(::WaitForSingleObject(process, 10000) == WAIT_OBJECT_0);
        DWORD code = 0;
        ::GetExitCodeProcess(process, &code);
        CHECK(code == 7u);
        ::CloseHandle(process);
    }
}
#endif

}  // namespace

int main() {
    checkBracketOrder();
    checkResultPassesThrough();
    checkResumeOnThrow();
    checkWatchdogPauseCounting();
#if defined(_WIN32)
    checkInstallerPathNonAscii();
#endif
    checkResponsiveBracketAndThreads();
    checkResponsiveResultAndThrow();
#if defined(_WIN32)
    checkResponsivePostedMessages();
    checkResponsiveApartment();
    checkResponsiveRealShellExecute();
#endif
    return testSummary("test_shell_open");
}
