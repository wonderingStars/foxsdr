// web_bookmarks.hpp - the browser's own bookmark sets (feat/webmodels).
//
// WHAT EXISTED. The web page lists the host's station bookmarks and can add one by NAME only (control request `bookmarkAdd`), taking the
// receiver's current tuning. It has no group, no mode choice, no tags, no search, and every viewer shares the one list.
//
// WHAT THIS MODEL IS. A pure, thread-safe store of PERSONAL bookmark sets, one per viewer, each viewer named only by an OPAQUE TOKEN the caller
// hands in (the web session's cookie value, or any string of 8..128 characters from [A-Za-z0-9_-]). A bookmark here has a name, a frequency, a
// mode, a bandwidth, a group and tags; sets can be searched and exported to / imported from JSON (the page's "save to my device" and "restore"
// buttons, so a set can live in the viewer's own browser storage and outlast the server).
//
// IDENTIFIERS NEVER LEAVE MEMORY. The token is the key of an in-memory map and nothing else: this file opens no file, prints nothing, and
// stores nothing outside the object (tests/test_web_bookmarks.cpp scans the source for stdout/stderr/file use). The exported JSON carries the
// bookmarks and never the token. forgetViewer() drops a set (sign-out); a server restart drops them all.
//
// CAPS (all constants below, all tested at the boundary): name, group, tag lengths, tags per bookmark, bookmarks per viewer, viewers, bytes of
// an import, bookmarks in an import. An over-long text field is REFUSED, never silently cut (a cut name would no longer be the one the viewer
// typed).
//
// WHAT THIS DOES NOT DO. It does not read or write the host's shared station list (core/freq_manager owns that), and it does not tune anything:
// the page asks the host to tune to a bookmark's frequency and mode through the existing control request. A hand-off in
// docs/webmodels-handoff.md says how the page would use it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::net {

inline constexpr std::size_t kMaxBookmarkName = 64;
inline constexpr std::size_t kMaxBookmarkGroup = 32;
inline constexpr std::size_t kMaxBookmarkTag = 24;
inline constexpr std::size_t kMaxBookmarkTags = 8;
inline constexpr std::size_t kMaxBookmarksPerViewer = 500;
inline constexpr std::size_t kMaxBookmarkViewers = 64;
inline constexpr std::size_t kMaxBookmarkImportBytes = 256 * 1024;
inline constexpr std::size_t kMaxSearchQuery = 64;
inline constexpr double kMinBookmarkHz = 1.0;
inline constexpr double kMaxBookmarkHz = 30.0e9;
inline constexpr double kMinBookmarkBandwidthHz = 50.0;
inline constexpr double kMaxBookmarkBandwidthHz = 20.0e6;

struct WebBookmark {
    std::uint64_t id = 0;  // assigned by the book, never reused within a book
    std::string name;
    double freqHz = 0.0;
    std::string mode = "NFM";  // one of the engine's eight names, upper case
    double bandwidthHz = 12500.0;
    std::string group;  // "" = ungrouped
    std::vector<std::string> tags;  // lower case, unique, in the order given
};

enum class BookmarkResult {
    Ok,
    BadToken,        // not 8..128 of [A-Za-z0-9_-]
    TooManyViewers,  // a new viewer would exceed kMaxBookmarkViewers
    TooManyBookmarks,
    BadName,         // empty, over the cap, a control character, or invalid UTF-8
    BadGroup,
    BadTag,
    TooManyTags,
    BadFrequency,    // not finite or outside [kMinBookmarkHz, kMaxBookmarkHz]
    BadMode,
    BadBandwidth,
    Duplicate,       // the same name AND frequency AND mode already in this viewer's set
    NotFound,
    BadJson,         // not JSON, or the wrong shape
    TooLarge,        // an import over kMaxBookmarkImportBytes
};

const char* bookmarkResultText(BookmarkResult r);

// Validation of one text field: valid UTF-8 (no overlong forms, no surrogates), no control characters (below 0x20, 0x7F), not blank, within `maxBytes`.
bool validBookmarkText(const std::string& s, std::size_t maxBytes);

// "nfm" -> "NFM"; false when not one of NFM WFM AM DSB USB CW LSB RAW.
bool normaliseBookmarkMode(const std::string& in, std::string& out);

// A tag: 1..kMaxBookmarkTag characters of [a-z0-9_-] after lower-casing.
bool normaliseBookmarkTag(const std::string& in, std::string& out);

bool validViewerToken(const std::string& token);

struct BookmarkQuery {
    std::string text;   // case-insensitive substring of name, group or a tag; empty = everything
    std::string group;  // exact (case-insensitive) group; "*" is not special; empty = any
    std::string mode;   // exact mode (any case); empty = any
    std::string tag;    // exact tag (any case); empty = any
    double minHz = 0.0;   // inclusive; 0 = no lower bound
    double maxHz = 0.0;   // inclusive; 0 = no upper bound
};

struct BookmarkImportResult {
    BookmarkResult result = BookmarkResult::Ok;
    std::size_t imported = 0;
    std::size_t skipped = 0;  // entries that failed validation (the rest of the file still counts)
};

class WebBookmarkBook {
public:
    // Adds to the viewer's set. `bm.id` is ignored and replaced. Validation order is the enum's. On Ok `idOut` holds the new id.
    BookmarkResult add(const std::string& viewerToken, WebBookmark bm, std::uint64_t* idOut = nullptr);
    BookmarkResult remove(const std::string& viewerToken, std::uint64_t id);
    // Replaces fields of an existing one (same validation).
    BookmarkResult update(const std::string& viewerToken, std::uint64_t id, WebBookmark bm);

    // The viewer's whole set, ordered by (group, frequency, name, id). An unknown viewer has an empty set (and is not created).
    std::vector<WebBookmark> list(const std::string& viewerToken) const;
    // The matching subset in the same order. An over-long query matches nothing.
    std::vector<WebBookmark> search(const std::string& viewerToken, const BookmarkQuery& q) const;
    // The distinct groups, sorted, case-insensitively unique (the first spelling seen wins).
    std::vector<std::string> groups(const std::string& viewerToken) const;

    // JSON: {"version":1,"bookmarks":[{"name":..,"freqHz":..,"mode":..,"bandwidthHz":..,"group":..,"tags":[..]}]}; the token is never in it.
    std::string exportJson(const std::string& viewerToken) const;
    // Accepts that object or a bare array. Each entry is validated like add(); bad entries are counted and skipped; entries beyond the
    // per-viewer cap are skipped too. Nothing is thrown. `replace` clears the set first (only when the JSON itself parsed).
    BookmarkImportResult importJson(const std::string& viewerToken, const std::string& json, bool replace);

    void forgetViewer(const std::string& viewerToken);
    void clear();
    std::size_t viewerCount() const;
    std::size_t size(const std::string& viewerToken) const;

private:
    struct Set {
        std::vector<WebBookmark> items;
    };
    static BookmarkResult validate(WebBookmark& bm);
    BookmarkResult addLocked(const std::string& token, WebBookmark bm, std::uint64_t* idOut);

    mutable std::mutex mu_;
    std::map<std::string, Set> sets_;
    std::uint64_t nextId_ = 1;
};

}  // namespace cascade::net
