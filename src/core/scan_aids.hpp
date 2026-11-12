// Scanner additions as pure models: priority channels checked every N seconds, temporary and permanent lockouts, a scan log (what stopped the scan,
// when, for how long, at what level) with per-frequency statistics and CSV, and muting the audio while the scanner is only stepping.
//
// STANDALONE. core/scanner.hpp (the dwell / hold / resume state machine) is NOT changed; these models sit beside it and the wiring lane connects them
// (docs/feature-records/recording-models.md, Part 8). Time is always an argument (milliseconds from any monotonic-ish clock; a BACKWARD jump counts as
// zero elapsed, the same rule as the scanner); nothing here sleeps, reads a clock or touches a file.
//
// WHAT THE REFERENCE PROGRAMS DO is taken from the owner's gap list (FEATURE-GAPS.md, "Scanning and memories"): priority channels (Uniden style, SDR--,
// DSD+), lockouts temporary and permanent, scan metrics (SDR# Scanner Metrics, SDRuno statistics), mute while scanning (SDR++ CE). No program source was
// read; every default is marked as one the owner can change.
//
// THREADS: every class is for one thread (the control thread that also runs the scanner tick).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/civil_time.hpp"
#include "core/scanner.hpp"

namespace cascade::core::scanaids {

inline bool validHz(double hz) { return std::isfinite(hz) && hz >= 0.0 && hz <= 1e13; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 1. Priority channels.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct PriorityConfig {
    bool enabled = false;
    double intervalS = 2.0;               // DEFAULT 2 s (owner can change): how often the priority list is checked while the scanner scans or holds
    int dwellMs = 200;                    // DEFAULT: how long each priority channel is listened to for a signal on each check
    int hangMs = 2000;                    // DEFAULT: how long a priority channel stays up after its signal goes quiet
    bool interruptHold = true;            // a check may interrupt a hold on an ordinary channel (a priority channel outranks a conversation)
};

struct PriorityAction {
    enum class Kind { None, GoPriority, ReturnToScan };
    Kind kind = Kind::None;
    int index = -1;                       // the priority channel to tune to (GoPriority)
    double hz = 0.0;
};

class PriorityScanner {
public:
    static constexpr std::size_t kMaxChannels = 16;

    // Channels in check order (finite, 0..1e13, de-duplicated within 1 Hz, at most 16).
    bool setChannels(const std::vector<double>& hz, std::string* err = nullptr) {
        std::vector<double> v;
        for (double f : hz) {
            if (!validHz(f)) { if (err != nullptr) { *err = "a priority frequency is not valid"; } return false; }
            bool dup = false;
            for (double g : v) { if (std::fabs(g - f) < 1.0) { dup = true; break; } }
            if (!dup) { v.push_back(f); }
        }
        if (v.size() > kMaxChannels) { if (err != nullptr) { *err = "more than 16 priority channels"; } return false; }
        ch_ = std::move(v);
        reset();
        return true;
    }
    const std::vector<double>& channels() const { return ch_; }

    void setConfig(const PriorityConfig& c) {
        cfg_ = c;
        if (!(cfg_.intervalS >= 0.1) || !std::isfinite(cfg_.intervalS)) { cfg_.intervalS = 2.0; }
        cfg_.intervalS = std::min(cfg_.intervalS, 3600.0);
        cfg_.dwellMs = std::max(10, std::min(cfg_.dwellMs, 10000));
        cfg_.hangMs = std::max(0, std::min(cfg_.hangMs, 600000));
        reset();
    }
    const PriorityConfig& config() const { return cfg_; }

    enum class State { Idle, Checking, Holding };
    State state() const { return state_; }
    int currentIndex() const { return idx_; }
    // Total times each check cycle found a signal (a statistic for the scan log).
    std::uint64_t hits() const { return hits_; }

    void reset() { state_ = State::Idle; idx_ = -1; haveLast_ = false; nextCheckMs_ = 0.0; sinceCheckMs_ = 0.0; phaseMs_ = 0.0; }

    // One step. `nowMs` is the clock; `signalHere` is the squelch / carrier state of whatever the radio is tuned to NOW (a priority channel while checking
    // or holding); `scannerHolding` says the scanner is listening to an ordinary signal (Paused or Holding in core/scanner.hpp), which a check may only
    // interrupt when interruptHold is set. Returns the action for the caller (retune, or go back to scanning where it was).
    PriorityAction update(double nowMs, bool signalHere, bool scannerHolding) {
        PriorityAction a;
        if (!cfg_.enabled || ch_.empty() || !std::isfinite(nowMs)) {
            if (state_ != State::Idle) { reset(); a.kind = PriorityAction::Kind::ReturnToScan; }
            return a;
        }
        const double delta = haveLast_ ? (nowMs > lastMs_ ? nowMs - lastMs_ : 0.0) : 0.0;
        if (!haveLast_) { haveLast_ = true; nextCheckMs_ = nowMs + cfg_.intervalS * 1000.0; }
        lastMs_ = nowMs;
        switch (state_) {
            case State::Idle:
                // a backward jump of the clock postpones: elapsed time is kept in `sinceCheckMs_`
                sinceCheckMs_ += delta;
                if (sinceCheckMs_ >= cfg_.intervalS * 1000.0 && (!scannerHolding || cfg_.interruptHold)) {
                    state_ = State::Checking; idx_ = 0; phaseMs_ = 0.0;
                    a.kind = PriorityAction::Kind::GoPriority; a.index = 0; a.hz = ch_[0];
                }
                break;
            case State::Checking:
                phaseMs_ += delta;
                if (phaseMs_ >= cfg_.dwellMs) {
                    if (signalHere) {
                        state_ = State::Holding; phaseMs_ = 0.0; ++hits_;
                    } else if (static_cast<std::size_t>(idx_) + 1 < ch_.size()) {
                        ++idx_; phaseMs_ = 0.0;
                        a.kind = PriorityAction::Kind::GoPriority; a.index = idx_; a.hz = ch_[static_cast<std::size_t>(idx_)];
                    } else {
                        state_ = State::Idle; idx_ = -1; sinceCheckMs_ = 0.0;
                        a.kind = PriorityAction::Kind::ReturnToScan;
                    }
                }
                break;
            case State::Holding:
                if (signalHere) { phaseMs_ = 0.0; }
                else {
                    phaseMs_ += delta;
                    if (phaseMs_ >= cfg_.hangMs) {
                        state_ = State::Idle; idx_ = -1; sinceCheckMs_ = 0.0;
                        a.kind = PriorityAction::Kind::ReturnToScan;
                    }
                }
                break;
        }
        return a;
    }

private:
    std::vector<double> ch_;
    PriorityConfig cfg_;
    State state_ = State::Idle;
    int idx_ = -1;
    double phaseMs_ = 0.0, lastMs_ = 0.0, nextCheckMs_ = 0.0, sinceCheckMs_ = 0.0;
    bool haveLast_ = false;
    std::uint64_t hits_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 2. Lockouts.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct LockEntry {
    double loHz = 0.0, hiHz = 0.0;
    bool permanent = false;
    double untilMs = 0.0;                 // temporary entries: 0 = until cleared or the program ends; else the clock value at which the entry expires
};

class LockoutList {
public:
    static constexpr std::size_t kMaxEntries = 1024;      // per kind

    // Locks the range [lo, hi] (a single frequency is lo == hi, or a width of half a step each side). Temporary entries last until clear / expiry / exit;
    // permanent ones are saved (toJson). False when the numbers are invalid or the list is full.
    bool lock(double loHz, double hiHz, bool permanent, double nowMs = 0.0, double ttlMs = 0.0) {
        if (!validHz(loHz) || !validHz(hiHz) || hiHz < loHz || !std::isfinite(ttlMs) || ttlMs < 0.0 || !std::isfinite(nowMs)) { return false; }
        auto& list = permanent ? perm_ : temp_;
        const double until = (!permanent && ttlMs > 0.0) ? nowMs + ttlMs : 0.0;
        // PERMANENT entries merge when they overlap or touch. TEMPORARY entries never stretch another entry's lifetime: a request wholly inside an entry that lasts
        // at least as long is already covered (no change), an overlap with an entry of the SAME expiry merges, anything else is an entry of its own.
        for (LockEntry& e : list) {
            if (permanent) {
                if (loHz <= e.hiHz && hiHz >= e.loHz) { e.loHz = std::min(e.loHz, loHz); e.hiHz = std::max(e.hiHz, hiHz); return true; }
            } else {
                const bool covers = e.untilMs == 0.0 || (until != 0.0 && e.untilMs >= until);
                if (covers && loHz >= e.loHz && hiHz <= e.hiHz) { return true; }
                if (e.untilMs == until && loHz <= e.hiHz && hiHz >= e.loHz) { e.loHz = std::min(e.loHz, loHz); e.hiHz = std::max(e.hiHz, hiHz); return true; }
            }
        }
        if (list.size() >= kMaxEntries) { return false; }
        LockEntry e; e.loHz = loHz; e.hiHz = hiHz; e.permanent = permanent; e.untilMs = until;
        list.push_back(e);
        return true;
    }
    bool lockFrequency(double hz, double halfWidthHz, bool permanent, double nowMs = 0.0, double ttlMs = 0.0) {
        if (!validHz(hz) || !std::isfinite(halfWidthHz) || halfWidthHz < 0.0) { return false; }
        return lock(std::max(0.0, hz - halfWidthHz), hz + halfWidthHz, permanent, nowMs, ttlMs);
    }

    // Is `hz` inside any entry that has not expired at `nowMs`?
    bool isLocked(double hz, double nowMs = 0.0) const {
        if (!std::isfinite(hz)) { return false; }
        for (const LockEntry& e : perm_) { if (hz >= e.loHz && hz <= e.hiHz) { return true; } }
        for (const LockEntry& e : temp_) { if (hz >= e.loHz && hz <= e.hiHz && (e.untilMs == 0.0 || nowMs < e.untilMs)) { return true; } }
        return false;
    }

    // Removes every entry (either kind) that contains `hz`. Returns how many were removed.
    int unlockAt(double hz) {
        int n = 0;
        auto cut = [&](std::vector<LockEntry>& l) { const auto it = std::remove_if(l.begin(), l.end(), [&](const LockEntry& e) { return hz >= e.loHz && hz <= e.hiHz; }); n += static_cast<int>(l.end() - it); l.erase(it, l.end()); };
        cut(perm_); cut(temp_);
        return n;
    }
    // Removes every entry that overlaps [lo, hi].
    int unlockRange(double loHz, double hiHz) {
        int n = 0;
        auto cut = [&](std::vector<LockEntry>& l) { const auto it = std::remove_if(l.begin(), l.end(), [&](const LockEntry& e) { return loHz <= e.hiHz && hiHz >= e.loHz; }); n += static_cast<int>(l.end() - it); l.erase(it, l.end()); };
        cut(perm_); cut(temp_);
        return n;
    }
    void clearTemporary() { temp_.clear(); }
    void clearAll() { temp_.clear(); perm_.clear(); }
    // Drops temporary entries that have expired at `nowMs`; returns how many.
    int expire(double nowMs) {
        const auto it = std::remove_if(temp_.begin(), temp_.end(), [&](const LockEntry& e) { return e.untilMs != 0.0 && nowMs >= e.untilMs; });
        const int n = static_cast<int>(temp_.end() - it);
        temp_.erase(it, temp_.end());
        return n;
    }
    std::size_t permanentCount() const { return perm_.size(); }
    std::size_t temporaryCount() const { return temp_.size(); }
    const std::vector<LockEntry>& permanent() const { return perm_; }
    const std::vector<LockEntry>& temporary() const { return temp_; }

    // Only the permanent entries are saved (a temporary lockout ends with the program, by definition).
    nlohmann::json toJson() const {
        nlohmann::json j = nlohmann::json::array();
        for (const LockEntry& e : perm_) { j.push_back({e.loHz, e.hiHz}); }
        return j;
    }
    bool fromJson(const nlohmann::json& j) {
        if (!j.is_array() || j.size() > kMaxEntries) { return false; }
        std::vector<LockEntry> next;
        for (const auto& p : j) {
            if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) { return false; }
            LockEntry e; e.loHz = p[0].get<double>(); e.hiHz = p[1].get<double>(); e.permanent = true;
            if (!validHz(e.loHz) || !validHz(e.hiHz) || e.hiHz < e.loHz) { return false; }
            next.push_back(e);
        }
        perm_ = std::move(next);
        return true;
    }

private:
    std::vector<LockEntry> perm_, temp_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 3. Scan log.
// ---------------------------------------------------------------------------------------------------------------------------------------------
enum class StopReason : int { Signal = 0, Priority = 1, Manual = 2 };
enum class EndReason : int { SignalGone = 0, Skipped = 1, ListenLimit = 2, Stopped = 3, Superseded = 4, Locked = 5 };

inline const char* stopReasonName(StopReason r) { switch (r) { case StopReason::Signal: return "signal"; case StopReason::Priority: return "priority"; default: return "manual"; } }
inline const char* endReasonName(EndReason r) {
    switch (r) { case EndReason::SignalGone: return "signal_gone"; case EndReason::Skipped: return "skipped"; case EndReason::ListenLimit: return "listen_limit"; case EndReason::Stopped: return "stopped";
                 case EndReason::Superseded: return "superseded"; default: return "locked"; }
}

struct ScanEvent {
    std::int64_t startUnixMs = 0;
    double hz = 0.0;
    std::string mode, label;
    StopReason stop = StopReason::Signal;
    EndReason end = EndReason::SignalGone;
    double durationMs = 0.0;
    double peakDb = -200.0, meanDb = -200.0;             // level readings the caller supplies (dBFS); mean is time-weighted
};

struct FreqStat {
    double hz = 0.0;
    std::uint64_t stops = 0;
    double totalMs = 0.0;
    double longestMs = 0.0;
    std::int64_t lastUnixMs = 0;
    double peakDb = -200.0;
};

class ScanLog {
public:
    explicit ScanLog(std::size_t capacity = 10000) { setCapacity(capacity); }
    void setCapacity(std::size_t n) { cap_ = std::max<std::size_t>(1, std::min<std::size_t>(n, 1000000)); trim(); }

    bool open() const { return open_; }

    // The scanner stopped on `hz` at clock `nowMs` (wall time `unixMs`). A begin while one is open ends the open one as Superseded.
    void begin(std::int64_t unixMs, double nowMs, double hz, const std::string& mode, StopReason why, double levelDb, const std::string& label = {}) {
        if (open_) { end(nowMs, EndReason::Superseded); }
        cur_ = ScanEvent{};
        cur_.startUnixMs = unixMs; cur_.hz = std::isfinite(hz) ? hz : 0.0; cur_.mode = mode.substr(0, 16); cur_.label = label.substr(0, 64); cur_.stop = why;
        startMs_ = nowMs; lastMs_ = nowMs; weighted_ = 0.0; totalWeight_ = 0.0;
        lastLevel_ = std::isfinite(levelDb) ? levelDb : -200.0;
        cur_.peakDb = lastLevel_;
        open_ = true;
    }
    // A level reading while the stop lasts.
    void sample(double nowMs, double levelDb) {
        if (!open_ || !std::isfinite(levelDb)) { return; }
        const double dt = nowMs > lastMs_ ? nowMs - lastMs_ : 0.0;
        weighted_ += lastLevel_ * dt; totalWeight_ += dt;
        lastMs_ = std::max(lastMs_, nowMs);
        lastLevel_ = levelDb;
        cur_.peakDb = std::max(cur_.peakDb, levelDb);
    }
    void end(double nowMs, EndReason why) {
        if (!open_) { return; }
        sample(nowMs, lastLevel_);
        cur_.end = why;
        cur_.durationMs = nowMs > startMs_ ? nowMs - startMs_ : 0.0;
        cur_.meanDb = totalWeight_ > 0.0 ? weighted_ / totalWeight_ : cur_.peakDb;
        push(cur_);
        open_ = false;
    }
    void clear() { ev_.clear(); head_ = 0; dropped_ = 0; open_ = false; }

    std::size_t size() const { return ev_.size(); }
    std::uint64_t dropped() const { return dropped_; }
    // Oldest first.
    std::vector<ScanEvent> events() const {
        std::vector<ScanEvent> out;
        out.reserve(ev_.size());
        for (std::size_t i = 0; i < ev_.size(); ++i) { out.push_back(ev_[(head_ + i) % ev_.size()]); }
        return out;
    }

    // Per-frequency totals, frequencies grouped by whole Hz, ordered by total time descending then frequency.
    std::vector<FreqStat> stats() const {
        std::vector<FreqStat> v;
        for (const ScanEvent& e : events()) {
            const double key = std::floor(e.hz + 0.5);
            auto it = std::find_if(v.begin(), v.end(), [&](const FreqStat& s) { return s.hz == key; });
            if (it == v.end()) { FreqStat s; s.hz = key; v.push_back(s); it = v.end() - 1; }
            ++it->stops; it->totalMs += e.durationMs; it->longestMs = std::max(it->longestMs, e.durationMs); it->lastUnixMs = std::max(it->lastUnixMs, e.startUnixMs); it->peakDb = std::max(it->peakDb, e.peakDb);
        }
        std::sort(v.begin(), v.end(), [](const FreqStat& a, const FreqStat& b) { return a.totalMs != b.totalMs ? a.totalMs > b.totalMs : a.hz < b.hz; });
        return v;
    }

    // CSV (RFC 4180 quoting, header first; a field that starts with = + - @ tab or CR gets a leading apostrophe so a spreadsheet does not run it).
    std::string toCsv() const {
        std::string s = "time_utc,freq_hz,mode,label,duration_s,peak_db,mean_db,stop_reason,end_reason\r\n";
        for (const ScanEvent& e : events()) {
            const civil::DateTime t = civil::fromUnixMs(e.startUnixMs);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ,%.0f,", t.year, t.month, t.day, t.hour, t.minute, t.second, t.millis, std::floor(e.hz + 0.5));
            s += buf;
            s += csvField(e.mode); s += ','; s += csvField(e.label); s += ',';
            std::snprintf(buf, sizeof(buf), "%.3f,%.1f,%.1f,%s,%s\r\n", e.durationMs / 1000.0, e.peakDb, e.meanDb, stopReasonName(e.stop), endReasonName(e.end));
            s += buf;
        }
        return s;
    }

    static std::string csvField(std::string f) {
        if (!f.empty() && (f[0] == '=' || f[0] == '+' || f[0] == '-' || f[0] == '@' || f[0] == '\t' || f[0] == '\r')) { f.insert(f.begin(), '\''); }
        if (f.find_first_of(",\"\r\n") != std::string::npos) {
            std::string q = "\"";
            for (char c : f) { if (c == '"') { q += "\"\""; } else { q += c; } }
            return q + "\"";
        }
        return f;
    }

private:
    void push(const ScanEvent& e) {
        if (ev_.size() < cap_) { ev_.push_back(e); return; }
        ev_[head_] = e; head_ = (head_ + 1) % ev_.size(); ++dropped_;
    }
    void trim() { if (ev_.size() > cap_) { std::vector<ScanEvent> keep = events(); dropped_ += keep.size() - cap_; keep.erase(keep.begin(), keep.end() - static_cast<std::ptrdiff_t>(cap_)); ev_ = std::move(keep); head_ = 0; } }

    std::vector<ScanEvent> ev_;
    std::size_t head_ = 0, cap_ = 10000;
    std::uint64_t dropped_ = 0;
    bool open_ = false;
    ScanEvent cur_;
    double startMs_ = 0.0, lastMs_ = 0.0, weighted_ = 0.0, totalWeight_ = 0.0, lastLevel_ = -200.0;
};

// ---------------------------------------------------------------------------------------------------------------------------------------------
// 4. Mute while scanning.
// ---------------------------------------------------------------------------------------------------------------------------------------------
class ScanMute {
public:
    void setEnabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // After a stop on a signal ends, the audio stays up this long so the tail of the transmission is not clipped (DEFAULT 150 ms; owner can change).
    void setTailMs(int ms) { tailMs_ = std::max(0, std::min(ms, 10000)); }

    // `state` is the scanner's state this tick. Returns true when the audio should be MUTED. Idle, Paused and Holding are never muted (a signal is being
    // listened to or the operator is not scanning); Scanning is muted once the tail has run out.
    bool update(Scanner::State state, double nowMs) {
        if (!std::isfinite(nowMs)) { return muted_; }
        const bool stepping = state == Scanner::State::Scanning;
        if (!enabled_) { muted_ = false; prevStepping_ = stepping; return false; }
        if (!stepping) { muted_ = false; prevStepping_ = false; if (state == Scanner::State::Idle) { wasIdleOrFirst_ = true; } return false; }
        if (!prevStepping_) {
            // just came back from a stop (or the scan just started): hold the audio open for the tail unless the scan is fresh from Idle
            tailUntilMs_ = wasIdleOrFirst_ ? nowMs : nowMs + tailMs_;
        }
        prevStepping_ = true;
        wasIdleOrFirst_ = false;
        muted_ = nowMs >= tailUntilMs_;
        return muted_;
    }
    // The scanner went Idle (or the operator stopped it): the next Scanning starts muted at once.
    void notifyIdle() { prevStepping_ = false; wasIdleOrFirst_ = true; muted_ = false; }
    bool muted() const { return muted_; }

private:
    bool enabled_ = false;
    int tailMs_ = 150;
    bool muted_ = false, prevStepping_ = false, wasIdleOrFirst_ = true;
    double tailUntilMs_ = 0.0;
};

}  // namespace cascade::core::scanaids
