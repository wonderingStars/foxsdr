// test_engine_api_markerlimit.cpp - a plugin may have CASCADE_MAX_MARKERS_PER_PLUGIN (256 from 0.99.109, it was 64) marks set at once, and the engine says so
// in fact.plugin.markerLimit (engine wiring W2, brief item W1 "F": the marker cap and its fact).
//
//   K1  the number is 256 and the fact reads it; the old 64 is not the limit any more
//   K2  a stand-in plugin sets exactly 256 marks (all accepted), the 257th is LIMIT, replacing one at the limit still works, removing one makes room again
//   K3  one plugin's quota is its own: another plugin still has all 256
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

#include "check.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_ui.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace {

static_assert(CASCADE_MAX_MARKERS_PER_PLUGIN == 256u, "the per-plugin marker quota is 256 from 0.99.109");

constexpr const char* kHooks = "testHooks=1;token=s3cret";
const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

const CascadeHostApi* g_api[2] = {nullptr, nullptr};
template <int I>
void attachI(const CascadeHostApi* h) {
    g_api[I] = h;
}
const CascadeHostClientApi kHc[2] = {{sizeof(CascadeHostClientApi), &attachI<0>}, {sizeof(CascadeHostClientApi), &attachI<1>}};

cascade::core::LoadedPlugin plug(int i, const char* name) {
    cascade::core::LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.capabilities = CASCADE_CAP_HOST_CLIENT;
    p.hostClient = &kHc[i];
    return p;
}

CascadeMarker marker(std::uint32_t id) {
    CascadeMarker m{};
    m.structSize = sizeof(m);
    m.id = id;
    m.kind = CASCADE_MARKER_POINT;
    m.freqHz = 100.0e6 + static_cast<double>(id) * 1000.0;
    return m;
}

void testFact() {
    std::printf("K1: the number and its fact\n");
    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    CHECK(s.ok());
    for (int i = 0; i < 3; ++i) { eng.controlPass(); }
    char buf[64] = {};
    const int32_t n = api()->get_setting(s.raw(), "fact.plugin.markerLimit", buf, sizeof(buf));
    CHECK(n >= 0);
    CHECK_EQ(std::string(buf), std::string("256"));
    CHECK(std::string(buf) != "64");
}

void testQuota() {
    std::printf("K2/K3: the quota\n");
    cascade::core::PluginUi ui;
    cascade::core::HostServices svc;
    svc.centreHz = [] { return 145.0e6; };
    svc.sampleRateHz = [] { return 2.4e6; };
    svc.tune = [](double) { return static_cast<std::int32_t>(CASCADE_TUNE_OK); };
    ui.setServices(svc);
    ui.rebuild({plug(0, "Mk1"), plug(1, "Mk2")});
    CHECK(g_api[0] != nullptr && g_api[1] != nullptr);
    if (g_api[0] == nullptr || g_api[1] == nullptr) { return; }
    void* c0 = g_api[0]->ctx;
    void* c1 = g_api[1]->ctx;
    for (std::uint32_t i = 0; i < CASCADE_MAX_MARKERS_PER_PLUGIN; ++i) {
        CascadeMarker m = marker(1000u + i);
        if (g_api[0]->set_marker(c0, &m) != CASCADE_API_OK) {
            CHECK(false);
            break;
        }
    }
    CascadeMarker extra = marker(9999);
    CHECK(g_api[0]->set_marker(c0, &extra) == CASCADE_API_LIMIT);   // the 257th
    CascadeMarker again = marker(1000);
    again.freqHz = 101.0e6;
    CHECK(g_api[0]->set_marker(c0, &again) == CASCADE_API_OK);      // replacing one at the limit is fine
    CHECK(g_api[0]->remove_marker(c0, 1001) == CASCADE_API_OK);
    CHECK(g_api[0]->set_marker(c0, &extra) == CASCADE_API_OK);      // room again
    // K3: another plugin has all of its own
    for (std::uint32_t i = 0; i < CASCADE_MAX_MARKERS_PER_PLUGIN; ++i) {
        CascadeMarker m = marker(2000u + i);
        CHECK(g_api[1]->set_marker(c1, &m) == CASCADE_API_OK);
    }
    CascadeMarker over = marker(9998);
    CHECK(g_api[1]->set_marker(c1, &over) == CASCADE_API_LIMIT);
}

}  // namespace

int main() {
    engine_api_test::hermetic("markerlimit");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testFact();
    testQuota();
    return check::finish("test_engine_api_markerlimit");
}
