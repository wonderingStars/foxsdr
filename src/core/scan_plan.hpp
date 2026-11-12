// A SCAN PLAN: several search ranges in one scan, a direction (forward or backward), a step-back cursor, the trigger rule, and "save this hit as a
// bookmark".
//
// core/scanner.hpp (the dwell / hold / resume state machine) is not changed and does not need to be: its LIST MODE already walks any list of
// frequencies in order and wraps. A plan therefore EXPANDS into that list (ScannerParams::list), already in the wanted direction, and the scanner
// stays one range's worth of code. What the plan adds is on this side of the scanner:
//   - up to 16 ranges, each with its own step, mode and bandwidth (a scan over airband 118-137 MHz AM at 25 kHz, then 144-146 MHz NFM at 12.5 kHz);
//   - the expanded list is capped (kMaxPoints) and a plan that would exceed it is REFUSED with a reason, not truncated: a silent half-scan is worse;
//   - points shared by two ranges appear once, at their first occurrence;
//   - Backward reverses the whole list (the last range's highest point first);
//   - ScanCursor: the operator's "next" and "back" over the same list, wrapping at both ends, and a re-seek after the plan or direction changes so the
//     scan stays where it was;
//   - bookmarkFromHit: the mode and bandwidth of the range the hit lies in, a generated name, a duplicate refused.
//
// A plan is saved as versioned JSON (version 1); another version is refused and nothing changes. No identifier of any kind is stored: ranges,
// labels typed by the user, the trigger numbers.
//
// THREADS: one thread. Pure: no clock, no file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/bookmark.hpp"
#include "core/scan_trigger.hpp"
#include "core/scanner_params.hpp"
#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

struct ScanRange {
    double startHz = 0.0;
    double stopHz = 0.0;
    double stepHz = 12500.0;
    std::string mode = "NFM";
    double bandwidthHz = 12500.0;
    std::string label;
};

inline bool validRange(const ScanRange& r) {
    const auto okHz = [](double v) { return std::isfinite(v) && v > 0.0 && v <= 1e13; };
    return okHz(r.startHz) && okHz(r.stopHz) && r.stopHz >= r.startHz && std::isfinite(r.stepHz) && r.stepHz >= 1.0 && r.stepHz <= 1e9 &&
           validModeName(r.mode) && std::isfinite(r.bandwidthHz) && r.bandwidthHz >= 50.0 && r.bandwidthHz <= 1e7 && validUserText(r.label, 64);
}

enum class ScanDirection { Forward = 0, Backward = 1 };

struct ScanPoints {
    std::vector<double> hz;
    std::vector<std::uint8_t> range;  // the range index each point came from
};

class ScanPlan {
public:
    static constexpr std::size_t kMaxRanges = 16;
    static constexpr std::size_t kMaxPoints = 100000;

    // Adds a range. false (nothing changed) for an invalid range or a 17th.
    bool addRange(const ScanRange& r, std::string* err = nullptr) {
        if (ranges_.size() >= kMaxRanges) { return fail(err, "more than 16 ranges"); }
        if (!validRange(r)) { return fail(err, "the range is not valid"); }
        ranges_.push_back(r);
        return true;
    }
    bool removeRange(std::size_t i) {
        if (i >= ranges_.size()) { return false; }
        ranges_.erase(ranges_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    void clear() { ranges_.clear(); }
    const std::vector<ScanRange>& ranges() const { return ranges_; }

    void setDirection(ScanDirection d) { dir_ = d; }
    ScanDirection direction() const { return dir_; }
    ScanTriggerConfig& trigger() { return trigger_; }
    const ScanTriggerConfig& trigger() const { return trigger_; }

    // The points of the scan in scan order. nullopt (with the reason) when there is no range or the plan would exceed kMaxPoints.
    std::optional<ScanPoints> expand(std::string* err = nullptr) const {
        if (ranges_.empty()) { fail(err, "no range"); return std::nullopt; }
        ScanPoints out;
        std::set<double> seen;  // ordered, so a near-duplicate is found from its neighbours in O(log n)
        for (std::size_t ri = 0; ri < ranges_.size(); ++ri) {
            const ScanRange& r = ranges_[ri];
            const double n = std::floor((r.stopHz - r.startHz) / r.stepHz) + 1.0;
            if (n > static_cast<double>(kMaxPoints)) { fail(err, "more than 100000 points"); return std::nullopt; }
            for (std::size_t k = 0; k < static_cast<std::size_t>(n); ++k) {
                const double f = r.startHz + static_cast<double>(k) * r.stepHz;
                const auto it = seen.lower_bound(f - 0.5);
                if (it != seen.end() && *it <= f + 0.5) { continue; }  // shared with an earlier range: kept once, at its first place
                seen.insert(f);
                out.hz.push_back(f);
                out.range.push_back(static_cast<std::uint8_t>(ri));
                if (out.hz.size() > kMaxPoints) { fail(err, "more than 100000 points"); return std::nullopt; }
            }
        }
        if (dir_ == ScanDirection::Backward) {
            std::reverse(out.hz.begin(), out.hz.end());
            std::reverse(out.range.begin(), out.range.end());
        }
        return out;
    }

    // The scanner's parameters for this plan: `base` with its list replaced by the expanded points (start/stop follow the extremes for display).
    std::optional<ScannerParams> toParams(const ScannerParams& base, std::string* err = nullptr) const {
        const auto pts = expand(err);
        if (!pts) { return std::nullopt; }
        ScannerParams p = base;
        p.list = pts->hz;
        const auto mm = std::minmax_element(pts->hz.begin(), pts->hz.end());
        p.startHz = *mm.first;
        p.stopHz = *mm.second;
        return p;
    }

    // The range a frequency lies in (the first that contains it), or nullptr.
    const ScanRange* rangeOf(double hz) const {
        for (const ScanRange& r : ranges_) {
            if (hz >= r.startHz - 0.5 && hz <= r.stopHz + 0.5) { return &r; }
        }
        return nullptr;
    }

    nlohmann::json toJson() const {
        nlohmann::json rs = nlohmann::json::array();
        for (const ScanRange& r : ranges_) {
            rs.push_back({{"startHz", r.startHz}, {"stopHz", r.stopHz}, {"stepHz", r.stepHz}, {"mode", r.mode}, {"bandwidthHz", r.bandwidthHz}, {"label", r.label}});
        }
        return {{"version", 1}, {"direction", dir_ == ScanDirection::Forward ? "forward" : "backward"}, {"trigger", triggerJson()}, {"ranges", rs}};
    }
    // Strict. On any refusal the plan is left exactly as it was.
    bool fromJson(const nlohmann::json& j, std::string* err = nullptr) {
        if (!j.is_object()) { return fail(err, "not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail(err, "unknown version"); }
        if (!j.contains("direction") || !j["direction"].is_string()) { return fail(err, "direction missing"); }
        const std::string d = j["direction"].get<std::string>();
        if (d != "forward" && d != "backward") { return fail(err, "unknown direction"); }
        if (!j.contains("ranges") || !j["ranges"].is_array()) { return fail(err, "ranges missing"); }
        if (j["ranges"].size() > kMaxRanges) { return fail(err, "more than 16 ranges"); }
        std::vector<ScanRange> rs;
        for (const auto& e : j["ranges"]) {
            if (!e.is_object()) { return fail(err, "a range is not an object"); }
            ScanRange r;
            const char* nums[] = {"startHz", "stopHz", "stepHz", "bandwidthHz"};
            double* dst[] = {&r.startHz, &r.stopHz, &r.stepHz, &r.bandwidthHz};
            for (int i = 0; i < 4; ++i) {
                if (!e.contains(nums[i]) || !e[nums[i]].is_number()) { return fail(err, "a range field is missing"); }
                *dst[i] = e[nums[i]].get<double>();
            }
            if (!e.contains("mode") || !e["mode"].is_string() || !e.contains("label") || !e["label"].is_string()) { return fail(err, "a range text field is missing"); }
            r.mode = e["mode"].get<std::string>();
            r.label = e["label"].get<std::string>();
            if (!validRange(r)) { return fail(err, "a range is not valid"); }
            rs.push_back(std::move(r));
        }
        if (!j.contains("trigger")) { return fail(err, "trigger missing"); }
        ScanTrigger t;
        if (!t.configFromJson(j["trigger"], err)) { return false; }
        trigger_ = t.config();
        ranges_ = std::move(rs);
        dir_ = d == "forward" ? ScanDirection::Forward : ScanDirection::Backward;
        return true;
    }

private:
    nlohmann::json triggerJson() const {
        ScanTrigger t;
        t.setConfig(trigger_);
        return t.configToJson();
    }
    static bool fail(std::string* err, const char* m) { if (err != nullptr) { *err = m; } return false; }
    std::vector<ScanRange> ranges_;
    ScanDirection dir_ = ScanDirection::Forward;
    ScanTriggerConfig trigger_{};
};

// The operator's position in an expanded plan: next, back (wrapping both ways), and a re-seek after the plan changed.
class ScanCursor {
public:
    void load(ScanPoints p, double keepNearHz = NAN) {
        pts_ = std::move(p);
        idx_ = 0;
        if (std::isfinite(keepNearHz) && !pts_.hz.empty()) { seek(keepNearHz); }
    }
    bool empty() const { return pts_.hz.empty(); }
    std::size_t size() const { return pts_.hz.size(); }
    std::size_t index() const { return idx_; }
    double current() const { return pts_.hz.empty() ? 0.0 : pts_.hz[idx_]; }
    double next() {
        if (pts_.hz.empty()) { return 0.0; }
        idx_ = (idx_ + 1) % pts_.hz.size();
        return pts_.hz[idx_];
    }
    // One point back; from the first point it wraps to the last.
    double back() {
        if (pts_.hz.empty()) { return 0.0; }
        idx_ = (idx_ + pts_.hz.size() - 1) % pts_.hz.size();
        return pts_.hz[idx_];
    }
    // Moves to the point nearest `hz` (the first of two equally near).
    void seek(double hz) {
        if (pts_.hz.empty() || !std::isfinite(hz)) { return; }
        std::size_t best = 0;
        double bd = std::fabs(pts_.hz[0] - hz);
        for (std::size_t i = 1; i < pts_.hz.size(); ++i) {
            const double d = std::fabs(pts_.hz[i] - hz);
            if (d < bd) { bd = d; best = i; }
        }
        idx_ = best;
    }
    const ScanPoints& points() const { return pts_; }

private:
    ScanPoints pts_;
    std::size_t idx_ = 0;
};

// "145.5000 MHz" style text for a generated bookmark name (four decimals: 100 Hz).
inline std::string scanHitName(double hz) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "Scan %.4f MHz", hz / 1e6);
    return buf;
}

// The bookmark for a scan hit: the mode and bandwidth of the range the hit lies in (the plan's first range when it lies in none), the group name
// given, the name generated. nullopt when the frequency is not valid or `existing` already holds a bookmark within dupTolHz in the same mode.
inline std::optional<Bookmark> bookmarkFromHit(double hz, const ScanPlan& plan, const std::vector<Bookmark>& existing, const std::string& group = "Scan hits",
                                               double dupTolHz = 1.0) {
    if (!std::isfinite(hz) || hz <= 0.0 || hz > 1e13 || plan.ranges().empty() || !validUserText(group, 64)) { return std::nullopt; }
    const ScanRange* r = plan.rangeOf(hz);
    if (r == nullptr) { r = &plan.ranges().front(); }
    for (const Bookmark& b : existing) {
        if (std::fabs(b.freqHz - hz) <= dupTolHz && b.mode == r->mode) { return std::nullopt; }
    }
    Bookmark b;
    b.name = scanHitName(hz);
    b.freqHz = hz;
    b.mode = r->mode;
    b.bandwidthHz = r->bandwidthHz;
    b.group = group;
    return b;
}

}  // namespace cascade::core::tuning
