// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/console_close.hpp"

#include <atomic>

#if defined(_WIN32)
#include <windows.h>

#include "core/diag_log.hpp"
#endif

namespace cascade::core {

namespace {

std::atomic<bool> g_requested{false};

#if defined(_WIN32)

std::atomic<bool> g_installed{false};

// Runs on a thread the system creates for the event (HandlerRoutine: "the system
// creates a new thread in the process to execute the function"), so it touches
// nothing of the window's: one atomic store, one log line, and for a closing
// console a wait. The frame loop sees the flag at its next turn.
BOOL WINAPI onConsoleEvent(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            g_requested.store(true, std::memory_order_release);
            diagLogf("console: Ctrl+C or Ctrl+Break - closing the window");
            // No timeout on these two: the process runs on until the loop ends it.
            return TRUE;
        case CTRL_CLOSE_EVENT:
            g_requested.store(true, std::memory_order_release);
            diagLogf("console: its window is closing - closing the window; the shutdown has %u ms",
                     kConsoleCloseWaitMs);
            // Returning hands the process to the system, which ends it ("Return
            // TRUE. In this case, no other handler functions are called and the
            // system terminates the process"), so wait here while the main thread
            // shuts down. A clean finish exits the process, and this thread with
            // it, before the wait is over.
            for (unsigned waited = 0; waited < kConsoleCloseWaitMs; waited += 50) { ::Sleep(50); }
            return TRUE;
        default:
            // CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT do not reach a program that
            // loads user32.dll, and anything else is not ours to answer.
            return FALSE;
    }
}

#endif  // _WIN32

}  // namespace

bool installConsoleCloseHandler() {
#if defined(_WIN32)
    if (g_installed.exchange(true)) { return true; }
    return ::SetConsoleCtrlHandler(&onConsoleEvent, TRUE) != 0;
#else
    return false;
#endif
}

bool consoleCloseRequested() { return g_requested.load(std::memory_order_acquire); }

}  // namespace cascade::core
