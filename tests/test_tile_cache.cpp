// Tests for core/tile_cache.hpp: cache-header expiry (RFC 9111 rules), key and path safety, image-only payloads, size cap with
// least-recently-used eviction, corrupted files, restart, concurrency, and a scan proving the header contains no fetching code.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/tile_cache.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <random>
#include <set>
#include <thread>

#include "test_check.hpp"

namespace tc = cascade::core::tilecache;
namespace fs = std::filesystem;

static std::vector<unsigned char> png(std::size_t size, unsigned char fill = 7) {
    std::vector<unsigned char> d(size, fill);
    const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8 && i < static_cast<int>(size); ++i) d[static_cast<std::size_t>(i)] = sig[i];
    return d;
}
static tc::CacheHeaders H(const char* cc, const char* expires = "", const char* date = "") { return {cc, expires, date}; }
static const std::int64_t N = 1700000000;

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static void spit(const fs::path& p, const std::string& s) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(s.data(), static_cast<std::streamsize>(s.size()));
}

int main() {
    const fs::path base = fs::temp_directory_path() / "netmap-tiles-test";
    fs::remove_all(base);
    fs::create_directories(base);

    // --- HTTP date ---------------------------------------------------------------------------------------------------------------
    CHECK(tc::parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777);      // the RFC's example date
    CHECK(tc::parseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT") == 0);
    CHECK(tc::parseHttpDate("Thu, 29 Feb 2024 12:00:00 GMT") == 1709208000);
    CHECK(!tc::parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT"));                  // obsolete forms are invalid here
    CHECK(!tc::parseHttpDate("Sun Nov  6 08:49:37 1994"));
    CHECK(!tc::parseHttpDate("0"));
    CHECK(!tc::parseHttpDate(""));
    CHECK(!tc::parseHttpDate("Sun, 06 Nov 1994 08:49:37 UTC"));
    CHECK(!tc::parseHttpDate("Sun, 06 Nov 1994 08:49:37 gmt"));
    CHECK(!tc::parseHttpDate("Sun, 31 Feb 2024 08:49:37 GMT"));
    CHECK(!tc::parseHttpDate("Sun, 06 Nox 1994 08:49:37 GMT"));
    CHECK(!tc::parseHttpDate("Xyz, 06 Nov 1994 08:49:37 GMT"));
    CHECK(!tc::parseHttpDate("Sun, 06 Nov 1994 25:49:37 GMT"));
    CHECK(!tc::parseHttpDate("Sun, 06 Nov 19x4 08:49:37 GMT"));
    CHECK(!tc::parseHttpDate("Sun, 06 Nov 1969 08:49:37 GMT"));

    // --- expiry from headers ----------------------------------------------------------------------------------------------------------
    {
        auto ex = [&](const char* cc, const char* e = "", const char* d = "") { return tc::expiryFromHeaders(H(cc, e, d), N); };
        CHECK(ex("max-age=3600").expiresAt == N + 3600);
        CHECK(ex("public, max-age=86400").expiresAt == N + 86400);
        CHECK(ex("  Max-Age = 60 , public").expiresAt == N + 60);
        CHECK(ex("MAX-AGE=60").expiresAt == N + 60);
        CHECK(ex("max-age=0").expiresAt == N);
        CHECK(ex("no-cache").expiresAt == N && ex("no-cache").what == tc::Expiry::Store);
        CHECK(ex("no-cache, max-age=3600").expiresAt == N);                         // the more restrictive wins
        CHECK(ex("no-store").what == tc::Expiry::NoStore);
        CHECK(ex("max-age=3600, no-store").what == tc::Expiry::NoStore);
        CHECK(ex("s-maxage=10, max-age=60").expiresAt == N + 60);                   // a private cache ignores s-maxage
        CHECK(ex("s-maxage=10").expiresAt == N + 7 * 86400);                        // ... so nothing usable: the default
        for (const char* bad : {"max-age=abc", "max-age=-5", "max-age=1.5", "max-age=\"60\"", "max-age=", "max-age", "max-age=99999999999"}) {
            const auto r = ex(bad);
            CHECK(r.expiresAt == N && r.headersInvalid);                            // invalid freshness information: stale at once
        }
        CHECK(ex("max-age=60, max-age=120").headersInvalid);                        // conflicting duplicates
        CHECK(ex("max-age=60, max-age=60").expiresAt == N + 60 && !ex("max-age=60, max-age=60").headersInvalid);
        CHECK(ex("max-age=9999999999").expiresAt == N + 30 * 86400);                // capped at 30 days
        CHECK(ex("").expiresAt == N + 7 * 86400);                                   // no information: the seven-day default
        CHECK(ex("private").expiresAt == N + 7 * 86400);
        // Expires minus Date
        CHECK(ex("", "Sun, 06 Nov 1994 09:49:37 GMT", "Sun, 06 Nov 1994 08:49:37 GMT").expiresAt == N + 3600);
        CHECK(ex("", "Sun, 06 Nov 1994 08:49:37 GMT", "Sun, 06 Nov 1994 09:49:37 GMT").expiresAt == N);   // already past
        CHECK(ex("", "0").expiresAt == N && ex("", "0").headersInvalid);                                    // "0" means expired
        CHECK(ex("", "garbage").headersInvalid);
        CHECK(ex("", "Sun, 06 Nov 1994 09:49:37 GMT", "nonsense").expiresAt == N);   // a bad Date: judged from the time received (1994 < now)
        CHECK(ex("max-age=100", "Sun, 06 Nov 1994 09:49:37 GMT", "Sun, 06 Nov 1994 08:49:37 GMT").expiresAt == N + 100);   // Expires ignored
        // custom policy
        tc::ExpiryPolicy pol;
        pol.defaultTtlSec = 100;
        pol.maxTtlSec = 200;
        CHECK(tc::expiryFromHeaders(H(""), N, pol).expiresAt == N + 100);
        CHECK(tc::expiryFromHeaders(H("max-age=500"), N, pol).expiresAt == N + 200);
    }

    // --- keys and paths ----------------------------------------------------------------------------------------------------------------
    {
        tc::TileCache c(base / "keys");
        auto bad = [&](tc::TileKey k) {
            const bool a = !tc::keyValid(k);
            const bool b = !c.pathFor(k).has_value();
            const bool d = c.put(k, png(100), H("max-age=60"), N) == tc::PutResult::BadKey;
            const bool e = c.get(k, N).state == tc::GetState::Miss;
            return a && b && d && e;
        };
        CHECK(bad({"", 1, 0, 0}));
        CHECK(bad({"OSM", 1, 0, 0}));                       // upper case
        CHECK(bad({"-osm", 1, 0, 0}));                      // must start with a letter or digit
        CHECK(bad({"osm/../../x", 1, 0, 0}));
        CHECK(bad({"..", 1, 0, 0}));
        CHECK(bad({"a/b", 1, 0, 0}));
        CHECK(bad({"a\\b", 1, 0, 0}));
        CHECK(bad({"a b", 1, 0, 0}));
        CHECK(bad({"a.b", 1, 0, 0}));
        CHECK(bad({"os\xC3\xA9", 1, 0, 0}));
        CHECK(bad({std::string("a\0b", 3), 1, 0, 0}));
        CHECK(bad({std::string(33, 'a'), 1, 0, 0}));
        CHECK(bad({"osm", -1, 0, 0}));
        CHECK(bad({"osm", 23, 0, 0}));
        CHECK(bad({"osm", 3, 8, 0}));                       // x = 2^z
        CHECK(bad({"osm", 3, 0, 8}));
        CHECK(bad({"osm", 3, -1, 0}));
        CHECK(bad({"osm", 0, 1, 0}));
        CHECK(tc::keyValid({"osm", 0, 0, 0}));
        CHECK(tc::keyValid({std::string(32, 'a'), 22, 4194303, 4194303}));
        CHECK(tc::keyValid({"a_b-c9", 22, 0, 0}));
        const auto p = c.pathFor({"osm", 5, 17, 9});
        CHECK(p && *p == (base / "keys" / "osm" / "5" / "17" / "9.tile"));
        // nothing was written outside the cache folder
        std::set<std::string> top;
        for (auto& e : fs::directory_iterator(base)) top.insert(e.path().filename().string());
        CHECK(top.size() == 1 && top.count("keys") == 1);
        CHECK(c.tileCount() == 0 && c.stats().refused >= 18);
    }

    // --- store, fresh, stale, miss, no-store, no-cache -----------------------------------------------------------------------------------
    {
        tc::TileCache c(base / "basic");
        const tc::TileKey k{"osm", 4, 8, 5};
        CHECK(c.get(k, N).state == tc::GetState::Miss);
        const auto img = png(2000, 9);
        CHECK(c.put(k, img, H("max-age=3600"), N) == tc::PutResult::Stored);
        auto g = c.get(k, N + 100);
        CHECK(g.state == tc::GetState::Fresh && g.data == img && g.expiresAt == N + 3600);
        CHECK(c.get(k, N + 3599).state == tc::GetState::Fresh);
        auto st = c.get(k, N + 3600);
        CHECK(st.state == tc::GetState::Stale && st.data == img);                  // expired: still returned, marked stale
        CHECK(c.totalBytes() == 2000 + tc::TileCache::kHeaderBytes && c.tileCount() == 1);
        // replacing keeps one entry and the new bytes and size
        const auto img2 = png(500, 3);
        CHECK(c.put(k, img2, H("max-age=60"), N + 10) == tc::PutResult::Stored);
        CHECK(c.tileCount() == 1 && c.totalBytes() == 500 + tc::TileCache::kHeaderBytes);
        CHECK(c.get(k, N + 20).data == img2);
        // no-cache: stored, stale at once; no-store: refused and a held copy is removed
        CHECK(c.put(k, img, H("no-cache"), N) == tc::PutResult::Stored);
        CHECK(c.get(k, N).state == tc::GetState::Stale);
        CHECK(c.put(k, img, H("no-store"), N) == tc::PutResult::NoStore);
        CHECK(c.tileCount() == 0 && c.get(k, N).state == tc::GetState::Miss && !fs::exists(*c.pathFor(k)));
        // payloads: only images
        CHECK(c.put(k, std::vector<unsigned char>{'<', 'h', 't', 'm', 'l', '>'}, H(""), N) == tc::PutResult::NotAnImage);
        CHECK(c.put(k, std::vector<unsigned char>(), H(""), N) == tc::PutResult::NotAnImage);
        CHECK(c.put(k, std::vector<unsigned char>{0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3}, H(""), N) == tc::PutResult::Stored);      // JPEG
        std::vector<unsigned char> webp = {'R', 'I', 'F', 'F', 1, 0, 0, 0, 'W', 'E', 'B', 'P', 0, 0};
        CHECK(c.put({"osm", 4, 8, 6}, webp, H(""), N) == tc::PutResult::Stored);
        CHECK(c.put({"osm", 4, 8, 7}, std::vector<unsigned char>{'G', 'I', 'F', '8', '9', 'a', 0, 0, 0, 0, 0, 0}, H(""), N) == tc::PutResult::NotAnImage);
        CHECK(c.stats().refused >= 3);
        // the seven-day default applies with no headers
        CHECK(c.get(k, N + 7 * 86400 - 1).state == tc::GetState::Fresh);
        CHECK(c.get(k, N + 7 * 86400).state == tc::GetState::Stale);
        CHECK(c.remove(k) && !c.remove(k) && c.get(k, N).state == tc::GetState::Miss);
    }

    // --- size cap, least recently used -----------------------------------------------------------------------------------------------------
    {
        tc::Config cfg;
        cfg.maxBytes = 3 * (1000 + 32) + 10;
        cfg.maxTileBytes = 5000;
        tc::TileCache c(base / "lru", cfg);
        auto key = [](int i) { return tc::TileKey{"osm", 6, i, 0}; };
        CHECK(c.put(key(1), png(1000), H("max-age=999"), N) == tc::PutResult::Stored);
        CHECK(c.put(key(2), png(1000), H("max-age=999"), N) == tc::PutResult::Stored);
        CHECK(c.put(key(3), png(1000), H("max-age=999"), N) == tc::PutResult::Stored);
        CHECK(c.tileCount() == 3);
        CHECK(c.get(key(1), N).state == tc::GetState::Fresh);                       // 1 is now the most recently used; 2 is the oldest
        CHECK(c.put(key(4), png(1000), H("max-age=999"), N) == tc::PutResult::Stored);
        CHECK(c.tileCount() == 3 && c.totalBytes() <= cfg.maxBytes);
        CHECK(c.get(key(2), N).state == tc::GetState::Miss);                        // the least recently used went
        CHECK(c.get(key(1), N).state == tc::GetState::Fresh && c.get(key(3), N).state == tc::GetState::Fresh && c.get(key(4), N).state == tc::GetState::Fresh);
        CHECK(!fs::exists(*c.pathFor(key(2))));
        CHECK(c.stats().evicted == 1);
        // a tile bigger than the per-tile limit or the whole cache is refused and nothing is evicted for it
        CHECK(c.put(key(5), png(6000), H(""), N) == tc::PutResult::TooLarge);
        CHECK(c.tileCount() == 3);
        tc::Config tiny;
        tiny.maxBytes = 100;
        tc::TileCache t(base / "tiny", tiny);
        CHECK(t.put(key(1), png(200), H(""), N) == tc::PutResult::TooLarge);
        // a burst of random puts never exceeds the cap
        std::mt19937 rng(1);
        for (int i = 0; i < 500; ++i) {
            c.put(key(static_cast<int>(rng() % 60)), png(500 + rng() % 3000), H("max-age=60"), N);
            CHECK(c.totalBytes() <= cfg.maxBytes);
        }
        std::uint64_t onDisk = 0;
        std::size_t files = 0;
        for (auto& e : fs::recursive_directory_iterator(base / "lru"))
            if (e.is_regular_file()) { onDisk += e.file_size(); ++files; }
        CHECK(onDisk == c.totalBytes() && files == c.tileCount());                   // the index and the disk agree
    }

    // --- restart: the index is rebuilt and the LRU order survives ---------------------------------------------------------------------------
    {
        const fs::path dir = base / "restart";
        tc::Config cfg;
        cfg.maxBytes = 3 * (1000 + 32) + 10;
        {
            tc::TileCache c(dir, cfg);
            for (int i = 1; i <= 3; ++i) c.put({"osm", 6, i, 0}, png(1000, static_cast<unsigned char>(i)), H("max-age=999"), N);
        }
        // make tile 2 the oldest and tile 1 the newest by file time
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(dir / "osm" / "6" / "2" / "0.tile", now - std::chrono::hours(3));
        fs::last_write_time(dir / "osm" / "6" / "3" / "0.tile", now - std::chrono::hours(2));
        fs::last_write_time(dir / "osm" / "6" / "1" / "0.tile", now - std::chrono::hours(1));
        // stray temp file and an unrelated file in the tree
        spit(dir / "osm" / "6" / "1" / "7.tile.tmp", "half written");
        spit(dir / "notes.txt", "keep me");
        tc::TileCache c(dir, cfg);
        CHECK(c.tileCount() == 3);
        CHECK(!fs::exists(dir / "osm" / "6" / "1" / "7.tile.tmp"));
        CHECK(fs::exists(dir / "notes.txt"));
        CHECK(c.get({"osm", 6, 3, 0}, N).data.size() == 1000);
        CHECK(c.put({"osm", 6, 9, 0}, png(1000), H("max-age=999"), N) == tc::PutResult::Stored);
        CHECK(c.get({"osm", 6, 2, 0}, N).state == tc::GetState::Miss);              // the oldest by file time went first
        CHECK(c.get({"osm", 6, 1, 0}, N).state == tc::GetState::Fresh);
        // opening with a smaller cap trims at once
        tc::Config small;
        small.maxBytes = 1000 + 32 + 5;
        tc::TileCache s(dir, small);
        CHECK(s.tileCount() == 1 && s.totalBytes() <= small.maxBytes);
    }

    // --- corrupted files are tolerated ---------------------------------------------------------------------------------------------------------
    {
        const fs::path dir = base / "corrupt";
        tc::TileCache c(dir);
        const tc::TileKey k{"osm", 6, 1, 1};
        CHECK(c.put(k, png(1500, 4), H("max-age=999"), N) == tc::PutResult::Stored);
        const fs::path f = *c.pathFor(k);
        const std::string good = slurp(f);
        auto damage = [&](const std::string& bytes) {
            spit(f, bytes);
            const auto g = c.get(k, N);
            const bool miss = g.state == tc::GetState::Miss && g.data.empty();
            const bool removed = !fs::exists(f);
            c.put(k, png(1500, 4), H("max-age=999"), N);
            return miss && removed;
        };
        std::string flip = good;
        flip[100] ^= 0x01;
        CHECK(damage(flip));                                              // one payload bit: CRC catches it
        CHECK(damage(good.substr(0, good.size() - 1)));                   // truncated by a byte
        CHECK(damage(good.substr(0, 20)));                                // truncated inside the header
        CHECK(damage(""));                                                // empty file
        CHECK(damage(good + "x"));                                        // trailing byte
        std::string badMagic = good;
        badMagic[0] = 'Z';
        CHECK(damage(badMagic));
        std::string bigSize = good;
        bigSize[27] = static_cast<char>(0xFF);                            // an absurd payload size
        CHECK(damage(bigSize));
        CHECK(damage(std::string(1000, '\0')));
        CHECK(c.stats().corrupt == 8);
        // corrupt files found at open are deleted
        spit(f, flip);
        tc::TileCache again(dir);
        CHECK(again.tileCount() == 0 && !fs::exists(f) && again.stats().corrupt == 1);
    }

    // --- concurrency -----------------------------------------------------------------------------------------------------------------------------
    {
        tc::Config cfg;
        cfg.maxBytes = 40 * (800 + 32);
        tc::TileCache c(base / "threads", cfg);
        std::atomic<int> okGets{0};
        std::vector<std::thread> th;
        for (int t = 0; t < 4; ++t)
            th.emplace_back([&, t] {
                std::mt19937 rng(50 + t);
                for (int i = 0; i < 400; ++i) {
                    const tc::TileKey k{"osm", 7, static_cast<int>(rng() % 30), static_cast<int>(rng() % 3)};
                    if (rng() % 2) c.put(k, png(800, static_cast<unsigned char>(k.x)), H("max-age=60"), N);
                    else {
                        const auto g = c.get(k, N);
                        if (g.state == tc::GetState::Fresh) {
                            if (g.data.size() == 800 && g.data[100] == static_cast<unsigned char>(k.x)) ++okGets;
                        }
                    }
                }
            });
        for (auto& t : th) t.join();
        CHECK(c.totalBytes() <= cfg.maxBytes);
        CHECK(okGets > 0);
        CHECK(c.stats().corrupt == 0);                                     // no torn files were ever read
    }

    // --- there is no fetching code in the header ----------------------------------------------------------------------------------------------------
    {
        std::string here = __FILE__;
        const std::size_t cut = here.find_last_of("/\\");
        const fs::path hdr = fs::path(here.substr(0, cut)) / ".." / "src" / "core" / "tile_cache.hpp";
        const std::string text = slurp(hdr);
        if (text.empty()) {
            std::printf("SKIP header scan: %s not readable from this build layout\n", hdr.string().c_str());
        } else {
            for (const char* banned : {"httplib", "winsock", "WinHttp", "wininet", "curl", "<netdb", "socket(", "http://", "https://"}) {
                std::size_t at = 0;
                bool inCode = false;
                while ((at = text.find(banned, at)) != std::string::npos) {
                    // a mention inside a comment line (// ...) is fine; a hit on a non-comment line is not
                    const std::size_t ls = text.rfind('\n', at);
                    const std::string line = text.substr(ls == std::string::npos ? 0 : ls + 1, at - (ls == std::string::npos ? 0 : ls + 1));
                    if (line.find("//") == std::string::npos) inCode = true;
                    at += 1;
                }
                CHECK(!inCode);
            }
        }
    }

    fs::remove_all(base);
    return testSummary("test_tile_cache");
}
