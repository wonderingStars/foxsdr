// utf8_text.hpp - the few things the interface does to UTF-8 text by hand:
// step it a character at a time, cut it to a buffer without splitting a
// character, and put a translated word into capitals.
//
// WHY ONE FILE. Until the interface was translated every string it drew was
// ASCII, and "a byte" and "a character" were the same thing. Now a Polish
// "ł" is two bytes, and each place that stepped, cut or capitalised a string
// by the byte became a place that could draw half a letter - which Dear ImGui
// renders as a box or a question mark, on exactly the screens of the people
// the translation was made for. One implementation, tested once, used at every
// such place, is how that fault stays fixed.
//
// NOT A UNICODE LIBRARY. Nothing here normalises, collates or knows about
// scripts; it knows how UTF-8 is framed, and the capitals of the Latin
// letters the catalogues use (Latin-1 and Latin Extended-A). A malformed byte
// is treated as one character on its own, so bad input degrades to an odd
// glyph and never to a read past the end.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_UTF8_TEXT_HPP
#define CASCADE_CORE_UTF8_TEXT_HPP

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace cascade::core {

// The number of bytes in the character that starts at `p` (which is not the
// terminating NUL): 1 for ASCII, for a stray continuation byte, and for a
// lead byte whose sequence is broken off early - so a caller stepping by this
// always moves forward and never past a NUL.
inline std::size_t utf8CharLen(const char* p) {
    const auto c = static_cast<unsigned char>(p[0]);
    std::size_t len = 1;
    if (c >= 0xC0 && c < 0xE0) {
        len = 2;
    } else if (c >= 0xE0 && c < 0xF0) {
        len = 3;
    } else if (c >= 0xF0 && c < 0xF8) {
        len = 4;
    }
    for (std::size_t k = 1; k < len; ++k) {
        if ((static_cast<unsigned char>(p[k]) & 0xC0u) != 0x80u) { return 1; }
    }
    return len;
}

// The character after the one at `p`.
inline const char* utf8Next(const char* p) { return p + utf8CharLen(p); }

// How many characters `s` holds - the number of glyphs it draws as.
inline std::size_t utf8Count(const char* s) {
    std::size_t n = 0;
    for (const char* p = s; p != nullptr && *p != '\0'; p = utf8Next(p)) { ++n; }
    return n;
}

// The longest prefix of the first `n` bytes of `s` that does not end part way
// through a character. `n` itself when the byte after the prefix starts a new
// character (or there is none); otherwise the start of the character the cut
// would have split. This is what a buffer cut short must be cut back to.
inline std::size_t utf8Floor(const char* s, std::size_t n) {
    if (s == nullptr || n == 0) { return 0; }
    // Walk back over continuation bytes to the lead byte of the last
    // character that begins inside the prefix.
    std::size_t lead = n;
    while (lead > 0 && (static_cast<unsigned char>(s[lead - 1]) & 0xC0u) == 0x80u &&
           n - lead < 3) {
        --lead;
    }
    if (lead == 0) { return n; }  // nothing but continuation bytes: leave it
    const auto c = static_cast<unsigned char>(s[lead - 1]);
    std::size_t want = 1;
    if (c >= 0xC0 && c < 0xE0) {
        want = 2;
    } else if (c >= 0xE0 && c < 0xF0) {
        want = 3;
    } else if (c >= 0xF0 && c < 0xF8) {
        want = 4;
    }
    const std::size_t have = n - (lead - 1);
    return have < want ? lead - 1 : n;
}

// snprintf into a fixed buffer, and when the text does not fit, cut it back to
// the last whole character rather than wherever the buffer happened to end.
// Returns what snprintf returns - including snprintf's own answer for a null or
// zero-sized buffer, the length the text WOULD take, so a call that measures
// first behaves exactly as it did. For the status cards, chips and plates that
// print translated sentences into char arrays sized for English.
inline int formatUtf8(char* buf, std::size_t cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = std::vsnprintf(cap == 0 ? nullptr : buf, buf == nullptr ? 0 : cap, fmt, ap);
    va_end(ap);
    if (buf != nullptr && cap > 0 && r >= 0 && static_cast<std::size_t>(r) >= cap) {
        // Truncated: snprintf stopped at cap - 1 bytes wherever that fell.
        buf[utf8Floor(buf, cap - 1)] = '\0';
    }
    return r;
}

// Copies `src` into a fixed buffer, stopping at "##" (an ImGui id suffix,
// which is plumbing, not lettering) and never splitting a character when the
// buffer is too small. Returns the bytes copied.
inline std::size_t copyVisibleUtf8(char* dst, std::size_t cap, const char* src) {
    if (dst == nullptr || cap == 0) { return 0; }
    std::size_t n = 0;
    for (const char* p = src; p != nullptr && *p != '\0' && n + 1 < cap; ++p) {
        if (p[0] == '#' && p[1] == '#') { break; }
        dst[n++] = *p;
    }
    n = utf8Floor(dst, n);
    dst[n] = '\0';
    return n;
}

// One code point from `s` at `i`, advancing `i`; a malformed byte is returned
// as itself.
inline unsigned int utf8Decode(std::string_view s, std::size_t& i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const unsigned int c = b(i);
    std::size_t len = 1;
    unsigned int cp = c;
    if (c >= 0xC0 && c < 0xE0) {
        len = 2;
        cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c < 0xF0) {
        len = 3;
        cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c < 0xF8) {
        len = 4;
        cp = c & 0x07u;
    }
    if (len == 1 || i + len > s.size()) {
        ++i;
        return c;
    }
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0u) != 0x80u) {
            ++i;
            return c;
        }
        cp = (cp << 6) | (b(i + k) & 0x3Fu);
    }
    i += len;
    return cp;
}

inline void utf8Append(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Upper case for a key legend or a plate. The bench letters its keys in
// capitals, and std::toupper works on bytes: it leaves "é" alone at best and
// mangles half of it at worst. This covers the Latin-1 and Latin Extended-A
// letters that have a simple capital - enough for every catalogue's alphabet
// - and leaves anything else as written.
inline std::string upperLegend(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        unsigned int cp = utf8Decode(s, i);
        if (cp >= 'a' && cp <= 'z') {
            cp -= 0x20;
        } else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) {
            cp -= 0x20;
        } else if ((cp >= 0x100 && cp <= 0x12F) || (cp >= 0x132 && cp <= 0x137) ||
                   (cp >= 0x14A && cp <= 0x177)) {
            cp &= ~1u;  // pairs start on an even code point: capital, small
        } else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) {
            if ((cp & 1u) == 0u) { cp -= 1; }  // pairs start on an odd one
        }
        utf8Append(out, cp);
    }
    return out;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_UTF8_TEXT_HPP
