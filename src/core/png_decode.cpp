// png_decode.cpp - see png_decode.hpp. The ONE translation unit that compiles
// third_party/stb/stb_image.h, with the PNG reader and nothing else.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

// What is compiled in: PNG only (no JPEG, GIF, BMP, TGA, PSD, HDR or PNM reader
// is built, so a hostile file of any of those is refused by name before it gets
// near a parser), and nothing that touches the file system - the caller hands
// over bytes. The failure strings stay ON (STBI_NO_FAILURE_STRINGS is not
// defined): the store shows the reason when a picture will not decode.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#include "core/png_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace cascade::core {

namespace {

// The eight bytes every PNG begins with (RFC 2083, 12.12).
constexpr unsigned char kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

std::string reasonOr(const char* stb, const char* fallback) {
    return (stb != nullptr && stb[0] != '\0') ? std::string(stb) : std::string(fallback);
}

}  // namespace

bool decodePng(const std::vector<unsigned char>& bytes, int maxSide, PngImage& out,
               std::string& error) {
    out = PngImage{};
    error.clear();

    if (maxSide < 1) {
        error = "no picture size is allowed";
        return false;
    }
    // THE SIGNATURE, CHECKED HERE. stb would refuse a JPEG too (its other readers
    // are compiled out), but "not a PNG" is the one sentence a person can act on,
    // and it must not depend on which readers a build happens to contain.
    if (bytes.size() < sizeof kPngSignature ||
        std::memcmp(bytes.data(), kPngSignature, sizeof kPngSignature) != 0) {
        error = "not a PNG picture";
        return false;
    }
    // stb takes the length as an int.
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        error = "the picture file is too large";
        return false;
    }
    const int len = static_cast<int>(bytes.size());

    // THE HEADER FIRST: IHDR's two numbers, read without allocating anything. A
    // hostile file claims 70000 x 70000 in 33 bytes; refusing it here is what
    // keeps that claim from ever becoming a buffer.
    //
    // READ HERE TOO, by hand, from the first chunk (length 13, "IHDR", then width and height
    // as big-endian 32-bit numbers), so that the answer to "too big" is this wrapper's and
    // does not depend on where the library's own limits happen to sit: stb refuses a header
    // whose pixels would need over a gigabyte with its own words, and would accept one 4097
    // pixels wide without being asked.
    if (bytes.size() >= 24 && std::memcmp(bytes.data() + 12, "IHDR", 4) == 0) {
        const auto be32 = [&](std::size_t at) {
            return (static_cast<std::uint32_t>(bytes[at]) << 24) |
                   (static_cast<std::uint32_t>(bytes[at + 1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[at + 2]) << 8) | static_cast<std::uint32_t>(bytes[at + 3]);
        };
        const std::uint32_t hw = be32(16);
        const std::uint32_t hh = be32(20);
        if (hw == 0u || hh == 0u) {
            error = "the picture has no pixels";
            return false;
        }
        if (hw > static_cast<std::uint32_t>(maxSide) || hh > static_cast<std::uint32_t>(maxSide)) {
            error = "the picture is larger than this window will draw";
            return false;
        }
    }
    int w = 0;
    int h = 0;
    int comp = 0;
    if (stbi_info_from_memory(bytes.data(), len, &w, &h, &comp) == 0) {
        error = reasonOr(stbi_failure_reason(), "the picture header cannot be read");
        return false;
    }
    if (w < 1 || h < 1) {
        error = "the picture has no pixels";
        return false;
    }
    if (w > maxSide || h > maxSide) {
        error = "the picture is larger than this window will draw";
        return false;
    }

    // Four channels asked for, whatever the file has: grey, grey+alpha, RGB,
    // palette and 16-bit all come back as 8-bit RGBA.
    int dw = 0;
    int dh = 0;
    int dc = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), len, &dw, &dh, &dc, 4);
    if (px == nullptr) {
        error = reasonOr(stbi_failure_reason(), "the picture cannot be decoded");
        return false;
    }
    // The header and the decode must agree; if they do not the file is lying
    // about itself and nothing it says is believed.
    if (dw != w || dh != h) {
        stbi_image_free(px);
        error = "the picture header and its data disagree";
        return false;
    }
    const std::size_t n = static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 4u;
    out.width = dw;
    out.height = dh;
    out.rgba.assign(px, px + n);
    stbi_image_free(px);
    return true;
}

}  // namespace cascade::core
