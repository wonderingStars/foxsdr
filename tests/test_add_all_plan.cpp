// test_add_all_plan.cpp - ADD ALL planned by the ENGINE (engine/stage3b-pre,
// docs/engine-stage3.md OPEN 5).
//
// Until this round the run's plan was the store WINDOW's: the engine asked
// its host (EngineHost::planAddAll), the window built its whole store model
// and planned over it, and a headless engine - no window to ask - refused ADD
// ALL outright. Now the engine plans from its own catalogue and inventory with
// the SAME rule the window's key is lettered from (core/add_all_plan.hpp),
// and the command carries only what the engine cannot know: whether every
// maker's notice is acknowledged (the ADD ALL tick), and the one module whose
// own notice tick is on.
//
// No window here at all - an Engine alone, with a catalogue of three:
//   plain     no notice                      -> always added
//   noticed   a maker's notice                -> added only when acknowledged
//   elsewhere no build for this machine       -> never added, named as skipped
//
//   A  FOXAPI_OP_STORE_UPDATE_ALL, not acknowledged: the run is {plain}
//   B  ...acknowledged: {plain, noticed}
//   C  FOXAPP_OP_STORE_ADD_ALL, not acknowledged, but noticed's own tick on:
//      {plain, noticed}; with ANOTHER module's tick on: {plain}
//   D  no catalogue: refused, and the reason is the window's own sentence
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

using cascade::engine::Engine;
namespace cmd = cascade::core::cmd;

namespace cascade::gui {
struct AppWindowTestAccess {
    static void catalogue(Engine& e) {
        e.catalog_.clear();
        for (int k = 0; k < 3; ++k) {
            cascade::core::PluginCatalogEntry c;
            c.id = k == 0 ? "plain" : k == 1 ? "noticed" : "elsewhere";
            c.name = k == 0 ? "Plain" : k == 1 ? "Noticed" : "Elsewhere";
            c.version = "1.0.0";
            c.licence = "MIT";
            if (k == 1) { c.legalNotice = "a maker's notice"; }
            c.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
            cascade::core::PluginPlatform pf;
            pf.os = k == 2 ? std::string("no-such-os") : cascade::core::PluginRepo::hostOs();
            pf.arch = cascade::core::PluginRepo::hostArch();
            pf.file = c.id + ".dll";
            pf.url = "https://127.0.0.1:9/" + c.id + ".dll";
            pf.sha256 = std::string(64, 'a');
            c.platforms.push_back(pf);
            c.compatible = k != 2;
            e.catalog_.push_back(c);
        }
        e.catalogStatus_ = "3 plugins";
    }
    static void noCatalogue(Engine& e) {
        e.catalog_.clear();
        e.catalogStatus_.clear();
        e.catalogError_.clear();
        e.pluginInventory_.plugins.clear();
    }
    // THE SITE SCENARIO (deep review of engine/stage4, batch A): two official plugins and two from a site the user added (B3 moved the sites into the
    // engine: their entries are in catalog_ with `source` set). plain: fitted at 0.9.0, 1.0.0 offered. noticed: not fitted, a maker's notice.
    // site-new: from the site, not fitted. site-fitted: from the site, fitted at 1.0.0, 2.0.0 offered.
    static void siteScenario(Engine& e) {
        e.catalog_.clear();
        e.pluginInventory_.plugins.clear();
        const std::string site = "https://plugins.example.test/index.json";
        struct Row {
            const char* id;
            const char* version;
            bool fromSite;
            bool notice;
        };
        const Row rows[] = {{"plain", "1.0.0", false, false}, {"noticed", "1.0.0", false, true}, {"site-new", "1.0.0", true, false}, {"site-fitted", "2.0.0", true, false}};
        for (const Row& r : rows) {
            cascade::core::PluginCatalogEntry c;
            c.id = r.id;
            c.name = r.id;
            c.version = r.version;
            c.licence = "MIT";
            if (r.notice) { c.legalNotice = "a maker's notice"; }
            if (r.fromSite) { c.source = site; }
            c.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
            cascade::core::PluginPlatform pf;
            pf.os = cascade::core::PluginRepo::hostOs();
            pf.arch = cascade::core::PluginRepo::hostArch();
            pf.file = std::string(r.id) + ".dll";
            pf.url = std::string("https://127.0.0.1:9/") + r.id + ".dll";
            pf.sha256 = std::string(64, 'a');
            c.platforms.push_back(pf);
            c.compatible = true;
            e.catalog_.push_back(c);
        }
        const auto record = [&](const char* id, const char* source, const char* version) {
            cascade::core::InstalledPlugin ip;
            ip.id = id;
            ip.source = source;
            ip.name = id;
            ip.version = version;
            ip.file = std::string(id) + ".dll";
            ip.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
            e.pluginInventory_.plugins.push_back(ip);
        };
        record("plain", "", "0.9.0");
        record("site-fitted", site.c_str(), "1.0.0");
        e.catalogStatus_ = "4 plugins";
    }
    static std::vector<std::string> runIds(Engine& e) { return e.addAllRun_.active ? e.addAllRun_.ids : std::vector<std::string>{}; }
    static void stop(Engine& e) {
        e.addAllRun_ = Engine::AddAllRun{};
        e.installError_.clear();
    }
    static const std::string& installError(Engine& e) { return e.installError_; }
    static std::vector<cascade::core::PluginCatalogEntry>& catalogOf(Engine& e) { return e.catalog_; }
    static std::vector<cascade::core::InstalledPlugin>& inventoryOf(Engine& e) { return e.pluginInventory_.plugins; }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

namespace {
void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) { s += (s.empty() ? "" : ",") + x; }
    return "{" + s + "}";
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_add_all_plan\n");
#if defined(_WIN32)
    const std::string tmp = std::string(std::getenv("TEMP") != nullptr ? std::getenv("TEMP") : ".") +
                            "\\foxsdr_add_all_" + std::to_string(_getpid());
#else
    const std::string tmp = "/tmp/foxsdr_add_all_" + std::to_string(static_cast<int>(getpid()));
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    Engine e;
    e.initialise();
    A::catalogue(e);

    // A
    (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_STORE_UPDATE_ALL, 0));
    std::printf("A: UPDATE_ALL, not acknowledged: %s (error \"%s\")\n", join(A::runIds(e)).c_str(),
                A::installError(e).c_str());
    CHECK(A::runIds(e) == std::vector<std::string>({"plain"}));
    A::stop(e);

    // B
    (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_STORE_UPDATE_ALL, 1));
    std::printf("B: UPDATE_ALL, acknowledged: %s\n", join(A::runIds(e)).c_str());
    CHECK(A::runIds(e) == std::vector<std::string>({"plain", "noticed"}));
    A::stop(e);

    // C
    {
        const cmd::QueuedCommand q = cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, "noticed", 0);
        (void)e.applyCommand(q.c, q.longText);
        std::printf("C: ADD_ALL, noticed's own tick: %s\n", join(A::runIds(e)).c_str());
        CHECK(A::runIds(e) == std::vector<std::string>({"plain", "noticed"}));
        A::stop(e);
        const cmd::QueuedCommand other = cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, "plain", 0);
        (void)e.applyCommand(other.c, other.longText);
        std::printf("   ADD_ALL, another module's tick: %s\n", join(A::runIds(e)).c_str());
        CHECK(A::runIds(e) == std::vector<std::string>({"plain"}));
        A::stop(e);
    }

    // E, F: A PLUGIN SITE'S ENTRIES ARE NEVER IN THE RUN (core/add_all_plan.hpp: "neither to fetch nor to update"). The window's key is lettered and counted
    // without them (it plans over its own model, where a site row is thirdParty), but the engine's rows never set the flag: a stranger's plugin that FoxSDR has
    // not reviewed was downloaded, hash-checked against the site's own catalogue and loaded on the strength of the one box that acknowledges the official
    // plugins' notices - and an already fitted one was replaced by whatever binary its site had published since, with no fresh consent.
    A::siteScenario(e);
    (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_STORE_UPDATE_ALL, 1));
    std::printf("F: UPDATE_ALL, acknowledged, with a site's plugin fitted and updated: %s\n", join(A::runIds(e)).c_str());
    // (the API's form is the whole key - what is not fitted is fetched, then what has a newer build is updated: the official `noticed`, then the official `plain`)
    CHECK(A::runIds(e) == std::vector<std::string>({"noticed", "plain"}));
    bool siteInF = false;
    for (const std::string& id : A::runIds(e)) { siteInF = siteInF || id.find('@') != std::string::npos; }
    CHECK(!siteInF);
    A::stop(e);
    {
        const cmd::QueuedCommand q = cmd::makeText(FOXAPP_OP_STORE_ADD_ALL, "", 1);
        (void)e.applyCommand(q.c, q.longText);
        std::printf("E: ADD_ALL, acknowledged, with a site's plugins in the catalogue: %s\n", join(A::runIds(e)).c_str());
        CHECK(A::runIds(e) == std::vector<std::string>({"noticed", "plain"}));   // the installs first, then the updates; the site's two are in neither
        bool site = false;
        for (const std::string& id : A::runIds(e)) { site = site || id.find('@') != std::string::npos; }
        CHECK(!site);
        A::stop(e);
    }
    // ...and the same plugin from the OFFICIAL catalogue (no source) is in the run as ever: the exclusion is the site's, not the id's
    A::siteScenario(e);
    for (cascade::core::PluginCatalogEntry& c : A::catalogOf(e)) { c.source.clear(); }
    A::inventoryOf(e).back().source.clear();
    (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_STORE_UPDATE_ALL, 1));
    CHECK(A::runIds(e) == std::vector<std::string>({"noticed", "site-new", "plain", "site-fitted"}));   // the installs (catalogue order), then the updates
    A::stop(e);

    // D
    A::noCatalogue(e);
    (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_STORE_UPDATE_ALL, 1));
    std::printf("D: no catalogue: run %s, \"%s\"\n", join(A::runIds(e)).c_str(), A::installError(e).c_str());
    CHECK(A::runIds(e).empty());
    CHECK(A::installError(e).find("no catalogue has been read yet") != std::string::npos);

    return testSummary("test_add_all_plan");
}
