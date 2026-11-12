// fcd_hid_win.cpp - the production HidLink for the FUNcube Dongle Pro / Pro+ on Windows: SetupAPI to list the HID
// interfaces, hid.dll for the report sizes, overlapped ReadFile / WriteFile with bounded waits. All of it ships
// with Windows; nothing is installed, no driver is bound, no dialog can appear (a device that cannot be opened
// is reported as an error sentence). Only interfaces whose device path carries the dongle's vendor and product
// id are ever opened - no other HID device (keyboards, mice, smart-card readers) is touched.
//
// NOT RUN ON A DONGLE and not exercised by any test except "enumeration finds nothing on a machine without one".
// Off Windows: no dongle is found.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "source/fcd_control.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// clang-format off
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
// clang-format on
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

namespace cascade::source {

#if defined(_WIN32)

namespace {

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) { s.push_back(c > 0 && c < 0x80 ? static_cast<char>(c) : '?'); }
    return s;
}

class WinHid final : public HidLink {
public:
    WinHid(HANDLE h, DWORD inLen, DWORD outLen) : h_(h), inLen_(inLen), outLen_(outLen) {
        wEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        rEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~WinHid() override { close(); }

    bool write(const std::uint8_t* report64, std::string& error) override {
        std::lock_guard<std::mutex> lk(wMu_);
        if (h_ == INVALID_HANDLE_VALUE) { error = "the dongle's control interface is closed"; return false; }
        std::vector<std::uint8_t> buf(std::max<DWORD>(outLen_, 1 + fcd::kReportBytes), 0);
        buf[0] = 0;  // report id
        std::memcpy(buf.data() + 1, report64, fcd::kReportBytes);
        OVERLAPPED ov{};
        ov.hEvent = wEvent_;
        ::ResetEvent(wEvent_);
        DWORD done = 0;
        if (!::WriteFile(h_, buf.data(), static_cast<DWORD>(std::max<DWORD>(outLen_, 1 + fcd::kReportBytes)), &done, &ov)) {
            if (::GetLastError() != ERROR_IO_PENDING) { error = "writing to the dongle failed"; return false; }
            if (::WaitForSingleObject(wEvent_, 500) != WAIT_OBJECT_0) {
                ::CancelIoEx(h_, &ov);
                ::WaitForSingleObject(wEvent_, 500);
                error = "writing to the dongle timed out";
                return false;
            }
            if (!::GetOverlappedResult(h_, &ov, &done, FALSE)) { error = "writing to the dongle failed"; return false; }
        }
        return true;
    }

    int read(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        std::lock_guard<std::mutex> lk(rMu_);
        if (h_ == INVALID_HANDLE_VALUE) { return -1; }
        std::vector<std::uint8_t> buf(std::max<DWORD>(inLen_, 1 + fcd::kReportBytes), 0);
        OVERLAPPED ov{};
        ov.hEvent = rEvent_;
        ::ResetEvent(rEvent_);
        DWORD got = 0;
        if (!::ReadFile(h_, buf.data(), static_cast<DWORD>(buf.size()), &got, &ov)) {
            if (::GetLastError() != ERROR_IO_PENDING) { return -1; }
            if (::WaitForSingleObject(rEvent_, timeoutMs) != WAIT_OBJECT_0) {
                ::CancelIoEx(h_, &ov);
                ::WaitForSingleObject(rEvent_, 500);
                return 0;
            }
            if (!::GetOverlappedResult(h_, &ov, &got, FALSE)) { return -1; }
        }
        if (got <= 1) { return 0; }
        const std::size_t n = std::min<std::size_t>(got - 1, cap);  // drop the report id
        std::memcpy(dst, buf.data() + 1, n);
        return static_cast<int>(n);
    }

    void close() override {
        if (h_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
        if (wEvent_) { ::CloseHandle(wEvent_); wEvent_ = nullptr; }
        if (rEvent_) { ::CloseHandle(rEvent_); rEvent_ = nullptr; }
    }

private:
    HANDLE h_;
    DWORD inLen_, outLen_;
    HANDLE wEvent_ = nullptr, rEvent_ = nullptr;
    std::mutex wMu_, rMu_;
};

bool pathHas(const std::string& lowerPath, std::uint16_t vid, std::uint16_t pid) {
    char want[40];
    std::snprintf(want, sizeof(want), "vid_%04x&pid_%04x", vid, pid);
    return lowerPath.find(want) != std::string::npos;
}

}  // namespace

std::vector<FcdDeviceInfo> enumerateFcdHid() {
    std::vector<FcdDeviceInfo> out;
    GUID hidGuid{};
    ::HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = ::SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { return out; }
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        if (!::SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd)) { break; }
        DWORD need = 0;
        ::SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0 || need > 4096) { continue; }
        std::vector<std::uint8_t> raw(need, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!::SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) { continue; }
        std::string path = narrow(detail->DevicePath);
        std::string lower = path;
        for (char& c : lower) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        FcdDeviceInfo d;
        if (pathHas(lower, fcd::kVid, fcd::kPidProPlus)) { d.isProPlus = true; }
        else if (pathHas(lower, fcd::kVid, fcd::kPidPro)) { d.isProPlus = false; }
        else { continue; }
        d.path = path;
        out.push_back(std::move(d));
    }
    ::SetupDiDestroyDeviceInfoList(set);
    std::sort(out.begin(), out.end(), [](const FcdDeviceInfo& a, const FcdDeviceInfo& b) { return a.path < b.path; });
    return out;
}

std::unique_ptr<HidLink> openFcdHid(const std::string& path, std::string& error) {
    std::string lower = path;
    for (char& c : lower) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (!pathHas(lower, fcd::kVid, fcd::kPidProPlus) && !pathHas(lower, fcd::kVid, fcd::kPidPro)) {
        error = "that is not a FUNcube Dongle control interface";
        return nullptr;
    }
    std::wstring wide(path.begin(), path.end());
    HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "the FUNcube Dongle's control interface could not be opened (is another program using it?)";
        return nullptr;
    }
    PHIDP_PREPARSED_DATA pre = nullptr;
    HIDP_CAPS caps{};
    if (!::HidD_GetPreparsedData(h, &pre) || ::HidP_GetCaps(pre, &caps) != HIDP_STATUS_SUCCESS) {
        if (pre) { ::HidD_FreePreparsedData(pre); }
        ::CloseHandle(h);
        error = "the FUNcube Dongle's control interface did not describe its reports";
        return nullptr;
    }
    ::HidD_FreePreparsedData(pre);
    return std::make_unique<WinHid>(h, caps.InputReportByteLength, caps.OutputReportByteLength);
}

#else

std::vector<FcdDeviceInfo> enumerateFcdHid() { return {}; }
std::unique_ptr<HidLink> openFcdHid(const std::string&, std::string& error) {
    error = "the FUNcube Dongle control link is built for Windows only";
    return nullptr;
}

#endif

}  // namespace cascade::source
