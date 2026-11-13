// test_engine_api_wire_c_open.cpp - the engine's side of the plugin calls symbol_rate, request_open_url and request_open_library_folder (plugin_abi.h, wiring lane C row P2; engine_wire_c.cpp,
// src/engine/wire_c_open.*), against a real Engine with stand-in plugins attached to its own plugin API core. The opening itself is a STAND-IN (State::openTarget): no browser or
// file manager is ever started by this test.
//
//   O1  the symbol rate: the user's setting reaches the plugins that poll it (0 by default), a value out of range is refused and changes nothing
//   O2  a request opens NOTHING; it is listed as a fact (what, for whom, the address); the confirm op takes it and the stand-in is called once with exactly that address
//   O3  the folder request opens the plugin's OWN folder under plugin-libs (created), never a path the plugin chose; the name cannot climb out
//   O4  the refusals of the confirm op: no such request, a bad kind, a missing separator; a request is used up by the confirm that takes it; a failing open says why (fact.plugin.open.last)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>
#include <vector>

#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_ui.hpp"
#include "engine/wire_c_open.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginUi;

namespace {

struct Captured {
    const CascadeHostApi* api = nullptr;
};
Captured g_cap[2];
template <int I>
void attachI(const CascadeHostApi* h) {
    g_cap[I].api = h;
}
const CascadeHostClientApi kHc[2] = {{sizeof(CascadeHostClientApi), &attachI<0>}, {sizeof(CascadeHostClientApi), &attachI<1>}};

LoadedPlugin plug(int i, const char* name) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.capabilities = CASCADE_CAP_HOST_CLIENT;
    p.hostClient = &kHc[i];
    return p;
}
const CascadeHostApi* H(int i) { return g_cap[i].api; }
void* C(int i) { return g_cap[i].api != nullptr ? g_cap[i].api->ctx : nullptr; }

HostServices services() {
    HostServices s;
    s.centreHz = [] { return 145.0e6; };
    s.sampleRateHz = [] { return 2.4e6; };
    s.tune = [](double) { return static_cast<std::int32_t>(CASCADE_TUNE_OK); };
    s.unixTimeMs = [] { return static_cast<std::int64_t>(1'790'000'000'000LL); };
    return s;
}

FoxCommand op(std::uint32_t o, const std::string& text) { return testing::cmd(o, 0.0, 0, text.c_str()); }
std::string confirmText(const std::string& key, const char* kind) { return key + '\x1F' + kind; }

struct Opened {
    std::vector<std::string> targets;
    std::vector<bool> folders;
    bool fail = false;
};

void testSymbolRate(const fs::path& dir) {
    Rig r(dir);
    PluginUi ui(r.eng.pluginApiShared());
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    r.pass(4);
    CHECK(H(0)->symbol_rate(C(0)) == 0.0);
    CHECK(r.status(setting("sig.symbol_rate", "31.25")) == FOXAPI_OK);
    r.pass(4);
    CHECK(H(0)->symbol_rate(C(0)) == 31.25);
    CHECK(r.status(setting("sig.symbol_rate", "2000000")) == FOXAPI_OUT_OF_RANGE);
    CHECK(r.status(setting("sig.symbol_rate", "-1")) == FOXAPI_OUT_OF_RANGE);
    CHECK(r.status(setting("sig.symbol_rate", "fast")) == FOXAPI_BAD_ARGUMENT);
    r.pass(4);
    CHECK(H(0)->symbol_rate(C(0)) == 31.25);   // the refused values changed nothing
    CHECK(r.status(setting("sig.symbol_rate", "0")) == FOXAPI_OK);
    r.pass(4);
    CHECK(H(0)->symbol_rate(C(0)) == 0.0);
}

void testOpen(const fs::path& dir) {
    Opened seen;
    Rig r(dir, [&seen](cascade::engine::wirec::State& s) {
        s.openTarget = [&seen](const std::string& what, bool folder, std::string& why) {
            seen.targets.push_back(what);
            seen.folders.push_back(folder);
            if (seen.fail) {
                why = "the stand-in refused";
                return false;
            }
            return true;
        };
    });
    PluginUi ui(r.eng.pluginApiShared());
    ui.setServices(services());
    ui.rebuild({plug(0, "tgrec"), plug(1, "../../evil")});
    if (H(0) == nullptr || H(1) == nullptr) { CHECK(false); return; }
    const std::string keyA = PluginUi::tuneKey(plug(0, "tgrec"));
    const std::string keyB = PluginUi::tuneKey(plug(1, "../../evil"));
    r.pass(4);
    CHECK(r.fact("plugin.open.count") == "0");

    // O2: a request opens nothing
    CHECK(H(0)->request_open_url(C(0), "https://example.org/get/mbelib") == CASCADE_API_OK);
    CHECK(H(0)->request_open_library_folder(C(0)) == CASCADE_API_OK);
    r.pass(6);
    CHECK(seen.targets.empty());
    CHECK(r.fact("plugin.open.count") == "2");
    const std::string row1 = r.fact("plugin.open.1");
    CHECK(row1 == keyA + '\x1F' + "tgrec" + '\x1F' + "url" + '\x1F' + "https://example.org/get/mbelib");
    const std::string folderA = cascade::engine::wirec::libraryFolderFor(dir, "tgrec").string();
    CHECK(r.fact("plugin.open.2") == keyA + '\x1F' + "tgrec" + '\x1F' + "folder" + '\x1F' + folderA);
    CHECK(!fs::exists(folderA));   // listing a folder request creates nothing

    // the confirm opens exactly that address, once
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText(keyA, "url"))) == FOXAPI_OK);
    CHECK(seen.targets.size() == 1u && seen.targets[0] == "https://example.org/get/mbelib" && !seen.folders[0]);
    CHECK(r.fact("plugin.open.last") == "opened the page");
    CHECK(r.fact("plugin.open.count") == "1");
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText(keyA, "url"))) == FOXAPI_NOT_FOUND);   // used up
    CHECK(seen.targets.size() == 1u);

    // O3: the folder is the plugin's own, created at the confirm
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText(keyA, "folder"))) == FOXAPI_OK);
    CHECK(seen.targets.size() == 2u && seen.targets[1] == folderA && seen.folders[1]);
    CHECK(fs::is_directory(folderA));
    CHECK(r.fact("plugin.open.last") == "opened the library folder");
    CHECK(H(1)->request_open_library_folder(C(1)) == CASCADE_API_OK);
    r.pass(4);
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText(keyB, "folder"))) == FOXAPI_OK);
    if (seen.targets.size() == 3u) {
        const fs::path evil = fs::path(seen.targets[2]);
        CHECK(evil.parent_path() == fs::path(dir) / "plugin-libs");   // one level below plugin-libs: no separator survived
        CHECK(evil.filename().string().find("..") == std::string::npos || evil.filename().string() == ".._.._evil");
        CHECK(evil.filename().string().find('/') == std::string::npos && evil.filename().string().find('\\') == std::string::npos);
    } else {
        CHECK(false);
    }

    // O4: the refusals
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText(keyA, "exe"))) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, keyA)) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText("no-such-plugin", "url"))) == FOXAPI_NOT_FOUND);
    const std::size_t before = seen.targets.size();
    CHECK(H(0)->request_open_url(C(0), "https://example.org/again") == CASCADE_API_OK);
    r.pass(4);
    seen.fail = true;
    std::string msg;
    CHECK(r.status(op(FOXAPP_OP_PLUGIN_OPEN_CONFIRM, confirmText(keyA, "url")), &msg) == FOXAPI_FAILED);
    CHECK(seen.targets.size() == before + 1u);
    CHECK(r.fact("plugin.open.last") == "the stand-in refused");
    seen.fail = false;

    // (a stopped plugin's request is not listed or confirmable: tests/test_plugin_recorder.cpp [S4] holds that at the API core; the engine's own pass owns the stopped set here)
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_open");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("open");
    int n = 0;
    for (auto fn : {testSymbolRate, testOpen}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_open");
}
