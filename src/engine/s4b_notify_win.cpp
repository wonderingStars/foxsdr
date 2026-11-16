// s4b_notify_win.cpp - see s4b_notify_win.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_notify_win.hpp"

#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace cascade::engine::s4b {

namespace {

constexpr std::size_t kTitleUnits = 63;
constexpr std::size_t kBodyUnits = 255;
constexpr std::size_t kQueueMax = 8;
constexpr int kPerMinute = 6;

// UTF-8 to UTF-16, replacing anything malformed with U+FFFD, control characters (but not a line break) with a space, stopping at `maxUnits` on a code point.
std::u16string toUnits(const std::string& in, std::size_t maxUnits) {
    std::u16string out;
    std::size_t i = 0;
    while (i < in.size() && out.size() < maxUnits) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) { cp = c; }
        else if (c >= 0xC2 && c <= 0xDF && i + 1 < in.size()) { len = 2; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF && i + 2 < in.size()) { len = 3; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4 && i + 3 < in.size()) { len = 4; cp = c & 0x07u; }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) { cp = 0xFFFD; len = ok ? len : 1; }
        i += len;
        if (cp < 0x20 && cp != '\n') { cp = ' '; }
        if (cp == 0x7F) { cp = ' '; }
        if (cp >= 0x10000) {
            if (out.size() + 2 > maxUnits) { break; }
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

BalloonText balloonText(const std::string& title, const std::string& body) {
    BalloonText t;
    t.title = toUnits(title, kTitleUnits);
    t.body = toUnits(body, kBodyUnits);
    if (t.title.empty()) { t.title = u"FoxSDR"; }
    return t;
}

DesktopNotifier::~DesktopNotifier() {
#if defined(_WIN32)
    if (thread_.joinable()) {
        stop_.store(true);
        if (hwnd_ != nullptr) { ::PostMessageW(static_cast<HWND>(hwnd_), WM_APP + 1, 0, 0); }
        thread_.join();
    }
#endif
}

bool DesktopNotifier::notify(const cascade::core::alerts::Notification& n, std::string* err) {
#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lk(m_);
        const std::int64_t now = nowMs();
        if (now - windowStartMs_ >= 60000) { windowStartMs_ = now; inWindow_ = 0; }
        if (inWindow_ >= kPerMinute || queue_.size() >= kQueueMax) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            if (err != nullptr) { *err = "too many desktop notifications: at most six a minute"; }
            return false;
        }
        ++inWindow_;
        queue_.push_back(balloonText(n.title, n.body));
        if (!thread_.joinable()) {
            stop_.store(false);
            thread_ = std::thread([this] { run(); });
        }
    }
    for (int i = 0; i < 200 && !started_.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    if (!started_.load() || hwnd_ == nullptr) {
        if (err != nullptr) { *err = "the notification window could not be made"; }
        return false;
    }
    ::PostMessageW(static_cast<HWND>(hwnd_), WM_APP, 0, 0);
    return true;
#else
    (void)n;
    if (err != nullptr) { *err = "desktop notifications are not available on this system"; }
    return false;
#endif
}

void DesktopNotifier::run() {
#if defined(_WIN32)
    static const wchar_t kClass[] = L"FoxSDRNotifyWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        DesktopNotifier* self = reinterpret_cast<DesktopNotifier*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self != nullptr) {
            if (m == WM_APP) {
                BalloonText t;
                {
                    std::lock_guard<std::mutex> lk(self->m_);
                    if (self->queue_.empty()) { return 0; }
                    t = std::move(self->queue_.front());
                    self->queue_.pop_front();
                }
                NOTIFYICONDATAW d{};
                d.cbSize = sizeof(d);
                d.hWnd = h;
                d.uID = 1;
                d.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
                d.hIcon = ::LoadIconW(nullptr, MAKEINTRESOURCEW(32516));
                wcsncpy_s(d.szTip, L"FoxSDR", _TRUNCATE);
                for (std::size_t i = 0; i < t.title.size() && i < 63; ++i) { d.szInfoTitle[i] = static_cast<wchar_t>(t.title[i]); }
                for (std::size_t i = 0; i < t.body.size() && i < 255; ++i) { d.szInfo[i] = static_cast<wchar_t>(t.body[i]); }
                d.dwInfoFlags = NIIF_INFO;
                if (!::Shell_NotifyIconW(NIM_MODIFY, &d)) { ::Shell_NotifyIconW(NIM_ADD, &d); }
                ::SetTimer(h, 1, 10000, nullptr);
                self->shown_.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            if (m == WM_TIMER) {
                NOTIFYICONDATAW d{};
                d.cbSize = sizeof(d);
                d.hWnd = h;
                d.uID = 1;
                ::Shell_NotifyIconW(NIM_DELETE, &d);
                ::KillTimer(h, 1);
                return 0;
            }
            if (m == WM_APP + 1) {
                NOTIFYICONDATAW d{};
                d.cbSize = sizeof(d);
                d.hWnd = h;
                d.uID = 1;
                ::Shell_NotifyIconW(NIM_DELETE, &d);
                ::PostQuitMessage(0);
                return 0;
            }
        }
        return ::DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    ::RegisterClassW(&wc);   // (already registered by an earlier notifier: the call fails and the class is used)
    HWND h = ::CreateWindowExW(0, kClass, L"FoxSDR notifications", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (h == nullptr) { return; }
    ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    hwnd_ = h;
    started_.store(true);
    MSG msg;
    while (!stop_.load() && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    ::DestroyWindow(h);
    hwnd_ = nullptr;
#endif
}

}  // namespace cascade::engine::s4b
