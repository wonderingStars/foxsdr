// BIT-VIEW LIBRARY, part 11: the binary editor model - a bit buffer with a byte / bit view, code-table
// remapping, and edit with undo.
//
// CODE TABLES. A table maps a symbol (1..8 bits) to text, in up to four PLANES selected by shift symbols:
// ITA2 has two (letters, figures), and a user table can have up to four, which is how the Code300-32
// program describes its user alphabets ("ITA 2 (32/64/96 characters 2/3/4 shift)", from the vendor's
// product text seen in a search result; the program itself was not run or opened). Built-in tables:
//   ASCII-7, ASCII-8 (printable, others shown as '.'), ITA5 (the 7-bit IA5 = ASCII layout; the national-use
//   positions of the international reference version were NOT checked), ITA2 and ITA2-US (opened:
//   en.wikipedia.org/wiki/Baudot_code, 2026-11-11), SIXBIT (DEC: code + 0x20), AIS 6-bit (0..31 = '@'..'_',
//   32..63 = ' '..'?': written from memory of ITU-R M.1371 / the gpsd AIVDM page, NOT opened).
// A USER TABLE is a text file ("foxsdr-bittable 1"), parsed with line numbers and limits.
//
//   foxsdr-bittable 1
//   name my-alphabet
//   bits 5
//   plane LTRS              # a plane starts here; the first plane is active at the start
//   shift FIGS = 27         # in this plane, symbol 27 switches to plane FIGS
//   1 = E
//   2 = <LF>
//   4 = <space>
//   plane FIGS
//   shift LTRS = 31
//   1 = 3
//
// Glyph words are up to 8 characters; <space> is a blank. A symbol without an entry shows '?'.
//
// EDITING. Every edit is one "replace the bits [pos, pos+n) with these bits" record, so undo and redo are
// exact whatever the edit was. The history is bounded (256 edits and 4 Mbit of saved bits); the buffer
// is bounded at 1 Mbit; an edit that would break a bound is refused and changes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "core/bits/bits.hpp"

namespace cascade::bits {

struct CodeTable {
    std::string name;
    unsigned bits = 8;                                  // symbol width
    std::vector<std::array<std::string, 256>> planes;   // glyph per symbol per plane ("" = none)
    // shifts[p][symbol] = plane to switch to (-1 none)
    std::vector<std::array<int16_t, 256>> shifts;
    bool msbFirst = true;                               // how symbols are read from the bit stream by default
};

namespace detail {
inline CodeTable newTable(const char* name, unsigned bits, size_t planes, bool msbFirst = true) {
    CodeTable t;
    t.name = name;
    t.bits = bits;
    t.msbFirst = msbFirst;
    t.planes.resize(planes);
    t.shifts.resize(planes);
    for (auto& s : t.shifts) s.fill(-1);
    return t;
}
}  // namespace detail

inline const char* controlName(unsigned c) {
    static const char* n[32] = {"NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS", "HT", "LF", "VT", "FF", "CR", "SO", "SI",
                                "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB", "CAN", "EM", "SUB", "ESC", "FS", "GS", "RS", "US"};
    return c < 32 ? n[c] : "DEL";
}

inline CodeTable tableAscii7() {
    CodeTable t = detail::newTable("ASCII-7", 7, 1);
    for (unsigned c = 0; c < 128; ++c) t.planes[0][c] = (c >= 0x20 && c < 0x7F) ? std::string(1, static_cast<char>(c)) : std::string("<") + controlName(c) + ">";
    return t;
}
inline CodeTable tableAscii8() {
    CodeTable t = detail::newTable("ASCII-8", 8, 1);
    for (unsigned c = 0; c < 256; ++c) t.planes[0][c] = (c >= 0x20 && c < 0x7F) ? std::string(1, static_cast<char>(c)) : ".";
    return t;
}
inline CodeTable tableIta5() {
    CodeTable t = tableAscii7();
    t.name = "ITA5";
    return t;
}
inline CodeTable tableSixbit() {
    CodeTable t = detail::newTable("SIXBIT", 6, 1);
    for (unsigned c = 0; c < 64; ++c) t.planes[0][c] = std::string(1, static_cast<char>(c + 0x20));
    return t;
}
inline CodeTable tableAis6() {
    CodeTable t = detail::newTable("AIS-6bit", 6, 1);
    for (unsigned c = 0; c < 64; ++c) t.planes[0][c] = std::string(1, static_cast<char>(c < 32 ? 0x40 + c : c));
    return t;
}
// ITA2 as the opened table gives it: code element 1 is the least significant bit; the symbol value is the hex shown there.
inline CodeTable tableIta2(bool usTty = false) {
    static const char* L[32] = {"<NUL>", "E", "<LF>", "A", "<space>", "S", "I", "U", "<CR>", "D", "R", "J", "N", "F", "C", "K",
                                "T", "Z", "L", "W", "H", "Y", "P", "Q", "O", "B", "G", "", "M", "X", "V", ""};
    static const char* F[32] = {"<NUL>", "3", "<LF>", "-", "<space>", "'", "8", "7", "<CR>", "<ENQ>", "4", "<BEL>", ",", "!", ":", "(",
                                "5", "+", ")", "2", "GBP", "6", "0", "1", "9", "?", "&", "", ".", "/", "=", ""};
    CodeTable t = detail::newTable(usTty ? "ITA2-US" : "ITA2", 5, 2);
    for (unsigned c = 0; c < 32; ++c) {
        t.planes[0][c] = L[c];
        t.planes[1][c] = F[c];
    }
    if (usTty) {  // the US-TTY figure-set differences from the opened page
        t.planes[1][0x05] = "<BEL>";
        t.planes[1][0x09] = "$";
        t.planes[1][0x0B] = "'";
        t.planes[1][0x11] = "\"";
        t.planes[1][0x14] = "#";
        t.planes[1][0x1E] = ";";
    }
    for (size_t p = 0; p < 2; ++p) {
        t.shifts[p][0x1B] = 1;  // FIGS
        t.shifts[p][0x1F] = 0;  // LTRS
    }
    return t;
}

struct TableError {
    int line = 0;
    std::string message;
};
struct TableParse {
    bool ok = false;
    CodeTable table;
    std::vector<TableError> errors;
};

inline TableParse parseTable(std::string_view text) {
    TableParse res;
    CodeTable& t = res.table;
    auto err = [&](int line, const std::string& m) { if (res.errors.size() < 32) res.errors.push_back({line, m}); };
    if (text.size() > 8 * 1024) { err(0, "the table file is longer than 8 KB"); return res; }
    bool sawVersion = false, sawBits = false;
    int cur = -1;
    std::vector<std::string> planeNames;
    struct PendingShift { int line; size_t plane; std::string target; unsigned sym; };
    std::vector<PendingShift> pending;
    size_t entries = 0, lines = 0;
    int lineNo = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view ln = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (++lines > 400) { err(lineNo, "more than 400 lines"); break; }
        const size_t h = ln.find('#');
        if (h != std::string_view::npos) ln = ln.substr(0, h);
        std::vector<std::string> w;
        {
            size_t i = 0;
            while (i < ln.size()) {
                while (i < ln.size() && (ln[i] == ' ' || ln[i] == '\t' || ln[i] == '\r')) ++i;
                size_t j = i;
                while (j < ln.size() && ln[j] != ' ' && ln[j] != '\t' && ln[j] != '\r') ++j;
                if (j > i) w.emplace_back(ln.substr(i, j - i));
                i = j;
            }
        }
        if (w.empty()) continue;
        auto number = [&](const std::string& s, unsigned long lo, unsigned long hi, unsigned long* out) {
            if (s.empty() || s.size() > 12) return false;
            char* end = nullptr;
            const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) ? 2 : 10;
            const unsigned long v = std::strtoul(base == 2 ? s.c_str() + 2 : s.c_str(), &end, base);
            if (*end != '\0' || s[0] == '-' || v < lo || v > hi) return false;
            *out = v;
            return true;
        };
        if (!sawVersion) {
            if (w[0] != "foxsdr-bittable" || w.size() != 2 || w[1] != "1") { err(lineNo, "the first statement must be 'foxsdr-bittable 1'"); break; }
            sawVersion = true;
            continue;
        }
        if (w[0] == "name") {
            if (w.size() != 2 || w[1].size() > 40) { err(lineNo, "name: one word of up to 40 characters"); continue; }
            t.name = w[1];
        } else if (w[0] == "bits") {
            unsigned long v;
            if (w.size() != 2 || !number(w[1], 1, 8, &v)) { err(lineNo, "bits: a number 1..8"); continue; }
            if (cur >= 0) { err(lineNo, "'bits' must come before the first plane"); continue; }
            t.bits = static_cast<unsigned>(v);
            sawBits = true;
        } else if (w[0] == "plane") {
            if (!sawBits) { err(lineNo, "'bits' must come before the first plane"); continue; }
            if (w.size() != 2 || w[1].size() > 12) { err(lineNo, "plane: one name of up to 12 characters"); continue; }
            if (planeNames.size() >= 4) { err(lineNo, "at most four planes"); continue; }
            if (std::find(planeNames.begin(), planeNames.end(), w[1]) != planeNames.end()) { err(lineNo, "plane '" + w[1] + "' is already defined"); continue; }
            planeNames.push_back(w[1]);
            t.planes.emplace_back();
            t.shifts.emplace_back();
            t.shifts.back().fill(-1);
            cur = static_cast<int>(planeNames.size()) - 1;
        } else if (w[0] == "shift") {
            // shift NAME = SYMBOL
            unsigned long sym;
            if (cur < 0) { err(lineNo, "'shift' must be inside a plane"); continue; }
            if (w.size() != 4 || w[2] != "=" || !number(w[3], 0, (1ul << t.bits) - 1, &sym)) { err(lineNo, "shift: expected 'shift PLANE = SYMBOL'"); continue; }
            pending.push_back({lineNo, static_cast<size_t>(cur), w[1], static_cast<unsigned>(sym)});
        } else {
            // CODE = GLYPH
            unsigned long sym;
            if (cur < 0) { err(lineNo, "an entry must be inside a plane"); continue; }
            if (w.size() != 3 || w[1] != "=" || !number(w[0], 0, (1ul << t.bits) - 1, &sym)) { err(lineNo, "expected 'CODE = GLYPH' with CODE inside 0.." + std::to_string((1u << t.bits) - 1)); continue; }
            if (w[2].size() > 8) { err(lineNo, "a glyph is at most 8 characters"); continue; }
            if (!t.planes[static_cast<size_t>(cur)][sym].empty()) { err(lineNo, "symbol " + w[0] + " is already defined in this plane"); continue; }
            if (++entries > 1024) { err(lineNo, "more than 1024 entries"); continue; }
            t.planes[static_cast<size_t>(cur)][sym] = w[2] == "<space>" ? " " : w[2];
        }
    }
    if (!sawVersion && res.errors.empty()) err(0, "the table is empty: it must begin with 'foxsdr-bittable 1'");
    if (sawVersion && res.errors.empty()) {
        if (planeNames.empty()) err(0, "no plane is defined");
        for (const auto& p : pending) {
            const auto it = std::find(planeNames.begin(), planeNames.end(), p.target);
            if (it == planeNames.end()) { err(p.line, "shift to unknown plane '" + p.target + "'"); continue; }
            t.shifts[p.plane][p.sym] = static_cast<int16_t>(it - planeNames.begin());
        }
    }
    res.ok = res.errors.empty();
    return res;
}

// Read `count` symbols (all that remain when 0) from `pos`; the text, shifts applied, and how many symbols had no glyph.
struct DecodedText {
    std::string text;
    size_t unknown = 0;
    size_t symbols = 0;
};
inline DecodedText decodeText(const Bits& b, const CodeTable& t, size_t pos = 0, size_t count = 0, bool msbFirst = true) {
    DecodedText out;
    if (t.bits == 0 || t.planes.empty()) return out;
    size_t plane = 0;
    for (size_t p = pos; p + t.bits <= b.size() && (count == 0 || out.symbols < count); p += t.bits) {
        const unsigned sym = static_cast<unsigned>(getField(b, p, t.bits, msbFirst));
        ++out.symbols;
        if (t.shifts[plane][sym] >= 0) { plane = static_cast<size_t>(t.shifts[plane][sym]); continue; }
        const std::string& g = t.planes[plane][sym];
        if (g.empty()) { out.text += '?'; ++out.unknown; }
        else out.text += g;
    }
    return out;
}

// Substitute symbols: every `width`-bit symbol s becomes map[s] (a user permutation / re-labelling), for a
// recode between two alphabets that share the same bit width.
inline Bits remapSymbols(const Bits& b, unsigned width, const std::array<uint8_t, 256>& map, bool msbFirst = true) {
    Bits out(b);
    if (width == 0 || width > 8) return out;
    for (size_t p = 0; p + width <= out.size(); p += width) {
        const uint8_t v = map[static_cast<size_t>(getField(out, p, width, msbFirst))];
        for (unsigned i = 0; i < width; ++i) out[p + i] = static_cast<uint8_t>(msbFirst ? (v >> (width - 1 - i)) & 1u : (v >> i) & 1u);
    }
    return out;
}

// ---- the editor -------------------------------------------------------------------------------------------

class BitEditor {
public:
    static constexpr size_t kMaxBits = 1u << 20;
    static constexpr size_t kMaxEdits = 256;
    static constexpr size_t kMaxSavedBits = 4u << 20;

    explicit BitEditor(Bits initial = {}) : bits_(std::move(initial)) { if (bits_.size() > kMaxBits) bits_.resize(kMaxBits); }

    const Bits& bits() const { return bits_; }
    size_t size() const { return bits_.size(); }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    size_t historySize() const { return undo_.size(); }
    bool dirty() const { return revision_ != savedRevision_; }
    void markSaved() { savedRevision_ = revision_; }

    bool setBit(size_t i, unsigned v) { return i < bits_.size() && replace(i, 1, Bits{static_cast<uint8_t>(v & 1u)}); }
    bool flipBit(size_t i) { return i < bits_.size() && replace(i, 1, Bits{static_cast<uint8_t>(bits_[i] ^ 1u)}); }
    bool setByte(size_t byteIndex, uint8_t v, bool msbFirst = true) {
        const size_t p = byteIndex * 8;
        if (p + 8 > bits_.size()) return false;
        return replace(p, 8, fromBytes({v}, msbFirst));
    }
    bool insertBits(size_t pos, const Bits& x) { return pos <= bits_.size() && replace(pos, 0, x); }
    bool eraseBits(size_t pos, size_t n) { return pos <= bits_.size() && n <= bits_.size() - pos && replace(pos, n, {}); }
    bool invertRange(size_t pos, size_t n) {
        if (pos > bits_.size() || n > bits_.size() - pos) return false;
        return replace(pos, n, inverted(slice(bits_, pos, n)));
    }
    bool reverseRange(size_t pos, size_t n) {
        if (pos > bits_.size() || n > bits_.size() - pos) return false;
        Bits r = slice(bits_, pos, n);
        std::reverse(r.begin(), r.end());
        return replace(pos, n, std::move(r));
    }
    // XOR a repeating pattern over the range (whitening by hand)
    bool xorRange(size_t pos, size_t n, const Bits& pattern) {
        if (pattern.empty() || pos > bits_.size() || n > bits_.size() - pos) return false;
        Bits r = slice(bits_, pos, n);
        for (size_t i = 0; i < r.size(); ++i) r[i] = static_cast<uint8_t>(r[i] ^ pattern[i % pattern.size()]);
        return replace(pos, n, std::move(r));
    }
    // Re-label every `width`-bit symbol of the range through a map (see remapSymbols).
    bool remapRange(size_t pos, size_t n, unsigned width, const std::array<uint8_t, 256>& map, bool msbFirst = true) {
        if (width == 0 || width > 8 || pos > bits_.size() || n > bits_.size() - pos) return false;
        return replace(pos, n, remapSymbols(slice(bits_, pos, n), width, map, msbFirst));
    }
    bool replaceAll(const Bits& x) { return replace(0, bits_.size(), x); }

    bool undo() {
        if (undo_.empty()) return false;
        Edit e = std::move(undo_.back());
        undo_.pop_back();
        apply(e.pos, e.inserted.size(), e.removed);
        std::swap(e.removed, e.inserted);
        savedBitsUsed_ -= std::min(savedBitsUsed_, e.removed.size() + e.inserted.size());
        redo_.push_back(std::move(e));
        --revision_;
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        Edit e = std::move(redo_.back());
        redo_.pop_back();
        apply(e.pos, e.inserted.size(), e.removed);
        std::swap(e.removed, e.inserted);
        savedBitsUsed_ += e.removed.size() + e.inserted.size();
        undo_.push_back(std::move(e));
        ++revision_;
        return true;
    }

    // The byte view: rows of `perRow` bytes, offset, hex, then the text of the table (printable only).
    std::string byteView(size_t perRow = 8, const CodeTable* table = nullptr) const {
        static const char* hex = "0123456789ABCDEF";
        if (perRow == 0) perRow = 8;
        const auto bytes = toBytes(bits_);
        std::string out;
        for (size_t r = 0; r < bytes.size(); r += perRow) {
            char off[16];
            std::snprintf(off, sizeof off, "%06zX  ", r);
            out += off;
            std::string txt;
            for (size_t i = r; i < r + perRow; ++i) {
                if (i < bytes.size()) {
                    out += hex[bytes[i] >> 4];
                    out += hex[bytes[i] & 15];
                    out += ' ';
                    txt += (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? static_cast<char>(bytes[i]) : '.';
                } else out += "   ";
            }
            out += " " + txt + "\n";
        }
        (void)table;
        return out;
    }

private:
    struct Edit {
        size_t pos = 0;
        Bits removed, inserted;
    };

    void apply(size_t pos, size_t removeN, const Bits& ins) {
        bits_.erase(bits_.begin() + static_cast<std::ptrdiff_t>(pos), bits_.begin() + static_cast<std::ptrdiff_t>(pos + removeN));
        bits_.insert(bits_.begin() + static_cast<std::ptrdiff_t>(pos), ins.begin(), ins.end());
    }

    bool replace(size_t pos, size_t n, Bits x) {
        if (pos > bits_.size() || n > bits_.size() - pos) return false;
        if (bits_.size() - n + x.size() > kMaxBits) return false;
        if (n + x.size() > kMaxSavedBits) return false;
        Edit e;
        e.pos = pos;
        e.removed = slice(bits_, pos, n);
        e.inserted = std::move(x);
        if (e.removed == e.inserted) return true;  // nothing changes: no history entry
        apply(pos, n, e.inserted);
        savedBitsUsed_ += e.removed.size() + e.inserted.size();
        undo_.push_back(std::move(e));
        if (!redo_.empty() && savedRevision_ != kLost && savedRevision_ > revision_) savedRevision_ = kLost;  // the saved state was on the redo branch we just dropped
        redo_.clear();
        ++revision_;
        while (undo_.size() > kMaxEdits || savedBitsUsed_ > kMaxSavedBits) {
            savedBitsUsed_ -= std::min(savedBitsUsed_, undo_.front().removed.size() + undo_.front().inserted.size());
            undo_.pop_front();
        }
        if (savedRevision_ != kLost && revision_ - undo_.size() > savedRevision_) savedRevision_ = kLost;  // the saved state fell out of the history
        return true;
    }

    Bits bits_;
    std::deque<Edit> undo_, redo_;
    size_t savedBitsUsed_ = 0;
    static constexpr size_t kLost = static_cast<size_t>(-1);
    size_t revision_ = 0, savedRevision_ = 0;
};

}  // namespace cascade::bits
