// shell_open.cpp - see shell_open.hpp.
//
// Shell calls can block without pumping messages. Keep the GUI taking sent messages
// while a joined COM worker performs the call; retain the watchdog pause and result.
// Posted input remains queued until the current frame completes. Sent messages can
// still invoke window procedures, as in any Windows message pump.
// tests/test_hung_window_excuse.cpp exercises a shell-like wait exceeding the
// Windows hung-window timeout and checks that genuine unpumped waits still report.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/shell_open.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <exception>
#include <system_error>
#include <thread>
#endif

namespace cascade::gui {

#if defined(_WIN32)
namespace {

// Waits for `done` while this thread keeps taking SENT messages and keeps telling Windows it
// takes messages at all. Posted messages are left where they are.
void waitTakingSentMessages(HANDLE done) {
    for (;;) {
        const DWORD r = ::MsgWaitForMultipleObjectsEx(1, &done, 100, QS_SENDMESSAGE, 0);
        if (r == WAIT_OBJECT_0) { return; }
        MSG peeked;
        ::PeekMessageW(&peeked, nullptr, 0, 0, PM_NOREMOVE);
        if (r == WAIT_FAILED && ::WaitForSingleObject(done, 100) == WAIT_OBJECT_0) { return; }
    }
}

}  // namespace
#endif

bool runShellOpenResponsive(const ShellPauseHooks& hooks, const std::function<bool()>& call) {
#if defined(_WIN32)
    return runShellOpen(hooks, [&call]() -> bool {
        if (!call) { return false; }
        struct Event {
            HANDLE handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ~Event() { if (handle) { ::CloseHandle(handle); } }
        } event;
        const HANDLE done = event.handle;
        // NO EVENT, OR NO THREAD TO BE HAD (handle or thread exhaustion): the call is made here,
        // as it was before 0.99.91. The button still gets its answer; the only cost is the old
        // symptom, a window Windows may call hung while the shell waits. Never an exception out
        // of an ImGui button body, which no caller expects.
        if (done == nullptr) { return call(); }
        bool answer = false;
        std::exception_ptr error;
        std::thread worker;
        try {
            worker = std::thread([&call, &answer, &error, done] {
                const HRESULT com =
                    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                try {
                    answer = call();
                } catch (...) {
                    error = std::current_exception();
                }
                if (SUCCEEDED(com)) { ::CoUninitialize(); }
                ::SetEvent(done);
            });
        } catch (const std::system_error&) {
            return call();
        }
        waitTakingSentMessages(done);
        worker.join();
        if (error) { std::rethrow_exception(error); }
        return answer;
    });
#else
    return runShellOpen(hooks, call);
#endif
}

}  // namespace cascade::gui
