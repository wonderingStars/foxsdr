// probe_winusb_rawio.cpp - DEVELOPER BENCH PROBE. Not a test: not globbed by
// tests/CMakeLists.txt (no test_ prefix), not registered with ctest, not
// shipped. It answers ONE question that Microsoft's documentation does not:
// what does WinUsb_SetPipePolicy(RAW_IO) return for a pipe id the interface does
// not carry? (The review of 5f085271 infers Windows error 87; the Airspy HF+
// field fault is explained by that inference.)
//
// It opens the FIRST WinUSB-bound RTL-SDR (0bda:2838 / 0bda:2832) with the same
// exclusive overlapped CreateFileW the application uses, prints the interface's
// alternate settings and pipes, then calls SetPipePolicy(RAW_IO) on 0x81 (the
// real bulk IN pipe: expected to succeed) and on 0x82 and 0x05 (pipes that do
// not exist: the question), prints each Windows error, and closes. It sends no
// control transfer and queues no read. Close FoxSDR and any other SDR program
// first: the handle is exclusive.
//
//   cmake --build <build dir> --config Release --target probe_winusb_rawio
//   <build dir>\tests\Release\probe_winusb_rawio.exe
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#if defined(_WIN32)

#include <cstdio>
#include <string>
#include <vector>

// clang-format off
#include <windows.h>
#include <winusb.h>
// clang-format on

#include "usb/usb_device.hpp"

namespace {

void tryRawIo(WINUSB_INTERFACE_HANDLE h, UCHAR pipe) {
    UCHAR one = TRUE;
    ::SetLastError(0);
    const BOOL ok = ::WinUsb_SetPipePolicy(h, pipe, RAW_IO, sizeof(one), &one);
    if (ok != FALSE) {
        std::printf("SetPipePolicy(RAW_IO) on pipe 0x%02X: succeeded\n", static_cast<unsigned>(pipe));
    } else {
        std::printf("SetPipePolicy(RAW_IO) on pipe 0x%02X: FAILED, Windows error %lu\n",
                    static_cast<unsigned>(pipe), static_cast<unsigned long>(::GetLastError()));
    }
}

}  // namespace

int main() {
    const std::vector<cascade::usb::UsbId> ids = {{0x0bda, 0x2838}, {0x0bda, 0x2832}};
    const std::vector<cascade::usb::UsbDeviceInfo> found = cascade::usb::enumerateWinUsb(ids);
    if (found.empty()) {
        std::printf("no WinUSB-bound RTL-SDR found\n");
        return 2;
    }
    std::printf("opening the first of %zu device(s) (no serial printed)\n", found.size());
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, found[0].path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<std::size_t>(n > 0 ? n : 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, found[0].path.c_str(), -1, wpath.data(), n);
    const HANDLE file = ::CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                      nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("CreateFileW failed, Windows error %lu (is another program using it?)\n",
                    static_cast<unsigned long>(::GetLastError()));
        return 3;
    }
    WINUSB_INTERFACE_HANDLE h = nullptr;
    if (::WinUsb_Initialize(file, &h) == FALSE) {
        std::printf("WinUsb_Initialize failed, Windows error %lu\n",
                    static_cast<unsigned long>(::GetLastError()));
        ::CloseHandle(file);
        return 4;
    }
    for (UCHAR idx = 0; idx < 8; ++idx) {
        USB_INTERFACE_DESCRIPTOR d{};
        if (::WinUsb_QueryInterfaceSettings(h, idx, &d) == FALSE) { break; }
        std::printf("setting index %u: bAlternateSetting %u, %u endpoint(s)\n",
                    static_cast<unsigned>(idx), static_cast<unsigned>(d.bAlternateSetting),
                    static_cast<unsigned>(d.bNumEndpoints));
        for (UCHAR p = 0; p < d.bNumEndpoints; ++p) {
            WINUSB_PIPE_INFORMATION pi{};
            if (::WinUsb_QueryPipe(h, idx, p, &pi) == FALSE) { continue; }
            std::printf("  pipe 0x%02X type %d packet %u\n", static_cast<unsigned>(pi.PipeId),
                        static_cast<int>(pi.PipeType), static_cast<unsigned>(pi.MaximumPacketSize));
        }
    }
    tryRawIo(h, 0x81);
    tryRawIo(h, 0x82);
    tryRawIo(h, 0x05);
    ::WinUsb_Free(h);
    ::CloseHandle(file);
    std::printf("closed\n");
    return 0;
}

#else
int main() { return 0; }
#endif
