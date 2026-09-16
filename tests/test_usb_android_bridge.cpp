// THE PLAIN-C BRIDGE THE ANDROID JAVA LAYER CALLS (src/usb/usb_android_bridge.h),
// proven on a build box with no Android and no radio.
//
// WHY THIS IS A SEPARATE TEST FROM tests/test_usb_usbfs.cpp. That file drives
// the C++ registry directly - registerAdoptedDevice(), unregisterAdoptedDevice(),
// adoptedDeviceCount() - which is the layer BELOW this one. What this file
// pins is the extern "C" surface the JNI translation unit actually calls, and
// the four things about it a Java caller depends on and cannot see:
//
//   1. the RETURNED PATH is the one enumerateWinUsb() reports and
//      openWinUsb() accepts, so Java can hand it straight back to
//      foxsdr_usb_unregister() and a driver can open the device by it;
//   2. a NEGATIVE fd is refused with NULL rather than registering a device
//      nothing can ever open (UsbManager.openDevice() returning null, then
//      getFileDescriptor() on nothing, is how Java produces one);
//   3. NULL serial/product strings are accepted - getSerialNumber() needs a
//      permission this app does not hold, so it is very often null, and an
//      RTL-SDR clone with no serial EEPROM has none to report either;
//   4. registering the same fd TWICE replaces rather than duplicates, which
//      is what a permission re-grant or a replug that reuses an fd number
//      does, and what would otherwise leave a stale entry the Source section
//      would offer the user twice.
//
// THE SEQUENCE MATTERS AS MUCH AS THE PIECES, so the checks below run in the
// order Java runs them: register on permission granted, enumerate (the Source
// section's own scan), open, unregister on detach, enumerate again.
//
// NOT PROVEN HERE, and nothing on this machine can prove it: that a real
// UsbDeviceConnection's fd streams samples. openWinUsb() on a socketpair fd
// gets as far as the claim ioctl and is told ENOTTY, which is exactly what
// "this is not a usbfs fd" looks like - the deepest a host test can reach.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "test_check.hpp"

#if defined(__linux__)

#include <sys/socket.h>
#include <unistd.h>

#include "usb/usb_android_bridge.h"
#include "usb/usb_device.hpp"
#include "usb/usbfs_device.hpp"

using cascade::usb::UsbDeviceInfo;
using cascade::usb::UsbId;

namespace {

// The commonest RTL-SDR in the world, and the pair the on-device self-test
// registers too (see android/app/src/main/java/com/foxsdr/app/Usb.java).
constexpr std::uint16_t kRtlVid = 0x0bda;
constexpr std::uint16_t kRtlPid = 0x2838;

const UsbDeviceInfo* findPath(const std::vector<UsbDeviceInfo>& devices,
                              const std::string& path) {
    for (const UsbDeviceInfo& d : devices) {
        if (d.path == path) { return &d; }
    }
    return nullptr;
}

}  // namespace

int main() {
    const std::vector<UsbId> ids = {{kRtlVid, kRtlPid}};

    // A file descriptor that is open and is NOT a usbfs device: the same
    // stand-in tests/test_usb_usbfs.cpp uses, because what the registry does
    // with an fd is keep the number until something opens it.
    int sv[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    if (sv[0] < 0) { return testSummary("test_usb_android_bridge"); }

    const int before = foxsdr_usb_list_count();
    CHECK(before >= 0);
    CHECK(before == static_cast<int>(cascade::usb::adoptedDeviceCount()));

    // 1. A NEGATIVE FD IS REFUSED, and nothing is registered for it.
    {
        char* nothing = foxsdr_usb_register(-1, kRtlVid, kRtlPid, "SN", "product");
        CHECK(nothing == nullptr);
        CHECK(foxsdr_usb_list_count() == before);
        foxsdr_usb_free_string(nothing);  // NULL is accepted and ignored
    }

    // 2. THE REGISTRATION JAVA MAKES ON PERMISSION GRANTED, and the three
    //    things it hands back to the rest of the program.
    char* path = foxsdr_usb_register(sv[0], kRtlVid, kRtlPid, "FOXSDR-TEST-0001",
                                     "RTL2838UHIDIR");
    CHECK(path != nullptr);
    if (path == nullptr) {
        ::close(sv[0]);
        ::close(sv[1]);
        return testSummary("test_usb_android_bridge");
    }
    const std::string registered(path);
    CHECK(registered == "fd:" + std::to_string(sv[0]));
    CHECK(foxsdr_usb_list_count() == before + 1);
    CHECK(foxsdr_usb_list_count() == static_cast<int>(cascade::usb::adoptedDeviceCount()));

    // ...and the Source section's own scan sees it. Asserted BY PATH, never
    // by list size: the Android emulator this also runs on exposes a real
    // /sys/bus/usb/devices, so the answer is "at least ours", not "only ours".
    {
        const std::vector<UsbDeviceInfo> found = cascade::usb::enumerateWinUsb(ids);
        const UsbDeviceInfo* mine = findPath(found, registered);
        CHECK(mine != nullptr);
        if (mine != nullptr) {
            CHECK(mine->vid == kRtlVid);
            CHECK(mine->pid == kRtlPid);
            CHECK(mine->serial == "FOXSDR-TEST-0001");
            CHECK(mine->description == "RTL2838UHIDIR");
        }
        // An id list that does not name this device must NOT report it: the
        // registry is filtered exactly like the sysfs walk, which is what
        // stops a HackRF row appearing in the RTL-SDR list.
        const std::vector<UsbDeviceInfo> others =
            cascade::usb::enumerateWinUsb(std::vector<UsbId>{{0x1d50, 0x6089}});
        CHECK(findPath(others, registered) == nullptr);
    }

    // 3. THE PATH OPENS THROUGH THE ORDINARY DRIVER ENTRY POINT. It cannot
    //    succeed - a socketpair is not a usbfs device - but WHICH failure it
    //    is proves the routing: "not a usbfs device" comes from adoptUsbFd()
    //    after the registry lookup found the entry and dup()ed the fd.
    {
        std::string err = "untouched";
        CHECK(cascade::usb::openWinUsb(registered, err) == nullptr);
        CHECK(err.find("is not a usbfs device") != std::string::npos);
        if (err.find("is not a usbfs device") == std::string::npos) {
            std::printf("  openWinUsb said: %s\n", err.c_str());
        }
        // The caller's own fd is untouched by that attempt - adoptUsbFd
        // dup()s and closes only its own copy. Still open, still a socket.
        char probe = 'x';
        CHECK(::write(sv[0], &probe, 1) == 1);
        CHECK(::read(sv[1], &probe, 1) == 1);
    }

    // 4. NULL STRINGS, which is what UsbDevice.getSerialNumber() and
    //    getProductName() return more often than not.
    {
        char* second = foxsdr_usb_register(sv[1], kRtlVid, kRtlPid, nullptr, nullptr);
        CHECK(second != nullptr);
        if (second != nullptr) {
            const std::string secondPath(second);
            CHECK(secondPath == "fd:" + std::to_string(sv[1]));
            CHECK(foxsdr_usb_list_count() == before + 2);
            const std::vector<UsbDeviceInfo> found = cascade::usb::enumerateWinUsb(ids);
            const UsbDeviceInfo* mine = findPath(found, secondPath);
            CHECK(mine != nullptr);
            if (mine != nullptr) {
                CHECK(mine->serial.empty());
                CHECK(mine->description.empty());
            }
            foxsdr_usb_unregister(secondPath.c_str());
            CHECK(foxsdr_usb_list_count() == before + 1);
            foxsdr_usb_free_string(second);
        }
    }

    // 5. RE-REGISTERING THE SAME FD REPLACES THE ENTRY. A permission
    //    re-grant, or a replug that reuses the fd number, must not leave two
    //    rows for one radio - and the ids that arrive second are the ones
    //    that count, because they are the ones Java just read off the device.
    {
        char* again = foxsdr_usb_register(sv[0], 0x1d50, 0x6089, "HACKRF", "HackRF One");
        CHECK(again != nullptr);
        if (again != nullptr) {
            CHECK(std::string(again) == registered);
            CHECK(foxsdr_usb_list_count() == before + 1);
            const std::vector<UsbDeviceInfo> rtl = cascade::usb::enumerateWinUsb(ids);
            CHECK(findPath(rtl, registered) == nullptr);  // no longer an RTL-SDR
            const std::vector<UsbDeviceInfo> hack =
                cascade::usb::enumerateWinUsb(std::vector<UsbId>{{0x1d50, 0x6089}});
            const UsbDeviceInfo* mine = findPath(hack, registered);
            CHECK(mine != nullptr);
            if (mine != nullptr) { CHECK(mine->serial == "HACKRF"); }
            foxsdr_usb_free_string(again);
        }
    }

    // 6. UNREGISTERING WHAT IS NOT THERE IS A NO-OP, not an error. The JNI
    //    layer unregisters on detach, on permission revoked and on activity
    //    destroy, and cannot always know which of those already ran.
    {
        foxsdr_usb_unregister(nullptr);
        foxsdr_usb_unregister("");
        foxsdr_usb_unregister("fd:999999");
        foxsdr_usb_unregister("/dev/bus/usb/001/002");
        CHECK(foxsdr_usb_list_count() == before + 1);
    }

    // 7. THE DETACH PATH: gone from the count, gone from the enumeration, and
    //    openWinUsb() now says so in the words a log reader needs.
    {
        foxsdr_usb_unregister(registered.c_str());
        CHECK(foxsdr_usb_list_count() == before);
        CHECK(foxsdr_usb_list_count() == static_cast<int>(cascade::usb::adoptedDeviceCount()));
        const std::vector<UsbDeviceInfo> hack =
            cascade::usb::enumerateWinUsb(std::vector<UsbId>{{0x1d50, 0x6089}});
        CHECK(findPath(hack, registered) == nullptr);
        std::string err = "untouched";
        CHECK(cascade::usb::openWinUsb(registered, err) == nullptr);
        CHECK(err.find("no adopted USB device is registered") != std::string::npos);
    }

    foxsdr_usb_free_string(path);
    ::close(sv[0]);
    ::close(sv[1]);

    return testSummary("test_usb_android_bridge");
}

#else  // !__linux__

// The bridge is compiled only on Linux and Android (src/usb/usb_android_bridge.cpp
// #errors elsewhere and CMakeLists.txt excludes it there), so on Windows and
// macOS this test has nothing to link against and says so rather than
// silently vanishing from the suite - the same shape as test_usb_usbfs.cpp's
// own non-Linux branch.
int main() {
    std::printf("test_usb_android_bridge: SKIPPED (the Android USB bridge is Linux-only)\n");
    return 0;
}

#endif  // __linux__
