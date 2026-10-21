// png_decode.hpp - a PNG picture to 8-bit RGBA, for the plugin store's screenshots.
//
// WHY THIS EXISTS (0.99.72). The store's page for one plugin shows the pictures
// the catalogue names, and the application had no PNG decoder: map tiles reach
// the host as raw pixels from a basemap plugin, which does its own decoding.
// This is the one decoder, a small wrapper around the single public-domain
// header third_party/stb/stb_image.h, built in ONE translation unit
// (png_decode.cpp) with only the PNG reader compiled in.
//
// THE INPUT IS HOSTILE UNTIL PROVEN OTHERWISE. A catalogue picture is hashed
// against the digest the catalogue published, which proves the bytes are the
// ones the maintainer named and nothing about whether they are a picture, so a
// file that is not a PNG, one that is cut short, and one whose header claims a
// size no screen needs are all refused with a reason - and the last is refused
// FROM THE HEADER, before a single pixel is allocated (stbi_info_from_memory
// reads the 33 bytes of IHDR and nothing more).
//
// Pure data in, pure data out: no file, no GL, no ImGui. Thread-safe (stb keeps
// no global state here); the caller uploads the pixels.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PNG_DECODE_HPP
#define CASCADE_CORE_PNG_DECODE_HPP

#include <string>
#include <vector>

namespace cascade::core {

// A decoded picture: `width` x `height` pixels, four bytes each (R, G, B, A),
// rows top to bottom with no padding, so rgba.size() == width * height * 4.
struct PngImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

// Decodes `bytes` as a PNG. Returns true with `out` filled, or false with
// `error` (English, one short sentence; `out` is left empty) when:
//   - the bytes do not begin with the 8-byte PNG signature (a JPEG, a GIF, text,
//     an empty file) - checked here, before stb sees them;
//   - the header says either side is zero or over `maxSide` pixels, or the
//     header itself cannot be read - refused before any pixel memory is asked
//     for, so a 70000 x 70000 claim costs nothing;
//   - the data does not decode (cut short, damaged, an unsupported variant), with
//     the reason stb gave.
// `maxSide` below 1 refuses everything. 16-bit and palette pictures come back
// as 8-bit RGBA like any other.
bool decodePng(const std::vector<unsigned char>& bytes, int maxSide, PngImage& out,
               std::string& error);

}  // namespace cascade::core

#endif  // CASCADE_CORE_PNG_DECODE_HPP
