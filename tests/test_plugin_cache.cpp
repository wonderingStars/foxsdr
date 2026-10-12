// Tests for the plugin store's two on-disk caches (0.99.72), both in core/plugin_repo.{hpp,cpp}:
//
//   THE CATALOGUE CACHE. The index the last good read produced is kept beside
//   installed.json as catalogue.json with the time of the read in
//   catalogue.json.time, so the store opens populated and a failed refresh no longer
//   leaves it empty. Proved here: it is written after a good read and read back with
//   its time; it is NEVER written from text the client refused (a refused index must
//   not be what the next start offers); a corrupt cache is ignored with its reason
//   and not touched; a write that cannot finish leaves the old cache whole; and the
//   plugin folder's signature does not count the cache files (counted, every fetch
//   looked like a changed folder and the rescan it exists to skip ran after all).
//
//   THE SCREENSHOT CACHE. A plugin's pictures are fetched over https only, capped at
//   4 MiB, hashed while they stream, and kept as store-cache/<sha256>.png. Proved
//   here, against a stand-in for the transport (PluginRepo::setTransportForTest -
//   the same seam test_health_paths uses; the cap, the hash, the temporary file and
//   the rename all run for real): a cached picture is returned with NO request; a
//   cached file that is not the picture its name says is replaced; a picture over the
//   cap is refused, one exactly at it is not; a digest that is not the catalogue's is
//   refused and nothing is left on disk; an http URL is refused before any request or
//   any file; and pruning removes what the catalogue no longer names - and only that.
//
// THE GAP, stated: the real WinHTTP transport (TLS, the 4 s / 15 s timeouts, the
// redirect rule) is not exercised, as for every other transfer in this module; what
// the timeouts are is read off the code, not measured.
//
// Temp policy as test_plugin_repo.cpp: per-case directories with the process id in
// the name, removed on success.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_repo.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/plugin_dir_signature.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::CatalogScreenshot;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginDirSignature;
using cascade::core::PluginRepo;

namespace {

fs::path g_root;

fs::path freshDir(const char* tag) {
    const fs::path d = g_root / tag;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void writeBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// The names of the regular files directly in `d`, sorted. Empty for a directory that is
// not there.
std::vector<std::string> filesIn(const fs::path& d) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec)) { out.push_back(it->path().filename().string()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string sha256Of(const std::string& bytes) {
    std::string hex;
    std::string err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

std::string abiText() {
    return std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
}

// A small, valid index with two plugins, one with a picture.
std::string indexText(const std::string& pictureSha = std::string(64, 'a')) {
    return std::string("{\"schemaVersion\":1,\"plugins\":["
                       "{\"id\":\"adsb\",\"name\":\"ADS-B\",\"version\":\"1.8.1\",\"abiVersion\":") +
           abiText() +
           ",\"category\":\"aircraft\",\"screenshots\":[{\"url\":\"https://example.invalid/a.png\","
           "\"sha256\":\"" + pictureSha + "\",\"caption\":\"the map\"}]},"
           "{\"id\":\"apt\",\"name\":\"NOAA APT\",\"version\":\"1.0.0\",\"abiVersion\":" +
           abiText() + "}]}";
}

// The transport stand-in. One per case; `hits` counts requests, `urls` records them.
struct Served {
    std::string body;
    std::size_t chunk = 64 * 1024;  // how the body is handed to the sink
    bool fail = false;
    std::string error = "the server returned HTTP 404";
    std::atomic<int> hits{0};
    std::string lastUrl;
};

void serve(const std::shared_ptr<Served>& s) {
    PluginRepo::setTransportForTest(
        [s](const std::string& url, std::uint64_t, const std::function<bool(const void*, std::size_t)>& sink,
            std::string& error) {
            ++s->hits;
            s->lastUrl = url;
            if (s->fail) {
                error = s->error;
                return false;
            }
            for (std::size_t at = 0; at < s->body.size(); at += s->chunk) {
                const std::size_t n = std::min(s->chunk, s->body.size() - at);
                if (!sink(s->body.data() + at, n)) { return false; }
            }
            return true;
        });
}

CatalogScreenshot shotFor(const std::string& bytes, const std::string& url = "https://example.invalid/p.png") {
    CatalogScreenshot s;
    s.url = url;
    s.sha256 = sha256Of(bytes);
    s.caption = "a picture";
    return s;
}

// Bytes that look like a PNG's start and are otherwise filler of the size asked for.
std::string pngLike(std::size_t size, char fill = 'x') {
    std::string b = "\x89PNG\r\n\x1a\n";
    if (size > b.size()) { b.append(size - b.size(), fill); }
    b.resize(size);
    return b;
}

// --- the catalogue cache ---------------------------------------------------------------

void testCatalogueCache() {
    const std::string text = indexText();

    // WRITTEN AFTER A GOOD READ, and read back with its time.
    {
        const fs::path d = freshDir("cat_roundtrip");
        std::string err = "stale";
        CHECK(PluginRepo::saveCatalogueCache(d.string(), text, 1760000000, err));
        CHECK(err.empty());
        CHECK(fs::is_regular_file(d / PluginRepo::catalogueCacheFileName()));
        CHECK(fs::is_regular_file(d / PluginRepo::catalogueCacheTimeFileName()));
        // The text exactly as read, and nothing else in the folder (no .part).
        CHECK(readBytes(d / PluginRepo::catalogueCacheFileName()) == text);
        CHECK(filesIn(d) == (std::vector<std::string>{"catalogue.json", "catalogue.json.time"}));

        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = -1;
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(err.empty());
        CHECK(when == 1760000000);
        CHECK(entries.size() == 2u);
        if (entries.size() == 2u) {
            CHECK(entries[0].id == "adsb");
            CHECK(entries[0].category == "aircraft");
            CHECK(entries[0].screenshots.size() == 1u);
            CHECK(entries[1].id == "apt");
        }
        // The same entries a direct parse gives.
        std::vector<PluginCatalogEntry> direct;
        CHECK(PluginRepo::parseIndex(text, direct, err));
        CHECK(direct.size() == entries.size());

        // THE TIME ROUND-TRIPS for any value, including "not recorded" (0) and one far out.
        for (const std::int64_t t : {std::int64_t{0}, std::int64_t{1}, std::int64_t{1760000123},
                                     std::int64_t{4102444800}}) {
            CHECK(PluginRepo::saveCatalogueCache(d.string(), text, t, err));
            std::int64_t back = -1;
            CHECK(PluginRepo::loadCachedIndex(d.string(), entries, back, err));
            CHECK(back == t);
        }
    }

    // NEVER AFTER A REFUSED ONE. Text the client refuses is not kept, in a folder with no
    // cache (nothing appears) and in one that has a good cache (it stays exactly as it was).
    {
        const fs::path d = freshDir("cat_refused");
        std::string err;
        for (const std::string& bad :
             {std::string("this is not json"), std::string("{\"schemaVersion\":2,\"plugins\":[]}"),
              std::string("{\"schemaVersion\":1}"),
              std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"x\"}]}"), std::string()}) {
            err.clear();
            CHECK(!PluginRepo::saveCatalogueCache(d.string(), bad, 1760000000, err));
            CHECK(!err.empty());
            CHECK(filesIn(d).empty());
        }
        // Over the index cap is refused as well, before any file.
        {
            std::string huge = text;
            huge.append(static_cast<std::size_t>(PluginRepo::kMaxIndexBytes), ' ');
            err.clear();
            CHECK(!PluginRepo::saveCatalogueCache(d.string(), huge, 1760000000, err));
            CHECK(contains(err, "limit"));
            CHECK(filesIn(d).empty());
        }
        CHECK(PluginRepo::saveCatalogueCache(d.string(), text, 1760000000, err));
        CHECK(!PluginRepo::saveCatalogueCache(d.string(), "garbage", 1760009999, err));
        CHECK(readBytes(d / PluginRepo::catalogueCacheFileName()) == text);
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 0;
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(when == 1760000000);  // the time did not move either
    }

    // THROUGH fetchIndex: a good read keeps its text, a refused one keeps nothing, and what
    // the app writes is lastIndexText() only.
    {
        const fs::path d = freshDir("cat_fetch");
        auto s = std::make_shared<Served>();
        serve(s);
        PluginRepo repo;
        std::string err;
        s->body = text;
        CHECK(repo.fetchIndex("https://example.invalid/index.json", err));
        CHECK(repo.lastIndexText() == text);
        CHECK(PluginRepo::saveCatalogueCache(d.string(), repo.lastIndexText(), 1760000000, err));

        s->body = "<html>not an index</html>";
        CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
        CHECK(repo.lastIndexText().empty());  // a refused index leaves nothing to keep
        CHECK(repo.entries().empty());
        // The app would not call save with nothing; if it did, nothing is written.
        CHECK(!PluginRepo::saveCatalogueCache(d.string(), repo.lastIndexText(), 1760009999, err));
        CHECK(readBytes(d / PluginRepo::catalogueCacheFileName()) == text);

        // The server failing is no different.
        s->fail = true;
        CHECK(!repo.fetchIndex("https://example.invalid/index.json", err));
        CHECK(repo.lastIndexText().empty());
        PluginRepo::setTransportForTest(nullptr);
    }

    // A CORRUPT CACHE IS IGNORED WITH ITS REASON - and left where it is.
    {
        const fs::path d = freshDir("cat_corrupt");
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 77;
        std::string err;
        // No cache at all.
        CHECK(!PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(err == "no catalogue has been kept yet");
        CHECK(entries.empty() && when == 0);
        // Not JSON.
        writeBytes(d / "catalogue.json", "{ this is not json");
        writeBytes(d / "catalogue.json.time", "1760000000\n");
        CHECK(!PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(contains(err, "not usable"));
        CHECK(contains(err, "not valid JSON"));
        CHECK(entries.empty());
        CHECK(readBytes(d / "catalogue.json") == "{ this is not json");  // not repaired, not deleted
        // JSON, but a document this client refuses (a different schema).
        writeBytes(d / "catalogue.json", "{\"schemaVersion\":9,\"plugins\":[]}");
        CHECK(!PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(contains(err, "schemaVersion"));
        CHECK(entries.empty());
        // A folder where the file should be.
        fs::remove(d / "catalogue.json");
        fs::create_directories(d / "catalogue.json");
        CHECK(!PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(!err.empty());
        CHECK(entries.empty());
        fs::remove_all(d / "catalogue.json");
    }

    // THE TIME FILE costs the date, never the list.
    {
        const fs::path d = freshDir("cat_time");
        writeBytes(d / "catalogue.json", text);
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 5;
        std::string err;
        // Absent.
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(entries.size() == 2u && when == 0);
        // Garbage, negative, empty, too long to be a time.
        for (const char* bad : {"abc\n", "-5\n", "", "\n", "17600 00000\n", "99999999999999999999\n"}) {
            writeBytes(d / "catalogue.json.time", bad);
            when = 5;
            CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
            CHECK(entries.size() == 2u);
            CHECK(when == 0);
        }
        // CRLF, as a text editor leaves it.
        writeBytes(d / "catalogue.json.time", "1760000000\r\n");
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(when == 1760000000);
    }

    // A WRITE THAT CANNOT FINISH LEAVES THE OLD CACHE WHOLE: with a folder squatting on the
    // ".part" name the new text cannot be staged, and the old pair is exactly as it was.
    {
        const fs::path d = freshDir("cat_atomic");
        std::string err;
        CHECK(PluginRepo::saveCatalogueCache(d.string(), text, 1760000000, err));
        fs::create_directories(d / "catalogue.json.part");
        const std::string newer = indexText(std::string(64, 'b'));
        CHECK(!PluginRepo::saveCatalogueCache(d.string(), newer, 1760005555, err));
        CHECK(!err.empty());
        CHECK(readBytes(d / "catalogue.json") == text);
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 0;
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(when == 1760000000);
        fs::remove_all(d / "catalogue.json.part");
        // ...and once the squatter is gone, the same call succeeds and replaces both.
        CHECK(PluginRepo::saveCatalogueCache(d.string(), newer, 1760005555, err));
        CHECK(readBytes(d / "catalogue.json") == newer);
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(when == 1760005555);
    }

    // THE PLUGIN FOLDER'S SIGNATURE DOES NOT COUNT THE CACHE (and still counts everything
    // else): a fetch that rewrote catalogue.json is not a changed folder.
    {
        const fs::path d = freshDir("cat_signature");
        writeBytes(d / "adsb-1.0.0.dll", "module");
        writeBytes(d / "installed.json", "{}");
        PluginDirSignature before;
        CHECK(cascade::core::readPluginDirSignature(d.string(), before));
        CHECK(before.files.size() == 2u);
        std::string err;
        CHECK(PluginRepo::saveCatalogueCache(d.string(), text, 1760000000, err));
        writeBytes(d / "catalogue.json.part", "debris");
        PluginDirSignature after;
        CHECK(cascade::core::readPluginDirSignature(d.string(), after));
        CHECK(after == before);
        CHECK(after.find("catalogue.json") == nullptr);
        CHECK(after.find("catalogue.json.time") == nullptr);
        // A rewrite with other bytes and another time still is not a change...
        CHECK(PluginRepo::saveCatalogueCache(d.string(), indexText(std::string(64, 'c')), 1760009999, err));
        PluginDirSignature rewritten;
        CHECK(cascade::core::readPluginDirSignature(d.string(), rewritten));
        CHECK(rewritten == before);
        // ...while any other file in the folder is, whatever its name.
        for (const char* other : {"catalogue.json2", "my-catalogue.json", "catalogue.txt", "notes.json"}) {
            writeBytes(d / other, "x");
            PluginDirSignature changed;
            CHECK(cascade::core::readPluginDirSignature(d.string(), changed));
            CHECK(changed != before);
            CHECK(changed.find(other) != nullptr);
            fs::remove(d / other);
        }
    }
}

// --- the screenshot cache --------------------------------------------------------------

void testScreenshotPaths() {
    const std::string sha(64, 'a');
    const std::string dir = (g_root / "paths").string();
    const std::string p = PluginRepo::screenshotCachePath(dir, sha);
    CHECK(p == (fs::path(dir) / "store-cache" / (sha + ".png")).string());
    CHECK(fs::path(p).filename().string() == sha + ".png");
    CHECK(fs::path(p).parent_path().filename().string() == PluginRepo::screenshotCacheDirName());
    CHECK(std::string(PluginRepo::screenshotCacheDirName()) == "store-cache");
    // Case is folded, so a catalogue's capitals and a cache's lower case are one file.
    CHECK(PluginRepo::screenshotCachePath(dir, std::string(64, 'A')) == p);
    // A digest that is not 64 hexadecimal digits names no file - never a path that could leave the folder.
    for (const std::string& bad : {std::string(), std::string(63, 'a'), std::string(65, 'a'),
                                   std::string(63, 'a') + "g", std::string("../../etc/passwd"),
                                   std::string(60, 'a') + "..\\x"}) {
        CHECK(PluginRepo::screenshotCachePath(dir, bad).empty());
    }
}

void testScreenshotFetch() {
    // FETCHED, HASHED, KEPT - and the second ask costs no request.
    {
        const fs::path d = freshDir("shot_fetch");
        auto s = std::make_shared<Served>();
        serve(s);
        s->body = pngLike(300 * 1000);
        const CatalogScreenshot pic = shotFor(s->body, "https://raw.example.invalid/screenshots/adsb/1.png");
        PluginRepo repo;
        std::string local;
        std::string err = "stale";
        CHECK(repo.fetchScreenshot(d.string(), pic, local, err));
        CHECK(err.empty());
        CHECK(s->hits == 1);
        CHECK(s->lastUrl == pic.url);  // the request is the URL and nothing else
        CHECK(local == PluginRepo::screenshotCachePath(d.string(), pic.sha256));
        CHECK(readBytes(local) == s->body);
        CHECK(filesIn(d / "store-cache") == (std::vector<std::string>{pic.sha256 + ".png"}));  // no .part

        // THE CACHE HIT: a transport that would fail is never asked, the path comes back.
        s->fail = true;
        std::string again;
        CHECK(repo.fetchScreenshot(d.string(), pic, again, err));
        CHECK(again == local);
        CHECK(s->hits == 1);

        // A cached file that is NOT the picture its name says (damaged, or put there) is not
        // believed: it is replaced from the network.
        s->fail = false;
        writeBytes(local, "not the picture");
        std::string healed;
        CHECK(repo.fetchScreenshot(d.string(), pic, healed, err));
        CHECK(s->hits == 2);
        CHECK(readBytes(healed) == s->body);
        PluginRepo::setTransportForTest(nullptr);
    }

    // THE CAP: one byte over 4 MiB is refused and nothing is left on disk; exactly 4 MiB is kept.
    {
        const fs::path d = freshDir("shot_cap");
        auto s = std::make_shared<Served>();
        serve(s);
        PluginRepo repo;
        std::string local;
        std::string err;

        const std::size_t cap = static_cast<std::size_t>(PluginRepo::kMaxScreenshotBytes);
        CHECK(std::to_string(cap) == "4194304");  // 4 MiB, the number the privacy note states
        s->body = pngLike(cap + 1);
        CatalogScreenshot big = shotFor(s->body);
        CHECK(!repo.fetchScreenshot(d.string(), big, local, err));
        CHECK(contains(err, "larger than 4 MiB"));
        CHECK(local.empty());
        CHECK(filesIn(d / "store-cache").empty());
        // The catalogue's own claim about the size is not what decides: lie small, still refused.
        big.sizeBytes = 10;
        CHECK(!repo.fetchScreenshot(d.string(), big, local, err));
        CHECK(filesIn(d / "store-cache").empty());
        // Handed over in one piece rather than chunks: the same.
        s->chunk = cap + 1;
        CHECK(!repo.fetchScreenshot(d.string(), big, local, err));
        CHECK(filesIn(d / "store-cache").empty());

        s->chunk = 64 * 1024;
        s->body = pngLike(cap);
        const CatalogScreenshot exact = shotFor(s->body);
        CHECK(repo.fetchScreenshot(d.string(), exact, local, err));
        CHECK(fs::file_size(local) == cap);
        PluginRepo::setTransportForTest(nullptr);
    }

    // A DIGEST THAT IS NOT THE CATALOGUE'S: refused, and nothing is left on disk.
    {
        const fs::path d = freshDir("shot_sha");
        auto s = std::make_shared<Served>();
        serve(s);
        PluginRepo repo;
        std::string local;
        std::string err;
        const std::string promised = pngLike(5000, 'a');
        s->body = pngLike(5000, 'b');  // same size, other bytes
        const CatalogScreenshot pic = shotFor(promised);
        CHECK(!repo.fetchScreenshot(d.string(), pic, local, err));
        CHECK(s->hits == 1);
        CHECK(contains(err, "integrity"));
        CHECK(contains(err, pic.sha256));            // what was expected
        CHECK(contains(err, sha256Of(s->body)));      // what arrived
        CHECK(local.empty());
        CHECK(filesIn(d / "store-cache").empty());
        // A good picture already cached under ANOTHER digest is not disturbed by it.
        s->body = pngLike(4000, 'g');
        const CatalogScreenshot good = shotFor(s->body);
        CHECK(repo.fetchScreenshot(d.string(), good, local, err));
        s->body = pngLike(5000, 'b');
        CHECK(!repo.fetchScreenshot(d.string(), pic, local, err));
        CHECK(filesIn(d / "store-cache") == (std::vector<std::string>{good.sha256 + ".png"}));
        PluginRepo::setTransportForTest(nullptr);
    }

    // HTTPS ONLY, and a well-formed digest, both BEFORE any request or any file.
    {
        const fs::path d = freshDir("shot_gate");
        auto s = std::make_shared<Served>();
        serve(s);
        s->body = pngLike(1000);
        PluginRepo repo;
        std::string local;
        std::string err;
        CatalogScreenshot pic = shotFor(s->body, "http://raw.example.invalid/p.png");
        CHECK(!repo.fetchScreenshot(d.string(), pic, local, err));
        CHECK(contains(err, "non-https"));
        for (const char* url : {"ftp://example.invalid/p.png", "file:///C:/p.png", "", "https://"}) {
            pic.url = url;
            CHECK(!repo.fetchScreenshot(d.string(), pic, local, err));
        }
        pic.url = "https://example.invalid/p.png";
        pic.sha256 = "short";
        CHECK(!repo.fetchScreenshot(d.string(), pic, local, err));
        CHECK(contains(err, "64 hexadecimal"));
        CHECK(!repo.fetchScreenshot("", shotFor(s->body), local, err));
        CHECK(s->hits == 0);                         // no request, ever
        CHECK(!fs::exists(d / "store-cache"));       // and no folder either
        PluginRepo::setTransportForTest(nullptr);
    }

    // A SERVER THAT FAILS leaves nothing, and says why in its own words.
    {
        const fs::path d = freshDir("shot_fail");
        auto s = std::make_shared<Served>();
        serve(s);
        s->fail = true;
        PluginRepo repo;
        std::string local;
        std::string err;
        CHECK(!repo.fetchScreenshot(d.string(), shotFor(pngLike(100)), local, err));
        CHECK(err == "the server returned HTTP 404");
        CHECK(local.empty());
        CHECK(filesIn(d / "store-cache").empty());
        PluginRepo::setTransportForTest(nullptr);
    }
}

void testScreenshotPrune() {
    const fs::path d = freshDir("shot_prune");
    const fs::path cache = d / "store-cache";
    fs::create_directories(cache);
    const std::string a(64, 'a');
    const std::string b(64, 'b');
    const std::string c(64, 'c');
    for (const std::string& sha : {a, b, c}) { writeBytes(cache / (sha + ".png"), "picture " + sha.substr(0, 1)); }
    writeBytes(cache / (std::string(64, 'd') + ".png.part"), "debris");
    writeBytes(cache / "notes.txt", "mine");
    writeBytes(cache / "logo.png", "mine too");  // not a digest name: not ours
    writeBytes(cache / (std::string(63, 'e') + ".png"), "one digit short: not ours");

    const auto entry = [](const std::string& id, std::vector<std::string> shas) {
        PluginCatalogEntry e;
        e.id = id;
        for (const std::string& s : shas) {
            CatalogScreenshot p;
            p.url = "https://example.invalid/p.png";
            p.sha256 = s;
            p.caption = "x";
            e.screenshots.push_back(p);
        }
        return e;
    };

    // The catalogue names a (in one plugin) and C in capitals (in another): b and the debris go.
    {
        const std::vector<PluginCatalogEntry> cat{entry("one", {a}), entry("two", {std::string(64, 'C')}),
                                                  entry("none", {})};
        const std::vector<std::string> removed = PluginRepo::pruneScreenshotCache(d.string(), cat);
        CHECK(removed == (std::vector<std::string>{b + ".png", std::string(64, 'd') + ".png.part"}));
        CHECK(filesIn(cache) == (std::vector<std::string>{a + ".png", c + ".png", std::string(63, 'e') + ".png",
                                                          "logo.png", "notes.txt"}));
    }
    // Nothing more to remove the second time.
    CHECK(PluginRepo::pruneScreenshotCache(d.string(), {entry("one", {a}), entry("two", {c})}).empty());
    // A catalogue that names none removes every picture - and still only pictures.
    {
        const std::vector<std::string> removed = PluginRepo::pruneScreenshotCache(d.string(), {});
        CHECK(removed == (std::vector<std::string>{a + ".png", c + ".png"}));
        CHECK(filesIn(cache) == (std::vector<std::string>{std::string(63, 'e') + ".png", "logo.png", "notes.txt"}));
    }
    // No cache folder: nothing to do, and none is made.
    const fs::path other = freshDir("shot_prune_none");
    CHECK(PluginRepo::pruneScreenshotCache(other.string(), {entry("one", {a})}).empty());
    CHECK(!fs::exists(other / "store-cache"));
}

// A plugin that comes from the regional list shows no pictures: a picture is fetched from the
// address its entry names, and the store's only other address is the public catalogue's origin
// (PRIVACY.md). The public entries beside it keep theirs.
void testRegionalEntriesCarryNoPictures() {
    const auto withPicture = [](const std::string& id, const std::string& url) {
        PluginCatalogEntry e;
        e.id = id;
        e.name = id;
        e.version = "1.0.0";
        e.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
        e.compatible = true;
        cascade::core::PluginPlatform p;
        p.os = PluginRepo::hostOs();
        p.arch = PluginRepo::hostArch();
        p.file = id + "-1.0.0.dll";
        p.url = url;
        p.sha256 = std::string(64, 'a');
        e.platforms.push_back(p);
        CatalogScreenshot s;
        s.url = "https://elsewhere.example.invalid/" + id + ".png";
        s.sha256 = std::string(64, 'b');
        s.caption = "a picture";
        e.screenshots.push_back(s);
        return e;
    };
    const std::string prefix = PluginRepo::regionalDownloadPrefix();
    const PluginCatalogEntry pub = withPicture("pub", "https://example.invalid/pub-1.0.0.dll");
    const PluginCatalogEntry reg = withPicture("radar", prefix + "us/radar-1.0.0.dll");
    const PluginRepo::RegionalMergeResult r = PluginRepo::mergeRegional({pub}, {reg}, prefix);
    CHECK(r.added == 1);
    CHECK(r.merged.size() == 2u);
    if (r.merged.size() == 2u) {
        CHECK(r.merged[0].id == "pub" && r.merged[0].screenshots.size() == 1u);
        CHECK(r.merged[1].id == "radar" && r.merged[1].regional);
        CHECK(r.merged[1].screenshots.empty());
    }
}

// --- a plugin site's kept copy (0.99.74) ------------------------------------------------
//
// Each site the user adds is kept as catalogue-<16 hex of sha256(address)>.json beside a .time,
// exactly as the official catalogue is kept as catalogue.json: written after a good read, never
// from text the client refused, loaded at start-up, deleted when the site is removed - and, like
// the official copy, NOT counted by the plugin folder's signature (counted, every fetch with a
// site added would look like a changed folder and the rescan it exists to skip would run after all).
void testSiteKeptCopy() {
    const std::string siteA = "https://hobbyist.example/hdradio/index.json";
    const std::string siteB = "https://other.example/index.json";
    const std::string text = indexText();
    const std::string jsonA = PluginRepo::siteCacheFileName(siteA);
    const std::string timeA = PluginRepo::siteCacheTimeFileName(siteA);
    const std::string jsonB = PluginRepo::siteCacheFileName(siteB);
    CHECK(!jsonA.empty() && !timeA.empty() && jsonA != jsonB);

    // Written after a good read, read back with its time, under its own names - and the
    // official kept copy beside it is a different pair.
    {
        const fs::path d = freshDir("site_roundtrip");
        std::string err;
        CHECK(PluginRepo::saveSiteCache(d.string(), siteA, text, 1790000123, err));
        CHECK(err.empty());
        std::vector<std::string> expectedFiles{jsonA, timeA};
        std::sort(expectedFiles.begin(), expectedFiles.end());
        CHECK(filesIn(d) == expectedFiles);
        CHECK(readBytes(d / jsonA) == text);
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 0;
        CHECK(PluginRepo::loadSiteCache(d.string(), siteA, entries, when, err));
        CHECK(entries.size() == 2u);
        CHECK(when == 1790000123);
        // Entries come back as parseIndex() read them: `source` is the merge's to give.
        for (const PluginCatalogEntry& e : entries) { CHECK(e.source.empty()); }
        // Another site's copy is not this one's.
        CHECK(!PluginRepo::loadSiteCache(d.string(), siteB, entries, when, err));
        CHECK(entries.empty());
        CHECK(contains(err, "no catalogue has been kept yet"));
        // ...and the official copy is untouched by either.
        CHECK(PluginRepo::saveCatalogueCache(d.string(), indexText(std::string(64, 'c')), 1790000999, err));
        CHECK(PluginRepo::loadSiteCache(d.string(), siteA, entries, when, err));
        CHECK(when == 1790000123);
        CHECK(PluginRepo::loadCachedIndex(d.string(), entries, when, err));
        CHECK(when == 1790000999);
    }

    // NEVER FROM TEXT THE CLIENT REFUSED, and never over the site cap: nothing is written.
    {
        const fs::path d = freshDir("site_refused");
        std::string err;
        CHECK(!PluginRepo::saveSiteCache(d.string(), siteA, "{ not json", 1790000123, err));
        CHECK(contains(err, "not kept"));
        CHECK(!PluginRepo::saveSiteCache(d.string(), siteA, std::string(), 1790000123, err));
        std::string huge = text;
        huge.append(static_cast<std::size_t>(PluginRepo::kMaxSiteIndexBytes), ' ');
        CHECK(!PluginRepo::saveSiteCache(d.string(), siteA, huge, 1790000123, err));
        CHECK(contains(err, std::to_string(PluginRepo::kMaxSiteIndexBytes)));
        // The official copy's cap is the larger one (4 MiB): the same text is kept there.
        CHECK(PluginRepo::saveCatalogueCache(d.string(), huge, 1790000123, err));
        CHECK(!PluginRepo::saveSiteCache(d.string(), std::string(), text, 1790000123, err));
        CHECK(filesIn(d) == (std::vector<std::string>{"catalogue.json", "catalogue.json.time"}));
    }

    // A corrupt copy is ignored with its reason, not repaired and not deleted.
    {
        const fs::path d = freshDir("site_corrupt");
        writeBytes(d / jsonA, "{ this is not json");
        writeBytes(d / timeA, "1790000123\n");
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 7;
        std::string err;
        CHECK(!PluginRepo::loadSiteCache(d.string(), siteA, entries, when, err));
        CHECK(entries.empty());
        CHECK(when == 0);
        CHECK(contains(err, "not usable and was ignored"));
        CHECK(readBytes(d / jsonA) == "{ this is not json");
    }

    // REMOVE deletes this site's two files (and a ".part" debris) and nothing else.
    {
        const fs::path d = freshDir("site_remove");
        std::string err;
        CHECK(PluginRepo::saveSiteCache(d.string(), siteA, text, 1790000123, err));
        CHECK(PluginRepo::saveSiteCache(d.string(), siteB, text, 1790000124, err));
        CHECK(PluginRepo::saveCatalogueCache(d.string(), text, 1790000125, err));
        writeBytes(d / (jsonA + ".part"), "debris");
        writeBytes(d / "adsb-1.0.0.dll", "module");
        writeBytes(d / "installed.json", "{}");
        CHECK(PluginRepo::removeSiteCache(d.string(), siteA));
        const std::vector<std::string> left = filesIn(d);
        CHECK(std::find(left.begin(), left.end(), jsonA) == left.end());
        CHECK(std::find(left.begin(), left.end(), timeA) == left.end());
        CHECK(std::find(left.begin(), left.end(), jsonA + ".part") == left.end());
        for (const char* kept : {"adsb-1.0.0.dll", "installed.json", "catalogue.json", "catalogue.json.time"}) {
            CHECK(std::find(left.begin(), left.end(), std::string(kept)) != left.end());
        }
        CHECK(std::find(left.begin(), left.end(), jsonB) != left.end());
        std::vector<PluginCatalogEntry> entries;
        std::int64_t when = 0;
        CHECK(!PluginRepo::loadSiteCache(d.string(), siteA, entries, when, err));
        CHECK(PluginRepo::loadSiteCache(d.string(), siteB, entries, when, err));
        // Removing a copy that is not there is not a failure.
        CHECK(PluginRepo::removeSiteCache(d.string(), siteA));
    }

    // THE FOLDER'S SIGNATURE DOES NOT COUNT A SITE'S KEPT COPY, and still counts every other file.
    {
        const fs::path d = freshDir("site_signature");
        writeBytes(d / "adsb-1.0.0.dll", "module");
        writeBytes(d / "installed.json", "{}");
        PluginDirSignature before;
        CHECK(cascade::core::readPluginDirSignature(d.string(), before));
        CHECK(before.files.size() == 2u);
        std::string err;
        CHECK(PluginRepo::saveSiteCache(d.string(), siteA, text, 1790000123, err));
        CHECK(PluginRepo::saveSiteCache(d.string(), siteB, text, 1790000124, err));
        writeBytes(d / (jsonA + ".part"), "debris");
        writeBytes(d / (timeA + ".part"), "debris");
        PluginDirSignature after;
        CHECK(cascade::core::readPluginDirSignature(d.string(), after));
        CHECK(after == before);
        CHECK(after.find(jsonA) == nullptr);
        CHECK(after.find(timeA) == nullptr);
        CHECK(after.find(jsonB) == nullptr);
        // A rewrite with other bytes and another time still is not a change...
        CHECK(PluginRepo::saveSiteCache(d.string(), siteA, indexText(std::string(64, 'c')), 1790009999, err));
        PluginDirSignature rewritten;
        CHECK(cascade::core::readPluginDirSignature(d.string(), rewritten));
        CHECK(rewritten == before);
        // ...while a file that merely looks like one is, whatever its name.
        for (const std::string& other : {std::string("catalogue-0123456789abcde.json"),     // 15 hex
                                         std::string("catalogue-0123456789ABCDEF.json"),    // upper case
                                         std::string("catalogue-0123456789abcdef.json.bak"),
                                         std::string("my-catalogue-0123456789abcdef.json"),
                                         jsonA + "x"}) {
            writeBytes(d / other, "x");
            PluginDirSignature changed;
            CHECK(cascade::core::readPluginDirSignature(d.string(), changed));
            if (changed == before) { std::printf("FAIL the signature skipped \"%s\"\n", other.c_str()); }
            CHECK(changed != before);
            CHECK(changed.find(other) != nullptr);
            fs::remove(d / other);
        }
    }
}

}  // namespace

int main() {
    std::printf("test_plugin_cache\n");
    g_root = fs::path("plugin_cache_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    testCatalogueCache();
    testSiteKeptCopy();
    testScreenshotPaths();
    testScreenshotFetch();
    testScreenshotPrune();
    testRegionalEntriesCarryNoPictures();

    PluginRepo::setTransportForTest(nullptr);
    const int rc = testSummary("test_plugin_cache");
    if (rc == 0) { fs::remove_all(g_root, ec); }
    return rc;
}
