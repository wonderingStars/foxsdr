// usbfs_device.hpp - the Linux-only seam into src/usb/usbfs_device.cpp's sysfs
// walk, so a test can drive the PARSER against a fixture directory tree
// without a real bus and without touching the cross-platform contract in
// usb_device.hpp.
//
// Split the same way winusb_device.cpp splits unboundFrom() from the SetupAPI
// walk that feeds it: readSysfsUsbNodes() is the WALK (real directory reads,
// real hex parsing, exercised against a fixture tree by
// tests/test_usb_usbfs.cpp) and sysfsUsbNodesToDevices() is the DECISION
// (which nodes are devices worth matching, and how each becomes a
// UsbDeviceInfo) - pure, so it is tested with in-memory nodes and no
// filesystem at all.
//
// This header is deliberately NOT part of usb_device.hpp: nothing outside
// usbfs_device.cpp and its test includes it, so the cross-platform transport
// contract that every driver and both platforms' tests build against stays
// untouched by this file's existence.
//
// ANDROID ADDENDUM (the adopted-device registry below). On Android there is
// no sysfs walk and no device-node permission to have: UsbManager has
// already found the device, a person has already granted it permission
// through the system's own USB-attach dialog, and
// UsbDeviceConnection.getFileDescriptor() hands Java an already-open usbfs
// fd for it. So the Android build needs a second way into the same
// enumerateWinUsb()/openWinUsb() pair every driver already calls, that
// bypasses the sysfs walk entirely and adopts that fd instead. That is
// registerAdoptedDevice()/unregisterAdoptedDevice()/adoptUsbFd() below, and
// the extern "C" wrapper a JNI translation unit calls is
// src/usb/usb_android_bridge.h.
//
// This is declared here rather than in usb_device.hpp for the same reason
// the sysfs split is: it is Linux(/Android)-only machinery that no driver
// calls directly (drivers only ever see enumerateWinUsb()/openWinUsb()), so
// it has no business in the cross-platform contract every platform and
// every driver builds against. It IS declared for every __linux__ build,
// including desktop Linux, rather than only under a hypothetical
// __ANDROID__ guard - see the comment on registerAdoptedDevice() for why
// that is safe.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if defined(__linux__)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::usb {

// One /sys/bus/usb/devices/<name> node, read verbatim. `name` is the
// directory's own name ("1-2", "usb1", or an interface child "1-2:1.0") -
// kept so the decision function can tell a device node from an interface
// child without re-deriving it from a path.
struct SysfsUsbNode {
    std::string name;
    bool hasIds = false;      // idVendor/idProduct both parsed
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    std::string serial;        // absent file -> empty, not an error
    std::string product;
    std::string manufacturer;
    std::uint16_t bcdDevice = 0;
    bool hasBusDev = false;    // busnum/devnum both parsed
    unsigned busnum = 0;
    unsigned devnum = 0;
};

// The walk: every entry directly under `sysfsDevicesDir` (ordinarily
// "/sys/bus/usb/devices", a fixture directory in tests), read without
// opening a single device node. An interface child ("1-2:1.0") is walked
// like any other entry - hasIds/hasBusDev simply come back false for one,
// since neither file exists in an interface directory - and the decision
// function is what tells them apart from a real device.
std::vector<SysfsUsbNode> readSysfsUsbNodes(const std::string& sysfsDevicesDir);

// The decision: keep nodes whose VID/PID is in `ids` and which carry a
// bus/dev pair (an interface child never does), build the /dev/bus/usb path
// from it, and sort by that path for the same stable-order reason
// enumerateWinUsb() sorts on Windows.
std::vector<UsbDeviceInfo> sysfsUsbNodesToDevices(const std::vector<SysfsUsbNode>& nodes,
                                                   const std::vector<UsbId>& ids);

// ---------------------------------------------------------------------------
// THE ADOPTED-DEVICE REGISTRY, for Android.
//
// enumerateWinUsb() on desktop Linux answers entirely from the sysfs walk
// above. On Android there is no sysfs walk to answer from - the JNI layer
// (another slice) calls registerAdoptedDevice() once per UsbDevice the user
// has granted permission for, and from that point on enumerateWinUsb(ids)
// must include it (matched by vid/pid, same as every sysfs entry) and
// openWinUsb() must be able to open it by the path registerAdoptedDevice()
// assigned it.
//
// COMPILED ON EVERY __linux__ TARGET, DESKTOP INCLUDED, ON PURPOSE: nothing
// besides an explicit registerAdoptedDevice() call ever puts anything in
// here, so on desktop Linux - where nothing ever calls it - the registry
// stays empty for the life of the process and enumerateWinUsb()'s sysfs-only
// answer is bit-for-bit what it always was. Compiling it everywhere rather
// than behind a second __ANDROID__ guard is what lets
// tests/test_usb_usbfs.cpp drive the registry directly on this WSL build
// box, with no Android device or emulator anywhere near it, which is the
// only way this half of the feature could be proven at all in this
// environment. See the header's ANDROID ADDENDUM comment above.
// ---------------------------------------------------------------------------

// Registers an already-open, already permission-granted usbfs fd - as
// android.hardware.usb.UsbDeviceConnection.getFileDescriptor() returns it -
// so enumerateWinUsb(ids) reports it (once `info.vid`/`info.pid` is in
// `ids`) and openWinUsb() can open it. `info.path` is IGNORED and
// overwritten with "fd:<fd>", which is the only path enumerateWinUsb() will
// ever hand back for this entry and the only one openWinUsb() will accept to
// reach it - so a caller need not (and cannot usefully) set it.
//
// Registering the same `fd` value again REPLACES the earlier entry rather
// than adding a second one: a JNI layer that re-registers after Java
// re-opens the same device (a permission re-grant, a replug that happens to
// reuse the fd number) must not leave a stale entry for a fd it no longer
// owns.
//
// Does not open, dup, or otherwise touch `fd` - it is only ever acted on
// later, from openWinUsb()/adoptUsbFd(), and only ever by duplicating it
// (see adoptUsbFd()'s own comment for why). Thread-safe: a JNI callback can
// register from whatever thread Android calls it on while a driver's own
// thread is mid-enumerateWinUsb().
void registerAdoptedDevice(UsbDeviceInfo info, int fd);

// Removes a registration by the path registerAdoptedDevice() assigned it
// ("fd:<n>" - from the UsbDeviceInfo enumerateWinUsb() returned, or from
// foxsdr_usb_register()'s return value in usb_android_bridge.h). Never
// closes `fd` itself - Java's UsbDeviceConnection owns that closing, whether
// through its own close() or the user revoking permission. Unregistering a
// path nothing is registered under is a no-op, not an error: the JNI layer
// unregisters on every teardown path (device detach, activity destroy,
// permission revoked) and cannot always know which of those already ran.
void unregisterAdoptedDevice(const std::string& path);

// How many devices are currently registered - a JNI-visible sanity check
// (foxsdr_usb_list_count() in usb_android_bridge.h) and what
// tests/test_usb_usbfs.cpp asserts against directly rather than re-deriving
// the count from enumerateWinUsb()'s own (id-filtered) answer.
std::size_t adoptedDeviceCount();

// Adopts an already-open usbfs fd into the same UsbfsDevice class the sysfs
// path above returns, so a native driver cannot tell the difference once it
// has one: same control/bulk/reset behaviour, same bounded waits, same
// destructor. Called from openWinUsb() when it is given a path
// registerAdoptedDevice() produced; exposed here directly too, since that is
// the only way tests on a machine with no usbfs device at all
// (/sys/bus/usb/devices does not exist on this WSL2 build box) can prove its
// two failure-independent behaviours: the fd-ownership decision below, and
// the "not a usbfs fd" message, without going through the registry at all.
//
// THE FD-OWNERSHIP DECISION, and why: this function DUPLICATES `fd` rather
// than taking ownership of the value the caller passed in. The Java side
// keeps its own UsbDeviceConnection - and therefore its own claim on that
// fd number - open for as long as the app holds the device, quite
// independently of how long any one native UsbDevice built from it lives
// (a driver can close and reopen a source without Java ever closing its
// connection); closing the caller's fd out from under it on
// ~UsbfsDevice() would be a use-after-close the moment Java next touched its
// own connection. A dup costs one fd for the lifetime of the native
// UsbfsDevice and is closed exactly where every other fd this class owns
// is closed: its destructor. `fd` itself is never closed, never
// fcntl'd, never touched beyond being handed to dup() - not even on
// failure below.
//
// `vid`/`pid`/`serial` are NOT stored on the returned UsbfsDevice (path() is
// still just "fd:<fd>", exactly as the sysfs path only ever carried a bus
// path) - the identity a driver's logs and its device-picking logic need is
// already in the UsbDeviceInfo enumerateWinUsb() gave it before open() was
// ever called, the same as on the sysfs path. They are accepted here purely
// so a claim failure's log line can name the device it failed for, since
// "fd 37 would not claim" means nothing next to a Play-Store user's bug
// report six months from now and "0bda:2838 (fd 37) would not claim" does.
//
// Claims interface 0 (kSdrInterface, same constant and same assumption the
// sysfs path's DISCONNECT_CLAIM uses) with the plain USBDEVFS_CLAIMINTERFACE
// ioctl - never USBDEVFS_DISCONNECT_CLAIM, which this path deliberately does
// not use: Android runs no kernel driver against an SDR's vendor interface
// for DISCONNECT_CLAIM to detach in the first place (there is no
// dvb_usb_rtl28xxu on a phone), and the one thing that MAY already hold the
// claim is the Java side, whose grip DISCONNECT_CLAIM would rip away instead
// of cooperating with. An EBUSY from that ioctl is therefore read as exactly
// that - Java's own UsbDeviceConnection.claimInterface() got there first -
// logged, and treated as success: a claim already held by the same process
// is not a fault, and usbfs control/bulk transfers on this fd work
// regardless of which side of the JNI boundary issued the claim. Any other
// failure is reported through `error` and the function returns null; in
// particular ENOTTY - the ioctl a non-usbfs fd (a pipe, /dev/null) answers
// with "no such ioctl on this file" - is reported as the fd not being a
// usbfs device at all, which is the only failure this function's own test
// can produce without real hardware (see tests/test_usb_usbfs.cpp).
std::unique_ptr<UsbDevice> adoptUsbFd(int fd, std::uint16_t vid, std::uint16_t pid,
                                      const std::string& serial, std::string& error);

}  // namespace cascade::usb

#endif  // __linux__
