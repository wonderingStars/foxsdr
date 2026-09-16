// usb_android_bridge.h - the ONLY thing the Android JNI layer (another
// slice) needs to know about to hand this app's native USB transport a
// device android.hardware.usb.UsbManager has already opened and
// permission-granted.
//
// WHY A SEPARATE, PLAIN-C HEADER rather than exposing
// cascade::usb::registerAdoptedDevice() directly to the JNI translation
// unit. A JNI .cpp file is already crossing one ABI boundary (Java <-> C);
// crossing a second one (this app's own C++ types: std::string,
// cascade::usb::UsbDeviceInfo, the exact STL/ABI this binary's NDK toolchain
// and STL choice produced them with) at the same seam is how a perfectly
// good build breaks the day either side changes compiler, NDK version, or
// STL (libc++ vs the now-removed libstdc++) without the other side
// rebuilding in lockstep. A `extern "C"` function taking only `int`,
// `uint16_t` and `const char*` has none of that: it is exactly as portable
// as any other JNI native method, and its implementation
// (usb_android_bridge.cpp) is the only file that has to know both C++ types
// exist.
//
// THE WHOLE CONTRACT, in the order a JNI layer uses it:
//   1. Android's system USB-permission dialog grants the app access to a
//      UsbDevice; Java opens it (UsbManager.openDevice()) and gets a
//      UsbDeviceConnection.
//   2. JNI calls foxsdr_usb_register() once, handing over
//      UsbDeviceConnection.getFileDescriptor(), UsbDevice.getVendorId()/
//      getProductId(), and (if available - see the parameter comment below)
//      the device's serial and product strings.
//   3. From that point on, every native driver's ordinary
//      cascade::usb::enumerateWinUsb()/openWinUsb() pair (src/usb/usb_device.hpp)
//      sees this device exactly as it would see one found by the sysfs walk
//      on desktop Linux - a driver written before Android existed needs no
//      Android-specific code path at all.
//   4. When Java's UsbDeviceConnection is closed (device detached,
//      permission revoked, activity destroyed), JNI calls
//      foxsdr_usb_unregister() with the path foxsdr_usb_register() returned,
//      so a later enumerateWinUsb() stops offering a device whose fd Java is
//      about to close.
//
// This header registers a device; it does not open one. Opening happens
// later, from whichever native driver's own open() calls
// cascade::usb::openWinUsb() with the path this file's registration
// produced - the same call every driver already makes for a sysfs-found
// device, unchanged.
//
// OWNERSHIP: registering a fd here never closes it, and neither does
// unregistering. The fd is duplicated (dup()) at the point a driver actually
// opens the device, not at registration time - see
// src/usb/usbfs_device.hpp's comment on adoptUsbFd() for why. Java's
// UsbDeviceConnection therefore owns this fd for exactly as long as it
// already intended to.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_USB_ANDROID_BRIDGE_H
#define CASCADE_USB_ANDROID_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers an already-open, already permission-granted usbfs file
// descriptor with the native USB transport, so
// cascade::usb::enumerateWinUsb() and cascade::usb::openWinUsb() (the same
// pair every native RTL-SDR/HackRF/Airspy/RX888/Mirics driver already calls)
// can see and open it.
//
//   fd            - android.hardware.usb.UsbDeviceConnection.getFileDescriptor().
//                   Not duplicated or closed by this call; the caller (Java,
//                   through its UsbDeviceConnection) keeps owning it for as
//                   long as it already intended to.
//   vid, pid      - UsbDevice.getVendorId() / getProductId().
//   serial_utf8   - UsbDevice.getSerialNumber(), UTF-8, NUL-terminated - or
//                   NULL/empty if unavailable (that call needs
//                   android.permission.MANAGE_USB_MANAGER; a device without
//                   a serial is opened by index the same way an RTL-SDR
//                   clone with no serial EEPROM already is on desktop
//                   Linux/Windows).
//   product_utf8  - UsbDevice.getProductName(), UTF-8, NUL-terminated - or
//                   NULL/empty; shown to the user the same way a sysfs
//                   device's empty `product` file already falls back to the
//                   manufacturer string or a driver-supplied name.
//
// Returns a newly heap-allocated, NUL-terminated path string identifying
// this registration ("fd:<fd>") for a later foxsdr_usb_unregister() call, or
// NULL if `fd` is negative. The caller owns the returned string and must
// free it with foxsdr_usb_free_string() - never with Java-side or libc
// free() directly, in case a future build's allocator for this library
// differs from the JNI translation unit's own.
char* foxsdr_usb_register(int fd, uint16_t vid, uint16_t pid, const char* serial_utf8,
                          const char* product_utf8);

// Removes a registration by the path foxsdr_usb_register() returned. Safe to
// call with a path that names nothing currently registered (a no-op, not an
// error) - see usbfs_device.hpp's unregisterAdoptedDevice() for why a JNI
// teardown path cannot always know whether this already ran. Never closes
// the underlying fd.
void foxsdr_usb_unregister(const char* path);

// Frees a string returned by foxsdr_usb_register(). NULL is accepted and
// ignored.
void foxsdr_usb_free_string(char* s);

// How many devices are currently registered - a JNI-visible sanity check
// (log it after register/unregister calls during bring-up) with no need to
// round-trip through enumerateWinUsb()'s id-filtered answer to get a count.
int foxsdr_usb_list_count(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CASCADE_USB_ANDROID_BRIDGE_H
