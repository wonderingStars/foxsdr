// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_bookmarks.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <nlohmann/json.hpp>

namespace cascade::net {
namespace {

using nlohmann::json;

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

bool validUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned c = p[i];
        if (c < 0x80) { ++i; continue; }
        std::size_t len = 0;
        unsigned cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else { return false; }
        if (i + len > n) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) { return false; }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) { return false; }
        if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) { return false; }
        i += len;
    }
    return true;
}

bool sameText(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

bool containsNoCase(const std::string& hay, const std::string& needleLower) {
    return lower(hay).find(needleLower) != std::string::npos;
}

bool less(const WebBookmark& a, const WebBookmark& b) {
    const std::string ga = lower(a.group), gb = lower(b.group);
    if (ga != gb) { return ga < gb; }
    if (a.freqHz != b.freqHz) { return a.freqHz < b.freqHz; }
    if (a.name != b.name) { return a.name < b.name; }
    return a.id < b.id;
}

}  // namespace

const char* bookmarkResultText(BookmarkResult r) {
    switch (r) {
        case BookmarkResult::Ok: return "ok";
        case BookmarkResult::BadToken: return "the viewer token is not valid";
        case BookmarkResult::TooManyViewers: return "too many viewers hold bookmark sets";
        case BookmarkResult::TooManyBookmarks: return "this viewer's bookmark set is full";
        case BookmarkResult::BadName: return "the bookmark name is empty, too long or has invalid characters";
        case BookmarkResult::BadGroup: return "the group name is too long or has invalid characters";
        case BookmarkResult::BadTag: return "a tag is not 1-24 letters, digits, '-' or '_'";
        case BookmarkResult::TooManyTags: return "too many tags on one bookmark";
        case BookmarkResult::BadFrequency: return "the frequency is out of range";
        case BookmarkResult::BadMode: return "the mode is not one the receiver knows";
        case BookmarkResult::BadBandwidth: return "the bandwidth is out of range";
        case BookmarkResult::Duplicate: return "that bookmark is already in the set";
        case BookmarkResult::NotFound: return "no such bookmark";
        case BookmarkResult::BadJson: return "the bookmark file is not valid";
        case BookmarkResult::TooLarge: return "the bookmark file is too large";
    }
    return "unknown";
}

bool validBookmarkText(const std::string& s, std::size_t maxBytes) {
    if (s.empty() || s.size() > maxBytes) { return false; }
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) { return false; }
    }
    if (!validUtf8(s)) { return false; }
    return s.find_first_not_of(' ') != std::string::npos;
}

bool normaliseBookmarkMode(const std::string& in, std::string& out) {
    static const char* const kModes[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
    const std::string u = upper(in);
    for (const char* m : kModes) {
        if (u == m) { out = u; return true; }
    }
    return false;
}

bool normaliseBookmarkTag(const std::string& in, std::string& out) {
    if (in.empty() || in.size() > kMaxBookmarkTag) { return false; }
    std::string t = lower(in);
    for (const char c : t) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) { return false; }
    }
    out = std::move(t);
    return true;
}

bool validViewerToken(const std::string& token) {
    if (token.size() < 8 || token.size() > 128) { return false; }
    for (const char c : token) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) { return false; }
    }
    return true;
}

BookmarkResult WebBookmarkBook::validate(WebBookmark& bm) {
    if (!validBookmarkText(bm.name, kMaxBookmarkName)) { return BookmarkResult::BadName; }
    if (!bm.group.empty() && !validBookmarkText(bm.group, kMaxBookmarkGroup)) { return BookmarkResult::BadGroup; }
    if (bm.tags.size() > kMaxBookmarkTags) { return BookmarkResult::TooManyTags; }
    std::vector<std::string> tags;
    for (const std::string& t : bm.tags) {
        std::string n;
        if (!normaliseBookmarkTag(t, n)) { return BookmarkResult::BadTag; }
        if (std::find(tags.begin(), tags.end(), n) == tags.end()) { tags.push_back(std::move(n)); }
    }
    bm.tags = std::move(tags);
    if (!std::isfinite(bm.freqHz) || bm.freqHz < kMinBookmarkHz || bm.freqHz > kMaxBookmarkHz) { return BookmarkResult::BadFrequency; }
    std::string mode;
    if (!normaliseBookmarkMode(bm.mode, mode)) { return BookmarkResult::BadMode; }
    bm.mode = std::move(mode);
    if (!std::isfinite(bm.bandwidthHz) || bm.bandwidthHz < kMinBookmarkBandwidthHz || bm.bandwidthHz > kMaxBookmarkBandwidthHz) {
        return BookmarkResult::BadBandwidth;
    }
    return BookmarkResult::Ok;
}

BookmarkResult WebBookmarkBook::addLocked(const std::string& token, WebBookmark bm, std::uint64_t* idOut) {
    if (!validViewerToken(token)) { return BookmarkResult::BadToken; }
    const BookmarkResult v = validate(bm);
    if (v != BookmarkResult::Ok) { return v; }
    auto it = sets_.find(token);
    if (it == sets_.end()) {
        if (sets_.size() >= kMaxBookmarkViewers) { return BookmarkResult::TooManyViewers; }
        it = sets_.emplace(token, Set{}).first;
    }
    auto& items = it->second.items;
    if (items.size() >= kMaxBookmarksPerViewer) {
        return BookmarkResult::TooManyBookmarks;
    }
    for (const WebBookmark& e : items) {
        if (e.name == bm.name && e.freqHz == bm.freqHz && e.mode == bm.mode) { return BookmarkResult::Duplicate; }
    }
    bm.id = nextId_++;
    if (idOut) { *idOut = bm.id; }
    items.push_back(std::move(bm));
    return BookmarkResult::Ok;
}

BookmarkResult WebBookmarkBook::add(const std::string& viewerToken, WebBookmark bm, std::uint64_t* idOut) {
    std::lock_guard<std::mutex> lk(mu_);
    // A viewer created only to hold a refused bookmark must not linger: addLocked() creates the set after validating, but a refusal for a
    // full set happens on an existing one, so nothing to undo here.
    return addLocked(viewerToken, std::move(bm), idOut);
}

BookmarkResult WebBookmarkBook::remove(const std::string& viewerToken, std::uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!validViewerToken(viewerToken)) { return BookmarkResult::BadToken; }
    const auto it = sets_.find(viewerToken);
    if (it == sets_.end()) { return BookmarkResult::NotFound; }
    auto& items = it->second.items;
    const auto e = std::find_if(items.begin(), items.end(), [id](const WebBookmark& b) { return b.id == id; });
    if (e == items.end()) { return BookmarkResult::NotFound; }
    items.erase(e);
    return BookmarkResult::Ok;
}

BookmarkResult WebBookmarkBook::update(const std::string& viewerToken, std::uint64_t id, WebBookmark bm) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!validViewerToken(viewerToken)) { return BookmarkResult::BadToken; }
    const BookmarkResult v = validate(bm);
    if (v != BookmarkResult::Ok) { return v; }
    const auto it = sets_.find(viewerToken);
    if (it == sets_.end()) { return BookmarkResult::NotFound; }
    auto& items = it->second.items;
    const auto e = std::find_if(items.begin(), items.end(), [id](const WebBookmark& b) { return b.id == id; });
    if (e == items.end()) { return BookmarkResult::NotFound; }
    for (const WebBookmark& o : items) {
        if (o.id != id && o.name == bm.name && o.freqHz == bm.freqHz && o.mode == bm.mode) { return BookmarkResult::Duplicate; }
    }
    bm.id = id;
    *e = std::move(bm);
    return BookmarkResult::Ok;
}

std::vector<WebBookmark> WebBookmarkBook::list(const std::string& viewerToken) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<WebBookmark> out;
    const auto it = sets_.find(viewerToken);
    if (it == sets_.end()) { return out; }
    out = it->second.items;
    std::sort(out.begin(), out.end(), less);
    return out;
}

std::vector<WebBookmark> WebBookmarkBook::search(const std::string& viewerToken, const BookmarkQuery& q) const {
    std::vector<WebBookmark> out;
    if (q.text.size() > kMaxSearchQuery || q.group.size() > kMaxBookmarkGroup * 4 || q.tag.size() > kMaxBookmarkTag * 4) { return out; }
    const std::string text = lower(q.text);
    std::string wantMode;
    if (!q.mode.empty()) {
        if (!normaliseBookmarkMode(q.mode, wantMode)) { return out; }
    }
    const std::string wantTag = lower(q.tag);
    for (const WebBookmark& b : list(viewerToken)) {
        if (!text.empty()) {
            bool hit = containsNoCase(b.name, text) || containsNoCase(b.group, text);
            for (const std::string& t : b.tags) { hit = hit || t.find(text) != std::string::npos; }
            if (!hit) { continue; }
        }
        if (!q.group.empty() && !sameText(b.group, q.group)) { continue; }
        if (!wantMode.empty() && b.mode != wantMode) { continue; }
        if (!wantTag.empty() && std::find(b.tags.begin(), b.tags.end(), wantTag) == b.tags.end()) { continue; }
        if (q.minHz > 0.0 && b.freqHz < q.minHz) { continue; }
        if (q.maxHz > 0.0 && b.freqHz > q.maxHz) { continue; }
        out.push_back(b);
    }
    return out;
}

std::vector<std::string> WebBookmarkBook::groups(const std::string& viewerToken) const {
    std::vector<std::string> out;
    for (const WebBookmark& b : list(viewerToken)) {
        if (b.group.empty()) { continue; }
        const bool seen = std::any_of(out.begin(), out.end(), [&](const std::string& g) { return sameText(g, b.group); });
        if (!seen) { out.push_back(b.group); }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

std::string WebBookmarkBook::exportJson(const std::string& viewerToken) const {
    json arr = json::array();
    for (const WebBookmark& b : list(viewerToken)) {
        json e;
        e["name"] = b.name;
        e["freqHz"] = b.freqHz;
        e["mode"] = b.mode;
        e["bandwidthHz"] = b.bandwidthHz;
        e["group"] = b.group;
        e["tags"] = b.tags;
        arr.push_back(std::move(e));
    }
    json root;
    root["version"] = 1;
    root["bookmarks"] = std::move(arr);
    // Replace invalid UTF-8 rather than throw: names were validated on the way in, so this is belt and braces.
    return root.dump(-1, ' ', false, json::error_handler_t::replace);
}

BookmarkImportResult WebBookmarkBook::importJson(const std::string& viewerToken, const std::string& text, bool replace) {
    BookmarkImportResult r;
    if (!validViewerToken(viewerToken)) { r.result = BookmarkResult::BadToken; return r; }
    if (text.size() > kMaxBookmarkImportBytes) { r.result = BookmarkResult::TooLarge; return r; }
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    const json* list = nullptr;
    if (doc.is_array()) {
        list = &doc;
    } else if (doc.is_object()) {
        const auto it = doc.find("bookmarks");
        if (it != doc.end() && it->is_array()) { list = &*it; }
    }
    if (list == nullptr) { r.result = BookmarkResult::BadJson; return r; }
    std::lock_guard<std::mutex> lk(mu_);
    // The set is replaced only when the document is understood, and an import that would create a new viewer beyond the cap leaves
    // everything as it was.
    if (replace) {
        sets_.erase(viewerToken);
    }
    for (const json& e : *list) {
        if (!e.is_object()) { ++r.skipped; continue; }
        WebBookmark b;
        const auto name = e.find("name");
        const auto freq = e.find("freqHz");
        if (name == e.end() || !name->is_string() || freq == e.end() || !freq->is_number()) { ++r.skipped; continue; }
        b.name = name->get<std::string>();
        b.freqHz = freq->get<double>();
        const auto mode = e.find("mode");
        if (mode != e.end()) {
            if (!mode->is_string()) { ++r.skipped; continue; }
            b.mode = mode->get<std::string>();
        }
        const auto bw = e.find("bandwidthHz");
        if (bw != e.end()) {
            if (!bw->is_number()) { ++r.skipped; continue; }
            b.bandwidthHz = bw->get<double>();
        }
        const auto grp = e.find("group");
        if (grp != e.end()) {
            if (!grp->is_string()) { ++r.skipped; continue; }
            b.group = grp->get<std::string>();
        }
        const auto tags = e.find("tags");
        bool tagsOk = true;
        if (tags != e.end()) {
            if (!tags->is_array()) { tagsOk = false; }
            else {
                for (const json& t : *tags) {
                    if (!t.is_string()) { tagsOk = false; break; }
                    b.tags.push_back(t.get<std::string>());
                }
            }
        }
        if (!tagsOk) { ++r.skipped; continue; }
        const BookmarkResult a = addLocked(viewerToken, std::move(b), nullptr);
        if (a == BookmarkResult::Ok) { ++r.imported; }
        else if (a == BookmarkResult::TooManyViewers) { r.result = a; break; }
        else { ++r.skipped; }
    }
    return r;
}

void WebBookmarkBook::forgetViewer(const std::string& viewerToken) {
    std::lock_guard<std::mutex> lk(mu_);
    sets_.erase(viewerToken);
}

void WebBookmarkBook::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    sets_.clear();
}

std::size_t WebBookmarkBook::viewerCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return sets_.size();
}

std::size_t WebBookmarkBook::size(const std::string& viewerToken) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = sets_.find(viewerToken);
    return it == sets_.end() ? 0 : it->second.items.size();
}

}  // namespace cascade::net
