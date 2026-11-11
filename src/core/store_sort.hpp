// store_sort.hpp - the plugin store's sort orders and release-note history (feat/webmodels).
//
// TWO STANDALONE MODELS over core/plugin_repo_data.hpp's PluginCatalogEntry. They fetch nothing, write nothing and print nothing.
//
// 1. SORTING. sortStoreEntries() returns the indices of the entries in the order the store should list them: by name, by "recently updated"
//    (the catalogue's `published` date, newest first), by popularity (installs, most first) or by rating (best first). The catalogue entry has
//    no install count and no rating today: both arrive in a separate StoreStats map keyed by plugin id, so a catalogue or a server that one day
//    carries them needs no change here. An entry with no figure sorts AFTER every entry that has one, and ties break by name then id, so the
//    order is total and stable. A rating is ranked by a damped average, (priorVotes * priorMean + sum) / (priorVotes + votes), so a single
//    five-star vote does not outrank a hundred votes averaging 4.6.
//
//    WHAT IS NOT BUILT: collecting ratings. A rating needs a server that stores votes and an identity to stop one person voting forty times
//    (an account, or a signed install id), which is an account system and a Worker: not built, listed in the record. The aggregate install
//    count could come from the telemetry the product already sends (the usage dataset); that is the owner's decision too.
//
// 2. RELEASE-NOTE HISTORY. The catalogue carries ONE `whatsNew` per plugin, for the version it lists. ReleaseNoteHistory remembers each
//    (source, id, version) it has seen with that version's notes and published date, so the plugin's page can show the notes of the versions
//    this install has met, newest first. FIRST SEEN WINS: a later catalogue that rewrites the notes of a version already recorded changes
//    nothing (a catalogue that rewrites history is a mistake or an attack, the same stance as core/plugin_repo.hpp's no-downgrade rule).
//    toJson()/fromJson() let the host keep it in the plugins manifest; fromJson() reads outside bytes and is fuzzed. Caps: kMaxHistoryPerPlugin
//    versions per plugin (the oldest by version fall off), kMaxHistoryPlugins plugins, notes cut at kMaxNoteBytes on a character boundary.
//
// The version comparison here is a COPY of PluginRepo::compareVersions (same rules, listed in plugin_repo.hpp near line 640), because this header
// is for the store window, which may not include plugin_repo.hpp (the fence); tests/test_store_sort.cpp runs both over the same inputs and fails
// if they ever disagree.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/plugin_repo_data.hpp"

namespace cascade::core {

enum class StoreSortKey { Name, Updated, Popular, Rating };

struct StoreStats {
    std::uint64_t installs = 0;
    double ratingAverage = 0.0;      // 0..5; ignored when ratingCount is 0 or the value is not finite
    std::uint32_t ratingCount = 0;
};
using StoreStatsMap = std::map<std::string, StoreStats>;  // keyed by PluginCatalogEntry::id

inline constexpr double kRatingPriorVotes = 5.0;
inline constexpr double kRatingPriorMean = 3.0;

// The damped rating (see above); 0 for an entry with no usable rating.
double dampedRating(const StoreStats& s);

// SemVer-like comparison, <0 / 0 / >0 (copy of PluginRepo::compareVersions).
int compareStoreVersions(const std::string& a, const std::string& b);

// "YYYY-MM-DD" with a real month and day (not a leap-year check beyond day <= 31 per month rules below); false otherwise.
bool validStoreDate(const std::string& s);

std::vector<std::size_t> sortStoreEntries(const std::vector<PluginCatalogEntry>& entries, const StoreStatsMap& stats, StoreSortKey key);

inline constexpr std::size_t kMaxHistoryPerPlugin = 50;
inline constexpr std::size_t kMaxHistoryPlugins = 400;
inline constexpr std::size_t kMaxNoteBytes = 1000;
inline constexpr std::size_t kMaxHistoryJsonBytes = 4 * 1024 * 1024;

struct ReleaseNote {
    std::string version;
    std::string published;  // "" when the catalogue gave none or a malformed one
    std::string notes;      // may be empty: the catalogue said nothing for that version
};

class ReleaseNoteHistory {
public:
    // Records every entry's (source, id, version) not seen before. Returns how many were new. Entries with an empty id or version are ignored.
    std::size_t observe(const std::vector<PluginCatalogEntry>& entries);
    // Newest version first. `source` is PluginCatalogEntry::source ("" = the official catalogue).
    std::vector<ReleaseNote> history(const std::string& id, const std::string& source = std::string()) const;
    std::size_t pluginCount() const { return map_.size(); }

    std::string toJson() const;
    // Replaces `into` only when the document is understood; entries that fail validation are skipped. Never throws.
    static bool fromJson(const std::string& json, ReleaseNoteHistory& into);

private:
    bool add(const std::string& source, const std::string& id, ReleaseNote note);
    std::map<std::string, std::vector<ReleaseNote>> map_;  // key: source + '\n' + id
};

}  // namespace cascade::core
