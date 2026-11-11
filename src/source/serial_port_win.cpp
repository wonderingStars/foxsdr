// serial_port_win.cpp - see serial_port_win.hpp. UNVERIFIED ON HARDWARE.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/serial_port_win.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::source::tinysa {

bool isValidComPortName(const std::string& name) {
    std::string s = name;
    if (s.rfind("\\\\.\\", 0) == 0) { s = s.substr(4); }
    if (s.size() < 4 || s.size() > 6 || s.compare(0, 3, "COM") != 0) { return false; }
    unsigned n = 0;
    for (std::size_t i = 3; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') { return false; }
        n = n * 10 + static_cast<unsigned>(s[i] - '0');
    }
    if (s[3] == '0') { return false; }   // no "COM0", no leading zero
    return n >= 1 && n <= 256;
}

WinSerialLink::~WinSerialLink() { close(); }

#ifdef _WIN32

bool WinSerialLink::open(const std::string& name, unsigned readTimeoutMs) {
    close();
    if (!isValidComPortName(name)) { return false; }
    const std::string path = name.rfind("\\\\.\\", 0) == 0 ? name : "\\\\.\\" + name;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = readTimeoutMs;
    to.WriteTotalTimeoutConstant = 2000;
    if (!SetCommTimeouts(h, &to)) { CloseHandle(h); return false; }
    handle_ = h;
    return true;
}

void WinSerialLink::close() {
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool WinSerialLink::write(const std::uint8_t* data, std::size_t n) {
    if (handle_ == nullptr) { return false; }
    std::size_t done = 0;
    while (done < n) {
        DWORD w = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), data + done, static_cast<DWORD>(n - done), &w, nullptr) ||
            w == 0) {
            return false;
        }
        done += w;
    }
    return true;
}

long WinSerialLink::read(std::uint8_t* buf, std::size_t cap) {
    if (handle_ == nullptr) { return -1; }
    DWORD r = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(cap), &r, nullptr)) { return -1; }
    return static_cast<long>(r);
}

#else

bool WinSerialLink::open(const std::string&, unsigned) { return false; }
void WinSerialLink::close() { handle_ = nullptr; }
bool WinSerialLink::write(const std::uint8_t*, std::size_t) { return false; }
long WinSerialLink::read(std::uint8_t*, std::size_t) { return -1; }

#endif

}  // namespace cascade::source::tinysa
