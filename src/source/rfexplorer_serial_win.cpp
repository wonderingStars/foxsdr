// rfexplorer_serial_win.cpp - see rfexplorer_serial_win.hpp. UNVERIFIED ON HARDWARE.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rfexplorer_serial_win.hpp"

#include <algorithm>

#include "source/serial_port_win.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::source::rfexplorer {

bool isSupportedBaud(int baud) {
    for (const int b : {500000, 115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200}) {
        if (b == baud) { return true; }
    }
    return false;
}

RfeSerialLink::~RfeSerialLink() { close(); }

#ifdef _WIN32

namespace {
HANDLE asHandle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }
constexpr const char* kPipePrefix = "\\\\.\\pipe\\foxsdr-rfe-";
constexpr int kWriteBoundMs = 2000;
}  // namespace

bool RfeSerialLink::openPath(const std::string& path, int baud, bool comm, unsigned readTimeoutMs, std::string& error) {
    close();
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        error = code == ERROR_ACCESS_DENIED ? "the port is in use by another program"
                : (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                    ? "there is no such port"
                    : "the port could not be opened (Windows error " + std::to_string(static_cast<unsigned long>(code)) + ")";
        return false;
    }
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ev == nullptr) {
        ::CloseHandle(h);
        error = "the port could not be set up (no event)";
        return false;
    }
    if (comm) {
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!::GetCommState(h, &dcb)) {
            ::CloseHandle(ev);
            ::CloseHandle(h);
            error = "that is not a serial port";
            return false;
        }
        dcb.BaudRate = static_cast<DWORD>(baud);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = 1;
        dcb.fParity = 0;
        dcb.fOutxCtsFlow = 0;
        dcb.fOutxDsrFlow = 0;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fDsrSensitivity = 0;
        dcb.fOutX = 0;
        dcb.fInX = 0;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fAbortOnError = 0;
        if (!::SetCommState(h, &dcb)) {
            ::CloseHandle(ev);
            ::CloseHandle(h);
            error = "the port would not take " + std::to_string(baud) + " baud";
            return false;
        }
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;
        to.ReadTotalTimeoutMultiplier = MAXDWORD;
        to.ReadTotalTimeoutConstant = readTimeoutMs;
        to.WriteTotalTimeoutConstant = kWriteBoundMs;
        ::SetCommTimeouts(h, &to);
        ::PurgeComm(h, PURGE_RXCLEAR | PURGE_RXABORT);
    }
    readTimeoutMs_ = readTimeoutMs;
    handle_ = reinterpret_cast<std::intptr_t>(h);
    readEvent_ = reinterpret_cast<std::intptr_t>(ev);
    return true;
}

bool RfeSerialLink::open(const std::string& comName, int baud, unsigned readTimeoutMs, std::string& error) {
    error.clear();
    if (!tinysa::isValidComPortName(comName)) {
        error = "the port name must be COM1 to COM256";
        return false;
    }
    if (!isSupportedBaud(baud)) {
        error = "the RF Explorer speaks 500000, 115200, 57600, 38400, 19200, 9600, 4800, 2400 or 1200 baud";
        return false;
    }
    const std::string path = comName.rfind("\\\\.\\", 0) == 0 ? comName : "\\\\.\\" + comName;
    return openPath(path, baud, true, std::clamp(readTimeoutMs, 10u, 5000u), error);
}

bool RfeSerialLink::openTestPipe(const std::string& pipeName, unsigned readTimeoutMs, std::string& error) {
    error.clear();
    if (pipeName.rfind(kPipePrefix, 0) != 0 || pipeName.size() > 100 || pipeName.find('/') != std::string::npos) {
        error = "only the pipe names of the tests are accepted";
        return false;
    }
    return openPath(pipeName, 0, false, std::clamp(readTimeoutMs, 10u, 5000u), error);
}

void RfeSerialLink::close() {
    if (handle_ != -1) {
        ::CloseHandle(asHandle(handle_));
        handle_ = -1;
    }
    if (readEvent_ != -1) {
        ::CloseHandle(asHandle(readEvent_));
        readEvent_ = -1;
    }
}

long RfeSerialLink::read(std::uint8_t* buf, std::size_t cap) {
    if (handle_ == -1 || buf == nullptr || cap == 0) { return -1; }
    HANDLE h = asHandle(handle_);
    HANDLE ev = asHandle(readEvent_);
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(cap, 0x7FFFFFFF));
    // A completed read of zero bytes is the timeout on a COM port and the end of the stream on a pipe.
    auto zeroBytes = [&]() -> long { return ::GetFileType(h) == FILE_TYPE_PIPE ? -1 : 0; };
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ::ResetEvent(ev);
    DWORD n = 0;
    if (::ReadFile(h, buf, want, &n, &ov)) { return n > 0 ? static_cast<long>(n) : zeroBytes(); }
    const DWORD code = ::GetLastError();
    if (code != ERROR_IO_PENDING) { return -1; }
    const DWORD wait = ::WaitForSingleObject(ev, readTimeoutMs_);
    if (wait == WAIT_OBJECT_0) {
        if (!::GetOverlappedResult(h, &ov, &n, FALSE)) { return -1; }
        return n > 0 ? static_cast<long>(n) : zeroBytes();
    }
    if (wait != WAIT_TIMEOUT) { return -1; }
    // Timed out with the read still posted: cancel it and WAIT for the cancel to land (the OS owns `buf` until it has), keeping data that won the race.
    ::CancelIoEx(h, &ov);
    if (::GetOverlappedResult(h, &ov, &n, TRUE)) { return n > 0 ? static_cast<long>(n) : 0; }
    return ::GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
}

bool RfeSerialLink::write(const std::uint8_t* data, std::size_t len) {
    if (handle_ == -1 || (data == nullptr && len > 0)) { return false; }
    HANDLE h = asHandle(handle_);
    std::size_t done = 0;
    while (done < len) {
        HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ev == nullptr) { return false; }
        OVERLAPPED ov{};
        ov.hEvent = ev;
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - done, 4096));
        bool ok = false;
        if (::WriteFile(h, data + done, want, &n, &ov)) {
            ok = true;
        } else if (::GetLastError() == ERROR_IO_PENDING) {
            if (::WaitForSingleObject(ev, kWriteBoundMs) == WAIT_OBJECT_0) {
                ok = ::GetOverlappedResult(h, &ov, &n, FALSE) != 0;
            } else {
                ::CancelIoEx(h, &ov);
                n = 0;
                ::GetOverlappedResult(h, &ov, &n, TRUE);
            }
        }
        ::CloseHandle(ev);
        if (!ok || n == 0) { return false; }
        done += n;
    }
    return true;
}

#else

bool RfeSerialLink::openPath(const std::string&, int, bool, unsigned, std::string& error) { error = "serial ports are supported on Windows only"; return false; }
bool RfeSerialLink::open(const std::string&, int, unsigned, std::string& error) { error = "serial ports are supported on Windows only"; return false; }
bool RfeSerialLink::openTestPipe(const std::string&, unsigned, std::string& error) { error = "serial ports are supported on Windows only"; return false; }
void RfeSerialLink::close() { handle_ = -1; readEvent_ = -1; }
long RfeSerialLink::read(std::uint8_t*, std::size_t) { return -1; }
bool RfeSerialLink::write(const std::uint8_t*, std::size_t) { return false; }

#endif

}  // namespace cascade::source::rfexplorer
