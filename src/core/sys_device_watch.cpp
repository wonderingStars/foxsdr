// sys_device_watch.cpp - see sys_device_watch.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_device_watch.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbt.h>
#endif

namespace cascade::core::sys {
namespace {

const char* const kGuidUsbDevice = "a5dcbf10-6530-11d2-901f-00c04fb951ed";   // GUID_DEVINTERFACE_USB_DEVICE
const char* const kGuidWinUsb = "dee824ef-729b-4a0e-9c14-b7117d33a817";      // GUID_DEVINTERFACE_WINUSB

std::string lower(std::string s) {
    for (auto& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return s;
}

bool hexN(const std::string& s, std::size_t at, std::size_t n, unsigned& out) {
    if (at + n > s.size()) { return false; }
    unsigned v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[at + i];
        unsigned d;
        if (c >= '0' && c <= '9') { d = static_cast<unsigned>(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = static_cast<unsigned>(c - 'a' + 10); }
        else if (c >= 'A' && c <= 'F') { d = static_cast<unsigned>(c - 'A' + 10); }
        else { return false; }
        v = v * 16 + d;
    }
    out = v;
    return true;
}

bool wellFormedGuid(const std::string& g) {   // lower case 8-4-4-4-12
    if (g.size() != 36) { return false; }
    for (std::size_t i = 0; i < g.size(); ++i) {
        const bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        const char c = g[i];
        if (dash ? (c != '-') : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return true;
}

std::int64_t steadyMs() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

DeviceInterfaceInfo parseDeviceInterfacePath(const std::string& pathIn) {
    DeviceInterfaceInfo r;
    if (pathIn.empty() || pathIn.size() > 512) { return r; }
    std::string p = lower(pathIn);
    if (p.rfind("\\\\?\\", 0) == 0 || p.rfind("\\\\.\\", 0) == 0) { p = p.substr(4); }
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (true) {
        const std::size_t h = p.find('#', pos);
        if (h == std::string::npos) { parts.push_back(p.substr(pos)); break; }
        parts.push_back(p.substr(pos, h - pos));
        pos = h + 1;
    }
    if (parts.size() < 4) { return r; }
    if (parts[0] != "usb") { return r; }
    const std::string& ids = parts[1];
    unsigned vid = 0, pid = 0;
    if (ids.rfind("vid_", 0) != 0 || !hexN(ids, 4, 4, vid)) { return r; }
    if (ids.compare(8, 5, "&pid_") != 0 || !hexN(ids, 13, 4, pid)) { return r; }
    if (ids.size() > 17 && ids[17] != '&') { return r; }
    const std::string& last = parts.back();
    if (last.size() != 38 || last.front() != '{' || last.back() != '}') { return r; }
    const std::string guid = last.substr(1, 36);
    if (!wellFormedGuid(guid)) { return r; }
    // parts[2] is the serial (or the instance id); it is not read into the result.
    r.valid = true;
    r.vid = vid;
    r.pid = pid;
    r.classGuid = guid;
    r.isUsbRadioClass = (guid == kGuidUsbDevice || guid == kGuidWinUsb);
    return r;
}

bool ArrivalCoalescer::onEvent(DeviceChange change, const DeviceInterfaceInfo& info, std::int64_t nowMs) {
    if (!info.valid || !info.isUsbRadioClass) { return false; }
    if (!pending()) { first_ = nowMs; }
    last_ = nowMs;
    if (change == DeviceChange::Arrival) { ++s_.arrivals; } else { ++s_.removals; }
    return true;
}

bool ArrivalCoalescer::due(std::int64_t nowMs, ArrivalSummary& out) {
    if (!pending()) { return false; }
    if (nowMs - last_ < cfg_.settleMs && nowMs - first_ < cfg_.maxWaitMs) { return false; }
    out = s_;
    s_ = ArrivalSummary{};
    return true;
}

DeviceArrivalWatcher::DeviceArrivalWatcher(ArrivalConfig cfg, std::function<void(const ArrivalSummary&)> onSettled, Clock clock)
    : coalescer_(cfg), onSettled_(std::move(onSettled)), clock_(clock ? std::move(clock) : Clock(steadyMs)) {}

DeviceArrivalWatcher::~DeviceArrivalWatcher() { stop(); }

void DeviceArrivalWatcher::handleBroadcast(DeviceChange change, const std::string& path) {
    ++events_;
    const DeviceInterfaceInfo info = parseDeviceInterfacePath(path);
    std::lock_guard<std::mutex> lk(mu_);
    if (coalescer_.onEvent(change, info, clock_())) { ++counted_; }
}

void DeviceArrivalWatcher::handleTimer() {
    ArrivalSummary s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!coalescer_.due(clock_(), s)) { return; }
    }
    if (onSettled_) { onSettled_(s); }
    ++actions_;
}

#ifdef _WIN32
namespace {

constexpr UINT kStopMessage = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;

std::wstring widen(const std::string& s) {
    std::wstring w;
    for (unsigned char c : s) { w.push_back(static_cast<wchar_t>(c)); }   // interface paths are ASCII
    return w;
}
std::string narrowAscii(const wchar_t* w, std::size_t maxChars) {
    std::string s;
    for (std::size_t i = 0; i < maxChars && w[i] != L'\0'; ++i) { s.push_back(w[i] < 128 ? static_cast<char>(w[i]) : '?'); }
    return s;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<DeviceArrivalWatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_DEVICECHANGE:
            if (self != nullptr && (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) && lp != 0) {
                const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lp);
                if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && hdr->dbch_size >= sizeof(DEV_BROADCAST_DEVICEINTERFACE_W)) {
                    const auto* di = reinterpret_cast<const DEV_BROADCAST_DEVICEINTERFACE_W*>(lp);
                    const std::size_t maxChars = (hdr->dbch_size - offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name)) / sizeof(wchar_t);
                    self->handleBroadcast(wp == DBT_DEVICEARRIVAL ? DeviceChange::Arrival : DeviceChange::Removal, narrowAscii(di->dbcc_name, maxChars < 600 ? maxChars : 600));
                }
            }
            return TRUE;
        case WM_TIMER:
            if (self != nullptr && wp == kTimerId) { self->handleTimer(); }
            return 0;
        case kStopMessage:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::KillTimer(hwnd, kTimerId);
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

std::atomic<unsigned> g_classCounter{0};

bool guidFromString(const char* text, GUID& g) {
    const std::string s = text;   // 8-4-4-4-12, lower case
    if (!wellFormedGuid(s)) { return false; }
    unsigned d1 = 0, d2 = 0, d3 = 0, b[8] = {0};
    if (!hexN(s, 0, 8, d1) || !hexN(s, 9, 4, d2) || !hexN(s, 14, 4, d3) || !hexN(s, 19, 2, b[0]) || !hexN(s, 21, 2, b[1])) { return false; }
    for (int i = 0; i < 6; ++i) { if (!hexN(s, 24 + 2 * static_cast<std::size_t>(i), 2, b[2 + i])) { return false; } }
    g.Data1 = d1; g.Data2 = static_cast<unsigned short>(d2); g.Data3 = static_cast<unsigned short>(d3);
    for (int i = 0; i < 8; ++i) { g.Data4[i] = static_cast<unsigned char>(b[i]); }
    return true;
}

}  // namespace
#endif

bool DeviceArrivalWatcher::start(bool registerWithOs) {
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lk(startMu_);
        if (running_.load()) { return true; }
        startDone_ = false;
        startOk_ = false;
        wantOs_ = registerWithOs;
    }
    stopRequested_.store(false);
    thread_ = std::thread([this] { threadMain(); });
    // threadMain signals through startDone_ (a polled flag under startMu_, short wait: the thread does nothing slow before it).
    for (int i = 0; i < 2000; ++i) {
        {
            std::lock_guard<std::mutex> lk(startMu_);
            if (startDone_) { break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::lock_guard<std::mutex> lk(startMu_);
    return startDone_ && startOk_ && (!registerWithOs || registered_.load());
#else
    (void)registerWithOs;
    return false;
#endif
}

void DeviceArrivalWatcher::threadMain() {
#ifdef _WIN32
    const unsigned n = ++g_classCounter;
    const std::wstring className = L"FoxSdrDeviceWatch" + std::to_wstring(n) + L"_" + std::to_wstring(::GetCurrentProcessId());
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = className.c_str();
    HWND hwnd = nullptr;
    if (::RegisterClassW(&wc) != 0) {
        hwnd = ::CreateWindowExW(0, className.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }
    std::vector<HDEVNOTIFY> notifies;
    if (hwnd != nullptr) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (wantOs_) {
            for (const char* g : {kGuidUsbDevice, kGuidWinUsb}) {
                DEV_BROADCAST_DEVICEINTERFACE_W filter{};
                filter.dbcc_size = sizeof(filter);
                filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
                if (!guidFromString(g, filter.dbcc_classguid)) { continue; }
                // Registers to be TOLD; opens no device.
                const HDEVNOTIFY h = ::RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
                if (h != nullptr) { notifies.push_back(h); }
            }
            registered_.store(notifies.size() == 2);
        }
        ::SetTimer(hwnd, kTimerId, 50, nullptr);
        hwnd_ = hwnd;
        running_.store(true);
    }
    {
        std::lock_guard<std::mutex> lk(startMu_);
        startOk_ = (hwnd != nullptr);
        startDone_ = true;
    }
    if (hwnd != nullptr) {
        MSG m;
        while (::GetMessageW(&m, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
    }
    for (HDEVNOTIFY h : notifies) { ::UnregisterDeviceNotification(h); }
    registered_.store(false);
    running_.store(false);
    hwnd_ = nullptr;
    if (hwnd != nullptr) { ::UnregisterClassW(className.c_str(), wc.hInstance); }
    else if (::GetClassInfoW(wc.hInstance, className.c_str(), &wc)) { ::UnregisterClassW(className.c_str(), wc.hInstance); }
#endif
}

void DeviceArrivalWatcher::stop() {
#ifdef _WIN32
    if (thread_.joinable()) {
        HWND h = static_cast<HWND>(hwnd_);
        if (h != nullptr) { ::PostMessageW(h, kStopMessage, 0, 0); }
        thread_.join();
    }
#endif
    running_.store(false);
}

bool DeviceArrivalWatcher::inject(DeviceChange change, const std::string& path) {
#ifdef _WIN32
    HWND h = static_cast<HWND>(hwnd_);
    if (!running_.load() || h == nullptr) { return false; }
    const std::wstring w = widen(path);
    std::vector<char> buf(offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name) + (w.size() + 1) * sizeof(wchar_t), 0);
    auto* di = reinterpret_cast<DEV_BROADCAST_DEVICEINTERFACE_W*>(buf.data());
    di->dbcc_size = static_cast<DWORD>(buf.size());
    di->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    std::memcpy(di->dbcc_name, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
    // The same message the OS sends; SendMessage returns once the window procedure has handled it.
    ::SendMessageW(h, WM_DEVICECHANGE, change == DeviceChange::Arrival ? DBT_DEVICEARRIVAL : DBT_DEVICEREMOVECOMPLETE, reinterpret_cast<LPARAM>(buf.data()));
    return true;
#else
    (void)change; (void)path;
    return false;
#endif
}

}  // namespace cascade::core::sys
