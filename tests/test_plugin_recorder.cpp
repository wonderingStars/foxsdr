// test_plugin_recorder.cpp - the RECORDER calls a plugin makes (plugin_abi.h CascadeHostApi::recorder_start / recorder_stop, appended after host API level 2; core/plugin_api.*;
// wiring lane C row P1). Stand-in plugins built the way test_plugin_events.cpp builds its fakes. The engine's side of the same request (the take itself) is
// tests/test_engine_api_wire_c_recorder.cpp's.
//
//   R1  THE TABLE GREW AT THE END: nothing before the new members moved; an old host table (without them) is told apart by CASCADE_HOST_HAS / COVERS; the API level did not move
//   R2  THE GRANT: without the SETTINGS grant start and stop are DENIED (and the plugin is recorded as having asked, so the host offers the key); with it they are queued,
//       in order, carrying the asking plugin and the label
//   R3  EVERY REFUSAL: NULL, an empty label, a character outside [A-Za-z0-9._-], a label over 32 bytes (OUT_OF_RANGE; 32 is fine); a full queue is BUSY at the ninth
//   R4  WHO MAY: a stopped plugin is DENIED; a plugin that is gone is DETACHED; the re-check at apply time refuses a start after the grant was taken away and a request
//       of a plugin that is no longer live, but lets a live plugin's stop through
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_ui.hpp"
#include "test_check.hpp"

using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginApiCore;
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

std::vector<PluginApiCore::RecorderRequest> drain(PluginApiCore& core) {
    std::vector<PluginApiCore::RecorderRequest> out;
    core.takeRecorderRequests(out);
    return out;
}

void testTable() {
    std::printf("[R1] the table\n");
    static_assert(offsetof(CascadeHostApi, recorder_start) == offsetof(CascadeHostApi, events_seq) + sizeof(void*), "appended after the decode events");
    static_assert(offsetof(CascadeHostApi, recorder_stop) == offsetof(CascadeHostApi, recorder_start) + sizeof(void*), "adjacent");
    static_assert(sizeof(CascadeHostApi) == offsetof(CascadeHostApi, recorder_stop) + sizeof(void*), "and last");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    const CascadeHostApi* h = H(0);
    CHECK(h != nullptr);
    if (h == nullptr) { return; }
    CHECK(h->structSize == sizeof(CascadeHostApi));
    CHECK(CASCADE_HOST_HAS(h, recorder_start) && CASCADE_HOST_HAS(h, recorder_stop));
    CHECK(h->apiLevel == CASCADE_HOST_API_LEVEL);   // the struct size is the version: the level did not move
    CascadeHostApi old = *h;
    old.structSize = static_cast<std::uint32_t>(offsetof(CascadeHostApi, recorder_start));
    CHECK(!CASCADE_HOST_COVERS(&old, recorder_start));
    CHECK(CASCADE_HOST_COVERS(h, recorder_start));
    CHECK(CASCADE_HOST_COVERS(h, post_event));   // the members before it are still covered
}

void testGrant() {
    std::printf("[R2] the grant\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A"), plug(1, "B")});
    if (H(0) == nullptr || H(1) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    const std::string keyA = PluginUi::tuneKey(plug(0, "A"));
    // no grant: refused, and the plugin is on the list of those that asked for one
    CHECK(H(0)->recorder_start(C(0), "tg-1") == CASCADE_API_DENIED);
    CHECK(H(0)->recorder_stop(C(0)) == CASCADE_API_DENIED);
    CHECK(drain(core).empty());
    bool asked = false;
    for (const std::string& k : core.settingsRequesters()) { asked = asked || k == keyA; }
    CHECK(asked);
    core.setSettingsGranted(keyA, true);
    CHECK(H(0)->recorder_start(C(0), "tg-1") == CASCADE_API_OK);
    CHECK(H(0)->recorder_stop(C(0)) == CASCADE_API_OK);
    CHECK(H(0)->recorder_start(C(0), "ch16.vhf_A-B") == CASCADE_API_OK);
    // the other plugin has no grant: its request is refused whoever else holds one
    CHECK(H(1)->recorder_start(C(1), "x") == CASCADE_API_DENIED);
    const std::vector<PluginApiCore::RecorderRequest> got = drain(core);
    CHECK(got.size() == 3u);
    if (got.size() == 3u) {
        CHECK(got[0].start && std::string(got[0].label) == "tg-1");
        CHECK(!got[1].start && got[1].label[0] == '\0');
        CHECK(got[2].start && std::string(got[2].label) == "ch16.vhf_A-B");
        CHECK(got[0].client == got[1].client && got[1].client == got[2].client);
        CHECK(core.clientKey(got[0].client) == keyA);
    }
    CHECK(drain(core).empty());   // drained once
}

void testRefusals() {
    std::printf("[R3] every refusal\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    core.setSettingsGranted(PluginUi::tuneKey(plug(0, "A")), true);
    CHECK(H(0)->recorder_start(C(0), nullptr) == CASCADE_API_BAD_ARGUMENT);
    CHECK(H(0)->recorder_start(C(0), "") == CASCADE_API_BAD_ARGUMENT);
    for (const char* bad : {"tg 1", "tg/1", "tg\\1", "tg:1", "..\\x", "tg\n1", "caf\xC3\xA9", "a*b", "a\"b"}) { CHECK(H(0)->recorder_start(C(0), bad) == CASCADE_API_BAD_ARGUMENT); }
    CHECK(H(0)->recorder_start(C(0), std::string(33, 'a').c_str()) == CASCADE_API_OUT_OF_RANGE);
    CHECK(H(0)->recorder_start(C(0), std::string(200, 'a').c_str()) == CASCADE_API_OUT_OF_RANGE);
    CHECK(drain(core).empty());   // nothing refused was queued
    CHECK(H(0)->recorder_start(C(0), std::string(32, 'a').c_str()) == CASCADE_API_OK);
    CHECK(drain(core).size() == 1u);
    // a full queue
    for (std::size_t i = 0; i < PluginApiCore::kRecorderQueue; ++i) { CHECK(H(0)->recorder_start(C(0), "q") == CASCADE_API_OK); }
    CHECK(H(0)->recorder_start(C(0), "q") == CASCADE_API_BUSY);
    CHECK(H(0)->recorder_stop(C(0)) == CASCADE_API_BUSY);
    CHECK(drain(core).size() == PluginApiCore::kRecorderQueue);
    CHECK(H(0)->recorder_start(C(0), "q") == CASCADE_API_OK);   // room again
}

void testWho() {
    std::printf("[R4] who may\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    const std::string key = PluginUi::tuneKey(plug(0, "A"));
    core.setSettingsGranted(key, true);
    CHECK(H(0)->recorder_start(C(0), "tg-9") == CASCADE_API_OK);
    CHECK(H(0)->recorder_stop(C(0)) == CASCADE_API_OK);
    std::vector<PluginApiCore::RecorderRequest> got = drain(core);
    CHECK(got.size() == 2u);
    if (got.size() != 2u) { return; }
    CHECK(core.recorderRequestStillAllowed(got[0]));
    CHECK(core.recorderRequestStillAllowed(got[1]));
    core.setSettingsGranted(key, false);   // the grant is taken away between the request and the pass
    CHECK(!core.recorderRequestStillAllowed(got[0]));   // a start is not applied
    CHECK(core.recorderRequestStillAllowed(got[1]));    // a stop of a live plugin always is
    CHECK(H(0)->recorder_start(C(0), "tg-9") == CASCADE_API_DENIED);
    core.setSettingsGranted(key, true);
    // a stopped plugin
    ui.setStopped({key});
    CHECK(H(0)->recorder_start(C(0), "tg-9") == CASCADE_API_DENIED);
    CHECK(!core.recorderRequestStillAllowed(got[1]));
    ui.setStopped({});
    // a plugin that is gone: DETACHED
    const CascadeHostApi* h = H(0);
    void* ctx = C(0);
    ui.rebuild({});
    CHECK(h->recorder_start(ctx, "tg-9") == CASCADE_API_DETACHED);
    CHECK(h->recorder_stop(ctx) == CASCADE_API_DETACHED);
    CHECK(!core.recorderRequestStillAllowed(got[0]));
}

}  // namespace

int main() {
    testTable();
    testGrant();
    testRefusals();
    testWho();
    return testSummary("test_plugin_recorder");
}
