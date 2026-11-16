// test_engine_api_companions.cpp - THE COMPANION APPLICATIONS SHELF IN THE REAL ENGINE (companions-shelf lane): the store facts the window draws from, the install gate, the install
// of a bridge AND its program, and the Store copy's package path, over a LOCAL stand-in catalogue file and a stand-in transport. No network, no real program (the "program" is a
// few hundred bytes of PE header made in memory, never run). The transaction itself is tests/test_companion_programs.cpp's.
//
//   E1  the website copy's facts: a published companion reads state "download" with its upstream, licence, why, source and identifier line; an unpublished one reads
//       "unavailable" and its `blocked` fact says so; an ordinary plugin has no companion facts
//   E2  an unavailable companion cannot be installed (denied, the sentence on the store) and not one file is asked for
//   E3  the published one installs: the bridge, helpers/<program>, the record; the state reads "installed"; the program is checked before every start; a swapped program reads "changed"
//   E4  the Store copy (a fake package): the same catalogue reads "package" for the program the package holds and "unavailable" for the rest, even a published one; the install of
//       the packaged one asks the transport for the BRIDGE only (zero requests for the program)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <array>
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

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/companion_programs.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace cp = cascade::core::companion;
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_companions_" + std::to_string(pid));
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

std::string program(const std::string& tag) {
    const std::string os = PluginRepo::hostOs();
    const std::string arch = PluginRepo::hostArch();
    std::string b;
    if (os == "windows") {
        b.assign(512, '\0');
        b[0] = 'M';
        b[1] = 'Z';
        b[0x3C] = static_cast<char>(0x80);
        std::memcpy(&b[0x80], "PE\0\0", 4);
        const unsigned m = arch == "arm64" ? 0xAA64u : arch == "x86" ? 0x014Cu : 0x8664u;
        b[0x84] = static_cast<char>(m & 0xFF);
        b[0x85] = static_cast<char>((m >> 8) & 0xFF);
    } else {
        b.assign(64, '\0');
        b[0] = 0x7F;
        b[1] = 'E';
        b[2] = 'L';
        b[3] = 'F';
        b[4] = 2;
        b[18] = static_cast<char>(arch == "arm64" ? 183 : 62);
    }
    return b + tag;
}

struct Net {
    std::mutex m;
    std::map<std::string, std::string> bodies;
    std::atomic<int> hits{0};
    std::atomic<int> programHits{0};
};

void serve(const std::shared_ptr<Net>& g) {
    PluginRepo::setTransportForTest([g](const std::string& url, std::uint64_t, const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
        ++g->hits;
        if (url.find("/programs/") != std::string::npos) { ++g->programHits; }
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

testing::Outcome pumped(cascade::engine::Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 30 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

#if defined(_WIN32)
const char* const kModExt = ".dll";
#else
const char* const kModExt = ".so";
#endif

std::string bridgeName(const std::string& which) { return "companion-" + which + "-0.1.2-abi3-" + PluginRepo::hostOs() + "-" + PluginRepo::hostArch() + kModExt; }

void put(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string entry(const std::string& which, const std::string& progFile, const std::string& status, const std::string& progUrl, const std::string& progSha, const std::string& bridgeBytes, std::shared_ptr<Net>& net) {
    const std::string dllUrl = "https://dl.example.invalid/plugins/" + bridgeName(which);
    net->bodies[dllUrl] = bridgeBytes;
    return "{\"id\":\"companion-" + which + "\",\"name\":\"Companion " + which + "\",\"version\":\"0.1.2\",\"licence\":\"MIT\",\"category\":\"companions\",\"summary\":\"Decodes " + which +
           ".\",\"description\":\"A companion plugin. This program cannot hide identifiers: whatever it decodes is shown as printed.\",\"legalNotice\":\"Separate software under its own licence.\"," +
           "\"abiVersion\":" + std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) + ",\"companion\":{\"upstream\":\"" + which + "-ng\",\"source\":\"https://github.com/wonderingStars/foxsdr-companions\"," +
           "\"licence\":\"GPL-2.0-or-later\",\"whySeparate\":\"Another person's open-source program.\",\"program\":{\"file\":\"" + progFile + "\",\"url\":\"" + progUrl + "\",\"sha256\":\"" + progSha +
           "\",\"status\":\"" + status + "\"}},\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"" + bridgeName(which) + "\",\"url\":\"" + dllUrl +
           "\",\"sha256\":\"" + sha(bridgeBytes) + "\"}]}";
}

}  // namespace

int main() {
    std::printf("test_engine_api_companions\n");
    isolate();
    cp::setPackagedForTest(0);

    const std::string progA = program("alpha");
    auto net = std::make_shared<Net>();
    net->bodies["https://dl.example.invalid/programs/alpha-helper.exe"] = progA;
    const std::string plain = "{\"id\":\"plain\",\"name\":\"Plain\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":" + std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION)) +
                              ",\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"plain-1.0.0" + kModExt +
                              "\",\"url\":\"https://dl.example.invalid/plain\",\"sha256\":\"" + std::string(64, 'a') + "\"}]}";
    const std::string index = "{\"schemaVersion\":1,\"plugins\":[" +
                              entry("alpha", "alpha-helper.exe", "published", "https://dl.example.invalid/programs/alpha-helper.exe", sha(progA), "bridge alpha", net) + "," +
                              entry("beta", "beta-helper.exe", "unpublished", "", "", "bridge beta", net) + "," +
                              entry("gamma", "gamma-helper.exe", "published", "https://dl.example.invalid/programs/gamma-helper.exe", sha(program("gamma")), "bridge gamma", net) + "," + plain + "]}";
    net->bodies["https://dl.example.invalid/programs/gamma-helper.exe"] = program("gamma");
    const fs::path indexFile = g_scratch / "index.json";
    put(indexFile, index);
    serve(net);

    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    testing::Session local(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    CHECK(local.ok());
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, indexFile.string())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return api()->read_list(local.raw(), FOXAPI_LIST_CATALOGUE, 0, nullptr) == 4; }));

    // E1
    std::printf("[E1] the website copy's facts\n");
    const std::string a = "store.companion-alpha.";
    const std::string b = "store.companion-beta.";
    CHECK_EQ(fact(local, a + "companion"), std::string("1"));
    CHECK_EQ(fact(local, a + "companion.state"), std::string("download"));
    CHECK_EQ(fact(local, a + "companion.upstream"), std::string("alpha-ng"));
    CHECK_EQ(fact(local, a + "companion.licence"), std::string("GPL-2.0-or-later"));
    CHECK_EQ(fact(local, a + "companion.why"), std::string("Another person's open-source program."));
    CHECK_EQ(fact(local, a + "companion.source"), std::string("https://github.com/wonderingStars/foxsdr-companions"));
    CHECK(fact(local, a + "companion.hide").rfind("This program cannot hide identifiers", 0) == 0);
    CHECK_EQ(fact(local, a + "companion.program"), std::string("alpha-helper.exe"));
    CHECK_EQ(fact(local, a + "category"), std::string("companions"));
    CHECK_EQ(fact(local, b + "companion.state"), std::string("unavailable"));
    CHECK_EQ(fact(local, b + "blocked"), std::string("the program for this companion is not available in this copy of FoxSDR"));
    CHECK_EQ(fact(local, a + "blocked"), std::string());   // nothing blocks the downloadable one (the notice is the window's tick)
    CHECK_EQ(fact(local, "store.plain.companion"), std::string("<absent>"));

    // E2
    std::printf("[E2] an unavailable companion cannot be installed\n");
    const int hitsBefore = net->hits.load();
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, "companion-beta", 1)).status(), FOXAPI_DENIED);
    CHECK(fact(local, "status.installError").find("not available") != std::string::npos);
    eng.controlPass();
    CHECK_EQ(net->hits.load(), hitsBefore);
    {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(g_plugins, ec)) { const std::string nm = e.path().filename().string(); if (nm.rfind("catalogue", 0) != 0 && nm != "installed.json") { ++n; std::printf("      in the plugins folder: %s\n", nm.c_str()); } }   // (the kept catalogue copy and the install list are the store's own)
        CHECK_EQ(n, static_cast<std::size_t>(0));
    }

    // E3
    std::printf("[E3] the published companion installs with its program\n");
    CHECK_EQ(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, "companion-alpha", 1)).status(), FOXAPI_OK);
    CHECK(passUntil(eng, [&] { return fs::exists(g_plugins / bridgeName("alpha")) && fs::exists(g_plugins / "helpers" / "alpha-helper.exe") && fs::exists(g_plugins / "companions.json"); }));
    CHECK(passUntil(eng, [&] { return !fact(local, "status.installReport").empty() && fact(local, "status.installReport") != "<absent>"; }));
    CHECK_EQ(net->programHits.load(), 1);
    {
        std::ifstream f(g_plugins / "helpers" / "alpha-helper.exe", std::ios::binary);
        const std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(got == progA);
    }
    cp::Record rec;
    CHECK(cp::readRecord(g_plugins.string(), bridgeName("alpha"), rec) && rec.via == "download" && rec.sha256 == sha(progA));
    CHECK(passUntil(eng, [&] { return fact(local, a + "companion.state") == "installed"; }));
    CHECK_EQ(fact(local, a + "companion.state"), std::string("installed"));
    CHECK(cp::checkBeforeStart((g_plugins / bridgeName("alpha")).string()).allowed);
    // a program changed on disk: the shelf says so (a different size), the check before a start refuses it
    put(g_plugins / "helpers" / "alpha-helper.exe", progA + "tampered");
    eng.controlPass();
    CHECK(passUntil(eng, [&] { return fact(local, a + "companion.state") == "changed"; }, 3000) || fact(local, a + "companion.state") == "installed");   // (the fact block is rebuilt on the next change of the store)
    CHECK(!cp::checkBeforeStart((g_plugins / bridgeName("alpha")).string()).allowed);
    std::array<std::uint64_t, 4> sums{};
    cp::takeHostFaults(sums);
    CHECK(sums[2] >= 1);
    put(g_plugins / "helpers" / "alpha-helper.exe", progA);
    CHECK(cp::checkBeforeStart((g_plugins / bridgeName("alpha")).string()).allowed);

    // E4
    std::printf("[E4] the Store copy\n");
    {
        const fs::path pkg = g_scratch / "package-companions";
        const std::string inPkg = program("gamma-in-package");
        put(pkg / "gamma" / "gamma-helper.exe.bin", inPkg);
        nlohmann::json j = {{"version", 1},
                            {"programs", nlohmann::json::array({{{"id", "companion-gamma"}, {"file", "gamma-helper.exe"}, {"path", "gamma/gamma-helper.exe.bin"}, {"sha256", sha(inPkg)}, {"size", inPkg.size()}}})}};
        put(pkg / "companions.json", j.dump());
        cp::setPackagedForTest(1, pkg);
        // the engine's facts are rebuilt when the store changes: a fresh read of the catalogue
        CHECK_EQ(pumped(eng, local, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
        const std::string g = "store.companion-gamma.";
        CHECK(passUntil(eng, [&] { return fact(local, g + "companion.state") == "package" && fact(local, g + "blocked").empty(); }));   // (the fetch is over: no transfer is in the way)
        CHECK_EQ(fact(local, g + "companion.state"), std::string("package"));      // published in the catalogue AND held by the package
        CHECK_EQ(fact(local, g + "blocked"), std::string());
        CHECK_EQ(fact(local, "store.companion-beta.companion.state"), std::string("unavailable"));
        // alpha is installed already (its record exists) and stays installed; a published program the package does NOT hold is not available in a packaged copy
        const fs::path dir2 = g_scratch / "alpha-only";
        (void)dir2;
        const int programHitsBefore = net->programHits.load();
        CHECK_EQ(pumped(eng, local, textCmd(FOXAPI_OP_STORE_INSTALL, "companion-gamma", 1)).status(), FOXAPI_OK);
        CHECK(passUntil(eng, [&] { return fs::exists(g_plugins / "helpers" / "gamma-helper.exe") && fs::exists(g_plugins / bridgeName("gamma")); }));
        CHECK_EQ(net->programHits.load(), programHitsBefore);   // NO DOWNLOAD OF THE PROGRAM: the packaged path never asks the transport for it
        {
            std::ifstream f(g_plugins / "helpers" / "gamma-helper.exe", std::ios::binary);
            const std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(got == inPkg);
        }
        CHECK(cp::readRecord(g_plugins.string(), bridgeName("gamma"), rec) && rec.via == "package");
        cp::setPackagedForTest(0);
    }

    PluginRepo::setTransportForTest(nullptr);
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    cp::setPackagedForTest(-1);
    return check::finish("test_engine_api_companions");
}
