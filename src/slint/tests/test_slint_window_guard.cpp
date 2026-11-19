// The window guard (src/slint/window_guard.hpp; docs/multi-radio-records/outside-resize.md): a size asked of the window from OUTSIDE (SetWindowPos from another thread
// or program) never exceeds what the toolkit's drawing surface can take. A real top-level window is made on the desktop this test runs on (the hidden desktop) and is
// NEVER shown; the same call a foreign program makes is made on it from the same thread and from another thread, and the rectangle the window really has is read back.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

#include "test_check.hpp"
#include "window_guard.hpp"

using namespace foxsdr::slintguard;

#ifdef _WIN32
namespace {
// what the toolkit would be told: the largest size this window's own procedure (below the guard) was ever handed, by WM_WINDOWPOSCHANGED and by WM_SIZE
long g_seenMaxW = 0, g_seenMaxH = 0;
LRESULT CALLBACK plainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_WINDOWPOSCHANGED) {
        const WINDOWPOS* p = reinterpret_cast<const WINDOWPOS*>(l);
        if (p->cx > g_seenMaxW) { g_seenMaxW = p->cx; }
        if (p->cy > g_seenMaxH) { g_seenMaxH = p->cy; }
    } else if (m == WM_SIZE) {
        if (static_cast<long>(LOWORD(l)) > g_seenMaxW) { g_seenMaxW = LOWORD(l); }
        if (static_cast<long>(HIWORD(l)) > g_seenMaxH) { g_seenMaxH = HIWORD(l); }
    }
    return ::DefWindowProcW(h, m, w, l);
}

HWND makeWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &plainProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FoxSdrWindowGuardTest";
    ::RegisterClassW(&wc);
    return ::CreateWindowExW(0, wc.lpszClassName, L"guard test", WS_POPUP, 100, 100, 1200, 800, nullptr, nullptr, wc.hInstance, nullptr);   // never shown; a popup like the toolkit's own frameless window: the window system does not limit the size of a window with no sizing frame
}

SIZE sizeOf(HWND h) {
    RECT r{};
    ::GetWindowRect(h, &r);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

// what a foreign program does: SetWindowPos on another program's window (here another thread's call, which the window's own thread must pump)
bool foreignSetPos(HWND h, int x, int y, int w, int hh, UINT flags) {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::thread t([&] { ok = ::SetWindowPos(h, nullptr, x, y, w, hh, flags) != FALSE; done = true; });
    MSG m;
    const DWORD t0 = ::GetTickCount();
    while (!done && ::GetTickCount() - t0 < 10000) {
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
        ::Sleep(1);
    }
    t.join();
    return ok;
}
}  // namespace
#endif

int main() {
    // ---------------------------------------------------------------- the pure rule
    bool c = true;
    CHECK(clampExtent(1080, &c) == 1080 && !c);
    CHECK(clampExtent(kMaxWindowExtent, &c) == kMaxWindowExtent && !c);   // the largest side that was seen to survive is allowed
    CHECK(clampExtent(kMaxWindowExtent + 1, &c) == kMaxWindowExtent && c);
    CHECK(clampExtent(65535, &c) == kMaxWindowExtent && c);
    CHECK(clampExtent(0, &c) == 0 && !c);                                  // empty and negative requests are the window system's business
    CHECK(clampExtent(-5, &c) == -5 && !c);

#ifdef _WIN32
    // ---------------------------------------------------------------- a real window
    HWND h = makeWindow();
    CHECK(h != nullptr);
    if (h == nullptr) { return testSummary("window guard"); }
    CHECK(installWindowGuard(h));
    CHECK(installWindowGuard(h));                      // again: still one guard, still true
    CHECK(installWindowGuards() == 1);                 // the sweep finds the window this thread owns and counts it

    // the window can still be made the size a person asks for, from inside and from outside
    CHECK(::SetWindowPos(h, nullptr, 0, 0, 1920, 1080, SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(sizeOf(h).cx == 1920 && sizeOf(h).cy == 1080);
    CHECK(foreignSetPos(h, 10, 10, 1024, 640, SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(sizeOf(h).cx == 1024 && sizeOf(h).cy == 640);
    CHECK(guardClampCount() == 0);                     // nothing that ordinary was touched

    // the fault: the window manager (or a script with a typo) asks for a height no drawing surface can have
    struct Case { int w, h; };
    const Case big[] = {{1920, 65535}, {1920, 16385}, {65535, 1080}, {30000, 30000}, {1920, 100000}};
    for (const Case& k : big) {
        CHECK(foreignSetPos(h, 0, 0, k.w, k.h, SWP_NOZORDER | SWP_NOACTIVATE));
        const SIZE s = sizeOf(h);
        CHECK(s.cx <= kMaxWindowExtent && s.cy <= kMaxWindowExtent);
        CHECK(g_seenMaxW <= kMaxWindowExtent && g_seenMaxH <= kMaxWindowExtent);   // RED without the guard: the toolkit's own window procedure is told 65535
    }
    CHECK(guardClampCount() >= 4);                     // the guard says it acted (`state window.guard.clamps`)
    CHECK(foreignSetPos(h, 0, 0, 1920, 65535, SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(g_seenMaxH <= kMaxWindowExtent && sizeOf(h).cy <= kMaxWindowExtent);   // the height is held at the limit or below

    // the same from this thread (a toolkit call, a tiling tool in this process)
    CHECK(::SetWindowPos(h, nullptr, 0, 0, 2000, 40000, SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(g_seenMaxH <= kMaxWindowExtent && sizeOf(h).cy <= kMaxWindowExtent);

    // a request that does not ask for a size (a move, a restack) is never touched, and the window comes back to a normal size
    const long before = guardClampCount();
    CHECK(foreignSetPos(h, 50, 60, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(guardClampCount() == before);
    CHECK(foreignSetPos(h, 0, 0, 1600, 900, SWP_NOZORDER | SWP_NOACTIVATE));
    CHECK(sizeOf(h).cx == 1600 && sizeOf(h).cy == 900);

    // the largest size the window system will give it (maximise) never reads above the limit either
    MINMAXINFO mm{};
    ::SendMessageW(h, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mm));
    CHECK(mm.ptMaxTrackSize.x <= kMaxWindowExtent && mm.ptMaxTrackSize.y <= kMaxWindowExtent);

    ::DestroyWindow(h);
#else
    // Not Windows: no window is subclassed; the stand-ins say so (window_guard_win.cpp)
    CHECK(installWindowGuards() == 0);
    CHECK(!installWindowGuard(nullptr));
    CHECK(guardClampCount() == 0);
#endif
    return testSummary("window guard");
}
