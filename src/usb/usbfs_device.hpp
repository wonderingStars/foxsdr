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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#if defined(__linux__)

#include <cstdint>
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

}  // namespace cascade::usb

#endif  // __linux__
