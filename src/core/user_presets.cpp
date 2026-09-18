// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/user_presets.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace cascade::core {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Does `s` hold "<digits>.<digits>.<digits>" starting at `pos`?
bool versionAt(const std::string& s, std::size_t pos) {
    for (int part = 0; part < 3; ++part) {
        if (pos >= s.size() || !isDigit(s[pos])) { return false; }
        while (pos < s.size() && isDigit(s[pos])) { ++pos; }
        if (part < 2) {
            if (pos >= s.size() || s[pos] != '.') { return false; }
            ++pos;
        }
    }
    return true;
}

// Control characters out (a label is one line on a key), then cut to the
// byte budget WITHOUT splitting a UTF-8 sequence: backing off over
// continuation bytes (10xxxxxx) lands on the lead byte of the character the
// cut would have broken, and that whole character goes.
std::string cleanLabel(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20u || u == 0x7Fu) { continue; }
        out.push_back(c);
    }
    if (out.size() > kMaxUserPresetLabelBytes) {
        std::size_t cut = kMaxUserPresetLabelBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) { --cut; }
        out.resize(cut);
    }
    // Trailing spaces left by the cut, or typed, would make two keys that
    // read identically differ by nothing visible.
    while (!out.empty() && out.back() == ' ') { out.pop_back(); }
    return out;
}

// One entry through every per-entry rule. False when it cannot be kept at all.
bool cleanEntry(UserPreset& p) {
    if (p.plugin.empty() || p.plugin.size() > kMaxUserPresetKeyBytes) { return false; }
    if (!userPresetFrequencyValid(p.frequencyHz)) { return false; }
    if (p.demodMode > 8u) { p.demodMode = 0u; }
    if (!(p.bandwidthHz >= 0.0 && p.bandwidthHz <= 1.0e9)) { p.bandwidthHz = 0.0; }
    p.label = cleanLabel(p.label);
    if (p.label.empty()) { p.label = defaultUserPresetLabel(p.frequencyHz); }
    return true;
}

bool sameChannel(const UserPreset& a, const UserPreset& b) {
    return a.plugin == b.plugin &&
           std::fabs(a.frequencyHz - b.frequencyHz) <= kUserPresetSameChannelHz;
}

std::size_t countFor(const std::vector<UserPreset>& list, const std::string& plugin) {
    std::size_t n = 0;
    for (const UserPreset& q : list) {
        if (q.plugin == plugin) { ++n; }
    }
    return n;
}

}  // namespace

std::string userPresetKey(const std::string& moduleFileName) {
    std::string s = moduleFileName;
    const std::size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) { s = s.substr(slash + 1); }
    const std::size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) { s.resize(dot); }
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        if (s[i] == '-' && versionAt(s, i + 1)) { return s.substr(0, i); }
    }
    return s;
}

bool userPresetFrequencyValid(double hz) {
    return hz > 0.0 && hz <= kMaxUserPresetHz;  // false for NaN
}

std::string defaultUserPresetLabel(double frequencyHz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f MHz", frequencyHz / 1.0e6);
    return buf;
}

std::vector<UserPreset> sanitiseUserPresets(const std::vector<UserPreset>& in) {
    std::vector<UserPreset> out;
    for (UserPreset p : in) {
        if (out.size() >= kMaxUserPresets) { break; }
        if (!cleanEntry(p)) { continue; }
        bool dup = false;
        for (const UserPreset& q : out) {
            if (sameChannel(p, q)) {
                dup = true;
                break;
            }
        }
        if (dup) { continue; }
        if (countFor(out, p.plugin) >= kMaxUserPresetsPerPlugin) { continue; }
        out.push_back(std::move(p));
    }
    return out;
}

UserPresetAdd addUserPreset(std::vector<UserPreset>& list, const UserPreset& in) {
    UserPreset p = in;
    if (!cleanEntry(p)) { return UserPresetAdd::Invalid; }
    for (const UserPreset& q : list) {
        if (sameChannel(p, q)) { return UserPresetAdd::AlreadySaved; }
    }
    if (countFor(list, p.plugin) >= kMaxUserPresetsPerPlugin) { return UserPresetAdd::PluginFull; }
    if (list.size() >= kMaxUserPresets) { return UserPresetAdd::ListFull; }
    list.push_back(std::move(p));
    return UserPresetAdd::Added;
}

bool removeUserPreset(std::vector<UserPreset>& list, const std::string& plugin,
                      std::size_t ordinal) {
    std::size_t seen = 0;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->plugin != plugin) { continue; }
        if (seen == ordinal) {
            list.erase(it);
            return true;
        }
        ++seen;
    }
    return false;
}

std::vector<UserPreset> userPresetsFor(const std::vector<UserPreset>& list,
                                       const std::string& plugin) {
    std::vector<UserPreset> out;
    for (const UserPreset& q : list) {
        if (q.plugin == plugin) { out.push_back(q); }
    }
    return out;
}

}  // namespace cascade::core
