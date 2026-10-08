// test_plugin_catalogue_app.cpp - the REAL AppWindow keeping the catalogue it read and fetching a
// plugin's pictures (0.99.72, the plugin store redesign). No window, no network: the catalogue is a
// local file (the same seam CHECK NOW's local form uses) and the pictures come from the transport
// stand-in (PluginRepo::setTransportForTest), so the worker, the poll, the cache files and the
// rename all run for real.
//
// THE CATALOGUE KEPT (core/plugin_repo.hpp, saveCatalogueCache / loadCachedIndex; the core's own
// rules are in tests/test_plugin_cache.cpp):
//   - a good read writes catalogue.json and catalogue.json.time into the plugins folder, the text
//     exactly as read; a read that is refused writes nothing and leaves the kept copy as it was;
//   - a NEW window loads it at start-up (catalog_ filled, catalogueFromCache_ true, the time of
//     the read) and fetches NOTHING; one session's read is told apart from a kept copy
//     (catalogueReadThisSession, which the rail's IDLE and the store's one automatic read use);
//   - a copy that is not a catalogue is ignored at start-up with a log line and left alone;
//   - a refresh that FAILS no longer empties the store: a list on screen stays (the kept copy, or
//     an earlier read of this session - and the kept copy does not replace the latter), and with
//     nothing on screen the kept copy comes back with its time; the reason is catalogError_.
//
// THE PICTURES (AppWindow::requestScreenshots and the per-frame pump):
//   - every picture of a plugin is Pending at once, then Ready with a path in store-cache, ONE
//     transfer at a time (the stand-in counts how many are in flight at once);
//   - a picture already Ready or Pending is not asked for again; a Failed one is only when the
//     caller says retry; an unknown plugin queues nothing;
//   - they WAIT for a catalogue fetch, an install or an ADD ALL run, and an install, an update, a
//     catalogue fetch and ADD ALL refuse to start while one is in flight (the install gate says
//     "a transfer is already in progress");
//   - a catalogue read that no longer names a picture drops its status and its cached file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::CatalogScreenshot;
using cascade::core::DiagLog;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginRepo;

namespace cascade::gui {

struct AppWindowTestAccess {
    using PictureState = AppWindow::PictureState;

    static bool fetchCatalogue(AppWindow& a, const std::string& url) {
        a.pluginCatalogueUrl_ = url;
        a.startCatalogFetch();
        if (!a.catalogPending_) { return false; }
        for (int i = 0; i < 6000; ++i) {
            a.pollPluginAsync();
            if (!a.catalogPending_) { return a.catalogError_.empty(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    // One frame's polling, until `done` or the wait runs out.
    static bool pollUntil(AppWindow& a, const std::function<bool()>& done) {
        for (int i = 0; i < 6000; ++i) {
            a.pollPluginAsync();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    static void poll(AppWindow& a) { a.pollPluginAsync(); }
    static std::string pluginDir(const AppWindow& a) { return a.pluginDir_; }
    static const std::vector<PluginCatalogEntry>& catalogue(const AppWindow& a) { return a.catalog_; }
    static void setCatalogue(AppWindow& a, std::vector<PluginCatalogEntry> c) {
        a.catalog_ = std::move(c);
    }
    static const std::string& error(const AppWindow& a) { return a.catalogError_; }
    static bool fromCache(const AppWindow& a) { return a.catalogueFromCache_; }
    static std::int64_t readTime(const AppWindow& a) { return a.catalogueReadTime_; }
    static bool readThisSession(const AppWindow& a) { return a.catalogueReadThisSession(); }
    static bool catalogPending(const AppWindow& a) { return a.catalogPending_; }
    static bool installPending(const AppWindow& a) { return a.installPending_; }
    static void setInstallPending(AppWindow& a, bool v) { a.installPending_ = v; }
    static void request(AppWindow& a, const std::string& id, bool retry = false) {
        a.requestScreenshots(id, retry);
    }
    static AppWindow::PictureStatus status(const AppWindow& a, const CatalogScreenshot& s) {
        return a.pictureStatus(s);
    }
    static bool shotPending(const AppWindow& a) { return a.screenshotPending_; }
    static std::size_t queued(const AppWindow& a) { return a.screenshotQueue_.size(); }
    static std::string blockedReason(const AppWindow& a, int idx) {
        return a.pluginInstallBlockedReason(idx, true);
    }
    static void startInstall(AppWindow& a, const PluginCatalogEntry& e) { a.startInstall(e); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using PictureState = cascade::gui::AppWindowTestAccess::PictureState;

namespace {

fs::path g_scratch;
fs::path g_dir;  // the plugins folder
fs::path g_localCatalogue;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_plugin_catalogue_app_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
    cascade::core::PackageIdentity id;
    id.packaged = true;
    id.fullName = "catalogue-app-test";
    cascade::core::setPackageIdentityForTest(id);
    g_dir = fs::path(cascade::core::PluginHost::userPluginDir());
    fs::create_directories(g_dir, ec);
    g_localCatalogue = g_scratch / "local-catalogue.json";
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string sha256Of(const std::string& bytes) {
    std::string hex;
    std::string err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

std::size_t logged(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

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

// A catalogue document with plugins "p1".."pN" (N = count), p1 carrying the pictures
// whose digests are `shots` (url, sha256 pairs).
std::string catalogueText(int count, const std::vector<std::pair<std::string, std::string>>& shots = {},
                          const std::string& firstId = "p1") {
    std::string t = "{\"schemaVersion\":1,\"plugins\":[";
    for (int i = 0; i < count; ++i) {
        if (i != 0) { t += ","; }
        const std::string id = i == 0 ? firstId : "p" + std::to_string(i + 1);
        t += "{\"id\":\"" + id + "\",\"name\":\"Plugin " + id + "\",\"version\":\"1.0.0\",\"licence\":\"MIT\","
             "\"abiVersion\":" + std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
        if (i == 0 && !shots.empty()) {
            t += ",\"screenshots\":[";
            for (std::size_t k = 0; k < shots.size(); ++k) {
                if (k != 0) { t += ","; }
                t += "{\"url\":\"" + shots[k].first + "\",\"sha256\":\"" + shots[k].second +
                     "\",\"caption\":\"picture " + std::to_string(k) + "\"}";
            }
            t += "]";
        }
        t += "}";
    }
    return t + "]}";
}

void clearPluginsFolder() {
    std::error_code ec;
    std::vector<fs::path> all;
    for (const auto& e : fs::directory_iterator(g_dir, ec)) { all.push_back(e.path()); }
    for (const fs::path& p : all) { fs::remove_all(p, ec); }
}

// --- the catalogue kept -------------------------------------------------------------------

void testCatalogueKept() {
    clearPluginsFolder();
    const std::string c1 = catalogueText(2);
    writeAll(g_localCatalogue, c1);

    // A GOOD READ WRITES THE COPY, exactly as read, with the time.
    std::int64_t keptTime = 0;
    {
        cascade::gui::AppWindow app;
        CHECK(Access::catalogue(app).empty());
        CHECK(!Access::fromCache(app));
        CHECK(!Access::readThisSession(app));
        const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        const std::int64_t after = static_cast<std::int64_t>(std::time(nullptr));
        CHECK(Access::catalogue(app).size() == 2u);
        CHECK(!Access::fromCache(app));
        CHECK(Access::readThisSession(app));
        keptTime = Access::readTime(app);
        CHECK(keptTime >= before && keptTime <= after);
        CHECK(fs::is_regular_file(g_dir / "catalogue.json"));
        CHECK(fs::is_regular_file(g_dir / "catalogue.json.time"));
        CHECK(readAll(g_dir / "catalogue.json") == c1);
        CHECK(fs::equivalent(fs::path(Access::pluginDir(app)), g_dir));
    }

    // A NEW WINDOW OPENS WITH IT, fetches nothing, and does not take it for a read of this session.
    {
        DiagLog::instance().resetForTest();
        cascade::gui::AppWindow app;
        CHECK(Access::catalogue(app).size() == 2u);
        CHECK(Access::fromCache(app));
        CHECK(Access::readTime(app) == keptTime);
        CHECK(!Access::readThisSession(app));
        CHECK(!Access::catalogPending(app));
        CHECK(logged("plugin store: kept catalogue loaded - 2 plugins") == 1u);

        // A REFRESH THAT FAILS leaves the list where it is, with the reason and the old time.
        CHECK(!Access::fetchCatalogue(app, (g_scratch / "no-such-catalogue.json").string()));
        CHECK(!Access::error(app).empty());
        CHECK(Access::catalogue(app).size() == 2u);
        CHECK(Access::fromCache(app));
        CHECK(Access::readTime(app) == keptTime);
        CHECK(logged("plugin store: catalogue refresh failed - showing the kept copy of 2 plugins") == 1u);
        // ...and the reason is not in the log (it can carry the address).
        CHECK(logged("no-such-catalogue") == 0u);

        // A REFUSED INDEX writes nothing and leaves the copy as it was.
        writeAll(g_localCatalogue, "this is not a catalogue");
        CHECK(!Access::fetchCatalogue(app, g_localCatalogue.string()));
        CHECK(Access::catalogue(app).size() == 2u);
        CHECK(readAll(g_dir / "catalogue.json") == c1);
        CHECK(readAll(g_dir / "catalogue.json.time") == std::to_string(keptTime) + "\n");

        // A GOOD READ AFTER IT replaces the kept copy and takes the "from the kept copy" off the list.
        const std::string c2 = catalogueText(3);
        writeAll(g_localCatalogue, c2);
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        CHECK(Access::catalogue(app).size() == 3u);
        CHECK(!Access::fromCache(app));
        CHECK(Access::readThisSession(app));
        CHECK(readAll(g_dir / "catalogue.json") == c2);
    }

    // WITH NOTHING ON SCREEN a failed refresh brings the kept copy back, with its time.
    {
        clearPluginsFolder();
        cascade::gui::AppWindow app;  // no kept copy: starts empty
        CHECK(Access::catalogue(app).empty());
        std::string err;
        CHECK(PluginRepo::saveCatalogueCache(g_dir.string(), c1, 1760001234, err));
        CHECK(!Access::fetchCatalogue(app, (g_scratch / "no-such-catalogue.json").string()));
        CHECK(Access::catalogue(app).size() == 2u);
        CHECK(Access::fromCache(app));
        CHECK(Access::readTime(app) == 1760001234);
        CHECK(!Access::error(app).empty());
        CHECK(!Access::readThisSession(app));
    }

    // AN EARLIER READ OF THIS SESSION IS NOT REPLACED BY THE KEPT COPY when a refresh fails.
    {
        clearPluginsFolder();
        cascade::gui::AppWindow app;
        writeAll(g_localCatalogue, catalogueText(3));
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        CHECK(Access::catalogue(app).size() == 3u);
        const std::int64_t readAt = Access::readTime(app);
        // Another catalogue is what the disk keeps now (written from outside).
        std::string err;
        CHECK(PluginRepo::saveCatalogueCache(g_dir.string(), catalogueText(1), 1760000001, err));
        CHECK(!Access::fetchCatalogue(app, (g_scratch / "no-such-catalogue.json").string()));
        CHECK(Access::catalogue(app).size() == 3u);   // the list read this session, not the kept one
        CHECK(!Access::fromCache(app));
        CHECK(Access::readTime(app) == readAt);
        CHECK(Access::readThisSession(app));
        CHECK(!Access::error(app).empty());
    }

    // A KEPT COPY THAT IS NOT A CATALOGUE is ignored at start-up, said so, and left alone.
    {
        clearPluginsFolder();
        writeAll(g_dir / "catalogue.json", "{ not json");
        writeAll(g_dir / "catalogue.json.time", "1760000000\n");
        DiagLog::instance().resetForTest();
        cascade::gui::AppWindow app;
        CHECK(Access::catalogue(app).empty());
        CHECK(!Access::fromCache(app));
        CHECK(Access::readTime(app) == 0);
        CHECK(logged("plugin store: kept catalogue ignored - the kept catalogue is not usable") == 1u);
        CHECK(readAll(g_dir / "catalogue.json") == "{ not json");
    }
    clearPluginsFolder();
}

// --- the pictures -------------------------------------------------------------------------

struct Gate {
    std::mutex m;
    std::map<std::string, std::string> bodies;  // url -> bytes
    std::set<std::string> failing;              // urls that answer an error
    std::atomic<int> inFlight{0};
    std::atomic<int> maxInFlight{0};
    std::atomic<int> hits{0};
    std::atomic<bool> hold{false};              // a request waits while this is true
};

void serve(const std::shared_ptr<Gate>& g) {
    PluginRepo::setTransportForTest(
        [g](const std::string& url, std::uint64_t,
            const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
            ++g->hits;
            const int now = ++g->inFlight;
            int seen = g->maxInFlight.load();
            while (now > seen && !g->maxInFlight.compare_exchange_weak(seen, now)) {}
            while (g->hold.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));  // long enough to overlap if they could
            bool ok = true;
            {
                std::lock_guard<std::mutex> lk(g->m);
                if (g->failing.count(url) != 0) {
                    error = "the server returned HTTP 404";
                    ok = false;
                } else {
                    const auto it = g->bodies.find(url);
                    if (it == g->bodies.end()) {
                        error = "the server returned HTTP 410";
                        ok = false;
                    } else {
                        ok = sink(it->second.data(), it->second.size());
                    }
                }
            }
            --g->inFlight;
            return ok;
        });
}

CatalogScreenshot shot(const std::string& url, const std::string& bytes) {
    CatalogScreenshot s;
    s.url = url;
    s.sha256 = sha256Of(bytes);
    s.caption = "a picture";
    return s;
}

PluginCatalogEntry entryWith(const std::string& id, std::vector<CatalogScreenshot> shots) {
    PluginCatalogEntry e;
    e.id = id;
    e.name = id;
    e.version = "1.0.0";
    e.licence = "MIT";
    e.abiVersion = static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    e.compatible = true;
    e.screenshots = std::move(shots);
    return e;
}

void testPictures() {
    clearPluginsFolder();
    const std::string body1(50000, '1');
    const std::string body2(60000, '2');
    const std::string body3(70000, '3');
    const CatalogScreenshot a1 = shot("https://pics.example.invalid/a1.png", body1);
    const CatalogScreenshot a2 = shot("https://pics.example.invalid/a2.png", body2);
    const CatalogScreenshot b1 = shot("https://pics.example.invalid/b1.png", body3);

    auto gate = std::make_shared<Gate>();
    gate->bodies[a1.url] = body1;
    gate->bodies[a2.url] = body2;
    gate->bodies[b1.url] = body3;
    serve(gate);

    cascade::gui::AppWindow app;
    Access::setCatalogue(app, {entryWith("A", {a1, a2}), entryWith("B", {b1})});

    // NEVER REQUESTED is None - the grid asks for nothing, and nothing is fetched.
    CHECK(Access::status(app, a1).state == PictureState::None);
    for (int i = 0; i < 10; ++i) { Access::poll(app); }
    CHECK(gate->hits == 0);

    // AN UNKNOWN PLUGIN queues nothing.
    Access::request(app, "no-such-plugin");
    CHECK(Access::queued(app) == 0u);

    // EVERY PICTURE IS PENDING AT ONCE, then Ready with a path, ONE TRANSFER AT A TIME.
    Access::request(app, "A");
    CHECK(Access::status(app, a1).state == PictureState::Pending);
    CHECK(Access::status(app, a2).state == PictureState::Pending);
    CHECK(Access::status(app, b1).state == PictureState::None);
    CHECK(Access::queued(app) == 2u);
    CHECK(Access::pollUntil(app, [&] {
        return Access::status(app, a1).state == PictureState::Ready &&
               Access::status(app, a2).state == PictureState::Ready;
    }));
    CHECK(gate->hits == 2);
    CHECK(gate->maxInFlight == 1);
    const auto st1 = Access::status(app, a1);
    const auto st2 = Access::status(app, a2);
    CHECK(st1.path == PluginRepo::screenshotCachePath(g_dir.string(), a1.sha256));
    CHECK(st2.path == PluginRepo::screenshotCachePath(g_dir.string(), a2.sha256));
    CHECK(readAll(st1.path) == body1 && readAll(st2.path) == body2);
    CHECK(st1.reason.empty());
    CHECK(!Access::shotPending(app));
    CHECK(Access::queued(app) == 0u);

    // A READY PICTURE IS NOT ASKED FOR AGAIN, however often the page says so.
    for (int i = 0; i < 3; ++i) { Access::request(app, "A", true); }
    for (int i = 0; i < 10; ++i) { Access::poll(app); }
    CHECK(gate->hits == 2);
    CHECK(Access::queued(app) == 0u);

    // A FAILED ONE says why, and is asked for again only when the caller says retry.
    gate->failing.insert(b1.url);
    Access::request(app, "B");
    CHECK(Access::pollUntil(app, [&] { return Access::status(app, b1).state == PictureState::Failed; }));
    CHECK(gate->hits == 3);
    CHECK(Access::status(app, b1).reason == "the server returned HTTP 404");
    CHECK(Access::status(app, b1).path.empty());
    CHECK(!fs::exists(PluginRepo::screenshotCachePath(g_dir.string(), b1.sha256)));
    Access::request(app, "B");  // the per-frame call: no retry
    for (int i = 0; i < 10; ++i) { Access::poll(app); }
    CHECK(gate->hits == 3);
    CHECK(Access::status(app, b1).state == PictureState::Failed);
    gate->failing.clear();
    Access::request(app, "B", true);  // the page opened again
    CHECK(Access::status(app, b1).state == PictureState::Pending);
    CHECK(Access::pollUntil(app, [&] { return Access::status(app, b1).state == PictureState::Ready; }));
    CHECK(gate->hits == 4);

    // THE PICTURES WAIT FOR AN INSTALL (here only said to be in flight) and go when it is over.
    {
        const std::string body4(1000, '4');
        const CatalogScreenshot c1 = shot("https://pics.example.invalid/c1.png", body4);
        gate->bodies[c1.url] = body4;
        Access::setCatalogue(app, {entryWith("A", {a1, a2}), entryWith("B", {b1}), entryWith("C", {c1})});
        Access::setInstallPending(app, true);
        Access::request(app, "C");
        for (int i = 0; i < 20; ++i) { Access::poll(app); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(Access::status(app, c1).state == PictureState::Pending);
        CHECK(!Access::shotPending(app));
        CHECK(gate->hits == 4);
        Access::setInstallPending(app, false);
        CHECK(Access::pollUntil(app, [&] { return Access::status(app, c1).state == PictureState::Ready; }));
        CHECK(gate->hits == 5);
    }

    // WHILE ONE IS IN FLIGHT, an install, a catalogue fetch and the install gate all say "busy".
    {
        const std::string body5(2000, '5');
        const CatalogScreenshot d1 = shot("https://pics.example.invalid/d1.png", body5);
        gate->bodies[d1.url] = body5;
        Access::setCatalogue(app, {entryWith("D", {d1})});
        gate->hold = true;
        Access::request(app, "D");
        CHECK(Access::pollUntil(app, [&] { return Access::shotPending(app) && gate->inFlight == 1; }));
        CHECK(Access::blockedReason(app, 0) == "a transfer is already in progress");
        Access::startInstall(app, Access::catalogue(app)[0]);
        CHECK(!Access::installPending(app));
        g_localCatalogue = g_scratch / "busy-catalogue.json";
        writeAll(g_localCatalogue, catalogueText(1));
        CHECK(!Access::fetchCatalogue(app, g_localCatalogue.string()));  // refused to start
        CHECK(!Access::catalogPending(app));
        gate->hold = false;
        CHECK(Access::pollUntil(app, [&] { return Access::status(app, d1).state == PictureState::Ready; }));
        // ...and with the picture in, the gate is the install's own again.
        CHECK(Access::blockedReason(app, 0) != "a transfer is already in progress");
    }

    // A CATALOGUE THAT NO LONGER NAMES A PICTURE DROPS ITS STATUS AND ITS FILE, and only those.
    {
        // The catalogue as the store would read it now names only b1.
        const std::string keepsB = catalogueText(1, {{b1.url, b1.sha256}}, "B");
        g_localCatalogue = g_scratch / "next-catalogue.json";
        writeAll(g_localCatalogue, keepsB);
        const auto filesBefore = filesIn(g_dir / "store-cache");
        CHECK(filesBefore.size() >= 4u);  // a1 a2 b1 c1 d1
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        CHECK(Access::status(app, a1).state == PictureState::None);
        CHECK(Access::status(app, a2).state == PictureState::None);
        CHECK(Access::status(app, b1).state == PictureState::Ready);
        CHECK(filesIn(g_dir / "store-cache") == (std::vector<std::string>{b1.sha256 + ".png"}));
    }
    PluginRepo::setTransportForTest(nullptr);
}

}  // namespace

int main() {
    std::printf("test_plugin_catalogue_app\n");
    isolate();
    testCatalogueKept();
    testPictures();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_plugin_catalogue_app");
}
