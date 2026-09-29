// fx2_loader.hpp - loading RAM firmware into a Cypress EZ-USB FX2 over its
// control endpoint, from an Intel-HEX file.
//
// WHY THIS EXISTS. The AOR digital-I/Q interface is an FX2 with VOLATILE
// firmware: every time it loses power it comes back without the program
// that makes it stream, and the host has to put it back (AOR developer
// document, "FX2 firmware"). AOR supplies that program as fx2fw.hex.
//
// THE SOURCES, and only these. The load mechanism is the FX2's own, built
// into its USB core and documented in Cypress's "EZ-USB FX2 Technical
// Reference Manual": vendor request 0xA0 ("Firmware Load") writes the data
// stage of a vendor OUT control transfer into on-chip RAM at the address in
// wValue; the 8051 is held in reset by writing 1 to the CPUCS register at
// 0xE600 (its bit 0, 8051RES) through the same request, and released by
// writing 0. The file format is Intel's published Intel-HEX. No other
// loader's source (fxload, cycfx2prog, SoapyAOR) was read or copied.
//
// WHAT IS REFUSED, deliberately strictly, because a half-understood image
// written into a receiver's RAM is worse than one refused with a sentence:
// a record whose checksum does not add up, a record of a type this loader
// does not know, a truncated line or file (no end-of-file record), data past
// the FX2's 16-bit address space or overlapping earlier data, data that
// writes CPUCS itself (it would release the CPU half way through the load),
// and anything after the end-of-file record. Start-address records (types
// 03 and 05) are accepted and ignored: after reset the 8051 starts at 0
// whatever the file says.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::source::fx2 {

// EZ-USB FX2 Technical Reference Manual: the Firmware Load vendor request and
// the CPU control register it can reach.
constexpr std::uint8_t kFirmwareLoadRequest = 0xA0;
constexpr std::uint16_t kCpucsAddress = 0xE600;
constexpr std::uint8_t kCpucsHold = 0x01;     // 8051RES = 1: CPU held in reset
constexpr std::uint8_t kCpucsRelease = 0x00;  // 8051RES = 0: CPU runs from 0

// The most bytes one 0xA0 transfer carries. A CHOICE, not a device limit:
// 64 bytes is one maximum-size control packet at full and high speed, so no
// transfer relies on a multi-packet data stage into the load request. A few
// hundred transfers for a typical FX2 image, each bounded by its timeout.
constexpr std::size_t kLoadChunkBytes = 64;

// A contiguous run of bytes at a 16-bit FX2 address.
struct HexSegment {
    std::uint16_t address = 0;
    std::vector<std::uint8_t> bytes;
};

struct HexImage {
    bool valid = false;
    // Empty when valid; otherwise which line and which check, in words.
    std::string error;
    // In ascending address order, adjacent runs merged.
    std::vector<HexSegment> segments;
    std::size_t totalBytes = 0;
    std::size_t dataRecords = 0;
};

// Parses Intel-HEX text. Never throws; accepts LF or CRLF line ends and
// blank lines.
HexImage parseIntelHex(const std::string& text);

// Reads and parses a file. `found` is false when the file does not exist or
// cannot be read (so a caller can say "missing" rather than "broken").
HexImage readIntelHexFile(const std::string& path, bool& found);

// Loads `image` into the FX2 on `dev`: CPUCS hold, the image in ascending
// address order in chunks of at most kLoadChunkBytes, CPUCS release - every
// step a vendor OUT (0x40) request 0xA0 with the address in wValue and 0 in
// wIndex, each bounded by `timeoutMs`. Returns false with `error` set when
// any transfer is refused or short. ON A FAILED WRITE THE CPU IS LEFT HELD,
// on purpose: releasing it would run a partial program.
bool loadImage(cascade::usb::UsbDevice& dev, const HexImage& image, unsigned timeoutMs,
               std::string& error);

// --- waiting for the device to come back ------------------------------------
//
// After CPUCS is released the new program may disconnect and re-enumerate
// ("renumerate") - or it may not; the AOR document says only to wait for the
// device to be ready. So the wait watches the transport's list and decides
// from what it sees, bounded throughout:
//
//   Returned   the device was seen GONE and then back, or came back under a
//              different path (usbfs gives a re-enumerated device a new
//              device number) - after at least `settle` in either case;
//   NeverLeft  the budget ran out with the device listed the whole time, at
//              its old path - the firmware did not re-enumerate, or did it
//              faster than one poll; the caller has to find out by trying it;
//   Gone       the budget ran out with the device absent.
//
// `list` returns the matching devices; `sleep` waits one poll interval (a
// test passes a no-op). At most budget / poll + 1 lookups are made, and a
// real-clock deadline of `budget` bounds it as well.
enum class Reenumeration { Returned, NeverLeft, Gone };

struct ReenumerationTiming {
    std::chrono::milliseconds settle{1000};
    std::chrono::milliseconds budget{5000};
    std::chrono::milliseconds poll{100};
};

Reenumeration awaitReenumeration(
    const std::string& oldPath,
    const std::function<std::vector<cascade::usb::UsbDeviceInfo>()>& list,
    const std::function<void(std::chrono::milliseconds)>& sleep, const ReenumerationTiming& timing,
    cascade::usb::UsbDeviceInfo& out);

}  // namespace cascade::source::fx2
