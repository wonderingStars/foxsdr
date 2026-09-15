// usb_device.hpp - the transport every NATIVE radio driver talks through.
//
// WHY THIS EXISTS. Every crash FoxSDR has received from an RTL-SDR user in
// its first month lived inside libusb-1.0.dll, behind a SoapySDR vendor
// module loaded from somebody else's install: a lock freed and reused, a
// reader thread we did not create and could not guard, an enumeration probe
// that opened and reset the very dongle we were streaming from. None of that
// code is ours, so none of it could be fixed here. A native driver owns its
// USB traffic, its reader thread and its enumeration, and this header is the
// whole of what it may ask of Windows.
//
// THE CONTRACT, in three rules that the implementation and every driver keep:
//
//  1. ENUMERATION NEVER TOUCHES AN OPEN DEVICE. Devices are listed through
//     SetupAPI device-interface enumeration and device properties (bus
//     reported description, serial from the instance id). No handle is opened,
//     no control transfer is sent, to list a device. This is the rule the
//     vendor probe broke.
//  2. THE CALLER OWNS THE THREADS. The transport spawns nothing. Bulk
//     streaming is a ring of overlapped reads the caller pumps from its own
//     reader thread with readBulk(); control transfers are synchronous on
//     whichever thread calls them. A driver therefore knows every thread that
//     can be inside its device.
//  3. EVERY WAIT IS BOUNDED. Every call takes a timeout and honours it;
//     endBulkStream() cancels and waits for its own requests and returns
//     within a bounded time whatever the device does. A wedged device costs a
//     timeout, never a hung thread.
//
// Errors are returned, never thrown: a driver decides what an error means
// for the radio (retry, fault, dead) and says so through IqSource::faulted()
// and lastError(). The transport just reports what Windows said.
//
// The real implementation (src/usb/winusb_device.cpp) wraps WinUsb_* with
// FILE_FLAG_OVERLAPPED and RAW_IO on the bulk pipe. Tests implement this
// interface with a fake that scripts control answers and bulk payloads, so a
// driver can be proven byte-for-byte without a dongle.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cascade::usb {

// One device as SetupAPI reports it. `path` is the device interface path
// openWinUsb() takes; `serial` is the last instance-id segment, which for
// most SDR dongles is the USB serial string ("00000001" on a stock RTL-SDR).
struct UsbDeviceInfo {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::string path;
    std::string serial;
    std::string description;  // the bus-reported device description, may be empty
};

// Lists devices bound to WinUSB whose VID/PID appear in `ids`, in a stable
// order (by path). Never opens anything (rule 1). An unbound device (no
// driver, or the DVB-T driver) is NOT listed: it cannot be opened, and the
// Source section says what to do about that separately.
//
// ONE CORRECTION TO RULE 1 AS IT IS WORDED ABOVE, measured on the bench and
// true for both native drivers. The sketch says devices are found under
// GUID_DEVINTERFACE_USB_DEVICE; on a COMPOSITE dongle - which an RTL2832U
// is - that GUID finds only the usbccgp PARENT, which cannot be
// WinUsb_Initialize'd, while the WinUSB-bound interface child (MI_00)
// registers its path under whatever GUID its INF declared, and a
// Zadig/libwdi install generates that GUID per machine. So the walk matches
// DEVNODES by hardware id and service (WinUSB) and resolves each one's
// interface path from the GUIDs it actually registered. The rule itself is
// unchanged and kept in full: nothing is opened and no transfer is sent to
// produce this list. src/usb/winusb_device.cpp has the measurements.
//
// It also means `serial` and `description` come from the USB DEVICE node
// rather than the interface child: the child's instance id ends in a bus
// path ("7&38A180A5&0&0000") and its description is "Bulk-In, Interface",
// while its parent carries "00000001" and "RTL2838UHIDIR".
struct UsbId {
    std::uint16_t vid;
    std::uint16_t pid;
};
std::vector<UsbDeviceInfo> enumerateWinUsb(const std::vector<UsbId>& ids);

// THE DONGLES THAT ARE PLUGGED IN AND CANNOT BE OPENED, which is a different
// question from the one above and needs its own answer.
//
// enumerateWinUsb() lists what a native driver can reach. Everything else is
// invisible to it by design - and "invisible" is exactly what an RTL-SDR
// still running the DVB-T television driver looks like, which is how every
// dongle arrives. For several releases the whole of what such a user was told
// was that their receiver was not in the list: indistinguishable from the
// application not supporting their hardware, and shown to people whose only
// mistake was not having run Zadig yet.
//
// So this lists them separately: devices whose VID/PID is one of `ids`, which
// are PRESENT on the bus, and which have no WinUSB-bound interface. One entry
// per physical device, not per interface - a composite dongle has several
// devnodes carrying its hardware id and they are one radio.
//
// `path` IS ALWAYS EMPTY in these entries, and that is the point rather than
// an omission: there is no interface to open, so there is no path to give,
// and an entry from this list can never be mistaken for one that openWinUsb()
// would take. The Source section offers no row for them - an open that cannot
// succeed is not a choice - and says what to do instead.
//
// Rule 1 holds here too: nothing is opened and no transfer is sent.
std::vector<UsbDeviceInfo> enumerateUnbound(const std::vector<UsbId>& ids);

// --- the classification, separated so it can be proven ----------------------
//
// One devnode as the SetupAPI walk found it. Pulled out of enumerateUnbound()
// because the decision it makes - which of a machine's devnodes add up to "a
// dongle that is here and unreachable" - is the part that can be wrong, and
// it cannot be exercised on a bench where the one dongle present is correctly
// bound. unboundFrom() is pure, and tests/test_usb_winusb.cpp drives it with
// the shapes a real machine produces: a composite dongle whose MI_00 child is
// on WinUSB, one whose child is on nothing at all, a non-composite one on the
// DVB-T driver, and two dongles where only one is bound.
struct UsbNode {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    // The instance id of the USB DEVICE node this devnode belongs to - the
    // devnode itself when it is the device node, its parent when it is an
    // interface child. What makes "one entry per physical device" decidable.
    std::string deviceId;
    // True when this devnode IS that device node rather than a child of it.
    bool isDeviceNode = false;
    std::string service;  // the driver bound to THIS devnode; empty = none
    std::string serial;
    std::string description;
};

std::vector<UsbDeviceInfo> unboundFrom(const std::vector<UsbNode>& nodes);

// Standard request-type bits, so drivers do not spell 0x40 and 0xC0.
constexpr std::uint8_t kRequestTypeVendorOut = 0x40;  // host to device
constexpr std::uint8_t kRequestTypeVendorIn = 0xC0;   // device to host

class UsbDevice {
public:
    virtual ~UsbDevice() = default;

    // Control transfers. Return the byte count moved (0 is a valid answer for
    // an OUT with no data), or a negative value on failure, after which
    // lastError() names it. `timeoutMs` bounds the whole transfer. THE COUNT
    // IS THE ACTUAL COUNT, never the requested length: a HackRF answers its
    // version-string request with however many bytes the string has and no
    // terminator, so a short read reported as a full one reads as garbage.
    virtual int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                           std::uint16_t index, const std::uint8_t* data, std::size_t len,
                           unsigned timeoutMs) = 0;
    virtual int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                          std::uint16_t index, std::uint8_t* data, std::size_t len,
                          unsigned timeoutMs) = 0;

    // Bulk IN streaming (rule 2). beginBulkStream queues `bufferCount`
    // overlapped reads of `bufferBytes` each on `endpoint` (0x81 on an
    // RTL-SDR and a HackRF). readBulk copies the next completed buffer into
    // dst (cap >= bufferBytes) and re-queues it: returns the bytes copied,
    // 0 when nothing completed within timeoutMs, negative when the pipe has
    // failed (device gone, pipe error) - after which the driver must
    // endBulkStream() and decide. endBulkStream cancels every queued read,
    // waits for them to drain, frees the ring and returns (rule 3);
    // idempotent. ORDERING IS THE DRIVER'S OBLIGATION: no thread may be
    // inside readBulk when endBulkStream runs, because the ring it is
    // reading from is what endBulkStream frees. A driver joins its reader
    // first; a driver that could not join it within its bound leaks the
    // device instead of calling this.
    virtual bool beginBulkStream(std::uint8_t endpoint, std::size_t bufferBytes,
                                 std::size_t bufferCount) = 0;
    virtual int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) = 0;
    virtual void endBulkStream() = 0;
    virtual bool streaming() const = 0;

    // Clears a halted pipe (WinUsb_ResetPipe). Some firmware stalls the bulk
    // endpoint after a mode change; the driver knows when.
    virtual bool resetPipe(std::uint8_t endpoint) = 0;

    virtual const std::string& path() const = 0;
    virtual const std::string& lastError() const = 0;
};

// Opens a WinUSB device by the path enumerateWinUsb() gave. Null with
// `error` set on failure (in use by another process, unplugged since the
// listing, not bound to WinUSB after all).
std::unique_ptr<UsbDevice> openWinUsb(const std::string& path, std::string& error);

}  // namespace cascade::usb
