// core/schedule.hpp - TIMED RECORDINGS, the pure part (design section 4 of the pre-release features): the entry, its rules, and the clock
// that decides when an entry starts and stops. No engine, no files, no sleeping: time is an argument.
//
// MODEL. An entry says: on RADIO r, at START (a UTC time, seconds since 1970), for LENGTH seconds, repeating ONCE, DAILY or WEEKLY (every
// 86 400 / 604 800 seconds from the first start: time is stored in UTC and a repeat is elapsed time, so a daylight-saving change moves the
// local hour of a repeat by an hour - the display's business, not the clock's), optionally TUNE the radio to a frequency / mode / bandwidth
// first, RECORD the audio and/or the I/Q, and optionally put the tuning back at the end ("return to where I was").
//
// THE RULES THE CLOCK KEEPS (tested one by one in tests/test_schedule.cpp):
//  * An entry starts at its time, not more than kGraceS (30 s) LATE. A start that is later than that - the program was closed, the machine
//    slept, the clock jumped forward - is MISSED: counted, shown, and NEVER started silently after the fact.
//  * An occurrence is handled once. `lastStartUnix` is the occurrence last handled (run, missed or refused) and is saved with the entry, so a
//    restart neither runs an occurrence again nor counts it missed twice; a clock set BACK never re-runs one either.
//  * A running recording ends `lengthS` after the moment it began, by the clock it was started on. It also ends at once if the clock is
//    set back by more than kBackStepMs (2 s) while it runs - the end time can no longer be trusted - and the occurrence is then done.
//  * Nothing fires for an entry that is disabled, and removing or disabling an entry ends its running recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SCHEDULE_HPP
#define CASCADE_CORE_SCHEDULE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core::sched {

enum class Repeat : int { Once = 0, Daily = 1, Weekly = 2 };

inline constexpr int kMaxEntries = 32;
inline constexpr int kMaxNameChars = 40;
inline constexpr int kMinLengthS = 1;
inline constexpr int kMaxLengthS = 86400;
inline constexpr std::int64_t kGraceS = 30;
inline constexpr std::int64_t kBackStepMs = 2000;
inline constexpr std::int64_t kOverlapHorizonS = 14 * 86400;   // entries are compared over the next two weeks
inline constexpr int kMaxMissedCount = 999;

// What happened to the entry's latest occurrence.
enum class Outcome : int { None = 0, Ran = 1, Missed = 2, Refused = 3, Skipped = 4 };

struct Entry {
    std::uint32_t id = 0;              // 1.., unique, assigned by the engine, never reused while the engine lives
    std::string name;                  // at most kMaxNameChars bytes
    int radio = 1;                     // 1..8
    std::int64_t startUnix = 0;        // the first (or only) start, UTC seconds
    int lengthS = 60;                  // kMinLengthS..kMaxLengthS
    Repeat repeat = Repeat::Once;
    bool tune = false;                 // tune the radio first
    double freqHz = 0.0;               // the tuned frequency (with tune)
    int mode = 0;                      // FOXAPI_DEMOD_* 1..7, 0 = leave the mode alone
    double bandwidthHz = 0.0;          // 0 = leave it
    bool recordAudio = true;
    bool recordIq = false;
    bool returnToWhereIWas = true;
    bool enabled = true;
    std::int64_t createdUnix = 0;
    std::int64_t lastStartUnix = 0;    // the occurrence last handled; 0 = none yet
    Outcome lastOutcome = Outcome::None;
    int missed = 0;                    // occurrences missed since the entry was made or last edited (capped at kMaxMissedCount)
    bool operator==(const Entry&) const = default;
};

inline std::int64_t periodS(Repeat r) { return r == Repeat::Daily ? 86400 : r == Repeat::Weekly ? 604800 : 0; }

// The latest occurrence start at or before `t`, or -1 when the first start is later. For ONCE it is startUnix or nothing.
std::int64_t latestStartAtOrBefore(const Entry& e, std::int64_t t);
// The first occurrence start strictly after `t`, or -1 (a ONCE entry whose time has passed).
std::int64_t nextStartAfter(const Entry& e, std::int64_t t);
// Occurrences with lastStartUnix < start <= t, counted (at most kMaxMissedCount+1).
int unhandledCount(const Entry& e, std::int64_t t);

// A sentence for what is wrong with `e` ("" when nothing is): the radio, the length, the time, the name, the frequency, the mode, what to
// record. `nowUnix` is the clock: a ONCE entry must not start more than kGraceS in the past.
std::string validate(const Entry& e, std::int64_t nowUnix);
// The same, made safe for the file (damaged entries dropped, values clamped, ids made unique, at most kMaxEntries).
std::vector<Entry> sanitise(std::vector<Entry> in);

// True when two occurrences of a and b - at any time in [from, from + kOverlapHorizonS] - are on the air at the same moment on the SAME radio.
bool overlaps(const Entry& a, const Entry& b, std::int64_t from);

// What the clock decides at one look.
struct Decision {
    enum class Kind { Start, Stop, Missed } kind = Kind::Start;
    std::uint32_t id = 0;
    std::int64_t occurrence = 0;   // the occurrence's start time (UTC seconds); for Missed, the latest of those missed
    int count = 0;                 // Missed: how many occurrences were missed in this look
    std::string why;               // Stop: why
};

class Runner {
public:
    // Replaces the entries. A running recording whose entry is gone, disabled, or moved to another radio ends at the next tick.
    void setEntries(std::vector<Entry> entries);
    const std::vector<Entry>& entries() const { return entries_; }
    // True after tick(), started() or refused() changed an entry's saved fields (lastStartUnix, outcome, missed): the engine saves, then clears it.
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    // One look at the clock. Start decisions are not yet in force: the engine answers with started() or refused().
    std::vector<Decision> tick(std::int64_t nowMs);
    // The engine began the occurrence at `nowMs`.
    void started(std::uint32_t id, std::int64_t occurrence, std::int64_t nowMs);
    // The engine could not begin it (the radio is missing or busy, the disk refuses): the occurrence is used up.
    void refused(std::uint32_t id, std::int64_t occurrence);
    // The engine ended the recording (after a Stop decision, or by itself).
    void ended(std::uint32_t id);
    // Run an entry now, outside its schedule: gives the Start decision at the next tick (the occurrence is `nowS`, marked as handled when run).
    bool runNow(std::uint32_t id);
    // Skip the NEXT occurrence: it is marked handled (Skipped) without starting. False when there is none.
    bool skipNext(std::uint32_t id, std::int64_t nowS);
    bool active(std::uint32_t id) const;
    int activeCount() const { return static_cast<int>(active_.size()); }
    std::int64_t activeSince(std::uint32_t id) const;   // ms on the tick clock, 0 when not running

private:
    struct Run {
        std::uint32_t id = 0;
        int radio = 0;
        std::int64_t occurrence = 0;
        std::int64_t startedMs = 0;
        std::int64_t lengthS = 0;
    };
    Entry* find(std::uint32_t id);
    std::vector<Entry> entries_;
    std::vector<Run> active_;
    std::vector<std::uint32_t> runNow_;
    std::vector<std::uint32_t> pendingStart_;   // Start decisions given and not yet answered
    bool dirty_ = false;
};

}  // namespace cascade::core::sched

#endif  // CASCADE_CORE_SCHEDULE_HPP
