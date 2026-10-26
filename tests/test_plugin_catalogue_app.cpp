// test_plugin_catalogue_app.cpp - the ENGINE keeping the catalogue it read and fetching a plugin's pictures (0.99.72, the plugin store
// redesign), and listing the plugin sites a user added (0.99.74). No window, no network: the catalogue is a local file (the same seam CHECK
// NOW's local form uses) and the pictures come from the transport stand-in (PluginRepo::setTransportForTest), so the worker, the poll, the
// cache files and the rename all run for real.
//
// CONVERTED (stub campaign B3, 0.99.84 house pattern): master ran all of this in the window (AppWindow::startCatalogFetch / pollPluginAsync /
// loadCachedCatalogue / requestScreenshots / addPluginSite ...); it is the ENGINE's now (src/engine/engine_store.cpp). Every touch of it goes
// through rawEngine and the pass seam (tests/pass_seam.hpp: the same test runs inline and with the pass on its own thread). What differs in the
// way it is checked, and why - the assertions the engine still answers are untouched:
//   - removing a site deletes its kept copy on a WORKER (the pass never waits on a disk): the helper waits for the pass to collect it before
//     the disk is looked at;
//   - the store window's MODEL (buildPluginStoreModel: a card's thirdParty/siteHost/legal-notice sentence/origin note, the sites panel's state
//     and label) is the WINDOW's and comes back in the next batch, where tests/test_plugin_store_window.cpp holds it over the 0.99.72 model;
//     the section that read it here reads what the window will read instead - the table: the entry's key, fact.store.<key>.site, the gate
//     asked with and without the notice acknowledged, and fact.store.sites;
//   - the web control (AppWindow::applyControlRequest) is the engine's STORE_INSTALL op from a REMOTE session now: the refusal is asserted on the
//     op's own path with the session's origin set, and the same through a real web session in tests/test_engine_api_store.cpp.
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
#include "core/app_commands.hpp"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::CatalogScreenshot;
using cascade::core::DiagLog;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginRepo;

namespace cascade::gui {

struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    using PictureState = Engine::PictureState;
    using PictureStatus = Engine::PictureStatus;
    static Engine& eng(const AppWindow& a) { return rawEngine(const_cast<AppWindow&>(a).engine_); }
    // Runs `f` on the pass's own thread (or here, inline) and returns what it returns.
    template <class R, class F>
    static R rd(const AppWindow& a, F f) {
        return passseam::read<R>(eng(a), std::function<R()>(f));
    }
    template <class F>
    static void wr(const AppWindow& a, F f) {
        passseam::on(eng(a), std::function<void()>(f));
    }

    static bool fetchCatalogue(AppWindow& a, const std::string& url) {
        bool started = false;
        wr(a, [&] {
            eng(a).pluginCatalogueUrl_ = url;
            eng(a).startCatalogFetch();
            started = eng(a).catalogPending_;
        });
        if (!started) { return false; }
        for (int i = 0; i < 6000; ++i) {
            bool pending = true;
            bool ok = false;
            wr(a, [&] {
                eng(a).pollPluginAsync();
                pending = eng(a).catalogPending_;
                ok = eng(a).catalogError_.empty();
            });
            if (!pending) { return ok; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    // One pass's polling, until `done` or the wait runs out.
    static bool pollUntil(AppWindow& a, const std::function<bool()>& done) {
        for (int i = 0; i < 6000; ++i) {
            wr(a, [&] { eng(a).pollPluginAsync(); });
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    static void poll(AppWindow& a) { wr(a, [&] { eng(a).pollPluginAsync(); }); }
    static std::string pluginDir(const AppWindow& a) { return rd<std::string>(a, [&] { return eng(a).pluginDir_; }); }
    static std::vector<PluginCatalogEntry> catalogue(const AppWindow& a) {
        return rd<std::vector<PluginCatalogEntry>>(a, [&] { return eng(a).catalog_; });
    }
    static void setCatalogue(AppWindow& a, std::vector<PluginCatalogEntry> c) {
        wr(a, [&] {
            eng(a).catalog_ = std::move(c);
            ++eng(a).catalogSeq_;
        });
    }
    static std::string error(const AppWindow& a) { return rd<std::string>(a, [&] { return eng(a).catalogError_; }); }
    static bool fromCache(const AppWindow& a) { return rd<bool>(a, [&] { return eng(a).catalogueFromCache_; }); }
    static std::int64_t readTime(const AppWindow& a) { return rd<std::int64_t>(a, [&] { return eng(a).catalogueReadTime_; }); }
    static bool readThisSession(const AppWindow& a) { return rd<bool>(a, [&] { return eng(a).catalogueReadThisSession(); }); }
    static bool catalogPending(const AppWindow& a) { return rd<bool>(a, [&] { return eng(a).catalogPending_; }); }
    static bool installPending(const AppWindow& a) { return rd<bool>(a, [&] { return eng(a).installPending_; }); }
    static void setInstallPending(AppWindow& a, bool v) { wr(a, [&] { eng(a).installPending_ = v; }); }
    static void setAddAllActive(AppWindow& a, bool v) { wr(a, [&] { eng(a).addAllRun_.active = v; }); }
    static void request(AppWindow& a, const std::string& id, bool retry = false) {
        wr(a, [&] { eng(a).requestScreenshots(id, retry); });
    }
    static PictureStatus status(const AppWindow& a, const CatalogScreenshot& s) {
        return rd<PictureStatus>(a, [&] { return eng(a).pictureStatus(s); });
    }
    static bool shotPending(const AppWindow& a) { return rd<bool>(a, [&] { return eng(a).screenshotPending_; }); }
    static std::size_t queued(const AppWindow& a) { return rd<std::size_t>(a, [&] { return eng(a).screenshotQueue_.size(); }); }
    static std::string blockedReason(const AppWindow& a, int idx, bool acknowledged = true) {
        return rd<std::string>(a, [&] { return eng(a).pluginInstallBlockedReason(idx, acknowledged); });
    }
    static void startInstall(AppWindow& a, const PluginCatalogEntry& e) { wr(a, [&] { eng(a).startInstall(e); }); }

    // --- plugin sites (0.99.74) ---
    static void setSites(AppWindow& a, std::vector<std::string> s) { wr(a, [&] { eng(a).pluginSites_ = std::move(s); }); }
    static std::vector<std::string> sites(const AppWindow& a) { return rd<std::vector<std::string>>(a, [&] { return eng(a).pluginSites_; }); }
    static void loadCachedSites(AppWindow& a) { wr(a, [&] { eng(a).loadCachedSites(); }); }
    static bool addSite(AppWindow& a, const std::string& url, std::string& why) {
        bool ok = false;
        wr(a, [&] { ok = eng(a).addPluginSite(url, why); });
        return ok;
    }
    // REMOVE, and the wait for the worker that deletes the site's kept copy.
    static void removeSite(AppWindow& a, const std::string& url) {
        wr(a, [&] { eng(a).removePluginSite(url); });
        for (int i = 0; i < 6000; ++i) {
            bool pending = true;
            wr(a, [&] {
                eng(a).pollPluginAsync();
                pending = eng(a).cleanupPending_;
            });
            if (!pending) { return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(false);
    }
    static int indexByKey(const AppWindow& a, const std::string& key) {
        return rd<int>(a, [&] { return eng(a).catalogIndexByKey(key); });
    }
    struct SiteState {
        int state = 0;  // 0 not read, 1 kept, 2 read, 3 failed, -1 no such site
        int listed = 0;
        int hiddenSameId = 0;
        int hiddenOffHost = 0;
        std::string error;
    };
    static SiteState siteState(const AppWindow& a, const std::string& url) {
        return rd<SiteState>(a, [&] {
            for (const Engine::SiteStatus& s : eng(a).siteStatus_) {
                if (s.url != url) { continue; }
                SiteState out;
                out.state = static_cast<int>(s.state);
                out.listed = s.listed;
                out.hiddenSameId = s.hiddenSameId;
                out.hiddenOffHost = s.hiddenOffHost;
                out.error = s.error;
                return out;
            }
            SiteState none;
            none.state = -1;
            return none;
        });
    }
    static std::size_t siteStatusCount(const AppWindow& a) { return rd<std::size_t>(a, [&] { return eng(a).siteStatus_.size(); }); }
    static std::string installError(const AppWindow& a) { return rd<std::string>(a, [&] { return eng(a).installError_; }); }
    // The install the web page asks for: STORE_INSTALL from a REMOTE session (the engine's origin flag is what ApiHostImpl::apply sets around
    // the call), the notice acknowledged. Returns the op's status.
    static int remoteInstall(AppWindow& a, const std::string& id) {
        int status = 0;
        wr(a, [&] {
            eng(a).commandFromRemote_ = true;
            const cascade::core::cmd::QueuedCommand q = cascade::core::cmd::makeText(FOXAPI_OP_STORE_INSTALL, id, 1);
            status = eng(a).applyCommand(q.c, q.longText).status;
            eng(a).commandFromRemote_ = false;
        });
        return status;
    }
    // The table, as a publish builds it (the lists and the facts of one block).
    static std::shared_ptr<const cascade::engine::ApiListBlock> block(const AppWindow& a) {
        return rd<std::shared_ptr<const cascade::engine::ApiListBlock>>(a, [&] { return eng(a).buildApiLists(0); });
    }
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

    // THE PICTURES WAIT FOR AN ADD ALL RUN TOO: between two of its downloads no install is in flight, and the queue still waits for the run
    // (here only said to be active) and goes when it is over.
    {
        const std::string body6(1500, '6');
        const CatalogScreenshot e1 = shot("https://pics.example.invalid/e1.png", body6);
        gate->bodies[e1.url] = body6;
        Access::setCatalogue(app, {entryWith("E", {e1})});
        const int hitsBefore = gate->hits.load();
        Access::setAddAllActive(app, true);
        Access::request(app, "E");
        for (int i = 0; i < 20; ++i) { Access::poll(app); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(Access::status(app, e1).state == PictureState::Pending);
        CHECK(!Access::shotPending(app));
        CHECK(gate->hits == hitsBefore);
        Access::setAddAllActive(app, false);
        CHECK(Access::pollUntil(app, [&] { return Access::status(app, e1).state == PictureState::Ready; }));
        CHECK(gate->hits == hitsBefore + 1);
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

// --- the plugin sites (0.99.74) ---------------------------------------------------------------
//
// A site's index is another file the REAL AppWindow reads on the same worker after the official one
// (the local-path form of a site, exactly as the official address has): its plugins are merged under
// the official ones - official winning an id - and carry their source; a site that fails never
// blanks the list; each site is kept as its own copy and loaded at start-up; ADD and REMOVE change
// the list, REMOVE deletes the kept copy and leaves what was installed; and the install gate and the
// web control treat a site's plugin as the third-party code it is.
std::string siteText(const std::vector<std::string>& ids, bool withBuild = true) {
    std::string t = "{\"schemaVersion\":1,\"plugins\":[";
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0) { t += ","; }
        t += "{\"id\":\"" + ids[i] + "\",\"name\":\"Site " + ids[i] + "\",\"version\":\"0.1.0\","
             "\"licence\":\"GPL-3.0-or-later\",\"abiVersion\":" +
             std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
        if (withBuild) {
            t += ",\"platforms\":[{\"os\":\"" + std::string(PluginRepo::hostOs()) + "\",\"arch\":\"" +
                 PluginRepo::hostArch() + "\",\"file\":\"" + ids[i] + "-0.1.0" +
#if defined(_WIN32)
                 ".dll"
#else
                 ".so"
#endif
                 "\",\"url\":\"https://hobbyist.example/files/" + ids[i] + "\",\"sha256\":\"" +
                 std::string(64, 'a') + "\"}]";
        }
        t += "}";
    }
    return t + "]}";
}

void testPluginSites() {
    clearPluginsFolder();
    const fs::path siteA = g_scratch / "site-a.json";
    const fs::path siteB = g_scratch / "site-b.json";
    writeAll(g_localCatalogue, catalogueText(2));            // official: p1, p2
    writeAll(siteA, siteText({"hdradio", "p2"}));            // p2 clashes with the official p2
    writeAll(siteB, siteText({"other"}));
    const std::string urlA = siteA.string();
    const std::string urlB = siteB.string();
    const std::string labelA = PluginRepo::siteLabel(urlA);  // "site-a.json"
    CHECK(labelA == "site-a.json");

    // READ: the site's plugins join the official ones, carry their source, and the official p2 wins.
    {
        cascade::gui::AppWindow app;
        Access::setSites(app, {urlA});
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        const auto& cat = Access::catalogue(app);
        CHECK(cat.size() == 3u);  // p1, p2, hdradio
        int officialP2 = 0, siteP2 = 0, hd = 0;
        for (const PluginCatalogEntry& e : cat) {
            if (e.id == "p2") { (e.source.empty() ? officialP2 : siteP2) += 1; }
            if (e.id == "hdradio") {
                ++hd;
                CHECK(e.source == urlA);
            }
        }
        CHECK(officialP2 == 1 && siteP2 == 0 && hd == 1);
        const Access::SiteState st = Access::siteState(app, urlA);
        CHECK(st.state == 2);  // read
        CHECK(st.listed == 1);
        CHECK(st.hiddenSameId == 1);
        CHECK(st.hiddenOffHost == 0);
        // The official status counts the official plugins only.
        CHECK(Access::error(app).empty());
        // THE KEPT COPY: the site's own pair of files, exactly as read.
        const std::string kept = PluginRepo::siteCacheFileName(urlA);
        CHECK(!kept.empty() && fs::is_regular_file(g_dir / kept));
        CHECK(fs::is_regular_file(g_dir / PluginRepo::siteCacheTimeFileName(urlA)));
        CHECK(readAll(g_dir / kept) == siteText({"hdradio", "p2"}));
        // THE TABLE the store's page is drawn from: the key, the entry's site, the sites' lines, and the gate that is always there.
        {
            const auto block = Access::block(app);
            // fact.store.sites: "address TAB state TAB listed TAB hidden-same-id TAB hidden-off-host TAB error" - state 2 is read.
            CHECK(block->facts.at("store.sites").value == urlA + "\t2\t1\t1\t0\t");
            CHECK(block->facts.at("store.sites").localOnly);  // a stranger's address the user typed
            int cards = 0;
            for (const cascade::engine::ApiListBlock::StoreEntry& se : block->store) {
                const std::string key = se.id;
                if (se.name == "Site hdradio") {
                    ++cards;
                    CHECK(key == "hdradio@" + labelA);  // the id for an official plugin, "<id>@<site label>" for a site's
                    CHECK(block->facts.at("store." + key + ".site").value == urlA);
                    CHECK(block->facts.at("store." + key + ".site").localOnly);
                    // The gate, with a site's plugin that publishes no legal notice of its own: the acknowledgement is still asked for,
                    // and given it, it passes.
                    const int idx = Access::indexByKey(app, key);
                    CHECK(idx >= 0);
                    CHECK(Access::blockedReason(app, idx, true).empty());
                    CHECK(Access::blockedReason(app, idx, false) == "the legal notice must be acknowledged first");
                    CHECK(block->facts.at("store." + key + ".blocked").value.empty());  // asked WITH the notice acknowledged
                } else {
                    CHECK(key == (se.name == "Plugin p1" ? "p1" : "p2"));
                    CHECK(block->facts.at("store." + key + ".site").value.empty());
                    // An official plugin with no notice of its own: the tick changes nothing about its gate (a site's is the opposite, above).
                    const int oi = Access::indexByKey(app, key);
                    CHECK(Access::blockedReason(app, oi, false) == Access::blockedReason(app, oi, true));
                }
            }
            CHECK(cards == 1);
        }
        CHECK(Access::indexByKey(app, "hdradio@" + labelA) >= 0);
        CHECK(Access::indexByKey(app, "hdradio") == -1);  // the bare id names no entry of a site

        // THE WEB CONTROL FITS OFFICIAL PLUGINS ONLY: a site's id is refused with a reason - by the bare id the page lists, and by the key.
        CHECK(Access::remoteInstall(app, "hdradio") == FOXAPI_DENIED);
        CHECK(!Access::installPending(app));
        CHECK(Access::installError(app).find("can only be fitted from the plugin store on this computer") != std::string::npos);
        CHECK(Access::remoteInstall(app, "hdradio@" + labelA) == FOXAPI_DENIED);
        CHECK(!Access::installPending(app));

        // A SITE THAT FAILS THE NEXT TIME never blanks the list: its earlier entries stay, with the reason.
        fs::remove(siteA);
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));  // the OFFICIAL read is fine
        CHECK(Access::catalogue(app).size() == 3u);
        const Access::SiteState gone = Access::siteState(app, urlA);
        CHECK(gone.state == 3);  // failed
        CHECK(!gone.error.empty());
        CHECK(gone.listed == 1);
        CHECK(Access::error(app).empty());  // the official list's own status is not the site's
        // ...and the kept copy is untouched by the failure.
        CHECK(readAll(g_dir / kept) == siteText({"hdradio", "p2"}));
        writeAll(siteA, siteText({"hdradio", "p2"}));
    }

    // A NEW WINDOW loads the kept copy of each site (no request), under the official kept copy.
    {
        DiagLog::instance().resetForTest();
        cascade::gui::AppWindow app;
        CHECK(Access::catalogue(app).size() == 2u);  // the official kept copy only: no sites yet
        Access::setSites(app, {urlA});
        Access::loadCachedSites(app);
        CHECK(Access::catalogue(app).size() == 3u);
        CHECK(Access::fromCache(app));
        CHECK(!Access::readThisSession(app));
        CHECK(!Access::catalogPending(app));
        const Access::SiteState st = Access::siteState(app, urlA);
        CHECK(st.state == 1);  // kept
        CHECK(st.listed == 1 && st.hiddenSameId == 1);
        // A site with no kept copy is not read yet.
        Access::setSites(app, {urlA, urlB});
        Access::loadCachedSites(app);
        CHECK(Access::siteState(app, urlB).state == 0);
        CHECK(Access::catalogue(app).size() == 3u);
    }

    // THE OFFICIAL CATALOGUE FAILING does not stop the sites: they are read anyway and merged under what stood
    // in the official catalogue's place.
    {
        cascade::gui::AppWindow app;
        Access::setSites(app, {urlA});
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        CHECK(Access::catalogue(app).size() == 3u);
        writeAll(siteA, siteText({"hdradio", "newcomer"}));
        CHECK(!Access::fetchCatalogue(app, (g_scratch / "no-such-catalogue.json").string()));
        CHECK(!Access::error(app).empty());                  // the official failure is shown
        CHECK(Access::catalogue(app).size() == 4u);          // p1, p2 (earlier), hdradio, newcomer
        bool newcomer = false;
        for (const PluginCatalogEntry& e : Access::catalogue(app)) {
            if (e.id == "newcomer") { newcomer = (e.source == urlA); }
        }
        CHECK(newcomer);
        CHECK(Access::siteState(app, urlA).state == 2);
        writeAll(siteA, siteText({"hdradio", "p2"}));
    }

    // ADD AND REMOVE.
    {
        clearPluginsFolder();
        writeAll(g_localCatalogue, catalogueText(2));
        cascade::gui::AppWindow app;
        CHECK(Access::fetchCatalogue(app, g_localCatalogue.string()));
        std::string why;
        // Refused: not a site address, a repeat, the official address, a transfer in flight.
        CHECK(!Access::addSite(app, "http://hobbyist.example/i.json", why));
        CHECK(!why.empty());
        CHECK(Access::sites(app).empty());
        CHECK(Access::addSite(app, "  " + urlA + "  ", why));
        CHECK(Access::sites(app).size() == 1u && Access::sites(app)[0] == urlA);
        CHECK(Access::catalogPending(app));  // ADD starts a read
        CHECK(!Access::addSite(app, urlB, why));  // not while it runs
        CHECK(why.find("transfer is already in progress") != std::string::npos);
        CHECK(Access::pollUntil(app, [&] { return !Access::catalogPending(app); }));
        CHECK(Access::catalogue(app).size() == 3u);
        CHECK(Access::siteState(app, urlA).state == 2);
        CHECK(!Access::addSite(app, urlA, why));
        CHECK(why.find("already in the list") != std::string::npos);
        CHECK(!Access::addSite(app, PluginRepo::defaultIndexUrl(), why));
        // A second site, then an INSTALLED plugin from the first (a record and its file), then REMOVE.
        CHECK(Access::addSite(app, urlB, why));
        CHECK(Access::pollUntil(app, [&] { return !Access::catalogPending(app); }));
        CHECK(Access::catalogue(app).size() == 4u);
        CHECK(Access::siteStatusCount(app) == 2u);
        const std::string module = std::string("hdradio-0.1.0") +
#if defined(_WIN32)
                                   ".dll";
#else
                                   ".so";
#endif
        writeAll(g_dir / module, "installed bytes");
        std::string manifest = "{\"schemaVersion\":1,\"plugins\":[{\"id\":\"hdradio\",\"source\":\"";
        for (const char c : urlA) {
            if (c == '\\') { manifest += "\\\\"; } else { manifest += c; }
        }
        manifest += "\",\"version\":\"0.1.0\",\"file\":\"" + module + "\"}]}";
        writeAll(g_dir / "installed.json", manifest);
        const std::string manifestBefore = readAll(g_dir / "installed.json");
        const std::string keptA = PluginRepo::siteCacheFileName(urlA);
        const std::string keptB = PluginRepo::siteCacheFileName(urlB);
        CHECK(fs::is_regular_file(g_dir / keptA) && fs::is_regular_file(g_dir / keptB));

        Access::removeSite(app, urlA);
        CHECK(Access::sites(app).size() == 1u && Access::sites(app)[0] == urlB);
        CHECK(Access::siteState(app, urlA).state == -1);
        CHECK(Access::catalogue(app).size() == 3u);  // p1, p2, other: hdradio left with its site
        for (const PluginCatalogEntry& e : Access::catalogue(app)) { CHECK(e.source != urlA); }
        // Its kept copy is gone (and B's is not); what was installed from it stays installed.
        CHECK(!fs::exists(g_dir / keptA));
        CHECK(!fs::exists(g_dir / PluginRepo::siteCacheTimeFileName(urlA)));
        CHECK(fs::is_regular_file(g_dir / keptB));
        CHECK(readAll(g_dir / module) == "installed bytes");
        CHECK(readAll(g_dir / "installed.json") == manifestBefore);
        // The table has one site, and it is the second.
        CHECK(Access::block(app)->facts.at("store.sites").value == urlB + "\t2\t1\t0\t0\t");
        // Removing one that is not there changes nothing.
        Access::removeSite(app, urlA);
        CHECK(Access::sites(app).size() == 1u);
    }

    // A SITE'S PICTURE ON ANOTHER HOST IS NEVER REQUESTED, whatever got into the list: mergeSite holds every
    // url to the site's host when the list is made, and the request is the second look (it is the request
    // that contacts an address). One picture on the site's host is queued; the one elsewhere is not.
    {
        clearPluginsFolder();
        cascade::gui::AppWindow app;
        PluginCatalogEntry e;
        e.id = "pic";
        e.name = "Pic";
        e.version = "1.0.0";
        e.source = "https://hobbyist.example/index.json";
        CatalogScreenshot onHost;
        onHost.url = "https://hobbyist.example/shots/ok.png";
        onHost.sha256 = std::string(64, 'a');
        onHost.caption = "on the site's host";
        CatalogScreenshot elsewhere;
        elsewhere.url = "https://tracker.example/pixel.png";
        elsewhere.sha256 = std::string(64, 'b');
        elsewhere.caption = "somewhere else";
        e.screenshots = {onHost, elsewhere};
        Access::setCatalogue(app, {e});
        Access::request(app, "pic@hobbyist.example");
        CHECK(Access::status(app, onHost).state == PictureState::Pending);
        CHECK(Access::status(app, elsewhere).state == PictureState::None);
        CHECK(Access::queued(app) == 1u);
        // The bare id names no entry of a site: nothing more is queued.
        Access::request(app, "pic");
        CHECK(Access::queued(app) == 1u);
    }

    // AT MOST EIGHT.
    {
        clearPluginsFolder();
        cascade::gui::AppWindow app;
        std::vector<std::string> many;
        for (int i = 0; i < 8; ++i) { many.push_back("https://s" + std::to_string(i) + ".example/i.json"); }
        Access::setSites(app, many);
        std::string why;
        CHECK(!Access::addSite(app, "https://ninth.example/i.json", why));
        CHECK(why.find("full") != std::string::npos);
        CHECK(Access::sites(app).size() == 8u);
    }
}

}  // namespace

int main() {
    std::printf("test_plugin_catalogue_app\n");
    isolate();
    testCatalogueKept();
    testPictures();
    testPluginSites();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_plugin_catalogue_app");
}
