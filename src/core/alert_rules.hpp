// alert_rules - rules that turn decode events into alerts: keywords, wildcards (no regular expressions), target lists (callsigns, ICAO
// addresses, MMSIs, squawks), frequency / decoder / radio filters, rate limiting and quiet hours; rule file load and save; a notification
// sink interface.
//
// STANDALONE, header-only; nothing is wired (docs/feature-records/core-formats.md, "WIRING HAND-OFF"). The matcher takes
// decodedb::DecodeEvent (core/decode_db.hpp), the same event the decode database stores, and yields Alert values; showing them is a
// NotificationSink's job and the sinks here are the null sink and a recording sink for tests. A WINDOWS TOAST IS NOT IMPLEMENTED: see the
// record (it needs an app identity, a Start-menu shortcut for an unpackaged copy, and shows on the desktop when tested).
//
// WHEN A RULE MATCHES an event:
//   1. the rule is enabled;
//   2. every FILTER that is set holds: decoders (any of, case-insensitive exact), frequency range (inclusive), radio;
//   3. the CRITERIA: with no keyword, wildcard or target at all the rule matches on the filters alone ("anything decoded on 121.5");
//      otherwise at least ONE of the three groups must match (keywords OR wildcards OR targets; inside a group, any entry):
//        keyword  - case-insensitive substring of the text, the kind or a field value;
//        wildcard - '*' (any run) and '?' (one character) over the WHOLE text, case-insensitive; never a regular expression;
//        target   - a callsign, an ICAO address (6 hex digits), an MMSI (9 digits) or a squawk (4 octal digits). A target is found in the
//                   event's NAMED FIELDS (callsign / call / ident / flight; icao / hex / address; mmsi; squawk / code) and, except a
//                   squawk, as a whole token of the text (letters, digits, '-' and '/'). Squawks are matched in fields only: a bare
//                   four-digit number in free text is too often something else.
//   then, in order, QUIET HOURS (suppress unless the rule is `urgent`), the per-rule MINIMUM INTERVAL, the per-target interval (the same
//   target or text again within perKeyIntervalMs is suppressed; at most 4096 keys are remembered, least recently used forgotten), and the
//   hourly cap (maxPerHour alerts in any sliding hour). Time is the EVENT's time, so replaying a log gives the same alerts. Each suppression is
//   counted per rule.
//
// RULE FILE (JSON, UTF-8): {"version": 1, "rules": [ {name, enabled, severity ("info" | "notice" | "urgent"), keywords[], wildcards[],
// targets: {callsigns[], icao[], mmsi[], squawks[]}, decoders[], frequency: {minHz, maxHz}, radio, rate: {minIntervalMs, perKeyIntervalMs,
// maxPerHour}, quietHours: {start "HH:MM", end "HH:MM", utcOffsetMinutes, days: [0..6]}, bypassQuietHours} ]}. Loading REFUSES THE WHOLE FILE on
// the first error and names the rule (a half-loaded alert file would silently drop alerts): more than 1 MiB, more than 500 rules, a wrong type,
// an empty or duplicate name, an entry that is empty, too long or has a control character, a malformed target, a frequency range upside down
// or negative, a time that is not HH:MM, an offset outside +-14 h, a day outside 0..6, duplicate JSON keys are NOT detected (the last wins).
// Saving writes a temp file and renames it, so a crash leaves the old file or the new one.
//
// THREADS: AlertEngine is thread-safe (one mutex; processing is microseconds); setRules from the UI thread while the decode thread runs
// process() is fine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/decode_db.hpp"

namespace cascade::core::alerts {

using decodedb::DecodeEvent;

enum class Severity : int { Info = 0, Notice = 1, Urgent = 2 };

inline const char* severityName(Severity s) { return s == Severity::Urgent ? "urgent" : (s == Severity::Notice ? "notice" : "info"); }

struct QuietHours {
    bool enabled = false;
    int startMinute = 0;                 // minute of the day, 0..1439, in the rule's own time zone
    int endMinute = 0;                   // quiet from start up to (not including) end; start > end wraps midnight; start == end = all day
    int utcOffsetMinutes = 0;            // the zone the minutes are in, -840..840
    std::uint8_t days = 0x7F;            // bit 0 = Sunday .. bit 6 = Saturday: days on which the quiet period STARTS
};

struct Rule {
    std::string name;
    bool enabled = true;
    Severity severity = Severity::Notice;
    std::vector<std::string> keywords, wildcards;
    std::vector<std::string> callsigns, icaoAddresses, mmsis, squawks;
    std::vector<std::string> decoders;
    bool hasFreq = false;
    double freqMinHz = 0.0, freqMaxHz = 0.0;
    int radio = -1;
    std::int64_t minIntervalMs = 0;      // per rule; 0 = none
    std::int64_t perKeyIntervalMs = 0;   // per matched target / text; 0 = none
    std::uint32_t maxPerHour = 0;        // 0 = none
    QuietHours quiet;
    bool bypassQuietHours = false;       // an "urgent" rule that must reach the person at night
};

struct Alert {
    std::string rule;
    Severity severity = Severity::Notice;
    std::int64_t timeMs = 0;
    std::string matchedBy;               // "keyword: mayday", "callsign: BAW123", "wildcard: *MAYDAY*", "filters"
    std::string title;                   // "<rule>: <decoder> <frequency>"
    std::string body;                    // the text, cut to 200 bytes on a character boundary
    DecodeEvent event;
};

struct RuleStats {
    std::string rule;
    std::uint64_t matched = 0, fired = 0, quiet = 0, rateLimited = 0;
};

namespace detail {

constexpr std::size_t kMaxFileBytes = 1u << 20;
constexpr std::size_t kMaxRules = 500;
constexpr std::size_t kMaxEntries = 1000;
constexpr std::size_t kMaxEntryLen = 200;
constexpr std::size_t kMaxName = 64;
constexpr std::size_t kMaxKeys = 4096;

inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
inline char uc(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }
inline std::string lower(std::string s) { for (char& c : s) { c = lc(c); } return s; }
inline std::string upper(std::string s) { for (char& c : s) { c = uc(c); } return s; }
inline bool hasControl(const std::string& s) { for (unsigned char c : s) { if (c < 0x20 || c == 0x7F) { return true; } } return false; }

inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) { --b; }
    return s.substr(a, b - a);
}

inline bool containsLower(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) { return true; }
    if (hay.size() < needleLower.size()) { return false; }
    const std::size_t last = hay.size() - needleLower.size();
    for (std::size_t i = 0; i <= last; ++i) {
        if (lc(hay[i]) != needleLower[0]) { continue; }
        std::size_t k = 1;
        while (k < needleLower.size() && lc(hay[i + k]) == needleLower[k]) { ++k; }
        if (k == needleLower.size()) { return true; }
    }
    return false;
}

// Glob over the whole text: '*' any run, '?' one byte; case-insensitive ASCII. Iterative with one backtrack point (linear for one '*', at
// worst O(text x pattern)); the pattern is at most 200 bytes and the text at most 16 KiB.
inline bool globMatch(const std::string& patLower, const std::string& text) {
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < patLower.size() && (patLower[p] == '?' || patLower[p] == lc(text[t])) && patLower[p] != '*') { ++p; ++t; }
        else if (p < patLower.size() && patLower[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else { return false; }
    }
    while (p < patLower.size() && patLower[p] == '*') { ++p; }
    return p == patLower.size();
}

inline bool tokenChar(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '/'; }

// Whether `targetUpper` is a whole token of `text` (case-insensitive).
inline bool hasToken(const std::string& text, const std::string& targetUpper) {
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && !tokenChar(text[i])) { ++i; }
        const std::size_t s = i;
        while (i < text.size() && tokenChar(text[i])) { ++i; }
        if (i - s == targetUpper.size()) {
            bool eq = true;
            for (std::size_t k = 0; k < targetUpper.size(); ++k) { if (uc(text[s + k]) != targetUpper[k]) { eq = false; break; } }
            if (eq) { return true; }
        }
    }
    return false;
}

inline bool fieldIs(const std::string& key, std::initializer_list<const char*> names) {
    for (const char* n : names) { if (lower(key) == n) { return true; } }
    return false;
}

inline bool fieldEquals(const std::string& value, const std::string& targetUpper) { return upper(trim(value)) == targetUpper; }

inline bool validCallsign(const std::string& s) {
    if (s.empty() || s.size() > 12) { return false; }
    for (char c : s) { if (!tokenChar(c)) { return false; } }
    return true;
}
inline bool validIcao(const std::string& s) {
    if (s.size() != 6) { return false; }
    for (char c : s) { if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) { return false; } }
    return true;
}
inline bool validMmsi(const std::string& s) {
    if (s.size() != 9) { return false; }
    for (char c : s) { if (c < '0' || c > '9') { return false; } }
    return true;
}
inline bool validSquawk(const std::string& s) {
    if (s.size() != 4) { return false; }
    for (char c : s) { if (c < '0' || c > '7') { return false; } }
    return true;
}

inline std::string cutBody(const std::string& s) {
    constexpr std::size_t cap = 200;
    if (s.size() <= cap) { return s; }
    std::size_t n = cap;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) { --n; }
    return s.substr(0, n) + "...";
}

inline bool inQuiet(const QuietHours& q, std::int64_t timeMs) {
    if (!q.enabled) { return false; }
    // local minutes since the Unix epoch in the rule's zone
    const std::int64_t localMin = timeMs / 60000 + q.utcOffsetMinutes - ((timeMs % 60000 < 0) ? 1 : 0);
    std::int64_t minuteOfDay = localMin % 1440;
    std::int64_t day = localMin / 1440;
    if (minuteOfDay < 0) { minuteOfDay += 1440; --day; }
    const int weekday = static_cast<int>(((day % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday (4); 0 = Sunday
    auto dayOn = [&](int d) { return ((q.days >> (((d % 7) + 7) % 7)) & 1) != 0; };
    if (q.startMinute == q.endMinute) { return dayOn(weekday); }                    // all day
    if (q.startMinute < q.endMinute) { return minuteOfDay >= q.startMinute && minuteOfDay < q.endMinute && dayOn(weekday); }
    // wraps midnight: the evening part belongs to the day it starts on, the morning part to the day before
    if (minuteOfDay >= q.startMinute) { return dayOn(weekday); }
    if (minuteOfDay < q.endMinute) { return dayOn(weekday - 1); }
    return false;
}

}  // namespace detail

// ---- rule validation ------------------------------------------------------------------------------------------------------------------

// Returns an empty string for a valid rule, else the first problem (a sentence).
inline std::string validate(const Rule& r) {
    using namespace detail;
    if (r.name.empty() || r.name.size() > kMaxName || hasControl(r.name)) { return "the name must be 1 to 64 characters without control characters"; }
    auto list = [&](const std::vector<std::string>& v, const char* what) -> std::string {
        if (v.size() > kMaxEntries) { return std::string("more than 1000 ") + what; }
        for (const auto& s : v) {
            if (s.empty() || s.size() > kMaxEntryLen) { return std::string("a ") + what + " entry is empty or longer than 200 bytes"; }
            if (hasControl(s)) { return std::string("a ") + what + " entry has a control character"; }
        }
        return {};
    };
    for (auto [v, w] : {std::pair<const std::vector<std::string>*, const char*>{&r.keywords, "keyword"}, {&r.wildcards, "wildcard"}, {&r.decoders, "decoder"}}) {
        const std::string e = list(*v, w);
        if (!e.empty()) { return e; }
    }
    for (const auto& s : r.callsigns) { if (!validCallsign(s)) { return "callsign '" + s + "' is not 1 to 12 letters, digits, '-' or '/'"; } }
    for (const auto& s : r.icaoAddresses) { if (!validIcao(s)) { return "ICAO address '" + s + "' is not 6 hex digits"; } }
    for (const auto& s : r.mmsis) { if (!validMmsi(s)) { return "MMSI '" + s + "' is not 9 digits"; } }
    for (const auto& s : r.squawks) { if (!validSquawk(s)) { return "squawk '" + s + "' is not 4 octal digits"; } }
    if (r.callsigns.size() > kMaxEntries || r.icaoAddresses.size() > kMaxEntries || r.mmsis.size() > kMaxEntries || r.squawks.size() > kMaxEntries) { return "more than 1000 targets in one list"; }
    if (r.hasFreq && !(r.freqMinHz >= 0.0 && r.freqMaxHz >= r.freqMinHz && r.freqMaxHz <= 1.0e13)) { return "the frequency range is negative, upside down or beyond 10 THz"; }
    if (r.radio < -1 || r.radio > 65535) { return "radio must be -1 (any) or 0..65535"; }
    if (r.minIntervalMs < 0 || r.perKeyIntervalMs < 0 || r.minIntervalMs > 31LL * 86400000 || r.perKeyIntervalMs > 31LL * 86400000) { return "an interval is negative or longer than 31 days"; }
    if (r.maxPerHour > 1000000) { return "maxPerHour is beyond 1000000"; }
    if (r.quiet.enabled) {
        if (r.quiet.startMinute < 0 || r.quiet.startMinute > 1439 || r.quiet.endMinute < 0 || r.quiet.endMinute > 1439) { return "quiet hours must be within 00:00 to 23:59"; }
        if (r.quiet.utcOffsetMinutes < -840 || r.quiet.utcOffsetMinutes > 840) { return "the quiet-hours UTC offset must be within +-14 hours"; }
        if ((r.quiet.days & 0x80) != 0) { return "quiet-hours days must be 0 to 6"; }
    }
    return {};
}

// ---- the matcher ------------------------------------------------------------------------------------------------------------------------

class AlertEngine {
public:
    // Replaces the rules (rate-limit state is kept for rules that keep their name). Invalid rules are refused: nothing changes and the reason
    // is returned in *err.
    bool setRules(const std::vector<Rule>& rules, std::string* err = nullptr) {
        std::vector<std::string> names;
        for (const Rule& r : rules) {
            const std::string e = validate(r);
            if (!e.empty()) { if (err != nullptr) { *err = "rule '" + r.name + "': " + e; } return false; }
            if (std::find(names.begin(), names.end(), r.name) != names.end()) { if (err != nullptr) { *err = "duplicate rule name '" + r.name + "'"; } return false; }
            names.push_back(r.name);
        }
        if (rules.size() > detail::kMaxRules) { if (err != nullptr) { *err = "more than 500 rules"; } return false; }
        std::lock_guard<std::mutex> lk(mu_);
        // States are heap objects held by unique_ptr: a State owns an LRU list and a map of iterators into it, which must never be COPIED (a
        // copied map would point into the original list) - and a vector of plain States copies them whenever it reallocates.
        std::vector<std::unique_ptr<State>> next;
        for (const Rule& r : rules) {
            std::unique_ptr<State> s;
            for (auto& old : states_) { if (old && old->rule.name == r.name) { s = std::move(old); break; } }
            if (!s) { s = std::make_unique<State>(); }
            s->rule = r;
            s->prepare();
            next.push_back(std::move(s));
        }
        states_ = std::move(next);
        return true;
    }

    std::vector<Alert> process(const DecodeEvent& e) {
        std::vector<Alert> out;
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& sp : states_) {
            State& s = *sp;
            if (!s.rule.enabled) { continue; }
            std::string by, key;
            if (!s.matches(e, &by, &key)) { continue; }
            ++s.stats.matched;
            const Rule& r = s.rule;
            if (detail::inQuiet(r.quiet, e.timeMs) && !r.bypassQuietHours) { ++s.stats.quiet; continue; }
            if (r.minIntervalMs > 0 && s.hasLast && e.timeMs - s.lastFiredMs < r.minIntervalMs && e.timeMs >= s.lastFiredMs) { ++s.stats.rateLimited; continue; }
            if (r.perKeyIntervalMs > 0 && !s.keyAllowed(key, e.timeMs)) { ++s.stats.rateLimited; continue; }
            if (r.maxPerHour > 0) {
                while (!s.recent.empty() && s.recent.front() <= e.timeMs - 3600000) { s.recent.pop_front(); }
                if (s.recent.size() >= r.maxPerHour) { ++s.stats.rateLimited; continue; }
                s.recent.push_back(e.timeMs);
            }
            s.hasLast = true;
            s.lastFiredMs = e.timeMs;
            ++s.stats.fired;
            Alert a;
            a.rule = r.name;
            a.severity = r.severity;
            a.timeMs = e.timeMs;
            a.matchedBy = by;
            char f[40];
            std::snprintf(f, sizeof f, "%.4f MHz", e.freqHz / 1e6);
            a.title = r.name + ": " + (e.decoder.empty() ? std::string("decode") : e.decoder) + (e.freqHz > 0 ? std::string(" ") + f : std::string());
            a.body = detail::cutBody(e.text);
            a.event = e;
            out.push_back(std::move(a));
        }
        return out;
    }

    std::vector<RuleStats> stats() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<RuleStats> v;
        for (const auto& s : states_) { RuleStats r = s->stats; r.rule = s->rule.name; v.push_back(r); }
        return v;
    }

private:
    struct State {
        State() = default;
        State(const State&) = delete;
        State& operator=(const State&) = delete;
        Rule rule;
        std::vector<std::string> kwLower, wcLower, callsignsUp, icaoUp, mmsiUp, squawkUp, decodersLower;
        RuleStats stats;
        bool hasLast = false;
        std::int64_t lastFiredMs = 0;
        std::deque<std::int64_t> recent;
        std::list<std::pair<std::string, std::int64_t>> lru;
        std::unordered_map<std::string, std::list<std::pair<std::string, std::int64_t>>::iterator> keys;

        void prepare() {
            kwLower.clear(); wcLower.clear(); callsignsUp.clear(); icaoUp.clear(); mmsiUp.clear(); squawkUp.clear(); decodersLower.clear();
            for (const auto& s : rule.keywords) { kwLower.push_back(detail::lower(s)); }
            for (const auto& s : rule.wildcards) { wcLower.push_back(detail::lower(s)); }
            for (const auto& s : rule.callsigns) { callsignsUp.push_back(detail::upper(s)); }
            for (const auto& s : rule.icaoAddresses) { icaoUp.push_back(detail::upper(s)); }
            for (const auto& s : rule.mmsis) { mmsiUp.push_back(s); }
            for (const auto& s : rule.squawks) { squawkUp.push_back(s); }
            for (const auto& s : rule.decoders) { decodersLower.push_back(detail::lower(s)); }
        }

        bool matches(const DecodeEvent& e, std::string* by, std::string* key) const {
            if (!decodersLower.empty()) {
                const std::string d = detail::lower(e.decoder);
                if (std::find(decodersLower.begin(), decodersLower.end(), d) == decodersLower.end()) { return false; }
            }
            if (rule.hasFreq && !(e.freqHz >= rule.freqMinHz && e.freqHz <= rule.freqMaxHz)) { return false; }
            if (rule.radio >= 0 && e.radio != static_cast<std::uint16_t>(rule.radio)) { return false; }
            const bool anyCriteria = !kwLower.empty() || !wcLower.empty() || !callsignsUp.empty() || !icaoUp.empty() || !mmsiUp.empty() || !squawkUp.empty();
            if (!anyCriteria) { *by = "filters"; *key = e.decoder + "|" + e.text; return true; }
            for (const auto& k : kwLower) {
                bool hit = detail::containsLower(e.text, k) || detail::containsLower(e.kind, k);
                for (std::size_t i = 0; !hit && i < e.fields.size(); ++i) { hit = detail::containsLower(e.fields[i].second, k); }
                if (hit) { *by = "keyword: " + k; *key = "k:" + k; return true; }
            }
            for (std::size_t i = 0; i < wcLower.size(); ++i) {
                if (detail::globMatch(wcLower[i], e.text)) { *by = "wildcard: " + rule.wildcards[i]; *key = "w:" + wcLower[i] + "|" + e.text; return true; }
            }
            auto target = [&](const std::vector<std::string>& list, std::initializer_list<const char*> fieldNames, bool tokenToo, const char* label) -> bool {
                for (const auto& t : list) {
                    bool hit = false;
                    for (const auto& f : e.fields) { if (detail::fieldIs(f.first, fieldNames) && detail::fieldEquals(f.second, t)) { hit = true; break; } }
                    if (!hit && tokenToo) { hit = detail::hasToken(e.text, t); }
                    if (hit) { *by = std::string(label) + ": " + t; *key = std::string("t:") + label + t; return true; }
                }
                return false;
            };
            if (target(callsignsUp, {"callsign", "call", "ident", "flight"}, true, "callsign")) { return true; }
            if (target(icaoUp, {"icao", "hex", "address"}, true, "icao")) { return true; }
            if (target(mmsiUp, {"mmsi"}, true, "mmsi")) { return true; }
            if (target(squawkUp, {"squawk", "code"}, false, "squawk")) { return true; }
            return false;
        }

        bool keyAllowed(const std::string& key, std::int64_t now) {
            const auto it = keys.find(key);
            if (it != keys.end()) {
                const std::int64_t last = it->second->second;
                lru.splice(lru.begin(), lru, it->second);
                if (now >= last && now - last < rule.perKeyIntervalMs) { return false; }
                lru.front().second = now;
                return true;
            }
            lru.emplace_front(key, now);
            keys[key] = lru.begin();
            while (lru.size() > detail::kMaxKeys) { keys.erase(lru.back().first); lru.pop_back(); }
            return true;
        }
    };

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<State>> states_;
};

// ---- rule file ----------------------------------------------------------------------------------------------------------------------------

struct LoadResult {
    std::vector<Rule> rules;
    std::string error;        // set when the file was refused (rules is then empty)
};

namespace detail {

inline std::string hhmm(int m) { char b[8]; std::snprintf(b, sizeof b, "%02d:%02d", m / 60, m % 60); return b; }

inline bool parseHhmm(const nlohmann::json& j, int* out) {
    if (!j.is_string()) { return false; }
    const std::string s = j.get<std::string>();
    if (s.size() != 5 || s[2] != ':') { return false; }
    for (int i : {0, 1, 3, 4}) { if (s[static_cast<std::size_t>(i)] < '0' || s[static_cast<std::size_t>(i)] > '9') { return false; } }
    const int h = (s[0] - '0') * 10 + (s[1] - '0'), m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) { return false; }
    *out = h * 60 + m;
    return true;
}

inline bool strList(const nlohmann::json& o, const char* key, std::vector<std::string>* out, std::string* why) {
    const auto it = o.find(key);
    if (it == o.end()) { return true; }
    if (!it->is_array()) { *why = std::string("\"") + key + "\" must be a list"; return false; }
    if (it->size() > kMaxEntries) { *why = std::string("\"") + key + "\" has more than 1000 entries"; return false; }
    for (const auto& v : *it) {
        if (!v.is_string()) { *why = std::string("\"") + key + "\" must hold only text"; return false; }
        out->push_back(v.get<std::string>());
    }
    return true;
}

inline bool intField(const nlohmann::json& o, const char* key, std::int64_t* out, std::string* why) {
    const auto it = o.find(key);
    if (it == o.end()) { return true; }
    if (!it->is_number_integer()) { *why = std::string("\"") + key + "\" must be a whole number"; return false; }
    *out = it->get<std::int64_t>();
    return true;
}

}  // namespace detail

inline LoadResult parseRules(const std::string& text) {
    using namespace detail;
    LoadResult r;
    auto refuse = [&](const std::string& m) { r.rules.clear(); r.error = m; return r; };
    if (text.size() > kMaxFileBytes) { return refuse("the rule file is larger than 1 MiB"); }
    if (text.find('\0') != std::string::npos) { return refuse("the rule file contains a NUL byte"); }
    {   // nesting depth, counted outside strings, BEFORE the parser sees it: a rule file is three levels deep, never more than 8
        int depth = 0;
        bool inStr = false, esc = false;
        for (char c : text) {
            if (inStr) { if (esc) { esc = false; } else if (c == '\\') { esc = true; } else if (c == '"') { inStr = false; } continue; }
            if (c == '"') { inStr = true; }
            else if (c == '[' || c == '{') { if (++depth > 8) { return refuse("the rule file is nested more than 8 levels deep"); } }
            else if (c == ']' || c == '}') { --depth; }
        }
    }
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return refuse("the rule file is not a JSON object"); }
    const auto ver = j.find("version");
    if (ver == j.end() || !ver->is_number_integer() || ver->get<std::int64_t>() != 1) { return refuse("unsupported or missing \"version\" (this reader knows version 1)"); }
    const auto rl = j.find("rules");
    if (rl == j.end() || !rl->is_array()) { return refuse("\"rules\" must be a list"); }
    if (rl->size() > kMaxRules) { return refuse("more than 500 rules"); }
    std::size_t idx = 0;
    for (const auto& o : *rl) {
        ++idx;
        const std::string at = "rule " + std::to_string(idx) + ": ";
        std::string why;
        if (!o.is_object()) { return refuse(at + "must be an object"); }
        Rule x;
        const auto nm = o.find("name");
        if (nm == o.end() || !nm->is_string()) { return refuse(at + "needs a text \"name\""); }
        x.name = nm->get<std::string>();
        const std::string at2 = "rule " + std::to_string(idx) + " ('" + (x.name.size() > 64 ? x.name.substr(0, 64) : x.name) + "'): ";
        if (const auto en = o.find("enabled"); en != o.end()) { if (!en->is_boolean()) { return refuse(at2 + "\"enabled\" must be true or false"); } x.enabled = en->get<bool>(); }
        if (const auto b = o.find("bypassQuietHours"); b != o.end()) { if (!b->is_boolean()) { return refuse(at2 + "\"bypassQuietHours\" must be true or false"); } x.bypassQuietHours = b->get<bool>(); }
        if (const auto sv = o.find("severity"); sv != o.end()) {
            const std::string s = sv->is_string() ? lower(sv->get<std::string>()) : std::string();
            if (s == "info") { x.severity = Severity::Info; } else if (s == "notice") { x.severity = Severity::Notice; } else if (s == "urgent") { x.severity = Severity::Urgent; }
            else { return refuse(at2 + "\"severity\" must be \"info\", \"notice\" or \"urgent\""); }
        }
        if (!strList(o, "keywords", &x.keywords, &why) || !strList(o, "wildcards", &x.wildcards, &why) || !strList(o, "decoders", &x.decoders, &why)) { return refuse(at2 + why); }
        if (const auto t = o.find("targets"); t != o.end()) {
            if (!t->is_object()) { return refuse(at2 + "\"targets\" must be an object"); }
            if (!strList(*t, "callsigns", &x.callsigns, &why) || !strList(*t, "icao", &x.icaoAddresses, &why) || !strList(*t, "mmsi", &x.mmsis, &why) || !strList(*t, "squawks", &x.squawks, &why)) { return refuse(at2 + why); }
        }
        if (const auto f = o.find("frequency"); f != o.end()) {
            if (!f->is_object() || !f->contains("minHz") || !f->contains("maxHz") || !(*f)["minHz"].is_number() || !(*f)["maxHz"].is_number()) { return refuse(at2 + "\"frequency\" needs numeric \"minHz\" and \"maxHz\""); }
            x.hasFreq = true;
            x.freqMinHz = (*f)["minHz"].get<double>();
            x.freqMaxHz = (*f)["maxHz"].get<double>();
        }
        std::int64_t v = -1;
        if (!intField(o, "radio", &v, &why)) { return refuse(at2 + why); }
        if (o.contains("radio")) { if (v < -1 || v > 65535) { return refuse(at2 + "\"radio\" must be -1 or 0..65535"); } x.radio = static_cast<int>(v); }
        if (const auto rt = o.find("rate"); rt != o.end()) {
            if (!rt->is_object()) { return refuse(at2 + "\"rate\" must be an object"); }
            std::int64_t a = 0, b = 0, c = 0;
            if (!intField(*rt, "minIntervalMs", &a, &why) || !intField(*rt, "perKeyIntervalMs", &b, &why) || !intField(*rt, "maxPerHour", &c, &why)) { return refuse(at2 + why); }
            if (c < 0 || c > 1000000) { return refuse(at2 + "\"maxPerHour\" must be 0 to 1000000"); }
            x.minIntervalMs = a; x.perKeyIntervalMs = b; x.maxPerHour = static_cast<std::uint32_t>(c);
        }
        if (const auto q = o.find("quietHours"); q != o.end()) {
            if (!q->is_object()) { return refuse(at2 + "\"quietHours\" must be an object"); }
            x.quiet.enabled = true;
            if (!q->contains("start") || !q->contains("end") || !parseHhmm((*q)["start"], &x.quiet.startMinute) || !parseHhmm((*q)["end"], &x.quiet.endMinute)) { return refuse(at2 + "quiet hours need \"start\" and \"end\" as HH:MM"); }
            std::int64_t off = 0;
            if (!intField(*q, "utcOffsetMinutes", &off, &why)) { return refuse(at2 + why); }
            if (off < -840 || off > 840) { return refuse(at2 + "the quiet-hours UTC offset must be within +-14 hours"); }
            x.quiet.utcOffsetMinutes = static_cast<int>(off);
            if (const auto d = q->find("days"); d != q->end()) {
                if (!d->is_array() || d->size() > 7) { return refuse(at2 + "quiet-hours \"days\" must be a list of 0..6"); }
                std::uint8_t mask = 0;
                for (const auto& e : *d) {
                    if (!e.is_number_integer() || e.get<std::int64_t>() < 0 || e.get<std::int64_t>() > 6) { return refuse(at2 + "quiet-hours \"days\" must be a list of 0..6"); }
                    mask = static_cast<std::uint8_t>(mask | (1u << e.get<std::int64_t>()));
                }
                x.quiet.days = mask;
            }
        }
        const std::string bad = validate(x);
        if (!bad.empty()) { return refuse(at2 + bad); }
        for (const Rule& prev : r.rules) { if (prev.name == x.name) { return refuse(at2 + "the name is used twice"); } }
        r.rules.push_back(std::move(x));
    }
    return r;
}

inline std::string rulesToJson(const std::vector<Rule>& rules) {
    nlohmann::json root = nlohmann::json::object();
    root["version"] = 1;
    nlohmann::json arr = nlohmann::json::array();
    for (const Rule& r : rules) {
        nlohmann::json o = nlohmann::json::object();
        o["name"] = r.name;
        o["enabled"] = r.enabled;
        o["severity"] = severityName(r.severity);
        o["keywords"] = r.keywords;
        o["wildcards"] = r.wildcards;
        o["targets"] = {{"callsigns", r.callsigns}, {"icao", r.icaoAddresses}, {"mmsi", r.mmsis}, {"squawks", r.squawks}};
        o["decoders"] = r.decoders;
        if (r.hasFreq) { o["frequency"] = {{"minHz", r.freqMinHz}, {"maxHz", r.freqMaxHz}}; }
        o["radio"] = r.radio;
        o["rate"] = {{"minIntervalMs", r.minIntervalMs}, {"perKeyIntervalMs", r.perKeyIntervalMs}, {"maxPerHour", r.maxPerHour}};
        if (r.quiet.enabled) {
            nlohmann::json days = nlohmann::json::array();
            for (int d = 0; d < 7; ++d) { if ((r.quiet.days >> d) & 1) { days.push_back(d); } }
            o["quietHours"] = {{"start", detail::hhmm(r.quiet.startMinute)}, {"end", detail::hhmm(r.quiet.endMinute)}, {"utcOffsetMinutes", r.quiet.utcOffsetMinutes}, {"days", days}};
        }
        o["bypassQuietHours"] = r.bypassQuietHours;
        arr.push_back(o);
    }
    root["rules"] = arr;
    return root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

inline LoadResult loadRulesFile(const std::string& path) {
    LoadResult r;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (!std::filesystem::is_regular_file(p, ec)) { r.error = "cannot open " + path; return r; }
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec || size > detail::kMaxFileBytes) { r.error = ec ? "cannot read the size of " + path : "the rule file is larger than 1 MiB"; return r; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { r.error = "cannot open " + path; return r; }
    std::string text(static_cast<std::size_t>(size), ' ');
    f.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(f.gcount()));
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) { text.erase(0, 3); }
    return parseRules(text);
}

// Validates, then writes a temp file and renames it over `path`.
inline bool saveRulesFile(const std::string& path, const std::vector<Rule>& rules, std::string* err) {
    for (const Rule& r : rules) {
        const std::string e = validate(r);
        if (!e.empty()) { if (err != nullptr) { *err = "rule '" + r.name + "': " + e; } return false; }
    }
    const std::string text = rulesToJson(rules);
    std::filesystem::path tmp(path);
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err != nullptr) { *err = "cannot create " + tmp.string(); } return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { if (err != nullptr) { *err = "disk error while writing " + tmp.string(); } std::error_code ec; std::filesystem::remove(tmp, ec); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, std::filesystem::path(path), ec);
    if (ec) { std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "cannot replace " + path; } return false; }
    return true;
}

// ---- notifications -------------------------------------------------------------------------------------------------------------------------

struct Notification {
    std::string title, body;
    Severity severity = Severity::Notice;
    std::int64_t timeMs = 0;
    std::string rule;
};

// Whatever shows or sends an alert. notify() may block (a toast call, a network post): call it from a worker thread, never the decode thread.
class NotificationSink {
public:
    virtual ~NotificationSink() = default;
    virtual const char* name() const = 0;
    virtual bool notify(const Notification& n, std::string* err) = 0;
};

inline Notification toNotification(const Alert& a) {
    Notification n;
    n.title = a.title;
    n.body = a.body.empty() ? a.matchedBy : a.body + "\n(" + a.matchedBy + ")";
    n.severity = a.severity;
    n.timeMs = a.timeMs;
    n.rule = a.rule;
    return n;
}

class NullSink : public NotificationSink {
public:
    const char* name() const override { return "none"; }
    bool notify(const Notification&, std::string*) override { return true; }
};

// Keeps what it was given (tests, an in-app alert list).
class RecordingSink : public NotificationSink {
public:
    const char* name() const override { return "recording"; }
    bool notify(const Notification& n, std::string*) override {
        std::lock_guard<std::mutex> lk(mu_);
        items_.push_back(n);
        if (items_.size() > 1000) { items_.erase(items_.begin()); }
        return true;
    }
    std::vector<Notification> items() const { std::lock_guard<std::mutex> lk(mu_); return items_; }
private:
    mutable std::mutex mu_;
    std::vector<Notification> items_;
};

// Hands every alert to the sink; returns how many it accepted. A sink that throws is treated as failing.
inline std::size_t deliver(const std::vector<Alert>& alerts, NotificationSink& sink, std::string* err = nullptr) {
    std::size_t ok = 0;
    for (const Alert& a : alerts) {
        std::string e;
        bool good = false;
        try { good = sink.notify(toNotification(a), &e); } catch (...) { e = "the notification sink threw"; }
        if (good) { ++ok; } else if (err != nullptr && err->empty()) { *err = e.empty() ? "the sink refused an alert" : e; }
    }
    return ok;
}

}  // namespace cascade::core::alerts
