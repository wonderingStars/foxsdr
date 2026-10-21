// test_usb_iso.cpp - isochronous IN, the part every platform shares: packet
// concatenation honouring ACTUAL lengths, the endpoint-descriptor read that
// sizes the packet slots, and the fake transport's iso path that the AOR
// driver's tests stand on.
//
// WHAT IS AND IS NOT PROVEN HERE. concatIsoPackets() and
// isoEndpointBytesPerInterval() are the exact functions the usbfs and WinUSB
// transports call on a completed transfer, so they are tested against
// hand-built transfers and descriptors (tested against a fake). The usbfs
// URB submission/reap and the WinUSB WinUsb_*Isoch* calls around them need a
// device and are NOT tested - there is no isochronous device on this bench -
// and the WinUSB half has not been compiled with MSVC.
//
// BOUNDS. Every buffer is an exactly sized std::vector, so an
// AddressSanitizer build turns any out-of-range access into a failure.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "test_check.hpp"
#include "usb/usb_fake.hpp"
#include "usb/usb_iso.hpp"

using cascade::usb::concatIsoPackets;
using cascade::usb::FakeIsoPacket;
using cascade::usb::FakeUsbDevice;
using cascade::usb::IsoPacket;
using cascade::usb::IsoTransferStats;
using cascade::usb::isoEndpointBytesPerInterval;
using cascade::usb::UsbDevice;

namespace {

// A transfer laid out as usbfs lays one out: slot k at k * slot, filled with
// `fill` so a byte read from outside a packet's actual length is visible.
struct Built {
    std::vector<std::uint8_t> buffer;
    std::vector<IsoPacket> packets;
    std::vector<std::uint8_t> expected;  // the in-order concatenation
};

Built build(const std::vector<std::size_t>& lengths, std::size_t slot, std::uint8_t& counter) {
    Built b;
    b.buffer.assign(slot * lengths.size(), 0xEE);
    for (std::size_t k = 0; k < lengths.size(); ++k) {
        IsoPacket p;
        p.offset = k * slot;
        p.length = lengths[k];
        for (std::size_t i = 0; i < lengths[k]; ++i) {
            b.buffer[k * slot + i] = counter;
            b.expected.push_back(counter);
            counter = static_cast<std::uint8_t>(counter + 1 == 0xEE ? 0 : counter + 1);
        }
        b.packets.push_back(p);
    }
    return b;
}

// A configuration descriptor: interface 0 alt 0 with the given endpoint,
// and an interface 0 alt 1 with a different one, to prove alt 0 is the one
// read.
std::vector<std::uint8_t> descriptors(std::uint8_t ep, std::uint8_t attributes,
                                      std::uint16_t wMaxPacketSize, bool withDevice) {
    std::vector<std::uint8_t> d;
    if (withDevice) {
        // Device descriptor, 18 bytes, VID 08D0 PID A001.
        const std::uint8_t dev[18] = {18, 1, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 64, 0xD0, 0x08,
                                      0x01, 0xA0, 0x00, 0x01, 1,    2,    0,    1};
        d.insert(d.end(), dev, dev + 18);
    }
    const std::uint8_t cfg[9] = {9, 2, 0, 0, 1, 1, 0, 0x80, 50};
    d.insert(d.end(), cfg, cfg + 9);
    const std::uint8_t if0[9] = {9, 4, 0, 0, 2, 0xFF, 0, 0, 0};
    d.insert(d.end(), if0, if0 + 9);
    const std::uint8_t out2[7] = {7, 5, 0x02, 0x02, 0x00, 0x02, 0};  // bulk OUT 0x02
    d.insert(d.end(), out2, out2 + 7);
    const std::uint8_t in[7] = {7, 5, ep, attributes, static_cast<std::uint8_t>(wMaxPacketSize & 0xFF),
                                static_cast<std::uint8_t>(wMaxPacketSize >> 8), 1};
    d.insert(d.end(), in, in + 7);
    const std::uint8_t if0alt1[9] = {9, 4, 0, 1, 1, 0xFF, 0, 0, 0};
    d.insert(d.end(), if0alt1, if0alt1 + 9);
    const std::uint8_t in88[7] = {7, 5, 0x88, 0x01, 0x00, 0x04, 1};
    d.insert(d.end(), in88, in88 + 7);
    const std::size_t total = d.size() - (withDevice ? 18 : 0);
    const std::size_t cfgAt = withDevice ? 18 : 0;
    d[cfgAt + 2] = static_cast<std::uint8_t>(total & 0xFF);
    d[cfgAt + 3] = static_cast<std::uint8_t>(total >> 8);
    return d;
}

}  // namespace

int main() {
    // --- concatenation honours each packet's ACTUAL length ----------------
    {
        std::uint8_t counter = 0;
        const Built b = build({0, 512, 1024, 1536, 0, 7, 1536, 512}, 1536, counter);
        std::vector<std::uint8_t> dst(b.expected.size());
        IsoTransferStats st;
        const std::size_t n = concatIsoPackets(b.buffer.data(), b.buffer.size(), b.packets.data(),
                                               b.packets.size(), 1536, dst.data(), dst.size(), st);
        CHECK(n == b.expected.size());
        CHECK(dst == b.expected);
        CHECK(st.bytes == n);
        CHECK(st.packets == 8);
        CHECK(st.emptyPackets == 2);
        CHECK(st.shortPackets == 4);  // 512, 1024, 7, 512
        CHECK(st.errorPackets == 0);
        CHECK(st.malformedPackets == 0);
        CHECK(st.overflowBytes == 0);
        // Not one filler byte (0xEE) reached the output.
        bool filler = false;
        for (std::uint8_t c : dst) { filler = filler || c == 0xEE; }
        CHECK(!filler);
    }
    // --- a failed packet contributes nothing, even with a length ----------
    {
        std::uint8_t counter = 0;
        Built b = build({512, 512, 512}, 1536, counter);
        b.packets[1].status = -71;  // -EPROTO on usbfs
        std::vector<std::uint8_t> want(b.expected.begin(), b.expected.begin() + 512);
        want.insert(want.end(), b.expected.begin() + 1024, b.expected.end());
        std::vector<std::uint8_t> dst(1536);
        IsoTransferStats st;
        const std::size_t n = concatIsoPackets(b.buffer.data(), b.buffer.size(), b.packets.data(),
                                               3, 1536, dst.data(), dst.size(), st);
        CHECK(n == 1024);
        dst.resize(n);
        CHECK(dst == want);
        CHECK(st.errorPackets == 1);
    }
    // --- descriptors that point outside the buffer are refused ------------
    {
        std::uint8_t counter = 0;
        Built b = build({100, 100, 100, 100}, 256, counter);
        b.packets[0].offset = 1000;                // starts past the end (buffer is 1024)
        b.packets[1].offset = 1000;                // runs off the end: 1000 + 100 > 1024
        b.packets[2].length = 300;                 // longer than its slot
        b.packets[3].offset = static_cast<std::size_t>(-50);  // wraps if added naively
        std::vector<std::uint8_t> dst(1024);
        IsoTransferStats st;
        const std::size_t n = concatIsoPackets(b.buffer.data(), b.buffer.size(), b.packets.data(),
                                               4, 256, dst.data(), dst.size(), st);
        CHECK(n == 0);
        CHECK(st.malformedPackets == 4);
    }
    // --- a destination too small is filled and the rest counted -----------
    {
        std::uint8_t counter = 0;
        const Built b = build({1536, 1536}, 1536, counter);
        std::vector<std::uint8_t> dst(2000);
        IsoTransferStats st;
        const std::size_t n = concatIsoPackets(b.buffer.data(), b.buffer.size(), b.packets.data(),
                                               2, 1536, dst.data(), dst.size(), st);
        CHECK(n == 2000);
        CHECK(st.overflowBytes == 3072 - 2000);
        CHECK(std::vector<std::uint8_t>(b.expected.begin(), b.expected.begin() + 2000) == dst);
        // Zero capacity, null destination: nothing written, nothing read.
        const std::size_t z = concatIsoPackets(b.buffer.data(), b.buffer.size(), b.packets.data(),
                                               2, 1536, nullptr, 0, st);
        CHECK(z == 0);
        CHECK(st.overflowBytes == 3072);
    }
    // --- many random transfers ---------------------------------------------
    {
        std::mt19937 rng(9);
        const std::size_t lens[4] = {0, 512, 1024, 1536};
        std::uint8_t counter = 0;
        int bad = 0;
        for (int t = 0; t < 200; ++t) {
            std::vector<std::size_t> l(128);
            for (std::size_t& x : l) { x = lens[rng() % 4]; }
            const Built b = build(l, 1536, counter);
            std::vector<std::uint8_t> dst(128 * 1536);
            IsoTransferStats st;
            const std::size_t n = concatIsoPackets(b.buffer.data(), b.buffer.size(),
                                                   b.packets.data(), 128, 1536, dst.data(),
                                                   dst.size(), st);
            dst.resize(n);
            if (dst != b.expected) { ++bad; }
        }
        CHECK(bad == 0);
    }

    // --- the endpoint's bytes per interval, from its descriptors ----------
    {
        std::size_t bytes = 0;
        // 512 bytes, 2 additional transactions: 1536 per microframe.
        auto d = descriptors(0x86, 0x01, static_cast<std::uint16_t>(512 | (2u << 11)), true);
        CHECK(isoEndpointBytesPerInterval(d.data(), d.size(), 0, 0x86, bytes));
        CHECK(bytes == 1536);
        // Configuration descriptor alone (no device descriptor first).
        d = descriptors(0x86, 0x05, static_cast<std::uint16_t>(1024 | (1u << 11)), false);
        CHECK(isoEndpointBytesPerInterval(d.data(), d.size(), 0, 0x86, bytes));
        CHECK(bytes == 2048);
        // A BULK 0x86 is not an isochronous endpoint: false.
        d = descriptors(0x86, 0x02, 512, true);
        CHECK(!isoEndpointBytesPerInterval(d.data(), d.size(), 0, 0x86, bytes));
        // 0x88 exists only in alternate setting 1: not found in alt 0.
        CHECK(!isoEndpointBytesPerInterval(d.data(), d.size(), 0, 0x88, bytes));
        // Wrong interface number.
        d = descriptors(0x86, 0x01, 1024, true);
        CHECK(!isoEndpointBytesPerInterval(d.data(), d.size(), 1, 0x86, bytes));
        // Truncated anywhere: never reads past the end, and false once the
        // endpoint descriptor itself is cut.
        int truncatedTrue = 0;
        for (std::size_t cut = 0; cut < d.size(); ++cut) {
            std::vector<std::uint8_t> t(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(cut));
            std::size_t b2 = 0;
            if (isoEndpointBytesPerInterval(t.data(), t.size(), 0, 0x86, b2)) { ++truncatedTrue; }
        }
        // Only cuts that keep the whole 0x86 descriptor may succeed.
        CHECK(truncatedTrue == static_cast<int>(d.size() - (18 + 9 + 9 + 7 + 7) ));
        // A zero bLength would loop forever if trusted.
        std::vector<std::uint8_t> z = {0, 2, 0, 0};
        CHECK(!isoEndpointBytesPerInterval(z.data(), z.size(), 0, 0x86, bytes));
    }

    // --- the fake's iso path: packets of chosen lengths in, bytes out ------
    {
        FakeUsbDevice dev;
        dev.journal = std::make_shared<std::vector<std::string>>();
        CHECK(dev.beginIsoStream(0x86, 1536, 128, 8));
        CHECK(dev.isoStreaming());
        CHECK(dev.isoTransferBytes() == 1536u * 128u);
        std::vector<FakeIsoPacket> t;
        std::vector<std::uint8_t> want;
        const std::size_t lens[] = {0, 512, 1024, 1536, 0, 0, 33, 1536};
        std::uint8_t c = 1;
        for (std::size_t l : lens) {
            FakeIsoPacket p;
            for (std::size_t i = 0; i < l; ++i) { p.payload.push_back(c); want.push_back(c); ++c; }
            t.push_back(p);
        }
        dev.feedIso(t);
        std::vector<std::uint8_t> dst(dev.isoTransferBytes());
        IsoTransferStats st;
        CHECK(dev.readIso(dst.data(), dst.size(), 50, st) == UsbDevice::IsoRead::Completed);
        dst.resize(st.bytes);
        CHECK(dst == want);
        CHECK(st.emptyPackets == 3);
        CHECK(st.shortPackets == 3);
        // Nothing queued: a timeout, not a zero-byte completion.
        CHECK(dev.readIso(dst.data(), dst.size(), 10, st) == UsbDevice::IsoRead::Timeout);
        // A transfer of only empty packets IS a completion, with 0 bytes.
        dev.feedIso(std::vector<FakeIsoPacket>(128));
        std::vector<std::uint8_t> dst2(dev.isoTransferBytes());
        CHECK(dev.readIso(dst2.data(), dst2.size(), 10, st) == UsbDevice::IsoRead::Completed);
        CHECK(st.bytes == 0);
        CHECK(st.emptyPackets == 128);
        // Bulk OUT is recorded in order with the iso events.
        const std::uint8_t cmd[3] = {1, 2, 3};
        CHECK(dev.writeBulk(0x02, cmd, 3, 100) == 3);
        dev.endIsoStream();
        CHECK(!dev.isoStreaming());
        CHECK(dev.journal->size() == 3);
        CHECK((*dev.journal)[0] == "iso-begin 86");
        CHECK((*dev.journal)[1] == "bulk-out 02 [01 02 03]");
        CHECK((*dev.journal)[2] == "iso-end");
    }
    // --- the base class refuses what it cannot do --------------------------
    {
        // A transport that predates iso (every other fake in tests/) gets the
        // defaults: refusals, never a crash.
        struct Minimal final : UsbDevice {
            int controlOut(std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t,
                           const std::uint8_t*, std::size_t, unsigned) override { return -1; }
            int controlIn(std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t*,
                          std::size_t, unsigned) override { return -1; }
            bool beginBulkStream(std::uint8_t, std::size_t, std::size_t) override { return false; }
            int readBulk(std::uint8_t*, std::size_t, unsigned) override { return -1; }
            void endBulkStream() override {}
            bool streaming() const override { return false; }
            bool resetPipe(std::uint8_t) override { return false; }
            const std::string& path() const override { return p; }
            const std::string& lastError() const override { return p; }
            std::string p;
        } m;
        const std::uint8_t b = 0;
        CHECK(m.writeBulk(2, &b, 1, 10) < 0);
        CHECK(!m.beginIsoStream(0x86, 1536, 128, 8));
        IsoTransferStats st;
        CHECK(m.readIso(nullptr, 0, 1, st) == UsbDevice::IsoRead::Failed);
        CHECK(!m.isoStreaming());
    }
    return testSummary("test_usb_iso");
}
