// Tests for the installer hold (security review R1, finding F1, 0.99.114): core/updater.hpp recordVerifiedInstaller / holdVerifiedInstaller / InstallerHold.
//
// downloadUpdate hashes the installer and leaves it under a predictable name in the user's temp folder until the user presses "Install now and restart". The two
// launchers (gui/app_window.cpp launchInstaller, slint/host_duties.cpp shellOpenInstaller) now call holdVerifiedInstaller right before ShellExecute and keep the hold
// across the call. Stand-in files only: nothing here is an installer, and the one process these tests start is a copy of a small system program that prints the
// computer's name and exits.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/sys_hash.hpp"
#include "core/updater.hpp"
#include "test_check.hpp"

namespace {

namespace fs = std::filesystem;

// ---- F1 (review R1, 0.99.114): THE VERIFIED INSTALLER CANNOT BE SWAPPED BETWEEN THE CHECK AND THE START ------------------------------
// downloadUpdate hashes the installer and leaves it under a predictable name in the user's temp folder until the user presses
// "Install now and restart" - an unbounded wait. holdVerifiedInstaller is what the two launchers call right before ShellExecute: it
// re-hashes the file through a handle that stays open (read-only sharing: nobody else can write, delete or rename the file while it is
// held) and refuses a file that is not the one that was verified. Stand-in files only; nothing here is an installer.
std::string readWhole(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeWhole(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

#ifdef _WIN32
// Open `p` the way a swapping process would; the Win32 error, 0 when it opened.
DWORD openFor(const fs::path& p, DWORD access) {
    HANDLE h = ::CreateFileW(p.wstring().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return ::GetLastError(); }
    ::CloseHandle(h);
    return 0;
}
#endif

void testTheInstallerIsReCheckedAndHeldBeforeItRuns() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-test-installer-hold-" + std::to_string(TEST_GETPID()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "foxsdr-setup-9.9.9.exe";
    const std::string good = "stand-in installer bytes, version nine";
    const std::string evil = "stand-in installer bytes, SWAPPED here";
    const std::string sha = cascade::core::sys::sha256Hex(good);

    // A path this process never verified is not started (the launchers take a bare path from the UI).
    writeWhole(file, good);
    {
        cascade::core::InstallerHold hold;
        std::string err;
        CHECK(!cascade::core::holdVerifiedInstaller(file.string(), hold, err));
        CHECK(!hold.held());
        CHECK(!err.empty());
    }

    // The file that was verified is accepted, and held.
    cascade::core::recordVerifiedInstaller(file.string(), sha);
    {
        cascade::core::InstallerHold hold;
        std::string err;
        CHECK(cascade::core::holdVerifiedInstaller(file.string(), hold, err));
        CHECK(hold.held());
#ifdef _WIN32
        // While it is held nobody can write, delete or rename it - a read is still allowed (the shell and the loader read it).
        CHECK(openFor(file, GENERIC_WRITE) == ERROR_SHARING_VIOLATION);
        CHECK(openFor(file, DELETE) == ERROR_SHARING_VIOLATION);
        CHECK(!::MoveFileW(file.wstring().c_str(), (dir / "moved.exe").wstring().c_str()));
        CHECK(!::DeleteFileW(file.wstring().c_str()));
        CHECK(openFor(file, GENERIC_READ) == 0);
#endif
        CHECK(readWhole(file) == good);
    }
#ifdef _WIN32
    CHECK(openFor(file, GENERIC_WRITE) == 0);   // released with the hold
#endif

    // Swapped after the download, before the press: refused, and nothing is left held.
    writeWhole(file, evil);
    {
        cascade::core::InstallerHold hold;
        std::string err;
        CHECK(!cascade::core::holdVerifiedInstaller(file.string(), hold, err));
        CHECK(!hold.held());
        CHECK(!err.empty());
    }
#ifdef _WIN32
    CHECK(openFor(file, GENERIC_WRITE) == 0);
#endif

    // Replaced by deleting it and writing another file of the same length under the same name: still refused.
    writeWhole(file, good);
    fs::remove(file, ec);
    writeWhole(file, std::string(good.size(), 'x'));
    {
        cascade::core::InstallerHold hold;
        std::string err;
        CHECK(!cascade::core::holdVerifiedInstaller(file.string(), hold, err));
        CHECK(!hold.held());
    }

    // Gone: refused, not a crash.
    fs::remove(file, ec);
    {
        cascade::core::InstallerHold hold;
        std::string err;
        CHECK(!cascade::core::holdVerifiedInstaller(file.string(), hold, err));
        CHECK(!hold.held());
    }
    fs::remove_all(dir, ec);
}

#ifdef _WIN32
// The hold must not stop the shell from starting the file (the same ShellExecuteEx the launchers call), or the fix would break "Install now".
// A harmless stand-in: a copy of a small system program that prints the computer's name and exits; started hidden, waited for.
void testAHeldStandInStillStartsThroughTheShell() {
    std::error_code ec;
    wchar_t sysdir[MAX_PATH] = {0};
    CHECK(::GetSystemDirectoryW(sysdir, MAX_PATH) > 0);
    const fs::path source = fs::path(sysdir) / "hostname.exe";
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-test-installer-start-" + std::to_string(TEST_GETPID()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path file = dir / "foxsdr-setup-9.9.9.exe";
    fs::copy_file(source, file, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    const std::string sha = cascade::core::sys::sha256Hex(readWhole(file));
    cascade::core::recordVerifiedInstaller(file.string(), sha);

    cascade::core::InstallerHold hold;
    std::string err;
    CHECK(cascade::core::holdVerifiedInstaller(file.string(), hold, err));
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring wide = file.wstring();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = wide.c_str();
    sei.nShow = SW_HIDE;
    const bool started = ::ShellExecuteExW(&sei) != FALSE;
    CHECK(started);
    if (started && sei.hProcess != nullptr) {
        CHECK(::WaitForSingleObject(sei.hProcess, 20000) == WAIT_OBJECT_0);
        DWORD code = 99;
        ::GetExitCodeProcess(sei.hProcess, &code);
        CHECK(code == 0);
        ::CloseHandle(sei.hProcess);
    }
    if (SUCCEEDED(com)) { ::CoUninitialize(); }
    hold = cascade::core::InstallerHold();   // released
    fs::remove_all(dir, ec);
}
#endif

}  // namespace

int main() {
    testTheInstallerIsReCheckedAndHeldBeforeItRuns();
#ifdef _WIN32
    testAHeldStandInStillStartsThroughTheShell();
#endif
    return testSummary("test_installer_hold");
}
