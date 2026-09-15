// rx888_firmware.hpp - the SDDC FX3 firmware image FoxSDR carries, and the
// parser for the Cypress image format it is in.
//
// WHY A DRIVER SHIPS A FIRMWARE AT ALL. An RX888 has no flash. Out of a power
// cycle the FX3 runs its on-chip bootloader, enumerates as 04B4:00F3 and can
// do exactly one thing: accept a RAM image over the control endpoint and jump
// to it. Until something does that, the radio is not a radio - it has no
// bulk endpoint, no ADC clock, no tuner, and none of the vendor requests in
// rx888_protocol.hpp exist. Every host application for this hardware
// therefore carries the image, and so does this one.
//
// THE IMAGE IS SDDC_FX3.img FROM ExtIO_sddc BY OSCAR STEILA (IK1XPV), MIT,
// byte for byte as that project publishes it, embedded the same way ExtIO
// embeds it (its firmware.h). Its SHA-256 and length are pinned in
// tests/test_rx888_source.cpp, so a re-generation that changed the bytes
// fails rather than shipping quietly. The licence text is reproduced in
// installer/THIRD-PARTY-LICENSES.txt under COMPONENT: ExtIO_sddc.
//
// THE FORMAT, which is Cypress's and is documented in AN76405 "EZ-USB FX3
// Boot Options". It was verified against the actual bytes of the image this
// file carries rather than taken on trust:
//
//   offset 0  'C' 'Y'                     the signature
//   offset 2  imageCTL                    (0x1C in this image)
//   offset 3  imageType                   0xB0, "normal FW binary with
//                                         checksum", the only type accepted
//   then, repeatedly:
//     uint32 length    in 32-BIT WORDS, not bytes
//     uint32 address   where the words go
//     length*4 bytes   the data
//   terminated by a record whose length is 0, whose address field is the
//   PROGRAM ENTRY POINT, followed by
//   uint32 checksum    the sum of every data word, modulo 2^32
//
// The shipped image has four records. The first is 0x8E0 words at
// 0x00000100 - the FX3's I-TCM - beginning `10 40 2d e9`, which is an ARM
// `push {r4, lr}`; the next two are the bulk of the code in SYSMEM at
// 0x40003000 and 0x40013000. Its last twelve bytes are
// `00000000 fc2c0140 3068b1e5`: length zero, entry 0x40012CFC, checksum
// 0xE5B16830, and the computed sum of all 146220 data bytes agrees with it.
// All of that is asserted in the test.
//
// THE UPLOAD, for which the same document is the authority: vendor request
// 0xA0 OUT, the destination address split as wValue = address & 0xFFFF and
// wIndex = address >> 16, at most 4096 bytes of payload per transfer, the
// address advancing by the chunk. When every record has been written, one
// more 0xA0 with NO DATA and the entry point in wValue/wIndex makes the chip
// jump to it - at which point it drops off the bus and comes back as a
// different device. (Corroborated by, but not taken from, the fxload-derived
// ezusb.c in the same pack, which is GPL-2+ and therefore not a source this
// project may copy: see the licence note at the top of rx888_protocol.hpp.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::source::rx888 {

// The most payload one 0xA0 transfer may carry (AN76405). The chunking is
// visible on the wire, so the test asserts it.
constexpr std::size_t kFx3UploadChunkBytes = 4096;

// The Cypress vendor request the bootloader implements, and nothing else.
constexpr std::uint8_t kFx3BootRequest = 0xA0;

// One contiguous run of the image, as the file lays it out. `data` points
// into the caller's buffer and is valid for as long as that buffer is.
struct Fx3Section {
    std::uint32_t address = 0;
    const std::uint8_t* data = nullptr;
    std::size_t bytes = 0;
};

struct Fx3Image {
    bool valid = false;
    // Empty when valid. Written for a log line a user might be asked to
    // quote, so it says which check failed and what it saw.
    std::string error;
    std::vector<Fx3Section> sections;
    std::uint32_t entry = 0;
    std::uint32_t storedChecksum = 0;    // the word the file ends with
    std::uint32_t computedChecksum = 0;  // the sum of every data word
    std::size_t totalBytes = 0;          // across all sections
};

// Parses a Cypress boot image. Never throws and never reads past `size`: a
// truncated image is a named error, not a crash, because the thing being
// parsed here is the only reason a 128 MB/s bulk endpoint ever appears.
Fx3Image parseFx3Image(const std::uint8_t* data, std::size_t size);

// The embedded SDDC image.
const std::uint8_t* sddcFirmwareImage();
std::size_t sddcFirmwareImageSize();

}  // namespace cascade::source::rx888
