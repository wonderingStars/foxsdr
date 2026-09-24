// winusb_device.hpp - the Windows-only seam into src/usb/winusb_device.cpp's
// WinUsbDevice, so a test can drive its CANCELLATION PATHS - the ones that
// only run once a device has already gone wrong - without a device, and
// deterministically.
//
// WHY A SEAM AND NOT A DONGLE. The two defects this exists for (bug hunt
// 2026-09-24) live on the path where a cancelled request does NOT come back
// within kAbortDrainWait: a bulk read or a control transfer the kernel still
// holds after AbortPipe + CancelIoEx. A healthy dongle cancels in well under
// a millisecond, so no bench can produce that state on demand. A table of
// the WinUSB and kernel calls WinUsbDevice makes on an OPEN device can: a
// fake that answers "pending" and never completes is exactly the wedged
// device, and a fake that completes later is exactly the late completion
// the leak exists to survive.
//
// Nothing outside winusb_device.cpp and tests/test_winusb_abandon.cpp
// includes this header, so the cross-platform contract in usb_device.hpp is
// untouched by it - the same split usbfs_device.hpp makes on Linux.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if defined(_WIN32)

#include <cstddef>
#include <memory>
#include <string>

// clang-format off
#include <windows.h>
#include <winusb.h>
// clang-format on

#include "usb/usb_device.hpp"

namespace cascade::usb {

// Every call WinUsbDevice makes on an open device, and nothing else. The
// events it waits on are real kernel events either way: a fake completes a
// request by filling in its OVERLAPPED and signalling hEvent, as the kernel
// does.
struct WinUsbApi {
    BOOL(__stdcall* controlTransfer)(WINUSB_INTERFACE_HANDLE, WINUSB_SETUP_PACKET, PUCHAR, ULONG,
                                     PULONG, LPOVERLAPPED);
    BOOL(__stdcall* readPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG,
                              LPOVERLAPPED);
    BOOL(__stdcall* getOverlappedResult)(WINUSB_INTERFACE_HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
    BOOL(__stdcall* abortPipe)(WINUSB_INTERFACE_HANDLE, UCHAR);
    BOOL(__stdcall* resetPipe)(WINUSB_INTERFACE_HANDLE, UCHAR);
    BOOL(__stdcall* setPipePolicy)(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, ULONG, PVOID);
    BOOL(__stdcall* setPowerPolicy)(WINUSB_INTERFACE_HANDLE, ULONG, ULONG, PVOID);
    BOOL(__stdcall* winUsbFree)(WINUSB_INTERFACE_HANDLE);
    BOOL(__stdcall* cancelIoEx)(HANDLE, LPOVERLAPPED);
    BOOL(__stdcall* closeFile)(HANDLE);
};

// The table openWinUsb() uses: the real WinUsb_* functions, CancelIoEx and
// CloseHandle.
const WinUsbApi& realWinUsbApi();

// A WinUsbDevice over `api` and the given handles, which only `api` ever
// sees. `api` must outlive the device.
std::unique_ptr<UsbDevice> makeWinUsbDeviceForTest(const WinUsbApi& api, HANDLE file,
                                                   WINUSB_INTERFACE_HANDLE winusb,
                                                   std::string path);

// How many overlapped request blocks (buffer + OVERLAPPED + event) exist in
// this process right now: a stream's ring, a device's control request, and
// every block the transport has abandoned to a request that never came back.
// A test asserts on the DELTA - an abandoned block must still exist after the
// device that owned it has been destroyed, because the kernel may still
// write into it.
std::size_t liveUsbRequestsForTest();

}  // namespace cascade::usb

#endif  // _WIN32
