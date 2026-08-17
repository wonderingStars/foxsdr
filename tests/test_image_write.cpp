// The BMP writer, checked by reading the file back byte by byte.
//
// This exists because the writer shipped unverified: as a private method of
// AppWindow it could only be exercised by clicking a button. The three ways a
// BMP goes wrong - rows stored bottom-up, channels in B,G,R order, and rows
// padded to four bytes - all produce a file that OPENS and looks wrong rather
// than one that fails, so a glance at the output proves nothing and only
// reading the bytes back does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/image_write.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::vector<unsigned char> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
}

std::uint32_t le32(const std::vector<unsigned char>& d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | (static_cast<std::uint32_t>(d[at + 1]) << 8) |
           (static_cast<std::uint32_t>(d[at + 2]) << 16) |
           (static_cast<std::uint32_t>(d[at + 3]) << 24);
}
std::uint16_t le16(const std::vector<unsigned char>& d, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(d[at]) |
                                      (static_cast<std::uint16_t>(d[at + 1]) << 8));
}

// A width that is NOT a multiple of four, so row padding is exercised. With
// width 3 a row is 9 bytes and must be padded to 12; a writer that forgets
// shears the image by three bytes per row.
constexpr std::uint32_t kW = 3;
constexpr std::uint32_t kH = 2;

cascade::core::HostImage rgbFixture() {
    cascade::core::HostImage im;
    im.plugin = "test";
    im.width = kW;
    im.height = kH;
    im.format = CASCADE_IMAGE_RGB24;
    im.complete = true;
    im.pixels.assign(static_cast<std::size_t>(kW) * kH * 3u, 0u);
    // Distinct, asymmetric content. Top-left is pure RED and bottom-right is
    // pure BLUE: a B/R swap and a vertical flip each change which corner holds
    // which, and a symmetric pattern would hide both.
    auto put = [&im](std::uint32_t x, std::uint32_t y, unsigned char r, unsigned char g,
                     unsigned char b) {
        unsigned char* p = im.pixels.data() + (static_cast<std::size_t>(y) * kW + x) * 3u;
        p[0] = r;
        p[1] = g;
        p[2] = b;
    };
    put(0, 0, 255, 0, 0);    // top-left  RED
    put(2, 0, 0, 255, 0);    // top-right GREEN
    put(0, 1, 255, 255, 0);  // bottom-left YELLOW
    put(2, 1, 0, 0, 255);    // bottom-right BLUE
    return im;
}

}  // namespace

int main() {
    const fs::path dir =
        fs::temp_directory_path() / ("cascade_bmp_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / "img.bmp";

    // --- refusals --------------------------------------------------------
    {
        cascade::core::HostImage empty;
        std::string err;
        CHECK(!cascade::core::writeBmp24(empty, out.string(), err));
        CHECK(!err.empty());
    }
    {
        // Declared bigger than the buffer: writing it would read past the end.
        cascade::core::HostImage bad = rgbFixture();
        bad.pixels.resize(4);
        std::string err;
        CHECK(!cascade::core::writeBmp24(bad, out.string(), err));
        CHECK(!err.empty());
    }

    // --- encodeBmp24 must produce EXACTLY what writeBmp24 writes ----------
    // The web server serves the in-memory form. The two share the format code
    // precisely so the bottom-up row order and the B,G,R channel order cannot
    // drift apart; this is what proves they have not.
    {
        const cascade::core::HostImage fixture = rgbFixture();
        const fs::path ref = dir / "ref.bmp";
        std::string err;
        CHECK(cascade::core::writeBmp24(fixture, ref.string(), err));
        std::ifstream f(ref, std::ios::binary);
        const std::vector<std::uint8_t> onDisk(
            (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::uint8_t> inMemory;
        CHECK(cascade::core::encodeBmp24(fixture, inMemory, err));
        CHECK(err.empty());
        CHECK(!inMemory.empty());
        CHECK(inMemory == onDisk);

        // The same refusals, so a caller cannot get bytes out of an image the
        // writer would have rejected.
        cascade::core::HostImage empty;
        std::vector<std::uint8_t> nothing;
        CHECK(!cascade::core::encodeBmp24(empty, nothing, err));
        CHECK(nothing.empty());
        cascade::core::HostImage bad = rgbFixture();
        bad.pixels.resize(4);
        CHECK(!cascade::core::encodeBmp24(bad, nothing, err));
        CHECK(nothing.empty());
    }

    // --- a real write, read back byte by byte ----------------------------
    const cascade::core::HostImage im = rgbFixture();
    std::string err = "stale";
    CHECK(cascade::core::writeBmp24(im, out.string(), err));
    CHECK(err.empty());
    CHECK(fs::exists(out));

    const std::vector<unsigned char> d = readAll(out);
    const std::size_t rowBytes = kW * 3u;                 // 9
    const std::size_t stride = ((rowBytes + 3u) / 4u) * 4u;  // 12 - the padding
    CHECK(stride == 12u);
    CHECK(d.size() == 54u + stride * kH);

    // Header fields.
    CHECK(d[0] == 'B' && d[1] == 'M');
    CHECK(le32(d, 2) == d.size());
    CHECK(le32(d, 10) == 54u);        // pixel data offset
    CHECK(le32(d, 14) == 40u);        // BITMAPINFOHEADER
    CHECK(le32(d, 18) == kW);
    CHECK(le32(d, 22) == kH);         // positive => bottom-up
    CHECK(le16(d, 26) == 1u);         // planes
    CHECK(le16(d, 28) == 24u);        // bits per pixel
    CHECK(le32(d, 30) == 0u);         // BI_RGB
    CHECK(le32(d, 34) == stride * kH);

    // --- the three traps, checked individually ---------------------------
    const std::size_t base = 54u;
    auto pix = [&](std::uint32_t fileRow, std::uint32_t x) {
        return base + static_cast<std::size_t>(fileRow) * stride + x * 3u;
    };

    // BOTTOM-UP: file row 0 is the image's LAST row. The source bottom-left is
    // yellow, so if this reads red the rows were written top-down.
    {
        const std::size_t p = pix(0, 0);
        CHECK(d[p + 0] == 0);    // B
        CHECK(d[p + 1] == 255);  // G
        CHECK(d[p + 2] == 255);  // R   -> yellow
    }
    // ...and file row 1 is the image's FIRST row: pure red.
    {
        const std::size_t p = pix(1, 0);
        CHECK(d[p + 0] == 0);    // B
        CHECK(d[p + 1] == 0);    // G
        CHECK(d[p + 2] == 255);  // R
    }
    // B,G,R ORDER: bottom-right of the image is pure blue, which lands in file
    // row 0, x=2. Stored blue-first, so byte 0 is 255 and byte 2 is 0. If the
    // channels were written R,G,B this reads exactly backwards.
    {
        const std::size_t p = pix(0, 2);
        CHECK(d[p + 0] == 255);  // B
        CHECK(d[p + 1] == 0);
        CHECK(d[p + 2] == 0);
    }
    // PADDING: the three bytes after each 9-byte row must exist and be zero.
    for (std::uint32_t r = 0; r < kH; ++r) {
        for (std::size_t k = rowBytes; k < stride; ++k) {
            CHECK(d[base + static_cast<std::size_t>(r) * stride + k] == 0);
        }
    }

    // --- greyscale expands to grey, not to a colour cast -----------------
    {
        cascade::core::HostImage g;
        g.width = 2;
        g.height = 1;
        g.format = CASCADE_IMAGE_GRAY8;
        g.pixels = {40u, 200u};
        const fs::path gout = dir / "grey.bmp";
        std::string gerr;
        CHECK(cascade::core::writeBmp24(g, gout.string(), gerr));
        const std::vector<unsigned char> gd = readAll(gout);
        CHECK(gd.size() == 54u + 8u);  // 6 bytes of pixels padded to 8
        // Both channels of both pixels equal the source sample: any channel
        // differing would tint a monochrome weather image.
        CHECK(gd[54 + 0] == 40 && gd[54 + 1] == 40 && gd[54 + 2] == 40);
        CHECK(gd[54 + 3] == 200 && gd[54 + 4] == 200 && gd[54 + 5] == 200);
        fs::remove(gout, ec);
    }

    fs::remove(out, ec);
    fs::remove_all(dir, ec);
    return testSummary("test_image_write");
}
