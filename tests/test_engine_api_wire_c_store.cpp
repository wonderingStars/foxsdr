// test_engine_api_wire_c_store.cpp - the Plugin store's side of GPL HELPER plugins in the real engine (wiring lane C group 7: FOXAPP_OP_STORE_HELPER_CONSENT, the install gate in
// engine.cpp, the store facts), over a LOCAL stand-in catalogue file and a stand-in transport. The install transaction itself is tests/test_plugin_repo_helper.cpp's.
//
//   S1  the catalogue lists a helper plugin with its badge ("GPL helper - source: ..."), its detail lines and helper.consent "0"; an ordinary plugin's helper facts are empty
//   S2  without the user's consent the install is refused (fact blocked names the sentence) and NOT ONE file is asked for
//   S3  the consent op: a REMOTE session is DENIED; an unknown key and a plugin that is not a helper are NOT_FOUND; consent is OK, again is NO_CHANGE, withdrawing is OK;
//       helper.consent follows
//   S4  with consent the install runs: the bridge and the helper's three files are beside each other in the plugins folder, nothing else
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
using cascade::core::PluginRepo;

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks(std::string (*dir)(), bool (*remove)(const std::string&, const std::string&, std::string&)) {
        Engine::testHooks_.pluginDir = dir;
        Engine::testHooks_.pluginRemove = remove;
    }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_scratch;
fs::path g_plugins;
std::string pluginDirHook() { return g_plugins.string(); }
bool realRemove(const std::string& dir, const std::string& file, std::string& error) {
    std::error_code ec;
    (void)error;
    fs::remove(fs::path(dir) / file, ec);
    return !ec;
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_wcstore_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
    g_plugins = g_scratch / "plugins";
    fs::create_directories(g_plugins, ec);
    Access::installHooks(&pluginDirHook, &realRemove);
}

std::string sha(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

// the program of THIS machine's kind, made in memory
std::string program() {
    const std::string os = PluginRepo::hostOs();
    const std::string arch = PluginRepo::hostArch();
    if (os == "windows") {
        std::string b(512, '\0');
        b[0] = 'M';
        b[1] = 'Z';
        b[0x3C] = static_cast<char>(0x80);
        std::memcpy(&b[0x80], "PE\0\0", 4);
        const unsigned m = arch == "arm64" ? 0xAA64u : arch == "x86" ? 0x014Cu : 0x8664u;
        b[0x84] = static_cast<char>(m & 0xFF);
        b[0x85] = static_cast<char>((m >> 8) & 0xFF);
        return b;
    }
    std::string b(64, '\0');
    b[0] = 0x7F;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 2;
    b[18] = static_cast<char>(arch == "arm64" ? 183 : 62);
    return b;
}

struct Net {
    std::mutex m;
    std::map<std::string, std::string> bodies;
    std::atomic<int> hits{0};
};

void serve(const std::shared_ptr<Net>& g) {
    PluginRepo::setTransportForTest([g](const std::string& url, std::uint64_t, const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
        ++g->hits;
        std::lock_guard<std::mutex> lk(g->m);
        const auto it = g->bodies.find(url);
        if (it == g->bodies.end()) {
            error = "the server returned HTTP 404";
            return false;
        }
        return sink(it->second.data(), it->second.size());
    });
}

std::string fact(testing::Session& s, const std::string& key) {
    char buf[8192];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string("<absent>");
}

template <class F>
bool passUntil(cascade::engine::Engine& eng, F done, int timeoutMs = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

FoxCommand textCmd(uint32_t op, const std::string& text, long long i0 = 0, long long i1 = 0) {
    FoxCommand c = testing::cmd(op, 0.0, i0, text.c_str());
    c.ival[1] = i1;
    return c;
}

testing::Outcome pumpedOn(cascade::engine::Engine& eng, FoxSession* session, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(session, &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 30 && !o.landed; ++pass) {
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

#if defined(_WIN32)
const char* const kModExt = ".dll";
#else
const char* const kModExt = ".so";
#endif

void testHelperStore() {
    const std::string prog = program();
    const std::string manifest = "{\"protocol\":1,\"name\":\"multimon-ng\",\"executable\":\"multimon-ng.exe\"}";
    const std::string licence = "GNU GENERAL PUBLIC LICENSE (stand-in)";
    const std::string bridge = "stand-in bridge";
    const std::string base = "https://dl.example.invalid/h/";
    const std::string bridgeFile = std::string("helper-bridge-multimon-1.0.0") + kModExt;
    auto net = std::make_shared<Net>();
    net->bodies[base + bridgeFile] = bridge;
    net->bodies[base + "multimon-ng.exe"] = prog;
    net->bodies[base + "multimon-ng.helper.json"] = manifest;
    net->bodies[base + "multimon-ng.LICENSE.txt"] = licence;
    const std::string commit(40, 'b');
    const std::string index =
        std::string("{\"schemaVersion\":1,\"plugins\":[") +
        "{\"id\":\"helper-bridge-multimon\",\"name\":\"Pager decoders\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":" + std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) +
        ",\"helper\":{\"kind\":\"gpl-helper\",\"name\":\"multimon-ng\",\"version\":\"1.4.1\",\"licence\":\"GPL-2.0-only\",\"sourceUrl\":\"https://example.invalid/mm/tree/" + commit + "\",\"sourceCommit\":\"" + commit +
        "\"},\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"" + bridgeFile + "\",\"url\":\"" + base + bridgeFile + "\",\"sha256\":\"" + sha(bridge) +
        "\",\"companions\":[" + "{\"role\":\"helper\",\"file\":\"multimon-ng.exe\",\"installAs\":\"multimon-ng.exe\",\"url\":\"" + base + "multimon-ng.exe\",\"sha256\":\"" + sha(prog) +
        "\",\"sizeBytes\":512,\"executable\":true}," + "{\"role\":\"manifest\",\"file\":\"multimon-ng.helper.json\",\"installAs\":\"multimon-ng.helper.json\",\"url\":\"" + base +
        "multimon-ng.helper.json\",\"sha256\":\"" + sha(manifest) + "\",\"executable\":false}," + "{\"role\":\"licence\",\"file\":\"multimon-ng.LICENSE.txt\",\"installAs\":\"multimon-ng.LICENSE.txt\",\"url\":\"" + base +
        "multimon-ng.LICENSE.txt\",\"sha256\":\"" + sha(licence) + "\",\"executable\":false}]}]}," +
        "{\"id\":\"plain\",\"name\":\"Plain\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":" + std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) + ",\"platforms\":[{\"os\":\"" +
        PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"plain-1.0.0" + kModExt + "\",\"url\":\"https://dl.example.invalid/plain\",\"sha256\":\"" + std::string(64, 'a') + "\"}]}]}";
    const fs::path indexFile = g_scratch / "index.json";
    {
        std::ofstream f(indexFile, std::ios::binary | std::ios::trunc);
        f << index;
    }
    serve(net);

    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    testing::Session local(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    testing::Session remote(api(), eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(local.ok() && remote.ok());
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, indexFile.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return api()->read_list(local.raw(), FOXAPI_LIST_CATALOGUE, 0, nullptr) == 2; }));

    // S1
    const std::string id = "helper-bridge-multimon";
    CHECK_EQ(fact(local, "store." + id + ".helper"), std::string("GPL helper - source: https://example.invalid/mm/tree/") + commit);
    const std::string detail = fact(local, "store." + id + ".helper.detail");
    CHECK(detail.find("separate program, multimon-ng 1.4.1 (GPL-2.0-only)") != std::string::npos);
    CHECK(detail.find("Source code: https://example.invalid/mm/tree/") != std::string::npos);
    CHECK(detail.find("multimon-ng.exe") != std::string::npos && detail.find("multimon-ng.helper.json") != std::string::npos && detail.find("multimon-ng.LICENSE.txt") != std::string::npos);
    CHECK_EQ(fact(local, "store." + id + ".helper.consent"), std::string("0"));
    CHECK_EQ(fact(local, "store.plain.helper"), std::string());
    CHECK_EQ(fact(local, "store.plain.helper.consent"), std::string());
    CHECK(fact(local, "store." + id + ".blocked").find("separate program") != std::string::npos);
    CHECK_EQ(fact(local, "store.plain.blocked"), std::string());

    // S2: no consent, no install, no download
    const int hitsBefore = net->hits.load();
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, id, 1)).status(), FOXAPI_DENIED);
    CHECK(fact(local, "status.installError").find("separate program") != std::string::npos);
    eng.controlPass();
    CHECK_EQ(net->hits.load(), hitsBefore);
    {
        std::error_code ec;
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(g_plugins, ec)) { if (e.path().filename().string().rfind("catalogue", 0) != 0 && e.path().filename().string() != "installed.json") { ++n; std::printf("      in the plugins folder: %s\n", e.path().filename().string().c_str()); } }   // (the kept catalogue copy is the store's own)
        CHECK_EQ(n, static_cast<std::size_t>(0));
    }

    // S3: the consent op
    CHECK_EQ(pumpedOn(eng, remote.raw(), textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, id, 1)).status(), FOXAPI_DENIED);
    CHECK_EQ(fact(local, "store." + id + ".helper.consent"), std::string("0"));
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, "no-such-plugin", 1)).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, "plain", 1)).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, id, 0)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, id, 1)).status(), FOXAPI_OK);
    CHECK_EQ(fact(local, "store." + id + ".helper.consent"), std::string("1"));
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, id, 1)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(fact(local, "store." + id + ".blocked"), std::string());
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, id, 0)).status(), FOXAPI_OK);   // withdrawn
    CHECK_EQ(fact(local, "store." + id + ".helper.consent"), std::string("0"));
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, id, 1)).status(), FOXAPI_DENIED);
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_STORE_HELPER_CONSENT, id, 1)).status(), FOXAPI_OK);

    // S4: with consent the install runs, all four files land beside each other
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, id, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return fs::exists(g_plugins / bridgeFile) && fs::exists(g_plugins / "multimon-ng.LICENSE.txt"); }));
    CHECK(passUntil(eng, [&] { return fact(local, "status.installReport").find("installed") != std::string::npos || fact(local, "status.installReport").find("fitted") != std::string::npos ||
                                         !fact(local, "status.installReport").empty(); }));
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_plugins, ec)) { names.push_back(e.path().filename().string()); }
    std::sort(names.begin(), names.end());
    bool hasBridge = false, hasProgram = false, hasManifest = false, hasLicence = false, strays = false;
    for (const std::string& n : names) {
        if (n == bridgeFile) { hasBridge = true; }
        else if (n == "multimon-ng.exe") { hasProgram = true; }
        else if (n == "multimon-ng.helper.json") { hasManifest = true; }
        else if (n == "multimon-ng.LICENSE.txt") { hasLicence = true; }
        else if (n != "installed.json" && n != "helpers.json" && n.rfind("catalogue", 0) != 0) { strays = true; std::printf("      stray: %s\n", n.c_str()); }
    }
    CHECK(hasBridge && hasProgram && hasManifest && hasLicence);
    CHECK(!strays);
    CHECK(fs::exists(g_plugins / "helpers.json"));
    PluginRepo::setTransportForTest(nullptr);
}

// the names of the CATALOGUE list, in the order the engine lists them
std::string listedNames(testing::Session& s) {
    std::string out;
    const int32_t n = api()->read_list(s.raw(), FOXAPI_LIST_CATALOGUE, 0, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        api()->read_list(s.raw(), FOXAPI_LIST_CATALOGUE, static_cast<uint32_t>(i), &it);
        out += (out.empty() ? "" : ",") + std::string(it.name);
    }
    return out;
}

FoxCommand settingCmd(const std::string& key, const std::string& value) { return textCmd(FOXAPP_OP_WIREC_SETTING, key + std::string(1, '\x1F') + value); }

// W9: the order of the store's list
void testStoreSort() {
    const std::string abi = std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
    auto entry = [&](const std::string& id, const std::string& name, const std::string& version, const std::string& published) {
        return "{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"version\":\"" + version + "\",\"licence\":\"MIT\",\"published\":\"" + published + "\",\"abiVersion\":" + abi + ",\"platforms\":[{\"os\":\"" +
               PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"" + id + "-" + version + kModExt + "\",\"url\":\"https://dl.example.invalid/" + id + "\",\"sha256\":\"" +
               std::string(64, 'a') + "\"}]}";
    };
    const std::string index = "{\"schemaVersion\":1,\"plugins\":[" + entry("zz", "Zebra", "1.0.0", "2026-03-01") + "," + entry("aa", "alpha", "2.0.0", "2026-01-05") + "," + entry("mm", "Mango", "1.0.0", "2026-02-10") + "]}";
    const fs::path indexFile = g_scratch / "sort-index.json";
    {
        std::ofstream f(indexFile, std::ios::binary | std::ios::trunc);
        f << index;
    }
    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    testing::Session local(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    CHECK(local.ok());
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, indexFile.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return api()->read_list(local.raw(), FOXAPI_LIST_CATALOGUE, 0, nullptr) == 3; }));
    const std::string asCatalogue = listedNames(local);
    CHECK(asCatalogue.find("Zebra") < asCatalogue.find("alpha") && asCatalogue.find("alpha") < asCatalogue.find("Mango"));   // the catalogue's own order by default
    CHECK_EQ(pumped(eng, local, settingCmd("store.sort", "name")).status(), FOXAPI_OK);
    passUntil(eng, [&] { return listedNames(local).find("alpha") < listedNames(local).find("Zebra"); });
    CHECK_EQ(listedNames(local), std::string("alpha,Mango,Zebra"));   // by name, any case
    CHECK_EQ(pumped(eng, local, settingCmd("store.sort", "updated")).status(), FOXAPI_OK);
    passUntil(eng, [&] { return listedNames(local) == "Zebra,Mango,alpha"; });
    CHECK_EQ(listedNames(local), std::string("Zebra,Mango,alpha"));   // newest first: Zebra (March), Mango (February), alpha (January)
    CHECK_EQ(pumped(eng, local, settingCmd("store.sort", "popular")).status(), FOXAPI_OUT_OF_RANGE);   // no server data: not offered
    CHECK_EQ(pumped(eng, local, settingCmd("store.sort", "catalogue")).status(), FOXAPI_OK);
    passUntil(eng, [&] { return listedNames(local) == asCatalogue; });
    CHECK_EQ(listedNames(local), asCatalogue);
    // an install by key still finds its entry whatever the order (it refuses here only for want of a licence tick or file: it is not NOT_FOUND)
    CHECK(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, "mm", 1)).status() != FOXAPI_NOT_FOUND);
}

}  // namespace

int main() {
    std::printf("test_engine_api_wire_c_store\n");
    isolate();
    testHelperStore();
    testStoreSort();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_wire_c_store");
}
