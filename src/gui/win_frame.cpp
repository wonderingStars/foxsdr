// win_frame.cpp - see the header for why the title bar goes and what has to
// be put back when it does.
#include "gui/win_frame.hpp"

#include <cstdlib>

#include "core/diag_log.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <windowsx.h>
#endif

namespace cascade::gui::frame {

namespace {

CaptionLayout g_layout;

#ifdef _WIN32
HWND g_hwnd = nullptr;
WNDPROC g_previous = nullptr;

LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST: {
        // THE FRAME ANSWERS FIRST. The window keeps a real, if invisible,
        // sizing frame, so the operating system's own hit test already knows
        // an edge from a corner, and ImGui's backend underneath it answers
        // HTTRANSPARENT for a viewport that wants no input. Only a point the
        // system calls CLIENT is ours to reconsider: on the rail it is the
        // caption, unless it is on one of the rail's keys.
        const LRESULT below = CallWindowProcW(g_previous, hwnd, msg, wParam, lParam);
        if (below != HTCLIENT) { return below; }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const Zone zone =
            hitZone(static_cast<float>(pt.x), static_cast<float>(pt.y),
                    static_cast<float>(rc.right), static_cast<float>(rc.bottom), 0.0f, g_layout,
                    IsZoomed(hwnd) != FALSE);
        return zone == Zone::Caption ? HTCAPTION : HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
        // A DOUBLE-CLICK ON THE RAIL FILLS THE SCREEN, or restores it, as a
        // title bar's does. The system would send WM_NCLBUTTONDBLCLK for that
        // - but only to a window class registered with CS_DBLCLKS, which
        // GLFW's is not, so the second click is recognised here: within the
        // desktop's double-click time and distance of the last press on the
        // caption. (`near` is a Windows macro; hence `nearby`.)
        if (wParam == HTCAPTION) {
            static DWORD lastTime = 0;
            static POINT lastAt{0, 0};
            const DWORD now = GetMessageTime();
            const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const bool quick = lastTime != 0 && (now - lastTime) <= GetDoubleClickTime();
            const bool nearby = std::abs(at.x - lastAt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                                std::abs(at.y - lastAt.y) <= GetSystemMetrics(SM_CYDOUBLECLK);
            if (quick && nearby) {
                lastTime = 0;
                PostMessageW(hwnd, WM_SYSCOMMAND,
                             static_cast<WPARAM>(IsZoomed(hwnd) ? SC_RESTORE : SC_MAXIMIZE), 0);
                return 0;
            }
            lastTime = now;
            lastAt = at;
        }
        break;
    case WM_NCRBUTTONUP:
        // THE SYSTEM MENU, on a right-click of the rail, exactly where a
        // framed window offers it. The items are enabled by state so the menu
        // does not offer to maximise a window that already is.
        if (wParam == HTCAPTION) {
            HMENU menu = GetSystemMenu(hwnd, FALSE);
            if (menu != nullptr) {
                const bool zoomed = IsZoomed(hwnd) != FALSE;
                EnableMenuItem(menu, SC_RESTORE, zoomed ? MF_ENABLED : MF_GRAYED);
                EnableMenuItem(menu, SC_MAXIMIZE, zoomed ? MF_GRAYED : MF_ENABLED);
                EnableMenuItem(menu, SC_SIZE, zoomed ? MF_GRAYED : MF_ENABLED);
                EnableMenuItem(menu, SC_MOVE, zoomed ? MF_GRAYED : MF_ENABLED);
                EnableMenuItem(menu, SC_MINIMIZE, MF_ENABLED);
                const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                               GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0,
                                               hwnd, nullptr);
                if (cmd != 0) { PostMessageW(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0); }
            }
            return 0;
        }
        break;
    default:
        break;
    }
    return CallWindowProcW(g_previous, hwnd, msg, wParam, lParam);
}
#endif  // _WIN32

}  // namespace

Zone hitZone(float x, float y, float width, float height, float border,
             const CaptionLayout& layout, bool maximised) {
    if (!(width > 0.0f) || !(height > 0.0f)) { return Zone::Client; }
    if (x < 0.0f || y < 0.0f || x >= width || y >= height) { return Zone::Client; }
    if (!maximised && border > 0.0f) {
        const bool left = x < border;
        const bool right = x >= width - border;
        const bool top = y < border;
        const bool bottom = y >= height - border;
        if (top && left) { return Zone::TopLeft; }
        if (top && right) { return Zone::TopRight; }
        if (bottom && left) { return Zone::BottomLeft; }
        if (bottom && right) { return Zone::BottomRight; }
        if (left) { return Zone::Left; }
        if (right) { return Zone::Right; }
        if (top) { return Zone::Top; }
        if (bottom) { return Zone::Bottom; }
    }
    if (y < layout.railHeight) {
        if (!layout.keys.empty() && layout.keys.contains(x, y)) { return Zone::Client; }
        return Zone::Caption;
    }
    return Zone::Client;
}

bool install(GLFWwindow* window) {
#ifdef _WIN32
    if (window == nullptr) { return false; }
    if (g_hwnd != nullptr) { return true; }
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd == nullptr) { return false; }
    // THE CAPTION COMES OFF THE WINDOW'S STYLE, and nothing else changes.
    //
    // 0.78.0 kept WS_CAPTION and answered WM_NCCALCSIZE with "the client is
    // the whole window", which is the textbook way to hide a frame - and on
    // one user's machine the picture came out shifted up by exactly a
    // caption's height, every control sitting above the place it was
    // hit-tested, with a dark band where the caption would have been. Some
    // part of that machine's display path still sized the picture from the
    // window's STYLE, caption included, rather than from the client area the
    // window had declared. That cannot be argued with from here, so it is
    // not argued with: the style itself no longer says caption. What is left
    // is a plain resizable window - sizing frame, maximise and minimise
    // boxes, system menu - whose client area is the standard one for that
    // style on every driver, and which the system itself keeps consistent:
    // maximised, it parks the invisible frame off-screen as it does for any
    // window. The rail is made the caption by WM_NCHITTEST alone.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = (style & ~static_cast<LONG_PTR>(WS_CAPTION)) | WS_THICKFRAME |
                            WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    if (wanted != style) { SetWindowLongPtrW(hwnd, GWL_STYLE, wanted); }
    g_hwnd = hwnd;
    g_previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&frameProc)));
    // Recompute the frame NOW, through the system's own WM_NCCALCSIZE for
    // the new style, rather than on the next move or resize.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    // THE GEOMETRY, ON RECORD. A report of controls that do not sit where
    // they are drawn is a geometry question, and these are the numbers that
    // answer it: what the window is, what the system says its client is, and
    // the DPI the frame was sized for.
    {
        RECT wr{};
        RECT cr{};
        POINT origin{0, 0};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        ClientToScreen(hwnd, &origin);
        cascade::core::diagLogf(
            "frame: caption removed; window %ld,%ld %ldx%ld client %ldx%ld at %ld,%ld dpi %u",
            wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom,
            origin.x, origin.y, GetDpiForWindow(hwnd));
    }
    return true;
#else
    (void)window;
    return false;
#endif
}

bool installed() {
#ifdef _WIN32
    return g_hwnd != nullptr;
#else
    return false;
#endif
}

void setCaptionLayout(const CaptionLayout& layout) { g_layout = layout; }

CaptionLayout captionLayout() { return g_layout; }

}  // namespace cascade::gui::frame
