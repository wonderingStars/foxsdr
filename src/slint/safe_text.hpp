// safe_text.hpp - the one place text becomes a slint::SharedString.
//
// WHY. slint::SharedString checks its bytes with Rust's str::from_utf8().unwrap().
// One byte that is not UTF-8 - a Windows-1252 0xF3 in a localised Windows
// sentence, junk after a firmware's version string - and slint_cpp.dll panics
// and the process is gone (0.99.112, crash groups 6CCFD0C0E58FC30C and
// 104F425230C2A742: "Utf8Error { valid_up_to: 8, error_len: Some(1) }").
// Every publisher's ss() helper calls shared() below, so text the program did
// not write cannot reach a property or a model row unchecked.
//
// WHAT IT DOES. Valid text passes through byte for byte. Invalid bytes become
// U+FFFD, and the FIRST time a given call site (the translation unit's
// __FILE__) meets bad text, one log line says where and shows the first 32
// bytes from the first bad one as hex - never the text around them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef FOXSDR_SLINT_SAFE_TEXT_HPP
#define FOXSDR_SLINT_SAFE_TEXT_HPP

#include <slint.h>

#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "core/diag_log.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintsafe {

// The sites already reported, for the test to look at.
inline std::set<std::string>& reportedSites() {
    static std::set<std::string> s;
    return s;
}
inline std::mutex& reportedMutex() {
    static std::mutex m;
    return m;
}

// `s` as valid UTF-8, logging once per `site` when it had to be repaired.
inline std::string valid(std::string_view s, const char* site) {
    if (cascade::core::isValidUtf8(s)) { return std::string(s); }
    cascade::core::Utf8Repair r;
    std::string fixed = cascade::core::sanitizeUtf8(s, &r);
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(reportedMutex());
        first = reportedSites().insert(site != nullptr ? site : "?").second;
    }
    if (first) {
        // The file's own name (a log line is 191 bytes and the hex is 95 of them).
        std::string where = site != nullptr ? site : "?";
        const std::size_t slash = where.find_last_of("/\\");
        if (slash != std::string::npos) { where.erase(0, slash + 1); }
        cascade::core::diagLogf("slint text: %zu bad byte(s) in %s at %zu/%zu: %s", r.badBytes, where.c_str(), r.firstBad, s.size(),
                                cascade::core::hexOfBytes(s, r.firstBad, 32).c_str());
    }
    return fixed;
}

inline slint::SharedString shared(std::string_view s, const char* site) {
    if (cascade::core::isValidUtf8(s)) { return slint::SharedString(s); }  // the usual case: no copy
    return slint::SharedString(std::string_view(valid(s, site)));
}

}  // namespace foxsdr::slintsafe

#endif  // FOXSDR_SLINT_SAFE_TEXT_HPP
