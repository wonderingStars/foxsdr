// The FX2 RAM loader. See fx2_loader.hpp for the sources and the rules.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/fx2_loader.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

namespace cascade::source::fx2 {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

HexImage fail(std::size_t line, const std::string& what) {
    HexImage img;
    img.valid = false;
    char head[48];
    std::snprintf(head, sizeof(head), "line %zu: ", line);
    img.error = (line > 0 ? std::string(head) : std::string()) + what;
    return img;
}

}  // namespace

HexImage parseIntelHex(const std::string& text) {
    // Every data byte, by address, so overlaps can be refused exactly.
    std::vector<std::uint8_t> ram(0x10000, 0);
    std::vector<bool> used(0x10000, false);
    std::size_t dataRecords = 0;
    bool sawEof = false;

    std::size_t lineNo = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) { end = text.size(); }
        std::string line = text.substr(at, end - at);
        at = end + 1;
        ++lineNo;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) { ++lead; }
        line.erase(0, lead);
        if (line.empty()) {
            if (end == text.size()) { break; }
            continue;
        }
        if (sawEof) { return fail(lineNo, "a record after the end-of-file record"); }
        if (line[0] != ':') { return fail(lineNo, "does not start with ':'"); }
        if ((line.size() - 1) % 2 != 0) {
            return fail(lineNo, "an odd number of hex digits (the line is truncated)");
        }
        std::vector<std::uint8_t> rec;
        rec.reserve((line.size() - 1) / 2);
        for (std::size_t i = 1; i + 1 < line.size(); i += 2) {
            const int hi = hexDigit(line[i]);
            const int lo = hexDigit(line[i + 1]);
            if (hi < 0 || lo < 0) { return fail(lineNo, "a character that is not a hex digit"); }
            rec.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
        }
        if (rec.size() < 5) { return fail(lineNo, "too short to be a record (truncated)"); }
        const std::size_t count = rec[0];
        if (rec.size() != count + 5) {
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "declares %zu data bytes but carries %zu (truncated or corrupt)", count,
                          rec.size() - 5);
            return fail(lineNo, msg);
        }
        unsigned sum = 0;
        for (std::uint8_t b : rec) { sum += b; }
        if ((sum & 0xFFu) != 0) {
            unsigned partial = 0;
            for (std::size_t i = 0; i + 1 < rec.size(); ++i) { partial += rec[i]; }
            char msg[96];
            std::snprintf(msg, sizeof(msg), "checksum is %02X, expected %02X",
                          static_cast<unsigned>(rec.back()),
                          static_cast<unsigned>((0x100u - (partial & 0xFFu)) & 0xFFu));
            return fail(lineNo, msg);
        }
        const std::uint16_t addr = static_cast<std::uint16_t>((rec[1] << 8) | rec[2]);
        const std::uint8_t type = rec[3];
        const std::uint8_t* data = rec.data() + 4;
        switch (type) {
            case 0x00: {  // data
                if (static_cast<std::size_t>(addr) + count > 0x10000u) {
                    return fail(lineNo, "data runs past the FX2's 64 KB address space");
                }
                for (std::size_t i = 0; i < count; ++i) {
                    const std::size_t a = addr + i;
                    if (a == kCpucsAddress) {
                        return fail(lineNo, "writes the CPUCS register (0xE600) itself, which "
                                            "would release the CPU during the load");
                    }
                    if (used[a]) {
                        char msg[80];
                        std::snprintf(msg, sizeof(msg), "overlaps earlier data at 0x%04zX", a);
                        return fail(lineNo, msg);
                    }
                    used[a] = true;
                    ram[a] = data[i];
                }
                ++dataRecords;
                break;
            }
            case 0x01:  // end of file
                if (count != 0) { return fail(lineNo, "an end-of-file record with data"); }
                sawEof = true;
                break;
            case 0x02:    // extended segment address
            case 0x04: {  // extended linear address
                if (count != 2) { return fail(lineNo, "an address record that is not 2 bytes"); }
                if (data[0] != 0 || data[1] != 0) {
                    return fail(lineNo, "an extended address - the FX2 has 16-bit addresses");
                }
                break;
            }
            case 0x03:  // start segment address
            case 0x05:  // start linear address
                if (count != 4) { return fail(lineNo, "a start-address record that is not 4 bytes"); }
                break;  // meaningless to an 8051 that starts at 0 after reset
            default: {
                char msg[64];
                std::snprintf(msg, sizeof(msg), "unknown record type %02X",
                              static_cast<unsigned>(type));
                return fail(lineNo, msg);
            }
        }
        if (end == text.size()) { break; }
    }
    if (!sawEof) { return fail(0, "no end-of-file record - the file is truncated"); }

    HexImage img;
    for (std::size_t a = 0; a < 0x10000; ++a) {
        if (!used[a]) { continue; }
        if (img.segments.empty() ||
            static_cast<std::size_t>(img.segments.back().address) +
                    img.segments.back().bytes.size() != a) {
            HexSegment s;
            s.address = static_cast<std::uint16_t>(a);
            img.segments.push_back(std::move(s));
        }
        img.segments.back().bytes.push_back(ram[a]);
        ++img.totalBytes;
    }
    if (img.totalBytes == 0) { return fail(0, "no data records"); }
    img.dataRecords = dataRecords;
    img.valid = true;
    return img;
}

HexImage readIntelHexFile(const std::string& path, bool& found) {
    found = false;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        HexImage img;
        img.error = "cannot be read";
        return img;
    }
    found = true;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseIntelHex(ss.str());
}

bool loadImage(cascade::usb::UsbDevice& dev, const HexImage& image, unsigned timeoutMs,
               std::string& error) {
    if (!image.valid) {
        error = "the firmware image is not valid: " + image.error;
        return false;
    }
    const auto write = [&](std::uint16_t address, const std::uint8_t* data, std::size_t len,
                           const char* what) {
        const int r = dev.controlOut(cascade::usb::kRequestTypeVendorOut, kFirmwareLoadRequest,
                                     address, 0, data, len, timeoutMs);
        if (r < 0 || static_cast<std::size_t>(r) != len) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%s at 0x%04X failed: %s", what,
                          static_cast<unsigned>(address),
                          r < 0 ? dev.lastError().c_str() : "short write");
            error = msg;
            return false;
        }
        return true;
    };
    const std::uint8_t hold = kCpucsHold;
    if (!write(kCpucsAddress, &hold, 1, "holding the FX2 CPU in reset")) { return false; }
    for (const HexSegment& s : image.segments) {
        for (std::size_t off = 0; off < s.bytes.size(); off += kLoadChunkBytes) {
            const std::size_t n = std::min(kLoadChunkBytes, s.bytes.size() - off);
            if (!write(static_cast<std::uint16_t>(s.address + off), s.bytes.data() + off, n,
                       "writing firmware")) {
                return false;  // CPU deliberately left held: see the header
            }
        }
    }
    const std::uint8_t run = kCpucsRelease;
    return write(kCpucsAddress, &run, 1, "releasing the FX2 CPU");
}

Reenumeration awaitReenumeration(
    const std::string& oldPath,
    const std::function<std::vector<cascade::usb::UsbDeviceInfo>()>& list,
    const std::function<void(std::chrono::milliseconds)>& sleep, const ReenumerationTiming& timing,
    cascade::usb::UsbDeviceInfo& out) {
    const auto poll = timing.poll.count() > 0 ? timing.poll : std::chrono::milliseconds(1);
    const long long rounds = timing.budget.count() / poll.count();
    const auto deadline = std::chrono::steady_clock::now() + timing.budget;
    bool sawGone = false;
    bool present = false;
    for (long long round = 0; round <= rounds; ++round) {
        const std::chrono::milliseconds elapsed = poll * round;
        const std::vector<cascade::usb::UsbDeviceInfo> now = list();
        present = !now.empty();
        if (!present) {
            sawGone = true;
        } else {
            out = now.front();
            for (const auto& d : now) {
                if (d.path != oldPath) { out = d; }  // prefer a new path
            }
            if ((sawGone || out.path != oldPath) && elapsed >= timing.settle) {
                return Reenumeration::Returned;
            }
        }
        if (round == rounds || std::chrono::steady_clock::now() >= deadline) { break; }
        sleep(poll);
    }
    if (present && (sawGone || out.path != oldPath)) { return Reenumeration::Returned; }
    return present ? Reenumeration::NeverLeft : Reenumeration::Gone;
}

}  // namespace cascade::source::fx2
