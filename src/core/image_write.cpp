// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/image_write.hpp"

#include <fstream>
#include <vector>

namespace cascade::core {

bool writeBmp24(const HostImage& img, const std::string& path, std::string& error) {
    error.clear();
    if (img.width == 0 || img.height == 0 || img.pixels.empty()) {
        error = "there is no image to save yet";
        return false;
    }
    const std::size_t srcBpp = (img.format == CASCADE_IMAGE_RGB24) ? 3u : 1u;
    const std::size_t needed =
        static_cast<std::size_t>(img.width) * img.height * srcBpp;
    if (img.pixels.size() < needed) {
        // Refused rather than trusted: the buffer is smaller than the declared
        // dimensions, and writing it would read past the end.
        error = "image buffer is smaller than its declared size";
        return false;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(img.width) * 3u;
    // BMP pads every row to a multiple of four bytes. Omitting this produces a
    // file that opens and looks progressively sheared - the classic way to
    // write a "working" BMP that is subtly wrong.
    const std::size_t pad = (4u - (rowBytes % 4u)) % 4u;
    const std::size_t stride = rowBytes + pad;
    const std::size_t pixelBytes = stride * img.height;
    const std::uint32_t headerBytes = 14u + 40u;

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open \"" + path + "\" for writing";
        return false;
    }

    auto u16 = [&f](std::uint16_t v) {
        const unsigned char b[2] = {static_cast<unsigned char>(v & 0xFFu),
                                    static_cast<unsigned char>((v >> 8) & 0xFFu)};
        f.write(reinterpret_cast<const char*>(b), 2);
    };
    auto u32 = [&f](std::uint32_t v) {
        const unsigned char b[4] = {static_cast<unsigned char>(v & 0xFFu),
                                    static_cast<unsigned char>((v >> 8) & 0xFFu),
                                    static_cast<unsigned char>((v >> 16) & 0xFFu),
                                    static_cast<unsigned char>((v >> 24) & 0xFFu)};
        f.write(reinterpret_cast<const char*>(b), 4);
    };

    f.write("BM", 2);
    u32(static_cast<std::uint32_t>(headerBytes + pixelBytes));
    u16(0);
    u16(0);
    u32(headerBytes);
    u32(40);  // BITMAPINFOHEADER
    u32(img.width);
    u32(img.height);  // positive height = rows stored bottom-up
    u16(1);
    u16(24);
    u32(0);  // BI_RGB, uncompressed
    u32(static_cast<std::uint32_t>(pixelBytes));
    u32(2835);  // ~72 dpi
    u32(2835);
    u32(0);
    u32(0);

    // Rows BOTTOM-UP and channels B,G,R. Both are easy to forget and both give
    // a file that opens looking wrong rather than one that fails.
    std::vector<unsigned char> row(stride, 0);
    for (std::uint32_t yy = 0; yy < img.height; ++yy) {
        const std::uint32_t y = img.height - 1u - yy;
        const unsigned char* src =
            img.pixels.data() + static_cast<std::size_t>(y) * img.width * srcBpp;
        for (std::uint32_t x = 0; x < img.width; ++x) {
            unsigned char r, g, b;
            if (srcBpp == 3u) {
                r = src[x * 3 + 0];
                g = src[x * 3 + 1];
                b = src[x * 3 + 2];
            } else {
                r = g = b = src[x];
            }
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        for (std::size_t i = rowBytes; i < stride; ++i) { row[i] = 0; }
        f.write(reinterpret_cast<const char*>(row.data()),
                static_cast<std::streamsize>(stride));
    }
    f.flush();
    if (!f) {
        error = "writing \"" + path + "\" failed part way through";
        return false;
    }
    return true;
}

}  // namespace cascade::core
