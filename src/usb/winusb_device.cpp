// The WinUSB transport behind usb_device.hpp: SetupAPI to list, WinUSB to
// talk, and not one line of libusb anywhere near a radio we own.
//
// WHAT THE BENCH SAID ABOUT ENUMERATION, and why this file does not do what
// the header's first sketch described. The header says devices are listed
// "through SetupAPI device-interface enumeration ... for
// GUID_DEVINTERFACE_USB_DEVICE". Measured against the RTL-SDR on this desk
// (RTL2838UHIDIR, serial 00000001, bound to WinUSB with Zadig), that GUID
// finds the WRONG NODE and would have made the driver unopenable:
//
//   GUID_DEVINTERFACE_USB_DEVICE -> USB\VID_0BDA&PID_2838\00000001
//                                   service usbccgp  (the composite parent)
//   {dee824ef-...} (WinUSB)      -> USB\VID_0BDA&PID_2838&MI_00\7&...&0000
//                                   service WinUSB   (the interface WE want)
//
// A DVB dongle enumerates as a USB COMPOSITE device, so the WinUSB driver is
// bound to the interface child (MI_00), not to the device. usbhub registers
// GUID_DEVINTERFACE_USB_DEVICE for the parent only; the child's interface
// path is registered under whatever GUID its INF declared - libwdi/Zadig
// writes its own into Device Parameters\DeviceInterfaceGUIDs, and winusb.sys
// additionally registers GUID_DEVINTERFACE_WINUSB. Opening the parent path
// succeeds and WinUsb_Initialize on it then fails, because usbccgp owns it.
//
// So the walk here is: find the DEVNODES whose hardware ids carry a VID/PID
// the caller asked for AND whose service is WinUSB, then resolve each one's
// interface PATH from the GUIDs it actually registered (its own
// DeviceInterfaceGUIDs first, then GUID_DEVINTERFACE_WINUSB, then
// GUID_DEVINTERFACE_USB_DEVICE for a non-composite device that genuinely has
// only that one). Rule 1 of the contract is kept exactly: nothing is opened
// and no transfer is sent to produce the list.
//
// THE SERIAL AND THE DESCRIPTION COME FROM THE PARENT for the same reason.
// The MI_00 child's instance id ends "7&38A180A5&0&0000" - a bus path, not a
// serial - and its bus-reported description is "Bulk-In, Interface". The
// parent carries both of the things a user would recognise: instance id
// USB\VID_0BDA&PID_2838\00000001 (serial "00000001") and bus description
// "RTL2838UHIDIR". So this walks up with CM_Get_Parent past any "&MI_"
// composite-interface node and reads them there.
//
// EVERY WAIT IS BOUNDED (rule 3). Control transfers are overlapped and
// waited on with the caller's timeout, then cancelled; bulk reads are a ring
// of overlapped WinUsb_ReadPipe requests with RAW_IO set, waited on one at a
// time; endBulkStream aborts the pipe, cancels every request and waits
// kAbortDrainWait (250 ms) TOTAL for the whole ring to drain. If a request
// still has not completed after that the ring is LEAKED rather than freed -
// the kernel may still write into those buffers, and a freed buffer under a
// live DMA write is a worse defect than a leak on a path that only happens
// when a device has already gone wrong. Same reasoning as SoapySource's
// abandoned-driver policy, one layer down. A control transfer whose
// cancellation does not land within the same bound is leaked the same way,
// and never held the caller's buffer or a stack OVERLAPPED to begin with.
// Leaked means handed to abandonRequest()'s process-lifetime store - NOT kept
// in the device, whose own member teardown would free it moments later
// (tests/test_winusb_abandon.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "usb/usb_device.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_WIN32)

// clang-format off
#include <windows.h>
// initguid.h must precede the headers that DECLARE the GUIDs, or they are
// only extern references and the link fails.
#include <initguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <usbiodef.h>
#include <winusb.h>
// CLSIDFromString, for the per-install interface GUIDs libwdi writes into
// Device Parameters as strings.
#include <objbase.h>
// clang-format on

#include <atomic>

#include "usb/winusb_device.hpp"

#endif  // _WIN32

namespace cascade::usb {

namespace {

#if defined(_WIN32)

// THE ONE BOUNDED WAIT THIS FILE PERFORMS. endBulkStream() aborts the pipe
// and cancels every queued read, then gives the whole ring this long - not
// this long EACH - to come back before it gives up and leaks it. Registered
// in tests/test_shutdown_budget.cpp.
constexpr std::chrono::milliseconds kAbortDrainWait{250};

// GUID_DEVINTERFACE_WINUSB. winusb.sys registers this for every interface it
// binds, whatever the INF's own DeviceInterfaceGUIDs said, so it is the one
// GUID that is always there - but it is declared in no public header, hence
// the literal (value from the WDK's winusbio/usb.h documentation, and
// confirmed present on this bench's dongle by direct enumeration).
const GUID kWinUsbInterfaceGuid = {
    0xdee824ef, 0x729b, 0x4a0e, {0x9c, 0x14, 0xb7, 0x11, 0x7d, 0x33, 0xa8, 0x17}};

std::string narrow(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return std::string(); }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) { return std::wstring(); }
    std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string lastErrorText(const char* what, DWORD err) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed (Windows error %lu)", what,
                  static_cast<unsigned long>(err));
    return std::string(buf);
}

// One string device property, empty when absent. REG_MULTI_SZ properties
// (hardware ids) come back as the first string plus embedded NULs, which is
// enough for a substring match on "VID_xxxx&PID_yyyy" only if we read the
// whole buffer - so the multi-sz case is joined with '\n'.
std::wstring deviceStringProperty(HDEVINFO set, SP_DEVINFO_DATA& info, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    DWORD needed = 0;
    ::SetupDiGetDevicePropertyW(set, &info, &key, &type, nullptr, 0, &needed, 0);
    if (needed == 0) { return std::wstring(); }
    std::vector<BYTE> buf(needed + sizeof(wchar_t), 0);
    if (::SetupDiGetDevicePropertyW(set, &info, &key, &type, buf.data(), needed, &needed, 0) ==
        FALSE) {
        return std::wstring();
    }
    const auto* p = reinterpret_cast<const wchar_t*>(buf.data());
    if (type == DEVPROP_TYPE_STRING_LIST) {
        std::wstring joined;
        while (*p != L'\0') {
            if (!joined.empty()) { joined.push_back(L'\n'); }
            joined.append(p);
            p += std::wcslen(p) + 1;
        }
        return joined;
    }
    return std::wstring(p);
}

std::wstring instanceIdOf(DEVINST inst) {
    wchar_t buf[MAX_DEVICE_ID_LEN + 1] = {0};
    if (::CM_Get_Device_IDW(inst, buf, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
        return std::wstring();
    }
    return std::wstring(buf);
}

// The last '\' separated segment of a device instance id. For a USB device
// node that is the serial string the device reported, or a bus-generated id
// (containing '&') when it reported none.
std::string lastInstanceSegment(const std::wstring& instanceId) {
    const std::size_t at = instanceId.find_last_of(L'\\');
    const std::wstring tail =
        (at == std::wstring::npos) ? instanceId : instanceId.substr(at + 1);
    // A bus-generated id is not a serial: "7&38A180A5&0&0000". Devices that
    // really do report a serial never have '&' in it.
    if (tail.find(L'&') != std::wstring::npos) { return std::string(); }
    return narrow(tail.c_str());
}

// Walks up past composite-interface nodes ("...&MI_00\...") to the USB device
// node that owns the serial and the recognisable description. Returns the
// devnode itself when it is not a composite child.
DEVINST usbDeviceNodeOf(DEVINST inst) {
    for (int hops = 0; hops < 4; ++hops) {
        const std::wstring id = instanceIdOf(inst);
        if (id.find(L"&MI_") == std::wstring::npos) { return inst; }
        DEVINST parent = 0;
        if (::CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS) { return inst; }
        inst = parent;
    }
    return inst;
}

// The interface GUIDs this devnode's driver declared for itself, read from
// Device Parameters\DeviceInterfaceGUIDs (REG_MULTI_SZ). This is where
// libwdi/Zadig puts the GUID it generated for the device, and on this bench's
// dongle it is {E12D70D7-...} - a GUID no header knows.
std::vector<GUID> declaredInterfaceGuids(HDEVINFO set, SP_DEVINFO_DATA& info) {
    std::vector<GUID> out;
    const HKEY key = ::SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) { return out; }
    DWORD type = 0;
    DWORD size = 0;
    if (::RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type, nullptr, &size) ==
            ERROR_SUCCESS &&
        size > 0) {
        std::vector<BYTE> buf(size + 2 * sizeof(wchar_t), 0);
        if (::RegQueryValueExW(key, L"DeviceInterfaceGUIDs", nullptr, &type, buf.data(), &size) ==
            ERROR_SUCCESS) {
            const auto* p = reinterpret_cast<const wchar_t*>(buf.data());
            while (*p != L'\0') {
                GUID g{};
                if (::CLSIDFromString(p, &g) == NOERROR) { out.push_back(g); }
                p += std::wcslen(p) + 1;
            }
        }
    }
    ::RegCloseKey(key);
    return out;
}

// The interface path this devnode exposes under `guid`, or empty. Uses
// CM_Get_Device_Interface_List rather than a second SetupDi walk because it
// takes the instance id directly - no scanning, no chance of matching a
// different device with the same VID/PID.
std::string interfacePathFor(const GUID& guid, const std::wstring& instanceId) {
    GUID g = guid;
    ULONG len = 0;
    if (::CM_Get_Device_Interface_List_SizeW(&len, &g, const_cast<wchar_t*>(instanceId.c_str()),
                                             CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        len <= 1) {
        return std::string();
    }
    std::vector<wchar_t> buf(len, L'\0');
    if (::CM_Get_Device_Interface_ListW(&g, const_cast<wchar_t*>(instanceId.c_str()), buf.data(),
                                        len,
                                        CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return std::string();
    }
    if (buf[0] == L'\0') { return std::string(); }
    return narrow(buf.data());
}

// "VID_0BDA&PID_2838" as the hardware id spells it, upper case.
std::wstring hardwareIdFragment(std::uint16_t vid, std::uint16_t pid) {
    wchar_t buf[32];
    ::swprintf(buf, 32, L"VID_%04X&PID_%04X", static_cast<unsigned>(vid),
               static_cast<unsigned>(pid));
    return std::wstring(buf);
}

// ---------------------------------------------------------------------------
// The device itself.
// ---------------------------------------------------------------------------

// Request blocks alive in this process; see liveUsbRequestsForTest().
std::atomic<std::size_t> g_liveRequests{0};

// One overlapped request: the buffer the kernel reads or writes, the
// OVERLAPPED it completes, and the event it signals. Both the bulk ring and
// the control path are made of these, and both hand one to the kernel for as
// long as it may still touch it - which, after a cancel that did not land,
// is longer than the device lives.
struct Request {
    Request() { g_liveRequests.fetch_add(1, std::memory_order_relaxed); }
    ~Request() {
        if (event != nullptr) { ::CloseHandle(event); }
        g_liveRequests.fetch_sub(1, std::memory_order_relaxed);
    }
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    OVERLAPPED overlapped{};
    HANDLE event = nullptr;
    std::vector<std::uint8_t> buffer;
    bool pending = false;
};

// THE LEAK, and where it lives. A request whose cancellation did not land
// within kAbortDrainWait may still be completed by the kernel - bytes into
// its buffer, status into its OVERLAPPED, a SetEvent on its event - at any
// later moment. So it is handed here, to a store that is never destroyed:
// not a member of the device (the device's own member teardown freed the
// "leaked" ring microseconds after the leak, which is what this replaced),
// and not a plain static (its destructor would run at exit while the kernel
// may still hold the requests). Heap-allocated on first use and never
// deleted, so "for the life of the process" is literally true. It is also
// what keeps them reachable for a debugger.
void abandonRequest(std::unique_ptr<Request> req) {
    if (req == nullptr) { return; }
    static std::mutex* const mutex = new std::mutex;
    static auto* const store = new std::vector<std::unique_ptr<Request>>;
    std::lock_guard<std::mutex> lk(*mutex);
    store->push_back(std::move(req));
}

class WinUsbDevice final : public UsbDevice {
public:
    WinUsbDevice(const WinUsbApi& api, HANDLE file, WINUSB_INTERFACE_HANDLE winusb,
                 std::string path)
        : api_(&api), file_(file), winusb_(winusb), path_(std::move(path)) {}

    ~WinUsbDevice() override {
        endBulkStream();
        if (winusb_ != nullptr) { api_->winUsbFree(winusb_); }
        if (file_ != INVALID_HANDLE_VALUE) { api_->closeFile(file_); }
        // Requests the drain gave up on are not ours any more: endBulkStream()
        // and control() hand them to abandonRequest(), so nothing this
        // object's member teardown frees can still be in the kernel's hands.
    }

    WinUsbDevice(const WinUsbDevice&) = delete;
    WinUsbDevice& operator=(const WinUsbDevice&) = delete;

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned timeoutMs) override {
        return control(requestType, request, value, index, data, nullptr, len, timeoutMs);
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        return control(requestType, request, value, index, nullptr, data, len, timeoutMs);
    }

    bool beginBulkStream(std::uint8_t endpoint, std::size_t bufferBytes,
                         std::size_t bufferCount) override {
        if (streaming()) { return true; }
        if (winusb_ == nullptr) {
            setError("beginBulkStream() on a closed device");
            return false;
        }
        // RAW_IO's rule, enforced here rather than discovered as a failed
        // read: every transfer must be a whole number of maximum-size
        // packets. 512 is the high-speed bulk maximum and what every dongle
        // this transport serves actually reports.
        if (bufferBytes == 0 || (bufferBytes % kMaxPacketBytes) != 0) {
            setError("beginBulkStream() needs a buffer size that is a multiple of 512 bytes");
            return false;
        }
        if (bufferCount == 0 || bufferCount > kMaxRingSize) {
            setError("beginBulkStream() needs between 1 and 32 buffers");
            return false;
        }
        // RAW_IO: no buffering in winusb.sys, the transfer goes straight at
        // the pipe. This is what keeps a 2.4 MS/s stream from adding a copy
        // and a scheduling hop per buffer.
        UCHAR raw = TRUE;
        if (api_->setPipePolicy(winusb_, endpoint, RAW_IO, sizeof(raw), &raw) == FALSE) {
            setError(lastErrorText("WinUsb_SetPipePolicy(RAW_IO)", ::GetLastError()));
            return false;
        }

        // A STALLED PIPE SELF-CLEARS. Without this a single stall - which
        // some firmware produces after a mode change - fails one read and
        // then every read after it. Measured on the bench dongle: with the
        // endpoint deliberately halted (SET_FEATURE(ENDPOINT_HALT) on 0x81),
        // the first read returns Windows error 31 and the SECOND one
        // delivers 16384 bytes. Off, the stall is permanent.
        UCHAR on = TRUE;
        api_->setPipePolicy(winusb_, endpoint, AUTO_CLEAR_STALL, sizeof(on), &on);

        // NO SELECTIVE SUSPEND WHILE A RADIO IS STREAMING. Whether WinUSB
        // idles this device at all comes from its INF, which on a
        // Zadig-installed dongle is not ours to predict; a receiver that is
        // asleep delivers nothing and reports no error.
        UCHAR off = FALSE;
        api_->setPowerPolicy(winusb_, AUTO_SUSPEND, sizeof(off), &off);

        // THE PIPE IS RESET BEFORE THE FIRST READ, and this is not
        // defensive tidiness - it is the difference between a dongle that
        // streams and one that silently never does.
        //
        // WinUsb_ResetPipe sends CLEAR_FEATURE(ENDPOINT_HALT) to the device
        // and resynchronises the data toggle at both ends. Whatever the
        // PREVIOUS owner of this dongle left behind - a halted endpoint, a
        // toggle out of step after a process was killed mid-stream - survives
        // a handle close, so the first program to open it inherits it. A
        // libusb application gets this for free from its interface claim; we
        // have to ask.
        //
        // Proven on the bench: with the endpoint deliberately halted, reads
        // without this line fail (Windows error 31) and reads with it deliver
        // immediately. It is also the best explanation for the state this
        // driver was first run against, in which every read on a correctly
        // configured dongle timed out with no error at all until an
        // unrelated libusb program had claimed and released the interface.
        api_->resetPipe(winusb_, endpoint);

        endpoint_ = endpoint;
        ring_.clear();
        ring_.resize(bufferCount);
        for (std::size_t i = 0; i < bufferCount; ++i) {
            ring_[i] = std::make_unique<Request>();
            ring_[i]->buffer.assign(bufferBytes, 0);
            ring_[i]->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (ring_[i]->event == nullptr) {
                setError(lastErrorText("CreateEvent for a bulk buffer", ::GetLastError()));
                endBulkStream();
                return false;
            }
        }
        head_ = 0;
        streaming_ = true;
        for (std::size_t i = 0; i < ring_.size(); ++i) {
            if (!queue(*ring_[i])) {
                endBulkStream();
                return false;
            }
        }
        return true;
    }

    int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        if (!streaming_) {
            setError("readBulk() with no stream");
            return -1;
        }
        Request& req = *ring_[head_];
        if (!req.pending) {
            // Re-queue a slot whose previous submit failed transiently.
            if (!queue(req)) { return -1; }
        }
        const DWORD waited = ::WaitForSingleObject(req.event, timeoutMs);
        if (waited == WAIT_TIMEOUT) { return 0; }
        if (waited != WAIT_OBJECT_0) {
            setError(lastErrorText("waiting for a bulk read", ::GetLastError()));
            return -1;
        }
        DWORD moved = 0;
        if (api_->getOverlappedResult(winusb_, &req.overlapped, &moved, FALSE) == FALSE) {
            const DWORD err = ::GetLastError();
            req.pending = false;
            // A device that has gone (unplugged, or the driver torn from
            // under us) is a NEGATIVE return, never a hang and never a
            // silent zero: the driver above turns it into faulted().
            setError(lastErrorText("a bulk read", err));
            return -1;
        }
        req.pending = false;
        const std::size_t got = std::min<std::size_t>(moved, cap);
        if (got > 0) { std::memcpy(dst, req.buffer.data(), got); }
        head_ = (head_ + 1) % ring_.size();
        if (!queue(req)) { return -1; }
        return static_cast<int>(got);
    }

    void endBulkStream() override {
        if (!streaming_ && ring_.empty()) { return; }
        streaming_ = false;
        if (winusb_ != nullptr && endpoint_ != 0) {
            // Both, in this order: AbortPipe tells the function driver to
            // fail everything queued on the pipe, CancelIoEx covers requests
            // the I/O manager has not handed down yet.
            api_->abortPipe(winusb_, endpoint_);
        }
        for (auto& slot : ring_) {
            if (slot && slot->pending && file_ != INVALID_HANDLE_VALUE) {
                api_->cancelIoEx(file_, &slot->overlapped);
            }
        }
        // ONE deadline for the whole ring, exactly as the header states.
        const auto deadline = std::chrono::steady_clock::now() + kAbortDrainWait;
        bool allDrained = true;
        for (auto& slot : ring_) {
            if (!slot || !slot->pending) { continue; }
            const auto now = std::chrono::steady_clock::now();
            DWORD budget = 0;
            if (now < deadline) {
                budget = static_cast<DWORD>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            }
            if (::WaitForSingleObject(slot->event, budget) == WAIT_OBJECT_0) {
                DWORD moved = 0;
                api_->getOverlappedResult(winusb_, &slot->overlapped, &moved, FALSE);
                slot->pending = false;
            } else {
                allDrained = false;
            }
        }
        if (allDrained) {
            ring_.clear();  // each Request closes its own event
        } else {
            // THE LEAK, on purpose. At least one request is still live in the
            // kernel with a pointer into slot->buffer; freeing it would hand
            // the DMA engine memory the allocator has given away. The whole
            // ring goes to abandonRequest(), which keeps every buffer and
            // event alive for the life of the process - past this device's
            // own destruction, which is the case that matters: a driver
            // resets its UsbDevice straight after the failure that got it
            // here - and leaves the next beginBulkStream() a clean slate.
            for (auto& slot : ring_) { abandonRequest(std::move(slot)); }
            ring_.clear();
            setError("a cancelled bulk read did not complete within the drain bound; its "
                     "buffer is deliberately leaked rather than freed under the device");
        }
        endpoint_ = 0;
        head_ = 0;
    }

    bool streaming() const override { return streaming_; }

    bool resetPipe(std::uint8_t endpoint) override {
        if (winusb_ == nullptr) { return false; }
        if (api_->resetPipe(winusb_, endpoint) == FALSE) {
            setError(lastErrorText("WinUsb_ResetPipe", ::GetLastError()));
            return false;
        }
        return true;
    }

    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return lastError_; }

private:
    static constexpr std::size_t kMaxPacketBytes = 512;
    static constexpr std::size_t kMaxRingSize = 32;

    void setError(std::string msg) { lastError_ = std::move(msg); }

    bool queue(Request& req) {
        std::memset(&req.overlapped, 0, sizeof(req.overlapped));
        ::ResetEvent(req.event);
        req.overlapped.hEvent = req.event;
        ULONG moved = 0;
        if (api_->readPipe(winusb_, endpoint_, req.buffer.data(),
                              static_cast<ULONG>(req.buffer.size()), &moved,
                              &req.overlapped) != FALSE) {
            // Completed synchronously; the event is signalled either way, so
            // the read path needs no special case.
            req.pending = true;
            return true;
        }
        const DWORD err = ::GetLastError();
        if (err == ERROR_IO_PENDING) {
            req.pending = true;
            return true;
        }
        req.pending = false;
        setError(lastErrorText("queueing a bulk read", err));
        return false;
    }

    // `out` is the caller's bytes for an OUT transfer and `in` the caller's
    // buffer for an IN one; the other is null (both are when len is 0).
    int control(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                std::uint16_t index, const std::uint8_t* out, std::uint8_t* in, std::size_t len,
                unsigned timeoutMs) {
        if (winusb_ == nullptr) {
            setError("control transfer on a closed device");
            return -1;
        }
        if (len > 0xFFFF) {
            setError("control transfer longer than a USB setup packet can describe");
            return -1;
        }
        // One control request per device, so two threads may not be in here
        // at once. The drivers serialise their own control calls anyway; this
        // is the belt to that braces.
        std::lock_guard<std::mutex> lk(controlMutex_);

        // THE KERNEL NEVER HOLDS THE CALLER'S MEMORY, OR THIS FRAME'S. The
        // transfer runs on a heap Request - its own OVERLAPPED, its own event,
        // its own copy of the data - because a transfer whose cancellation
        // does not land in time is still the kernel's when this function
        // returns, and a late completion then writes its status and bytes
        // into whatever that memory has become. With a stack OVERLAPPED and
        // the caller's `data` that was a dead stack frame and a buffer the
        // caller had long since reused; and with one event shared by every
        // transfer, the stale completion also woke the NEXT transfer early.
        // A request that has to be abandoned is handed to abandonRequest()
        // and the next transfer gets a fresh one, event and all.
        if (control_ == nullptr) {
            auto fresh = std::make_unique<Request>();
            fresh->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (fresh->event == nullptr) {
                setError(lastErrorText("CreateEvent for a control transfer", ::GetLastError()));
                return -1;
            }
            control_ = std::move(fresh);
        }
        Request& req = *control_;
        req.buffer.resize(len);
        if (out != nullptr && len > 0) { std::memcpy(req.buffer.data(), out, len); }

        WINUSB_SETUP_PACKET setup{};
        setup.RequestType = requestType;
        setup.Request = request;
        setup.Value = value;
        setup.Index = index;
        setup.Length = static_cast<USHORT>(len);

        std::memset(&req.overlapped, 0, sizeof(req.overlapped));
        ::ResetEvent(req.event);
        req.overlapped.hEvent = req.event;
        ULONG moved = 0;
        if (api_->controlTransfer(winusb_, setup, len > 0 ? req.buffer.data() : nullptr,
                                  static_cast<ULONG>(len), &moved, &req.overlapped) != FALSE) {
            return finishControl(req, in, moved);
        }
        const DWORD err = ::GetLastError();
        if (err != ERROR_IO_PENDING) {
            setError(lastErrorText("a control transfer", err));
            return -1;
        }
        const DWORD waited = ::WaitForSingleObject(req.event, timeoutMs);
        if (waited != WAIT_OBJECT_0) {
            // Bounded, and then abandoned properly rather than left in
            // flight: cancel, and wait - bounded again - for the cancellation
            // itself, which on a healthy bus the I/O manager completes
            // promptly.
            api_->cancelIoEx(file_, &req.overlapped);
            if (::WaitForSingleObject(req.event, static_cast<DWORD>(kAbortDrainWait.count())) !=
                WAIT_OBJECT_0) {
                // THE CANCEL DID NOT LAND: the kernel still holds this request.
                // Leaked on purpose, exactly as endBulkStream() leaks a ring
                // that will not drain; the caller's buffer was never in it.
                abandonRequest(std::move(control_));
                setError("a control transfer did not answer within its timeout, and its "
                         "cancellation did not complete within the drain bound; its buffer is "
                         "deliberately leaked rather than freed under the device");
                return -1;
            }
            setError("a control transfer did not answer within its timeout");
            return -1;
        }
        if (api_->getOverlappedResult(winusb_, &req.overlapped, &moved, FALSE) == FALSE) {
            setError(lastErrorText("a control transfer", ::GetLastError()));
            return -1;
        }
        return finishControl(req, in, moved);
    }

    // The count the transfer really moved (never more than was asked for),
    // with an IN transfer's bytes copied out to the caller.
    static int finishControl(const Request& req, std::uint8_t* in, ULONG moved) {
        const std::size_t got = std::min<std::size_t>(moved, req.buffer.size());
        if (in != nullptr && got > 0) { std::memcpy(in, req.buffer.data(), got); }
        return static_cast<int>(got);
    }

    const WinUsbApi* api_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winusb_ = nullptr;
    std::string path_;
    std::string lastError_;

    std::mutex controlMutex_;
    std::unique_ptr<Request> control_;  // created on first use; see control()

    std::uint8_t endpoint_ = 0;
    bool streaming_ = false;
    std::size_t head_ = 0;
    std::vector<std::unique_ptr<Request>> ring_;
};

#endif  // _WIN32

}  // namespace

bool usbIdFromHardwareId(const std::string& hardwareId, UsbId& out) {
    const auto upper = [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    };
    std::string s;
    s.reserve(hardwareId.size());
    for (char c : hardwareId) { s.push_back(upper(c)); }
    const auto hex4 = [&s](std::size_t at, std::uint16_t& v) {
        if (at + 4 > s.size()) { return false; }
        unsigned acc = 0;
        for (std::size_t i = at; i < at + 4; ++i) {
            const char c = s[i];
            unsigned d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<unsigned>(c - '0');
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
            acc = acc * 16u + d;
        }
        v = static_cast<std::uint16_t>(acc);
        return true;
    };
    const std::size_t vidAt = s.find("VID_");
    if (vidAt == std::string::npos) { return false; }
    const std::size_t pidAt = s.find("PID_", vidAt + 4);
    if (pidAt == std::string::npos || pidAt != vidAt + 9 || s[vidAt + 8] != '&') { return false; }
    UsbId id{0, 0};
    if (!hex4(vidAt + 4, id.vid) || !hex4(pidAt + 4, id.pid)) { return false; }
    out = id;
    return true;
}

// THE CLASSIFICATION, and the two rules in it that a bench with one correctly
// bound dongle can never exercise.
//
//  1. A DEVICE IS BOUND IF ANY OF ITS DEVNODES IS. An RTL2832U is composite:
//     the USB device node runs usbccgp and the interface child MI_00 is what
//     Zadig binds to WinUSB. Asking the DEVICE node what service it runs
//     therefore answers "usbccgp" on a perfectly working dongle, and a rule
//     written that way would report every RTL-SDR on the machine as needing
//     Zadig - including the one currently streaming.
//  2. ONE ENTRY PER PHYSICAL DEVICE. The hardware-id fragment "VID_0BDA&PID_2838"
//     is a substring of the child's "VID_0BDA&PID_2838&MI_00" as well as the
//     parent's own id, so the walk sees the same dongle two or three times.
//     The device node is the one that carries the serial and the model name,
//     so it is the one reported.
//
// `path` is left empty on every entry - see the header: an unbound device has
// no interface to open, so there is nothing to put there.
std::vector<UsbDeviceInfo> unboundFrom(const std::vector<UsbNode>& nodes) {
    // Windows service names are case-insensitive and are reported with
    // whatever casing the INF used; "winusb", "WinUSB" and "WINUSB" are one
    // driver. ASCII-only on purpose - a service name is never anything else,
    // and locale-aware folding here would be a way to be wrong in Turkish.
    const auto sameService = [](const std::string& a, const char* b) {
        std::size_t i = 0;
        for (; i < a.size() && b[i] != '\0'; ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return i == a.size() && b[i] == '\0';
    };
    std::vector<std::string> bound;
    for (const UsbNode& n : nodes) {
        if (sameService(n.service, "WinUSB")) { bound.push_back(n.deviceId); }
    }
    std::vector<UsbDeviceInfo> out;
    std::vector<std::string> seen;
    for (const UsbNode& n : nodes) {
        if (!n.isDeviceNode) { continue; }
        if (std::find(bound.begin(), bound.end(), n.deviceId) != bound.end()) { continue; }
        if (std::find(seen.begin(), seen.end(), n.deviceId) != seen.end()) { continue; }
        seen.push_back(n.deviceId);
        UsbDeviceInfo entry;
        entry.vid = n.vid;
        entry.pid = n.pid;
        entry.serial = n.serial;
        entry.description = n.description;
        out.push_back(std::move(entry));
    }
    // Stable order for the same reason enumerateWinUsb sorts: the sentence
    // the Source section builds from this must not reshuffle between frames.
    std::sort(out.begin(), out.end(), [](const UsbDeviceInfo& a, const UsbDeviceInfo& b) {
        if (a.serial != b.serial) { return a.serial < b.serial; }
        return a.description < b.description;
    });
    return out;
}

#if defined(_WIN32)

std::vector<UsbDeviceInfo> enumerateWinUsb(const std::vector<UsbId>& ids) {
    std::vector<UsbDeviceInfo> out;
    if (ids.empty()) { return out; }

    std::vector<std::wstring> wanted;
    wanted.reserve(ids.size());
    for (const UsbId& id : ids) { wanted.push_back(hardwareIdFragment(id.vid, id.pid)); }

    // Every PRESENT devnode, of every class. The alternative - one walk per
    // interface GUID - cannot work here, because the GUID a Zadig-bound
    // device registers is generated per install and appears in no header.
    const HDEVINFO set = ::SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
                                                DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { return out; }

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &info) != FALSE; ++i) {
        const std::wstring hw = deviceStringProperty(set, info, DEVPKEY_Device_HardwareIds);
        if (hw.empty()) { continue; }
        std::size_t matched = wanted.size();
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            if (hw.find(wanted[k]) != std::wstring::npos) {
                matched = k;
                break;
            }
        }
        if (matched == wanted.size()) { continue; }

        // BOUND TO WINUSB, or it cannot be opened and must not be listed
        // (the header's rule: an unbound device is not an entry here).
        const std::wstring service = deviceStringProperty(set, info, DEVPKEY_Device_Service);
        if (::_wcsicmp(service.c_str(), L"WinUSB") != 0) { continue; }

        const std::wstring instanceId = instanceIdOf(info.DevInst);
        if (instanceId.empty()) { continue; }

        // The path, from whatever GUID this node actually registered.
        std::string path;
        for (const GUID& g : declaredInterfaceGuids(set, info)) {
            path = interfacePathFor(g, instanceId);
            if (!path.empty()) { break; }
        }
        if (path.empty()) { path = interfacePathFor(kWinUsbInterfaceGuid, instanceId); }
        if (path.empty()) { path = interfacePathFor(GUID_DEVINTERFACE_USB_DEVICE, instanceId); }
        if (path.empty()) { continue; }

        UsbDeviceInfo entry;
        entry.vid = ids[matched].vid;
        entry.pid = ids[matched].pid;
        entry.path = std::move(path);

        // Serial and description from the USB DEVICE node (see the header
        // comment): on a composite dongle this devnode is the MI_xx child,
        // whose id is a bus path and whose description is "Bulk-In,
        // Interface".
        const DEVINST devNode = usbDeviceNodeOf(info.DevInst);
        entry.serial = lastInstanceSegment(instanceIdOf(devNode));
        if (devNode == info.DevInst) {
            entry.description =
                narrow(deviceStringProperty(set, info, DEVPKEY_Device_BusReportedDeviceDesc)
                           .c_str());
        } else {
            // A second, tiny SetupDi set for the parent - the properties of a
            // devnode we did not enumerate are not reachable through `info`.
            const std::wstring parentId = instanceIdOf(devNode);
            const HDEVINFO pset = ::SetupDiCreateDeviceInfoList(nullptr, nullptr);
            if (pset != INVALID_HANDLE_VALUE) {
                SP_DEVINFO_DATA pinfo{};
                pinfo.cbSize = sizeof(pinfo);
                if (::SetupDiOpenDeviceInfoW(pset, parentId.c_str(), nullptr, 0, &pinfo) != FALSE) {
                    entry.description = narrow(
                        deviceStringProperty(pset, pinfo, DEVPKEY_Device_BusReportedDeviceDesc)
                            .c_str());
                }
                ::SetupDiDestroyDeviceInfoList(pset);
            }
        }
        out.push_back(std::move(entry));
    }
    ::SetupDiDestroyDeviceInfoList(set);

    // Stable order, as the header promises, so "index=0" means the same
    // dongle from one run to the next.
    std::sort(out.begin(), out.end(),
              [](const UsbDeviceInfo& a, const UsbDeviceInfo& b) { return a.path < b.path; });
    return out;
}

std::vector<UsbDeviceInfo> enumerateUnbound(const std::vector<UsbId>& ids) {
    // The same walk enumerateWinUsb does, gathered rather than filtered: every
    // devnode carrying one of these hardware ids, with what each one is bound
    // to, handed to unboundFrom() to decide. Nothing is opened (rule 1).
    std::vector<UsbNode> nodes;
    if (ids.empty()) { return std::vector<UsbDeviceInfo>(); }

    std::vector<std::wstring> wanted;
    wanted.reserve(ids.size());
    for (const UsbId& id : ids) { wanted.push_back(hardwareIdFragment(id.vid, id.pid)); }

    const HDEVINFO set = ::SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
                                                DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { return std::vector<UsbDeviceInfo>(); }

    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &info) != FALSE; ++i) {
        const std::wstring hw = deviceStringProperty(set, info, DEVPKEY_Device_HardwareIds);
        if (hw.empty()) { continue; }
        std::size_t matched = wanted.size();
        for (std::size_t k = 0; k < wanted.size(); ++k) {
            if (hw.find(wanted[k]) != std::wstring::npos) {
                matched = k;
                break;
            }
        }
        if (matched == wanted.size()) { continue; }

        const std::wstring instanceId = instanceIdOf(info.DevInst);
        if (instanceId.empty()) { continue; }
        const DEVINST devNode = usbDeviceNodeOf(info.DevInst);
        const std::wstring deviceId = instanceIdOf(devNode);
        if (deviceId.empty()) { continue; }

        UsbNode node;
        node.vid = ids[matched].vid;
        node.pid = ids[matched].pid;
        node.deviceId = narrow(deviceId.c_str());
        node.isDeviceNode = (devNode == info.DevInst);
        node.service = narrow(deviceStringProperty(set, info, DEVPKEY_Device_Service).c_str());
        if (node.isDeviceNode) {
            node.serial = lastInstanceSegment(instanceId);
            node.description = narrow(
                deviceStringProperty(set, info, DEVPKEY_Device_BusReportedDeviceDesc).c_str());
        }
        nodes.push_back(std::move(node));
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return unboundFrom(nodes);
}

bool presentUsbIds(std::vector<UsbId>& out) {
    out.clear();
    // Every PRESENT devnode the USB bus driver enumerated, of every class -
    // the same walk as above, narrowed to the "USB" enumerator, reading one
    // property each. Nothing is opened (rule 1).
    const HDEVINFO set =
        ::SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { return false; }
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(set, i, &info) != FALSE; ++i) {
        const std::wstring hw = deviceStringProperty(set, info, DEVPKEY_Device_HardwareIds);
        UsbId id{0, 0};
        if (!usbIdFromHardwareId(narrow(hw.c_str()), id)) { continue; }
        bool seen = false;
        for (const UsbId& o : out) {
            if (o.vid == id.vid && o.pid == id.pid) { seen = true; }
        }
        if (!seen) { out.push_back(id); }
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return true;
}

std::unique_ptr<UsbDevice> openWinUsb(const std::string& path, std::string& error) {
    error.clear();
    const std::wstring wpath = widen(path);
    if (wpath.empty()) {
        error = "openWinUsb() needs a device interface path";
        return nullptr;
    }
    // EXCLUSIVE, deliberately - share mode 0, not the FILE_SHARE_READ |
    // FILE_SHARE_WRITE a libusb application uses. A radio is not a file: two
    // programs holding one dongle both program its registers and both pull
    // its bulk endpoint, and what each of them gets is the other's settings
    // and half of the other's samples. libusb's sharing is why "close the
    // other SDR program" is folklore advice; here the SECOND opener is told
    // in words that the radio is in use, which is a fact rather than a
    // symptom. FILE_FLAG_OVERLAPPED is not optional: WinUsb_Initialize
    // refuses a synchronous handle (measured: ERROR_INVALID_HANDLE).
    const HANDLE file = ::CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
            error = "the radio is already in use by another program";
        } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            error = "the radio is no longer present";
        } else {
            error = lastErrorText("opening the radio", err);
        }
        return nullptr;
    }
    WINUSB_INTERFACE_HANDLE winusb = nullptr;
    if (::WinUsb_Initialize(file, &winusb) == FALSE) {
        error = lastErrorText("WinUsb_Initialize (is the device bound to WinUSB?)",
                              ::GetLastError());
        ::CloseHandle(file);
        return nullptr;
    }
    return std::make_unique<WinUsbDevice>(realWinUsbApi(), file, winusb, path);
}

// --- the test seam (usb/winusb_device.hpp) ----------------------------------

const WinUsbApi& realWinUsbApi() {
    static const WinUsbApi api = {
        &::WinUsb_ControlTransfer, &::WinUsb_ReadPipe,       &::WinUsb_GetOverlappedResult,
        &::WinUsb_AbortPipe,       &::WinUsb_ResetPipe,      &::WinUsb_SetPipePolicy,
        &::WinUsb_SetPowerPolicy,  &::WinUsb_Free,           &::CancelIoEx,
        &::CloseHandle,
    };
    return api;
}

std::unique_ptr<UsbDevice> makeWinUsbDeviceForTest(const WinUsbApi& api, HANDLE file,
                                                   WINUSB_INTERFACE_HANDLE winusb,
                                                   std::string path) {
    return std::make_unique<WinUsbDevice>(api, file, winusb, std::move(path));
}

std::size_t liveUsbRequestsForTest() { return g_liveRequests.load(std::memory_order_relaxed); }

#elif defined(__linux__)  // !_WIN32 && __linux__

// Nothing here: src/usb/usbfs_device.cpp (compiled only on Linux - see its
// CMakeLists.txt guard) defines enumerateWinUsb(), enumerateUnbound() and
// openWinUsb() for this platform, straight onto usbfs. This file still
// supplies unboundFrom() above (portable, and exercised by
// tests/test_usb_winusb.cpp on every platform) and the real WinUSB
// implementation under _WIN32; it supplies nothing else here so that Linux
// gets exactly one definition of each function rather than two.

#else  // !_WIN32 && !__linux__

// The transport is WinUSB-or-usbfs by design (see usb_device.hpp and
// src/usb/usbfs_device.cpp): the native drivers exist because the
// alternative was somebody else's libusb. On a platform that is neither -
// macOS, *BSD - the tree still COMPILES, and says why, so the rest of the
// product builds and its tests run against the fake.
std::vector<UsbDeviceInfo> enumerateWinUsb(const std::vector<UsbId>&) {
    return std::vector<UsbDeviceInfo>();
}

std::vector<UsbDeviceInfo> enumerateUnbound(const std::vector<UsbId>&) {
    // Nothing is bound to WinUSB here, and nothing is unbound from it
    // either: the question does not arise. unboundFrom() above is still built
    // and still tested here, because the decision it makes is portable even
    // though the walk that feeds it is not.
    return std::vector<UsbDeviceInfo>();
}

std::unique_ptr<UsbDevice> openWinUsb(const std::string&, std::string& error) {
    error = "native USB radio support needs WinUSB (Windows) or usbfs (Linux) in this build";
    return nullptr;
}

bool presentUsbIds(std::vector<UsbId>& out) {
    // No listing on this platform: absence is unknown, not "nothing here".
    out.clear();
    return false;
}

#endif  // _WIN32 / __linux__ / other

}  // namespace cascade::usb
