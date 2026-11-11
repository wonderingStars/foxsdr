// See schedule.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/schedule.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace cascade::core::sched {
namespace {

constexpr std::int64_t kEarliestUnix = 1577836800;    // 2020-01-01
constexpr std::int64_t kLatestUnix = 4102444800;      // 2100-01-01

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) { --q; }
    return q;
}

// Everything validate() checks except that a ONCE start is not in the past (a file is read long after it was written).
std::string shapeProblem(const Entry& e) {
    if (e.radio < 1 || e.radio > 8) { return "the radio must be a number from 1 to 8"; }
    if (e.lengthS < kMinLengthS || e.lengthS > kMaxLengthS) { return "the length must be between 1 second and 24 hours"; }
    if (static_cast<int>(e.repeat) < 0 || static_cast<int>(e.repeat) > 2) { return "the repeat must be once, daily or weekly"; }
    if (e.startUnix < kEarliestUnix || e.startUnix > kLatestUnix) { return "the start time is not a date between 2020 and 2100"; }
    if (e.name.size() > static_cast<std::size_t>(kMaxNameChars)) { return "the name is longer than 40 characters"; }
    for (const char c : e.name) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) { return "the name holds a control character"; }
    }
    if (e.tune) {
        if (!std::isfinite(e.freqHz) || e.freqHz <= 0.0 || e.freqHz > 6.0e9) { return "the frequency must be above 0 Hz and no higher than 6 GHz"; }
        if (e.mode < 0 || e.mode > 7) { return "the mode is not one a receiver can be set to"; }
        if (!std::isfinite(e.bandwidthHz) || e.bandwidthHz < 0.0 || e.bandwidthHz > 20.0e6) { return "the bandwidth must be between 0 and 20 MHz"; }
    }
    if (!e.recordAudio && !e.recordIq) { return "an entry must record the audio, the I/Q, or both"; }
    return std::string();
}

}  // namespace

std::int64_t latestStartAtOrBefore(const Entry& e, std::int64_t t) {
    if (t < e.startUnix) { return -1; }
    const std::int64_t p = periodS(e.repeat);
    if (p == 0) { return e.startUnix; }
    return e.startUnix + floorDiv(t - e.startUnix, p) * p;
}

std::int64_t nextStartAfter(const Entry& e, std::int64_t t) {
    const std::int64_t p = periodS(e.repeat);
    if (e.startUnix > t) { return e.startUnix; }
    if (p == 0) { return -1; }
    return e.startUnix + (floorDiv(t - e.startUnix, p) + 1) * p;
}

int unhandledCount(const Entry& e, std::int64_t t) {
    const std::int64_t latest = latestStartAtOrBefore(e, t);
    if (latest < 0 || latest <= e.lastStartUnix) { return 0; }
    const std::int64_t p = periodS(e.repeat);
    if (p == 0) { return 1; }
    const std::int64_t first = nextStartAfter(e, e.lastStartUnix);   // the first occurrence after the one last handled
    if (first < 0 || first > latest) { return 0; }
    const std::int64_t n = (latest - first) / p + 1;
    return static_cast<int>(std::min<std::int64_t>(n, kMaxMissedCount + 1));
}

std::string validate(const Entry& e, std::int64_t nowUnix) {
    std::string s = shapeProblem(e);
    if (!s.empty()) { return s; }
    if (e.repeat == Repeat::Once && e.startUnix + e.lengthS <= nowUnix) { return "the start time has already passed"; }
    return std::string();
}

std::vector<Entry> sanitise(std::vector<Entry> in) {
    std::vector<Entry> out;
    std::set<std::uint32_t> used;
    for (Entry& e : in) {
        if (static_cast<int>(out.size()) >= kMaxEntries) { break; }
        if (e.name.size() > static_cast<std::size_t>(kMaxNameChars)) { e.name.resize(kMaxNameChars); }
        for (char& c : e.name) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) { c = ' '; }
        }
        if (!shapeProblem(e).empty()) { continue; }
        if (!e.tune) { e.freqHz = 0.0; e.mode = 0; e.bandwidthHz = 0.0; }
        if (e.lastStartUnix < 0) { e.lastStartUnix = 0; }
        if (e.createdUnix < 0) { e.createdUnix = 0; }
        e.missed = std::max(0, std::min(kMaxMissedCount, e.missed));
        if (static_cast<int>(e.lastOutcome) < 0 || static_cast<int>(e.lastOutcome) > 4) { e.lastOutcome = Outcome::None; }
        if (e.id == 0 || used.count(e.id) != 0) { e.id = 0; }
        else { used.insert(e.id); }
        out.push_back(std::move(e));
    }
    std::uint32_t next = 1;
    for (Entry& e : out) {
        if (e.id != 0) { continue; }
        while (used.count(next) != 0) { ++next; }
        e.id = next;
        used.insert(next);
    }
    return out;
}

bool overlaps(const Entry& a, const Entry& b, std::int64_t from) {
    if (a.radio != b.radio) { return false; }
    // a's occurrences that end after `from` and start before the horizon
    std::int64_t s = latestStartAtOrBefore(a, from);
    if (s < 0 || s + a.lengthS <= from) { s = nextStartAfter(a, from); }
    const std::int64_t limit = from + kOverlapHorizonS;
    for (int i = 0; i < 64 && s >= 0 && s <= limit; ++i) {
        const std::int64_t end = s + a.lengthS;
        const std::int64_t bs = latestStartAtOrBefore(b, end - 1);
        if (bs >= 0 && bs + b.lengthS > s) { return true; }
        s = nextStartAfter(a, s);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------

Entry* Runner::find(std::uint32_t id) {
    for (Entry& e : entries_) {
        if (e.id == id) { return &e; }
    }
    return nullptr;
}

void Runner::setEntries(std::vector<Entry> entries) { entries_ = std::move(entries); }

bool Runner::active(std::uint32_t id) const {
    for (const Run& r : active_) {
        if (r.id == id) { return true; }
    }
    return false;
}

std::int64_t Runner::activeSince(std::uint32_t id) const {
    for (const Run& r : active_) {
        if (r.id == id) { return r.startedMs; }
    }
    return 0;
}

std::vector<Decision> Runner::tick(std::int64_t nowMs) {
    std::vector<Decision> out;
    const std::int64_t nowS = floorDiv(nowMs, 1000);
    // ---- recordings that must end ----
    for (const Run& r : active_) {
        const Entry* e = nullptr;
        for (const Entry& x : entries_) {
            if (x.id == r.id) { e = &x; }
        }
        Decision d;
        d.kind = Decision::Kind::Stop;
        d.id = r.id;
        d.occurrence = r.occurrence;
        if (e == nullptr || !e->enabled || e->radio != r.radio) {
            d.why = "the entry was removed, disabled or moved to another radio";
        } else if (nowMs >= r.startedMs + r.lengthS * 1000) {
            d.why = "the length was reached";
        } else if (nowMs < r.startedMs - kBackStepMs) {
            d.why = "the clock was set back";
        } else {
            continue;
        }
        out.push_back(std::move(d));
    }
    // ---- runs asked for by hand ----
    for (const std::uint32_t id : runNow_) {
        Entry* e = find(id);
        if (e == nullptr || active(id) || std::find(pendingStart_.begin(), pendingStart_.end(), id) != pendingStart_.end()) { continue; }
        Decision d;
        d.kind = Decision::Kind::Start;
        d.id = id;
        d.occurrence = 0;
        pendingStart_.push_back(id);
        out.push_back(std::move(d));
    }
    runNow_.clear();
    // ---- scheduled starts, and the starts that were missed ----
    for (Entry& e : entries_) {
        if (!e.enabled || active(e.id) || std::find(pendingStart_.begin(), pendingStart_.end(), e.id) != pendingStart_.end()) { continue; }
        const int n = unhandledCount(e, nowS);
        if (n == 0) { continue; }
        const std::int64_t latest = latestStartAtOrBefore(e, nowS);
        const std::int64_t late = nowS - latest;
        const bool startable = late <= kGraceS && late < e.lengthS;
        const int missed = startable ? n - 1 : n;
        if (missed > 0) {
            Decision d;
            d.kind = Decision::Kind::Missed;
            d.id = e.id;
            d.occurrence = startable ? latest - std::max<std::int64_t>(1, periodS(e.repeat)) : latest;
            d.count = missed;
            e.missed = std::min(kMaxMissedCount, e.missed + missed);
            if (!startable) { e.lastOutcome = Outcome::Missed; }
            dirty_ = true;
            out.push_back(std::move(d));
        }
        e.lastStartUnix = latest;
        dirty_ = true;
        if (startable) {
            Decision d;
            d.kind = Decision::Kind::Start;
            d.id = e.id;
            d.occurrence = latest;
            pendingStart_.push_back(e.id);
            out.push_back(std::move(d));
        }
    }
    return out;
}

void Runner::started(std::uint32_t id, std::int64_t occurrence, std::int64_t nowMs) {
    pendingStart_.erase(std::remove(pendingStart_.begin(), pendingStart_.end(), id), pendingStart_.end());
    Entry* e = find(id);
    if (e == nullptr) { return; }
    Run r;
    r.id = id;
    r.radio = e->radio;
    r.occurrence = occurrence;
    r.startedMs = nowMs;
    r.lengthS = e->lengthS;
    active_.push_back(r);
    e->lastOutcome = Outcome::Ran;
    dirty_ = true;
}

void Runner::refused(std::uint32_t id, std::int64_t) {
    pendingStart_.erase(std::remove(pendingStart_.begin(), pendingStart_.end(), id), pendingStart_.end());
    if (Entry* e = find(id)) {
        e->lastOutcome = Outcome::Refused;
        dirty_ = true;
    }
}

void Runner::ended(std::uint32_t id) {
    active_.erase(std::remove_if(active_.begin(), active_.end(), [id](const Run& r) { return r.id == id; }), active_.end());
}

bool Runner::runNow(std::uint32_t id) {
    Entry* e = find(id);
    if (e == nullptr || active(id)) { return false; }
    if (std::find(runNow_.begin(), runNow_.end(), id) == runNow_.end()) { runNow_.push_back(id); }
    return true;
}

bool Runner::skipNext(std::uint32_t id, std::int64_t nowS) {
    Entry* e = find(id);
    if (e == nullptr) { return false; }
    const std::int64_t s = nextStartAfter(*e, std::max(nowS, e->lastStartUnix));
    if (s < 0) { return false; }
    e->lastStartUnix = s;
    e->lastOutcome = Outcome::Skipped;
    dirty_ = true;
    return true;
}

}  // namespace cascade::core::sched
