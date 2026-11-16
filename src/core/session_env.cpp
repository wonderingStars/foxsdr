// session_env.cpp - see session_env.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/session_env.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <thread>

#include "core/radio_report.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#else
#include <unistd.h>
#endif

namespace cascade::core {

namespace {

std::string trimmed(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
    return s.substr(a, b - a);
}

#if defined(_WIN32)

std::string regString(HKEY root, const char* path, const char* name) {
    char buf[256] = {0};
    DWORD cb = sizeof(buf) - 1;
    if (::RegGetValueA(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) { return std::string(); }
    return trimmed(buf);
}

// "\Registry\Machine\SYSTEM\..." (an adapter's DeviceKey) as a path under HKEY_LOCAL_MACHINE; empty when it is not one.
std::string machinePathOf(const std::string& deviceKey) {
    static const char kPrefix[] = "\\Registry\\Machine\\";
    if (deviceKey.size() <= sizeof(kPrefix) - 1) { return std::string(); }
    std::string head = deviceKey.substr(0, sizeof(kPrefix) - 1);
    for (char& c : head) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    std::string want = kPrefix;
    for (char& c : want) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return head == want ? deviceKey.substr(sizeof(kPrefix) - 1) : std::string();
}

BOOL CALLBACK countMonitor(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<SessionEnv*>(param);
    ++out->displayCount;
    // Per-monitor DPI is in shcore.dll (Windows 8.1 and later): looked up at run time so the program does not depend on it.
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static GetDpiForMonitorFn fn = []() -> GetDpiForMonitorFn {
        HMODULE m = ::LoadLibraryExA("shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m != nullptr ? reinterpret_cast<GetDpiForMonitorFn>(::GetProcAddress(m, "GetDpiForMonitor")) : nullptr;
    }();
    UINT dx = 0;
    UINT dy = 0;
    if (fn != nullptr && SUCCEEDED(fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx > 0 && out->displayScalePct.size() < 8) {
        out->displayScalePct.push_back(std::clamp<long long>(static_cast<long long>(dx) * 100 / 96, 25, 1000));
    }
    return TRUE;
}

#endif

}  // namespace

SessionEnv readSessionEnv() {
    SessionEnv e;
    e.cpuThreads = static_cast<int>(std::thread::hardware_concurrency());
#if defined(_WIN32)
    e.cpu = radioreport::cpuWithoutClock(regString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString"));
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    if (::GlobalMemoryStatusEx(&mem)) { e.ramMb = static_cast<std::uint64_t>(mem.ullTotalPhys / (1024ull * 1024ull)); }

    // The primary graphics adapter: its model name, and its driver version from the adapter's own registry key.
    DISPLAY_DEVICEA dd{};
    dd.cb = sizeof(dd);
    DISPLAY_DEVICEA chosen{};
    bool haveAdapter = false;
    for (DWORD i = 0; ::EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
        const bool primary = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        if (!haveAdapter || primary) {
            chosen = dd;
            haveAdapter = true;
            if (primary) { break; }
        }
        dd = DISPLAY_DEVICEA{};
        dd.cb = sizeof(dd);
    }
    if (haveAdapter) {
        e.gpu = trimmed(chosen.DeviceString);
        const std::string path = machinePathOf(chosen.DeviceKey);
        if (!path.empty()) { e.gpuDriver = regString(HKEY_LOCAL_MACHINE, path.c_str(), "DriverVersion"); }
    }

    // The USB host controllers' product names (the "Intel(R) USB 3.1 eXtensible Host Controller" of Device Manager), never a device path.
    HDEVINFO set = ::SetupDiGetClassDevsA(&GUID_DEVCLASS_USB, nullptr, nullptr, DIGCF_PRESENT);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dev;
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &dev) && e.usbHosts.size() < 8; ++i) {
            char name[256] = {0};
            if (!::SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(name), sizeof(name) - 1, nullptr)) { continue; }
            const std::string n = trimmed(name);
            std::string low = n;
            for (char& c : low) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            if (low.find("host controller") == std::string::npos) { continue; }
            if (std::find(e.usbHosts.begin(), e.usbHosts.end(), n) == e.usbHosts.end()) { e.usbHosts.push_back(radioreport::clipUtf8(n, 64)); }
        }
        ::SetupDiDestroyDeviceInfoList(set);
    }

    ::EnumDisplayMonitors(nullptr, nullptr, countMonitor, reinterpret_cast<LPARAM>(&e));
#else
    {
        std::ifstream in("/proc/cpuinfo");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("model name", 0) == 0) {
                const std::size_t colon = line.find(':');
                if (colon != std::string::npos) { e.cpu = radioreport::cpuWithoutClock(trimmed(line.substr(colon + 1))); }
                break;
            }
        }
    }
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long pageSize = ::sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) { e.ramMb = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize) / (1024ull * 1024ull); }
#endif
    e.cpu = radioreport::clipUtf8(e.cpu, 64);
    e.gpu = radioreport::clipUtf8(e.gpu, 64);
    e.gpuDriver = radioreport::versionText(e.gpuDriver);
    return e;
}

}  // namespace cascade::core
