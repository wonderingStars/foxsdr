// A NOTE and TAGS for a bookmark, kept beside the bookmark list and not inside it.
//
// core/bookmark.hpp (the entry the engine API and every window share) is not changed: a new field there moves the engine API and the saved list. This
// is a sidecar: one record per bookmark, found by FREQUENCY (rounded to a hertz) and MODE, the pair that makes a bookmark the same bookmark when it is
// renamed or re-grouped. Moving a bookmark to another frequency or mode is rekey().
//
// A record holds a note (free text, at most 2000 bytes, line breaks and tabs allowed) and up to 16 tags (each at most 32 bytes; trimmed, ASCII
// letters folded to lower case, no comma, no control character, duplicates removed, order kept). A record with an empty note and no tags is not kept:
// setting one removes the record. At most 20000 records.
//
// Search: byTag() (exact, case-folded) and search() (case-folded ASCII substring of the note or any tag).
//
// SAVED as versioned JSON, {"version":1,"notes":[{"hz":..,"mode":"..","note":"..","tags":[..]}]}; any other version, a damaged record, a duplicate
// (hz, mode) or an invalid value refuses the WHOLE file and changes nothing. Nothing but what the user typed and the bookmark's own key is stored.
//
// THREADS: one thread. No file access here (the caller saves the text, as FreqManager does).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tuning_text.hpp"

namespace cascade::core::tuning {

struct BookmarkNote {
    std::string note;
    std::vector<std::string> tags;
};

class BookmarkNotes {
public:
    static constexpr std::size_t kMaxEntries = 20000, kMaxNoteBytes = 2000, kMaxTags = 16, kMaxTagBytes = 32;
    using Key = std::pair<std::int64_t, std::string>;  // (hertz, mode)

    static bool makeKey(double hz, const std::string& mode, Key& out) {
        if (!std::isfinite(hz) || hz <= 0.0 || hz > 1e13 || !validModeName(mode)) { return false; }
        out = {static_cast<std::int64_t>(std::llround(hz)), mode};
        return true;
    }

    // Trimmed, ASCII lower-case; false for an invalid tag.
    static bool normaliseTag(const std::string& in, std::string& out) {
        std::size_t a = 0, b = in.size();
        while (a < b && (in[a] == ' ' || in[a] == '\t')) { ++a; }
        while (b > a && (in[b - 1] == ' ' || in[b - 1] == '\t')) { --b; }
        out = in.substr(a, b - a);
        if (out.empty() || out.size() > kMaxTagBytes || out.find(',') != std::string::npos || !validUserText(out, kMaxTagBytes)) { return false; }
        for (char& c : out) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        return true;
    }

    // Sets (or, for an empty note and no tags, removes) the record. false with the reason, changing nothing, for an invalid key, note or tag.
    bool set(double hz, const std::string& mode, const std::string& note, const std::vector<std::string>& tags, std::string* err = nullptr) {
        Key k;
        if (!makeKey(hz, mode, k)) { return fail(err, "the frequency or mode is not valid"); }
        if (!validUserText(note, kMaxNoteBytes, true)) { return fail(err, "the note is too long or holds a control character"); }
        BookmarkNote rec;
        rec.note = note;
        for (const std::string& t : tags) {
            if (t.find_first_not_of(" \t") == std::string::npos) { continue; }   // a blank tag is dropped
            std::string n;
            if (!normaliseTag(t, n)) { return fail(err, "a tag is not valid"); }
            if (std::find(rec.tags.begin(), rec.tags.end(), n) == rec.tags.end()) { rec.tags.push_back(n); }
        }
        if (rec.tags.size() > kMaxTags) { return fail(err, "more than 16 tags"); }
        if (rec.note.empty() && rec.tags.empty()) { m_.erase(k); return true; }
        if (m_.find(k) == m_.end() && m_.size() >= kMaxEntries) { return fail(err, "too many notes"); }
        m_[k] = std::move(rec);
        return true;
    }
    const BookmarkNote* find(double hz, const std::string& mode) const {
        Key k;
        if (!makeKey(hz, mode, k)) { return nullptr; }
        const auto it = m_.find(k);
        return it == m_.end() ? nullptr : &it->second;
    }
    bool remove(double hz, const std::string& mode) {
        Key k;
        return makeKey(hz, mode, k) && m_.erase(k) > 0;
    }
    // The bookmark moved: its record follows. false when there is none, or the new key is invalid or already has a record.
    bool rekey(double oldHz, const std::string& oldMode, double newHz, const std::string& newMode) {
        Key a, b;
        if (!makeKey(oldHz, oldMode, a) || !makeKey(newHz, newMode, b)) { return false; }
        const auto it = m_.find(a);
        if (it == m_.end()) { return false; }
        if (a == b) { return true; }
        if (m_.count(b) != 0) { return false; }
        m_[b] = std::move(it->second);
        m_.erase(a);
        return true;
    }
    std::size_t size() const { return m_.size(); }
    void clear() { m_.clear(); }

    std::vector<Key> byTag(const std::string& tag) const {
        std::string n;
        std::vector<Key> out;
        if (!normaliseTag(tag, n)) { return out; }
        for (const auto& e : m_) {
            if (std::find(e.second.tags.begin(), e.second.tags.end(), n) != e.second.tags.end()) { out.push_back(e.first); }
        }
        return out;
    }
    std::vector<Key> search(const std::string& text) const {
        std::vector<Key> out;
        if (text.empty()) { return out; }
        const std::string needle = lower(text);
        for (const auto& e : m_) {
            bool hit = lower(e.second.note).find(needle) != std::string::npos;
            for (const std::string& t : e.second.tags) { hit = hit || t.find(needle) != std::string::npos; }
            if (hit) { out.push_back(e.first); }
        }
        return out;
    }

    nlohmann::json toJson() const {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : m_) {
            arr.push_back({{"hz", e.first.first}, {"mode", e.first.second}, {"note", e.second.note}, {"tags", e.second.tags}});
        }
        return {{"version", 1}, {"notes", arr}};
    }
    // Strict: the whole file or nothing.
    bool fromJson(const nlohmann::json& j, std::string* err = nullptr) {
        if (!j.is_object()) { return fail(err, "not an object"); }
        if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<long long>() != 1) { return fail(err, "unknown version"); }
        if (!j.contains("notes") || !j["notes"].is_array()) { return fail(err, "notes missing"); }
        if (j["notes"].size() > kMaxEntries) { return fail(err, "too many notes"); }
        std::map<Key, BookmarkNote> next;
        for (const auto& e : j["notes"]) {
            if (!e.is_object() || !e.contains("hz") || !e["hz"].is_number_integer() || !e.contains("mode") || !e["mode"].is_string() || !e.contains("note") ||
                !e["note"].is_string() || !e.contains("tags") || !e["tags"].is_array()) {
                return fail(err, "a record is damaged");
            }
            Key k;
            if (!makeKey(static_cast<double>(e["hz"].get<std::int64_t>()), e["mode"].get<std::string>(), k)) { return fail(err, "a record key is not valid"); }
            BookmarkNote rec;
            rec.note = e["note"].get<std::string>();
            if (!validUserText(rec.note, kMaxNoteBytes, true)) { return fail(err, "a note is not valid"); }
            if (e["tags"].size() > kMaxTags) { return fail(err, "more than 16 tags"); }
            for (const auto& t : e["tags"]) {
                std::string n;
                if (!t.is_string() || !normaliseTag(t.get<std::string>(), n) || n != t.get<std::string>()) { return fail(err, "a tag is not valid"); }
                if (std::find(rec.tags.begin(), rec.tags.end(), n) != rec.tags.end()) { return fail(err, "a tag is repeated"); }
                rec.tags.push_back(n);
            }
            if (rec.note.empty() && rec.tags.empty()) { return fail(err, "an empty record"); }
            if (!next.emplace(k, std::move(rec)).second) { return fail(err, "a key is repeated"); }
        }
        m_ = std::move(next);
        return true;
    }

private:
    static std::string lower(std::string s) {
        for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        return s;
    }
    static bool fail(std::string* err, const char* m) { if (err != nullptr) { *err = m; } return false; }
    std::map<Key, BookmarkNote> m_;
};

}  // namespace cascade::core::tuning
