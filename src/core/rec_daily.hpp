// Daily subfolders for recordings: a date-folder extension for the recording file-name templates, and a folder creator that proves every folder it makes
// is inside the recordings root.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). feat/recording-models' core/rec_naming.hpp already expands file-name templates
// (`{date}`, `{time}`, ... and up to three folder levels) and has resolvePath() for the file itself; that header is not in this base, so this one has its
// own small template engine and checks and the join is: recname::expand() gives the file's own relative folders (the radio name, say), this header gives
// the date folder, and ensureFolder() creates root/<subdir>/<date folder> before the recorder opens the file in it.
//
// DATE FOLDER TEMPLATE. Tokens `{yyyy}` `{mm}` `{dd}` `{date}` (yyyy-mm-dd) `{compact}` (yyyymmdd); `{{` and `}}` are literal braces; `/` or `\` separate
// levels. Presets: DashDate "{date}", SlashDate "{yyyy}/{mm}/{dd}", MonthThenDay "{yyyy}-{mm}/{dd}", Compact "{compact}", None "" (no date folder).
// UTC OR LOCAL: the choice is the caller's (Clock::Utc or Clock::Local with the local offset in minutes east of UTC, for the instant, daylight saving
// included; this layer is pure and never asks the system). The default is UTC, so a recording started 23:30 UTC on 31 December sits in the folder of 31
// December wherever the machine is; with Local it sits in the local date. A day boundary crossed DURING a recording does not move the file (the folder is
// decided once, at the start); a rollover file started after midnight asks again.
//
// SAFETY. Every relative path is validated as a whole: at most six levels, no empty level, no `.` or `..`, no drive or leading separator (so no absolute or
// UNC form), none of the characters Windows forbids (`< > : " | ? *`) or control characters, no trailing dot or space and no leading space (Windows strips
// them), no reserved device name (CON PRN AUX NUL COM0-9 LPT0-9, any case, with or without an extension), well-formed UTF-8 only, 64 bytes per level, 200
// bytes in all. ensureFolder() additionally requires the root to exist already (it never creates the root), creates one level at a time, refuses a level
// that already exists as a link, junction or file, and checks after each creation that the canonical path is still inside the canonical root.
//
// THREADS: pure functions, and ensureFolder() is safe to call from several threads for the same day (create_directory of an existing folder is a success).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace cascade::core::recdaily {

enum class Style : int { None = 0, DashDate, SlashDate, MonthThenDay, Compact };
enum class Clock : int { Utc = 0, Local = 1 };

struct Options {
    Style style = Style::DashDate;
    std::string customTemplate;          // when not empty it replaces the preset
    Clock clock = Clock::Utc;
    int localOffsetMinutes = 0;          // used with Clock::Local; -14:00 .. +14:00
};

struct Expanded {
    bool ok = false;
    std::string rel;                     // forward slashes, no leading or trailing separator; empty for Style::None
    std::string error;
};

namespace detail {

inline std::string presetTemplate(Style s) {
    switch (s) {
        case Style::None: return "";
        case Style::DashDate: return "{date}";
        case Style::SlashDate: return "{yyyy}/{mm}/{dd}";
        case Style::MonthThenDay: return "{yyyy}-{mm}/{dd}";
        case Style::Compact: return "{compact}";
    }
    return "";
}

// Days since 1970-01-01 to civil date (proleptic Gregorian), after Howard Hinnant's published algorithm (public domain).
inline void civilFromDays(long long z, int* y, int* m, int* d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yy + (*m <= 2 ? 1 : 0));
}

inline long long floorDiv(long long a, long long b) { long long q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) { --q; } return q; }

inline std::string two(int v) { std::string s = "00"; s[0] = static_cast<char>('0' + v / 10 % 10); s[1] = static_cast<char>('0' + v % 10); return s; }
inline std::string four(int v) {
    std::string s = "0000";
    for (int i = 3; i >= 0; --i) { s[static_cast<std::size_t>(i)] = static_cast<char>('0' + v % 10); v /= 10; }
    return s;
}

inline bool validUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else { return false; }
        if (i + n >= s.size()) { return false; }
        for (std::size_t k = 1; k <= n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { return false; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) { return false; }
        i += n + 1;
    }
    return true;
}

inline std::string upperAscii(std::string s) { for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); } return s; }

inline bool reservedName(const std::string& comp) {
    const std::size_t dot = comp.find('.');
    std::string stem = upperAscii(dot == std::string::npos ? comp : comp.substr(0, dot));
    while (!stem.empty() && stem.back() == ' ') { stem.pop_back(); }
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$") { return true; }
    if (stem.size() == 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0) && stem[3] >= '0' && stem[3] <= '9') { return true; }
    return false;
}

}  // namespace detail

constexpr std::size_t kMaxLevels = 6;
constexpr std::size_t kMaxComponentBytes = 64;
constexpr std::size_t kMaxTotalBytes = 200;

// Splits a relative path on '/' and '\' and validates every level. On success `parts` holds the levels; on failure `err` says which rule.
inline bool splitRelative(const std::string& rel, std::vector<std::string>* parts, std::string* err) {
    auto fail = [&](const std::string& m) { if (err != nullptr) { *err = m; } return false; };
    std::vector<std::string> out;
    if (rel.empty()) { if (parts != nullptr) { parts->clear(); } return true; }
    if (rel.size() > kMaxTotalBytes) { return fail("the folder path is longer than 200 bytes"); }
    if (!detail::validUtf8(rel)) { return fail("the folder path is not valid UTF-8"); }
    std::string cur;
    auto push = [&]() -> bool {
        if (cur.empty()) { return false; }
        out.push_back(cur);
        cur.clear();
        return true;
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            if (!push()) { return fail(out.empty() ? "an absolute or rooted folder path is not allowed" : "an empty folder level"); }
        } else {
            cur += c;
        }
    }
    if (!push()) { return fail("the folder path ends with a separator"); }
    if (out.size() > kMaxLevels) { return fail("more than six folder levels"); }
    for (const std::string& comp : out) {
        if (comp.size() > kMaxComponentBytes) { return fail("a folder name is longer than 64 bytes"); }
        if (comp == "." || comp == "..") { return fail("'.' and '..' are not allowed in a folder path"); }
        for (char ch : comp) {
            const unsigned char u = static_cast<unsigned char>(ch);
            if (u < 0x20 || u == 0x7F) { return fail("a control character in a folder name"); }
            if (std::string("<>:\"|?*").find(ch) != std::string::npos) { return fail(std::string("the character '") + ch + "' is not allowed in a folder name"); }
        }
        if (comp.front() == ' ') { return fail("a folder name starts with a space"); }
        if (comp.back() == ' ' || comp.back() == '.') { return fail("a folder name ends with a space or a dot"); }
        if (detail::reservedName(comp)) { return fail("a folder name is a reserved Windows device name"); }
    }
    if (parts != nullptr) { *parts = out; }
    return true;
}

inline std::string joinRelative(const std::vector<std::string>& parts) {
    std::string r;
    for (std::size_t i = 0; i < parts.size(); ++i) { if (i != 0) { r += '/'; } r += parts[i]; }
    return r;
}

// The date of an instant in the chosen clock; false when the offset is out of range or the year is outside 1900 .. 2200.
inline bool civilDate(long long unixMs, Clock clock, int offsetMinutes, int* y, int* m, int* d, std::string* err) {
    if (clock == Clock::Local && (offsetMinutes < -14 * 60 || offsetMinutes > 14 * 60)) {
        if (err != nullptr) { *err = "the local time offset is outside -14:00 .. +14:00"; }
        return false;
    }
    const long long shiftMs = clock == Clock::Local ? static_cast<long long>(offsetMinutes) * 60000LL : 0LL;
    if (unixMs > 9000000000000000LL || unixMs < -9000000000000000LL) { if (err != nullptr) { *err = "the time is out of range"; } return false; }
    const long long days = detail::floorDiv(unixMs + shiftMs, 86400000LL);
    detail::civilFromDays(days, y, m, d);
    if (*y < 1900 || *y > 2200) { if (err != nullptr) { *err = "the year is outside 1900 .. 2200"; } return false; }
    return true;
}

// Expands the date-folder template for an instant, then validates the result as a relative path. `subdir` (optional, already expanded by the
// file-name template layer) goes before the date folder and is validated with the same rules; it is refused, not repaired.
inline Expanded expandFolder(const Options& o, long long unixMs, const std::string& subdir = std::string()) {
    Expanded r;
    const std::string tmpl = !o.customTemplate.empty() ? o.customTemplate : detail::presetTemplate(o.style);
    int y = 0, m = 0, d = 0;
    std::string err;
    if (!civilDate(unixMs, o.clock, o.localOffsetMinutes, &y, &m, &d, &err)) { r.error = err; return r; }
    std::string out;
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        const char c = tmpl[i];
        if (c == '{') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { out += '{'; ++i; continue; }
            const std::size_t close = tmpl.find('}', i + 1);
            if (close == std::string::npos) { r.error = "an unclosed '{' in the date folder template"; return r; }
            const std::string tok = tmpl.substr(i + 1, close - i - 1);
            if (tok == "yyyy") { out += detail::four(y); }
            else if (tok == "mm") { out += detail::two(m); }
            else if (tok == "dd") { out += detail::two(d); }
            else if (tok == "date") { out += detail::four(y) + "-" + detail::two(m) + "-" + detail::two(d); }
            else if (tok == "compact") { out += detail::four(y) + detail::two(m) + detail::two(d); }
            else { r.error = "unknown token {" + tok.substr(0, 20) + "} in the date folder template"; return r; }
            i = close;
        } else if (c == '}') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') { out += '}'; ++i; continue; }
            r.error = "a stray '}' in the date folder template";
            return r;
        } else {
            out += c;
        }
    }
    std::vector<std::string> subParts, dateParts;
    if (!subdir.empty() && !splitRelative(subdir, &subParts, &err)) { r.error = "subfolder: " + err; return r; }
    if (!splitRelative(out, &dateParts, &err)) { r.error = err; return r; }
    subParts.insert(subParts.end(), dateParts.begin(), dateParts.end());
    if (subParts.size() > kMaxLevels) { r.error = "more than six folder levels"; return r; }
    r.rel = joinRelative(subParts);
    if (r.rel.size() > kMaxTotalBytes) { r.rel.clear(); r.error = "the folder path is longer than 200 bytes"; return r; }
    r.ok = true;
    return r;
}

struct Ensured {
    bool ok = false;
    std::filesystem::path dir;           // the folder that now exists
    int created = 0;                     // levels this call created
    std::string error;
};

namespace detail {

inline bool isLinkLike(const std::filesystem::file_status& st) {
    if (st.type() == std::filesystem::file_type::symlink) { return true; }
#if defined(_MSVC_STL_VERSION)
    if (st.type() == std::filesystem::file_type::junction) { return true; }
#endif
    return false;
}

// true when `inner` is `outer` or below it, comparing whole components of two canonical paths
inline bool isInside(const std::filesystem::path& outer, const std::filesystem::path& inner) {
    auto a = outer.begin(), ae = outer.end();
    auto b = inner.begin(), be = inner.end();
    for (; a != ae; ++a, ++b) {
        if (b == be) { return false; }
        if (a->empty()) { continue; }
        if (*a != *b) { return false; }
    }
    return true;
}

}  // namespace detail

// Creates root/<rel>, one level at a time. `rel` must pass splitRelative. The root must already exist and be a folder.
inline Ensured ensureFolder(const std::filesystem::path& root, const std::string& rel) {
    namespace fs = std::filesystem;
    Ensured e;
    std::vector<std::string> parts;
    std::string err;
    if (!splitRelative(rel, &parts, &err)) { e.error = err; return e; }
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec) || ec) { e.error = "the recordings folder does not exist"; return e; }
    const fs::path canonRoot = fs::canonical(root, ec);
    if (ec) { e.error = "cannot resolve the recordings folder"; return e; }
    fs::path cur = canonRoot;
    for (const std::string& comp : parts) {
        const fs::path next = cur / fs::path(std::u8string(comp.begin(), comp.end()));
        const fs::file_status st = fs::symlink_status(next, ec);
        const bool exists = !ec && st.type() != fs::file_type::not_found;
        if (exists) {
            if (detail::isLinkLike(st)) { e.error = "a folder on the path is a link or junction: " + comp; return e; }
            if (st.type() != fs::file_type::directory) { e.error = "a file is in the way of the folder: " + comp; return e; }
        } else {
            std::error_code cc;
            const bool made = fs::create_directory(next, cc);
            if (cc) {
                // another thread may have made it between our look and our create
                std::error_code c2;
                if (!fs::is_directory(next, c2)) { e.error = "cannot create the folder: " + cc.message(); return e; }
            } else if (made) {
                ++e.created;                       // false without an error: another thread made it first, not counted as ours
            }
        }
        std::error_code ce;
        const fs::path canon = fs::canonical(next, ce);
        if (ce || !detail::isInside(canonRoot, canon)) { e.error = "the folder resolves outside the recordings folder"; return e; }
        cur = canon;
    }
    e.ok = true;
    e.dir = cur;
    return e;
}

}  // namespace cascade::core::recdaily
