/*
 * foxsdr-radar.exe - the Fox & Schirmer Radar Unit, as a Windows desktop
 * application.
 *
 * WHAT THIS IS. A real Win32 window with its own taskbar entry, hosting the
 * radar panel in an embedded WebView2. It is not a browser and it does not
 * open one: there is no address bar, no tab strip, no way to navigate
 * anywhere else, and the window is the application. The panel inside it is
 * the HTML/CSS/JavaScript in resources/web/radar, which FoxSDR's own web
 * server publishes at /radar - served from the same origin as /api/status
 * and /api/tile so the map tiles and the live aircraft need no cross-origin
 * exception to reach it.
 *
 * WHY IT HIDES FOXSDR. This is a separate interface to the same receiver,
 * not a second view alongside it: opening it puts FoxSDR's own window away,
 * and closing this one - by the window's close box or by the panel's POWER
 * switch - brings it back. That is the whole contract, and the ONLY route
 * back, which is why the failure case below matters so much.
 *
 * WHAT HAPPENS IF THIS PROCESS DIES. FoxSDR would otherwise be left hidden
 * with no way to reach it, so the arrangement is a LEASE, not a switch: this
 * process renews it every few seconds while it lives, and FoxSDR shows
 * itself again on its own if the renewals stop. A crash here costs the user
 * a few seconds, not their application.
 */

#include <windows.h>

// WIN32_LEAN_AND_MEAN leaves both of these out of windows.h, and both are
// needed: shellapi for CommandLineToArgvW, shellscalingapi for the per-monitor
// DPI context.
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wrl.h>

#include <atomic>
#include <string>
#include <thread>

#include "WebView2.h"

#include <httplib.h>

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"FoxSdrRadarUnit";
constexpr wchar_t kWindowTitle[] = L"Fox & Schirmer Radar Unit";

// The cabinet is 1080 logical pixels wide plus its room padding; this opens
// large enough to show it whole on a 1920x1080 screen without scrolling.
constexpr int kDefaultWidth = 1240;
constexpr int kDefaultHeight = 1010;

// How often the lease is renewed, and the window FoxSDR waits before it
// decides this process is gone. Three missed renewals, so one slow frame or
// a paused debugger does not bring the other window back underneath us.
constexpr int kLeaseRenewSeconds = 4;

struct Options {
    std::string host = "127.0.0.1";
    int port = 8073;
};

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
std::atomic<bool> g_running{true};
Options g_options;

// A host name and a URL are ASCII by the time they reach here (the only
// sources are a literal default and a --host argument), but converting with
// std::string(w.begin(), w.end()) narrows every wchar_t silently and turns
// anything non-ASCII into a wrong byte with no warning at runtime. These two
// say what they accept and drop what they cannot represent.
std::string narrowAscii(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (const wchar_t c : w) {
        if (c > 0 && c < 128) { out.push_back(static_cast<char>(c)); }
    }
    return out;
}

std::wstring widenAscii(const std::string& a) {
    std::wstring out;
    out.reserve(a.size());
    for (const char c : a) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string apiBase() {
    return "http://" + g_options.host + ":" + std::to_string(g_options.port);
}

// Tell FoxSDR whether the radar holds the display. Failure is deliberately
// silent and non-fatal: the panel is still usable against a receiver that is
// not answering, and it says so itself rather than this shell popping a box.
void postRadarActive(bool active) {
    try {
        httplib::Client cli(g_options.host, g_options.port);
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(2, 0);
        const std::string body = active ? "{\"radarActive\":true}" : "{\"radarActive\":false}";
        cli.Post("/api/control", body, "application/json");
    } catch (...) {
        // Nothing to do and nothing worth saying: see above.
    }
}

void leaseThread() {
    while (g_running.load()) {
        postRadarActive(true);
        for (int i = 0; i < kLeaseRenewSeconds * 10 && g_running.load(); ++i) {
            ::Sleep(100);
        }
    }
}

std::wstring userDataFolder() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { return L""; }
    std::wstring dir(buf);
    dir += L"\\foxsdr\\radar-webview";
    // Created by WebView2 itself if the parent exists; make the parent.
    std::wstring parent(buf);
    parent += L"\\foxsdr";
    ::CreateDirectoryW(parent.c_str(), nullptr);
    return dir;
}

std::wstring startUrl() {
    return widenAscii(apiBase() + "/radar");
}

// Shown in the window itself when the receiver is not answering, so the
// application always has something honest on screen rather than WebView2's
// own "this site can't be reached", which would read as a broken program and
// name a browser this is not.
constexpr wchar_t kOfflinePage[] =
    L"<!doctype html><meta charset=utf-8><style>"
    L"html,body{margin:0;height:100%;background:#0b0c0a;color:#cfd3bc;"
    L"font-family:'Barlow Condensed','Segoe UI',sans-serif;display:flex;"
    L"align-items:center;justify-content:center}"
    L"div{max-width:36rem;text-align:center;line-height:1.6}"
    L"h1{font-size:1.4rem;letter-spacing:.3em;color:#9ad84f;margin:0 0 1rem}"
    L"code{font-family:Consolas,monospace;color:#b7f56a}"
    L"</style><div><h1>NO LINK TO FOXSDR</h1>"
    L"<p>The radar unit draws its aircraft, its map tiles and its signal reading "
    L"from a running copy of FoxSDR. Nothing here is stored locally, so there is "
    L"nothing to show until that link is up.</p>"
    L"<p>Start FoxSDR, turn on <code>Web access</code>, and open the radar "
    L"again.</p></div>";

void showOffline() {
    if (g_webview) { g_webview->NavigateToString(kOfflinePage); }
}

bool receiverAnswering() {
    try {
        httplib::Client cli(g_options.host, g_options.port);
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(2, 0);
        auto r = cli.Get("/radar");
        return r && r->status == 200;
    } catch (...) {
        return false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            if (g_controller) {
                RECT rc;
                ::GetClientRect(hwnd, &rc);
                g_controller->put_Bounds(rc);
            }
            return 0;
        case WM_CLOSE:
            // THE WAY BACK. Releasing the lease before the window goes means
            // FoxSDR is already returning as this one disappears, rather than
            // the user watching an empty desktop for the lease to expire.
            g_running.store(false);
            postRadarActive(false);
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void parseArgs(Options& opt) {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) { return; }
    for (int i = 1; i < argc; ++i) {
        const std::wstring a(argv[i]);
        if (a == L"--port" && i + 1 < argc) {
            try {
                const int p = std::stoi(argv[++i]);
                if (p > 0 && p < 65536) { opt.port = p; }
            } catch (...) { /* keep the default */ }
        } else if (a == L"--host" && i + 1 < argc) {
            opt.host = narrowAscii(std::wstring(argv[++i]));
        }
    }
    ::LocalFree(argv);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    parseArgs(g_options);

    // Per-monitor DPI: the panel is specified in logical pixels and WebView2
    // scales it, so anything less makes a 4K screen render it blurred.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    // The cabinet's own ground, so a resize never flashes white.
    wc.hbrBackground = ::CreateSolidBrush(RGB(0x0b, 0x0c, 0x0a));
    wc.lpszClassName = kWindowClass;
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, kDefaultWidth, kDefaultHeight,
                                  nullptr, nullptr, hInstance, nullptr);
    if (hwnd == nullptr) { return 1; }
    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    const bool linkUp = receiverAnswering();

    const std::wstring dataDir = userDataFolder();
    HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.empty() ? nullptr : dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, linkUp](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || env == nullptr) { return r; }
                return env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd, linkUp](HRESULT r2, ICoreWebView2Controller* ctl) -> HRESULT {
                            if (FAILED(r2) || ctl == nullptr) { return r2; }
                            g_controller = ctl;
                            g_controller->get_CoreWebView2(&g_webview);

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings))) {
                                // A panel, not a browser: no context menu, no
                                // developer tools, no status bar, and nothing
                                // that offers to open anything elsewhere.
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDefaultScriptDialogsEnabled(FALSE);
                            }

                            RECT rc;
                            ::GetClientRect(hwnd, &rc);
                            g_controller->put_Bounds(rc);

                            if (linkUp) {
                                g_webview->Navigate(startUrl().c_str());
                            } else {
                                showOffline();
                            }
                            return S_OK;
                        })
                        .Get());
            })
            .Get());

    if (FAILED(hr)) {
        // The runtime is part of Windows 11 and is installed by Edge on 10,
        // so this is rare - but it must say what is wrong rather than vanish.
        ::MessageBoxW(hwnd,
                      L"The Microsoft Edge WebView2 runtime is needed to draw the radar "
                      L"panel and could not be started.\n\nInstall the WebView2 Evergreen "
                      L"runtime from Microsoft and open the radar again.",
                      kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    std::thread lease;
    if (linkUp) { lease = std::thread(leaseThread); }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    g_running.store(false);
    if (lease.joinable()) { lease.join(); }
    // Belt and braces: the lease would expire on its own, but saying so
    // outright means the user's window comes back at once.
    postRadarActive(false);
    return static_cast<int>(msg.wParam);
}
