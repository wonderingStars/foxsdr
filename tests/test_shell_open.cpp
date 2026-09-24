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
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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

}  // namespace

int main() {
    checkBracketOrder();
    checkResultPassesThrough();
    checkResumeOnThrow();
    checkWatchdogPauseCounting();
#if defined(_WIN32)
    checkInstallerPathNonAscii();
#endif
    return testSummary("test_shell_open");
}
