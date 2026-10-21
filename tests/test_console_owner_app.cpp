// A launch that is not from a terminal leaves no console behind it, through the REAL
// cascade.exe (0.99.69, core/console_owner.hpp).
//
// The owner's requirement is no black window, not a shorter one. Releasing the
// console from main() was measured first and left a Windows Terminal window visible
// for about 255 ms - the console host is made by process start-up, before main -
// so the property under test is the strong one: the process never HAS a console
// when it is started the way the Start Menu starts it. Three ways of asking, in
// the order of how little they depend on the machine:
//
//   1. The image's subsystem field is WINDOWS_GUI (a console-subsystem image gets
//      a console from the loader before its first instruction; nothing in main
//      can undo that in time).
//   2. AttachConsole(pid) FAILS while the application runs and at the end: it only
//      succeeds for a process that has a console to attach to. Asked every
//      millisecond for the first two and a half seconds - past the 450-odd ms
//      before main - by a process that has no console of its own, which is what
//      Explorer is. That is the part that needs a helper: AttachConsole refuses a
//      caller that already has a console, and a test run from a terminal has one.
//      So this program starts ITSELF, with DETACHED_PROCESS, as the console-less
//      parent ("--launch"), and the helper starts the application the way
//      Explorer does (CreateProcess, no console flags, no inherited handles).
//   3. Where there is a desktop to look at, no console window owned by the
//      application (a ConsoleWindowClass or the PseudoConsoleWindow of its console)
//      appears during the run. Windows Terminal's own window belongs to a
//      WindowsTerminal.exe and cannot be pinned on the application on a machine
//      where other consoles come and go, so those are counted and reported, not
//      asserted on. On a session with no interactive desktop the enumeration sees
//      nothing and says so; 1 and 2 do not need one.
//
// A second scenario has the parent ASK for a console of its own (CREATE_NEW_CONSOLE,
// window hidden, which is what `start` does): a windows-subsystem image is not given
// one either, which is why the application has no FreeConsole to call.
//
// A third is the other half of the bargain: started from something that HAS a console
// (a windowless stand-in for a shell), `--version` and an unknown argument still
// print there, and still exit 0 and 1. ctest cannot see that on its own - it reads a
// pipe, which needs neither AttachConsole nor the CRT rebinding - so the screen is
// read back through the console API.
//
// The optional first argument is the cascade.exe to test, so the same test can be
// pointed at an older build (a console-subsystem one must fail it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

#if defined(_WIN32)

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The PE header's Subsystem field (IMAGE_SUBSYSTEM_*), or 0 if it cannot be read.
unsigned peSubsystem(const std::string& exe) {
    std::ifstream in(exe, std::ios::binary);
    if (!in) { return 0; }
    IMAGE_DOS_HEADER dos{};
    in.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!in || dos.e_magic != IMAGE_DOS_SIGNATURE) { return 0; }
    in.seekg(dos.e_lfanew);
    IMAGE_NT_HEADERS64 nt{};
    in.read(reinterpret_cast<char*>(&nt), sizeof(nt));
    if (!in || nt.Signature != IMAGE_NT_SIGNATURE) { return 0; }
    return nt.OptionalHeader.Subsystem;
}

// Top-level windows that were not there before and are a console's. A console
// window can be attributed to the application when its owner IS the application
// (measured on a console-subsystem build: the console's PseudoConsoleWindow, and a
// classic host's ConsoleWindowClass, are owned by the client process; Windows
// Terminal's own CASCADIA_HOSTING_WINDOW_CLASS belongs to a WindowsTerminal.exe).
// The others are counted but not blamed: this machine runs other consoles
// (other test runs, a developer's terminals) while the suite does.
struct WindowScan {
    const std::set<HWND>* baseline = nullptr;
    DWORD appPid = 0;
    std::set<HWND> consoleWindows;  // new, console class, owned by the application
    std::set<HWND> foreignConsoleWindows;  // new, visible, console class, someone else's
    std::size_t topLevel = 0;
};
BOOL CALLBACK scanWindow(HWND h, LPARAM lp) {
    auto* s = reinterpret_cast<WindowScan*>(lp);
    ++s->topLevel;
    if (s->baseline->count(h) != 0) { return TRUE; }
    wchar_t cls[96] = {};
    ::GetClassNameW(h, cls, 95);
    DWORD owner = 0;
    ::GetWindowThreadProcessId(h, &owner);
    const bool consoleClass = std::wcscmp(cls, L"ConsoleWindowClass") == 0 ||
                              std::wcscmp(cls, L"PseudoConsoleWindow") == 0;
    if (owner == s->appPid && consoleClass) {
        s->consoleWindows.insert(h);
    } else if (::IsWindowVisible(h) && (consoleClass || std::wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0)) {
        s->foreignConsoleWindows.insert(h);
    }
    return TRUE;
}
BOOL CALLBACK collectWindow(HWND h, LPARAM lp) {
    reinterpret_cast<std::set<HWND>*>(lp)->insert(h);
    return TRUE;
}

// ---------------------------------------------------------------------------
// THE HELPER: a process with no console, which starts cascade.exe and watches it.
// Prints what it saw on stdout (a pipe to the driver) and exits 0 when the
// scenario's property held.
// ---------------------------------------------------------------------------
// The text on this console's screen, line by line up to the cursor.
std::string screenText() {
    HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) { return std::string(); }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    std::string text;
    if (::GetConsoleScreenBufferInfo(out, &info) != 0) {
        for (SHORT y = 0; y <= info.dwCursorPosition.Y; ++y) {
            std::vector<char> line(static_cast<std::size_t>(info.dwSize.X));
            DWORD n = 0;
            COORD at{0, y};
            if (::ReadConsoleOutputCharacterA(out, line.data(), static_cast<DWORD>(line.size()), at, &n) == 0) {
                break;
            }
            std::string row(line.data(), n);
            while (!row.empty() && row.back() == ' ') { row.pop_back(); }
            text += row;
            text += '\n';
        }
    }
    ::CloseHandle(out);
    return text;
}

void clearScreen() {
    HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE) { return; }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (::GetConsoleScreenBufferInfo(out, &info) != 0) {
        DWORD w = 0;
        ::FillConsoleOutputCharacterA(out, ' ', static_cast<DWORD>(info.dwSize.X) * info.dwSize.Y,
                                      COORD{0, 0}, &w);
        ::SetConsoleCursorPosition(out, COORD{0, 0});
    }
    ::CloseHandle(out);
}

// THE TERMINAL CASE, the one the tool modes exist for: a process WITH a console (a
// stand-in for a shell; windowless, so nothing appears on the desktop) starts the
// tool the way PowerShell and cmd.exe start a windows-subsystem program - no
// inherited handles, so its standard handles are null - waits for it, and reads what
// the console screen shows. The text is only there if the application borrowed the
// console AND bound its CRT streams to it; a pipe the caller redirected to would
// work without either, which is why ctest cannot see this.
int terminalMain(const std::string& exe) {
    DWORD pids[1] = {};
    if (::GetConsoleProcessList(pids, 1) == 0) {
        std::printf("helper [terminal]: this process has no console to stand in for a shell\n");
        return 2;
    }
    struct Case {
        const char* args;
        DWORD exitCode;
        const char* text;
    };
    const Case cases[] = {
        {"--version", 0, "FoxSDR "},
        {"--bogus", 1, "unknown argument '--bogus'"},
    };
    bool ok = true;
    for (const Case& c : cases) {
        clearScreen();
        std::wstring cmd = L"\"" + widen(exe) + L"\" " + widen(c.args);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) == 0) {
            std::printf("helper [terminal]: CreateProcess failed (%lu)\n", static_cast<unsigned long>(::GetLastError()));
            return 3;
        }
        ::CloseHandle(pi.hThread);
        const DWORD waited = ::WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 0xDEAD;
        ::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hProcess);
        const std::string screen = screenText();
        const bool seen = screen.find(c.text) != std::string::npos;
        std::printf("helper [terminal]: `%s` exit code %lu, on the console screen: [%s] -> %s\n", c.args,
                    static_cast<unsigned long>(code),
                    screen.substr(0, screen.find('\n')).c_str(), seen ? "printed" : "NOTHING PRINTED");
        ok = ok && waited == WAIT_OBJECT_0 && code == c.exitCode && seen;
    }
    return ok ? 0 : 1;
}

int helperMain(const std::string& mode, const std::string& exe, const fs::path& dir) {
    if (mode == "terminal") { return terminalMain(exe); }
    const bool newConsole = (mode == "newconsole");
    DWORD pids[1] = {};
    if (::GetConsoleProcessList(pids, 1) != 0) {
        std::printf("helper: this process has a console, so AttachConsole cannot be asked\n");
        return 2;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", (dir / "config.json").string().c_str());
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", nullptr);

    std::set<HWND> baseline;
    ::EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&baseline));

    // `--frames 1000000` is a session that runs until ended: long enough to poll,
    // and a plain-session command line for the decision.
    std::wstring cmd = L"\"" + widen(exe) + L"\" --frames 1000000";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (newConsole) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};
    // Explorer's way: no console flag, no inherited handles (so the standard
    // handles come back null), the parent has no console.
    const Clock::time_point t0 = Clock::now();
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                         newConsole ? CREATE_NEW_CONSOLE : 0, nullptr, nullptr, &si, &pi) == 0) {
        std::printf("helper: CreateProcess failed (%lu)\n", static_cast<unsigned long>(::GetLastError()));
        return 3;
    }
    ::CloseHandle(pi.hThread);

    const fs::path logPath = dir / "logs" / "foxsdr.log";
    double firstConsole = -1.0;
    double lastConsole = -1.0;
    std::set<HWND> consoleWindows;
    std::set<HWND> foreignConsoleWindows;
    std::size_t topLevelMax = 0;
    bool loopStarted = false;
    // The decision is logged just before the application object exists.
    for (;;) {
        const double t = msSince(t0);
        if (::AttachConsole(pi.dwProcessId) != 0) {
            // It has a console. Let go of it again at once, so the next question
            // is asked from a console-less process too.
            ::FreeConsole();
            if (firstConsole < 0.0) { firstConsole = t; }
            lastConsole = t;
        }
        WindowScan scan;
        scan.baseline = &baseline;
        scan.appPid = pi.dwProcessId;
        ::EnumWindows(&scanWindow, reinterpret_cast<LPARAM>(&scan));
        consoleWindows.insert(scan.consoleWindows.begin(), scan.consoleWindows.end());
        foreignConsoleWindows.insert(scan.foreignConsoleWindows.begin(), scan.foreignConsoleWindows.end());
        if (scan.topLevel > topLevelMax) { topLevelMax = scan.topLevel; }
        if (!loopStarted && contains(readFile(logPath), "frame loop starting")) { loopStarted = true; }
        if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { break; }
        // A start-up's worth of polling at 1 ms (the console host appears within
        // the first half second), then gently until the loop has been drawing.
        if (t < 2500.0) {
            ::Sleep(1);
        } else if (loopStarted) {
            break;
        } else if (t > 60000.0) {
            break;
        } else {
            ::Sleep(25);
        }
    }
    // The process has been running for the whole of that: does it have a console NOW?
    const bool alive = ::WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0;
    bool finalConsole = false;
    if (::AttachConsole(pi.dwProcessId) != 0) {
        finalConsole = true;
        ::FreeConsole();
    }
    const std::string log = readFile(logPath);
    if (alive) { ::TerminateProcess(pi.hProcess, 99); }
    ::WaitForSingleObject(pi.hProcess, 10000);
    ::CloseHandle(pi.hProcess);

    const std::size_t at = log.find("console: ");
    const std::string consoleLine =
        at == std::string::npos ? std::string("(no console: line in the log)")
                                : log.substr(at, log.find('\n', at) - at);
    std::printf("helper [%s]: AttachConsole(application) succeeded %s (first %.0f ms, last %.0f ms); "
                "at the end: %s\n",
                mode.c_str(), firstConsole < 0.0 ? "never" : "at least once", firstConsole, lastConsole,
                finalConsole ? "it has a console" : "it has no console");
    std::printf("helper [%s]: console windows owned by the application: %zu; other new console windows on the "
                "desktop, not blamed on it: %zu (top-level windows seen: %zu%s)\n",
                mode.c_str(), consoleWindows.size(), foreignConsoleWindows.size(), topLevelMax,
                topLevelMax == 0 ? ", no interactive desktop here - this check saw nothing" : "");
    std::printf("helper [%s]: the application's log says: %s\n", mode.c_str(), consoleLine.c_str());

    // No console at any poll, none at the end, no console window of the application's,
    // and the application saw there was none to borrow - in both modes: a windows-subsystem
    // image is not given a console even by a parent that asks for a new one.
    const bool ok = alive && !finalConsole && firstConsole < 0.0 && consoleWindows.empty() &&
                    contains(log, "console: none, and the parent has none (");
    if (!alive) { std::printf("helper [%s]: the application ended on its own\n", mode.c_str()); }
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// THE DRIVER: runs this very program as the helper, with no console, and reads
// what it prints.
// ---------------------------------------------------------------------------
int runHelper(const std::string& self, const char* mode, const std::string& exe,
              const fs::path& dir, std::string& out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr;
    HANDLE wr = nullptr;
    if (::CreatePipe(&rd, &wr, &sa, 0) == 0) { return -1; }
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::wstring cmd = L"\"" + widen(self) + L"\" --launch " + widen(mode) + L" \"" + widen(exe) +
                       L"\" \"" + widen(dir.string()) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    // DETACHED_PROCESS: no console at all, which is what Explorer is. The terminal
    // scenario wants the opposite - a console, windowless - to stand in for a shell.
    const DWORD flags = std::strcmp(mode, "terminal") == 0 ? CREATE_NO_WINDOW : DETACHED_PROCESS;
    const BOOL ok = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    if (ok == 0) {
        ::CloseHandle(rd);
        return -2;
    }
    ::CloseHandle(pi.hThread);
    char buf[512];
    DWORD n = 0;
    while (::ReadFile(rd, buf, sizeof(buf), &n, nullptr) != 0 && n != 0) { out.append(buf, n); }
    ::CloseHandle(rd);
    ::WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0xDEAD;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::string selfPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (n == 0) { return std::string(); }
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(n), s.data(), len, nullptr, nullptr);
    return s;
}

fs::path scratch(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-consoleowner-") + tag + "-" +
                                 std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace

#endif  // _WIN32

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc >= 5 && std::strcmp(argv[1], "--launch") == 0) {
        return helperMain(argv[2], argv[3], argv[4]);
    }
    const std::string exe =
        argc >= 2 ? std::string(argv[1]) : std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    CHECK(fs::exists(exe));

    // 1. THE IMAGE: a windows-subsystem program, so the loader makes no console.
    const unsigned subsystem = peSubsystem(exe);
    std::printf("%s: PE subsystem %u (%s)\n", exe.c_str(), subsystem,
                subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? "windows" : "not the windows subsystem");
    CHECK(subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI);

    const std::string self = selfPath();
    CHECK(!self.empty());

    // 2 and 3. LAUNCHED THE WAY THE START MENU LAUNCHES IT: no console, ever.
    {
        const fs::path dir = scratch("explorer");
        std::string out;
        const int code = runHelper(self, "explorer", exe, dir, out);
        std::printf("%s", out.c_str());
        CHECK(code == 0);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // A PARENT THAT ASKS FOR A NEW CONSOLE (hidden): still none. A windows-subsystem
    // image does not get one, so there is nothing to release and no FreeConsole in
    // the application.
    {
        const fs::path dir = scratch("newconsole");
        std::string out;
        const int code = runHelper(self, "newconsole", exe, dir, out);
        std::printf("%s", out.c_str());
        CHECK(code == 0);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // STARTED FROM A TERMINAL: the tool modes still print, to the console the shell
    // has, and keep their exit codes.
    {
        const fs::path dir = scratch("terminal");
        std::string out;
        const int code = runHelper(self, "terminal", exe, dir, out);
        std::printf("%s", out.c_str());
        CHECK(code == 0);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#else
    (void)argc;
    (void)argv;
    SKIP_LINUX("Windows consoles: AttachConsole, FreeConsole and the PE subsystem field");
#endif
    return testSummary("test_console_owner_app");
}
