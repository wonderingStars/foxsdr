// test_fx2_loader.cpp - the FX2 RAM loader: Intel-HEX parsing, the exact
// control-transfer sequence the load puts on the wire, and the bounded wait
// for the device to come back.
//
// THE FIRMWARE HERE IS SYNTHETIC. AOR's fx2fw.hex is AOR copyright and is not
// in this repository yet, so every image below is written by this file, with
// its own record builder and its own checksum arithmetic (Intel-HEX: the
// two's complement of the byte sum). The expected wire sequence comes from
// the EZ-USB FX2 Technical Reference Manual's Firmware Load request (0xA0,
// vendor OUT, address in wValue) and CPUCS at 0xE600: hold with 01, write,
// release with 00. Tested against a fake; not tested on hardware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "source/fx2_loader.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

namespace fx2 = cascade::source::fx2;
using cascade::usb::FakeControl;
using cascade::usb::FakeUsbDevice;
using cascade::usb::UsbDeviceInfo;

namespace {

std::string record(std::uint8_t type, std::uint16_t addr, const std::vector<std::uint8_t>& data,
                   int checksumDelta = 0) {
    std::vector<std::uint8_t> r = {static_cast<std::uint8_t>(data.size()),
                                   static_cast<std::uint8_t>(addr >> 8),
                                   static_cast<std::uint8_t>(addr & 0xFF), type};
    r.insert(r.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (std::uint8_t b : r) { sum += b; }
    r.push_back(static_cast<std::uint8_t>((0x100u - (sum & 0xFFu) + static_cast<unsigned>(checksumDelta)) & 0xFFu));
    std::string s = ":";
    for (std::uint8_t b : r) {
        char h[3];
        std::snprintf(h, sizeof(h), "%02X", b);
        s += h;
    }
    return s;
}

std::vector<std::uint8_t> bytesFrom(std::uint8_t first, std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<std::uint8_t>(first + i * 7); }
    return v;
}

// 200 bytes at 0x0000 in 16-byte records, and 10 bytes at 0x1000 - the
// 0x1000 record FIRST in the file, so writing in address order is a choice
// the loader has to make rather than an accident of file order.
std::string goodImage(const char* eol = "\n") {
    std::string s;
    s += record(0x00, 0x1000, bytesFrom(0xA0, 10)) + eol;
    const std::vector<std::uint8_t> low = bytesFrom(0x01, 200);
    for (std::size_t off = 0; off < 200; off += 16) {
        const std::size_t n = off + 16 <= 200 ? 16 : 200 - off;
        s += record(0x00, static_cast<std::uint16_t>(off),
                    std::vector<std::uint8_t>(low.begin() + static_cast<std::ptrdiff_t>(off),
                                              low.begin() + static_cast<std::ptrdiff_t>(off + n))) +
             eol;
    }
    s += record(0x01, 0, {}) + eol;
    return s;
}

bool hasText(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

}  // namespace

int main() {
    // --- a good file --------------------------------------------------------
    {
        const fx2::HexImage img = fx2::parseIntelHex(goodImage());
        CHECK(img.valid);
        CHECK(img.error.empty());
        CHECK(img.segments.size() == 2);
        CHECK(img.segments[0].address == 0x0000);
        CHECK(img.segments[0].bytes == bytesFrom(0x01, 200));
        CHECK(img.segments[1].address == 0x1000);
        CHECK(img.segments[1].bytes == bytesFrom(0xA0, 10));
        CHECK(img.totalBytes == 210);
        CHECK(img.dataRecords == 14);
        // CRLF, blank lines, and zero-valued extended-address and
        // start-address records change nothing.
        std::string crlf = record(0x04, 0, {0, 0}) + "\r\n\r\n" + record(0x03, 0, {0, 0, 0, 0}) +
                           "\r\n" + goodImage("\r\n");
        const fx2::HexImage img2 = fx2::parseIntelHex(crlf);
        CHECK(img2.valid);
        CHECK(img2.totalBytes == 210);
        // Lower-case hex digits are hex digits.
        std::string lower = goodImage();
        for (char& c : lower) { if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); } }
        CHECK(fx2::parseIntelHex(lower).valid);
    }
    // --- bad checksum -------------------------------------------------------
    {
        std::string s = record(0x00, 0, bytesFrom(1, 16)) + "\n" + record(0x00, 16, bytesFrom(2, 16)) +
                        "\n" + record(0x00, 32, bytesFrom(3, 16), +1) + "\n" + record(0x01, 0, {}) + "\n";
        const fx2::HexImage img = fx2::parseIntelHex(s);
        CHECK(!img.valid);
        CHECK(hasText(img.error, "line 3"));
        CHECK(hasText(img.error, "checksum"));
        CHECK(img.segments.empty());
        std::printf("bad checksum: %s\n", img.error.c_str());
    }
    // --- bad record type ----------------------------------------------------
    {
        std::string s = record(0x00, 0, bytesFrom(1, 4)) + "\n" + record(0x06, 0, {1}) + "\n" +
                        record(0x01, 0, {}) + "\n";
        const fx2::HexImage img = fx2::parseIntelHex(s);
        CHECK(!img.valid);
        CHECK(hasText(img.error, "line 2"));
        CHECK(hasText(img.error, "unknown record type 06"));
        std::printf("bad type: %s\n", img.error.c_str());
    }
    // --- truncated ----------------------------------------------------------
    {
        const std::string good = goodImage();
        // Cut at every position: never valid until the whole EOF record is in.
        int validCuts = 0;
        for (std::size_t cut = 0; cut < good.size(); ++cut) {
            if (fx2::parseIntelHex(good.substr(0, cut)).valid) { ++validCuts; }
        }
        // Only the cut that drops just the final '\n' still has every record.
        CHECK(validCuts == 1);
        // Missing EOF record.
        const std::string noEof = good.substr(0, good.rfind(':'));
        const fx2::HexImage a = fx2::parseIntelHex(noEof);
        CHECK(!a.valid);
        CHECK(hasText(a.error, "no end-of-file record"));
        // Cut mid-record.
        const fx2::HexImage b = fx2::parseIntelHex(good.substr(0, 30));
        CHECK(!b.valid);
        CHECK(hasText(b.error, "line 1"));
        std::printf("truncated: %s / %s\n", a.error.c_str(), b.error.c_str());
        // Empty file.
        CHECK(!fx2::parseIntelHex("").valid);
    }
    // --- everything else refused --------------------------------------------
    {
        const std::string eof = record(0x01, 0, {}) + "\n";
        // Extended address that is not zero.
        CHECK(!fx2::parseIntelHex(record(0x04, 0, {0, 1}) + "\n" + record(0x00, 0, {1}) + "\n" + eof).valid);
        CHECK(!fx2::parseIntelHex(record(0x02, 0, {0x10, 0}) + "\n" + record(0x00, 0, {1}) + "\n" + eof).valid);
        // Overlapping data.
        const fx2::HexImage ov = fx2::parseIntelHex(record(0x00, 0x10, bytesFrom(1, 8)) + "\n" +
                                                    record(0x00, 0x14, bytesFrom(1, 8)) + "\n" + eof);
        CHECK(!ov.valid);
        CHECK(hasText(ov.error, "overlaps"));
        // Writing CPUCS itself.
        const fx2::HexImage cp = fx2::parseIntelHex(record(0x00, 0xE5FF, {1, 2}) + "\n" + eof);
        CHECK(!cp.valid);
        CHECK(hasText(cp.error, "CPUCS"));
        // Past 64 KB.
        CHECK(!fx2::parseIntelHex(record(0x00, 0xFFFE, {1, 2, 3}) + "\n" + eof).valid);
        // After EOF.
        CHECK(!fx2::parseIntelHex(record(0x00, 0, {1}) + "\n" + eof + record(0x00, 8, {1}) + "\n").valid);
        // Not starting with ':' / not hex / declared length wrong / EOF with data.
        CHECK(!fx2::parseIntelHex("0100000001FE\n" + eof).valid);
        CHECK(!fx2::parseIntelHex(":0100000G01FE\n" + eof).valid);
        CHECK(!fx2::parseIntelHex(":02000000010FE\n" + eof).valid);
        CHECK(!fx2::parseIntelHex(record(0x01, 0, {5}) + "\n").valid);
        // An image with no data at all.
        CHECK(!fx2::parseIntelHex(eof).valid);
    }
    // --- a file that does not exist ------------------------------------------
    {
        bool found = true;
        const fx2::HexImage img = fx2::readIntelHexFile("/nonexistent/dir/fx2fw.hex", found);
        CHECK(!found);
        CHECK(!img.valid);
    }

    // --- the exact control-transfer sequence ---------------------------------
    {
        const fx2::HexImage img = fx2::parseIntelHex(goodImage());
        FakeUsbDevice dev;
        std::string error;
        CHECK(fx2::loadImage(dev, img, 1000, error));
        CHECK(error.empty());
        const std::vector<FakeControl> w = dev.writes();
        const std::vector<std::uint8_t> low = bytesFrom(0x01, 200);
        struct Want {
            std::uint16_t value;
            std::vector<std::uint8_t> data;
        };
        const std::vector<Want> want = {
            {0xE600, {0x01}},
            {0x0000, std::vector<std::uint8_t>(low.begin(), low.begin() + 64)},
            {0x0040, std::vector<std::uint8_t>(low.begin() + 64, low.begin() + 128)},
            {0x0080, std::vector<std::uint8_t>(low.begin() + 128, low.begin() + 192)},
            {0x00C0, std::vector<std::uint8_t>(low.begin() + 192, low.end())},
            {0x1000, bytesFrom(0xA0, 10)},
            {0xE600, {0x00}},
        };
        CHECK(w.size() == want.size());
        CHECK(dev.controls.size() == want.size());  // no reads at all
        for (std::size_t k = 0; k < want.size() && k < w.size(); ++k) {
            const bool ok = w[k].requestType == 0x40 && w[k].request == 0xA0 &&
                            w[k].value == want[k].value && w[k].index == 0 &&
                            w[k].data == want[k].data;
            if (!ok) { std::printf("  step %zu: %s\n", k, w[k].text().c_str()); }
            CHECK(ok);
        }
        std::printf("wire: first %s, last %s\n", w.front().text().c_str(), w.back().text().c_str());
    }
    // --- a write that fails leaves the CPU held --------------------------------
    {
        const fx2::HexImage img = fx2::parseIntelHex(goodImage());
        FakeUsbDevice dev;
        dev.failControlAfter = 2;  // hold and one chunk succeed, the next fails
        std::string error;
        CHECK(!fx2::loadImage(dev, img, 1000, error));
        CHECK(hasText(error, "writing firmware at 0x0040"));
        CHECK(dev.controls.size() == 3);
        // No release was attempted after the failure.
        bool released = false;
        for (const FakeControl& c : dev.controls) {
            released = released || (c.value == 0xE600 && c.data == std::vector<std::uint8_t>{0x00});
        }
        CHECK(!released);
        // An invalid image sends nothing at all.
        FakeUsbDevice dev2;
        fx2::HexImage bad;
        CHECK(!fx2::loadImage(dev2, bad, 1000, error));
        CHECK(dev2.controls.empty());
    }

    // --- the bounded wait for re-enumeration ----------------------------------
    {
        const fx2::ReenumerationTiming timing{std::chrono::milliseconds(1000),
                                              std::chrono::milliseconds(5000),
                                              std::chrono::milliseconds(100)};
        const auto noSleep = [](std::chrono::milliseconds) {};
        UsbDeviceInfo dev;
        dev.vid = 0x08D0;
        dev.pid = 0xA001;
        dev.path = "/dev/bus/usb/001/005";
        UsbDeviceInfo back = dev;
        back.path = "/dev/bus/usb/001/006";

        // Gone for three polls, then back at a new path: Returned, after settle.
        int calls = 0;
        UsbDeviceInfo out;
        auto r = fx2::awaitReenumeration(
            dev.path,
            [&]() {
                ++calls;
                if (calls <= 1) { return std::vector<UsbDeviceInfo>{dev}; }
                if (calls <= 4) { return std::vector<UsbDeviceInfo>{}; }
                return std::vector<UsbDeviceInfo>{back};
            },
            noSleep, timing, out);
        CHECK(r == fx2::Reenumeration::Returned);
        CHECK(out.path == back.path);
        CHECK(calls == 11);  // round 10 is the first at >= 1000 ms of settle

        // Gone and back at the SAME path (Windows keeps the instance path):
        // still Returned.
        calls = 0;
        r = fx2::awaitReenumeration(
            dev.path,
            [&]() {
                ++calls;
                return calls == 3 ? std::vector<UsbDeviceInfo>{} : std::vector<UsbDeviceInfo>{dev};
            },
            noSleep, timing, out);
        CHECK(r == fx2::Reenumeration::Returned);

        // Never left: NeverLeft, and the number of lookups is bounded.
        calls = 0;
        r = fx2::awaitReenumeration(
            dev.path, [&]() { ++calls; return std::vector<UsbDeviceInfo>{dev}; }, noSleep, timing, out);
        CHECK(r == fx2::Reenumeration::NeverLeft);
        CHECK(calls == 51);
        CHECK(out.path == dev.path);

        // Never came back: Gone, bounded the same way.
        calls = 0;
        r = fx2::awaitReenumeration(
            dev.path, [&]() { ++calls; return std::vector<UsbDeviceInfo>{}; }, noSleep, timing, out);
        CHECK(r == fx2::Reenumeration::Gone);
        CHECK(calls == 51);

        // The sleeps requested add up to the budget and no more.
        long long slept = 0;
        fx2::awaitReenumeration(
            dev.path, []() { return std::vector<UsbDeviceInfo>{}; },
            [&](std::chrono::milliseconds m) { slept += m.count(); }, timing, out);
        CHECK(slept == 5000);
    }

    return testSummary("test_fx2_loader");
}
