// A person holding the window is excused; a window WINDOWS calls hung is not.
//
// THE FIELD CASE. Signature 35ED0AF291113FC8 (0.99.78 and 0.99.84): the GUI thread went silent
// with the frame timer in `user-wait` (the shell-open bracket: a ShellExecute or a file dialog)
// or with the window procedure's modal-loop bit raised, Windows greyed the window "(Not
// Responding)", and its user closed it from Windows' "<program> is not responding" dialog (exit
// code 0xCFFFFFFF, measured: see sentinel.hpp, kWindowsHungCloseCode). No freeze report was
// written, because the watchdog excused the stall for as long as it lasted: the user-paced
// pause (HangWatchdog::pauseUser) without any cap, and the modal-loop flags (rule 2) likewise.
// Both excuses are for a PERSON - reading a prompt, choosing a file, dragging the window - and a
// person is always waited for in a loop that takes messages. Windows calls a thread hung when it
// has stopped taking them (IsHungAppWindow, about 5 s), so that verdict is where both excuses
// end: past it, the stall is measured like any other and reported with every thread's stack.
//
// WHAT IS ASSERTED, with a real HangWatchdog started on a thread that owns a real window:
//   1. a user-paced pause over a thread that has stopped taking messages IS reported, once,
//      after Windows has called its window hung;
//   2. the same pause over a thread that keeps taking messages and draws no frame (a file
//      dialog's own loop, a prompt being read) is NOT reported, for just as long;
//   3. a real window-move loop (WM_SYSCOMMAND SC_MOVE, GUI_INMOVESIZE checked) whose handler
//      never returns IS reported;
//   4. the same loop with a handler that keeps taking messages is NOT reported.
// 3 and 4 take the mouse pointer while the loop lasts, so they run only with
// FOXSDR_TEST_TAKES_POINTER=1 (see main) and are counted as skipped otherwise.
// 1 and 3 fail without the change (the break-it check: both read 0 reports).
//
// THE SECOND FIELD CASE (0.99.89, 2026-10-25): "freeze: the frame was in user-wait", stalled in
// AppWindow::launchInstaller -> ShellExecuteW -> windows.storage.dll -> a plain kernel wait. The
// update banner's install key waits for SmartScreen, an antivirus check or an elevation prompt,
// and ShellExecute does that wait WITHOUT taking messages - so Windows greyed a healthy window
// "(Not Responding)" (which is very likely how 35ED0AF2's users came to close theirs) and, since
// 0.99.87, the watchdog reported it. Since 0.99.91 the shell call runs on a thread of its own
// (gui::runShellOpenResponsive) while the GUI thread waits taking sent messages:
//   5. a shell call that blocks for longer than Windows' verdict, made through
//      runShellOpenResponsive under the same user-paced pause: the window is NEVER called hung
//      while it lasts, nothing is reported, and the call's answer comes back;
//   6. a SendMessage from another thread to that window during the call is answered (a shell
//      dialog owned by the window, or a broadcast, cannot deadlock against the wait).
// 5 and 6 fail when the call is made inline on the GUI thread, as before 0.99.91 (the break-it
// check: the window reads hung and one report is written; the SendMessage times out).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include "core/hang_watchdog.hpp"
#include "gui/shell_open.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;

#if defined(_WIN32)
namespace {

constexpr unsigned kThresholdMs = 800;
constexpr unsigned kCapMs = 1500;
// Windows' own verdict takes about 5 s (measured 5.0-5.3 s); then a threshold and a few polls
// for the report, and margin.
constexpr unsigned kStallMs = 5300 + kThresholdMs + 3 * HangWatchdog::kPollMs + 1500;

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    return base / (std::string("cascade-hungwin-") + tag + "-" + std::to_string(::GetCurrentProcessId()));
}

void pumpFor(unsigned ms, HangWatchdog* beat) {
    const ULONGLONG until = ::GetTickCount64() + ms;
    MSG msg;
    while (::GetTickCount64() < until) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (beat != nullptr) { beat->heartbeat(); }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

// What the window procedure does when the move loop starts, per test.
std::function<void()> g_onEnterMove;
std::atomic<bool> g_sawInMoveSize{false};

LRESULT CALLBACK testProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ENTERSIZEMOVE) {
        GUITHREADINFO gi{};
        gi.cbSize = sizeof(gi);
        if (::GetGUIThreadInfo(::GetCurrentThreadId(), &gi) != 0 && (gi.flags & GUI_INMOVESIZE) != 0) {
            g_sawInMoveSize = true;
        }
        if (g_onEnterMove) { g_onEnterMove(); }
    }
    return ::DefWindowProcW(h, m, w, l);
}

HWND makeWindow() {
    static bool registered = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = testProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CascadeHungWindowTest";
    if (!registered) {
        ::RegisterClassW(&wc);
        registered = true;
    }
    // Off every screen, shown without activation: a real top-level window the shell can grey,
    // that takes nobody's focus.
    HWND h = ::CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"cascade hung-window test",
                               WS_OVERLAPPEDWINDOW, -3000, -3000, 240, 120, nullptr, nullptr,
                               wc.hInstance, nullptr);
    ::ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

// One scenario on a thread of its own: it owns the window, starts the watchdog (so it is the
// watchdog's GUI thread), beats like a frame loop, then does `stall` with no heartbeat.
// `stall` returns the report count AS THE HELD STATE ENDS - read inside it, before the frame loop
// would have beaten again - and the scenario then beats, as the frame loop does at once.
unsigned runScenario(const std::string& tag, const std::function<unsigned(HangWatchdog&, HWND)>& stall) {
    std::error_code ec;
    const fs::path dir = scratchDir(tag);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    unsigned reports = 0;
    std::thread gui([&] {
        HWND h = makeWindow();
        HangWatchdog w;
        w.setExcuseCapMs(kCapMs);
        w.start(dir.string(), kThresholdMs);
        pumpFor(300, &w);  // a live frame loop
        reports = stall(w, h);
        pumpFor(300, &w);  // the frame loop turns again
        w.stop();
        ::DestroyWindow(h);
        pumpFor(50, nullptr);
    });
    gui.join();
    std::printf("%s: %u report(s)\n", tag.c_str(), reports);
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    return reports;
}

// Ends a keyboard move loop from outside: Escape, and a cancel, until the loop has returned.
void endMoveLoopWhen(HWND h, const std::atomic<bool>& returned, unsigned afterMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(afterMs));
    for (int i = 0; i < 200 && !returned; ++i) {
        ::PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
        ::PostMessageW(h, WM_KEYUP, VK_ESCAPE, 0xC0000001);
        ::PostMessageW(h, WM_CANCELMODE, 0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace
#endif

int main() {
#if defined(_WIN32)
    // --- 1. user-paced, and the thread has stopped taking messages: reported ---------------
    {
        const unsigned n = runScenario("user-stopped", [](HangWatchdog& w, HWND) {
            w.pauseUser();
            ::Sleep(kStallMs);  // a ShellExecute that never comes back, taking no messages
            const unsigned n = w.reportsWritten();
            w.resumeUser();
            return n;
        });
        CHECK(n == 1u);
    }

    // --- 2. user-paced, and the thread is in a loop that takes messages: excused ----------
    {
        const unsigned n = runScenario("user-pumping", [](HangWatchdog& w, HWND) {
            w.pauseUser();
            pumpFor(kStallMs, nullptr);  // a dialog's own loop: messages, no frames
            const unsigned n = w.reportsWritten();
            w.resumeUser();
            return n;
        });
        CHECK(n == 0u);
    }

    // --- 5 and 6. a shell call that blocks past Windows' verdict, made off the GUI thread -----
    // The call itself only sleeps: what ShellExecuteW does while SmartScreen, an antivirus check
    // or an elevation prompt is answered, as far as the calling thread is concerned. Meanwhile
    // another thread asks Windows about the window every 100 ms, and sends it a message halfway.
    {
        std::atomic<bool> everHung{false};
        std::atomic<bool> sentAnswered{false};
        std::atomic<bool> callOffGuiThread{false};
        bool answer = false;
        const unsigned n = runScenario("shell-responsive", [&](HangWatchdog& w, HWND h) {
            cascade::gui::ShellPauseHooks hooks;
            hooks.pause = [&w] { w.pauseUser(); };
            hooks.resume = [&w] { w.resumeUser(); };
            std::atomic<bool> calling{true};
            std::thread watcher([&] {
                const ULONGLONG half = ::GetTickCount64() + kStallMs / 2;
                bool sent = false;
                while (calling) {
                    if (::IsHungAppWindow(h) != FALSE) { everHung = true; }
                    if (!sent && ::GetTickCount64() >= half) {
                        sent = true;
                        DWORD_PTR result = 0;
                        sentAnswered =
                            ::SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_BLOCK, 2000, &result) != 0;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            const DWORD guiThread = ::GetCurrentThreadId();
            answer = cascade::gui::runShellOpenResponsive(hooks, [&]() -> bool {
                callOffGuiThread = ::GetCurrentThreadId() != guiThread;
                ::Sleep(kStallMs);
                return true;
            });
            const unsigned n = w.reportsWritten();
            calling = false;
            watcher.join();
            return n;
        });
        CHECK(answer);
        CHECK(callOffGuiThread.load());
        CHECK(!everHung.load());       // 5: never "(Not Responding)"
        CHECK(n == 0u);                // 5: and so nothing reported
        CHECK(sentAnswered.load());    // 6
    }

    // --- 3 and 4 TAKE THE MOUSE POINTER, so they run only when asked -------------------------
    // A keyboard move loop (SC_MOVE) captures the pointer and moves it to the window for as long
    // as the loop lasts, about 8 s here - on a developer's desktop that is their mouse taken away
    // (it happened, 2026-10-23), and a click or a move of the real mouse ends or steers the loop,
    // which made case 4 flaky on a desktop in use. FOXSDR_TEST_TAKES_POINTER=1 runs them, on a
    // machine nobody is using; otherwise they are counted as skipped, never as passed.
    const char* takesPointer = std::getenv("FOXSDR_TEST_TAKES_POINTER");
    if (takesPointer == nullptr || std::string(takesPointer) != "1") {
        ++g_checksSkipped;
        std::printf("SKIP: cases 3 and 4 (a real window-move loop) take the mouse pointer; "
                    "set FOXSDR_TEST_TAKES_POINTER=1 to run them\n");
        return testSummary("test_hung_window_excuse");
    }

    // --- 3. a real move loop whose handler never returns: reported -------------------------
    {
        g_sawInMoveSize = false;
        const unsigned n = runScenario("move-stopped", [](HangWatchdog& w, HWND h) {
            std::atomic<bool> returned{false};
            unsigned n = 0;
            g_onEnterMove = [&] {
                ::Sleep(kStallMs);
                n = w.reportsWritten();  // still inside the loop
            };
            std::thread ender([&] { endMoveLoopWhen(h, returned, kStallMs); });
            ::SendMessageW(h, WM_SYSCOMMAND, SC_MOVE, 0);
            returned = true;
            ender.join();
            g_onEnterMove = nullptr;
            return n;
        });
        CHECK(g_sawInMoveSize.load());  // the precondition: the thread really was in the loop
        CHECK(n == 1u);
    }

    // --- 4. the same loop, held by a "person" who keeps it taking messages: excused --------
    {
        g_sawInMoveSize = false;
        const unsigned n = runScenario("move-pumping", [](HangWatchdog& w, HWND h) {
            std::atomic<bool> returned{false};
            unsigned n = 0;
            g_onEnterMove = [&] {
                pumpFor(kStallMs, nullptr);
                n = w.reportsWritten();  // still inside the loop
            };
            std::thread ender([&] { endMoveLoopWhen(h, returned, kStallMs); });
            ::SendMessageW(h, WM_SYSCOMMAND, SC_MOVE, 0);
            returned = true;
            ender.join();
            g_onEnterMove = nullptr;
            return n;
        });
        CHECK(g_sawInMoveSize.load());
        CHECK(n == 0u);
    }
#else
    SKIP_LINUX("IsHungAppWindow and the Windows modal loops are Windows-only");
#endif
    return testSummary("test_hung_window_excuse");
}
