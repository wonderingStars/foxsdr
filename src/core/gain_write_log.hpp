// gain_write_log.hpp - ONE PLAIN LINE FOR A MANUAL GAIN WRITE, rate limited (user bug A, 2026-11-23).
//
// A report of "changing the gain does nothing" could not be answered from a diagnostics bundle: the log said nothing about which gain was written, to what, or whether anything was written
// at all. Each manual gain write is now a line ("source: gain LNA 16.3 dB"), stage and value, never a frequency. A slider that is dragged writes many times a second, so a stage logs at
// most once per `gapMs` (500): the first write at once, writes inside the gap are held, the LAST held value wins and is logged once the gap has passed (flush(), called from the engine's
// pass, or the next write after the gap). Pure: the caller hands in a monotonic clock and writes the lines.
// KEYED BY RADIO AS WELL AS STAGE (review A5): with several radios open, two radios' "TUNER" are two stages with a gap of their own, and a line for radio 2 or more says so ("... (radio 2)").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cascade::core {

class GainWriteLog {
public:
    explicit GainWriteLog(std::int64_t gapMs = 500) : gap_(gapMs) {}

    // The line for a stage and a value ("source: gain LNA 16.3 dB").
    static std::string line(const std::string& stage, double db, unsigned radio = 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " %.1f dB", db);
        std::string s = "source: gain " + stage + buf;
        if (radio > 0) { s += " (radio " + std::to_string(radio + 1) + ")"; }   // `radio` counts from 0 (the first radio's lines are as they always were)
        return s;
    }

    // A manual gain was written. Returns the lines to log now (none while the stage is inside its gap: the value is held).
    std::vector<std::string> note(const std::string& stage, double db, std::int64_t nowMs, unsigned radio = 0) {
        std::vector<std::string> out;
        Entry& e = entries_[{radio, stage}];
        if (!e.logged || nowMs - e.lastMs >= gap_) {
            out.push_back(line(stage, db, radio));
            e.logged = true;
            e.lastMs = nowMs;
            e.held = false;
        } else {
            e.held = true;
            e.heldDb = db;
        }
        return out;
    }

    // The held values whose gap has now passed (the last value of a burst), each once.
    std::vector<std::string> flush(std::int64_t nowMs) {
        std::vector<std::string> out;
        for (auto& kv : entries_) {
            Entry& e = kv.second;
            if (e.held && nowMs - e.lastMs >= gap_) {
                out.push_back(line(kv.first.second, e.heldDb, kv.first.first));
                e.held = false;
                e.lastMs = nowMs;
            }
        }
        return out;
    }

    bool anyHeld() const {
        for (const auto& kv : entries_) { if (kv.second.held) { return true; } }
        return false;
    }

private:
    struct Entry {
        bool logged = false;
        bool held = false;
        std::int64_t lastMs = 0;
        double heldDb = 0.0;
    };
    std::int64_t gap_;
    std::map<std::pair<unsigned, std::string>, Entry> entries_;
};

}  // namespace cascade::core
