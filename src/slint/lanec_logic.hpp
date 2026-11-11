// lanec_logic.hpp - SCREEN LANE C (docs/multi-radio-records/screen-c.md): the pure rules of the tuning, scanning, memories and controls rows built in the Slint window, no Slint, no Win32.
//
//   C2/C3/C1/C5  the scanner block: the priority list typed as MHz, the lockouts of a LIST scan (the window unticks the bookmark: the one lockout the engine honours today), the mute-while-scanning
//                follower (SET_MUTED around a scan's listening), per-entry memories (squelch, scan threshold, repeater offset and tone, kept by the window)
//   C4           ScanWatcher: what the scan found (hits, time heard) from the state the engine publishes, and the CSV line a log appends
//   C12/T10      MemoryBook: the per-bookmark notes and what a recall sends
//   T5           QuickMemories: eight registers (frequency, mode, bandwidth)
//   T3           the tuning-step lock (a saved flag, read by the host's nudge)
//   T8           the delta lock: click-to-tune and the VFO slider move the CENTRE, the channel stays where it is (deltaLockApi / decideDelta)
//   F7           BandwidthMemory: the width each mode was last used at
//   T12          clockText
//   T9           (core/airband_data: airbandChannelNames)
//   U10          shortcutRows
//   P11          listRecordings
//
// Persistence: ONE small file, `lanec.json`, beside the settings file (never inside config.json: lane C added no key to either half of the settings).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "system_logic.hpp"

namespace foxsdr::slintlanec {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kQuickMemories = 8;

// ---- what one bookmark carries that the engine's bookmark does not (C12, C1, T10); kept by the window under name + frequency ----
struct Memory {
    double squelchDb = kNaN;        // C12: sent with a recall; NaN = leave the squelch alone
    double thresholdDb = kNaN;      // C1: the scan threshold for this entry (the engine's scanner reads it when wired: Params::thresholdDb)
    double offsetKhz = 0.0;         // T10: repeater shift, signed (-600 = transmit 600 kHz below the receive frequency); 0 = simplex
    double toneHz = 0.0;            // T10: CTCSS tone, 0 = none (stored; applied when the tone squelch is wired: F14)
    bool empty() const { return std::isnan(squelchDb) && std::isnan(thresholdDb) && offsetKhz == 0.0 && toneHz == 0.0; }
    bool operator==(const Memory& o) const {
        const auto same = [](double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; };
        return same(squelchDb, o.squelchDb) && same(thresholdDb, o.thresholdDb) && offsetKhz == o.offsetKhz && toneHz == o.toneHz;
    }
};
std::string memoryKey(const std::string& name, double hz);   // "<name>|<whole hertz>"

struct QuickMemory {
    bool set = false;
    double hz = 0.0;
    std::uint32_t mode = 0;   // FOXAPI_DEMOD_*
    double bandwidthHz = 0.0;
    bool operator==(const QuickMemory&) const = default;
};

// ---- the saved record ----
struct Store {
    bool stepLock = false;                         // T3
    std::uint32_t deltaMask = 0;                   // T8, bit i = radio i + 1
    bool bwMemory = false;                         // F7
    std::map<std::uint32_t, double> bwByMode;      // F7: FOXAPI_DEMOD_* -> Hz
    std::vector<double> priorityHz;                // C2 (the scanner's priority channels)
    double priorityEveryS = 30.0;                  // C2
    bool muteScan = false;                         // C5
    bool logScan = false;                          // C4
    int clockMode = 1;                             // T12: 0 off, 1 UTC, 2 local
    std::array<QuickMemory, kQuickMemories> quick{};   // T5
    std::map<std::string, Memory> memories;        // C12 / C1 / T10 by memoryKey
    bool operator==(const Store&) const = default;
    std::string toJson() const;
    static Store fromJson(const std::string& text);   // lenient: a bad field keeps its default, a bad file is an empty Store
};

// ---- C2: the priority list as the field shows and reads it ("162.550, 162.400"): MHz, at most 16, a bad token dropped ----
std::vector<double> parsePriorityList(const std::string& text);
std::string priorityText(const std::vector<double>& hz);
// "30" -> 30 s, clamped to 5..600; nullopt for text that is no number
std::optional<double> parsePriorityEvery(const std::string& text);
// a number of dB typed in a memory field ("-70", "off"/"" = unset), clamped to -200..20 for a squelch; nullopt = unreadable
std::optional<double> parseDbField(const std::string& text, bool& unset);
std::string dbFieldText(double v);   // "" for NaN
std::optional<double> parseKhzField(const std::string& text);   // repeater shift, signed kHz, |x| <= 100000
std::optional<double> parseToneField(const std::string& text);  // CTCSS Hz 0 or 60..260 (the standard tones lie in 67 to 254.1); 0/"" = none

// ---- C3: lockouts of a LIST scan. A lockout unticks the bookmark; a temporary one ticks it again when its time is up; clear ticks back what we unticked ----
struct ListLockout { int bookmarkId = 0; double hz = 0.0; double untilMs = 0.0; /* 0 = until cleared */ };
class ListLockouts {
public:
    void add(int bookmarkId, double hz, double nowMs, double forMs);   // forMs <= 0: until cleared; the same id twice refreshes
    std::vector<int> due(double nowMs);                                 // ids whose time is up (removed from the list)
    std::vector<int> clear();                                           // every id we unticked
    std::size_t size() const { return rows_.size(); }
    bool has(int bookmarkId) const;
    const std::vector<ListLockout>& rows() const { return rows_; }
private:
    std::vector<ListLockout> rows_;
};

// ---- C5: the follower that mutes the receiver while the scan steps and lets go of it while the scan listens; never touches a mute the user set ----
class ScanMuteFollower {
public:
    // The command to send now: SET_MUTED 1 when a scan has started stepping, SET_MUTED 0 when it found a signal or ended (only if WE muted); nullopt otherwise.
    std::optional<int> update(bool enabled, std::uint32_t scannerState /* FOXAPI_SCANNER_* */, bool userMuted);
    bool weMuted() const { return weMuted_; }
private:
    bool weMuted_ = false;
};

// ---- C4: what the scan found ----
struct ScanHit { double hz = 0.0; int count = 0; double heardMs = 0.0; double lastMs = 0.0; };
class ScanWatcher {
public:
    // Fed once a frame with the engine's clock (ms), the scanner state (FOXAPI_SCANNER_*) and the frequency it is on. Returns the CSV line of a hit that has just ended ("" otherwise).
    std::string observe(double nowMs, std::uint32_t scannerState, double hz, std::int64_t unixSeconds);
    const std::vector<ScanHit>& hits() const { return hits_; }   // by frequency
    int found() const { return found_; }                          // signals found, all frequencies
    double totalHeardMs() const { return totalHeardMs_; }
    const ScanHit* busiest() const;
    void reset();
    static std::string csvHeader() { return "time_utc,frequency_hz,heard_seconds"; }
private:
    bool listening_ = false;
    double hz_ = 0.0;
    double sinceMs_ = 0.0;
    std::vector<ScanHit> hits_;
    int found_ = 0;
    double totalHeardMs_ = 0.0;
};

// ---- T5: quick memories ----
// The commands a recall sends: the frequency first (a landing in another band of the plan may take that band's mode), then the mode, then the width (the mode resets it).
std::vector<FoxCommand> recallCommands(const QuickMemory& q);
std::string quickLabel(int index, const QuickMemory& q);   // "M3  145.500" / "M3" while empty

// ---- C12 / T10: what a recall of a memory sends besides the engine's own BOOKMARK_TUNE ----
std::vector<FoxCommand> memoryRecallCommands(const Memory& m);   // the squelch; nothing for NaN
std::string offsetText(double hz, const Memory& m);              // "TX 145.600 MHz" for a repeater entry, "" for simplex

// ---- T8: the delta lock ----
enum class DeltaVerdict { Pass, Converted, Refused };
// The channel keeps its place in the picture: click-to-tune (FOXAPP_OP_VFO_TO_ABSOLUTE) becomes a retune of the counter (SET_FREQUENCY: the VFO offset kept, the centre follows); the VFO offset
// itself (SET_VFO_OFFSET, the slider) is refused. `c` is changed in the Converted case.
DeltaVerdict decideDelta(FoxCommand& c);
// A copy of `real` (itself possibly the frequency lock's copy) whose submit and submit_text apply decideDelta to a radio whose delta lock is on. `mask` is read live.
using DeltaMaskFn = std::uint32_t (*)();
using DeltaRadioFn = int (*)(const FoxCommand&);
const FoxEngineApi* deltaLockApi(const FoxEngineApi* real, DeltaMaskFn mask, DeltaRadioFn radioOf);
long deltaRefusals();
long deltaConversions();

// ---- F7: the width each mode was last used at ----
class BandwidthMemory {
public:
    // Fed once a tick with the state; returns a width to apply (SET_BANDWIDTH) when a mode was just entered that has a remembered one. Records the user's width changes once they have settled.
    std::optional<double> update(bool enabled, std::uint32_t mode, double bandwidthHz, long tick, std::map<std::uint32_t, double>& memory, bool& changed);
private:
    std::uint32_t mode_ = 0;
    double lastBw_ = 0.0;
    long modeSince_ = 0;
    long bwSince_ = 0;
    bool userMoved_ = false;
    std::optional<double> expect_;
    bool seeded_ = false;
};

// ---- T12 ----
// mode 0 off (""), 1 "14:32:05 UTC", 2 "15:32:05 LT" (local time: `localOffsetSeconds` east of UTC).
std::string clockText(int mode, std::int64_t unixSeconds, int localOffsetSeconds);
int localOffsetSecondsNow(std::int64_t unixSeconds);   // the OS's, with its daylight rule; 0 where it cannot say

// ---- U10 ----
struct ShortcutRow { std::string keys; std::string what; };
// Every bound action of the saved key table (keys first) and the fixed ones the window answers outside it.
std::vector<ShortcutRow> shortcutRows(const foxsdr::slintsystem::KeyTable& table);

// ---- P11 ----
struct RecordingFile { std::string name; std::string path; std::uint64_t bytes = 0; std::int64_t unixSeconds = 0; std::string kind; /* "I/Q" "Audio" "Other" */ };
// The recordings in a folder (not its subfolders), newest first, at most `limit`; a missing folder is an empty list.
std::vector<RecordingFile> listRecordings(const std::string& dir, std::size_t limit);
std::string sizeText(std::uint64_t bytes);          // "812 B", "3.4 KB", "512.0 MB", "1.2 GB"
std::string dateText(std::int64_t unixSeconds);     // "2026-11-11 14:32" UTC
bool isIqWav(const std::string& path);              // a .wav, played through the I/Q file source

}  // namespace foxsdr::slintlanec
