// bookmark.hpp - one bookmarked frequency: the DATA of core/freq_manager.hpp (shell retirement R11a, 2026-10-29).
//
// The window draws, edits and imports bookmarks and needs the entry type and none of the machinery that keeps the list (FreqManager: the sorted
// list, the atomic save, the load repair). The struct moved here verbatim from freq_manager.hpp, which includes it, so every user of FreqManager
// sees the same Bookmark as before; the interface includes this header and not freq_manager.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

namespace cascade::core {

struct Bookmark {
    std::string name;
    double freqHz = 0;
    std::string mode = "WFM";
    double bandwidthHz = 150000;
    std::string group;       // "" = ungrouped
    bool favourite = false;
    // STABLE FOR THE ENTRY'S LIFETIME IN THIS SESSION, never saved (engine
    // extraction stage 1: the engine API names a bookmark by id, not by list
    // index, because an index moves under every insert). Assigned by
    // FreqManager on every way an entry enters the list, kept across updateAt,
    // and never reused within a session; 0 means "not in a list".
    std::uint64_t id = 0;
    bool scan = false;          // ticked: scanned / monitored
    double heardSeconds = 0.0;  // squelch-open time measured here
};

}  // namespace cascade::core
