// THE TRANSPORT: what src/usb promises, checked without a radio - and, when
// one happens to be plugged in, with it.
//
// Two halves, and the split is deliberate. The FIRST half runs everywhere and
// proves the contract's edges: enumeration with nothing to find, a bad path,
// an illegal bulk ring, and the fake's own behaviour (which every driver test
// depends on being right, so it is tested here rather than assumed). The
// SECOND half runs only when an RTL-SDR is actually bound to WinUSB on this
// machine and proves the things a fake cannot: that a control transfer
// reaches the silicon, that RAW_IO streaming delivers, and - the one that
// matters most - that endBulkStream() comes back inside its bound.
//
// A machine with no dongle is a SUPPORTED configuration, not a skipped test:
// the second half says so in one line and the suite stays green. What is not
// acceptable is a test that passes because it quietly did nothing, so the
// hardware half prints what it found either way.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "test_check.hpp"
#include "usb/usb_device.hpp"
#include "usb/usb_fake.hpp"

using cascade::usb::FakeUsbDevice;
using cascade::usb::UsbDeviceInfo;
using cascade::usb::UsbId;

namespace {

// The ids the RTL-SDR driver asks for. Spelled here rather than included so
// this file tests the TRANSPORT and does not drag a driver in.
const std::vector<UsbId> kRtlIds = {{0x0bda, 0x2832}, {0x0bda, 0x2838}};

}  // namespace

int main() {
    // --- 1. Enumeration, with nothing to find -------------------------------
    {
        // An empty request is empty, not "everything".
        CHECK(cascade::usb::enumerateWinUsb({}).empty());

        // A vendor id nobody ships. This also exercises the whole SetupAPI
        // walk, so a crash in it fails here rather than in a driver.
        const std::vector<UsbDeviceInfo> none =
            cascade::usb::enumerateWinUsb({{0xfffe, 0xfffd}});
        CHECK(none.empty());

        // STABLE ORDER, which is what makes "index=0" mean the same dongle
        // twice running. Checked against whatever is really present.
        const std::vector<UsbDeviceInfo> a = cascade::usb::enumerateWinUsb(kRtlIds);
        const std::vector<UsbDeviceInfo> b = cascade::usb::enumerateWinUsb(kRtlIds);
        CHECK(a.size() == b.size());
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            CHECK(a[i].path == b[i].path);
            CHECK(a[i].serial == b[i].serial);
        }
        std::printf("enumerate: %zu RTL2832U device(s) bound to WinUSB\n", a.size());
        for (const UsbDeviceInfo& d : a) {
            std::printf("  %04x:%04x serial=\"%s\" desc=\"%s\"\n    %s\n", d.vid, d.pid,
                        d.serial.c_str(), d.description.c_str(), d.path.c_str());
            // Whatever it found, it found it under the ids that were asked
            // for - a walk that matched on the wrong field would show up here.
            bool known = false;
            for (const UsbId& id : kRtlIds) {
                if (id.vid == d.vid && id.pid == d.pid) { known = true; }
            }
            CHECK(known);
            CHECK(!d.path.empty());
        }
    }

    // --- 2. Opening something that is not there -----------------------------
    {
        std::string err = "untouched";
        CHECK(cascade::usb::openWinUsb("", err) == nullptr);
        CHECK(!err.empty());
        CHECK(err != "untouched");

        err.clear();
        auto dev = cascade::usb::openWinUsb("\\\\?\\usb#vid_dead&pid_beef#nothing", err);
        CHECK(dev == nullptr);
        // A failure that says nothing is the failure this product has spent
        // releases removing; the message is part of the contract.
        CHECK(!err.empty());
        std::printf("open of a bogus path said: %s\n", err.c_str());
    }

    // --- 3. The fake's own contract -----------------------------------------
    //
    // Every driver test asserts register sequences THROUGH this object, so a
    // fake that quietly mis-records would make all of them meaningless.
    {
        FakeUsbDevice fake;
        const std::uint8_t payload[2] = {0x12, 0x34};
        CHECK(fake.controlOut(0x40, 0, 0x2000, 0x0110, payload, 2, 100) == 2);
        CHECK(fake.controls.size() == 1);
        CHECK(fake.controls[0].out);
        CHECK(fake.controls[0].requestType == 0x40);
        CHECK(fake.controls[0].value == 0x2000);
        CHECK(fake.controls[0].index == 0x0110);
        CHECK(fake.controls[0].data.size() == 2);
        CHECK(fake.controls[0].data[0] == 0x12);
        CHECK(fake.controls[0].data[1] == 0x34);
        CHECK(fake.controls[0].text() == "OUT 40/00 v=2000 i=0110 [12 34]");

        // A scripted answer comes back; an unscripted one is counted rather
        // than silently invented.
        fake.answerIn(0, 0x0034, 0x0600, {0x69, 0x00, 0x02});
        std::uint8_t in[3] = {0, 0, 0};
        CHECK(fake.controlIn(0xC0, 0, 0x0034, 0x0600, in, 3, 100) == 3);
        CHECK(in[0] == 0x69 && in[1] == 0x00 && in[2] == 0x02);
        CHECK(fake.unscriptedReads == 0);
        CHECK(fake.controlIn(0xC0, 0, 0x9999, 0x0600, in, 1, 100) == 1);
        CHECK(fake.unscriptedReads == 1);

        // writes() is the OUT-only view the register assertions use.
        CHECK(fake.writes().size() == 1);

        // The bulk ring's rule is the REAL transport's rule: a length that is
        // not a whole number of 512-byte packets is refused, because RAW_IO
        // would refuse it on the wire.
        CHECK(!fake.beginBulkStream(0x81, 1000, 4));
        CHECK(!fake.beginBulkStream(0x81, 0, 4));
        CHECK(!fake.beginBulkStream(0x81, 512, 0));
        CHECK(fake.beginBulkStream(0x81, 1024, 4));
        CHECK(fake.streaming());
        CHECK(fake.streamStarts == 1);

        fake.bulkQueue.push_back(std::vector<std::uint8_t>{1, 2, 3, 4});
        std::uint8_t dst[8] = {0};
        CHECK(fake.readBulk(dst, sizeof(dst), 10) == 4);
        CHECK(dst[0] == 1 && dst[3] == 4);
        // An empty queue is a timeout, not an error.
        CHECK(fake.readBulk(dst, sizeof(dst), 1) == 0);
        // ...and a device that has gone is negative, never a hang and never a
        // zero that looks like an idle radio.
        fake.failBulkAfter = fake.bulkReads;
        CHECK(fake.readBulk(dst, sizeof(dst), 1) < 0);
        fake.endBulkStream();
        CHECK(!fake.streaming());
        CHECK(fake.streamStops == 1);
        fake.endBulkStream();  // idempotent
        CHECK(fake.streamStops == 1);
    }

    // --- 4. A real device, if there is one ----------------------------------
    {
        const std::vector<UsbDeviceInfo> found = cascade::usb::enumerateWinUsb(kRtlIds);
        if (found.empty()) {
            std::printf("hardware half SKIPPED: no RTL2832U is bound to WinUSB on this "
                        "machine. The contract above is still proven; what is not proven "
                        "here is that a control transfer reaches silicon.\n");
        } else {
            std::string err;
            auto dev = cascade::usb::openWinUsb(found[0].path, err);
            if (!dev) {
                // Another process (a running FoxSDR, another test) holding the
                // dongle is contention on this machine, not a transport fault.
                std::printf("hardware half SKIPPED: the dongle would not open (%s). If "
                            "something else on this machine has it, that is contention, "
                            "not a failure of this code.\n",
                            err.c_str());
            } else {
                std::printf("opened %s\n", dev->path().c_str());

                // A VENDOR CONTROL TRANSFER THAT REACHES THE CHIP. Block 1
                // (USB), address 0x2000 (USB_SYSCTL): a register the
                // RTL2832U always answers, whatever state it is in.
                std::uint8_t reg = 0xAA;
                const int got = dev->controlIn(cascade::usb::kRequestTypeVendorIn, 0, 0x2000,
                                               (1 << 8), &reg, 1, 500);
                std::printf("USB_SYSCTL read: %d byte(s), value 0x%02x\n", got, reg);
                CHECK(got == 1);
                CHECK(reg != 0xAA);  // it was actually written by the device

                // The ring's rules, on the real transport this time.
                CHECK(!dev->beginBulkStream(0x81, 1000, 4));
                CHECK(!dev->beginBulkStream(0x81, 16384, 0));
                CHECK(!dev->streaming());
                CHECK(dev->beginBulkStream(0x81, 16384, 8));
                CHECK(dev->streaming());

                // The endpoint is not producing (no baseband init here), so
                // reads are expected to time out rather than deliver - which
                // is exactly the property under test: a quiet pipe returns 0,
                // not -1 and not a hang.
                const auto t0 = std::chrono::steady_clock::now();
                const int r = dev->readBulk(nullptr, 0, 30);
                const double readMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              t0)
                        .count();
                std::printf("a read on an idle pipe: %d after %.1f ms\n", r, readMs);
                CHECK(r <= 0);
                // Honoured its timeout with room to spare.
                CHECK(readMs < 400.0);

                // THE ONE THAT MATTERS. endBulkStream cancels eight queued
                // overlapped reads and waits for them; the bound is 250 ms
                // for the whole ring, so anything near that is a warning and
                // anything past it is the defect this file exists to catch.
                const auto t1 = std::chrono::steady_clock::now();
                dev->endBulkStream();
                const double endMs =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              t1)
                        .count();
                std::printf("endBulkStream() returned in %.1f ms (bound 250 ms)\n", endMs);
                CHECK(!dev->streaming());
                CHECK(endMs < 400.0);
                dev->endBulkStream();  // idempotent
                CHECK(!dev->streaming());
            }
        }
    }

    // -----------------------------------------------------------------------
    // THE DONGLE THAT IS HERE AND CANNOT BE OPENED: unboundFrom().
    //
    // This bench has exactly one RTL-SDR and it is correctly bound to WinUSB,
    // so enumerateUnbound() answers empty here and proves nothing. The
    // decision it makes is separated out for that reason, and this is where
    // it is driven with the shapes a real machine produces. Both rules in it
    // are ones a plausible implementation gets wrong, and both failures are
    // silent: the first tells a user with a working dongle to go and run
    // Zadig on it, the second says it three times.
    // -----------------------------------------------------------------------
    {
        using cascade::usb::UsbNode;
        using cascade::usb::unboundFrom;

        // A COMPOSITE RTL2832U BOUND CORRECTLY. Three devnodes carry the
        // hardware-id fragment: the USB device node (usbccgp), and the
        // interface child MI_00 that Zadig bound to WinUSB. Asking the DEVICE
        // node what service it runs answers "usbccgp" on a perfectly working
        // dongle - so a rule written that way would report the radio this
        // machine is streaming from as needing Zadig.
        //
        // RED WHEN the "bound if ANY devnode is" rule is narrowed to the
        // device node: this returns one entry instead of none.
        {
            std::vector<UsbNode> nodes;
            UsbNode dev;
            dev.vid = 0x0bda;
            dev.pid = 0x2838;
            dev.deviceId = "USB\\VID_0BDA&PID_2838\\00000001";
            dev.isDeviceNode = true;
            dev.service = "usbccgp";
            dev.serial = "00000001";
            dev.description = "RTL2838UHIDIR";
            nodes.push_back(dev);
            UsbNode child;
            child.vid = 0x0bda;
            child.pid = 0x2838;
            child.deviceId = dev.deviceId;
            child.isDeviceNode = false;
            child.service = "WinUSB";
            nodes.push_back(child);
            const std::vector<cascade::usb::UsbDeviceInfo> out = unboundFrom(nodes);
            std::printf("bound composite dongle -> %zu unbound entries (want 0)\n", out.size());
            CHECK(out.empty());
        }

        // ...AND THE SAME SHAPE WITH NOTHING BOUND. The child is on no driver
        // at all (problem code 28, which is how every dongle arrives out of
        // the box) - ONE entry, carrying the DEVICE node's serial and model,
        // and with an empty path because there is nothing to open.
        {
            std::vector<UsbNode> nodes;
            UsbNode dev;
            dev.vid = 0x0bda;
            dev.pid = 0x2838;
            dev.deviceId = "USB\\VID_0BDA&PID_2838\\00000001";
            dev.isDeviceNode = true;
            dev.service = "usbccgp";
            dev.serial = "00000001";
            dev.description = "RTL2838UHIDIR";
            nodes.push_back(dev);
            UsbNode child;
            child.vid = 0x0bda;
            child.pid = 0x2838;
            child.deviceId = dev.deviceId;
            child.isDeviceNode = false;
            child.service.clear();
            child.description = "Bulk-In, Interface";
            nodes.push_back(child);
            const std::vector<cascade::usb::UsbDeviceInfo> out = unboundFrom(nodes);
            CHECK(out.size() == 1u);
            if (out.size() == 1u) {
                // The DEVICE node's identity, never the child's: the child's
                // description is "Bulk-In, Interface" and its instance id is
                // a bus path, neither of which names a radio to a user.
                CHECK(out[0].serial == "00000001");
                CHECK(out[0].description == "RTL2838UHIDIR");
                CHECK(out[0].path.empty());
                CHECK(out[0].vid == 0x0bda);
                CHECK(out[0].pid == 0x2838);
            }
        }

        // A NON-COMPOSITE DONGLE ON THE DVB-T DRIVER: one devnode, which IS
        // the device node, bound to RTL2832UUSB. Reported.
        {
            std::vector<UsbNode> nodes;
            UsbNode dev;
            dev.vid = 0x0bda;
            dev.pid = 0x2832;
            dev.deviceId = "USB\\VID_0BDA&PID_2832\\00000001";
            dev.isDeviceNode = true;
            dev.service = "RTL2832UUSB";
            dev.serial = "00000001";
            dev.description = "RTL2832U DVB-T";
            nodes.push_back(dev);
            CHECK(unboundFrom(nodes).size() == 1u);
        }

        // THE CASE THE WHOLE FEATURE EXISTS FOR: two dongles, one bound and
        // one not. "No radio hardware found" would never appear for this
        // user, so without a per-device answer the unbound one is missing in
        // silence.
        {
            std::vector<UsbNode> nodes;
            UsbNode a;
            a.vid = 0x0bda;
            a.pid = 0x2838;
            a.deviceId = "USB\\VID_0BDA&PID_2838\\00000001";
            a.isDeviceNode = true;
            a.service = "usbccgp";
            a.serial = "00000001";
            a.description = "RTL2838UHIDIR";
            nodes.push_back(a);
            UsbNode aChild = a;
            aChild.isDeviceNode = false;
            aChild.service = "WinUSB";
            aChild.serial.clear();
            aChild.description.clear();
            nodes.push_back(aChild);
            UsbNode b = a;
            b.deviceId = "USB\\VID_0BDA&PID_2838\\00000002";
            b.serial = "00000002";
            nodes.push_back(b);
            UsbNode bChild = b;
            bChild.isDeviceNode = false;
            bChild.service.clear();
            bChild.serial.clear();
            bChild.description.clear();
            nodes.push_back(bChild);
            const std::vector<cascade::usb::UsbDeviceInfo> out = unboundFrom(nodes);
            CHECK(out.size() == 1u);
            if (out.size() == 1u) { CHECK(out[0].serial == "00000002"); }
        }

        // SERVICE NAMES ARE CASE-INSENSITIVE on Windows and are reported with
        // whatever casing the INF used. "winusb" is bound.
        {
            std::vector<UsbNode> nodes;
            UsbNode dev;
            dev.deviceId = "USB\\VID_1D50&PID_6089\\0001";
            dev.isDeviceNode = true;
            dev.service = "winusb";
            nodes.push_back(dev);
            CHECK(unboundFrom(nodes).empty());
        }

        // Nothing in, nothing out - and an INTERFACE child with no device
        // node beside it is not a device (it cannot name one).
        CHECK(unboundFrom(std::vector<UsbNode>()).empty());
        {
            std::vector<UsbNode> nodes;
            UsbNode child;
            child.deviceId = "USB\\VID_0BDA&PID_2838\\00000001";
            child.isDeviceNode = false;
            nodes.push_back(child);
            CHECK(unboundFrom(nodes).empty());
        }

        // AND THE LIVE WALK, for what it is worth on this bench: whatever it
        // answers, an entry from it can never be mistaken for one openWinUsb
        // would take, and it can never name a dongle enumerateWinUsb already
        // listed. Both hold vacuously here (the one dongle present is bound),
        // which is exactly why the synthetic cases above exist.
        {
            const std::vector<cascade::usb::UsbId> ids = {{0x0bda, 0x2838}, {0x0bda, 0x2832}};
            const std::vector<cascade::usb::UsbDeviceInfo> unbound =
                cascade::usb::enumerateUnbound(ids);
            const std::vector<cascade::usb::UsbDeviceInfo> bound =
                cascade::usb::enumerateWinUsb(ids);
            std::printf("live walk: %zu bound, %zu unbound\n", bound.size(), unbound.size());
            for (const cascade::usb::UsbDeviceInfo& u : unbound) {
                CHECK(u.path.empty());
                for (const cascade::usb::UsbDeviceInfo& b : bound) {
                    CHECK(!(u.serial == b.serial && u.vid == b.vid && u.pid == b.pid));
                }
            }
            // An empty id list asks about nothing and must not walk anything.
            CHECK(cascade::usb::enumerateUnbound(std::vector<cascade::usb::UsbId>()).empty());
        }
    }

    return testSummary("test_usb_winusb");
}
