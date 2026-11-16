// s4_logic.cpp - see s4_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s4_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"

namespace foxsdr::slints4 {

namespace {
constexpr std::size_t kMaxSaved = 512;

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) { --b; }
    return s.substr(a, b - a);
}
}  // namespace

std::optional<int> parseWhole(const std::string& text, int lo, int hi) {
    const std::string t = trimmed(text);
    if (t.empty() || t.size() > 9) { return std::nullopt; }
    long v = 0;
    for (const char c : t) {
        if (c < '0' || c > '9') { return std::nullopt; }
        v = v * 10 + (c - '0');
    }
    if (v < lo || v > hi) { return std::nullopt; }
    return static_cast<int>(v);
}

namespace {
std::string oneMember(const char* section, const char* member, nlohmann::json value) {
    nlohmann::json inner = nlohmann::json::object();
    inner[member] = std::move(value);
    nlohmann::json j = nlohmann::json::object();
    j[section] = std::move(inner);
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);   // never throws on text that is not valid UTF-8 (the dump-policy rule)
}
}  // namespace

std::string tciBool(const char* member, bool value) { return oneMember("tci", member, value); }
std::string tciPort(int port) { return oneMember("tci", "port", port); }
std::string udpBool(const char* member, bool value) { return oneMember("udp", member, value); }
std::string udpInt(const char* member, int value) { return oneMember("udp", member, value); }
std::string udpText(const char* member, const std::string& value) { return oneMember("udp", member, value); }

bool leavesThisComputer(const std::string& host) {
    std::string h;
    for (const char c : trimmed(host)) { h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c)))); }
    if (h.empty()) { return false; }
    return !(h == "localhost" || h == "::1" || h == "[::1]" || h.rfind("127.", 0) == 0);
}

std::string keywordsToLine(const std::string& lines) {
    std::string out;
    std::size_t from = 0;
    while (from <= lines.size()) {
        std::size_t to = lines.find('\n', from);
        if (to == std::string::npos) { to = lines.size(); }
        const std::string k = trimmed(lines.substr(from, to - from));
        from = to + 1;
        if (k.empty()) { continue; }
        if (!out.empty()) { out += ", "; }
        out += k;
    }
    return out;
}

std::string lineToKeywords(const std::string& line) {
    std::string out;
    std::size_t from = 0;
    while (from <= line.size()) {
        std::size_t to = line.find_first_of(",;\n", from);
        if (to == std::string::npos) { to = line.size(); }
        const std::string k = trimmed(line.substr(from, to - from));
        from = to + 1;
        if (k.empty()) { continue; }
        if (!out.empty()) { out += '\n'; }
        out += k;
    }
    return out;
}

std::optional<double> AutoBookmarks::update(bool enabled, std::uint32_t scannerState, double hz) {
    std::optional<double> out;
    if (!enabled) {
        wasEnabled_ = false;
        last_ = scannerState;
        return out;
    }
    if (!wasEnabled_) { wasEnabled_ = true; last_ = scannerState; return out; }   // turned on: what the scan is already holding is not saved
    if (scannerState == FOXAPI_SCANNER_PAUSED && last_ != FOXAPI_SCANNER_PAUSED && last_ != FOXAPI_SCANNER_HOLDING && std::isfinite(hz) && hz > 0.0) {
        const long long key = std::llround(hz / 100.0);
        if (saved_.count(key) == 0) {
            if (saved_.size() >= kMaxSaved) { saved_.clear(); }
            saved_.insert(key);
            out = hz;
        }
    }
    last_ = scannerState;
    return out;
}

void AutoBookmarks::reset() {
    saved_.clear();
    last_ = 0;
    wasEnabled_ = false;
}

}  // namespace foxsdr::slints4
