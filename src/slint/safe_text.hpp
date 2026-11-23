// safe_text.hpp - the one place text becomes a slint::SharedString.
//
// WHY. slint::SharedString checks its bytes with Rust's str::from_utf8().unwrap().
// One byte that is not UTF-8 - a Windows-1252 0xF3 in a localised Windows
// sentence, junk after a firmware's version string - and slint_cpp.dll panics
// and the process is gone (0.99.112, crash groups 6CCFD0C0E58FC30C and
// 104F425230C2A742: "Utf8Error { valid_up_to: 8, error_len: Some(1) }").
// Every publisher's ss() helper calls shared() below, so text the program did
// not write cannot reach a property or a model row unchecked. The one entry to
// Slint that takes a std::string_view straight to Rust, select_bundled_translation,
// has its own guard here (selectBundledTranslation).
//
// WHAT IT DOES. Valid text passes through byte for byte (checked once, no copy,
// no log). Invalid bytes become U+FFFD, and the FIRST time a given CALL SITE
// (the caller's file and line, from std::source_location, carried by every
// ss() helper) meets bad text, one log line says where and what kind of fault:
//
//   slint text: site signal_publish:931: first bad byte at 8 of 17, value f3, before a, after a, looks like system ANSI code page, code page 1252, 1 bad byte(s)
//
// THE LINE CARRIES NO TEXT. A log line can be uploaded in a report, and the text
// is a device name, a file name or something the user typed. So the line holds
// the site, the offset and total length, the value of the bad byte, and the byte
// before and after it ONLY when it is not an ASCII letter or digit (those print
// as "a"; any other is two hex digits, "none" at an end of the text). At most 16
// sites are reported per run, then one line says so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef FOXSDR_SLINT_SAFE_TEXT_HPP
#define FOXSDR_SLINT_SAFE_TEXT_HPP

#include <slint.h>

#include <cstdio>
#include <mutex>
#include <set>
#include <source_location>
#include <string>
#include <string_view>

#include "core/diag_log.hpp"
#include "safe_text_make.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintsafe {

// How many different call sites may log a repair in one run.
inline constexpr std::size_t kMaxReportedSites = 16;

// The sites already reported (and whether the cap notice was given), for the test to look at and reset.
struct SiteLog {
    std::mutex mutex;
    std::set<std::string> sites;
    bool capNoticeGiven = false;
};
inline SiteLog& siteLog() {
    static SiteLog s;
    return s;
}

// "signal_publish:931": the source file's base name without its extension, and the line.
inline std::string siteName(const char* file, unsigned line) {
    std::string where = file != nullptr ? file : "?";
    const std::size_t slash = where.find_last_of("/\\");
    if (slash != std::string::npos) { where.erase(0, slash + 1); }
    const std::size_t dot = where.find_last_of('.');
    if (dot != std::string::npos && dot != 0) { where.erase(dot); }
    return where + ":" + std::to_string(line);
}

// What the first bad byte looks like, from the bytes and the machine's code page alone.
inline const char* classifyBadText(std::string_view s, std::size_t firstBad, unsigned codePage) {
    const auto b = static_cast<unsigned char>(s[firstBad]);
    if (b >= 0xC2 && b <= 0xF4) {
        // a valid lead byte followed only by continuation bytes up to the END of the text, fewer than it needs: the text was cut inside a letter
        const std::size_t need = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2);
        std::size_t k = firstBad + 1;
        while (k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0u) == 0x80u) { ++k; }
        if (k == s.size() && k - firstBad < need) { return "cut-off UTF-8"; }
    }
    if (cascade::core::isDoubleByteCodePage(codePage)) {
        return cascade::core::isDbcsLeadByte(codePage, b) ? "double-byte code page" : "other";
    }
    if (codePage != 0 && codePage != 65001 && b >= 0x80) { return "system ANSI code page"; }
    return "other";
}

// A neighbour of the bad byte for the log: "a" for an ASCII letter or digit, "none" past an end, else its value as two hex digits.
inline std::string neighbourText(std::string_view s, std::size_t at, bool exists) {
    if (!exists || at >= s.size()) { return "none"; }
    const auto c = static_cast<unsigned char>(s[at]);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { return "a"; }
    char hex[4] = {0};
    std::snprintf(hex, sizeof(hex), "%02x", static_cast<unsigned>(c));
    return hex;
}

// The report line for invalid text `s` (never a byte of the text beyond the three it names). `badBytes` and `codePage` as measured.
inline std::string badTextLine(std::string_view s, std::size_t firstBad, std::size_t badBytes, const std::string& site, unsigned codePage) {
    char head[96] = {0};
    std::snprintf(head, sizeof(head), "first bad byte at %zu of %zu, value %02x", firstBad, s.size(), static_cast<unsigned>(static_cast<unsigned char>(s[firstBad])));
    char tail[96] = {0};
    std::snprintf(tail, sizeof(tail), "code page %u, %zu bad byte(s)", codePage, badBytes);
    return "slint text: site " + site + ": " + head + ", before " + neighbourText(s, firstBad - 1, firstBad > 0) + ", after " + neighbourText(s, firstBad + 1, true) +
           ", looks like " + classifyBadText(s, firstBad, codePage) + ", " + tail;
}

// Invalid text `s` as valid UTF-8, reported once per call site (file, line), at most kMaxReportedSites sites per run.
inline std::string repairAndReport(std::string_view s, const char* file, unsigned line) {
    // ANSI FIRST: Windows' and the STL's own sentences arrive in the machine's code page, so the bytes that are not UTF-8 are read there and the
    // reader sees the right accented letters; U+FFFD only if that gave nothing valid.
    std::string fixed = cascade::core::utf8OrAnsiToUtf8(s);
    if (!cascade::core::isValidUtf8(fixed)) { fixed = cascade::core::sanitizeUtf8(s); }
    const std::string site = siteName(file, line);
    bool report = false;
    bool capNotice = false;
    {
        SiteLog& log = siteLog();
        std::lock_guard<std::mutex> lk(log.mutex);
        if (log.sites.size() < kMaxReportedSites) {
            report = log.sites.insert(site).second;
        } else if (log.sites.count(site) == 0 && !log.capNoticeGiven) {
            log.capNoticeGiven = true;
            capNotice = true;
        }
    }
    if (report) {
        cascade::core::Utf8Repair r;
        (void)cascade::core::sanitizeUtf8(s, &r);   // where the first bad byte is, and how many there are
        cascade::core::diagLogf("%s", badTextLine(s, r.firstBad, r.badBytes, site, cascade::core::systemAnsiCodePage()).c_str());
    } else if (capNotice) {
        cascade::core::diagLogf("slint text: %zu call sites reported, further sites are repaired but not logged", kMaxReportedSites);
    }
    return fixed;
}

// `s` as valid UTF-8 (a copy), logging once per call site when it had to be repaired.
inline std::string validAt(std::string_view s, const char* file, unsigned line) {
    if (cascade::core::isValidUtf8(s)) { return std::string(s); }
    return repairAndReport(s, file, line);
}

// `s` as a SharedString. The call site comes from the caller's std::source_location (a constant, no allocation): valid text costs one
// scan and the one copy into the SharedString.
inline slint::SharedString shared(std::string_view s, std::source_location loc = std::source_location::current()) {
    if (cascade::core::isValidUtf8(s)) { return Make::from(s); }
    return Make::from(repairAndReport(s, loc.file_name(), loc.line()));
}

// A key event's text (the scripted input's `type`): the script is the author's, but a `type` step can carry any bytes.
inline slint::SharedString keyEventText(std::u8string_view t, std::source_location loc = std::source_location::current()) {
    return shared(std::string_view(reinterpret_cast<const char*>(t.data()), t.size()), loc);
}

// ---- the language code ------------------------------------------------------------------------------------------------------------------
// slint::select_bundled_translation takes its std::string_view straight to Rust, which unwraps its UTF-8 check (translations.rs, from_utf8().unwrap()),
// so it is called ONLY through selectBundledTranslation below (tools/slint_text_sites.py fails the build's scan for any other call).

// A plausible language code: 2 to 16 bytes, ASCII letters, digits, '-' and '_' ("en", "pt-BR", "zh_Hans_CN").
inline bool plausibleLanguageCode(std::string_view code) {
    if (code.size() < 2 || code.size() > 16 || !cascade::core::isValidUtf8(code)) { return false; }
    for (const char ch : code) {
        const bool ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_';
        if (!ok) { return false; }
    }
    return true;
}

// slint::select_bundled_translation(code) for a plausible code; for anything else one log line (its length, never the text) and the language
// in force is kept (false: "not bundled").
inline bool selectBundledTranslation(std::string_view code) {
    if (!plausibleLanguageCode(code)) {
        cascade::core::diagLogf("slint text: language code refused (%zu bytes, not a plausible code), the current language is kept", code.size());
        return false;
    }
    return slint::select_bundled_translation(code);
}

}  // namespace foxsdr::slintsafe

#endif  // FOXSDR_SLINT_SAFE_TEXT_HPP
