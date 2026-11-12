// Your own screenshots, clips and notes attached to a signal record (DEPTH-GAPS Part B, the "Artemis" row).
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). A "signal record" is a folder (the user's own: a recording's folder, an analyser
// project's folder, a decode-database export); AttachmentStore keeps a manifest `attachments.json` in it and copies attached files into it.
//
// WHAT CAN BE ATTACHED. Screenshots (PNG or JPEG, up to 20 MiB), clips (WAV, FLAC, OGG or MP3, up to 200 MiB) and notes (text, up to 8000 bytes, kept in the
// manifest itself). Files are checked by their first bytes, not by their name: a program renamed to .png is refused. At most 500 items and 1 GiB per
// record (limits in Limits, all changeable). Each attached file is COPIED into the record folder as `<id>-<clean name>.<ext>` (the user's original is never
// touched, moved or linked), hashed (SHA-256) while it is copied, and the hash and size go in the manifest so verify() can tell a file that was changed or
// removed later. The stored name is built here from the clean file stem (forbidden characters become `_`, at most 40 bytes); nothing the user typed is a path.
//
// SAFETY. The record folder must already exist; nothing is created outside it; remove() deletes only a file whose recorded name is a plain stored name inside
// the folder. The manifest is versioned (1), at most 4 MiB, written atomically (temp file, rename) and refused whole when anything in it is out of range or
// when a stored name has a separator or a dot-dot.
//
// PRIVACY. Titles and notes are user text kept in the manifest only. This header never prints or logs; error messages never repeat a title, a note or a path.
//
// THREADS: one owner thread (the screen's control thread); not thread-safe.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/analyser_project.hpp"

namespace cascade::core::attach {

enum class Kind : int { Screenshot = 0, Clip = 1, Note = 2 };

struct Limits {
    std::uint64_t maxScreenshotBytes = 20ull * 1024 * 1024;
    std::uint64_t maxClipBytes = 200ull * 1024 * 1024;
    std::uint64_t maxTotalBytes = 1024ull * 1024 * 1024;
    std::size_t maxItems = 500;
    std::size_t maxTitle = 64;
    std::size_t maxNote = 8000;
};

struct Item {
    std::uint32_t id = 0;
    Kind kind = Kind::Note;
    std::string storedName;              // a plain file name inside the record folder; empty for a note
    std::string title;
    std::string text;                    // notes only
    std::uint64_t sizeBytes = 0;
    std::string sha256;                  // files only
    std::int64_t addedMs = 0;
};

enum class FileState { Ok, Missing, SizeChanged, HashChanged, ReadError };

struct VerifyEntry { std::uint32_t id; FileState state; };

namespace detail {

using cascade::core::analyser::detail::cleanText;

inline std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return e;
}

// What the first bytes say the file is: "png", "jpg", "wav", "flac", "ogg", "mp3" or "".
inline std::string sniff(const unsigned char* b, std::size_t n) {
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' && b[4] == 0x0D && b[5] == 0x0A && b[6] == 0x1A && b[7] == 0x0A) { return "png"; }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) { return "jpg"; }
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WAVE", 4) == 0) { return "wav"; }
    if (n >= 4 && std::memcmp(b, "fLaC", 4) == 0) { return "flac"; }
    if (n >= 4 && std::memcmp(b, "OggS", 4) == 0) { return "ogg"; }
    if (n >= 3 && std::memcmp(b, "ID3", 3) == 0) { return "mp3"; }
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) { return "mp3"; }
    return "";
}

inline std::string cleanStem(const std::string& stem) {
    std::string s;
    for (char c : stem) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || std::string("<>:\"|?*/\\").find(c) != std::string::npos) { s += '_'; } else { s += c; }
        if (s.size() >= 40) { break; }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) { s.pop_back(); }
    while (!s.empty() && s.front() == ' ') { s.erase(s.begin()); }
    // keep only well-formed UTF-8: a cut inside a sequence, or bytes that were never valid, become '_'
    if (!cascade::core::analyser::detail::validUtf8(s)) { for (char& c : s) { if (static_cast<unsigned char>(c) >= 0x80) { c = '_'; } } }
    if (s.empty() || s == "." || s == "..") { s = "file"; }
    return s;
}

inline bool plainName(const std::string& n) {
    if (n.empty() || n.size() > 80 || n == "." || n == "..") { return false; }
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || std::string("<>:\"|?*/\\").find(c) != std::string::npos) { return false; }
    }
    return n.back() != '.' && n.back() != ' ';
}

}  // namespace detail

class AttachmentStore {
public:
    explicit AttachmentStore(const Limits& limits = Limits()) : lim_(limits) {}

    // The record folder must exist. A missing manifest is an empty store; an unreadable or invalid manifest is refused (nothing is overwritten).
    bool open(const std::filesystem::path& folder, std::string* err) {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (folder.empty() || !fs::is_directory(folder, ec) || ec) { return fail(err, "the record folder does not exist"); }
        folder_ = folder; items_.clear(); nextId_ = 1; opened_ = false;
        const fs::path man = folder / "attachments.json";
        if (!fs::exists(man, ec)) { opened_ = true; return true; }
        const std::uintmax_t size = fs::file_size(man, ec);
        if (ec || size > 4u * 1024u * 1024u) { return fail(err, "the attachments manifest cannot be read"); }
        std::ifstream f(man, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        f.read(text.data(), static_cast<std::streamsize>(size));
        if (!f || static_cast<std::uintmax_t>(f.gcount()) != size) { return fail(err, "the attachments manifest cannot be read"); }
        if (!parseManifest(text, err)) { items_.clear(); nextId_ = 1; return false; }
        opened_ = true;
        return true;
    }

    const std::vector<Item>& items() const { return items_; }
    const Limits& limits() const { return lim_; }

    std::uint64_t totalBytes() const { std::uint64_t t = 0; for (const Item& i : items_) { t += i.sizeBytes; } return t; }

    // Copies `source` into the record folder. Returns the new id, or 0 with `err`.
    std::uint32_t addFile(const std::filesystem::path& source, Kind kind, const std::string& title, std::int64_t nowMs, std::string* err) {
        namespace fs = std::filesystem;
        if (!opened_) { fail(err, "no record folder is open"); return 0; }
        if (kind == Kind::Note) { fail(err, "a note has no file; use addNote"); return 0; }
        if (!detail::cleanText(title, lim_.maxTitle, false)) { fail(err, "the title is too long or has invalid characters"); return 0; }
        if (items_.size() >= lim_.maxItems) { fail(err, "too many attachments on this record"); return 0; }
        std::error_code ec;
        if (!fs::is_regular_file(source, ec) || ec) { fail(err, "the file to attach does not exist or is not a file"); return 0; }
        const std::uint64_t size = fs::file_size(source, ec);
        if (ec || size == 0) { fail(err, "the file to attach is empty or unreadable"); return 0; }
        const std::uint64_t cap = kind == Kind::Screenshot ? lim_.maxScreenshotBytes : lim_.maxClipBytes;
        if (size > cap) { fail(err, kind == Kind::Screenshot ? "the screenshot is larger than its limit" : "the clip is larger than its limit"); return 0; }
        if (totalBytes() + size > lim_.maxTotalBytes) { fail(err, "this record's attachments would pass their total size limit"); return 0; }
        std::ifstream in(source, std::ios::binary);
        if (!in) { fail(err, "the file to attach cannot be opened"); return 0; }
        unsigned char head[16] = {};
        in.read(reinterpret_cast<char*>(head), sizeof head);
        const std::string type = detail::sniff(head, static_cast<std::size_t>(in.gcount()));
        const bool okType = kind == Kind::Screenshot ? (type == "png" || type == "jpg") : (type == "wav" || type == "flac" || type == "ogg" || type == "mp3");
        if (!okType) { fail(err, kind == Kind::Screenshot ? "the file is not a PNG or JPEG picture" : "the file is not a WAV, FLAC, OGG or MP3 clip"); return 0; }
        in.clear(); in.seekg(0);
        const std::string stored = std::to_string(nextId_) + "-" + detail::cleanStem(source.stem().string()) + "." + type;
        if (!detail::plainName(stored)) { fail(err, "no safe name could be made for the file"); return 0; }
        const fs::path dest = folder_ / stored;
        if (fs::exists(dest, ec)) { fail(err, "the stored name already exists"); return 0; }
        cascade::core::analyser::Sha256 sha;
        std::uint64_t copied = 0;
        {
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (!out) { fail(err, "cannot write into the record folder"); return 0; }
            std::vector<char> buf(1u << 20);
            while (in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const std::streamsize got = in.gcount();
                if (got <= 0) { break; }
                if (copied + static_cast<std::uint64_t>(got) > cap) { copied = cap + 1; break; }     // the file grew while it was copied
                out.write(buf.data(), got);
                sha.update(buf.data(), static_cast<std::size_t>(got));
                copied += static_cast<std::uint64_t>(got);
            }
            out.flush();
            if (!out || copied != size) { out.close(); fs::remove(dest, ec); fail(err, "the file changed or could not be copied"); return 0; }
        }
        Item it;
        it.id = nextId_; it.kind = kind; it.storedName = stored; it.title = title; it.sizeBytes = size; it.sha256 = sha.finishHex(); it.addedMs = nowMs;
        items_.push_back(it);
        ++nextId_;
        if (!save(err)) { items_.pop_back(); --nextId_; fs::remove(dest, ec); return 0; }
        return it.id;
    }

    std::uint32_t addNote(const std::string& title, const std::string& text, std::int64_t nowMs, std::string* err) {
        if (!opened_) { fail(err, "no record folder is open"); return 0; }
        if (!detail::cleanText(title, lim_.maxTitle, false)) { fail(err, "the title is too long or has invalid characters"); return 0; }
        if (text.empty() || !detail::cleanText(text, lim_.maxNote, true)) { fail(err, "the note is empty, too long or has invalid characters"); return 0; }
        if (items_.size() >= lim_.maxItems) { fail(err, "too many attachments on this record"); return 0; }
        Item it;
        it.id = nextId_; it.kind = Kind::Note; it.title = title; it.text = text; it.sizeBytes = text.size(); it.addedMs = nowMs;
        items_.push_back(it);
        ++nextId_;
        if (!save(err)) { items_.pop_back(); --nextId_; return 0; }
        return it.id;
    }

    // Changes the title and, for a note, the text.
    bool edit(std::uint32_t id, const std::string& title, const std::string* text, std::string* err) {
        Item* it = find(id);
        if (it == nullptr) { return fail(err, "no such attachment"); }
        if (!detail::cleanText(title, lim_.maxTitle, false)) { return fail(err, "the title is too long or has invalid characters"); }
        if (text != nullptr && (it->kind != Kind::Note || text->empty() || !detail::cleanText(*text, lim_.maxNote, true))) { return fail(err, "the note text is not valid for this attachment"); }
        const Item old = *it;
        it->title = title;
        if (text != nullptr) { it->text = *text; it->sizeBytes = text->size(); }
        if (!save(err)) { *it = old; return false; }
        return true;
    }

    // Removes the item and, for a file, the stored copy inside the record folder.
    bool remove(std::uint32_t id, std::string* err) {
        const auto pos = std::find_if(items_.begin(), items_.end(), [&](const Item& i) { return i.id == id; });
        if (pos == items_.end()) { return fail(err, "no such attachment"); }
        const Item old = *pos;
        items_.erase(pos);
        if (!save(err)) { items_.push_back(old); std::sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.id < b.id; }); return false; }
        if (!old.storedName.empty() && detail::plainName(old.storedName)) { std::error_code ec; std::filesystem::remove(folder_ / old.storedName, ec); }
        return true;
    }

    // Re-hashes every stored file.
    std::vector<VerifyEntry> verify() const {
        std::vector<VerifyEntry> out;
        for (const Item& i : items_) {
            if (i.kind == Kind::Note) { continue; }
            std::error_code ec;
            const std::filesystem::path p = folder_ / i.storedName;
            if (!std::filesystem::is_regular_file(p, ec) || ec) { out.push_back({i.id, FileState::Missing}); continue; }
            const std::uintmax_t size = std::filesystem::file_size(p, ec);
            if (ec) { out.push_back({i.id, FileState::ReadError}); continue; }
            if (size != i.sizeBytes) { out.push_back({i.id, FileState::SizeChanged}); continue; }
            std::string hex, e;
            if (!cascade::core::analyser::sha256File(p, &hex, nullptr, &e)) { out.push_back({i.id, FileState::ReadError}); continue; }
            out.push_back({i.id, hex == i.sha256 ? FileState::Ok : FileState::HashChanged});
        }
        return out;
    }

    std::string manifestText() const { return makeManifest(); }

private:
    static bool fail(std::string* err, const char* m) { if (err != nullptr) { *err = m; } return false; }

    Item* find(std::uint32_t id) { for (Item& i : items_) { if (i.id == id) { return &i; } } return nullptr; }

    std::string makeManifest() const {
        using nlohmann::json;
        json j;
        j["format"] = "foxsdr-attachments";
        j["version"] = 1;
        j["nextId"] = nextId_;
        json arr = json::array();
        for (const Item& i : items_) {
            json o;
            o["id"] = i.id;
            o["kind"] = i.kind == Kind::Screenshot ? "screenshot" : i.kind == Kind::Clip ? "clip" : "note";
            o["title"] = i.title;
            o["sizeBytes"] = i.sizeBytes;
            o["addedMs"] = i.addedMs;
            if (i.kind == Kind::Note) { o["text"] = i.text; } else { o["file"] = i.storedName; o["sha256"] = i.sha256; }
            arr.push_back(o);
        }
        j["items"] = arr;
        return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    bool save(std::string* err) {
        namespace fs = std::filesystem;
        const std::string text = makeManifest();
        const fs::path man = folder_ / "attachments.json";
        fs::path tmp = man; tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { return fail(err, "cannot write the attachments manifest"); }
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            f.flush();
            if (!f) { std::error_code ec; fs::remove(tmp, ec); return fail(err, "cannot write the attachments manifest"); }
        }
        std::error_code ec;
        fs::rename(tmp, man, ec);
        if (ec) { fs::remove(tmp, ec); return fail(err, "cannot replace the attachments manifest"); }
        return true;
    }

    bool parseManifest(const std::string& text, std::string* err) {
        using nlohmann::json;
        if (cascade::core::analyser::detail::maxDepth(text) > 16) { return fail(err, "the attachments manifest is nested too deeply"); }
        const json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return fail(err, "the attachments manifest is not valid JSON"); }
        if (j.value("format", std::string()) != "foxsdr-attachments") { return fail(err, "not an attachments manifest"); }
        const auto v = j.find("version");
        if (v == j.end() || !v->is_number_integer() || v->get<long long>() != 1) { return fail(err, "unknown attachments manifest version"); }
        const auto n = j.find("nextId");
        if (n == j.end() || !n->is_number_unsigned() || n->get<std::uint64_t>() == 0 || n->get<std::uint64_t>() > 0xFFFFFFFFull) { return fail(err, "nextId"); }
        const auto arr = j.find("items");
        if (arr == j.end() || !arr->is_array() || arr->size() > lim_.maxItems) { return fail(err, "items"); }
        std::vector<Item> v2;
        std::set<std::uint32_t> ids;
        std::set<std::string> names;
        for (const json& o : *arr) {
            if (!o.is_object()) { return fail(err, "an item is not an object"); }
            Item it;
            if (!o.contains("id") || !o["id"].is_number_unsigned() || o["id"].get<std::uint64_t>() == 0 || o["id"].get<std::uint64_t>() >= n->get<std::uint64_t>()) { return fail(err, "an item id"); }
            it.id = static_cast<std::uint32_t>(o["id"].get<std::uint64_t>());
            if (!ids.insert(it.id).second) { return fail(err, "a repeated item id"); }
            const std::string k = o.value("kind", std::string());
            if (k == "screenshot") { it.kind = Kind::Screenshot; } else if (k == "clip") { it.kind = Kind::Clip; } else if (k == "note") { it.kind = Kind::Note; } else { return fail(err, "an item kind"); }
            if (!o.contains("title") || !o["title"].is_string()) { return fail(err, "an item title"); }
            it.title = o["title"].get<std::string>();
            if (!detail::cleanText(it.title, lim_.maxTitle, false)) { return fail(err, "an item title"); }
            if (!o.contains("sizeBytes") || !o["sizeBytes"].is_number_unsigned()) { return fail(err, "an item size"); }
            it.sizeBytes = o["sizeBytes"].get<std::uint64_t>();
            if (o.contains("addedMs")) { if (!o["addedMs"].is_number_integer()) { return fail(err, "an item time"); } it.addedMs = o["addedMs"].get<std::int64_t>(); }
            if (it.kind == Kind::Note) {
                if (!o.contains("text") || !o["text"].is_string()) { return fail(err, "a note text"); }
                it.text = o["text"].get<std::string>();
                if (it.text.empty() || !detail::cleanText(it.text, lim_.maxNote, true)) { return fail(err, "a note text"); }
            } else {
                if (!o.contains("file") || !o["file"].is_string() || !o.contains("sha256") || !o["sha256"].is_string()) { return fail(err, "an item file"); }
                it.storedName = o["file"].get<std::string>();
                it.sha256 = o["sha256"].get<std::string>();
                if (!detail::plainName(it.storedName) || !names.insert(it.storedName).second) { return fail(err, "an item file name"); }
                if (it.sha256.size() != 64) { return fail(err, "an item hash"); }
                for (char c : it.sha256) { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return fail(err, "an item hash"); } }
            }
            v2.push_back(std::move(it));
        }
        items_ = std::move(v2);
        nextId_ = static_cast<std::uint32_t>(n->get<std::uint64_t>());
        return true;
    }

    Limits lim_;
    std::filesystem::path folder_;
    std::vector<Item> items_;
    std::uint32_t nextId_ = 1;
    bool opened_ = false;
};

}  // namespace cascade::core::attach
