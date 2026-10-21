// core/png_decode: the PNG decoder behind the plugin store's screenshots.
//
// The application had no PNG decoder (map tiles come from a plugin as raw
// pixels), so 0.99.72 vendors stb_image.h, compiles only its PNG reader, and
// wraps it in decodePng(). What the store feeds it is a file the catalogue
// hashed, which proves the bytes are the maintainer's and nothing about whether
// they are a picture, so the wrapper's refusals are the product:
//   - a 2 x 2 RGBA picture written out byte for byte decodes to its four pixels;
//   - a file cut short is refused, whether the cut is in the image data or takes
//     the whole end-of-file chunk;
//   - something that is not a PNG (a JPEG, text, nothing) is refused by its
//     signature, before the decoder sees it;
//   - a header that claims 70000 x 70000 is refused FROM THE HEADER, with no
//     pixel data in the file at all - what proves nothing was allocated for it;
//   - a picture over the caller's maxSide is refused, one at it is not;
//   - and the repository's own screenshot decodes to its real size.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/png_decode.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::decodePng;
using cascade::core::PngImage;

namespace {

// A 2 x 2 RGBA picture, written by a PNG encoder outside this tree (Python's
// zlib and crc32) so the test does not depend on the decoder to make its input:
//   (255,0,0,255)   (0,255,0,128)
//   (0,0,255,255)   (10,20,30,0)
const std::vector<unsigned char> kTwoByTwo = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
    0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x17, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8,
    0xcf, 0xc0, 0xf0, 0x1f, 0x08, 0x1b, 0x18, 0x80, 0xf4, 0x7f, 0x2e, 0x11, 0x39, 0x06, 0x00,
    0x3b, 0x7c, 0x05, 0xb8, 0xfb, 0x73, 0xdc, 0x62, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
    0x44, 0xae, 0x42, 0x60, 0x82};

// Signature and an IHDR chunk (correct CRC) claiming 70000 x 70000, 8-bit RGBA,
// and NOTHING after it: there is no pixel data to decode, and the decoder must
// say no from these 33 bytes.
const std::vector<unsigned char> kHugeHeader = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x01, 0x11, 0x70, 0x00, 0x01, 0x11, 0x70, 0x08, 0x06, 0x00, 0x00, 0x00, 0x3f,
    0x3e, 0x34, 0xcb};

std::vector<unsigned char> prefix(const std::vector<unsigned char>& v, std::size_t n) {
    return std::vector<unsigned char>(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
}

void testTwoByTwoDecodesToItsFourPixels() {
    PngImage img;
    std::string err = "stale";
    CHECK(decodePng(kTwoByTwo, 4096, img, err));
    CHECK(err.empty());
    CHECK(img.width == 2);
    CHECK(img.height == 2);
    const std::vector<unsigned char> want = {255, 0,  0,  255,  //
                                             0,   255, 0, 128,  //
                                             0,   0,  255, 255,  //
                                             10,  20, 30, 0};
    CHECK(img.rgba == want);
    // At exactly maxSide it is allowed: the limit is "over", not "at".
    PngImage at;
    CHECK(decodePng(kTwoByTwo, 2, at, err));
    CHECK(at.width == 2 && at.height == 2);
}

void testTruncationIsRefused() {
    PngImage img;
    std::string err;
    // Cut inside the image data: the chunk says 23 bytes and fewer arrive.
    CHECK(!decodePng(prefix(kTwoByTwo, kTwoByTwo.size() - 20), 4096, img, err));
    CHECK(!err.empty());
    CHECK(img.rgba.empty());
    CHECK(img.width == 0 && img.height == 0);
    // Cut so the whole end-of-file chunk is gone, but every data byte is there.
    err.clear();
    CHECK(!decodePng(prefix(kTwoByTwo, kTwoByTwo.size() - 12), 4096, img, err));
    CHECK(!err.empty());
    // Cut right after the header.
    err.clear();
    CHECK(!decodePng(prefix(kTwoByTwo, 33), 4096, img, err));
    CHECK(!err.empty());
    // Cut inside the signature.
    err.clear();
    CHECK(!decodePng(prefix(kTwoByTwo, 5), 4096, img, err));
    CHECK(!err.empty());
}

void testNotAPngIsRefusedBySignature() {
    PngImage img;
    std::string err;
    // A JPEG's start of image and a JFIF header.
    const std::vector<unsigned char> jpeg = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J',
                                             'F',  'I',  'F',  0x00, 0x01, 0x01, 0x00};
    CHECK(!decodePng(jpeg, 4096, img, err));
    CHECK(err == "not a PNG picture");
    // Text, a GIF and nothing at all.
    err.clear();
    const std::string text = "<html>this is not a picture</html>";
    CHECK(!decodePng(std::vector<unsigned char>(text.begin(), text.end()), 4096, img, err));
    CHECK(err == "not a PNG picture");
    err.clear();
    const std::string gif = "GIF89a";
    CHECK(!decodePng(std::vector<unsigned char>(gif.begin(), gif.end()), 4096, img, err));
    CHECK(err == "not a PNG picture");
    err.clear();
    CHECK(!decodePng(std::vector<unsigned char>(), 4096, img, err));
    CHECK(err == "not a PNG picture");
    // The signature alone is not a picture either, and is not the signature's fault.
    err.clear();
    CHECK(!decodePng(prefix(kTwoByTwo, 8), 4096, img, err));
    CHECK(!err.empty() && err != "not a PNG picture");
    // One wrong byte of the signature (a PNG that went through a text-mode
    // transfer loses its 0x0d 0x0a pair this way).
    std::vector<unsigned char> bad = kTwoByTwo;
    bad[4] = 0x0a;
    err.clear();
    CHECK(!decodePng(bad, 4096, img, err));
    CHECK(err == "not a PNG picture");
}

void testAnOversizeHeaderIsRefusedBeforeDecoding() {
    PngImage img;
    std::string err;
    // 70000 x 70000 with no data after the header. If the decoder tried to decode
    // it, the failure would be "out of data" or an allocation of 19.6 GB; the
    // reason here is the size, which only the header check can give.
    CHECK(!decodePng(kHugeHeader, 4096, img, err));
    CHECK(err == "the picture is larger than this window will draw");
    CHECK(img.rgba.empty());
    // The bound is the caller's: 70000 passes a 70000 limit's SIZE check (it
    // then fails for want of data, which is a different reason).
    err.clear();
    CHECK(!decodePng(kHugeHeader, 70000, img, err));
    CHECK(!err.empty());
    CHECK(err != "the picture is larger than this window will draw");
    // A one-pixel limit refuses the 2 x 2 picture; a limit below one refuses all.
    err.clear();
    CHECK(!decodePng(kTwoByTwo, 1, img, err));
    CHECK(err == "the picture is larger than this window will draw");
    err.clear();
    CHECK(!decodePng(kTwoByTwo, 0, img, err));
    CHECK(!err.empty());
}

fs::path findRepoRoot() {
    fs::path dir = fs::current_path();
    for (int level = 0; level < 12; ++level) {
        // A fresh error code each time round: a file that is not there sets one on some
        // standard libraries, and that must not end the walk at the first level.
        std::error_code ec1;
        std::error_code ec2;
        if (fs::is_regular_file(dir / "PRIVACY.md", ec1) && fs::is_directory(dir / "src", ec2)) {
            return dir;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

void testTheRepositoryScreenshotDecodes() {
    const fs::path root = findRepoRoot();
    CHECK(!root.empty());
    if (root.empty()) { return; }
    const fs::path file = root / "docs" / "screenshots" / "instrument-windows.png";
    std::ifstream in(file, std::ios::binary);
    CHECK(in.good());
    if (!in.good()) { return; }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
    CHECK(bytes.size() > 100000u);
    PngImage img;
    std::string err;
    CHECK(decodePng(bytes, 4096, img, err));
    std::printf("  instrument-windows.png: %d x %d, %zu bytes decoded (%s)\n", img.width,
                img.height, img.rgba.size(), err.c_str());
    CHECK(img.width == 1305);
    CHECK(img.height == 992);
    CHECK(img.rgba.size() == 1305u * 992u * 4u);
    // The picture is opaque screen capture: every alpha byte is 255.
    bool opaque = true;
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) {
        if (img.rgba[i] != 255) {
            opaque = false;
            break;
        }
    }
    CHECK(opaque);
    // And the same picture refused at a limit under its width.
    PngImage small;
    CHECK(!decodePng(bytes, 1304, small, err));
    CHECK(small.rgba.empty());
}

}  // namespace

int main() {
    testTwoByTwoDecodesToItsFourPixels();
    testTruncationIsRefused();
    testNotAPngIsRefusedBySignature();
    testAnOversizeHeaderIsRefusedBeforeDecoding();
    testTheRepositoryScreenshotDecodes();
    return testSummary("test_png_decode");
}
