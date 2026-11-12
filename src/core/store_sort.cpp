// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/store_sort.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <nlohmann/json.hpp>

namespace cascade::core {
namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::vector<std::string> splitSegments(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : v) {
        if (c == '.') { out.push_back(cur); cur.clear(); } else { cur.push_back(c); }
    }
    out.push_back(cur);
    return out;
}

bool allDigits(const std::string& s) {
    if (s.empty()) { return false; }
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string stripZeros(const std::string& s) {
    std::size_t i = 0;
    while (i + 1 < s.size() && s[i] == '0') { ++i; }
    return s.substr(i);
}

int compareDigits(const std::string& a, const std::string& b) {
    const std::string x = stripZeros(a), y = stripZeros(b);
    if (x.size() != y.size()) { return x.size() < y.size() ? -1 : 1; }
    if (x == y) { return 0; }
    return x < y ? -1 : 1;
}

// Cuts at a character boundary at or below maxBytes and drops control characters (a note is one paragraph: line breaks become spaces).
std::string cleanText(const std::string& in, std::size_t maxBytes) {
    std::string out;
    for (const char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\n' || c == '\r' || c == '\t') { out.push_back(' '); continue; }
        if (c < 0x20 || c == 0x7F) { continue; }
        out.push_back(ch);
    }
    if (out.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) { --cut; }
        out.resize(cut);
    }
    return out;
}

bool validUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned c = p[i];
        std::size_t len;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { len = 2; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; }
        else { return false; }
        if (i + len > n) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) { return false; }
        }
        i += len;
    }
    return true;
}

std::string keyOf(const std::string& source, const std::string& id) { return source + '\n' + id; }

}  // namespace

double dampedRating(const StoreStats& s) {
    if (s.ratingCount == 0 || !std::isfinite(s.ratingAverage)) { return 0.0; }
    const double avg = std::min(std::max(s.ratingAverage, 0.0), 5.0);
    const double n = static_cast<double>(s.ratingCount);
    return (kRatingPriorVotes * kRatingPriorMean + avg * n) / (kRatingPriorVotes + n);
}

int compareStoreVersions(const std::string& a, const std::string& b) {
    const auto sa = splitSegments(a);
    const auto sb = splitSegments(b);
    const std::size_t n = std::max(sa.size(), sb.size());
    for (std::size_t i = 0; i < n; ++i) {
        std::string x = i < sa.size() ? sa[i] : std::string();
        std::string y = i < sb.size() ? sb[i] : std::string();
        if (x.empty()) { x = "0"; }
        if (y.empty()) { y = "0"; }
        const bool nx = allDigits(x), ny = allDigits(y);
        if (nx && ny) {
            const int c = compareDigits(x, y);
            if (c != 0) { return c; }
        } else if (nx != ny) {
            return nx ? 1 : -1;
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

bool validStoreDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') { return false; }
    for (std::size_t i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) { continue; }
        if (s[i] < '0' || s[i] > '9') { return false; }
    }
    const int y = std::stoi(s.substr(0, 4));
    const int m = std::stoi(s.substr(5, 2));
    const int d = std::stoi(s.substr(8, 2));
    if (y < 1970 || m < 1 || m > 12 || d < 1) { return false; }
    static const int dim[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return d <= dim[m - 1];
}

std::vector<std::size_t> sortStoreEntries(const std::vector<PluginCatalogEntry>& entries, const StoreStatsMap& stats, StoreSortKey key) {
    std::vector<std::size_t> idx(entries.size());
    for (std::size_t i = 0; i < idx.size(); ++i) { idx[i] = i; }
    auto statsOf = [&](std::size_t i) -> const StoreStats* {
        const auto it = stats.find(entries[i].id);
        return it == stats.end() ? nullptr : &it->second;
    };
    auto byName = [&](std::size_t a, std::size_t b) {
        const std::string na = lower(entries[a].name.empty() ? entries[a].id : entries[a].name);
        const std::string nb = lower(entries[b].name.empty() ? entries[b].id : entries[b].name);
        if (na != nb) { return na < nb; }
        if (entries[a].id != entries[b].id) { return entries[a].id < entries[b].id; }
        return entries[a].source < entries[b].source;
    };
    // Returns <0 when a belongs first; 0 when the key does not separate them.
    auto keyCmp = [&](std::size_t a, std::size_t b) -> int {
        switch (key) {
            case StoreSortKey::Name: return 0;
            case StoreSortKey::Updated: {
                const bool va = validStoreDate(entries[a].published), vb = validStoreDate(entries[b].published);
                if (va != vb) { return va ? -1 : 1; }
                if (va && entries[a].published != entries[b].published) { return entries[a].published > entries[b].published ? -1 : 1; }
                return -compareStoreVersions(entries[a].version, entries[b].version);
            }
            case StoreSortKey::Popular: {
                const StoreStats* sa = statsOf(a);
                const StoreStats* sb = statsOf(b);
                if ((sa == nullptr) != (sb == nullptr)) { return sa != nullptr ? -1 : 1; }
                if (sa != nullptr && sa->installs != sb->installs) { return sa->installs > sb->installs ? -1 : 1; }
                return 0;
            }
            case StoreSortKey::Rating: {
                const StoreStats* sa = statsOf(a);
                const StoreStats* sb = statsOf(b);
                const bool ra = sa != nullptr && sa->ratingCount > 0 && std::isfinite(sa->ratingAverage);
                const bool rb = sb != nullptr && sb->ratingCount > 0 && std::isfinite(sb->ratingAverage);
                if (ra != rb) { return ra ? -1 : 1; }
                if (ra) {
                    const double da = dampedRating(*sa), db = dampedRating(*sb);
                    if (da != db) { return da > db ? -1 : 1; }
                }
                return 0;
            }
        }
        return 0;
    };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        const int c = keyCmp(a, b);
        if (c != 0) { return c < 0; }
        return byName(a, b);
    });
    return idx;
}

bool ReleaseNoteHistory::add(const std::string& source, const std::string& id, StoreReleaseNote note) {
    const std::string k = keyOf(source, id);
    auto it = map_.find(k);
    if (it == map_.end()) {
        if (map_.size() >= kMaxHistoryPlugins) { return false; }
        it = map_.emplace(k, std::vector<StoreReleaseNote>{}).first;
    }
    auto& v = it->second;
    for (const StoreReleaseNote& e : v) {
        if (compareStoreVersions(e.version, note.version) == 0 && e.version == note.version) { return false; }  // first seen wins
    }
    v.push_back(std::move(note));
    std::sort(v.begin(), v.end(), [](const StoreReleaseNote& a, const StoreReleaseNote& b) {
        const int c = compareStoreVersions(a.version, b.version);
        return c != 0 ? c > 0 : a.version > b.version;
    });
    if (v.size() > kMaxHistoryPerPlugin) { v.resize(kMaxHistoryPerPlugin); }
    return true;
}

std::size_t ReleaseNoteHistory::observe(const std::vector<PluginCatalogEntry>& entries) {
    std::size_t added = 0;
    for (const PluginCatalogEntry& e : entries) {
        if (e.id.empty() || e.version.empty() || e.id.size() > 128 || e.source.size() > 256) { continue; }
        StoreReleaseNote n;
        n.version = cleanText(e.version, 64);
        if (n.version.empty()) { continue; }
        n.published = validStoreDate(e.published) ? e.published : std::string();
        n.notes = cleanText(e.whatsNew, kMaxNoteBytes);
        if (add(e.source, e.id, std::move(n))) { ++added; }
    }
    return added;
}

std::vector<StoreReleaseNote> ReleaseNoteHistory::history(const std::string& id, const std::string& source) const {
    const auto it = map_.find(keyOf(source, id));
    return it == map_.end() ? std::vector<StoreReleaseNote>{} : it->second;
}

std::string ReleaseNoteHistory::toJson() const {
    using nlohmann::json;
    json plugins = json::array();
    for (const auto& kv : map_) {
        const std::size_t nl = kv.first.find('\n');
        json p;
        p["source"] = kv.first.substr(0, nl);
        p["id"] = kv.first.substr(nl + 1);
        json rel = json::array();
        for (const StoreReleaseNote& r : kv.second) {
            json e;
            e["version"] = r.version;
            e["published"] = r.published;
            e["notes"] = r.notes;
            rel.push_back(std::move(e));
        }
        p["releases"] = std::move(rel);
        plugins.push_back(std::move(p));
    }
    json root;
    root["version"] = 1;
    root["plugins"] = std::move(plugins);
    return root.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool ReleaseNoteHistory::fromJson(const std::string& text, ReleaseNoteHistory& into) {
    using nlohmann::json;
    if (text.size() > kMaxHistoryJsonBytes) { return false; }
    const json doc = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) { return false; }
    const auto v = doc.find("version");
    if (v == doc.end() || !v->is_number_integer() || v->get<long long>() != 1) { return false; }
    const auto pl = doc.find("plugins");
    if (pl == doc.end() || !pl->is_array()) { return false; }
    ReleaseNoteHistory fresh;
    for (const json& p : *pl) {
        if (!p.is_object()) { continue; }
        const auto id = p.find("id");
        const auto src = p.find("source");
        const auto rel = p.find("releases");
        if (id == p.end() || !id->is_string() || rel == p.end() || !rel->is_array()) { continue; }
        const std::string sid = id->get<std::string>();
        const std::string ssrc = (src != p.end() && src->is_string()) ? src->get<std::string>() : std::string();
        if (sid.empty() || sid.size() > 128 || ssrc.size() > 256 || sid.find('\n') != std::string::npos || ssrc.find('\n') != std::string::npos) {
            continue;
        }
        for (const json& r : *rel) {
            if (!r.is_object()) { continue; }
            const auto ver = r.find("version");
            if (ver == r.end() || !ver->is_string()) { continue; }
            StoreReleaseNote n;
            n.version = ver->get<std::string>();
            if (n.version.empty() || n.version.size() > 64 || !validUtf8(n.version)) { continue; }
            const auto pub = r.find("published");
            if (pub != r.end() && pub->is_string() && validStoreDate(pub->get<std::string>())) { n.published = pub->get<std::string>(); }
            const auto notes = r.find("notes");
            if (notes != r.end() && notes->is_string()) {
                const std::string raw = notes->get<std::string>();
                if (validUtf8(raw)) { n.notes = cleanText(raw, kMaxNoteBytes); }
            }
            fresh.add(ssrc, sid, std::move(n));
        }
    }
    into = std::move(fresh);
    return true;
}

}  // namespace cascade::core
