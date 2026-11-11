// The JSON shape of one decoded message, for the MQTT publisher and the webhook:
//
//   {"plugin":"ft8","time":"2026-11-11T16:20:24.123Z","unix_ms":1794414024123,"frequency_hz":14074000,
//    "text":"CQ DX K1ABC FN42","fields":{"snr":-12,"call":"K1ABC"}}
//
// - `text` is cut at maxTextBytes on a character boundary and then "truncated":true is added;
// - `fields` is an object; a later field with a key already seen is dropped (JSON allows duplicates, receivers disagree about them);
// - a non-finite number is written as null; the frequency is omitted when it is not a finite number (a decode with no frequency).
//
// ESCAPING (RFC 8259 section 7): `"` and `\` are backslash-escaped, the control characters U+0000..U+001F as \b \f \n \r \t or \u00XX.
// Decoded text comes off a radio and may hold ANY bytes, so the string is first made valid UTF-8: every byte that is not part of a well-formed
// sequence (overlong forms, surrogates, code points above U+10FFFF, truncated sequences) becomes U+FFFD, so the output is always valid JSON
// in valid UTF-8 whatever went in.
//
// Pure, no state; allocates only the result.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/waterfall_timing.hpp"

namespace cascade::core::decodejson {

struct Field {
    std::string key;
    std::string text;
    double number = 0.0;
    bool isNumber = false;
};

struct DecodeEvent {
    std::string plugin;
    std::int64_t unixMs = 0;
    double frequencyHz = std::nan("");
    std::string text;
    std::vector<Field> fields;
};

namespace detail {

// Length of the well-formed UTF-8 sequence at s[i], or 0 if there is none (RFC 3629 table: no overlongs, no surrogates, max U+10FFFF).
inline std::size_t utf8Len(std::string_view s, std::size_t i) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const std::size_t n = s.size() - i;
    const unsigned c = b(i);
    if (c < 0x80) { return 1; }
    auto cont = [&](std::size_t k) { return k < s.size() && (b(k) & 0xC0) == 0x80; };
    if (c >= 0xC2 && c <= 0xDF) { return (n >= 2 && cont(i + 1)) ? 2 : 0; }
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || !cont(i + 1) || !cont(i + 2)) { return 0; }
        const unsigned c1 = b(i + 1);
        if (c == 0xE0 && c1 < 0xA0) { return 0; }   // overlong
        if (c == 0xED && c1 >= 0xA0) { return 0; }  // surrogate
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || !cont(i + 1) || !cont(i + 2) || !cont(i + 3)) { return 0; }
        const unsigned c1 = b(i + 1);
        if (c == 0xF0 && c1 < 0x90) { return 0; }   // overlong
        if (c == 0xF4 && c1 >= 0x90) { return 0; }  // above U+10FFFF
        return 4;
    }
    return 0;
}

inline void appendEscaped(std::string& out, std::string_view s) {
    static const char* hex = "0123456789abcdef";
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20 || c == 0x7F) {  // DEL is legal JSON but a control character: escaped so no log or terminal trips on it
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 15];
                    } else {
                        out += static_cast<char>(c);
                    }
            }
            ++i;
            continue;
        }
        const std::size_t len = utf8Len(s, i);
        if (len == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
        } else {
            out.append(s.data() + i, len);
            i += len;
        }
    }
}

inline std::string numberText(double v) {
    if (!std::isfinite(v)) { return "null"; }
    char buf[40];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

}  // namespace detail

// The escaped CONTENT of a JSON string (no surrounding quotes).
inline std::string escapeString(std::string_view s) {
    std::string o;
    o.reserve(s.size() + 8);
    detail::appendEscaped(o, s);
    return o;
}

inline std::string isoUtc(std::int64_t unixMs) {
    const wftime::Civil c = wftime::civilOf(unixMs, wftime::Zone::utc());
    char b[40];
    std::snprintf(b, sizeof b, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", static_cast<long long>(c.year), c.month, c.day, c.hour, c.minute, c.second,
                  c.millis);
    return b;
}

inline std::string toJson(const DecodeEvent& e, std::size_t maxTextBytes = 2000) {
    std::string o = "{\"plugin\":\"";
    detail::appendEscaped(o, e.plugin);
    o += "\",\"time\":\"" + isoUtc(e.unixMs) + "\",\"unix_ms\":" + std::to_string(e.unixMs);
    if (std::isfinite(e.frequencyHz)) { o += ",\"frequency_hz\":" + detail::numberText(e.frequencyHz); }
    std::string_view text = e.text;
    bool truncated = false;
    if (text.size() > maxTextBytes) {
        std::size_t cut = maxTextBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) { --cut; }  // do not split a character
        text = text.substr(0, cut);
        truncated = true;
    }
    o += ",\"text\":\"";
    detail::appendEscaped(o, text);
    o += "\"";
    if (truncated) { o += ",\"truncated\":true"; }
    o += ",\"fields\":{";
    std::vector<std::string_view> seen;
    bool first = true;
    for (const Field& f : e.fields) {
        bool dup = false;
        for (std::string_view k : seen) { if (k == f.key) { dup = true; break; } }
        if (dup) { continue; }
        seen.push_back(f.key);
        if (!first) { o += ","; }
        first = false;
        o += "\"";
        detail::appendEscaped(o, f.key);
        o += "\":";
        if (f.isNumber) {
            o += detail::numberText(f.number);
        } else {
            o += "\"";
            detail::appendEscaped(o, f.text);
            o += "\"";
        }
    }
    o += "}}";
    return o;
}

}  // namespace cascade::core::decodejson
