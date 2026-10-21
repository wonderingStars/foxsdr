// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/console_owner.hpp"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cascade::core {

namespace {

ConsoleOutcome g_outcome;

#if defined(_WIN32)

// The parent's console, when it has one. A windows-subsystem process starts with
// no console and, unless its starter redirected them, with no standard handles
// either (they come back null), so after a successful attach each CRT stream that
// has nothing behind it is bound to the console, so that printf and
// fprintf(stderr) reach the terminal that ran the tool. A stream bound to a
// redirection (a pipe, a file) is never replaced. Returns whether there was a
// parent console.
bool attachParentConsole() {
    if (::AttachConsole(ATTACH_PARENT_PROCESS) == 0) { return false; }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const auto bind = [&sa](DWORD which, const wchar_t* device, const char* cDevice,
                            const char* mode, FILE* stream) {
        // THE CRT'S VIEW, not the Win32 table's: attaching fills the null standard
        // handles with the console's (measured: all three, from PowerShell and from
        // cmd), but the CRT bound its streams at start-up, to nothing (_fileno
        // is -2), and does not look again. A stream with a real descriptor was
        // bound to a handle the caller passed, a redirection, and stays as it is.
        if (::_fileno(stream) >= 0) { return; }
        const HANDLE current = ::GetStdHandle(which);
        if (current == nullptr || current == INVALID_HANDLE_VALUE) {
            const HANDLE h = ::CreateFileW(device, GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                                           FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) { return; }
            ::SetStdHandle(which, h);
        }
        FILE* ignored = nullptr;
        (void)::freopen_s(&ignored, cDevice, mode, stream);
    };
    bind(STD_INPUT_HANDLE, L"CONIN$", "CONIN$", "r", stdin);
    bind(STD_OUTPUT_HANDLE, L"CONOUT$", "CONOUT$", "w", stdout);
    bind(STD_ERROR_HANDLE, L"CONOUT$", "CONOUT$", "w", stderr);
    // stderr is unbuffered by contract; the reopened stream is told so again.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

#endif  // _WIN32

}  // namespace

ConsoleDecision decideConsole(unsigned processCount) {
    if (processCount == 0) {
        return {ConsoleAction::Attach, "no console of its own: borrowing the parent's, if any"};
    }
    return {ConsoleAction::Keep, "a console is already attached"};
}

ConsoleOutcome applyConsoleOwnership() {
#if defined(_WIN32)
    DWORD pids[2] = {};
    // A buffer too small for the list is answered with the count that would fit
    // and nothing written, which is all this needs: "none, or some".
    const DWORD n = ::GetConsoleProcessList(pids, 2);
    g_outcome = ConsoleOutcome{};
    g_outcome.decision = decideConsole(static_cast<unsigned>(n));
    if (g_outcome.decision.action == ConsoleAction::Attach) {
        g_outcome.attached = attachParentConsole();
    }
#else
    // No console to own: the decision is a fact about Windows, so nothing is asked
    // of the process and nothing is done to it.
    g_outcome = ConsoleOutcome{};
    g_outcome.decision = {ConsoleAction::Keep, "no console to own on this platform"};
#endif
    return g_outcome;
}

ConsoleOutcome consoleOutcome() { return g_outcome; }

}  // namespace cascade::core
