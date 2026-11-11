// hid_win.hpp - Windows device side of the knob input: an overlapped-read worker with a BOUNDED close, and HID enumeration / opening for the supported knobs.
//
// WHY SetupDi + CreateFile + overlapped ReadFile, not Raw Input. Raw Input (RegisterRawInputDevices) delivers to a window's message loop: it would tie the
// device to the UI thread or need a hidden message-only window and thread of its own, and a report then arrives as a WM_INPUT message the window must
// pump. A worker thread that owns its own handle needs no window, reads the report the device sent byte for byte (Raw Input hands over the same bytes, but
// only for devices it can parse), and can be cancelled with CancelIoEx. Cost: the handle must be opened for read, which Windows refuses for the top-level
// collections of keyboards and mice; if a knob presents itself as one of those the open fails with ERROR_ACCESS_DENIED and says so (UNVERIFIED: not known
// for either device).
//
// THE BOUNDED CLOSE. OverlappedReader::stop() signals a stop event, CancelIoEx()es the pending read and waits for it to complete; if the driver does not
// complete a cancelled read within a second it closes the handle (which cancels everything on it) and waits again; if even that fails the worker's
// state is leaked on purpose (the kernel may still write into its buffer) and stop() returns false after at most `timeoutMs`. It never waits forever, and a
// device that is unplugged or silent cannot hang the caller.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#ifdef _WIN32

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hid_parse.hpp"
#include "input_event.hpp"

namespace cascade::input {

class OverlappedReader {
public:
    using ReportFn = std::function<void(const std::uint8_t* data, std::size_t len)>;  // runs on the worker thread
    using LostFn = std::function<void(unsigned winError)>;                           // device gone / read failed; runs on the worker, then it exits

    OverlappedReader();
    ~OverlappedReader();
    OverlappedReader(const OverlappedReader&) = delete;
    OverlappedReader& operator=(const OverlappedReader&) = delete;

    // Takes ownership of `handle`, which must have been opened with FILE_FLAG_OVERLAPPED. One read of `bufLen` bytes is outstanding at a time.
    bool start(HANDLE handle, std::size_t bufLen, ReportFn onReport, LostFn onLost);
    // True when the worker ended (or never ran) within timeoutMs. False: it was abandoned (see the header comment).
    bool stop(unsigned timeoutMs = 2000);
    bool running() const;

    struct State;  // internal; public only so the worker function can name it

private:
    std::shared_ptr<State> st_;
};

struct HidDeviceInfo {
    std::uint16_t vendor = 0, product = 0;
    HidKind kind = HidKind::ShuttleXpress;
    std::string name;      // the product table's name
    std::string stableId;  // hid:vvvv:pppp[#n]
    std::wstring path;     // device interface path (not shown to a user)
    std::uint16_t usagePage = 0, usage = 0;
    std::uint16_t inputReportBytes = 0;  // HidP_GetCaps().InputReportByteLength: report ID byte included
    bool plausible = false;              // long enough to hold this product's report (the right top-level collection)
};

struct HidEnumeration {
    std::vector<HidDeviceInfo> supported;  // every interface of a supported product, plausible or not
    int otherDevices = 0;                  // HID interfaces of other products: counted, never described
    int unreadable = 0;                    // interfaces that could not be opened for attributes
};

HidEnumeration enumerateHid();

class HidInputDevice {
public:
    HidInputDevice() = default;
    ~HidInputDevice() { close(); }
    HidInputDevice(const HidInputDevice&) = delete;
    HidInputDevice& operator=(const HidInputDevice&) = delete;

    // Opens the interface and starts reading; events go to `q` (which must outlive this object). On failure returns false and fills `error`.
    bool open(const HidDeviceInfo& info, EventQueue& q, std::string& error);
    // For tests and other transports: read reports of `kind` from an already-open overlapped handle (a named pipe in the tests). `reportBytes` includes the report ID byte.
    bool attach(HANDLE overlappedHandle, HidKind kind, const std::string& stableId, std::size_t reportBytes, EventQueue& q);
    bool close(unsigned timeoutMs = 2000);
    bool isOpen() const { return reader_.running(); }
    std::uint64_t reports() const { return reports_.load(); }
    std::uint64_t rejected() const { return rejected_.load(); }  // reports the parser refused (truncated or malformed)

private:
    OverlappedReader reader_;
    std::atomic<std::uint64_t> reports_{0}, rejected_{0};
    std::unique_ptr<ShuttleXpressParser> shuttle_;
    std::unique_ptr<PowerMateParser> powermate_;
};

}  // namespace cascade::input

#endif  // _WIN32
