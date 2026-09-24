// utf8_text.hpp - the few things the interface does to UTF-8 text by hand:
// step it a character at a time, cut it to a buffer without splitting a
// character, format a translated sentence whole, and put a translated word
// into capitals.
//
// WHY ONE FILE. Until the interface was translated every string it drew was
// ASCII, and "a byte" and "a character" were the same thing. Now a Polish
// "ł" is two bytes, and each place that stepped, cut or capitalised a string
// by the byte became a place that could draw half a letter - which Dear ImGui
// renders as a box or a question mark, on exactly the screens of the people
// the translation was made for. One implementation, tested once, used at every
// such place, is how that fault stays fixed.
//
// NOT A UNICODE LIBRARY. Nothing here normalises or collates; it knows how
// UTF-8 is framed, and the capitals of the alphabets the catalogues are
// written in (Latin with its extensions and Vietnamese, Greek, Cyrillic), with
// the two capitalisation rules those need beyond a table: Turkish keeps the
// dot on its i, and Greek set in capitals drops its accents. A malformed byte
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
#include <utility>

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
// first behaves exactly as it did. Only for places that genuinely need a fixed
// array (an ImGui::InputText edit buffer, a struct that must stay trivially
// copyable); a translated sentence goes through the std::string form below,
// which is never cut - test_i18n fails the build's suite on a tr() here.
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

// THE WHOLE SENTENCE, NEVER CUT. printf into a std::string sized by the text
// itself, for every line that carries a translation. A fixed buffer is sized
// by whoever wrote the English: "RUNNING" fits a 16-byte chip, and the Russian
// for it does not, because Cyrillic, Greek and Vietnamese take two bytes a
// letter and Chinese three. Cutting on a character (the fixed-buffer form
// above) keeps the glyphs whole, but a sentence without its end is still a
// wrong sentence; how much of it fits on screen is text_fit.hpp's decision,
// made in pixels, not a byte count's.
//
// Measures first on a copy of the arguments, then writes. The result is built
// in a local string and moved into `out` only when complete, so `out` may be
// one of the arguments (formatUtf8(s, "%s, %d", s.c_str(), n) appends). Returns
// what vsnprintf returns; on an encoding error (negative) `out` is left empty.
// A null format is an empty one.
inline int vformatUtf8(std::string& out, const char* fmt, va_list ap) {
    if (fmt == nullptr) {
        out.clear();
        return 0;
    }
    va_list measure;
    va_copy(measure, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    if (n <= 0) {
        out.clear();
        return n;
    }
    std::string text(static_cast<std::size_t>(n), '\0');
    // n characters and the NUL, which lands on text[n] - the terminator
    // std::string always keeps, and writing '\0' there is allowed.
    std::vsnprintf(text.data(), static_cast<std::size_t>(n) + 1, fmt, ap);
    out = std::move(text);
    return n;
}

// The same, variadic: formatUtf8(line, tr("%d TRACKED"), n).
inline int formatUtf8(std::string& out, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vformatUtf8(out, fmt, ap);
    va_end(ap);
    return r;
}

// The same, returned: draw(formatText(tr("EVENT %u"), seq).c_str()). Empty on
// an encoding error.
inline std::string formatText(const char* fmt, ...) {
    std::string out;
    va_list ap;
    va_start(ap, fmt);
    vformatUtf8(out, fmt, ap);
    va_end(ap);
    return out;
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

// The simple capital of one code point, or the code point itself when it has
// none (or is already one). Covers the alphabets the catalogues are written
// in: Latin-1, Latin Extended-A and -B, Latin Extended Additional (the
// Vietnamese vowels with two marks), Greek, Cyrillic and its supplement. NOT
// language-sensitive and NOT Greek's all-caps accent rule - upperLegend()
// below adds both. Han, kana and Hangul have no case and pass through.
inline unsigned int upperSimple(unsigned int cp) {
    // Pairs laid out capital-then-small from an EVEN code point, and from an
    // ODD one - the two layouts Unicode's Latin and Cyrillic blocks use.
    auto evenPair = [](unsigned int c) { return c & ~1u; };
    auto oddPair = [](unsigned int c) { return (c & 1u) == 0u ? c - 1 : c; };
    if (cp < 0x80) { return (cp >= 'a' && cp <= 'z') ? cp - 0x20 : cp; }
    if (cp < 0x100) {
        if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) { return cp - 0x20; }
        if (cp == 0xFF) { return 0x178; }  // ÿ -> Ÿ, which lives in Extended-A
        return cp;  // ß has no simple capital; µ stays the micro sign
    }
    // --- Latin Extended-A
    if ((cp >= 0x100 && cp <= 0x12F) || (cp >= 0x132 && cp <= 0x137) ||
        (cp >= 0x14A && cp <= 0x177)) {
        return evenPair(cp);
    }
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E)) { return oddPair(cp); }
    if (cp == 0x131) { return 'I'; }  // dotless ı: its capital is plain I
    if (cp == 0x17F) { return 'S'; }  // long s
    // --- Latin Extended-B: the letters a catalogue can meet
    if (cp == 0x1A1 || cp == 0x1A3 || cp == 0x1A5) { return cp - 1; }  // ơ (Vietnamese)
    if (cp == 0x1B0) { return 0x1AF; }                                 // ư (Vietnamese)
    if (cp == 0x180) { return 0x243; }                                 // ƀ
    if (cp == 0x192) { return 0x191; }                                 // ƒ
    if (cp == 0x1C5 || cp == 0x1C6) { return 0x1C4; }  // ǅ ǆ -> Ǆ (the digraph capitals)
    if (cp == 0x1C8 || cp == 0x1C9) { return 0x1C7; }  // ǈ ǉ -> Ǉ
    if (cp == 0x1CB || cp == 0x1CC) { return 0x1CA; }  // ǋ ǌ -> Ǌ
    if (cp == 0x1F2 || cp == 0x1F3) { return 0x1F1; }  // ǲ ǳ -> Ǳ
    if (cp >= 0x1CD && cp <= 0x1DC) { return oddPair(cp); }   // ǎ ǐ ǒ ǔ ǖ ...
    if ((cp >= 0x1DE && cp <= 0x1EF) || (cp >= 0x1F8 && cp <= 0x21F) ||
        (cp >= 0x222 && cp <= 0x233) || (cp >= 0x246 && cp <= 0x24F)) {
        return evenPair(cp);  // ș ț (Romanian) are 0x219 / 0x21B
    }
    if (cp == 0x1F5) { return 0x1F4; }  // ǵ
    // --- Greek (the all-caps loss of the tonos is upperLegend's, not this)
    if (cp >= 0x3B1 && cp <= 0x3C9) { return cp == 0x3C2 ? 0x3A3 : cp - 0x20; }  // ς -> Σ
    if (cp == 0x3AC) { return 0x386; }                   // ά -> Ά
    if (cp >= 0x3AD && cp <= 0x3AF) { return cp - 0x25; }  // έ ή ί -> Έ Ή Ί
    if (cp == 0x3CA || cp == 0x3CB) { return cp - 0x20; }  // ϊ ϋ -> Ϊ Ϋ
    if (cp == 0x3CC) { return 0x38C; }                   // ό -> Ό
    if (cp == 0x3CD || cp == 0x3CE) { return cp - 0x3F; }  // ύ ώ -> Ύ Ώ
    if (cp >= 0x3D8 && cp <= 0x3EF) { return evenPair(cp); }  // archaic letters, Coptic
    // --- Cyrillic and its supplement
    if (cp >= 0x430 && cp <= 0x44F) { return cp - 0x20; }   // а-я -> А-Я
    if (cp >= 0x450 && cp <= 0x45F) { return cp - 0x50; }   // ѐ ё ђ ... џ -> Ѐ Ё Ђ ... Џ
    if ((cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF) ||
        (cp >= 0x4D0 && cp <= 0x52F)) {
        return evenPair(cp);  // ґ (Ukrainian) is 0x491
    }
    if (cp >= 0x4C1 && cp <= 0x4CE) { return oddPair(cp); }
    if (cp == 0x4CF) { return 0x4C0; }  // palochka
    // --- Latin Extended Additional: Vietnamese, Welsh, and the rest
    if ((cp >= 0x1E00 && cp <= 0x1E95) || (cp >= 0x1EA0 && cp <= 0x1EFF)) {
        return evenPair(cp);
    }
    return cp;
}

namespace detail {

// A Greek letter's all-caps form: the capital WITHOUT the tonos (Greek drops
// the stress mark in text set wholly in capitals - "Έξοδος" is "ΕΞΟΔΟΣ"). The
// dialytika stays: ϊ and ΐ both become Ϊ. `droppedTonos` says whether a
// tonos was taken off, which the next letter needs to know.
inline unsigned int greekCaps(unsigned int cp, bool& droppedTonos) {
    droppedTonos = true;
    switch (cp) {
    case 0x386: case 0x3AC: return 0x391;  // Ά ά -> Α
    case 0x388: case 0x3AD: return 0x395;  // Έ έ -> Ε
    case 0x389: case 0x3AE: return 0x397;  // Ή ή -> Η
    case 0x38A: case 0x3AF: return 0x399;  // Ί ί -> Ι
    case 0x38C: case 0x3CC: return 0x39F;  // Ό ό -> Ο
    case 0x38E: case 0x3CD: return 0x3A5;  // Ύ ύ -> Υ
    case 0x38F: case 0x3CE: return 0x3A9;  // Ώ ώ -> Ω
    case 0x390: return 0x3AA;              // ΐ -> Ϊ
    case 0x3B0: return 0x3AB;              // ΰ -> Ϋ
    default: break;
    }
    droppedTonos = false;
    return upperSimple(cp);
}

inline bool isGreek(unsigned int cp) { return cp >= 0x370 && cp <= 0x3FF; }

}  // namespace detail

// Upper case for a key legend or a plate. The bench letters its keys in
// capitals, and std::toupper works on bytes: it leaves "é" alone at best and
// mangles half of it at worst. Every letter upperSimple() knows is put into
// its capital; anything else is left as written. Two rules on top, because a
// capital that is merely the "simple" one would be misspelled:
//
//   TURKISH AND AZERBAIJANI keep the dot: their "i" is capitalised "İ"
//   (U+0130), and their dotless "ı" is "I". Every other language's "i" is
//   "I". So the language matters - `lang` is the catalogue's code, as
//   i18n::current() gives it ("tr", "az-Latn"...); left empty, the general
//   rule applies.
//
//   GREEK IN ALL CAPITALS DROPS THE TONOS, whatever the language in force
//   (the letters say they are Greek): "Ρυθμίσεις" is "ΡΥΘΜΙΣΕΙΣ", not
//   "ΡΥΘΜΊΣΕΙΣ". Where dropping it would join two vowels into a diphthong
//   that were not one - "ά" before "ι" or "υ", say, in "Μάιος" - the second
//   vowel takes a dialytika instead, "ΜΑΪΟΣ", which is how Greek typesetting
//   keeps the word readable. A combining acute after a Greek letter (text
//   typed in decomposed form) is dropped the same way.
inline std::string upperLegend(std::string_view s, std::string_view lang = {}) {
    const std::string_view primary = lang.substr(0, lang.find('-'));
    const bool turkic = primary == "tr" || primary == "az";
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    // The last Greek vowel whose tonos was dropped, while the next letter
    // could still form a false diphthong with it.
    unsigned int accentedBefore = 0;
    while (i < s.size()) {
        const unsigned int cp = utf8Decode(s, i);
        unsigned int up = cp;
        bool dropped = false;
        if (turkic && cp == 'i') {
            up = 0x130;  // İ
        } else if (cp == 0x301 && !out.empty()) {
            // A combining acute: dropped after a Greek letter, kept elsewhere.
            std::size_t back = out.size();
            while (back > 0 && (static_cast<unsigned char>(out[back - 1]) & 0xC0u) == 0x80u) { --back; }
            std::size_t at = back > 0 ? back - 1 : 0;
            const unsigned int prev = utf8Decode(out, at);
            if (detail::isGreek(prev)) { continue; }
        } else if (detail::isGreek(cp)) {
            up = detail::greekCaps(cp, dropped);
            // ι / υ after a vowel that just lost its tonos: keep them apart.
            if (!dropped && accentedBefore != 0) {
                const bool iota = cp == 0x3B9 || cp == 0x399;
                const bool upsilon = cp == 0x3C5 || cp == 0x3A5;
                const bool couldJoin =
                    (iota && (accentedBefore == 0x391 || accentedBefore == 0x395 ||
                              accentedBefore == 0x39F || accentedBefore == 0x3A5)) ||
                    (upsilon && (accentedBefore == 0x391 || accentedBefore == 0x395 ||
                                 accentedBefore == 0x397 || accentedBefore == 0x39F));
                if (iota && couldJoin) { up = 0x3AA; }     // Ϊ
                if (upsilon && couldJoin) { up = 0x3AB; }  // Ϋ
            }
        } else {
            up = upperSimple(cp);
        }
        accentedBefore = (dropped && up != 0x3AA && up != 0x3AB) ? up : 0;
        utf8Append(out, up);
    }
    return out;
}

namespace detail {

// The base letter of every Latin-1 and Latin Extended-A letter, lower case,
// indexed from U+00C0, so that "Áustria" sorts among the A's and typing
// "osterreich" finds "Österreich". '*' marks the two signs in the block
// (multiplication, division).
inline constexpr char kLatinBase[] =
    "aaaaaaaceeeeiiiidnooooo*ouuuuyts"  // U+00C0-U+00DF
    "aaaaaaaceeeeiiiidnooooo*ouuuuyty"  // U+00E0-U+00FF
    "aaaaaacccccccc"                    // U+0100-U+010D
    "ddddeeeeeeeeeegggggggghhhh"        // U+010E-U+0127
    "iiiiiiiiiiiijjkkk"                 // U+0128-U+0138
    "llllllllllnnnnnnnnn"               // U+0139-U+014B
    "oooooooorrrrrrsssssssstttttt"      // U+014C-U+0167
    "uuuuuuuuuuuuwwyyyzzzzzzs";         // U+0168-U+017F
static_assert(sizeof(kLatinBase) == 1 + 0x180 - 0xC0, "one entry per code point");

// The Vietnamese vowels of Latin Extended Additional, U+1EA0-U+1EF9, by base
// letter: ạ ả ấ ... ỹ.
inline constexpr char kVietBase[] =
    "aaaaaaaaaaaaaaaaaaaaaaaa"  // U+1EA0-U+1EB7
    "eeeeeeeeeeeeeeee"          // U+1EB8-U+1EC7
    "iiii"                      // U+1EC8-U+1ECB
    "oooooooooooooooooooooooo"  // U+1ECC-U+1EE3
    "uuuuuuuuuuuuuu"            // U+1EE4-U+1EF1
    "yyyyyyyy";                 // U+1EF2-U+1EF9
static_assert(sizeof(kVietBase) == 1 + 0x1EFA - 0x1EA0, "one entry per code point");

}  // namespace detail

// A KEY FOR SEARCHING AND SORTING a list by what the reader typed: case and
// the common accents taken off, so "osterreich" finds "Österreich", "россия"
// finds "Россия", "ελλαδα" finds "Ελλάδα" and "viet" finds "Việt Nam". Latin
// letters come back lower case and unaccented; every other letter comes back
// as its capital from upperSimple(), Greek without its tonos - one form per
// letter whichever case was typed. Not a collation: good enough that a list
// reads in the order a reader expects and a filter finds what they meant.
inline std::string foldForSearch(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned int cp = utf8Decode(s, i);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp));
        } else if (cp >= 0xC0 && cp < 0x180 && detail::kLatinBase[cp - 0xC0] != '*') {
            out.push_back(detail::kLatinBase[cp - 0xC0]);
        } else if (cp >= 0x1EA0 && cp <= 0x1EF9) {
            out.push_back(detail::kVietBase[cp - 0x1EA0]);
        } else if (cp == 0x1A0 || cp == 0x1A1) {
            out.push_back('o');  // ơ
        } else if (cp == 0x1AF || cp == 0x1B0) {
            out.push_back('u');  // ư
        } else if (cp >= 0x218 && cp <= 0x21B) {
            out.push_back(cp < 0x21A ? 's' : 't');  // ș ț
        } else if (detail::isGreek(cp)) {
            bool dropped = false;
            utf8Append(out, detail::greekCaps(cp, dropped));
        } else {
            utf8Append(out, upperSimple(cp));
        }
    }
    return out;
}

}  // namespace cascade::core

#endif  // CASCADE_CORE_UTF8_TEXT_HPP
