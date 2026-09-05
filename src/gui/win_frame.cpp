// win_frame.cpp - see the header for why the title bar goes and what has to
// be put back when it does.
#include "gui/win_frame.hpp"

#include <cstdlib>

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

#include <dwmapi.h>
#include <windowsx.h>
#endif

namespace cascade::gui::frame {

namespace {

CaptionLayout g_layout;

#ifdef _WIN32
HWND g_hwnd = nullptr;
WNDPROC g_previous = nullptr;

// The resize border, in pixels, at this window's DPI: the frame the system
// would have drawn, so a borderless window resizes from the same eight or so
// pixels a framed one does.
int frameThicknessPx(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
           GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        // THE CLIENT AREA IS THE WHOLE WINDOW. Answering zero to the
        // wParam-TRUE form leaves rgrc[0] - the proposed window rectangle -
        // as the client rectangle, so there is no caption and no frame for
        // the system to paint. A MAXIMISED window is the one exception: the
        // system sizes it larger than the monitor by the frame thickness on
        // every side, so that the invisible frame sits off-screen; with the
        // frame gone that would put the cabinet's own edges off-screen
        // instead, so the client is pulled in by the same amount.
        if (wParam == TRUE) {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            if (IsZoomed(hwnd)) {
                const int f = frameThicknessPx(hwnd);
                params->rgrc[0].left += f;
                params->rgrc[0].top += f;
                params->rgrc[0].right -= f;
                params->rgrc[0].bottom -= f;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        // The procs underneath run first: ImGui's answers HTTRANSPARENT for a
        // viewport that wants no input, and that answer wins.
        const LRESULT below = CallWindowProcW(g_previous, hwnd, msg, wParam, lParam);
        if (below == HTTRANSPARENT || below == HTNOWHERE) { return below; }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const bool zoomed = IsZoomed(hwnd) != FALSE;
        const Zone zone =
            hitZone(static_cast<float>(pt.x), static_cast<float>(pt.y),
                    static_cast<float>(rc.right), static_cast<float>(rc.bottom),
                    zoomed ? 0.0f : static_cast<float>(frameThicknessPx(hwnd)), g_layout,
                    zoomed);
        switch (zone) {
        case Zone::Caption: return HTCAPTION;
        case Zone::Left: return HTLEFT;
        case Zone::Right: return HTRIGHT;
        case Zone::Top: return HTTOP;
        case Zone::Bottom: return HTBOTTOM;
        case Zone::TopLeft: return HTTOPLEFT;
        case Zone::TopRight: return HTTOPRIGHT;
        case Zone::BottomLeft: return HTBOTTOMLEFT;
        case Zone::BottomRight: return HTBOTTOMRIGHT;
        case Zone::Client: break;
        }
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
        // A DOUBLE-CLICK ON THE RAIL FILLS THE SCREEN, or restores it, as a
        // title bar's does. The system would send WM_NCLBUTTONDBLCLK for that
        // - but only to a window class registered with CS_DBLCLKS, which
        // GLFW's is not, so the second click is recognised here: within the
        // desktop's double-click time and distance of the last press on the
        // caption.
        if (wParam == HTCAPTION) {
            static DWORD lastTime = 0;
            static POINT lastAt{0, 0};
            const DWORD now = GetMessageTime();
            const POINT at{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            // (`near` is a Windows macro; hence `nearby`.)
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
    g_hwnd = hwnd;
    g_previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&frameProc)));
    // One pixel of the system's frame extended into the client keeps the
    // window's shadow and its rounded corners on a desktop that composites
    // them; without it a captionless window is drawn as a flat rectangle
    // with no edge against the desktop.
    const MARGINS margins{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    // Recompute the client area NOW, through the WM_NCCALCSIZE above, rather
    // than on the next move or resize - otherwise the first frame is drawn
    // under a caption that is about to vanish.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
