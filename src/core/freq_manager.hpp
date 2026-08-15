// Frequency manager: a named list of bookmarked frequencies, persisted as
// JSON next to the app config. The in-memory list is ALWAYS sorted by
// freqHz ascending — that is the display order of the bookmark panel, and
// keeping the invariant here means no GUI code ever needs to sort.
//
// File format (schema 1):
//   {
//       "schemaVersion": 1,
//       "bookmarks": [
//           { "name": "...", "freqHz": 145500000.0,
//             "mode": "NFM", "bandwidthHz": 12500.0 },
//           ...
//       ]
//   }
//
// Load semantics (mirroring ConfigStore, which callers already rely on):
//   - missing file             -> empty list, returns true (first run)
//   - unreadable / not JSON /  -> empty list, returns false, error says why
//     non-object root
//   - schemaVersion mismatch   -> empty list, returns false (a future schema
//     (present but != 1)          may reinterpret fields; trusting any entry
//                                 would be a guess)
//   - "bookmarks" key missing  -> empty list, returns true (an object with
//                                 nothing in it is a valid empty collection)
//   - "bookmarks" not an array -> empty list, returns false (structural
//                                 damage, not a per-entry problem)
//   - PER-ENTRY TOLERANCE: a damaged entry is skipped, the rest load, and
//     load still returns true — one hand-edited mistake must not take the
//     user's whole bookmark list down. An entry is skipped when it is not a
//     JSON object, or its freqHz is absent / non-numeric / non-finite /
//     negative (a bookmark without a usable frequency points at nothing).
//
// Per-entry sanitization on load (repair, not reject):
//   - name         missing or wrong-typed -> "" (still a valid bookmark)
//   - mode         missing or wrong-typed -> "WFM"; UNKNOWN mode strings are
//                  kept verbatim — the mode list grows over time and a newer
//                  file must survive a round trip through an older build
//   - bandwidthHz  non-numeric / non-finite / <= 0 -> default 150000. The
//                  demod chain divides by bandwidth, so it must be positive;
//                  there is no meaningful positive floor to clamp to, so the
//                  repair value is the struct default rather than an
//                  invented epsilon.
//
// Save semantics: ATOMIC, exactly the ConfigStore approach — the JSON is
// written to a pid-suffixed temp file in the target's own directory, then
// renamed over the target, so a crash, full disk, or locked target leaves
// either the complete old file or the complete new one, never a hybrid.
// Parent directories are created on demand.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cascade::core {

struct Bookmark {
    std::string name;
    double freqHz = 0;
    std::string mode = "WFM";
    double bandwidthHz = 150000;
};

class FreqManager {
public:
    // %APPDATA%/sdr-minus-plus/bookmarks.json on Windows (same fallback
    // rules as ConfigStore::defaultPath — "." when APPDATA is unset);
    // $XDG_CONFIG_HOME or ~/.config equivalent elsewhere. The directory is
    // not created here — save() creates it when there is something to write.
    static std::string defaultPath();

    // Replaces the current list with the file's content (semantics in the
    // header comment above). Always leaves the list in a valid, sorted
    // state — empty on every failure path, never a stale mix.
    bool load(const std::string& path, std::string& error);

    // Atomic write (temp file + rename over target). On failure returns
    // false with `error` set and the previous target content — if any —
    // intact; the temp file is cleaned up.
    bool save(const std::string& path, std::string& error) const;

    // Always sorted by freqHz ascending. Equal frequencies keep their
    // insertion order (stable), so adding a duplicate frequency never
    // reshuffles what the user already sees.
    const std::vector<Bookmark>& list() const { return list_; }

    // Inserts keeping the sort invariant and returns the index the bookmark
    // landed at. A name that is already in use is deduplicated by appending
    // " (2)", " (3)", ... — first unused counter wins — because the GUI
    // addresses bookmarks by name and silent duplicates would make "rename"
    // and "delete" ambiguous. Values are stored verbatim: sanitization is a
    // load-time repair for hand-edited files; in-app callers own their input.
    int add(Bookmark b);

    // Both are bounds-checked: an out-of-range index returns false and
    // changes nothing. updateAt replaces the entry then re-sorts (the edit
    // may have moved its frequency); the replacement's name is taken
    // verbatim — deduping an in-place edit against the list would collide
    // the entry with its own old name, so renames are the caller's problem.
    bool removeAt(std::size_t index);
    bool updateAt(std::size_t index, const Bookmark& b);

private:
    // Sorted insert helper: keeps list_ ordered and returns the landing
    // index. upper_bound (not lower_bound) so equal frequencies append
    // after their peers — stable with respect to insertion order.
    std::size_t insertSorted(Bookmark b);

    std::vector<Bookmark> list_;
};

}  // namespace cascade::core
