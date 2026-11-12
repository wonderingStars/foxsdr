// tile_cache.hpp - an on-disk cache for the map tiles the user has VIEWED (row M9 of the gaps survey). NO FETCHING and NO BULK
// DOWNLOAD live here or are meant to: the screen lane calls put() with the bytes of a tile it has just fetched and drawn, and get()
// before it fetches. There is no "download this area" call, no prefetch, no neighbour-tile helper.
//
// POLICY FACTS (https://operations.osmfoundation.org/policies/tiles/, read 2026-11-11, no date shown on the page): the OpenStreetMap
// tile servers forbid bulk downloading, prefetching and "offline use"; clients must honour the cache headers and, if they cannot
// read them, cache a tile at least 7 days. THIS CACHE therefore (a) only stores what put() is given, (b) honours the headers below,
// (c) uses a 7-day default when a response has NO freshness information at all, and (d) reports an expired tile as STALE so the screen
// can revalidate it; whether a stale tile may be shown while offline is the screen's decision and is an OWNER DECISION for the
// OSM servers (see the record).
//
// HEADER RULES (RFC 9111, "HTTP Caching", June 2022, https://www.rfc-editor.org/rfc/rfc9111.html, sections 4.2.1, 5.2.2, 5.3, read 2026-11-11):
//  * Freshness lifetime: max-age if present, else Expires minus Date (the time received if there is no Date). A private cache ignores
//    s-maxage. Expires is ignored when max-age is present. An invalid Expires date (including "0") means already expired.
//  * max-age is 1*DIGIT; anything else (negative, a decimal, text, a quoted string) is invalid freshness information and the response
//    is treated as STALE (RFC 9111 4.2.1). Two different max-age values conflict: treated as stale.
//  * no-store: the tile is NOT stored, and a copy already held is removed. no-cache: stored, but stale at once (revalidate every use).
//  * Only the IMF-fixdate form of an HTTP date ("Sun, 06 Nov 1994 08:49:37 GMT") is parsed; the two obsolete forms are treated as invalid.
//  * The lifetime is capped at maxTtlSec (default 30 days) so one odd header cannot pin a tile for years: a deliberate departure,
//    stated.
//
// STORAGE: root/<source>/<z>/<x>/<y>.tile, each file = 32-byte header (magic "FXTILE01", fetchedAt, expiresAt as int64, payload size
// and CRC-32 as uint32, little endian) + payload. Only PNG, JPEG and WebP payloads up to maxTileBytes are stored (an HTML error page
// must never become a tile). A file whose header or CRC is wrong is deleted and counted, never returned.
// KEYS are validated: source = 1-32 of [a-z0-9_-] starting with a letter or digit, 0 <= z <= 22, 0 <= x,y < 2^z. Every path component
// is built from those validated values, so no key can name a file outside the root (tested with hostile keys).
// SIZE CAP with least-recently-used eviction: the index is rebuilt from the folder on open (file modification time orders the
// pre-existing tiles) and every get() refreshes a tile's position and, best effort, its file time so the order survives a restart.
//
// THREADS: one mutex; get() and put() may be called from any thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace cascade::core::tilecache {

namespace fs = std::filesystem;

struct TileKey {
    std::string source;
    int z = 0, x = 0, y = 0;
};

inline bool keyValid(const TileKey& k) {
    if (k.source.empty() || k.source.size() > 32) return false;
    for (std::size_t i = 0; i < k.source.size(); ++i) {
        const char c = k.source[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && (c == '_' || c == '-')))) return false;
    }
    if (k.z < 0 || k.z > 22) return false;
    const long long n = 1LL << k.z;
    return k.x >= 0 && k.y >= 0 && k.x < n && k.y < n;
}

struct CacheHeaders {
    std::string cacheControl;   // the Cache-Control header value, "" if absent
    std::string expires;        // the Expires header value, "" if absent
    std::string date;           // the Date header value, "" if absent
};

namespace detail {

inline std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
inline std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

inline std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

inline std::uint32_t crc32(const unsigned char* p, std::size_t n) {
    static const std::vector<std::uint32_t> table = [] {
        std::vector<std::uint32_t> t(256);
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline void putLE(std::string& s, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
inline std::uint64_t getLE(const unsigned char* p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return v;
}

}  // namespace detail

// "Sun, 06 Nov 1994 08:49:37 GMT" -> unix seconds. nullopt for anything else.
inline std::optional<std::int64_t> parseHttpDate(const std::string& text) {
    static const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::string s = detail::trim(text);
    if (s.size() != 29) return std::nullopt;
    if (s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' || s[22] != ':' || s[25] != ' ') return std::nullopt;
    if (s.substr(26) != "GMT") return std::nullopt;
    bool dayOk = false;
    for (const char* d : days)
        if (s.compare(0, 3, d) == 0) dayOk = true;
    if (!dayOk) return std::nullopt;
    auto num = [&](int pos, int len, int& out) {
        out = 0;
        for (int i = 0; i < len; ++i) {
            const char c = s[static_cast<std::size_t>(pos + i)];
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
        }
        return true;
    };
    int d, y, hh, mm, ss, mon = 0;
    if (!num(5, 2, d) || !num(12, 4, y) || !num(17, 2, hh) || !num(20, 2, mm) || !num(23, 2, ss)) return std::nullopt;
    bool monOk = false;
    for (int i = 0; i < 12; ++i)
        if (s.compare(8, 3, months[i]) == 0) { mon = i + 1; monOk = true; }
    if (!monOk || d < 1 || d > 31 || hh > 23 || mm > 59 || ss > 60 || y < 1970 || y > 9999) return std::nullopt;
    static const int mdays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (d > mdays[mon - 1]) return std::nullopt;
    return detail::daysFromCivil(y, static_cast<unsigned>(mon), static_cast<unsigned>(d)) * 86400 + hh * 3600 + mm * 60 + ss;
}

struct ExpiryPolicy {
    std::int64_t defaultTtlSec = 7 * 86400;    // no freshness information at all (the OSM policy's seven days)
    std::int64_t maxTtlSec = 30 * 86400;
};

enum class Expiry { Store, NoStore };

struct ExpiryResult {
    Expiry what = Expiry::Store;
    std::int64_t expiresAt = 0;   // unix seconds
    bool headersInvalid = false;  // freshness information was present but unusable (stale at once)
};

// Applies the rules in the header comment to a response received at `now`.
inline ExpiryResult expiryFromHeaders(const CacheHeaders& h, std::int64_t now, const ExpiryPolicy& pol = ExpiryPolicy{}) {
    ExpiryResult r;
    bool noCache = false, haveMaxAge = false, invalid = false;
    std::int64_t maxAge = 0;
    std::size_t pos = 0;
    const std::string cc = h.cacheControl;
    while (pos <= cc.size()) {
        std::size_t comma = cc.find(',', pos);
        if (comma == std::string::npos) comma = cc.size();
        const std::string dir = detail::trim(cc.substr(pos, comma - pos));
        pos = comma + 1;
        if (dir.empty()) continue;
        const std::size_t eq = dir.find('=');
        const std::string name = detail::lower(detail::trim(dir.substr(0, eq)));
        if (name == "no-store") {
            r.what = Expiry::NoStore;
            return r;
        }
        if (name == "no-cache") noCache = true;
        if (name == "max-age") {
            std::int64_t v = -1;
            if (eq != std::string::npos) {
                const std::string arg = detail::trim(dir.substr(eq + 1));
                if (!arg.empty() && arg.size() <= 10) {
                    v = 0;
                    for (char c : arg) {
                        if (c < '0' || c > '9') { v = -1; break; }
                        v = v * 10 + (c - '0');
                    }
                }
            }
            if (v < 0) invalid = true;
            else if (haveMaxAge && v != maxAge) invalid = true;
            else { haveMaxAge = true; maxAge = v; }
        }
    }
    std::int64_t ttl;
    if (noCache || invalid) {
        ttl = 0;
        r.headersInvalid = invalid;
    } else if (haveMaxAge) {
        ttl = maxAge;
    } else if (!h.expires.empty()) {
        const auto exp = parseHttpDate(h.expires);
        if (!exp) {
            ttl = 0;
            r.headersInvalid = true;
        } else {
            std::int64_t base = now;
            if (!h.date.empty()) {
                const auto d = parseHttpDate(h.date);
                if (d) base = *d;
            }
            ttl = std::max<std::int64_t>(0, *exp - base);
        }
    } else {
        ttl = pol.defaultTtlSec;
    }
    ttl = std::min(ttl, pol.maxTtlSec);
    r.expiresAt = now + ttl;
    return r;
}

enum class PutResult { Stored, BadKey, NotAnImage, TooLarge, NoStore, IoError };
enum class GetState { Miss, Fresh, Stale };

struct GetResult {
    GetState state = GetState::Miss;
    std::vector<unsigned char> data;
    std::int64_t expiresAt = 0;
};

struct Config {
    std::uint64_t maxBytes = 256ull << 20;
    std::uint64_t maxTileBytes = 1ull << 20;
    ExpiryPolicy expiry;
};

struct Stats {
    std::uint64_t hits = 0, staleHits = 0, misses = 0, stored = 0, evicted = 0, corrupt = 0, refused = 0;
};

inline bool looksLikeImage(const std::vector<unsigned char>& d) {
    if (d.size() >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' && d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) return true;
    if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return true;
    if (d.size() >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P') return true;
    return false;
}

class TileCache {
public:
    static constexpr std::size_t kHeaderBytes = 32;

    explicit TileCache(fs::path root, Config cfg = Config{}) : root_(std::move(root)), cfg_(cfg) {
        std::error_code ec;
        fs::create_directories(root_, ec);
        std::lock_guard<std::mutex> lk(m_);
        scanLocked();
        evictLocked(nullptr);
    }

    // The file a key maps to, or nullopt for an invalid key.
    std::optional<fs::path> pathFor(const TileKey& k) const {
        if (!keyValid(k)) return std::nullopt;
        return root_ / k.source / std::to_string(k.z) / std::to_string(k.x) / (std::to_string(k.y) + ".tile");
    }

    PutResult put(const TileKey& k, const std::vector<unsigned char>& data, const CacheHeaders& hdr, std::int64_t now) {
        std::lock_guard<std::mutex> lk(m_);
        const auto path = pathFor(k);
        if (!path) { ++stats_.refused; return PutResult::BadKey; }
        const std::string id = idOf(k);
        const ExpiryResult ex = expiryFromHeaders(hdr, now, cfg_.expiry);
        if (ex.what == Expiry::NoStore) {
            removeLocked(id);
            return PutResult::NoStore;
        }
        if (!looksLikeImage(data)) { ++stats_.refused; return PutResult::NotAnImage; }
        if (data.size() > cfg_.maxTileBytes || data.size() + kHeaderBytes > cfg_.maxBytes) { ++stats_.refused; return PutResult::TooLarge; }
        std::string file;
        file += "FXTILE01";
        detail::putLE(file, static_cast<std::uint64_t>(now), 8);
        detail::putLE(file, static_cast<std::uint64_t>(ex.expiresAt), 8);
        detail::putLE(file, data.size(), 4);
        detail::putLE(file, detail::crc32(data.data(), data.size()), 4);
        file.append(reinterpret_cast<const char*>(data.data()), data.size());
        std::error_code ec;
        fs::create_directories(path->parent_path(), ec);
        fs::path tmp = *path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return PutResult::IoError;
            f.write(file.data(), static_cast<std::streamsize>(file.size()));
            f.flush();
            if (!f) { f.close(); fs::remove(tmp, ec); return PutResult::IoError; }
        }
        fs::rename(tmp, *path, ec);
        if (ec) { fs::remove(tmp, ec); return PutResult::IoError; }
        auto it = index_.find(id);
        if (it != index_.end()) total_ -= it->second.size;
        index_[id] = Entry{*path, file.size(), ex.expiresAt, ++useSeq_};
        total_ += file.size();
        ++stats_.stored;
        evictLocked(&id);
        return PutResult::Stored;
    }

    GetResult get(const TileKey& k, std::int64_t now) {
        std::lock_guard<std::mutex> lk(m_);
        GetResult r;
        const auto path = pathFor(k);
        if (!path) { ++stats_.misses; return r; }
        const std::string id = idOf(k);
        auto it = index_.find(id);
        if (it == index_.end()) { ++stats_.misses; return r; }
        std::int64_t expires = 0;
        std::vector<unsigned char> payload;
        if (!readTile(it->second.path, payload, nullptr, &expires)) {
            ++stats_.corrupt;
            ++stats_.misses;
            std::error_code ec;
            fs::remove(it->second.path, ec);
            total_ -= it->second.size;
            index_.erase(it);
            return r;
        }
        it->second.lastUse = ++useSeq_;
        std::error_code ec;
        fs::last_write_time(it->second.path, fs::file_time_type::clock::now(), ec);   // best effort: keep LRU order across restarts
        r.data = std::move(payload);
        r.expiresAt = expires;
        r.state = now < expires ? GetState::Fresh : GetState::Stale;
        if (r.state == GetState::Fresh) ++stats_.hits; else ++stats_.staleHits;
        return r;
    }

    bool remove(const TileKey& k) {
        std::lock_guard<std::mutex> lk(m_);
        if (!keyValid(k)) return false;
        return removeLocked(idOf(k));
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& kv : index_) {
            std::error_code ec;
            fs::remove(kv.second.path, ec);
        }
        index_.clear();
        total_ = 0;
    }

    std::uint64_t totalBytes() const { std::lock_guard<std::mutex> lk(m_); return total_; }
    std::size_t tileCount() const { std::lock_guard<std::mutex> lk(m_); return index_.size(); }
    Stats stats() const { std::lock_guard<std::mutex> lk(m_); return stats_; }
    const fs::path& root() const { return root_; }

private:
    struct Entry {
        fs::path path;
        std::uint64_t size = 0;
        std::int64_t expiresAt = 0;
        std::uint64_t lastUse = 0;
    };

    static std::string idOf(const TileKey& k) {
        return k.source + "/" + std::to_string(k.z) + "/" + std::to_string(k.x) + "/" + std::to_string(k.y);
    }

    // Reads and verifies one tile file. fetchedAt / expiresAt are optional outputs.
    static bool readTile(const fs::path& p, std::vector<unsigned char>& payload, std::int64_t* fetchedAt, std::int64_t* expiresAt) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        unsigned char h[kHeaderBytes];
        f.read(reinterpret_cast<char*>(h), kHeaderBytes);
        if (f.gcount() != static_cast<std::streamsize>(kHeaderBytes)) return false;
        if (std::string(reinterpret_cast<char*>(h), 8) != "FXTILE01") return false;
        const std::uint64_t size = detail::getLE(h + 24, 4), crc = detail::getLE(h + 28, 4);
        if (size > (64ull << 20)) return false;
        payload.resize(static_cast<std::size_t>(size));
        f.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(size));
        if (f.gcount() != static_cast<std::streamsize>(size)) return false;
        if (f.peek() != std::ifstream::traits_type::eof()) return false;          // trailing bytes: not our file
        if (detail::crc32(payload.data(), payload.size()) != crc) return false;
        if (fetchedAt) *fetchedAt = static_cast<std::int64_t>(detail::getLE(h + 8, 8));
        if (expiresAt) *expiresAt = static_cast<std::int64_t>(detail::getLE(h + 16, 8));
        return true;
    }

    bool removeLocked(const std::string& id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        std::error_code ec;
        fs::remove(it->second.path, ec);
        total_ -= it->second.size;
        index_.erase(it);
        return true;
    }

    void scanLocked() {
        index_.clear();
        total_ = 0;
        std::error_code ec;
        if (!fs::is_directory(root_, ec)) return;
        struct Found { std::string id; Entry e; fs::file_time_type mtime; };
        std::vector<Found> found;
        for (fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path p = it->path();
            if (p.extension() == ".tmp") { fs::remove(p, ec); continue; }
            if (p.extension() != ".tile") continue;
            // expected shape: <source>/<z>/<x>/<y>.tile under the root
            const fs::path rel = p.lexically_relative(root_);
            std::vector<std::string> parts;
            for (const auto& c : rel) parts.push_back(c.string());
            if (parts.size() != 4) continue;
            TileKey k;
            try {
                k.source = parts[0];
                k.z = std::stoi(parts[1]);
                k.x = std::stoi(parts[2]);
                k.y = std::stoi(fs::path(parts[3]).stem().string());
            } catch (...) { continue; }
            if (!keyValid(k)) continue;
            std::vector<unsigned char> payload;
            std::int64_t fetched = 0, expires = 0;
            if (!readTile(p, payload, &fetched, &expires)) {
                fs::remove(p, ec);
                ++stats_.corrupt;
                continue;
            }
            Found fd;
            fd.id = idOf(k);
            fd.e = Entry{p, fs::file_size(p, ec), expires, 0};
            fd.mtime = fs::last_write_time(p, ec);
            found.push_back(std::move(fd));
        }
        std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.mtime < b.mtime; });
        for (auto& fd : found) {
            fd.e.lastUse = ++useSeq_;
            total_ += fd.e.size;
            index_[fd.id] = std::move(fd.e);
        }
    }

    // Evicts least-recently-used tiles until the total fits; never the one named by `keep`.
    void evictLocked(const std::string* keep) {
        while (total_ > cfg_.maxBytes && !index_.empty()) {
            auto victim = index_.end();
            for (auto it = index_.begin(); it != index_.end(); ++it) {
                if (keep && it->first == *keep) continue;
                if (victim == index_.end() || it->second.lastUse < victim->second.lastUse) victim = it;
            }
            if (victim == index_.end()) break;
            std::error_code ec;
            fs::remove(victim->second.path, ec);
            total_ -= victim->second.size;
            index_.erase(victim);
            ++stats_.evicted;
        }
    }

    fs::path root_;
    Config cfg_;
    mutable std::mutex m_;
    std::map<std::string, Entry> index_;
    std::uint64_t total_ = 0;
    std::uint64_t useSeq_ = 0;
    Stats stats_;
};

}  // namespace cascade::core::tilecache
