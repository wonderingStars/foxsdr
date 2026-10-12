// core/file_pick.hpp - the file dialog behind a PATH settings field's BROWSE key (0.99.74).
//
//   [A] placeholderNamesProgram: the one hint a PATH field carries.
//   [B] pickStartFolder: the dialog opens beside the current value, only if that folder is there.
//   [C] openFileFilters: the file-type list, Programs first only for a ".exe" hint.
//   [D] pickerCommand: the argv a Linux helper is run with - one element per argument, never a
//       shell command line.
//   [E] WINDOWS: THE REAL DIALOG. pickFile() is called for real, with an invisible window as its
//       owner, and a second thread drives the dialog the way a person would: reads its title,
//       its owner and the folder it opened in, then closes it (the person cancelled -> false and
//       no path) or types a file's name and presses Open (-> true and that file's UTF-8 path).
//       The dialog shows on this desktop for about a second.
//   [F] LINUX: THE REAL HELPER PROCESS. A script named zenity (then kdialog) on a PATH of the
//       test's own stands in for the program: filePickAvailable() finds it, pickFile() runs it
//       with the argv of [D] and no shell, reads the path it prints, and reads its exit status
//       as a cancel. With neither on PATH there is no dialog and no key. (UNVERIFIED ON WINDOWS:
//       only the Linux runners compile and run this section.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
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

#include <commdlg.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/file_pick.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::FilePickRequest;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path scratch() {
    const fs::path d = fs::temp_directory_path() / ("cascade-file-pick-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void testPlaceholder() {
    std::printf("[A] placeholderNamesProgram\n");
    using cascade::core::placeholderNamesProgram;
    CHECK(placeholderNamesProgram("C:\\tools\\tool.exe"));
    CHECK(placeholderNamesProgram("nrsc5.EXE"));
    CHECK(placeholderNamesProgram(".exe"));
    CHECK(placeholderNamesProgram("C:\\tools\\tool.exe  "));  // a trailing space is not part of it
    CHECK(!placeholderNamesProgram("/usr/bin/nrsc5"));
    CHECK(!placeholderNamesProgram("tool.exe.bak"));
    CHECK(!placeholderNamesProgram("exe"));
    CHECK(!placeholderNamesProgram(".ex"));
    CHECK(!placeholderNamesProgram(""));
    CHECK(!placeholderNamesProgram("   "));
}

void testStartFolder() {
    std::printf("[B] pickStartFolder\n");
    using cascade::core::pickStartFolder;
    const fs::path root = scratch();
    // "Zoë": a folder whose name is not ASCII must come back as the same UTF-8 bytes.
    const fs::path zoe = root / fs::path(std::u8string(u8"Zo\u00eb tools"));
    std::error_code ec;
    fs::create_directories(zoe, ec);
    CHECK(!ec);

    // The folder of a value whose folder exists.
    const std::string got = pickStartFolder(utf8(root / "nrsc5.exe"));
    CHECK(!got.empty());
    CHECK(!got.empty() && fs::equivalent(fs::path(std::u8string(got.begin(), got.end())), root, ec));
    // ...a non-ASCII one, byte for byte.
    CHECK(pickStartFolder(utf8(zoe / "nrsc5.exe")) == utf8(zoe));
    // A folder that is not there: nothing, so the dialog opens where the system would.
    CHECK(pickStartFolder(utf8(root / "no such folder" / "nrsc5.exe")).empty());
    // Not a folder at all (a FILE named like one).
    {
        std::ofstream(root / "afile") << "x";
        CHECK(pickStartFolder(utf8(root / "afile" / "nrsc5.exe")).empty());
    }
    // A bare name has no folder; an empty value has none either.
    CHECK(pickStartFolder("nrsc5.exe").empty());
    CHECK(pickStartFolder("").empty());
    fs::remove_all(root, ec);
}

void testFilters() {
    std::printf("[C] openFileFilters\n");
    using cascade::core::openFileFilters;
    FilePickRequest r;
    r.programsName = "Programs (*.exe)";
    r.allFilesName = "All files (*.*)";
    r.programs = true;
    auto f = openFileFilters(r);
    CHECK(f.size() == 2u);
    if (f.size() == 2u) {
        CHECK(f[0].first == "Programs (*.exe)" && f[0].second == "*.exe");  // selected when it opens
        CHECK(f[1].first == "All files (*.*)" && f[1].second == "*.*");
    }
    r.programs = false;
    f = openFileFilters(r);
    CHECK(f.size() == 1u);
    if (f.size() == 1u) { CHECK(f[0].first == "All files (*.*)" && f[0].second == "*.*"); }
    // A request with no words still lists something readable: the pattern.
    FilePickRequest bare;
    bare.programs = true;
    f = openFileFilters(bare);
    CHECK(f.size() == 2u);
    if (f.size() == 2u) {
        CHECK(f[0].first == "*.exe" && f[1].first == "*.*");
    }
}

void testCommands() {
    std::printf("[D] pickerCommand\n");
    using cascade::core::pickerCommand;
    using V = std::vector<std::string>;
    CHECK(pickerCommand("zenity", "Choose a file", "/home/u/bin") ==
          (V{"zenity", "--file-selection", "--title=Choose a file", "--filename=/home/u/bin/"}));
    // A folder that already ends in a slash gets no second one.
    CHECK(pickerCommand("zenity", "T", "/opt/")[3] == "--filename=/opt/");
    // No folder: no --filename at all.
    CHECK(pickerCommand("zenity", "T", "") == (V{"zenity", "--file-selection", "--title=T"}));
    CHECK(pickerCommand("kdialog", "Choose a file", "/home/u/bin") ==
          (V{"kdialog", "--title", "Choose a file", "--getopenfilename", "/home/u/bin"}));
    CHECK(pickerCommand("kdialog", "T", "") == (V{"kdialog", "--title", "T", "--getopenfilename"}));
    // A relative folder starting with '-' would be read as an option: kdialog is not given one.
    CHECK(pickerCommand("kdialog", "T", "-x") == (V{"kdialog", "--title", "T", "--getopenfilename"}));
    // A tool this does not know is not run.
    CHECK(pickerCommand("rm", "T", "/").empty());
    CHECK(pickerCommand("", "T", "/").empty());
    // NO SHELL: whatever a title or folder holds stays inside its one argument.
    const std::string nasty = "a b; rm -rf \"$HOME\" `x` 'y' \\";
    const V z = pickerCommand("zenity", nasty, "/tmp/dir with space;x");
    CHECK(z.size() == 4u);
    if (z.size() == 4u) {
        CHECK(z[2] == "--title=" + nasty);
        CHECK(z[3] == "--filename=/tmp/dir with space;x/");
    }
    const V k = pickerCommand("kdialog", nasty, "/tmp/dir with space;x");
    CHECK(k.size() == 5u);
    if (k.size() == 5u) {
        CHECK(k[2] == nasty);
        CHECK(k[4] == "/tmp/dir with space;x");
    }
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// [E] the real dialog
// ---------------------------------------------------------------------------

constexpr int kFileNameBox = 0x047c;  // the common dialog's "File name" box (dlgs.h: edt1)

struct Driver {
    DWORD guiThread = 0;     // the thread blocked inside pickFile()
    HWND owner = nullptr;
    bool choose = false;     // false: close the dialog; true: type `typed` and press Open
    std::wstring typed;      // what goes into the "File name" box
    // What it saw.
    std::atomic<bool> seen{false};
    std::wstring title;
    bool ownedByOwner = false;
    bool foundControls = false;
};

// The dialog's "File name" edit control and its Open button, found the way a person finds them:
// by what they are. The edit is an Edit somewhere under the control whose id is edt1; the
// button is the Button whose id is IDOK.
struct Controls {
    HWND edit = nullptr;
    HWND open = nullptr;
};

BOOL CALLBACK findControls(HWND w, LPARAM lp) {
    Controls* c = reinterpret_cast<Controls*>(lp);
    wchar_t cls[64] = {};
    ::GetClassNameW(w, cls, 64);
    if (std::wcscmp(cls, L"Button") == 0 && ::GetDlgCtrlID(w) == IDOK && c->open == nullptr) {
        c->open = w;
    }
    if (std::wcscmp(cls, L"Edit") == 0 && c->edit == nullptr) {
        for (HWND up = w; up != nullptr; up = ::GetParent(up)) {
            if (::GetDlgCtrlID(up) == kFileNameBox) {
                c->edit = w;
                break;
            }
        }
    }
    return TRUE;
}

BOOL CALLBACK findDialog(HWND w, LPARAM lp) {
    wchar_t cls[64] = {};
    ::GetClassNameW(w, cls, 64);
    if (::IsWindowVisible(w) && std::wcscmp(cls, L"#32770") == 0) {
        *reinterpret_cast<HWND*>(lp) = w;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK closeVisible(HWND w, LPARAM lp) {
    if (::IsWindowVisible(w) && w != reinterpret_cast<HWND>(lp)) { ::PostMessageW(w, WM_CLOSE, 0, 0); }
    return TRUE;
}

void drive(Driver* d) {
    HWND dlg = nullptr;
    for (int i = 0; i < 750 && dlg == nullptr; ++i) {  // up to 15 s
        ::EnumThreadWindows(d->guiThread, findDialog, reinterpret_cast<LPARAM>(&dlg));
        if (dlg == nullptr) { ::Sleep(20); }
    }
    if (dlg == nullptr) {
        // Never appeared. Close whatever the thread shows (not the owner) so the test FAILS
        // instead of waiting for ever on a dialog nobody will answer.
        ::EnumThreadWindows(d->guiThread, closeVisible, reinterpret_cast<LPARAM>(d->owner));
        return;
    }
    ::Sleep(300);  // its controls are created a moment after it is visible
    wchar_t buf[1024] = {};
    ::GetWindowTextW(dlg, buf, 1024);
    d->title = buf;
    d->ownedByOwner = ::GetWindow(dlg, GW_OWNER) == d->owner;
    d->seen = true;
    if (d->choose) {
        Controls c;
        ::EnumChildWindows(dlg, findControls, reinterpret_cast<LPARAM>(&c));
        d->foundControls = c.edit != nullptr && c.open != nullptr;
        if (d->foundControls) {
            ::SendMessageW(c.edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(d->typed.c_str()));
            ::Sleep(100);
            ::PostMessageW(c.open, BM_CLICK, 0, 0);
        }
    } else {
        ::PostMessageW(dlg, WM_CLOSE, 0, 0);
    }
    // A dialog still up two seconds later did not take the answer (a typed name that is not
    // there is answered by Windows with a message box of its own, over the dialog): close every
    // window the thread shows, the box first, until the dialog is gone - so the test reports a
    // wrong result instead of blocking.
    for (int i = 0; i < 100; ++i) {
        if (!::IsWindow(dlg)) { return; }
        ::Sleep(20);
    }
    for (int i = 0; i < 20 && ::IsWindow(dlg); ++i) {
        ::EnumThreadWindows(d->guiThread, closeVisible, reinterpret_cast<LPARAM>(d->owner));
        ::Sleep(250);
    }
}

void testWindowsDialog() {
    std::printf("[E] the real Windows dialog\n");
    const fs::path root = scratch();
    const fs::path chosen = root / fs::path(L"Zo\u00eb nrsc5.exe");
    { std::ofstream(chosen) << "MZ"; }

    // The window the dialog must be modal to: invisible, the owner the application passes.
    HWND owner = ::CreateWindowExW(0, L"STATIC", L"file pick owner", WS_POPUP, 0, 0, 10, 10, nullptr,
                                   nullptr, ::GetModuleHandleW(nullptr), nullptr);
    CHECK(owner != nullptr);

    FilePickRequest rq;
    rq.title = "FoxSDR pick-file test";
    rq.startFrom = utf8(chosen);  // opens in `root`
    rq.programs = true;
    rq.programsName = "Programs (*.exe)";
    rq.allFilesName = "All files (*.*)";
    rq.owner = owner;

    // THE PERSON CANCELS: false, no path, and the dialog was what we asked for.
    {
        Driver d;
        d.guiThread = ::GetCurrentThreadId();
        d.owner = owner;
        std::thread t(drive, &d);
        std::string path = "stale";
        const bool ok = cascade::core::pickFile(rq, path);
        t.join();
        CHECK(d.seen.load());
        CHECK(!ok);
        CHECK(path.empty());
        CHECK(d.title == L"FoxSDR pick-file test");
        CHECK(d.ownedByOwner);  // modal to the application window
    }

    // THE PERSON CHOOSES A FILE: true, and its path as UTF-8, accent and all. Only the file's
    // NAME is typed, not its folder: the dialog can only turn that into the full path if it
    // opened in the folder of the request's current value - which is how this proves the
    // start folder, the one thing the dialog gives no way to read back.
    {
        Driver d;
        d.guiThread = ::GetCurrentThreadId();
        d.owner = owner;
        d.choose = true;
        d.typed = L"Zo\xEB nrsc5.exe";  // "Zoë nrsc5.exe": the file made above
        std::thread t(drive, &d);
        std::string path;
        const bool ok = cascade::core::pickFile(rq, path);
        t.join();
        CHECK(d.seen.load());
        CHECK(d.foundControls);
        CHECK(ok);
        CHECK(path == utf8(chosen));
        std::printf("    chosen: %s\n", path.c_str());
    }

    // A START FOLDER THAT IS NOT THERE: the dialog still opens (where the system chooses), and
    // cancels cleanly.
    {
        Driver d;
        d.guiThread = ::GetCurrentThreadId();
        d.owner = owner;
        FilePickRequest elsewhere = rq;
        elsewhere.startFrom = utf8(root / "no such folder" / "x.exe");
        std::thread t(drive, &d);
        std::string path;
        const bool ok = cascade::core::pickFile(elsewhere, path);
        t.join();
        CHECK(d.seen.load());
        CHECK(!ok);
        CHECK(path.empty());
    }

    if (owner != nullptr) { ::DestroyWindow(owner); }
    std::error_code ec;
    fs::remove_all(root, ec);
}

#else

// ---------------------------------------------------------------------------
// [F] the real helper process
// ---------------------------------------------------------------------------

void writeScript(const fs::path& p, const std::string& body) {
    { std::ofstream(p) << body; }
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                           fs::perms::others_read | fs::perms::others_exec, ec);
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testLinuxHelper() {
    std::printf("[F] the helper process (a script standing in for zenity, then kdialog)\n");
    const fs::path root = scratch();
    const fs::path bin = root / "bin";
    const fs::path startDir = root / "start dir";
    const fs::path argsFile = root / "args.txt";
    std::error_code ec;
    fs::create_directories(bin, ec);
    fs::create_directories(startDir, ec);
    // Records its arguments, one per line, then prints FAKE_CHOICE and exits 0, or - with none -
    // exits 1 as zenity and kdialog do when the dialog is cancelled.
    const std::string body =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$FAKE_ARGS\"\n"
        "if [ -n \"$FAKE_CHOICE\" ]; then printf '%s\\n' \"$FAKE_CHOICE\"; exit 0; fi\n"
        "exit 1\n";
    const char* oldPath = std::getenv("PATH");
    const std::string saved = oldPath != nullptr ? oldPath : "";
    ::setenv("FAKE_ARGS", argsFile.string().c_str(), 1);

    FilePickRequest rq;
    rq.title = "Choose a file";
    rq.startFrom = (startDir / "nrsc5").string();

    // NEITHER ON PATH: no dialog, so no key, and a press (if there were one) is a cancel.
    ::setenv("PATH", bin.string().c_str(), 1);
    CHECK(!cascade::core::filePickAvailable());
    {
        std::string path = "stale";
        CHECK(!cascade::core::pickFile(rq, path));
        CHECK(path.empty());
    }

    // ZENITY: found, run with the argv of [D] and no shell, its output read as the path.
    writeScript(bin / "zenity", body);
    CHECK(cascade::core::filePickAvailable());
    ::setenv("FAKE_CHOICE", "/opt/nrsc5 dir/bin/nrsc5", 1);
    {
        std::string path;
        CHECK(cascade::core::pickFile(rq, path));
        CHECK(path == "/opt/nrsc5 dir/bin/nrsc5");
        const std::string want = "--file-selection\n--title=Choose a file\n--filename=" +
                                 startDir.string() + "/\n";
        CHECK(readAll(argsFile) == want);
    }
    // ...cancelled (exit 1, nothing printed): false and no path.
    ::unsetenv("FAKE_CHOICE");
    {
        std::string path = "stale";
        CHECK(!cascade::core::pickFile(rq, path));
        CHECK(path.empty());
    }

    // KDIALOG when zenity is absent.
    fs::remove(bin / "zenity", ec);
    writeScript(bin / "kdialog", body);
    CHECK(cascade::core::filePickAvailable());
    ::setenv("FAKE_CHOICE", "/usr/bin/nrsc5", 1);
    {
        std::string path;
        CHECK(cascade::core::pickFile(rq, path));
        CHECK(path == "/usr/bin/nrsc5");
        const std::string want = "--title\nChoose a file\n--getopenfilename\n" + startDir.string() + "\n";
        CHECK(readAll(argsFile) == want);
    }

    ::setenv("PATH", saved.c_str(), 1);
    ::unsetenv("FAKE_CHOICE");
    ::unsetenv("FAKE_ARGS");
    fs::remove_all(root, ec);
}

#endif

}  // namespace

int main() {
    std::printf("test_file_pick\n");
    testPlaceholder();
    testStartFolder();
    testFilters();
    testCommands();
#if defined(_WIN32)
    testWindowsDialog();
    std::printf("[F] the Linux helper process: not on this platform\n");
#else
    std::printf("[E] the real Windows dialog: not on this platform\n");
    testLinuxHelper();
#endif
    return testSummary("test_file_pick");
}
