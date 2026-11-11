// BIT-VIEW LIBRARY, part 9: the user-described decoder for unknown OOK / FSK devices.
//
// A device the program has never heard of is described in a short text file (this program's OWN
// format, "foxsdr-bitspec", version 1, designed from what a person measuring a remote knows: the
// pulse widths or the bit clock, the preamble, where the fields are, how the check works) and run over
// the level runs of a slicer (ookslicer.hpp). No other program's description language was read.
//
// FORMAT. One statement per line, '#' starts a comment, words separated by blanks, numbers decimal
// or 0x-hex. The first statement must be `foxsdr-bitspec 1`.
//
//   name WORD                              label (letters, digits, '_' '-', up to 40)
//   coding pwm | ppm                       pulse width (mark width carries the bit) or pulse position (space width)
//   coding chips LINECODE                  a clocked code: the runs are sliced into chips and decoded as LINECODE
//   stage LINECODE                         up to two further decoding stages over the bits of the previous one
//                                          (a device that sends every bit twice, as itself and inverted, is
//                                          Manchester inside Manchester: `coding chips manchester` + `stage manchester`)
//   bit_us N | auto                        bit time of the FIRST stage (chips) in microseconds
//   short_us N, long_us N                  pulse classes for pwm/ppm; a width within `tolerance` percent of one is that class
//   tolerance P                            percent, 1..60, default 30
//   reset_us N                             a space at least this long ends a message (default 5000)
//   balance yes|no                         compensate the slicer's mark/space bias (default yes for chips, no for pwm/ppm)
//   invert yes | no | auto                 invert the decoded bits (auto tries both, default auto)
//   preamble BITS                          pattern after which the frame starts (e.g. 1111 1111 0101)
//   preamble_errors N                      bits of the preamble allowed to differ (0..16, default 0)
//   min_bits N / max_bits N / frame_bits N frame size limits (max up to 8192; frame_bits fixes the length)
//   max_violations N                       line-code violations tolerated in a message (default 3)
//   field NAME at POS LEN TYPE [attributes]   TYPE: uint int bcd hex bits flag; POS counts bits after the
//                                          preamble, a negative POS from the end of the frame
//       attributes: lsb (whole field read LSB first)  nibble_lsb (each 4-bit group sent LSB first)
//                   digits_lsd (groups sent least significant first)  scale X  offset Y  unit TEXT  min X  max X
//   check KIND from A to B at P LEN [nibble_lsb] [digits_lsd] [poly H init H xorout H refin yes refout yes width W]
//       KIND: nibblesum sum8 xor8 crc; positions as for fields; the value at P must equal the computed one
//
// Bad specs are refused with LINE NUMBERS (parseSpec); resource limits: text up to 16 KB, 200 lines, 32
// fields, bit patterns up to 64 bits, messages up to max_bits, run lists up to 2^20 runs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/bits/bits.hpp"
#include "core/bits/crc.hpp"
#include "core/bits/linecode.hpp"
#include "core/bits/ookslicer.hpp"
#include "core/bits/sync.hpp"

namespace cascade::bits {

enum class SpecType : uint8_t { Uint, Int, Bcd, Hex, Bits, Flag };
enum class SpecCoding : uint8_t { Pwm, Ppm, Chips };
enum class SpecCheckKind : uint8_t { None, NibbleSum, Sum8, Xor8, Crc };
enum class Tri : uint8_t { No, Yes, Auto };

struct SpecField {
    std::string name;
    long pos = 0;
    unsigned len = 0;
    SpecType type = SpecType::Uint;
    bool lsb = false, nibbleLsb = false, digitsLsd = false;
    double scale = 1.0, offset = 0.0;
    std::string unit;
    bool hasMin = false, hasMax = false;
    double minV = 0, maxV = 0;
    int line = 0;
};

struct SpecCheck {
    SpecCheckKind kind = SpecCheckKind::None;
    long from = 0, to = 0, at = 0;
    unsigned len = 0;
    bool nibbleLsb = false, digitsLsd = false;
    CrcModel crc;
    int line = 0;
};

struct BitSpec {
    int version = 0;
    std::string name;
    SpecCoding coding = SpecCoding::Chips;
    std::vector<LineCode> stages;  // Chips: first = the chip-level code; later = over its bits
    double bitUs = 0;              // 0 = auto
    bool bitUsAuto = false;
    double shortUs = 0, longUs = 0;
    double tolerancePct = 30;
    double resetUs = 5000;
    bool balance = true;
    Tri invert = Tri::Auto;
    Bits preamble;
    unsigned preambleErrors = 0;
    size_t minBits = 8, maxBits = 4096, frameBits = 0;
    unsigned maxViolations = 3;
    std::vector<SpecField> fields;
    SpecCheck check;
};

struct SpecError {
    int line = 0;  // 1-based; 0 = about the spec as a whole
    std::string message;
};

struct SpecParse {
    bool ok = false;
    BitSpec spec;
    std::vector<SpecError> errors;
};

namespace detail {

inline std::vector<std::string> splitWords(std::string_view s) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r') ++j;
        if (j > i) w.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}

inline bool parseNumber(const std::string& s, double* out) {
    if (s.empty() || s.size() > 24) return false;
    char* end = nullptr;
    const double v = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? static_cast<double>(std::strtoull(s.c_str(), &end, 16))
                                                                                    : std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}
inline bool parseU64(const std::string& s, uint64_t* out) {
    if (s.empty() || s.size() > 20) return false;
    char* end = nullptr;
    const int base = (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
    const unsigned long long v = std::strtoull(s.c_str(), &end, base);
    if (end == s.c_str() || *end != '\0' || (s[0] == '-')) return false;
    *out = v;
    return true;
}
inline bool parseYesNo(const std::string& s, bool* out) {
    if (s == "yes" || s == "on" || s == "true") { *out = true; return true; }
    if (s == "no" || s == "off" || s == "false") { *out = false; return true; }
    return false;
}
inline bool validName(const std::string& s, size_t maxLen, bool allowDash) {
    if (s.empty() || s.size() > maxLen) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (allowDash && c == '-'))) return false;
    return true;
}
inline bool lineCodeFromName(const std::string& s, LineCode* out) {
    static const struct { const char* n; LineCode c; } t[] = {
        {"nrzl", LineCode::NrzL}, {"nrzm", LineCode::NrzM}, {"nrzs", LineCode::NrzS}, {"nrzi", LineCode::NrzS},
        {"manchester", LineCode::ManchesterIeee}, {"thomas", LineCode::ManchesterThomas},
        {"diffmanchester", LineCode::DiffManchester}, {"diffmanchester_inv", LineCode::DiffManchesterInv},
        {"biphasemark", LineCode::BiphaseMark}, {"biphasespace", LineCode::BiphaseSpace}, {"miller", LineCode::Miller}};
    for (const auto& e : t)
        if (s == e.n) { *out = e.c; return true; }
    return false;
}
inline bool typeFromName(const std::string& s, SpecType* out) {
    static const struct { const char* n; SpecType t; } t[] = {{"uint", SpecType::Uint}, {"int", SpecType::Int}, {"bcd", SpecType::Bcd},
                                                              {"hex", SpecType::Hex}, {"bits", SpecType::Bits}, {"flag", SpecType::Flag}};
    for (const auto& e : t)
        if (s == e.n) { *out = e.t; return true; }
    return false;
}
}  // namespace detail

inline SpecParse parseSpec(std::string_view text) {
    SpecParse res;
    BitSpec& sp = res.spec;
    auto err = [&](int line, const std::string& m) { if (res.errors.size() < 32) res.errors.push_back({line, m}); };
    if (text.size() > 16 * 1024) { err(0, "the spec is longer than 16 KB"); return res; }
    bool sawVersion = false, sawCoding = false, sawBalance = false;
    int lineNo = 0;
    size_t pos = 0, lines = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view ln = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (++lines > 200) { err(lineNo, "more than 200 lines"); break; }
        const size_t h = ln.find('#');
        if (h != std::string_view::npos) ln = ln.substr(0, h);
        if (ln.size() > 200) { err(lineNo, "line longer than 200 characters"); continue; }
        const auto w = detail::splitWords(ln);
        if (w.empty()) continue;
        const std::string& k = w[0];
        auto need = [&](size_t n) {
            if (w.size() != n) { err(lineNo, "'" + k + "' takes " + std::to_string(n - 1) + " value(s)"); return false; }
            return true;
        };
        auto num = [&](const std::string& s, double lo, double hi, double* out, const char* what) {
            double v = 0;
            if (!detail::parseNumber(s, &v)) { err(lineNo, std::string(what) + ": '" + s + "' is not a number"); return false; }
            if (v < lo || v > hi) { err(lineNo, std::string(what) + ": " + s + " is outside " + std::to_string(static_cast<long long>(lo)) + ".." + std::to_string(static_cast<long long>(hi))); return false; }
            *out = v;
            return true;
        };
        if (!sawVersion) {
            if (k != "foxsdr-bitspec") { err(lineNo, "the first statement must be 'foxsdr-bitspec 1'"); break; }
            double v = 0;
            if (w.size() != 2 || !detail::parseNumber(w[1], &v)) { err(lineNo, "'foxsdr-bitspec' needs a version number"); break; }
            if (v != 1) { err(lineNo, "unsupported spec version " + w[1] + " (this program reads version 1)"); break; }
            sp.version = 1;
            sawVersion = true;
            continue;
        }
        if (k == "name") {
            if (!need(2)) continue;
            if (!detail::validName(w[1], 40, true)) { err(lineNo, "name: letters, digits, '_' and '-' only, 1..40 characters"); continue; }
            sp.name = w[1];
        } else if (k == "coding") {
            if (w.size() < 2) { err(lineNo, "'coding' needs pwm, ppm or chips LINECODE"); continue; }
            sawCoding = true;
            if (w[1] == "pwm" && w.size() == 2) sp.coding = SpecCoding::Pwm;
            else if (w[1] == "ppm" && w.size() == 2) sp.coding = SpecCoding::Ppm;
            else if (w[1] == "chips" && w.size() == 3) {
                LineCode lc;
                if (!detail::lineCodeFromName(w[2], &lc)) { err(lineNo, "unknown line code '" + w[2] + "'"); continue; }
                sp.coding = SpecCoding::Chips;
                sp.stages.clear();
                sp.stages.push_back(lc);
            } else err(lineNo, "coding: expected 'pwm', 'ppm' or 'chips LINECODE'");
        } else if (k == "stage") {
            if (!need(2)) continue;
            LineCode lc;
            if (!detail::lineCodeFromName(w[1], &lc)) { err(lineNo, "unknown line code '" + w[1] + "'"); continue; }
            if (sp.stages.size() >= 3) { err(lineNo, "at most two 'stage' lines"); continue; }
            sp.stages.push_back(lc);
        } else if (k == "bit_us") {
            if (!need(2)) continue;
            if (w[1] == "auto") { sp.bitUsAuto = true; sp.bitUs = 0; }
            else { double v; if (num(w[1], 1, 1e6, &v, "bit_us")) { sp.bitUs = v; sp.bitUsAuto = false; } }
        } else if (k == "short_us" || k == "long_us") {
            if (!need(2)) continue;
            double v;
            if (num(w[1], 1, 1e6, &v, k.c_str())) (k == "short_us" ? sp.shortUs : sp.longUs) = v;
        } else if (k == "tolerance") {
            if (!need(2)) continue;
            double v;
            if (num(w[1], 1, 60, &v, "tolerance")) sp.tolerancePct = v;
        } else if (k == "reset_us") {
            if (!need(2)) continue;
            double v;
            if (num(w[1], 10, 1e7, &v, "reset_us")) sp.resetUs = v;
        } else if (k == "balance") {
            if (!need(2)) continue;
            bool b;
            if (!detail::parseYesNo(w[1], &b)) { err(lineNo, "balance: expected yes or no"); continue; }
            sp.balance = b;
            sawBalance = true;
        } else if (k == "invert") {
            if (!need(2)) continue;
            bool b;
            if (w[1] == "auto") sp.invert = Tri::Auto;
            else if (detail::parseYesNo(w[1], &b)) sp.invert = b ? Tri::Yes : Tri::No;
            else err(lineNo, "invert: expected yes, no or auto");
        } else if (k == "preamble") {
            std::string joined;
            for (size_t i = 1; i < w.size(); ++i) joined += w[i];
            bool ok = false;
            const Bits b = fromString(joined, &ok);
            if (!ok || b.empty()) { err(lineNo, "preamble: a pattern of 0s and 1s is needed"); continue; }
            if (b.size() > 64) { err(lineNo, "preamble: at most 64 bits"); continue; }
            sp.preamble = b;
        } else if (k == "preamble_errors") {
            if (!need(2)) continue;
            double v;
            if (num(w[1], 0, 16, &v, "preamble_errors")) sp.preambleErrors = static_cast<unsigned>(v);
        } else if (k == "min_bits" || k == "max_bits" || k == "frame_bits") {
            if (!need(2)) continue;
            double v;
            if (!num(w[1], k == "frame_bits" ? 0 : 1, 8192, &v, k.c_str())) continue;
            (k == "min_bits" ? sp.minBits : k == "max_bits" ? sp.maxBits : sp.frameBits) = static_cast<size_t>(v);
        } else if (k == "max_violations") {
            if (!need(2)) continue;
            double v;
            if (num(w[1], 0, 1000, &v, "max_violations")) sp.maxViolations = static_cast<unsigned>(v);
        } else if (k == "field") {
            // field NAME at POS LEN TYPE [attrs]
            if (w.size() < 6 || w[2] != "at") { err(lineNo, "field: expected 'field NAME at POS LEN TYPE [attributes]'"); continue; }
            SpecField f;
            f.line = lineNo;
            f.name = w[1];
            if (!detail::validName(f.name, 24, false)) { err(lineNo, "field name: letters, digits and '_' only, 1..24 characters"); continue; }
            double p, l;
            if (!num(w[3], -8192, 8192, &p, "field position") || !num(w[4], 1, 64, &l, "field length")) continue;
            f.pos = static_cast<long>(p);
            f.len = static_cast<unsigned>(l);
            if (!detail::typeFromName(w[5], &f.type)) { err(lineNo, "unknown field type '" + w[5] + "'"); continue; }
            bool bad = false;
            for (size_t i = 6; i < w.size() && !bad; ++i) {
                const std::string& a = w[i];
                double v;
                if (a == "lsb") f.lsb = true;
                else if (a == "nibble_lsb") f.nibbleLsb = true;
                else if (a == "digits_lsd") f.digitsLsd = true;
                else if (a == "scale" || a == "offset" || a == "min" || a == "max") {
                    if (i + 1 >= w.size() || !detail::parseNumber(w[i + 1], &v)) { err(lineNo, "'" + a + "' needs a number"); bad = true; break; }
                    ++i;
                    if (a == "scale") f.scale = v;
                    else if (a == "offset") f.offset = v;
                    else if (a == "min") { f.hasMin = true; f.minV = v; }
                    else { f.hasMax = true; f.maxV = v; }
                } else if (a == "unit") {
                    if (i + 1 >= w.size() || w[i + 1].size() > 8) { err(lineNo, "'unit' needs a word of up to 8 characters"); bad = true; break; }
                    f.unit = w[++i];
                } else { err(lineNo, "unknown field attribute '" + a + "'"); bad = true; }
            }
            if (bad) continue;
            if ((f.nibbleLsb || f.digitsLsd || f.type == SpecType::Bcd) && f.len % 4 != 0) { err(lineNo, "field length must be a multiple of 4 for nibble attributes and bcd"); continue; }
            if (f.type == SpecType::Flag && f.len != 1) { err(lineNo, "a flag is one bit"); continue; }
            for (const auto& e : sp.fields)
                if (e.name == f.name) { err(lineNo, "field '" + f.name + "' is already defined on line " + std::to_string(e.line)); bad = true; }
            if (bad) continue;
            if (sp.fields.size() >= 32) { err(lineNo, "more than 32 fields"); continue; }
            sp.fields.push_back(std::move(f));
        } else if (k == "check") {
            // check KIND from A to B at P LEN [attrs]
            if (sp.check.kind != SpecCheckKind::None) { err(lineNo, "only one 'check' statement is allowed"); continue; }
            if (w.size() < 9 || w[2] != "from" || w[4] != "to" || w[6] != "at") { err(lineNo, "check: expected 'check KIND from A to B at P LEN [attributes]'"); continue; }
            SpecCheck c;
            c.line = lineNo;
            if (w[1] == "nibblesum") c.kind = SpecCheckKind::NibbleSum;
            else if (w[1] == "sum8") c.kind = SpecCheckKind::Sum8;
            else if (w[1] == "xor8") c.kind = SpecCheckKind::Xor8;
            else if (w[1] == "crc") c.kind = SpecCheckKind::Crc;
            else { err(lineNo, "unknown check kind '" + w[1] + "'"); continue; }
            double a, b, p, l;
            if (!num(w[3], -8192, 8192, &a, "check from") || !num(w[5], -8192, 8192, &b, "check to") || !num(w[7], -8192, 8192, &p, "check position") ||
                !num(w[8], 1, 64, &l, "check length"))
                continue;
            c.from = static_cast<long>(a);
            c.to = static_cast<long>(b);
            c.at = static_cast<long>(p);
            c.len = static_cast<unsigned>(l);
            bool bad = false;
            c.crc.width = 0;
            for (size_t i = 9; i < w.size() && !bad; ++i) {
                const std::string& t = w[i];
                double v;
                if (t == "nibble_lsb") c.nibbleLsb = true;
                else if (t == "digits_lsd") c.digitsLsd = true;
                else if (t == "poly" || t == "init" || t == "xorout" || t == "width") {
                    uint64_t u;
                    if (i + 1 >= w.size() || !detail::parseU64(w[i + 1], &u)) { err(lineNo, "'" + t + "' needs a number"); bad = true; break; }
                    ++i;
                    if (t == "poly") c.crc.poly = u;
                    else if (t == "init") c.crc.init = u;
                    else if (t == "xorout") c.crc.xorout = u;
                    else { if (u < 1 || u > 64) { err(lineNo, "width must be 1..64"); bad = true; break; } c.crc.width = static_cast<unsigned>(u); }
                } else if (t == "refin" || t == "refout") {
                    bool b2;
                    if (i + 1 >= w.size() || !detail::parseYesNo(w[i + 1], &b2)) { err(lineNo, "'" + t + "' needs yes or no"); bad = true; break; }
                    ++i;
                    (t == "refin" ? c.crc.refin : c.crc.refout) = b2;
                } else { (void)v; err(lineNo, "unknown check attribute '" + t + "'"); bad = true; }
            }
            if (bad) continue;
            if (c.kind == SpecCheckKind::Crc) {
                if (c.crc.width == 0) { err(lineNo, "a crc check needs 'width'"); continue; }
                if (c.crc.width != c.len) { err(lineNo, "crc width and the check length must be equal"); continue; }
            }
            if (c.kind == SpecCheckKind::NibbleSum && c.len % 4) { err(lineNo, "a nibblesum check length must be a multiple of 4"); continue; }
            sp.check = c;
        } else {
            err(lineNo, "unknown statement '" + k + "'");
        }
    }
    if (!sawVersion && res.errors.empty()) err(0, "the spec is empty: it must begin with 'foxsdr-bitspec 1'");
    if (sawVersion) {
        if (!sawCoding) err(0, "no 'coding' statement");
        if (sp.coding == SpecCoding::Chips && sp.bitUs <= 0 && !sp.bitUsAuto && sawCoding) err(0, "a chips coding needs 'bit_us N' or 'bit_us auto'");
        if ((sp.coding == SpecCoding::Pwm || sp.coding == SpecCoding::Ppm) && (sp.shortUs <= 0 || sp.longUs <= 0) && sawCoding)
            err(0, "pwm/ppm need 'short_us' and 'long_us'");
        if (sp.shortUs > 0 && sp.longUs > 0 && sp.shortUs >= sp.longUs) err(0, "short_us must be smaller than long_us");
        if (sp.minBits > sp.maxBits) err(0, "min_bits is larger than max_bits");
        if (sp.frameBits && sp.frameBits > sp.maxBits) err(0, "frame_bits is larger than max_bits");
        if (sp.coding != SpecCoding::Chips && sp.stages.size() > 0) err(0, "'stage' needs 'coding chips'");
        const long span = static_cast<long>(sp.frameBits ? sp.frameBits : sp.maxBits);
        for (const auto& f : sp.fields) {
            const long a = f.pos >= 0 ? f.pos : (sp.frameBits ? span + f.pos : f.pos);
            if (f.pos >= 0 && a + static_cast<long>(f.len) > span) err(f.line, "field '" + f.name + "' ends beyond the frame (" + std::to_string(span) + " bits)");
        }
    }
    // Balancing the slicer's mark/space bias suits a clocked code (marks and spaces are the same kind of thing);
    // for pulse width / pulse position a mark and a space mean different things, so it is off unless asked for.
    if (!sawBalance && sp.coding != SpecCoding::Chips) sp.balance = false;
    res.ok = res.errors.empty();
    return res;
}

// ---- the interpreter ------------------------------------------------------------------------------------------

struct SpecValue {
    std::string name;
    uint64_t raw = 0;
    double value = 0;
    std::string text;
    bool valid = true;  // false: a bcd digit above 9, or outside min/max
};

enum class SpecCheckResult : uint8_t { NotChecked, Ok, Bad };

struct SpecMessage {
    double startUs = 0;
    Bits frame;                 // bits after the preamble
    size_t preamblePos = 0;
    unsigned preambleDist = 0;
    bool inverted = false;
    size_t violations = 0;
    std::vector<SpecValue> values;
    SpecCheckResult check = SpecCheckResult::NotChecked;
    bool plausible = true;
};

struct SpecStats {
    size_t candidates = 0;      // message-shaped stretches examined
    size_t noPreamble = 0, tooShort = 0, tooManyViolations = 0, badCheck = 0;
};

namespace detail {

inline long resolve(long p, size_t n) { return p >= 0 ? p : static_cast<long>(n) + p; }

// The bits [pos, pos+len) of the frame with the field's reading rules applied; ok false when out of range.
inline bool fieldBits(const Bits& frame, long pos, unsigned len, bool lsb, bool nibbleLsb, bool digitsLsd, Bits* out) {
    const long a = resolve(pos, frame.size());
    if (a < 0 || static_cast<size_t>(a) + len > frame.size()) return false;
    Bits b = slice(frame, static_cast<size_t>(a), len);
    if (lsb) std::reverse(b.begin(), b.end());
    if (nibbleLsb)
        for (size_t i = 0; i + 4 <= b.size(); i += 4) std::reverse(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i + 4));
    if (digitsLsd) {
        Bits r;
        for (size_t i = b.size(); i >= 4; i -= 4) r.insert(r.end(), b.begin() + static_cast<std::ptrdiff_t>(i - 4), b.begin() + static_cast<std::ptrdiff_t>(i));
        b = std::move(r);
    }
    *out = std::move(b);
    return true;
}

inline std::string fmtG(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

inline SpecValue decodeField(const SpecField& f, const Bits& frame, bool* inRange) {
    SpecValue v;
    v.name = f.name;
    Bits b;
    if (!fieldBits(frame, f.pos, f.len, f.lsb, f.nibbleLsb, f.digitsLsd, &b)) { v.valid = false; v.text = "(outside the frame)"; *inRange = false; return v; }
    const uint64_t raw = getField(b, 0, b.size(), true);
    v.raw = raw;
    switch (f.type) {
        case SpecType::Uint: v.value = static_cast<double>(raw); break;
        case SpecType::Int: {
            int64_t s = static_cast<int64_t>(raw);
            if (f.len < 64 && (raw >> (f.len - 1)) & 1u) s = static_cast<int64_t>(raw) - (1ll << f.len);
            v.value = static_cast<double>(s);
            break;
        }
        case SpecType::Bcd: {
            double d = 0;
            for (size_t i = 0; i < b.size(); i += 4) {
                const unsigned n = static_cast<unsigned>(getField(b, i, 4, true));
                if (n > 9) v.valid = false;
                d = d * 10 + n;
            }
            v.value = d;
            break;
        }
        case SpecType::Hex: v.value = static_cast<double>(raw); break;
        case SpecType::Flag: v.value = raw ? 1 : 0; break;
        case SpecType::Bits: v.value = static_cast<double>(raw); break;
    }
    v.value = v.value * f.scale + f.offset;
    if (f.hasMin && v.value < f.minV) v.valid = false;
    if (f.hasMax && v.value > f.maxV) v.valid = false;
    if (f.type == SpecType::Hex) {
        char t[24];
        std::snprintf(t, sizeof t, "0x%0*llX", static_cast<int>((f.len + 3) / 4), static_cast<unsigned long long>(raw));
        v.text = t;
    } else if (f.type == SpecType::Bits) {
        v.text = toString(b);
    } else if (f.type == SpecType::Flag) {
        v.text = raw ? "1" : "0";
    } else {
        v.text = fmtG(v.value) + (f.unit.empty() ? "" : " " + f.unit);
    }
    *inRange = true;
    return v;
}

inline SpecCheckResult runCheck(const SpecCheck& c, const Bits& frame) {
    if (c.kind == SpecCheckKind::None) return SpecCheckResult::NotChecked;
    const long a = resolve(c.from, frame.size()), z = resolve(c.to, frame.size());
    if (a < 0 || z < a || static_cast<size_t>(z) > frame.size()) return SpecCheckResult::Bad;
    Bits stored;
    if (!fieldBits(frame, c.at, c.len, false, c.nibbleLsb, c.digitsLsd, &stored)) return SpecCheckResult::Bad;
    const uint64_t want = getField(stored, 0, stored.size(), true);
    const Bits range = slice(frame, static_cast<size_t>(a), static_cast<size_t>(z - a));
    uint64_t got = 0;
    switch (c.kind) {
        case SpecCheckKind::NibbleSum: {
            uint64_t s = 0;
            for (size_t i = 0; i + 4 <= range.size(); i += 4) {
                Bits n = slice(range, i, 4);
                if (c.nibbleLsb) std::reverse(n.begin(), n.end());
                s += getField(n, 0, 4, true);
            }
            got = s & (c.len >= 64 ? ~0ull : ((1ull << c.len) - 1));
            break;
        }
        case SpecCheckKind::Sum8:
        case SpecCheckKind::Xor8: {
            uint64_t s = 0;
            for (size_t i = 0; i + 8 <= range.size(); i += 8) {
                const uint64_t by = getField(range, i, 8, true);
                s = c.kind == SpecCheckKind::Sum8 ? s + by : s ^ by;
            }
            got = s & (c.len >= 64 ? ~0ull : ((1ull << c.len) - 1));
            break;
        }
        case SpecCheckKind::Crc: {
            bool ok = true;
            got = crcCompute(c.crc, range, &ok);
            if (!ok) return SpecCheckResult::Bad;
            break;
        }
        case SpecCheckKind::None: break;
    }
    return got == want ? SpecCheckResult::Ok : SpecCheckResult::Bad;
}

// Decode one message's runs to bits per the spec; returns violations (line-code violations + runs off the chip grid).
inline bool decodeBits(const BitSpec& sp, const std::vector<LevelRun>& runs, const Tri invertTry, Bits* bits, size_t* violations) {
    *violations = 0;
    if (sp.coding == SpecCoding::Chips) {
        double T = 0;
        const size_t cpb0 = chipsPerBit(sp.stages.front());
        if (sp.bitUs > 0) T = sp.bitUs / static_cast<double>(cpb0);
        else T = estimateChipPeriod(runs);
        if (!(T > 0)) return false;
        const double tol = sp.tolerancePct / 100.0;
        size_t off = 0;
        for (const auto& r : runs) {
            const double q = r.duration / T;
            const double n = std::max(1.0, std::round(q));
            if (std::fabs(q - n) > tol + 0.0) ++off;
        }
        *violations += off;
        Bits cur = runsToChips(runs, T, 1u << 16);
        for (size_t s = 0; s < sp.stages.size(); ++s) {
            const LineCode lc = sp.stages[s];
            Decoded best;
            bool have = false;
            for (size_t ph = 0; ph < chipsPerBit(lc); ++ph) {
                Decoded d = decodeLineCode(cur, lc, ph, 0);
                if (!have || d.violations < best.violations) { best = std::move(d); have = true; }
            }
            if (!have) return false;
            *violations += best.violations;
            cur = std::move(best.bits);
        }
        *bits = invertTry == Tri::Yes ? inverted(cur) : cur;
        return true;
    }
    // pwm / ppm: pair each mark with the space after it
    const bool useSpace = sp.coding == SpecCoding::Ppm;
    const double tol = sp.tolerancePct / 100.0;
    Bits out;
    for (size_t i = 0; i + 1 < runs.size() || (i < runs.size() && !useSpace); ++i) {
        if (runs[i].level != 1) continue;
        double w;
        if (useSpace) {
            if (i + 1 >= runs.size()) break;
            w = runs[i + 1].duration;
        } else {
            w = runs[i].duration;
        }
        unsigned bit = 2;
        if (std::fabs(w - sp.shortUs) <= tol * sp.shortUs) bit = 0;
        else if (std::fabs(w - sp.longUs) <= tol * sp.longUs) bit = 1;
        if (bit == 2) { ++*violations; bit = std::fabs(w - sp.shortUs) < std::fabs(w - sp.longUs) ? 0u : 1u; }
        out.push_back(static_cast<uint8_t>(invertTry == Tri::Yes ? bit ^ 1u : bit));
        if (out.size() >= (1u << 16)) break;
    }
    *bits = std::move(out);
    return true;
}

}  // namespace detail

inline std::vector<SpecMessage> runSpec(const BitSpec& sp, const std::vector<LevelRun>& runsIn, SpecStats* stats = nullptr, size_t maxMessages = 256) {
    std::vector<SpecMessage> out;
    SpecStats st;
    if (sp.version != 1 || runsIn.empty() || runsIn.size() > (1u << 20)) { if (stats) *stats = st; return out; }
    // split into messages at spaces of at least resetUs
    std::vector<std::pair<size_t, size_t>> msgs;  // [begin, end) run indices
    std::vector<double> startUs;
    {
        double t = 0;
        size_t b = 0;
        bool open = false;
        double bt = 0;
        for (size_t i = 0; i < runsIn.size(); ++i) {
            const bool gap = runsIn[i].level == 0 && runsIn[i].duration >= sp.resetUs;
            if (gap) {
                if (open) { msgs.push_back({b, i}); startUs.push_back(bt); open = false; }
            } else if (!open) { b = i; bt = t; open = true; }
            t += runsIn[i].duration;
        }
        if (open) { msgs.push_back({b, runsIn.size()}); startUs.push_back(bt); }
    }
    for (size_t mi = 0; mi < msgs.size() && out.size() < maxMessages; ++mi) {
        std::vector<LevelRun> runs(runsIn.begin() + static_cast<std::ptrdiff_t>(msgs[mi].first), runsIn.begin() + static_cast<std::ptrdiff_t>(msgs[mi].second));
        if (runs.size() < 8) continue;
        ++st.candidates;
        if (sp.balance) runs = balanceRuns(std::move(runs));
        bool accepted = false;
        const Tri tries[2] = {sp.invert == Tri::Yes ? Tri::Yes : Tri::No, Tri::Yes};
        const int nTries = sp.invert == Tri::Auto ? 2 : 1;
        SpecStats local;
        for (int ti = 0; ti < nTries && !accepted; ++ti) {
            Bits bits;
            size_t viol = 0;
            if (!detail::decodeBits(sp, runs, tries[ti], &bits, &viol)) continue;
            if (viol > sp.maxViolations) { local.tooManyViolations = 1; continue; }
            size_t at = 0, dist = 0;
            if (!sp.preamble.empty()) {
                // the best hit (fewest differing bits, then earliest) among the first 64 candidates
                const auto hits = findPattern(bits, sp.preamble, sp.preambleErrors, false, 64);
                if (hits.empty()) { local.noPreamble = 1; continue; }
                size_t bi = 0;
                for (size_t h = 1; h < hits.size(); ++h)
                    if (hits[h].distance < hits[bi].distance) bi = h;
                at = hits[bi].pos + sp.preamble.size();
                dist = hits[bi].distance;
            }
            Bits frame = slice(bits, at, bits.size());
            if (sp.frameBits) {
                if (frame.size() < sp.frameBits) { local.tooShort = 1; continue; }
                frame.resize(sp.frameBits);
            } else {
                if (frame.size() > sp.maxBits) frame.resize(sp.maxBits);
                if (frame.size() < sp.minBits) { local.tooShort = 1; continue; }
            }
            SpecMessage m;
            m.startUs = startUs[mi];
            m.frame = frame;
            m.preamblePos = at - sp.preamble.size();
            m.preambleDist = static_cast<unsigned>(dist);
            m.inverted = tries[ti] == Tri::Yes;
            m.violations = viol;
            for (const auto& f : sp.fields) {
                bool inRange = true;
                SpecValue v = detail::decodeField(f, frame, &inRange);
                if (!v.valid) m.plausible = false;
                m.values.push_back(std::move(v));
            }
            m.check = detail::runCheck(sp.check, frame);
            if (m.check == SpecCheckResult::Bad) { local.badCheck = 1; continue; }
            out.push_back(std::move(m));
            accepted = true;
        }
        if (!accepted) {
            st.noPreamble += local.noPreamble;
            st.tooShort += local.tooShort;
            st.tooManyViolations += local.tooManyViolations;
            st.badCheck += local.badCheck;
        }
    }
    if (stats) *stats = st;
    return out;
}

inline const SpecValue* findValue(const SpecMessage& m, const std::string& name) {
    for (const auto& v : m.values)
        if (v.name == name) return &v;
    return nullptr;
}

}  // namespace cascade::bits
