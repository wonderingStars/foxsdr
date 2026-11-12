// FREQUENCY BACK / FORWARD HISTORY: a 16-entry stack of the frequencies (and modes) the operator tuned, walked like a browser's history.
//
// push() records where the radio is now; back() returns the previous entry and forward() the one after it, for the caller to tune to. Rules:
//   - an entry equal to the current one (within half a hertz and the same mode) is not recorded, so the retune a back() or forward() causes - which arrives at
//     push() like any other - changes nothing;
//   - pushing while the position is not at the newest entry (after back()) throws the entries ahead of it away, as a browser does;
//   - a push that comes within coalesceMs of the previous one (the operator is turning the dial) REPLACES the newest entry instead of adding one - when that
//     entry was itself added in the middle of the spin - so a dial spin is one step and back() returns to where the spin began, not to a hundred intermediate
//     frequencies. The first entry, and an entry that follows a pause, are never replaced. coalesceMs = 0 turns this off. A clock that goes backwards never
//     coalesces;
//   - at 16 entries the oldest is dropped.
// An invalid frequency (not finite, <= 0, > 1e13) or mode name is refused. Time is an argument (milliseconds from any monotonic-ish clock); no clock is read.
//
// SAVED (optional) as versioned JSON, {"version":1,"position":n,"entries":[{"hz":..,"mode":".."}]}; any other version, more than 16 entries, a position outside
// the list or an invalid entry refuses the WHOLE file. Frequencies and mode names only.
//
// THREADS: one thread. No file access here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

struct HistoryEntry {
    double hz = 0.0;
    std::string mode;
    bool operator==(const HistoryEntry& o) const { return std::fabs(hz - o.hz) < 0.5 && mode == o.mode; }
};

class FreqHistory {
public:
    static constexpr std::size_t kCapacity = 16;

    void setCoalesceMs(double ms) { coalesceMs_ = (std::isfinite(ms) && ms > 0.0) ? ms : 0.0; }
    double coalesceMs() const { return coalesceMs_; }

    // true when the history changed.
    bool push(double hz, const std::string& mode, double nowMs) {
        if (!std::isfinite(hz) || hz <= 0.0 || hz > 1e13 || !validModeName(mode)) { return false; }
        const HistoryEntry e{hz, mode};
        const bool within = coalesceMs_ > 0.0 && haveLast_ && std::isfinite(nowMs) && nowMs >= lastPushMs_ && nowMs - lastPushMs_ < coalesceMs_;
        const bool coalesce = within && !list_.empty() && pos_ + 1 == list_.size() && replaceable_;
        lastPushMs_ = std::isfinite(nowMs) ? nowMs : lastPushMs_;
        haveLast_ = std::isfinite(nowMs);
        if (!list_.empty() && list_[pos_] == e) { return false; }
        if (coalesce) {
            list_[pos_] = e;
            // a spin that came back to the entry before it leaves that entry as the newest
            if (pos_ > 0 && list_[pos_ - 1] == e) { list_.pop_back(); --pos_; replaceable_ = false; }
            return true;
        }
        if (!list_.empty()) { list_.resize(pos_ + 1); }   // the entries ahead are thrown away
        list_.push_back(e);
        if (list_.size() > kCapacity) { list_.erase(list_.begin()); }
        pos_ = list_.size() - 1;
        replaceable_ = within;   // an entry added in the middle of a spin is the spin's, and the next move of it replaces it; the first entry or one after a pause is kept
        return true;
    }
    bool canBack() const { return pos_ > 0; }
    bool canForward() const { return !list_.empty() && pos_ + 1 < list_.size(); }
    std::optional<HistoryEntry> back() {
        if (!canBack()) { return std::nullopt; }
        --pos_;
        replaceable_ = false;
        return list_[pos_];
    }
    std::optional<HistoryEntry> forward() {
        if (!canForward()) { return std::nullopt; }
        ++pos_;
        replaceable_ = false;
        return list_[pos_];
    }
    std::optional<HistoryEntry> current() const {
        if (list_.empty()) { return std::nullopt; }
        return list_[pos_];
    }
    std::size_t size() const { return list_.size(); }
    std::size_t position() const { return pos_; }
    const std::vector<HistoryEntry>& entries() const { return list_; }
    void clear() { list_.clear(); pos_ = 0; haveLast_ = false; replaceable_ = false; }

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const HistoryEntry& e : list_) { arr.push_back({{"hz", e.hz}, {"mode", e.mode}}); }
        return {{"version", 1}, {"position", pos_}, {"entries", arr}};
    }
    bool fromJson(const nlohmann::json& j, std::string* err = nullptr) {
        auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
        if (!j.is_object()) { return fail("not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail("unknown version"); }
        if (!j.contains("entries") || !j["entries"].is_array() || !j.contains("position") || !j["position"].is_number_integer()) { return fail("a field is missing"); }
        if (j["entries"].size() > kCapacity) { return fail("more than 16 entries"); }
        std::vector<HistoryEntry> next;
        for (const auto& e : j["entries"]) {
            if (!e.is_object() || !e.contains("hz") || !e["hz"].is_number() || !e.contains("mode") || !e["mode"].is_string()) { return fail("an entry is damaged"); }
            HistoryEntry h{e["hz"].get<double>(), e["mode"].get<std::string>()};
            if (!std::isfinite(h.hz) || h.hz <= 0.0 || h.hz > 1e13 || !validModeName(h.mode)) { return fail("an entry is not valid"); }
            next.push_back(h);
        }
        const long long p = j["position"].get<long long>();
        if (next.empty() ? p != 0 : (p < 0 || p >= static_cast<long long>(next.size()))) { return fail("the position is outside the list"); }
        list_ = std::move(next);
        pos_ = static_cast<std::size_t>(p);
        haveLast_ = false;
        replaceable_ = false;
        return true;
    }

private:
    std::vector<HistoryEntry> list_;
    std::size_t pos_ = 0;
    double coalesceMs_ = 1500.0;
    double lastPushMs_ = 0.0;
    bool haveLast_ = false;
    bool replaceable_ = false;
};

}  // namespace cascade::core::tuning
