// test_engine_api_plugin_smoke_radio.cpp - EVERY INSTALLED PLUGIN ON RADIO 2 (multi-radio engine, stage 7; docs/multi-radio-records/engine-stage-7.md).
//
// A smoke over all the plugins installed on this machine (the 43 of the release; argument 1, else $FOXSDR_DRIVE_PLUGINS_DIR, else %LOCALAPPDATA%\FoxSDR\plugins, read before the profile is
// pointed at a scratch folder): each one is loaded by the real host, put on radio 2 (a test radio on the signal generator, radio 1 running beside it) with PLUGIN_RADIO, started, left to run for
// two seconds, and stopped. Asserted for every plugin: the engine answers PLUGIN_RADIO with a status it documents (OK or NO_CHANGE for a decoder; UNSUPPORTED for a plugin that transmits;
// NOT_FOUND for a plugin that is not a decoder - it has no radio to run on), no plugin is reported faulted, neither radio faults, and radio 1 goes on running. A plugin whose decoder is
// matched to radio 2's signal must also have a live instance on radio 2's runner. The table printed names each plugin's answer, so that a plugin that cannot run this way is named with its reason.
// A machine without the plugins SKIPS.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;

namespace {
std::vector<cascade::source::NativeDeviceInfo> noNativeRadios() { return {}; }
std::vector<cascade::source::SoapyDeviceInfo> noSoapyRadios() { return {}; }
fs::path g_plugins;
std::string pluginDirText() { return g_plugins.string(); }
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHooks() {
        Engine::testHooks_.nativeScan = &noNativeRadios;
        Engine::testHooks_.soapyScan = &noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDirText;
    }
    static bool runnerHas(Engine& e, std::size_t slot, const std::string& file) { return e.runnerOf(slot).hasInstances(file); }
    static bool faulted(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.faulted(); }
    static bool running(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.running(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Rig {
    Engine eng;
    std::unique_ptr<testing::Session> s;
    Rig() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
        s = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kGrants);
        CHECK(s->ok());
        pump(3);
    }
    void pump(int n = 1) {
        for (int i = 0; i < n; ++i) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    void run(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
    testing::Outcome send(const FoxCommand& c) {
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s->raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int pass = 0; pass < 40 && !o.landed; ++pass) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s->raw(), r, 16);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[i];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[8192];
        const int32_t n = api()->get_setting(s->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    FoxCommand aimed(std::uint32_t op, std::uint32_t target, double n0 = 0.0, long long i0 = 0, const char* text = nullptr) {
        FoxCommand c = testing::cmd(op, n0, i0, text);
        c.target = target;
        return c;
    }
};

struct Item {
    std::string file;
    std::string name;
    std::uint32_t flags = 0;
    std::uint64_t caps = 0;
};
std::vector<Item> pluginList(Rig& r) {
    std::vector<Item> out;
    const int32_t n = api()->read_list(r.s->raw(), FOXAPI_LIST_PLUGINS, 0, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (api()->read_list(r.s->raw(), FOXAPI_LIST_PLUGINS, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        out.push_back({it.detail, it.name, it.flags, static_cast<std::uint64_t>(it.value[0])});
    }
    return out;
}

void testEveryPluginOnRadioTwo() {
    std::printf("every installed plugin on radio 2\n");
    Rig r;
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
    {
        FoxCommand add = testing::cmd(FOXAPI_OP_RADIO_ADD, 90000.0, 2);
        add.ival[1] = 1;
        CHECK_EQ(r.send(add).status(), FOXAPI_OK);
    }
    r.run(1500);
    std::vector<Item> items = pluginList(r);
    std::printf("  %zu plugins are loaded by the host\n", items.size());
    CHECK(items.size() >= 40);
    int decoders = 0, ran = 0, matched = 0, txRefused = 0, notDecoder = 0, faulted = 0;
    std::vector<std::string> notes;
    for (const Item& it : items) {
        FoxCommand p = testing::cmd(FOXAPI_OP_PLUGIN_RADIO, 0.0, 2, it.file.c_str());
        p.ival[1] = 1;
        const testing::Outcome o = r.send(p);
        const int32_t st = o.status();
        std::string verdict;
        bool instance = false;
        if (st == FOXAPI_OK || st == FOXAPI_NO_CHANGE) {
            ++decoders;
            r.run(2000);   // two seconds on radio 2
            instance = Access::runnerHas(r.eng, 1, it.file);
            // the list again: is it matched to radio 2's signal, is it faulted?
            std::uint32_t flags = 0;
            for (const Item& now : pluginList(r)) {
                if (now.file == it.file) { flags = now.flags; }
            }
            const bool isMatched = (flags & FOXAPI_ITEM_MATCHED) != 0u;
            const bool isFault = (flags & FOXAPI_ITEM_FAULT) != 0u;
            if (isFault) { ++faulted; }
            if (isMatched) { ++matched; }
            if (isMatched && !instance) { CHECK(instance); }
            if (instance) { ++ran; }
            verdict = std::string("on radio 2, ") + (instance ? "instance live" : "no instance (not matched to the signal / asleep)") + (isFault ? ", FAULTED" : "");
            // stop it
            const testing::Outcome stop = r.send(testing::cmd(FOXAPI_OP_DECODER_STOP, 0.0, 0, it.file.c_str()));
            (void)stop;
            r.run(300);
        } else if (st == FOXAPI_UNSUPPORTED) {
            ++txRefused;
            verdict = std::string("runs on radio 1 only: ") + o.result.message;
        } else {
            ++notDecoder;
            verdict = std::string("answered ") + std::to_string(st) + ": " + o.result.message;
            CHECK(st == FOXAPI_NOT_FOUND);
        }
        std::printf("  %-48s %s\n", it.file.c_str(), verdict.c_str());
        // the radios are still healthy after every one
        CHECK(!Access::faulted(r.eng, 0));
        CHECK(!Access::faulted(r.eng, 1));
        CHECK(Access::running(r.eng, 0));
        CHECK(Access::running(r.eng, 1));
    }
    std::printf("  decoders put on radio 2: %d (instances live: %d, matched to the signal: %d); runs on radio 1 only: %d; not a decoder: %d; reported faulted: %d\n", decoders, ran, matched, txRefused,
                notDecoder, faulted);
    CHECK(decoders >= 30);
    CHECK_EQ(faulted, 0);
    CHECK_EQ(r.fact("radio.count"), std::string("2"));
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_plugin_smoke_radio\n");
    fs::path from;
    if (argc >= 2) {
        from = argv[1];
    } else if (const char* o = std::getenv("FOXSDR_DRIVE_PLUGINS_DIR"); o != nullptr && *o != '\0') {
        from = o;
    } else if (const char* la = std::getenv("LOCALAPPDATA"); la != nullptr && *la != '\0') {
        from = fs::path(la) / "FoxSDR" / "plugins";
    }
    engine_api_test::hermetic("plugin_smoke_radio");
    std::error_code ec;
    int dlls = 0;
    if (!from.empty() && fs::is_directory(from, ec)) {
        for (const auto& e : fs::directory_iterator(from, ec)) {
            if (e.path().extension() == ".dll") { ++dlls; }
        }
    }
    if (dlls < 40) {
        std::printf("SKIPPED: %d plugin modules in '%s' (the release has 43; set FOXSDR_DRIVE_PLUGINS_DIR)\n", dlls, from.string().c_str());
        return 0;
    }
    g_plugins = fs::temp_directory_path() / ("foxsdr_plugin_smoke_radio_" + std::to_string(
#if defined(_WIN32)
                                                                                 _getpid()
#else
                                                                                 getpid()
#endif
                                                                                     ));
    fs::remove_all(g_plugins, ec);
    fs::create_directories(g_plugins, ec);
    for (const auto& e : fs::directory_iterator(from, ec)) {
        const std::string n = e.path().filename().string();
        if (e.path().extension() == ".dll" || n == "catalogue.json" || n == "installed.json") { fs::copy_file(e.path(), g_plugins / n, fs::copy_options::overwrite_existing, ec); }
    }
    Access::installHooks();
    testEveryPluginOnRadioTwo();
    fs::remove_all(g_plugins, ec);
    return check::finish("test_engine_api_plugin_smoke_radio");
}
