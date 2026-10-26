// test_engine_api_store.cpp - the plugin store's engine half (0.99.49 - 0.99.74; stub campaign B3), held against the real engine through the C
// table and the engine's own passes - the way test_engine_api_03 holds 0.3's additions, with no window anywhere and no network (a local index
// for the catalogue and the sites, the transport stand-in PluginRepo::setTransportForTest for every picture and download).
//
// One group per rule the batch carries; each was shown red against a mutant of the code that holds it (docs/engine-stage4.md 6.22, B3):
//   S1  a plugin SITE through the table: STORE_SITE adds it only with the consent, a REMOTE session cannot send it, its plugins carry their
//       key and their site, the sites' lines say what each read, REMOVE takes its entries and its kept copy away - and leaves what was
//       installed from it installed;
//   S2  A REMOTE SESSION CANNOT FIT A PLUGIN FROM A SITE (the web page's own session included): the refusal is the sentence the web page's rows
//       carry, nothing starts, nothing is fetched - while the same session fits an official plugin and the desktop fits the site's by its key;
//   S3  THE KEPT COPY: a good read is kept, a new engine opens with it and fetches nothing, a read that FAILS empties nothing and says why,
//       and a document that is not a catalogue is refused and leaves the kept copy as it was;
//   S4  THE PICTURES: STORE_PICTURES_WANT, ONE transfer at a time, behind a catalogue read, no repeat of a picture already there, a failed one
//       asked again only on a retry, the path LOCAL, and the one-transfer gate on everything else while one is in flight;
//   S5  NO PASS WAITS ON A WORKER: a catalogue read held in the transport, a picture held in the transport and a clean-up held in the remover
//       each leave every pass prompt (the pass collects what has finished and never what has not);
//   S6  THE SITES AT START-UP: a configuration that names sites gets each one's kept copy merged under the official list, no request made;
//   S7  (stub campaign B4) A CARD'S FACTS AND HOW AN ENTRY STANDS AGAINST WHAT IS INSTALLED: the shop-window fields, the declared capabilities,
//       the install state by id and version (an update whose file is not on disk is installed all the same), and UPDATE ALL - the same queue as
//       ADD ALL with nothing to fetch in it, and its own reason when there is nothing to update.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_cleanup.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
using cascade::core::PluginRepo;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is (tests/test_engine_api_03.cpp has the same one).
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks(std::string (*dir)(), bool (*remove)(const std::string&, const std::string&, std::string&)) {
        Engine::testHooks_.pluginDir = dir;
        Engine::testHooks_.pluginRemove = remove;
    }
    static void setOldCopies(Engine& e, std::vector<cascade::core::SupersededPlugin> v) { e.pluginOldCopies_ = std::move(v); }
    static bool catalogPending(Engine& e) { return e.catalogPending_; }
    static bool screenshotPending(Engine& e) { return e.screenshotPending_; }
    static bool cleanupPending(Engine& e) { return e.cleanupPending_; }
    static std::size_t queued(Engine& e) { return e.screenshotQueue_.size(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

// ---- the scratch world ---------------------------------------------------------------------------------------------------------------

fs::path g_scratch;
fs::path g_plugins;
std::string pluginDirHook() { return g_plugins.string(); }

// A delete the test can hold: while `held`, the remover waits (the worker that deletes is in it, and the pass must not be).
std::atomic<bool> g_removeHeld{false};
std::atomic<int> g_removeEntered{0};
bool holdingRemove(const std::string& dir, const std::string& file, std::string& error) {
    ++g_removeEntered;
    while (g_removeHeld.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    return cascade::core::defaultPluginFileRemover()(dir, file, error);
}

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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_store_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
    g_plugins = g_scratch / "plugins";
    fs::create_directories(g_plugins, ec);
    Access::installHooks(&pluginDirHook, &holdingRemove);
}

void clearPlugins() {
    std::error_code ec;
    std::vector<fs::path> all;
    for (const auto& e : fs::directory_iterator(g_plugins, ec)) { all.push_back(e.path()); }
    for (const fs::path& p : all) { fs::remove_all(p, ec); }
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string sha256Of(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

#if defined(_WIN32)
const char* const kModExt = ".dll";
#elif defined(__APPLE__)
const char* const kModExt = ".dylib";
#else
const char* const kModExt = ".so";
#endif

struct Shot {
    std::string url, bytes;
};

// One entry of an index: an official or a site's plugin with a build for THIS machine (so it is installable) and, optionally, pictures.
std::string entryJson(const std::string& id, const std::string& version, const std::string& host, const std::vector<Shot>& shots = {}) {
    std::string t = "{\"id\":\"" + id + "\",\"name\":\"Plugin " + id + "\",\"version\":\"" + version + "\",\"licence\":\"MIT\",\"abiVersion\":" +
                    std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) + ",\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() +
                    "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"" + id + "-" + version + kModExt + "\",\"url\":\"https://" + host +
                    "/files/" + id + "\",\"sha256\":\"" + std::string(64, 'a') + "\"}]";
    if (!shots.empty()) {
        t += ",\"screenshots\":[";
        for (std::size_t i = 0; i < shots.size(); ++i) {
            if (i != 0) { t += ","; }
            t += "{\"url\":\"" + shots[i].url + "\",\"sha256\":\"" + sha256Of(shots[i].bytes) + "\",\"caption\":\"picture " + std::to_string(i) + "\"}";
        }
        t += "]";
    }
    return t + "}";
}

std::string indexJson(const std::vector<std::string>& entries) {
    std::string t = "{\"schemaVersion\":1,\"plugins\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) { t += (i != 0 ? "," : "") + entries[i]; }
    return t + "]}";
}

// ---- the transport stand-in ----------------------------------------------------------------------------------------------------------

struct Net {
    std::mutex m;
    std::map<std::string, std::string> bodies;  // url -> bytes
    std::set<std::string> failing;              // urls that answer an HTTP error
    std::atomic<int> inFlight{0};
    std::atomic<int> maxInFlight{0};
    std::atomic<int> hits{0};
    std::atomic<bool> hold{false};              // a request waits while this is true
    std::map<std::string, int> perUrl;
};

void serve(const std::shared_ptr<Net>& g) {
    PluginRepo::setTransportForTest([g](const std::string& url, std::uint64_t, const std::function<bool(const void*, std::size_t)>& sink,
                                        std::string& error) {
        ++g->hits;
        {
            std::lock_guard<std::mutex> lk(g->m);
            ++g->perUrl[url];
        }
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
                    error = "the server returned HTTP 503";
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

// ---- the table -------------------------------------------------------------------------------------------------------------------------

std::string fact(testing::Session& s, const std::string& key, int32_t* status = nullptr) {
    char buf[8192];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    if (status != nullptr) { *status = n; }
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

// The CATALOGUE entry whose key (its `detail`) is `key`: a zeroed item when there is none.
FoxListItem entryByKey(testing::Session& s, const std::string& key) {
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_CATALOGUE); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_CATALOGUE, static_cast<uint32_t>(i));
        if (key == it.detail) { return it; }
    }
    return FoxListItem{};
}

template <class F>
bool passUntil(cascade::engine::Engine& eng, F done, int timeoutMs = 6000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void passes(cascade::engine::Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

FoxCommand textCmd(uint32_t op, const std::string& text, long long i0 = 0, long long i1 = 0) {
    FoxCommand c = testing::cmd(op, 0.0, i0, text.c_str());
    c.ival[1] = i1;
    return c;
}

// Submits ONE command on `session` and passes until its result lands.
testing::Outcome pumpedOn(cascade::engine::Engine& eng, FoxSession* session, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(session, &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(session, r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}
testing::Outcome pumped(cascade::engine::Engine& eng, testing::Session& s, const FoxCommand& c) { return pumpedOn(eng, s.raw(), c); }

struct Direct {
    cascade::engine::Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

// A web session, the engine's own (a REMOTE one with the web profile's closed list of ops).
struct WebSession {
    cascade::engine::Engine& eng;
    FoxSession* s = nullptr;
    explicit WebSession(cascade::engine::Engine& e) : eng(e) {
        std::int32_t st = 0;
        s = e.api().openInternalSession(cascade::engine::InternalKind::Web, "the web server", &st);
        CHECK_EQ(st, FOXAPI_OK);
    }
    ~WebSession() {
        if (s != nullptr) { api()->close_session(s); }
    }
};

const std::string kSiteRefusal = "plugins from sites you added can only be fitted from the plugin store on this computer";

// ---- S1: a plugin site through the table ---------------------------------------------------------------------------------------------------

void testSitesThroughTheTable() {
    clearPlugins();
    const fs::path official = g_scratch / "official.json";
    const fs::path siteA = g_scratch / "site-a.json";
    writeText(official, indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid"), entryJson("p2", "1.0.0", "plugins.example.invalid")}));
    // the site lists a plugin of its own and one that clashes with the official p2 (official wins an id)
    writeText(siteA, indexJson({entryJson("hdradio", "0.1.0", "hobbyist.example"), entryJson("p2", "9.9.9", "hobbyist.example")}));
    const std::string urlA = siteA.string();
    const std::string label = PluginRepo::siteLabel(urlA);
    CHECK_EQ(label, std::string("site-a.json"));

    Direct d;
    testing::Session local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(local.ok() && remote.ok());
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, official.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_CATALOGUE) == 2 && count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));

    // THE CONSENT: ADD without it writes nothing, lists nothing and says why; a REMOTE session cannot send the op at all.
    const std::uint64_t seq0 = local.state().listSeq;
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, urlA, 0, 0)).status(), FOXAPI_DENIED);
    CHECK(fact(local, "store.siteError").find("not ticked") != std::string::npos);
    CHECK_EQ(fact(local, "store.sites"), std::string());
    CHECK_EQ(count(local, FOXAPI_LIST_CATALOGUE), 2);
    CHECK(local.state().listSeq != seq0);  // the refusal's sentence is on the table, so the block moved
    CHECK_EQ(pumped(d.eng, remote, textCmd(FOXAPP_OP_STORE_SITE, urlA, 0, 1)).status(), FOXAPI_DENIED);
    CHECK_EQ(count(local, FOXAPI_LIST_CATALOGUE), 2);
    // An address the one validator refuses (not https) is refused with its reason, and nothing is listed.
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, "http://plugins.example.invalid/index.json", 0, 1)).status(), FOXAPI_BAD_ARGUMENT);
    CHECK(!fact(local, "store.siteError").empty());
    CHECK_EQ(fact(local, "store.sites"), std::string());

    // ADD with the consent: listed, read, merged under the official list.
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, urlA, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_CATALOGUE) == 3 && count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    CHECK_EQ(fact(local, "store.siteError"), std::string());
    // "address TAB state TAB listed TAB hidden-same-id TAB hidden-off-host TAB error": state 2, one listed, the clashing p2 hidden.
    CHECK_EQ(fact(local, "store.sites"), urlA + "\t2\t1\t1\t0\t");
    int32_t st = 0;
    fact(remote, "store.sites", &st);
    CHECK_EQ(st, FOXAPI_DENIED);  // a stranger's address the user typed: LOCAL only
    // The entry's key is "<id>@<site label>"; the official ones keep their bare id; the clash is the official p2.
    const std::string key = "hdradio@" + label;
    const FoxListItem hd = entryByKey(local, key);
    CHECK_EQ(std::string(hd.name), std::string("Plugin hdradio"));
    CHECK(entryByKey(local, "hdradio").name[0] == '\0');  // the bare id names no entry of a site
    CHECK_EQ(std::string(entryByKey(local, "p2").name), std::string("Plugin p2"));
    CHECK_EQ(fact(local, "store." + key + ".version"), std::string("0.1.0"));
    CHECK_EQ(fact(local, "store." + key + ".site"), urlA);
    CHECK_EQ(fact(local, "store.p2.site", &st), std::string());  // known and empty: official
    CHECK_EQ(st, 0);
    fact(remote, "store." + key + ".site", &st);
    CHECK_EQ(st, FOXAPI_DENIED);
    // The gate: a site's plugin ALWAYS carries a notice (asked with the acknowledgement, fact.blocked is empty).
    CHECK_EQ(fact(local, "store." + key + ".blocked"), std::string());
    // The kept copy of the site: the site's own pair of files, exactly as read.
    CHECK(fs::is_regular_file(g_plugins / PluginRepo::siteCacheFileName(urlA)));
    CHECK_EQ(readText(g_plugins / PluginRepo::siteCacheFileName(urlA)), readText(siteA));
    // ...a second ADD of the same address is refused, and an unknown one cannot be removed.
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, urlA, 0, 1)).status(), FOXAPI_BAD_ARGUMENT);
    CHECK(fact(local, "store.siteError").find("already in the list") != std::string::npos);
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, "https://nobody.example.invalid/i.json", 1)).status(), FOXAPI_NOT_FOUND);

    // WHAT WAS INSTALLED FROM THE SITE stays installed when the site goes: its record keeps its source and the file is not touched.
    const std::string module = std::string("hdradio-0.1.0") + kModExt;
    writeText(g_plugins / module, "installed bytes");
    std::string manifest = "{\"schemaVersion\":1,\"plugins\":[{\"id\":\"hdradio\",\"source\":\"";
    for (const char c : urlA) {
        if (c == '\\') { manifest += "\\\\"; } else { manifest += c; }
    }
    manifest += "\",\"version\":\"0.1.0\",\"file\":\"" + module + "\"}]}";
    writeText(g_plugins / "installed.json", manifest);
    const std::string manifestBefore = readText(g_plugins / "installed.json");

    // REMOVE: the entries leave the list, the kept copy is deleted (on a worker: the pass collects it), the install stays.
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, urlA, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_CATALOGUE) == 2 && !Access::cleanupPending(d.eng); }));
    CHECK_EQ(fact(local, "store.sites"), std::string());
    CHECK(!fs::exists(g_plugins / PluginRepo::siteCacheFileName(urlA)));
    CHECK(!fs::exists(g_plugins / PluginRepo::siteCacheTimeFileName(urlA)));
    CHECK_EQ(readText(g_plugins / module), std::string("installed bytes"));
    CHECK_EQ(readText(g_plugins / "installed.json"), manifestBefore);
    CHECK_EQ(entryByKey(local, key).name[0], '\0');
    clearPlugins();
}

// ---- S2: a REMOTE session cannot fit a plugin from a site ----------------------------------------------------------------------------------

void testRemoteCannotFitFromASite() {
    clearPlugins();
    const fs::path official = g_scratch / "official.json";
    const fs::path siteA = g_scratch / "site-a.json";
    writeText(official, indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid")}));
    writeText(siteA, indexJson({entryJson("hdradio", "0.1.0", "hobbyist.example")}));
    const std::string urlA = siteA.string();
    const std::string key = "hdradio@" + PluginRepo::siteLabel(urlA);

    // Every download answers "HTTP 503": a start that is let through shows as one request and that reason; one that is refused shows as none.
    auto net = std::make_shared<Net>();
    serve(net);

    Direct d;
    testing::Session local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    CHECK(local.ok());
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, official.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, urlA, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_CATALOGUE) == 2 && count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    WebSession web(d.eng);
    CHECK(web.s != nullptr);
    const int hits0 = net->hits.load();

    // THE WEB PAGE'S ROWS say it too: the site's row carries the refusal as its reason, the official row has none.
    {
        const auto status = d.eng.webStatusNow();
        bool site = false, official1 = false;
        for (const auto& c : status.catalogue) {
            if (c.id == "hdradio") {
                site = true;
                CHECK_EQ(c.blockedReason, kSiteRefusal);
            }
            if (c.id == "p1") {
                official1 = true;
                CHECK(c.blockedReason.empty());
            }
        }
        CHECK(site && official1);
    }

    // BY THE BARE ID THE PAGE LISTS, and by the key, with the notice acknowledged: DENIED, the sentence, nothing started, nothing fetched.
    for (const std::string& name : {std::string("hdradio"), key}) {
        const testing::Outcome o = pumpedOn(d.eng, web.s, textCmd(FOXAPI_OP_STORE_INSTALL, name, 1));
        CHECK_EQ(o.status(), FOXAPI_DENIED);
        CHECK_EQ(fact(local, "status.installError"), kSiteRefusal);
        CHECK_EQ(count(local, FOXAPI_LIST_STORE_ACTIVITY), 0);
        passes(d.eng, 3);
        CHECK_EQ(net->hits.load(), hits0);
    }

    // THE SAME SESSION FITS AN OFFICIAL PLUGIN: the rule is about sites, not about the web page.
    {
        const testing::Outcome o = pumpedOn(d.eng, web.s, textCmd(FOXAPI_OP_STORE_INSTALL, "p1", 1));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
        CHECK_EQ(net->hits.load(), hits0 + 1);
        CHECK(fact(local, "status.installError").find("HTTP 503") != std::string::npos);
    }

    // THE DESKTOP FITS THE SITE'S BY ITS KEY - and only with the notice acknowledged.
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, key, 0)).status(), FOXAPI_DENIED);
    CHECK(fact(local, "status.installError").find("legal notice") != std::string::npos);
    CHECK_EQ(net->hits.load(), hits0 + 1);
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, key, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    CHECK_EQ(net->hits.load(), hits0 + 2);
    CHECK(fact(local, "status.installError").find("HTTP 503") != std::string::npos);
    // ...and the bare id of a site's plugin names nothing to the desktop (the key does).
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, "hdradio", 1)).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(net->hits.load(), hits0 + 2);
    PluginRepo::setTransportForTest(nullptr);
    clearPlugins();
}

// ---- S3: the kept copy -----------------------------------------------------------------------------------------------------------------------

void testKeptCopy() {
    clearPlugins();
    const fs::path official = g_scratch / "official.json";
    const std::string text = indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid"), entryJson("p2", "1.0.0", "plugins.example.invalid")});
    writeText(official, text);
    std::string cacheTime;
    {
        Direct d;
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
        CHECK_EQ(fact(s, "store.fromCache"), std::string("0"));
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, official.string())).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 2 && count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
        // A GOOD READ WRITES THE COPY, exactly as read, with the time - and is a read of THIS session.
        CHECK(fs::is_regular_file(g_plugins / "catalogue.json"));
        CHECK_EQ(readText(g_plugins / "catalogue.json"), text);
        CHECK_EQ(fact(s, "store.fromCache"), std::string("0"));
        CHECK_EQ(fact(s, "store.readThisSession"), std::string("1"));
        CHECK_EQ(fact(s, "store.refreshFailed"), std::string("0"));
        cacheTime = fact(s, "store.cacheTime");
        CHECK(cacheTime != "0" && !cacheTime.empty());

        // A READ THAT FAILS EMPTIES NOTHING: the list stays, the reason is beside it, the time is the old one.
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, (g_scratch / "no-such-catalogue.json").string())).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0 && !fact(s, "status.catalogError").empty(); }));
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 2);
        CHECK_EQ(fact(s, "store.refreshFailed"), std::string("1"));
        CHECK_EQ(fact(s, "store.cacheTime"), cacheTime);
        CHECK_EQ(fact(s, "store.readThisSession"), std::string("1"));  // an earlier read of this session stays what it was

        // A DOCUMENT THAT IS NOT A CATALOGUE is refused and leaves the kept copy exactly as it was.
        const fs::path junk = g_scratch / "junk.json";
        writeText(junk, "this is not a catalogue");
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, junk.string())).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 2);
        CHECK_EQ(readText(g_plugins / "catalogue.json"), text);
        CHECK_EQ(readText(g_plugins / "catalogue.json.time"), cacheTime + "\n");
    }
    // A NEW ENGINE OPENS WITH IT and fetches nothing: the list is there before any request, from the cache, with the time it was read.
    {
        cascade::core::DiagLog::instance().resetForTest();
        Direct d;
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        passes(d.eng, 3);
        CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 2);
        CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
        CHECK_EQ(fact(s, "store.fromCache"), std::string("1"));
        CHECK_EQ(fact(s, "store.readThisSession"), std::string("0"));  // the first open still asks the network once
        CHECK_EQ(fact(s, "store.cacheTime"), cacheTime);
        std::size_t lines = 0;
        for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
            if (line.find("plugin store: kept catalogue loaded - 2 plugins") != std::string::npos) { ++lines; }
        }
        CHECK_EQ(lines, 1u);
        // ...and a refresh that fails there keeps the kept copy, with its time.
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, (g_scratch / "no-such-catalogue.json").string())).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0 && !fact(s, "status.catalogError").empty(); }));
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 2);
        CHECK_EQ(fact(s, "store.fromCache"), std::string("1"));
        CHECK_EQ(fact(s, "store.cacheTime"), cacheTime);
    }
    clearPlugins();
}

// ---- S4: the pictures ----------------------------------------------------------------------------------------------------------------------

void testPictures() {
    clearPlugins();
    const std::string body1(50000, '1'), body2(60000, '2');
    const Shot s1{"https://pics.example.invalid/p1-a.png", body1}, s2{"https://pics.example.invalid/p1-b.png", body2};
    const fs::path official = g_scratch / "official.json";
    writeText(official, indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid", {s1, s2}), entryJson("p2", "1.0.0", "plugins.example.invalid")}));
    auto net = std::make_shared<Net>();
    net->bodies[s1.url] = body1;
    net->bodies[s2.url] = body2;
    serve(net);

    Direct d;
    testing::Session local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(local.ok() && remote.ok());
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, official.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_CATALOGUE) == 2 && count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    const std::string sha1 = sha256Of(body1), sha2 = sha256Of(body2);

    // NEVER REQUESTED: nothing in the table, nothing fetched. The entry names its pictures (sha256 TAB caption).
    CHECK_EQ(count(local, FOXAPI_LIST_STORE_PICTURES), 0);
    CHECK_EQ(net->hits.load(), 0);
    CHECK_EQ(fact(local, "store.p1.pictures"), sha1 + "\tpicture 0\n" + sha2 + "\tpicture 1");
    CHECK_EQ(fact(local, "store.p2.pictures"), std::string());
    // AN UNKNOWN KEY queues nothing.
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "no-such-plugin")).status(), FOXAPI_NOT_FOUND);
    // A REMOTE SESSION cannot start a transfer to an address a stranger chose.
    CHECK_EQ(pumped(d.eng, remote, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p1")).status(), FOXAPI_DENIED);
    CHECK_EQ(count(local, FOXAPI_LIST_STORE_PICTURES), 0);

    // BOTH ARE PENDING AT ONCE, then Ready with a path, ONE TRANSFER AT A TIME.
    net->hold = true;
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p1")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return Access::screenshotPending(d.eng) && net->inFlight == 1; }));
    CHECK_EQ(count(local, FOXAPI_LIST_STORE_PICTURES), 2);
    int pending = 0, active = 0;
    for (int32_t i = 0; i < 2; ++i) {
        const FoxListItem it = item(local, FOXAPI_LIST_STORE_PICTURES, static_cast<uint32_t>(i));
        CHECK_EQ(it.value[0], 1.0);
        ++pending;
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { ++active; }
    }
    CHECK_EQ(pending, 2);
    CHECK_EQ(active, 1);  // the one in flight
    // WHILE ONE IS IN FLIGHT everything else sees "busy": the gate, a site's ADD, a clean-up, and a catalogue read that does not start.
    CHECK_EQ(fact(local, "store.p2.blocked"), std::string("a transfer is already in progress"));
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_SITE, (g_scratch / "x.json").string(), 0, 1)).status(), FOXAPI_BUSY);
    CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPP_OP_STORE_CLEAN_OLD, 0, 0)).status(), FOXAPI_BUSY);
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, "p2", 1)).status(), FOXAPI_DENIED);
    CHECK(fact(local, "status.installError").find("transfer is already in progress") != std::string::npos);
    CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(!Access::catalogPending(d.eng));  // it did not start
    net->hold = false;
    CHECK(passUntil(d.eng, [&] {
        return count(local, FOXAPI_LIST_STORE_PICTURES) == 2 && item(local, FOXAPI_LIST_STORE_PICTURES, 0).value[0] == 2.0 &&
               item(local, FOXAPI_LIST_STORE_PICTURES, 1).value[0] == 2.0;
    }));
    CHECK_EQ(net->hits.load(), 2);
    CHECK_EQ(net->maxInFlight.load(), 1);
    for (int32_t i = 0; i < 2; ++i) {
        const FoxListItem it = item(local, FOXAPI_LIST_STORE_PICTURES, static_cast<uint32_t>(i));
        const std::string sha = it.name;
        CHECK(sha == sha1 || sha == sha2);
        CHECK_EQ(std::string(it.detail), PluginRepo::screenshotCachePath(g_plugins.string(), sha));
        CHECK_EQ(fact(local, "store.picture." + sha + ".path"), std::string(it.detail));
        CHECK_EQ(readText(it.detail), sha == sha1 ? body1 : body2);
        CHECK(it.id != 0u);
        // The cached file's path is this machine's: a REMOTE session sees the row and not the path.
        const FoxListItem r = item(remote, FOXAPI_LIST_STORE_PICTURES, static_cast<uint32_t>(i));
        CHECK_EQ(std::string(r.name), sha);
        CHECK_EQ(std::string(r.detail), std::string());
    }
    int32_t st = 0;
    fact(remote, "store.picture." + sha1 + ".path", &st);
    CHECK_EQ(st, FOXAPI_DENIED);
    // A READY PICTURE IS NOT ASKED FOR AGAIN, however often the page says so (a retry included).
    for (int i = 0; i < 3; ++i) { CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p1", 1)).status(), FOXAPI_OK); }
    passes(d.eng, 6);
    CHECK_EQ(net->hits.load(), 2);

    // A CATALOGUE THAT NO LONGER NAMES A PICTURE DROPS ITS STATUS AND ITS FILE, and only those.
    writeText(official, indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid", {s1}), entryJson("p2", "1.0.0", "plugins.example.invalid")}));
    CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0 && count(local, FOXAPI_LIST_STORE_PICTURES) == 1; }));
    CHECK_EQ(std::string(item(local, FOXAPI_LIST_STORE_PICTURES, 0).name), sha1);
    CHECK(fs::exists(PluginRepo::screenshotCachePath(g_plugins.string(), sha1)));
    CHECK(!fs::exists(PluginRepo::screenshotCachePath(g_plugins.string(), sha2)));

    // A FAILURE, ASKED AGAIN ONLY ON A RETRY: a picture whose server answers an error.
    const std::string body3(4000, '3');
    const Shot s3{"https://pics.example.invalid/p3.png", body3};
    writeText(official, indexJson({entryJson("p3", "1.0.0", "plugins.example.invalid", {s3})}));
    net->failing.insert(s3.url);
    CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0 && count(local, FOXAPI_LIST_CATALOGUE) == 1; }));
    const int before = net->hits.load();
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p3")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_STORE_PICTURES) == 1 && item(local, FOXAPI_LIST_STORE_PICTURES, 0).value[0] == 3.0; }));
    CHECK_EQ(std::string(item(local, FOXAPI_LIST_STORE_PICTURES, 0).detail), std::string("the server returned HTTP 404"));
    CHECK_EQ(fact(local, "store.picture." + sha256Of(body3) + ".reason"), std::string("the server returned HTTP 404"));
    CHECK_EQ(net->hits.load(), before + 1);
    CHECK(!fs::exists(PluginRepo::screenshotCachePath(g_plugins.string(), sha256Of(body3))));
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p3")).status(), FOXAPI_OK);  // the per-frame call: no retry
    passes(d.eng, 6);
    CHECK_EQ(net->hits.load(), before + 1);
    net->failing.clear();
    net->bodies[s3.url] = body3;
    CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p3", 1)).status(), FOXAPI_OK);  // the page opened again
    CHECK(passUntil(d.eng, [&] { return item(local, FOXAPI_LIST_STORE_PICTURES, 0).value[0] == 2.0; }));
    CHECK_EQ(net->hits.load(), before + 2);

    // THE PICTURES WAIT BEHIND A CATALOGUE READ: asked for while one is in flight (held in the transport), they are PENDING - queued, none in
    // flight, nothing fetched - and go, one at a time, when it has landed.
    {
        const std::string body4(3000, '4'), body5(3500, '5');
        const Shot s4{"https://pics.example.invalid/p4-a.png", body4}, s5{"https://pics.example.invalid/p4-b.png", body5};
        const std::string index = indexJson({entryJson("p4", "1.0.0", "plugins.example.invalid", {s4, s5})});
        writeText(official, index);
        CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);  // the catalogue the pictures belong to
        CHECK(passUntil(d.eng, [&] { return count(local, FOXAPI_LIST_STORE_ACTIVITY) == 0 && count(local, FOXAPI_LIST_STORE_PICTURES) == 0; }));
        net->bodies["https://catalogue.example.invalid/index.json"] = index;
        net->bodies[s4.url] = body4;
        net->bodies[s5.url] = body5;
        net->hold = true;
        const int hits0 = net->hits.load();
        CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, "https://catalogue.example.invalid/index.json")).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);  // held in the transport
        CHECK(passUntil(d.eng, [&] { return Access::catalogPending(d.eng) && net->inFlight == 1; }));
        CHECK_EQ(pumped(d.eng, local, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p4")).status(), FOXAPI_OK);
        passes(d.eng, 5);
        CHECK_EQ(count(local, FOXAPI_LIST_STORE_PICTURES), 2);  // both are pending
        for (int32_t i = 0; i < 2; ++i) {
            const FoxListItem it = item(local, FOXAPI_LIST_STORE_PICTURES, static_cast<uint32_t>(i));
            CHECK_EQ(it.value[0], 1.0);
            CHECK_EQ(it.flags & FOXAPI_ITEM_ACTIVE, 0u);  // none in flight
        }
        CHECK(!Access::screenshotPending(d.eng));
        CHECK_EQ(Access::queued(d.eng), 2u);
        CHECK_EQ(net->hits.load(), hits0 + 1);  // the catalogue's request, and not one picture's
        net->hold = false;
        CHECK(passUntil(d.eng, [&] {
            return count(local, FOXAPI_LIST_STORE_PICTURES) == 2 && item(local, FOXAPI_LIST_STORE_PICTURES, 0).value[0] == 2.0 &&
                   item(local, FOXAPI_LIST_STORE_PICTURES, 1).value[0] == 2.0;
        }));
        CHECK_EQ(net->hits.load(), hits0 + 3);  // the catalogue, then each picture once
        CHECK_EQ(net->maxInFlight.load(), 1);
    }

    // (A site's picture on another host is never requested: the second look is held in tests/test_plugin_catalogue_app.cpp, over a list made by
    // hand - nothing that came through mergeSite could carry one.)
    PluginRepo::setTransportForTest(nullptr);
    clearPlugins();
}

// ---- S5: no pass waits on a worker --------------------------------------------------------------------------------------------------

// Passes while `held` stays true; the longest one, in milliseconds.
double longestPassWhile(cascade::engine::Engine& eng, int n) {
    double worst = 0.0;
    for (int i = 0; i < n; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        eng.controlPass();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worst = std::max(worst, ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return worst;
}

void testNoPassWaitsOnAWorker() {
    clearPlugins();
    // ONE ENGINE'S PASS, with a worker held in each of the three places the store does its work: the network of a catalogue read, the network
    // of a picture, and the disk of a clean-up. The bound is generous (a pass that waited on a held worker would wait for ever); what it
    // proves is that every pass in the hold returns at once and the pass that follows the release collects the result.
    const std::string body(2000, 'x');
    const Shot shot{"https://pics.example.invalid/hold.png", body};
    const std::string index = indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid", {shot})});
    auto net = std::make_shared<Net>();
    net->bodies["https://catalogue.example.invalid/index.json"] = index;
    net->bodies[shot.url] = body;
    serve(net);

    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    constexpr double kBoundMs = 400.0;

    // (a) A CATALOGUE READ held in the transport.
    net->hold = true;
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, "https://catalogue.example.invalid/index.json")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(Access::catalogPending(d.eng));
    const double a = longestPassWhile(d.eng, 40);
    std::printf("  S5 a: 40 passes with a catalogue read held, the longest %.2f ms\n", a);
    CHECK(a < kBoundMs);
    CHECK(Access::catalogPending(d.eng));  // still held: the passes did not wait for it, and did not lose it
    net->hold = false;
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 1 && count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));

    // (b) A PICTURE held in the transport.
    net->hold = true;
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_STORE_PICTURES_WANT, "p1")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return Access::screenshotPending(d.eng); }));
    const double b = longestPassWhile(d.eng, 40);
    std::printf("  S5 b: 40 passes with a picture held, the longest %.2f ms\n", b);
    CHECK(b < kBoundMs);
    CHECK(Access::screenshotPending(d.eng));
    net->hold = false;
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_PICTURES) == 1 && item(s, FOXAPI_LIST_STORE_PICTURES, 0).value[0] == 2.0; }));
    PluginRepo::setTransportForTest(nullptr);

    // (c) A CLEAN-UP held in the remover: the old copy is a real file the worker is about to delete.
    const std::string oldFile = std::string("old-1.0.0") + kModExt, newFile = std::string("old-1.1.0") + kModExt;
    writeText(g_plugins / oldFile, "old");
    writeText(g_plugins / newFile, "new");
    Access::setOldCopies(d.eng, {{oldFile, "Old", "1.0.0", newFile, "1.1.0"}});
    g_removeHeld = true;
    g_removeEntered = 0;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_STORE_CLEAN_OLD, 0, 0)).status(), FOXAPI_OK);
    CHECK(Access::cleanupPending(d.eng));
    CHECK(passUntil(d.eng, [&] { return g_removeEntered.load() == 1; }));  // the worker is inside the delete
    const double c = longestPassWhile(d.eng, 40);
    std::printf("  S5 c: 40 passes with a clean-up held in the delete, the longest %.2f ms\n", c);
    CHECK(c < kBoundMs);
    CHECK(Access::cleanupPending(d.eng));
    CHECK(fs::exists(g_plugins / oldFile));  // not deleted yet: the worker is held
    g_removeHeld = false;
    CHECK(passUntil(d.eng, [&] { return !Access::cleanupPending(d.eng); }));
    CHECK(!fs::exists(g_plugins / oldFile));
    CHECK(fs::exists(g_plugins / newFile));  // the NEWER copy is never touched
    CHECK_EQ(readText(g_plugins / newFile), std::string("new"));
    clearPlugins();
}

// ---- S6: the sites at start-up -----------------------------------------------------------------------------------------------------------

void testSitesAtStartUp() {
    clearPlugins();
    const fs::path official = g_scratch / "official.json";
    const fs::path siteA = g_scratch / "site-a.json";
    const fs::path siteB = g_scratch / "site-b.json";
    const std::string officialText = indexJson({entryJson("p1", "1.0.0", "plugins.example.invalid"), entryJson("p2", "1.0.0", "plugins.example.invalid")});
    const std::string textA = indexJson({entryJson("hdradio", "0.1.0", "hobbyist.example"), entryJson("p2", "9.9.9", "hobbyist.example")});
    writeText(official, officialText);
    writeText(siteA, textA);
    writeText(siteB, indexJson({entryJson("other", "0.2.0", "other.example")}));
    // The kept copies the last session left: the official one and site A's (site B was never read).
    std::string err;
    CHECK(PluginRepo::saveCatalogueCache(g_plugins.string(), officialText, 1760001234, err));
    CHECK(PluginRepo::saveSiteCache(g_plugins.string(), siteA.string(), textA, 1760005678, err));

    auto net = std::make_shared<Net>();
    serve(net);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 2);  // the official kept copy only: no site is known yet
    cascade::core::AppConfig cfg;
    cfg.pluginSites = {siteA.string(), siteB.string()};
    cfg.pluginCatalogueUrl = official.string();
    d.eng.applyConfig(cfg);
    passes(d.eng, 3);
    // Site A's kept copy is merged under the official list (the clash with p2 hidden); site B was never read, so it is "not read yet".
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 3);
    CHECK_EQ(fact(s, "store.sites"), siteA.string() + "\t1\t1\t1\t0\t\n" + siteB.string() + "\t0\t0\t0\t0\t");
    CHECK_EQ(fact(s, "store.fromCache"), std::string("1"));
    CHECK_EQ(fact(s, "store.readThisSession"), std::string("0"));
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    CHECK_EQ(net->hits.load(), 0);  // no request was made
    CHECK(!Access::catalogPending(d.eng));
    // The configuration the engine writes carries them, in order.
    cascade::core::AppConfig out;
    d.eng.fillConfig(out);
    CHECK(out.pluginSites == cfg.pluginSites);
    PluginRepo::setTransportForTest(nullptr);
    clearPlugins();
}

}  // namespace

// ---- S7: a card's facts, how an entry stands against what is installed, and UPDATE ALL (stub campaign B4) ---------------------------------------

// `entryJson` with more fields in it (a raw JSON fragment without its trailing comma).
std::string richEntryJson(const std::string& id, const std::string& version, const std::string& extra) {
    std::string t = entryJson(id, version, "plugins.example.invalid");
    const std::string marker = "\"licence\":\"MIT\",";
    const std::size_t at = t.find(marker);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) { t.insert(at + marker.size(), extra + ","); }
    return t;
}

void testCardFacts() {
    clearPlugins();
    // THREE PLUGINS INSTALLED, by their records: p-up at 1.0.0 (the catalogue has 2.0.0: the file the entry would write is NOT on disk),
    // p-same at 1.0.0 (the catalogue has it), p-new at 3.0.0 (the catalogue has 2.0.0: a NEWER copy is installed). The files are on disk (a
    // record whose file the user deleted is not an installed plugin); they are not real modules, which is no matter for the records' sake.
    std::string manifest = "{\"schemaVersion\":1,\"plugins\":[";
    const struct {
        const char* id;
        const char* version;
    } installed[] = {{"p-up", "1.0.0"}, {"p-same", "1.0.0"}, {"p-new", "3.0.0"}};
    for (std::size_t i = 0; i < 3; ++i) {
        const std::string file = std::string(installed[i].id) + "-" + installed[i].version + kModExt;
        writeText(g_plugins / file, "bytes");
        manifest += std::string(i == 0 ? "" : ",") + "{\"id\":\"" + installed[i].id + "\",\"name\":\"Plugin " + installed[i].id + "\",\"version\":\"" +
                    installed[i].version + "\",\"file\":\"" + file + "\",\"abiVersion\":" + std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) +
                    ",\"installedAt\":1790000000}";
    }
    writeText(g_plugins / "installed.json", manifest + "]}");
    const fs::path official = g_scratch / "official.json";
    writeText(official,
              indexJson({richEntryJson("p-up", "2.0.0", "\"capabilities\":[\"CASCADE_CAP_NOT_IN_THIS_BUILD\"]"),
                         entryJson("p-same", "1.0.0", "plugins.example.invalid"), entryJson("p-new", "2.0.0", "plugins.example.invalid"),
                         richEntryJson("p-none", "1.0.0",
                                       "\"category\":\"marine\",\"experimental\":true,\"whatsNew\":\"1.0.0: the first release.\","
                                       "\"published\":\"2026-10-07\",\"capabilities\":[\"CASCADE_CAP_DECODER\",\"CASCADE_CAP_NOT_IN_THIS_BUILD\"]")}));
    auto net = std::make_shared<Net>();  // (nothing is served: every download is answered HTTP 503 at once)
    serve(net);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(s.ok() && remote.ok());
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, official.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 4 && count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    passes(d.eng, 2);

    // THE SHOP-WINDOW FIELDS: every key for every entry, "" when the index said nothing (known and empty, never NOT_FOUND).
    int32_t st = 0;
    CHECK_EQ(fact(s, "store.p-none.category", &st), std::string("marine"));
    CHECK_EQ(fact(s, "store.p-none.experimental"), std::string("1"));
    CHECK_EQ(fact(s, "store.p-none.whatsNew"), std::string("1.0.0: the first release."));
    CHECK_EQ(fact(s, "store.p-none.published"), std::string("2026-10-07"));
    CHECK_EQ(fact(s, "store.p-none.sha256"), std::string(64, 'a'));  // this host's build's digest
    // ...the capabilities: the bit of the name this build knows (the unknown one sets none), and for an entry that names ONLY ones this build
    // has not heard of, 0x80000000 - "declares one this build does not know", not "declares nothing".
    CHECK_EQ(fact(s, "store.p-none.capabilities"), std::to_string(static_cast<unsigned>(CASCADE_CAP_DECODER)));
    CHECK_EQ(fact(s, "store.p-up.capabilities"), std::string("2147483648"));
    CHECK_EQ(fact(s, "store.p-same.category", &st), std::string());
    CHECK_EQ(st, 0);
    CHECK_EQ(fact(s, "store.p-same.experimental"), std::string("0"));
    CHECK_EQ(fact(s, "store.p-same.capabilities", &st), std::string());  // the index said nothing: "not declared", not "declares none"
    CHECK_EQ(st, 0);
    CHECK_EQ(fact(s, "store.p-same.published"), std::string());

    // HOW EACH STANDS AGAINST WHAT IS INSTALLED, by id and version - never by the file's name.
    CHECK_EQ(fact(s, "store.p-up.installState"), std::string("update"));
    CHECK_EQ(fact(s, "store.p-up.installedVersion"), std::string("1.0.0"));
    CHECK_EQ(fact(s, "store.p-same.installState"), std::string("installed"));
    CHECK_EQ(fact(s, "store.p-same.installedVersion"), std::string("1.0.0"));
    CHECK_EQ(fact(s, "store.p-new.installState"), std::string("newer"));
    CHECK_EQ(fact(s, "store.p-new.installedVersion"), std::string("3.0.0"));
    CHECK_EQ(fact(s, "store.p-none.installState"), std::string("none"));
    CHECK_EQ(fact(s, "store.p-none.installedVersion"), std::string());
    // (the module file of the installed copy joins the entry to PLUGINS: none of these records is a loaded module, so there is none to name)
    CHECK_EQ(fact(s, "store.p-up.installedFile", &st), std::string());
    CHECK_EQ(st, 0);
    // INSTALLED means installed in ANY version (0.99.72): the update's file has never been on disk, and the entry is installed all the same, and
    // GET on it is refused - "already installed" - where the file-name test alone would have offered a second install.
    CHECK(entryByKey(s, "p-up").flags & FOXAPI_ITEM_INSTALLED);
    CHECK(entryByKey(s, "p-same").flags & FOXAPI_ITEM_INSTALLED);
    CHECK(entryByKey(s, "p-new").flags & FOXAPI_ITEM_INSTALLED);
    CHECK((entryByKey(s, "p-none").flags & FOXAPI_ITEM_INSTALLED) == 0u);
    CHECK_EQ(fact(s, "store.p-up.blocked"), std::string("already installed"));
    CHECK_EQ(fact(s, "store.p-new.blocked"), std::string("already installed"));
    CHECK_EQ(fact(s, "store.p-none.blocked"), std::string());
    // ...and the plan is the one update.
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_UPDATES), 1);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_STORE_UPDATES, 0).name), std::string("p-up"));
    // The facts are no secret from a browser: a REMOTE session reads them (only the site's address and the file paths are LOCAL).
    CHECK_EQ(fact(remote, "store.p-none.category"), std::string("marine"));
    CHECK_EQ(fact(remote, "store.p-up.installState"), std::string("update"));

    // UPDATE ALL (the 0.99.72 key: FOXAPP_OP_STORE_ADD_ALL's ival[1] 1): the same queue with NOTHING TO FETCH in it. Over this catalogue ADD ALL
    // is one to fetch (p-none) and one to update (p-up); UPDATE ALL is the update alone.
    const auto runLine = [&](int ival1) {
        cascade::core::DiagLog::instance().resetForTest();
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_STORE_ADD_ALL, "", 0, ival1)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0 && !fact(s, "status.addAllSummary").empty(); }));
        std::string line;
        for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
            if (l.find("plugin store: add all starting") != std::string::npos) { line = l; }
        }
        return line;
    };
    const std::string updateOnly = runLine(1);
    CHECK(updateOnly.find("add all starting - 0 to fetch, 1 to update, 0 passed over") != std::string::npos);
    const std::string both = runLine(0);
    CHECK(both.find("add all starting - 1 to fetch, 1 to update") != std::string::npos);
    // NOTHING TO UPDATE: a catalogue with no update in it - UPDATE ALL starts nothing and says why, in its own words; ADD ALL there still fetches.
    writeText(official, indexJson({entryJson("p-same", "1.0.0", "plugins.example.invalid"), entryJson("p-none", "1.0.0", "plugins.example.invalid")}));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 2 && count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_UPDATES), 0);
    cascade::core::DiagLog::instance().resetForTest();
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_STORE_ADD_ALL, "", 0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    CHECK_EQ(fact(s, "status.installError"), std::string("every module in the catalogue is already fitted, and none has a newer build"));
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) { CHECK(l.find("add all starting") == std::string::npos); }
    PluginRepo::setTransportForTest(nullptr);
    clearPlugins();
}

int main() {
    std::printf("test_engine_api_store\n");
    isolate();
    testSitesThroughTheTable();
    testRemoteCannotFitFromASite();
    testKeptCopy();
    testPictures();
    testNoPassWaitsOnAWorker();
    testSitesAtStartUp();
    testCardFacts();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_store");
}
