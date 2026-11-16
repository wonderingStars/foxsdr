// s4b_logic.cpp - see s4b_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s4b_logic.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slints4b {

namespace {
std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) { --b; }
    return s.substr(a, b - a);
}
std::string tr(const char* s) { return cascade::i18n::tr(s); }
}  // namespace

HzListResult parseMhzList(const std::string& text) {
    HzListResult r;
    if (text.size() > 64 * 1024) { r.problem = tr("That list is too long."); return r; }
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t to = text.find_first_of(",; \t\r\n", from);
        if (to == std::string::npos) { to = text.size(); }
        const std::string item = trimmed(text.substr(from, to - from));
        from = to + 1;
        if (item.empty()) { continue; }
        if (item.size() > 24) { r.problem = tr("Each frequency is a number in MHz."); r.hz.clear(); return r; }
        char* end = nullptr;
        const double mhz = std::strtod(item.c_str(), &end);
        if (end == item.c_str() || *end != '\0' || !std::isfinite(mhz) || mhz <= 0.0 || mhz > 100000.0) {
            r.problem = tr("Each frequency is a number in MHz above 0 and up to 100000.");
            r.hz.clear();
            return r;
        }
        const double hz = std::round(mhz * 1e6);
        bool seen = false;
        for (const double h : r.hz) { if (std::fabs(h - hz) <= 1.0) { seen = true; break; } }
        if (seen) { continue; }
        if (r.hz.size() >= kMaxLocked) { r.problem = tr("At most 1000 frequencies can be kept."); r.hz.clear(); return r; }
        r.hz.push_back(hz);
    }
    return r;
}

std::string hzListToMhzLine(const std::string& engineText) {
    std::string out;
    std::size_t from = 0, n = 0;
    while (from <= engineText.size() && n < kMaxLocked) {
        std::size_t to = engineText.find(';', from);
        if (to == std::string::npos) { to = engineText.size(); }
        const std::string item = trimmed(engineText.substr(from, to - from));
        from = to + 1;
        if (item.empty()) { continue; }
        char* end = nullptr;
        const double hz = std::strtod(item.c_str(), &end);
        if (end == item.c_str() || !std::isfinite(hz) || hz <= 0.0) { continue; }
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.4f", hz / 1e6);
        if (!out.empty()) { out += ", "; }
        out += buf;
        ++n;
    }
    return out;
}

std::string hzListToWire(const std::vector<double>& hz) {
    std::string s;
    char buf[40];
    for (const double v : hz) {
        std::snprintf(buf, sizeof buf, "%.0f", v);
        if (!s.empty()) { s += ';'; }
        s += buf;
    }
    return s;
}

std::vector<Lockout> parseLockouts(const std::string& factText, std::size_t maxItems) {
    std::vector<Lockout> out;
    std::size_t from = 0;
    while (from <= factText.size() && out.size() < maxItems) {
        std::size_t to = factText.find(';', from);
        if (to == std::string::npos) { to = factText.size(); }
        const std::string item = factText.substr(from, to - from);
        from = to + 1;
        const std::size_t at = item.find('@');
        if (at == std::string::npos || at == 0 || at + 1 >= item.size()) { continue; }
        char* e1 = nullptr;
        char* e2 = nullptr;
        const std::string a = item.substr(0, at), b = item.substr(at + 1);
        const double hz = std::strtod(a.c_str(), &e1);
        const long long secs = std::strtoll(b.c_str(), &e2, 10);
        if (*e1 != '\0' || *e2 != '\0' || !std::isfinite(hz) || hz <= 0.0 || secs < 0) { continue; }
        out.push_back({hz, secs});
    }
    return out;
}

std::string lockoutLine(const Lockout& l) {
    char mhz[48];
    std::snprintf(mhz, sizeof mhz, "%.4f MHz", l.hz / 1e6);
    if (l.secondsLeft <= 0) { return std::string(mhz) + ": " + tr("locked out until cleared"); }
    const int m = static_cast<int>(l.secondsLeft / 60), s = static_cast<int>(l.secondsLeft % 60);
    std::string left;
    if (m > 0) { left = cascade::core::formatText(cascade::i18n::tr("%d min %d s left"), m, s); }
    else { left = cascade::core::formatText(cascade::i18n::tr("%d s left"), s); }
    return std::string(mhz) + ": " + left;
}

std::optional<double> parseDegrees(const std::string& text, double lo, double hi) {
    std::string t = trimmed(text);
    if (t.empty() || t.size() > 12) { return std::nullopt; }
    for (char& c : t) { if (c == ',') { c = '.'; } }
    std::size_t digits = 0, dots = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c >= '0' && c <= '9') { ++digits; }
        else if (c == '.') { ++dots; }
        else if (!(i == 0 && (c == '-' || c == '+'))) { return std::nullopt; }
    }
    if (digits == 0 || dots > 1) { return std::nullopt; }
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || *end != '\0' || !std::isfinite(v) || v < lo || v > hi) { return std::nullopt; }
    return v;
}

std::string rotatorTurnJson(double az, double el) {
    char b[96];
    std::snprintf(b, sizeof b, "{\"action\":\"turn\",\"az\":%.2f,\"el\":%.2f}", az, el);
    return b;
}

std::string rotatorActionJson(const char* action) { return std::string("{\"action\":\"") + action + "\"}"; }

}  // namespace foxsdr::slints4b
