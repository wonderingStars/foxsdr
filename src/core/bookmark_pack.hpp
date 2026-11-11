// SHARED PRESET AND BOOKMARK PACKS: the file format and the import / export, nothing more.
//
// A pack is one JSON file a user can hand to another user: a set of bookmarks (name, frequency, mode, bandwidth, group, favourite flag, and the note and
// tags of core/bookmark_notes.hpp) and, optionally, favourites that save the whole state (core/favourite_state.hpp). FREQUENCY-DATABASE DOWNLOADS ARE NOT
// BUILT: that is a service outside the program, and this header never opens a connection or a file.
//
//   { "format": "foxsdr-pack", "version": 1, "name": "...", "description": "...",
//     "bookmarks": [ {"name":..,"freqHz":..,"mode":..,"bandwidthHz":..,"group":..,"favourite":..,"note":..,"tags":[..]} ],
//     "favourites": [ <favourite_state.hpp> ] }
//
// WHAT NEVER GOES IN A PACK: ids, heard-seconds, the scan tick (those are this session's or this user's), a path, a callsign, an address, a position, an
// author. The pack's name and description are typed by the user at export; nothing else is added.
//
// IMPORT IS STRICT AND ALL OR NOTHING: format string and version 1 exactly; every bookmark with a name (1..64 bytes), a finite frequency 0 < f <= 1e13, a
// mode name, a bandwidth 50..1e7, a group (<= 64 bytes, may be empty), a favourite flag, a note (<= 2000 bytes) and tags; at most 5000 bookmarks and 100
// favourites; the file at most 4 MiB. planImport() then says what to do with the entries against the user's list: an entry within dupTolHz of an
// existing bookmark of the SAME MODE is a duplicate and is left out (never overwrites); the rest are returned in file order. The caller adds them.
//
// THREADS: pure functions. No file access.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/bookmark.hpp"
#include "core/bookmark_notes.hpp"
#include "core/favourite_state.hpp"
#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

struct PackBookmark {
    Bookmark bookmark;      // name, freqHz, mode, bandwidthHz, group, favourite; the rest at defaults
    BookmarkNote note;
};

struct Pack {
    std::string name;
    std::string description;
    std::vector<PackBookmark> bookmarks;
    std::vector<FavouriteState> favourites;
};

inline constexpr std::size_t kPackMaxBytes = 4u * 1024u * 1024u, kPackMaxBookmarks = 5000, kPackMaxFavourites = 100;

inline bool validPackBookmark(const PackBookmark& p) {
    const Bookmark& b = p.bookmark;
    return !b.name.empty() && validUserText(b.name, 64) && std::isfinite(b.freqHz) && b.freqHz > 0.0 && b.freqHz <= 1e13 && validModeName(b.mode) &&
           std::isfinite(b.bandwidthHz) && b.bandwidthHz >= 50.0 && b.bandwidthHz <= 1e7 && validUserText(b.group, 64) &&
           validUserText(p.note.note, BookmarkNotes::kMaxNoteBytes, true) && p.note.tags.size() <= BookmarkNotes::kMaxTags;
}

// The pack as text. `notes` may be null. Bookmarks that would not import (invalid) are left out and counted in *skipped.
inline std::string exportPack(const std::string& name, const std::string& description, const std::vector<Bookmark>& list, const BookmarkNotes* notes,
                              const std::vector<FavouriteState>& favourites, int* skipped = nullptr) {
    nlohmann::json bm = nlohmann::json::array();
    int bad = 0;
    for (const Bookmark& b : list) {
        PackBookmark p;
        p.bookmark.name = b.name; p.bookmark.freqHz = b.freqHz; p.bookmark.mode = b.mode; p.bookmark.bandwidthHz = b.bandwidthHz;
        p.bookmark.group = b.group; p.bookmark.favourite = b.favourite;
        if (notes != nullptr) {
            if (const BookmarkNote* n = notes->find(b.freqHz, b.mode)) { p.note = *n; }
        }
        if (!validPackBookmark(p) || bm.size() >= kPackMaxBookmarks) { ++bad; continue; }
        bm.push_back({{"name", p.bookmark.name}, {"freqHz", p.bookmark.freqHz}, {"mode", p.bookmark.mode}, {"bandwidthHz", p.bookmark.bandwidthHz},
                      {"group", p.bookmark.group}, {"favourite", p.bookmark.favourite}, {"note", p.note.note}, {"tags", p.note.tags}});
    }
    nlohmann::json fv = nlohmann::json::array();
    for (const FavouriteState& f : favourites) {
        if (!validFavourite(f) || fv.size() >= kPackMaxFavourites) { ++bad; continue; }
        fv.push_back(favouriteToJson(f));
    }
    if (skipped != nullptr) { *skipped = bad; }
    const nlohmann::json j = {{"format", "foxsdr-pack"}, {"version", 1}, {"name", validUserText(name, 64) ? name : std::string()},
                              {"description", validUserText(description, 500, true) ? description : std::string()}, {"bookmarks", bm}, {"favourites", fv}};
    return j.dump(2);
}

inline bool importPack(const std::string& text, Pack& out, std::string& err) {
    err.clear();
    if (text.size() > kPackMaxBytes) { err = "the pack is larger than 4 MiB"; return false; }
    std::string body = text;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF) { body.erase(0, 3); }
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "the pack is not a JSON object"; return false; }
    if (!j.contains("format") || !j["format"].is_string() || j["format"].get<std::string>() != "foxsdr-pack") { err = "not a FoxSDR pack"; return false; }
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { err = "unknown pack version"; return false; }
    if (!j.contains("name") || !j["name"].is_string() || !j.contains("description") || !j["description"].is_string() || !j.contains("bookmarks") ||
        !j["bookmarks"].is_array() || !j.contains("favourites") || !j["favourites"].is_array()) {
        err = "a pack field is missing or of the wrong type";
        return false;
    }
    Pack p;
    p.name = j["name"].get<std::string>();
    p.description = j["description"].get<std::string>();
    if (!validUserText(p.name, 64) || !validUserText(p.description, 500, true)) { err = "the pack name or description is not valid"; return false; }
    if (j["bookmarks"].size() > kPackMaxBookmarks || j["favourites"].size() > kPackMaxFavourites) { err = "too many entries"; return false; }
    for (const auto& e : j["bookmarks"]) {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string() || !e.contains("freqHz") || !e["freqHz"].is_number() || !e.contains("mode") ||
            !e["mode"].is_string() || !e.contains("bandwidthHz") || !e["bandwidthHz"].is_number() || !e.contains("group") || !e["group"].is_string() ||
            !e.contains("favourite") || !e["favourite"].is_boolean() || !e.contains("note") || !e["note"].is_string() || !e.contains("tags") || !e["tags"].is_array()) {
            err = "a bookmark is damaged";
            return false;
        }
        PackBookmark b;
        b.bookmark.name = e["name"].get<std::string>();
        b.bookmark.freqHz = e["freqHz"].get<double>();
        b.bookmark.mode = e["mode"].get<std::string>();
        b.bookmark.bandwidthHz = e["bandwidthHz"].get<double>();
        b.bookmark.group = e["group"].get<std::string>();
        b.bookmark.favourite = e["favourite"].get<bool>();
        b.note.note = e["note"].get<std::string>();
        if (e["tags"].size() > BookmarkNotes::kMaxTags) { err = "a bookmark has more than 16 tags"; return false; }
        for (const auto& t : e["tags"]) {
            std::string n;
            if (!t.is_string() || !BookmarkNotes::normaliseTag(t.get<std::string>(), n)) { err = "a tag is not valid"; return false; }
            b.note.tags.push_back(n);
        }
        if (!validPackBookmark(b)) { err = "a bookmark value is out of range"; return false; }
        p.bookmarks.push_back(std::move(b));
    }
    for (const auto& e : j["favourites"]) {
        FavouriteState f;
        if (!favouriteFromJson(e, f, &err)) { return false; }
        p.favourites.push_back(std::move(f));
    }
    out = std::move(p);
    return true;
}

struct PackImportPlan {
    std::vector<PackBookmark> toAdd;
    int duplicates = 0;
};

inline PackImportPlan planImport(const Pack& pack, const std::vector<Bookmark>& existing, double dupTolHz = 1.0) {
    PackImportPlan plan;
    for (const PackBookmark& p : pack.bookmarks) {
        bool dup = false;
        for (const Bookmark& b : existing) {
            if (std::fabs(b.freqHz - p.bookmark.freqHz) <= dupTolHz && b.mode == p.bookmark.mode) { dup = true; break; }
        }
        for (const PackBookmark& q : plan.toAdd) {
            if (std::fabs(q.bookmark.freqHz - p.bookmark.freqHz) <= dupTolHz && q.bookmark.mode == p.bookmark.mode) { dup = true; break; }
        }
        if (dup) { ++plan.duplicates; } else { plan.toAdd.push_back(p); }
    }
    return plan;
}

}  // namespace cascade::core::tuning
