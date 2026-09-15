// THE LINUX TRANSPORT (src/usb/usbfs_device.cpp), proven without a radio -
// and, like tests/test_usb_winusb.cpp on Windows, without needing one to be
// a supported configuration.
//
// THERE IS NO SDR HARDWARE ON THIS MACHINE (a WSL2 build box) AND NO
// /sys/bus/usb/devices AT ALL - measured, not assumed: opendir() on it
// returns ENOENT here, which is exactly the "empty bus" case
// readSysfsUsbNodes() has to answer with an empty list rather than a crash,
// and this file's first block proves that against the REAL path before it
// ever touches a fixture. Everything past that runs against a fixture
// directory tree this file builds itself, which is what lets the parser -
// hex VID/PID, a missing serial, an interface child with none of the
// per-device files, an unreadable attribute - be proven byte-for-byte
// without a bus to plug anything into.
//
// WHAT THIS FILE DOES NOT PROVE, for the same reason of hardware absence:
// that USBDEVFS_DISCONNECT_CLAIM actually detaches dvb_usb_rtl28xxu on a real
// dongle, that interface 0 is the right claim for all four device families,
// or that a real bulk stream delivers real samples. See
// installer/linux/README.md's closing section.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "test_check.hpp"

#if defined(__linux__)

#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "usb/usb_device.hpp"
#include "usb/usb_fake.hpp"
#include "usb/usbfs_device.hpp"

using cascade::usb::FakeUsbDevice;
using cascade::usb::SysfsUsbNode;
using cascade::usb::UsbDeviceInfo;
using cascade::usb::UsbId;

namespace {

// A tiny fixture-directory builder: no <filesystem> dependency needed for
// three files and a couple of subdirectories, and mkdir/creat's own error
// codes are exactly what the parser has to be robust against.
void writeFile(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f != nullptr) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

void makeDir(const std::string& path) {
    const int rc = ::mkdir(path.c_str(), 0755);
    CHECK(rc == 0);
}

// How many descriptors this process currently has open, so a leak from a
// failed open() shows up as a number that goes up rather than as a guess.
int openFdCount() {
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) { return -1; }
    int n = 0;
    for (struct dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") { ++n; }
    }
    ::closedir(d);
    return n;
}

}  // namespace

int main() {
    // --- 1. Enumeration on THIS machine: no bus, no crash, no error --------
    {
        const std::vector<UsbId> rtlIds = {{0x0bda, 0x2832}, {0x0bda, 0x2838}};
        const std::vector<UsbDeviceInfo> found = cascade::usb::enumerateWinUsb(rtlIds);
        std::printf("enumerate on this machine (no /sys/bus/usb/devices here): %zu found\n",
                    found.size());
        CHECK(found.empty());  // empty is the SUPPORTED answer, not a failure

        // The raw walk against the real path directly, so a missing
        // directory is proven to come back empty rather than merely unused
        // by the caller above.
        const std::vector<SysfsUsbNode> real =
            cascade::usb::readSysfsUsbNodes("/sys/bus/usb/devices");
        CHECK(real.empty());

        // An empty id list asks about nothing and must not walk anything -
        // matches enumerateWinUsb's own contract on Windows.
        CHECK(cascade::usb::enumerateWinUsb(std::vector<UsbId>()).empty());
    }

    // --- 2. Opening a path that is not there: a clear message, no fd leak --
    {
        const int before = openFdCount();
        CHECK(before >= 0);
        for (int i = 0; i < 20; ++i) {
            std::string err = "untouched";
            auto dev = cascade::usb::openWinUsb("/dev/bus/usb/253/253", err);
            CHECK(dev == nullptr);
            CHECK(!err.empty());
            CHECK(err != "untouched");
        }
        const int after = openFdCount();
        std::printf("fds open before/after 20 failed opens: %d / %d\n", before, after);
        CHECK(before >= 0 && after >= 0 && after == before);

        // Empty path is refused before a single syscall, same as the
        // Windows transport's own rule (tests/test_usb_winusb.cpp).
        std::string err;
        CHECK(cascade::usb::openWinUsb("", err) == nullptr);
        CHECK(!err.empty());
    }

    // --- 3. A bounded read on a handle with no device behind it, measured --
    //
    // The fake stands in for "device-less": readBulk() has nothing queued,
    // which is the same shape a real dongle's idle pipe produces (see
    // test_usb_winusb.cpp's own hardware half). What matters here is that
    // the wait is bounded and the bound is honoured with room to spare, on
    // THIS transport's contract exactly as on Windows'.
    {
        FakeUsbDevice fake;
        CHECK(fake.beginBulkStream(0x81, 16384, 8));
        std::uint8_t dst[16384];
        const unsigned timeoutMs = 50;
        const auto t0 = std::chrono::steady_clock::now();
        const int r = fake.readBulk(dst, sizeof(dst), timeoutMs);
        const double gotMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::printf("bounded read on a device-less handle: %d after %.1f ms (bound %u ms)\n", r,
                    gotMs, timeoutMs);
        CHECK(r == 0);  // empty queue is a timeout, never an error
        CHECK(gotMs < 400.0);
        fake.endBulkStream();
    }

    // --- 4. THE SYSFS PARSER, against a fixture directory tree -------------
    {
        char tmpl[] = "/tmp/foxsdr_usbfs_test_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        CHECK(dir != nullptr);
        if (dir != nullptr) {
            const std::string root = dir;

            // A root hub: real ids, no serial, present but irrelevant to the
            // ids this test asks about below.
            makeDir(root + "/usb1");
            writeFile(root + "/usb1/idVendor", "1d6b\n");
            writeFile(root + "/usb1/idProduct", "0002\n");
            writeFile(root + "/usb1/busnum", "1\n");
            writeFile(root + "/usb1/devnum", "1\n");

            // A real device: every field present, including a serial.
            makeDir(root + "/1-2");
            writeFile(root + "/1-2/idVendor", "0bda\n");
            writeFile(root + "/1-2/idProduct", "2838\n");
            writeFile(root + "/1-2/serial", "00000001\n");
            writeFile(root + "/1-2/product", "RTL2838UHIDIR\n");
            writeFile(root + "/1-2/manufacturer", "Realtek\n");
            writeFile(root + "/1-2/bcdDevice", "0100\n");
            writeFile(root + "/1-2/busnum", "1\n");
            writeFile(root + "/1-2/devnum", "5\n");

            // Its interface child: NONE of the per-device files exist here -
            // this is the real shape libusb/udev produce, not a guess - so
            // hasIds/hasBusDev must both read back false, which is what lets
            // sysfsUsbNodesToDevices() tell it apart from a device with no
            // work beyond that.
            makeDir(root + "/1-2:1.0");
            writeFile(root + "/1-2:1.0/bInterfaceNumber", "00\n");

            // A second real device with NO serial file at all - most RTL-SDR
            // clones ship this way - and a manufacturer string only, so the
            // description fallback is exercised too.
            makeDir(root + "/1-3");
            writeFile(root + "/1-3/idVendor", "0bda\n");
            writeFile(root + "/1-3/idProduct", "2832\n");
            writeFile(root + "/1-3/manufacturer", "Generic\n");
            writeFile(root + "/1-3/busnum", "1\n");
            writeFile(root + "/1-3/devnum", "6\n");

            // A node whose idVendor cannot be READ as a plain file - the
            // closest thing to "permission denied" this fixture can force
            // while running as root (root bypasses a chmod 000, so that
            // cannot be used to prove EACCES-handling here): a directory
            // where a value should be still fails fopen() the same way an
            // access-denied node does - both come back as "could not read",
            // not as a parsed value.
            makeDir(root + "/1-4");
            makeDir(root + "/1-4/idVendor");
            writeFile(root + "/1-4/idProduct", "0002\n");
            writeFile(root + "/1-4/busnum", "1\n");
            writeFile(root + "/1-4/devnum", "7\n");

            // A node with a value that is present but not valid hex - must
            // be rejected, not half-parsed.
            makeDir(root + "/1-5");
            writeFile(root + "/1-5/idVendor", "zzzz\n");
            writeFile(root + "/1-5/idProduct", "2838\n");
            writeFile(root + "/1-5/busnum", "1\n");
            writeFile(root + "/1-5/devnum", "8\n");

            // A node that parses REAL, MATCHING ids (the same ones as 1-2)
            // but has no busnum/devnum - the shape a malformed or unusual
            // sysfs layout would have to produce for the hasBusDev half of
            // the device/interface-child filter to matter at all, since an
            // ordinary interface child never has idVendor/idProduct in the
            // first place. Proven load-bearing by deliberately removing that
            // filter and watching this node turn into a bogus THIRD device
            // at /dev/bus/usb/000/000 - see the commit message for the
            // failing line this produced.
            makeDir(root + "/1-2-ghost");
            writeFile(root + "/1-2-ghost/idVendor", "0bda\n");
            writeFile(root + "/1-2-ghost/idProduct", "2838\n");

            const std::vector<SysfsUsbNode> nodes = cascade::usb::readSysfsUsbNodes(root);
            std::printf("fixture walk: %zu nodes\n", nodes.size());
            CHECK(nodes.size() == 7);

            {
                const SysfsUsbNode* ghost = nullptr;
                for (const SysfsUsbNode& n : nodes) {
                    if (n.name == "1-2-ghost") { ghost = &n; }
                }
                CHECK(ghost != nullptr);
                if (ghost != nullptr) {
                    CHECK(ghost->hasIds);
                    CHECK(ghost->vid == 0x0bda && ghost->pid == 0x2838);
                    CHECK(!ghost->hasBusDev);  // no busnum/devnum files at all
                }
            }

            auto find = [&](const std::string& name) -> const SysfsUsbNode* {
                for (const SysfsUsbNode& n : nodes) {
                    if (n.name == name) { return &n; }
                }
                return nullptr;
            };

            const SysfsUsbNode* hub = find("usb1");
            CHECK(hub != nullptr);
            if (hub != nullptr) {
                CHECK(hub->hasIds);
                CHECK(hub->vid == 0x1d6b);
                CHECK(hub->pid == 0x0002);
                CHECK(hub->serial.empty());
                CHECK(hub->hasBusDev);
                CHECK(hub->busnum == 1 && hub->devnum == 1);
            }

            const SysfsUsbNode* dev12 = find("1-2");
            CHECK(dev12 != nullptr);
            if (dev12 != nullptr) {
                CHECK(dev12->hasIds);
                CHECK(dev12->vid == 0x0bda);
                CHECK(dev12->pid == 0x2838);
                CHECK(dev12->serial == "00000001");
                CHECK(dev12->product == "RTL2838UHIDIR");
                CHECK(dev12->manufacturer == "Realtek");
                CHECK(dev12->bcdDevice == 0x0100);
                CHECK(dev12->hasBusDev);
                CHECK(dev12->busnum == 1 && dev12->devnum == 5);
            }

            const SysfsUsbNode* child = find("1-2:1.0");
            CHECK(child != nullptr);
            if (child != nullptr) {
                CHECK(!child->hasIds);
                CHECK(!child->hasBusDev);
                CHECK(child->serial.empty());
            }

            const SysfsUsbNode* dev13 = find("1-3");
            CHECK(dev13 != nullptr);
            if (dev13 != nullptr) {
                CHECK(dev13->hasIds);
                CHECK(dev13->vid == 0x0bda);
                CHECK(dev13->pid == 0x2832);
                CHECK(dev13->serial.empty());  // no serial file - MUST be empty, not an error
                CHECK(dev13->manufacturer == "Generic");
                CHECK(dev13->hasBusDev);
                CHECK(dev13->busnum == 1 && dev13->devnum == 6);
            }

            const SysfsUsbNode* dev14 = find("1-4");
            CHECK(dev14 != nullptr);
            if (dev14 != nullptr) {
                // idVendor could not be opened as a file (it is a
                // directory): hasIds must be false, never a garbage value.
                CHECK(!dev14->hasIds);
            }

            const SysfsUsbNode* dev15 = find("1-5");
            CHECK(dev15 != nullptr);
            if (dev15 != nullptr) {
                // "zzzz" is not valid hex: hasIds must be false, not 0.
                CHECK(!dev15->hasIds);
            }

            // --- 5. THE DECISION: which nodes match, and how they become --
            //        UsbDeviceInfo. Pure, driven with the nodes just parsed.
            const std::vector<UsbId> rtlIds = {{0x0bda, 0x2832}, {0x0bda, 0x2838}};
            const std::vector<UsbDeviceInfo> devices =
                cascade::usb::sysfsUsbNodesToDevices(nodes, rtlIds);
            std::printf("fixture decision: %zu device(s) matched\n", devices.size());
            // Exactly 1-2 and 1-3: the hub doesn't match the id list, the
            // interface child has no ids to match, and 1-4/1-5 never parsed
            // ids in the first place.
            CHECK(devices.size() == 2);
            if (devices.size() == 2) {
                // Sorted by path: bus 001 dev 005 before bus 001 dev 006.
                CHECK(devices[0].path == "/dev/bus/usb/001/005");
                CHECK(devices[0].vid == 0x0bda && devices[0].pid == 0x2838);
                CHECK(devices[0].serial == "00000001");
                CHECK(devices[0].description == "RTL2838UHIDIR");
                CHECK(devices[1].path == "/dev/bus/usb/001/006");
                CHECK(devices[1].vid == 0x0bda && devices[1].pid == 0x2832);
                CHECK(devices[1].serial.empty());
                // No product string on 1-3: the description falls back to
                // the manufacturer rather than being left empty.
                CHECK(devices[1].description == "Generic");
            }

            // An empty id list matches nothing, whatever the walk found.
            CHECK(cascade::usb::sysfsUsbNodesToDevices(nodes, std::vector<UsbId>()).empty());

            // Clean up the fixture tree.
            for (const char* leaf :
                 {"/usb1/idVendor", "/usb1/idProduct", "/usb1/busnum", "/usb1/devnum"}) {
                ::unlink((root + leaf).c_str());
            }
            ::rmdir((root + "/usb1").c_str());
            for (const char* leaf : {"/1-2/idVendor", "/1-2/idProduct", "/1-2/serial",
                                     "/1-2/product", "/1-2/manufacturer", "/1-2/bcdDevice",
                                     "/1-2/busnum", "/1-2/devnum"}) {
                ::unlink((root + leaf).c_str());
            }
            ::rmdir((root + "/1-2").c_str());
            ::unlink((root + "/1-2:1.0/bInterfaceNumber").c_str());
            ::rmdir((root + "/1-2:1.0").c_str());
            for (const char* leaf :
                 {"/1-3/idVendor", "/1-3/idProduct", "/1-3/manufacturer", "/1-3/busnum",
                  "/1-3/devnum"}) {
                ::unlink((root + leaf).c_str());
            }
            ::rmdir((root + "/1-3").c_str());
            ::rmdir((root + "/1-4/idVendor").c_str());
            for (const char* leaf : {"/1-4/idProduct", "/1-4/busnum", "/1-4/devnum"}) {
                ::unlink((root + leaf).c_str());
            }
            ::rmdir((root + "/1-4").c_str());
            for (const char* leaf : {"/1-5/idVendor", "/1-5/idProduct", "/1-5/busnum",
                                     "/1-5/devnum"}) {
                ::unlink((root + leaf).c_str());
            }
            ::rmdir((root + "/1-5").c_str());
            ::unlink((root + "/1-2-ghost/idVendor").c_str());
            ::unlink((root + "/1-2-ghost/idProduct").c_str());
            ::rmdir((root + "/1-2-ghost").c_str());
            ::rmdir(root.c_str());
        }
    }

    // --- 6. A NON-DEVICE DESCRIPTION FALLBACK, pure, no filesystem ----------
    // A node with neither product nor manufacturer gets an empty
    // description rather than a placeholder - the driver layer (e.g.
    // rtlsdr_source.cpp's enumerateRtlSdr) is what supplies "RTL-SDR" when
    // this comes back empty, so the transport must not pre-empt that.
    {
        SysfsUsbNode n;
        n.name = "1-9";
        n.hasIds = true;
        n.vid = 0x1d50;
        n.pid = 0x6089;
        n.hasBusDev = true;
        n.busnum = 3;
        n.devnum = 12;
        const std::vector<UsbId> ids = {{0x1d50, 0x6089}};
        const std::vector<UsbDeviceInfo> out =
            cascade::usb::sysfsUsbNodesToDevices({n}, ids);
        CHECK(out.size() == 1);
        if (out.size() == 1) {
            CHECK(out[0].description.empty());
            CHECK(out[0].path == "/dev/bus/usb/003/012");
        }
    }

    return testSummary("test_usb_usbfs");
}

#else  // !__linux__

// This transport is Linux-only (src/usb/usbfs_device.cpp is compiled only
// there - see CMakeLists.txt). On every other platform this test has nothing
// to prove and says so rather than silently vanishing from the suite.
int main() {
    std::printf("test_usb_usbfs: SKIPPED (usbfs is Linux-only; see test_usb_winusb for the "
                "WinUSB transport's own tests)\n");
    return 0;
}

#endif  // __linux__
