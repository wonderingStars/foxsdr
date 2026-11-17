// s4b_swl.hpp - screen lane S4b, G3-49: the listening (SWL) log. The writer of the log's file format existed (core/adif.hpp: ADIF 3.1.8 records for a listener's report) and nothing made entries; this
// keeps them. An entry is one thing the PERSON chose to log while listening: the engine fills in the time, the frequency, the mode and the signal level from the radio, the person types the station
// heard and a note. Nothing is logged automatically.
//
// PERSONAL DATA, ON PURPOSE AND ONLY HERE. The log is the person's own file, swl-log.json beside the settings; it is exported to a path they name, and to nowhere else. It is never a setting, never
// in a usage or crash report, the diagnostics log or a fact (the facts carry a COUNT and the model's problem sentences).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/adif.hpp"

namespace cascade::engine::s4b {

inline constexpr std::size_t kMaxSwlEntries = 20000;

class SwlBook {
public:
    explicit SwlBook(std::filesystem::path file) : file_(std::move(file)) { load(); }

    std::size_t size() const { return entries_.size(); }
    const std::string& error() const { return error_; }
    const std::vector<cascade::core::adif::ListenEntry>& entries() const { return entries_; }

    // One entry from JSON {"call":"BBC","station":"World Service","notes":"..."} with the radio's own figures beside it (the caller gives them). False with `why`.
    bool add(const std::string& json, std::int64_t nowUnix, double frequencyHz, const std::string& mode, double peakDbfs, int radio, std::string& why);
    bool remove(std::size_t oneBasedIndex, std::string& why);
    bool clear();
    // Writes the ADIF file at `path`: a full path ending in .adi or .adif. `written` / `skipped` count the entries. False with `why`.
    bool exportAdif(const std::string& path, const std::string& operatorCall, const std::string& grid, std::int64_t nowUnix, int& written, int& skipped, std::string& why) const;

private:
    void load();
    bool save();

    std::filesystem::path file_;
    std::vector<cascade::core::adif::ListenEntry> entries_;
    std::string error_;
};

// What the engine reads from the radio when the person logs what they hear.
struct RadioFigures {
    bool valid = false;
    double hz = 0.0;
    std::string mode;
    double peakDbfs = 0.0;     // NaN when unknown
    int radio = 1;
};

// FOXAPP_OP_SWL: text JSON {"action":"add","call":"","station":"","notes":""} | {"action":"remove","index":N} | {"action":"clear"} | {"action":"export","path":"<full path ending .adi or .adif>"}.
// Returns the FOXAPI status; `why` carries the sentence of a refusal, `exported` the sentence of a good export.
std::int32_t swlOp(SwlBook& book, const std::string& text, const RadioFigures& rf, std::int64_t nowUnix, const std::string& operatorCall, const std::string& grid, std::string& why, std::string& exported);

}  // namespace cascade::engine::s4b
