// The window guard's Win32 half (window_guard.hpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "window_guard.hpp"

#include <atomic>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#endif

namespace foxsdr::slintguard {

int clampExtent(int requested, bool* clamped) {
    if (requested > kMaxWindowExtent) {
        if (clamped != nullptr) { *clamped = true; }
        return kMaxWindowExtent;
    }
    if (clamped != nullptr) { *clamped = false; }
    return requested;
}

#ifdef _WIN32
namespace {
constexpr UINT_PTR kSubclassId = 0x464F5857;   // "FOXW"
std::atomic<long> g_clamps{0};

LRESULT CALLBACK guardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_WINDOWPOSCHANGING && lp != 0) {
        // the request, before the window system applies it and before the toolkit is told of the result
        WINDOWPOS* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if ((pos->flags & SWP_NOSIZE) == 0) {
            bool cw = false, ch = false;
            pos->cx = clampExtent(pos->cx, &cw);
            pos->cy = clampExtent(pos->cy, &ch);
            if (cw || ch) { g_clamps.fetch_add(1); }
        }
    } else if (msg == WM_GETMINMAXINFO && lp != 0) {
        // the largest the window may be made, as the window system itself reads it (a maximise, a snap, the pointer's own drag): lowered, never raised
        const LRESULT r = ::DefSubclassProc(hwnd, msg, wp, lp);
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMaxSize.x = clampExtent(mm->ptMaxSize.x);
        mm->ptMaxSize.y = clampExtent(mm->ptMaxSize.y);
        mm->ptMaxTrackSize.x = clampExtent(mm->ptMaxTrackSize.x);
        mm->ptMaxTrackSize.y = clampExtent(mm->ptMaxTrackSize.y);
        return r;
    } else if (msg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, &guardProc, kSubclassId);
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

struct EnumCtx { DWORD pid; DWORD tid; int carrying; };

BOOL CALLBACK enumProc(HWND h, LPARAM lp) {
    EnumCtx* c = reinterpret_cast<EnumCtx*>(lp);
    DWORD pid = 0;
    const DWORD tid = ::GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || tid != c->tid) { return TRUE; }
    wchar_t cls[64] = {};
    ::GetClassNameW(h, cls, 63);
    const std::wstring_view name(cls);
    if (name == L"ConsoleWindowClass" || name == L"IME") { return TRUE; }
    if (installWindowGuard(h)) { ++c->carrying; }
    return TRUE;
}
}  // namespace

bool installWindowGuard(void* hwnd) {
    HWND h = static_cast<HWND>(hwnd);
    if (h == nullptr || !::IsWindow(h)) { return false; }
    DWORD_PTR existing = 0;
    if (::GetWindowSubclass(h, &guardProc, kSubclassId, &existing)) { return true; }
    return ::SetWindowSubclass(h, &guardProc, kSubclassId, 0) != FALSE;
}

int installWindowGuards() {
    EnumCtx c{::GetCurrentProcessId(), ::GetCurrentThreadId(), 0};
    ::EnumWindows(&enumProc, reinterpret_cast<LPARAM>(&c));
    return c.carrying;
}

long guardClampCount() { return g_clamps.load(); }

#else
// Not Windows: nothing subclasses a window here (a window manager is not asked from outside the way SetWindowPos is), so the guard is a stand-in.
int installWindowGuards() { return 0; }
bool installWindowGuard(void*) { return false; }
long guardClampCount() { return 0; }
#endif

}  // namespace foxsdr::slintguard
