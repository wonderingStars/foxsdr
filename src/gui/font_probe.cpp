// font_probe.cpp - see font_probe.hpp.
//
// A PRIVATE stb_truetype. imgui_draw.cpp compiles the same header with
// STBTT_STATIC, so its functions are invisible outside that file; this file
// does the same, and the two copies cannot collide at link time. It is the
// vendored header unmodified - the same parser ImGui rasterises with, so
// "this face has the glyph" here and "ImGui draws the glyph" agree.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/font_probe.hpp"

#include <cstring>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4244 4456 4457 4702)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace cascade::gui::fontprobe {
namespace {

// The smallest file that can hold an sfnt header and one table record. Below
// it stb_truetype would read past the end, and it has no length to check.
constexpr std::size_t kMinFont = 12 + 16;

bool open(const Face& f, stbtt_fontinfo& info) {
    if (f.data == nullptr || f.len < kMinFont || f.index < 0) { return false; }
    if (f.index >= faceCount(f.data, f.len)) { return false; }
    const int offset = stbtt_GetFontOffsetForIndex(f.data, f.index);
    if (offset < 0 || static_cast<std::size_t>(offset) + kMinFont > f.len) { return false; }
    return stbtt_InitFont(&info, f.data, offset) != 0;
}

// A name record's bytes as UTF-8: platform 3 records are UTF-16BE, platform
// 1 records Mac Roman (only its ASCII half matters for matching a hint).
std::string decodeName(const char* s, int len, bool utf16) {
    std::string out;
    if (s == nullptr || len <= 0) { return out; }
    if (!utf16) {
        for (int i = 0; i < len; ++i) {
            const auto c = static_cast<unsigned char>(s[i]);
            out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return out;
    }
    for (int i = 0; i + 1 < len; i += 2) {
        const unsigned int u = (static_cast<unsigned char>(s[i]) << 8) | static_cast<unsigned char>(s[i + 1]);
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

}  // namespace

int faceCount(const unsigned char* data, std::size_t len) {
    if (data == nullptr || len < kMinFont) { return 0; }
    const int n = stbtt_GetNumberOfFonts(data);
    return n > 0 ? n : 0;
}

std::vector<std::string> names(const Face& f) {
    std::vector<std::string> out;
    stbtt_fontinfo info;
    if (!open(f, info)) { return out; }
    // English (United States) on the Windows platform, then the Mac Roman
    // record, which is where some older faces keep their only English name.
    struct Rec {
        int platform, encoding, language;
        bool utf16;
    };
    constexpr Rec kRecs[] = {{3, 1, 0x409, true}, {1, 0, 0, false}};
    for (const int nameId : {1, 4, 16}) {
        for (const Rec& r : kRecs) {
            int len = 0;
            const char* s = stbtt_GetFontNameString(&info, &len, r.platform, r.encoding, r.language, nameId);
            if (s != nullptr && len > 0) {
                out.push_back(decodeName(s, len, r.utf16));
                break;
            }
        }
    }
    return out;
}

bool has(const Face& f, unsigned int cp) {
    stbtt_fontinfo info;
    return open(f, info) && stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) != 0;
}

std::size_t missing(const Face& f, const std::vector<unsigned int>& cps, unsigned int* first) {
    stbtt_fontinfo info;
    if (!open(f, info)) {
        if (first != nullptr && !cps.empty()) { *first = cps.front(); }
        return cps.size();
    }
    std::size_t n = 0;
    for (const unsigned int cp : cps) {
        if (stbtt_FindGlyphIndex(&info, static_cast<int>(cp)) == 0) {
            if (n == 0 && first != nullptr) { *first = cp; }
            ++n;
        }
    }
    return n;
}

float emSpan(const Face& f) {
    stbtt_fontinfo info;
    if (!open(f, info)) { return 0.0f; }
    // ScaleForPixelHeight is 1 / (ascent - descent) in font units, which is
    // exactly how ImGui sizes a face; ScaleForMappingEmToPixels is 1 / em.
    const float perHeight = stbtt_ScaleForPixelHeight(&info, 1.0f);
    const float perEm = stbtt_ScaleForMappingEmToPixels(&info, 1.0f);
    return perHeight > 0.0f ? perEm / perHeight : 0.0f;
}

int pickFace(const unsigned char* data, std::size_t len, std::string_view hint,
             const std::vector<unsigned int>& probe) {
    const int n = faceCount(data, len);
    // Three preferences, best first: a name that IS the hint, a name that
    // contains it, any face with the probe characters. The first matters for
    // a collection whose Japanese face is plain "Source Han Sans" beside
    // "Source Han Sans SC" and "... K" - containment alone would take whichever
    // came first.
    int exact = -1;
    int contains = -1;
    int anyCovering = -1;
    for (int i = 0; i < n; ++i) {
        const Face f{data, len, i};
        if (missing(f, probe) != 0) { continue; }
        if (anyCovering < 0) { anyCovering = i; }
        if (hint.empty()) { break; }
        for (const std::string& name : names(f)) {
            if (exact < 0 && name == hint) { exact = i; }
            if (contains < 0 && name.find(hint) != std::string::npos) { contains = i; }
        }
    }
    if (exact >= 0) { return exact; }
    if (contains >= 0) { return contains; }
    return anyCovering;
}

}  // namespace cascade::gui::fontprobe
