// Error-path sweep A (wave 8): the three reports this sweep added to core/plugin_repo.cpp, each proved by making the failure
// happen against the stand-in transport (PluginRepo::setTransportForTest) and reading the anonymous count back from the health
// ledger (core/health_events.hpp), and each paired with the cases that must NOT count.
//
//   1. fetchRegionalIndex: a regional list that could not be had is counted like the public catalogue's own failure
//      (notePluginCatalogueFailed, token plug_cat). Not counted: a good list, a cancel.
//   2. fetchScreenshot: a picture that could not be had (transport refused it, not the picture the catalogue named, a refused
//      address) is counted plug_cat. Not counted: a good picture, a cache hit, a cancel.
//   3. recordInstall: a verified plugin whose record could not be made is counted plug_inst.write, for the official catalogue's
//      entries only. Not counted: a plugin SITE's entry (the 0.99.74 rule: the counter is the official catalogue's).
//
// A site's index (fetchSiteIndex) is also checked NOT to count: that is the design the allow list's reason cites.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_repo.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
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

#include "core/health_events.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::CatalogScreenshot;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginRepo;
namespace health = cascade::core::health;

namespace {

fs::path g_root;

std::uint32_t count(const char* token) {
    const auto counts = health::globalLedger()->counts();
    const auto it = counts.find(token);
    return it == counts.end() ? 0u : it->second;
}

void arm() {
    auto g = health::globalLedger();
    g->reset();
    g->arm("", cascade::core::newInstallId(), false);
}

struct Served {
    std::string body;
    bool fail = false;
    std::string error = "the server returned HTTP 503";
    std::atomic<int> hits{0};
};

void serve(const std::shared_ptr<Served>& s) {
    PluginRepo::setTransportForTest([s](const std::string&, std::uint64_t,
                                        const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
        ++s->hits;
        if (s->fail) {
            error = s->error;
            return false;
        }
        return s->body.empty() || sink(s->body.data(), s->body.size());
    });
}

std::string sha256Of(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

std::string goodIndex() {
    return std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"apt\",\"name\":\"NOAA APT\",\"version\":\"1.0.0\",\"abiVersion\":") +
           std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) + "}]}";
}

// 1 ------------------------------------------------------------------------------------------------------------------------
void testRegionalIndexIsCounted() {
    arm();
    auto s = std::make_shared<Served>();
    serve(s);
    PluginRepo repo;
    std::vector<PluginCatalogEntry> out;
    std::string err;

    // A good list: nothing counted.
    s->body = goodIndex();
    CHECK(repo.fetchRegionalIndex("https://example.invalid/regional.json", out, err));
    CHECK(out.size() == 1u);
    CHECK(count("plug_cat") == 0u);

    // The server answered an error: counted once.
    s->fail = true;
    CHECK(!repo.fetchRegionalIndex("https://example.invalid/regional.json", out, err));
    CHECK(!err.empty());
    CHECK(count("plug_cat") == 1u);

    // It answered, but not with a catalogue: counted.
    s->fail = false;
    s->body = "this is not a catalogue";
    CHECK(!repo.fetchRegionalIndex("https://example.invalid/regional.json", out, err));
    CHECK(count("plug_cat") == 2u);

    // An address that is not https is refused before any request, and counted.
    const int before = s->hits.load();
    CHECK(!repo.fetchRegionalIndex("http://example.invalid/regional.json", out, err));
    CHECK(s->hits.load() == before);
    CHECK(count("plug_cat") == 3u);

    // A cancel is not a fault: not counted.
    s->fail = true;
    s->error = "cancelled";
    CHECK(!repo.fetchRegionalIndex("https://example.invalid/regional.json", out, err));
    CHECK(err == "cancelled");
    CHECK(count("plug_cat") == 3u);

    // A plugin SITE's index failing is NOT counted (0.99.74: the counter is the official catalogue's).
    s->error = "the server returned HTTP 503";
    std::string raw;
    std::vector<std::string> report;
    CHECK(!repo.fetchSiteIndex("https://example.invalid/site.json", out, raw, report, err));
    CHECK(count("plug_cat") == 3u);
    PluginRepo::setTransportForTest(nullptr);
}

// 2 ------------------------------------------------------------------------------------------------------------------------
void testScreenshotFailureIsCounted() {
    arm();
    const fs::path dir = g_root / "shots";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto s = std::make_shared<Served>();
    serve(s);
    PluginRepo repo;
    const std::string bytes = std::string("\x89PNG\r\n\x1a\n") + std::string(64, 'x');
    CatalogScreenshot shot;
    shot.url = "https://example.invalid/p.png";
    shot.sha256 = sha256Of(bytes);
    shot.caption = "a picture";
    std::string path, err;

    // The picture arrives and is the one named: nothing counted.
    s->body = bytes;
    CHECK(repo.fetchScreenshot(dir.string(), shot, path, err));
    CHECK(count("plug_cat") == 0u);
    // ...and a second ask is a cache hit with no request: nothing counted.
    const int hits = s->hits.load();
    CHECK(repo.fetchScreenshot(dir.string(), shot, path, err));
    CHECK(s->hits.load() == hits);
    CHECK(count("plug_cat") == 0u);

    // A different picture (the cache is by digest): the transport refuses it. Counted.
    CatalogScreenshot other = shot;
    other.sha256 = sha256Of("another picture");
    s->fail = true;
    CHECK(!repo.fetchScreenshot(dir.string(), other, path, err));
    CHECK(count("plug_cat") == 1u);

    // Bytes that are not the picture the catalogue named: refused after the transfer, counted.
    s->fail = false;
    s->body = "not the picture";
    CHECK(!repo.fetchScreenshot(dir.string(), other, path, err));
    CHECK(err.find("integrity") != std::string::npos);
    CHECK(count("plug_cat") == 2u);

    // A refused address: counted.
    CatalogScreenshot plain = other;
    plain.url = "http://example.invalid/p.png";
    CHECK(!repo.fetchScreenshot(dir.string(), plain, path, err));
    CHECK(count("plug_cat") == 3u);

    // A cancel: not counted.
    s->fail = true;
    s->error = "cancelled";
    CHECK(!repo.fetchScreenshot(dir.string(), other, path, err));
    CHECK(count("plug_cat") == 3u);
    PluginRepo::setTransportForTest(nullptr);
}

// 3 ------------------------------------------------------------------------------------------------------------------------
void testRecordInstallFailureIsCounted() {
    arm();
    const std::string dir = (g_root / "record").string();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string err;

    // An official entry that cannot be recorded (no id): counted, write class.
    PluginCatalogEntry official;
    CHECK(!PluginRepo::recordInstall(dir, official, err));
    CHECK(!err.empty());
    CHECK(count("plug_inst.write") == 1u);

    // An official entry with an id but no build for this machine: counted.
    official.id = "apt";
    official.name = "NOAA APT";
    CHECK(!PluginRepo::recordInstall(dir, official, err));
    CHECK(count("plug_inst.write") == 2u);

    // A plugin SITE's entry: the same failure, NOT counted.
    PluginCatalogEntry site = official;
    site.source = "https://example.invalid/site.json";
    CHECK(!PluginRepo::recordInstall(dir, site, err));
    CHECK(count("plug_inst.write") == 2u);
}

}  // namespace

int main() {
    std::printf("test_errpath_sweep_a\n");
    g_root = fs::path("errpath_sweep_a_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    testRegionalIndexIsCounted();
    testScreenshotFailureIsCounted();
    testRecordInstallFailureIsCounted();

    health::globalLedger()->reset();
    const int rc = testSummary("test_errpath_sweep_a");
    if (rc == 0) { fs::remove_all(g_root, ec); }
    return rc;
}
