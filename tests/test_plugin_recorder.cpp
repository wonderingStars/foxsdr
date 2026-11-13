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
    static_assert(offsetof(CascadeHostApi, symbol_rate) == offsetof(CascadeHostApi, recorder_stop) + sizeof(void*), "the symbol rate follows");
    static_assert(offsetof(CascadeHostApi, request_open_url) == offsetof(CascadeHostApi, symbol_rate) + sizeof(void*), "adjacent");
    static_assert(offsetof(CascadeHostApi, request_open_library_folder) == offsetof(CascadeHostApi, request_open_url) + sizeof(void*), "adjacent");
    static_assert(sizeof(CascadeHostApi) == offsetof(CascadeHostApi, request_open_library_folder) + sizeof(void*), "and last");
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

// wire-c begin: the symbol rate, the open requests and the marker credit (plugin_abi.h, appended after the recorder members; wiring lane C rows P2 a, b, f).
//   S1  the symbol rate: 0 until the engine sets it, then what was set; polled by every plugin
//   S2  an open request holds NOTHING open: it is stored (one URL and one folder per plugin, the newest replacing), listed, and used up by the confirm that takes it
//   S3  every refused address: not https, upper-case scheme, http, file:, javascript:, ftp, a user name or password, a space or control or non-ASCII byte, a backslash, no host,
//       empty, NULL, too long (OUT_OF_RANGE; exactly 200 bytes is fine)
//   S4  who may: a stopped plugin is DENIED, a plugin that is gone is DETACHED, and a request of a plugin that is no longer live is not listed
//   S5  the marker credit: a marker of the OLD size (before the field) is taken with an empty credit; a full one keeps its credit, cut at a whole character
void testSymbolRate() {
    std::printf("[S1] symbol rate\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A"), plug(1, "B")});
    if (H(0) == nullptr || H(1) == nullptr) { CHECK(false); return; }
    CHECK(CASCADE_HOST_HAS(H(0), symbol_rate));
    CHECK(H(0)->symbol_rate(C(0)) == 0.0);
    ui.api().setSymbolRate(31.25);
    CHECK(H(0)->symbol_rate(C(0)) == 31.25 && H(1)->symbol_rate(C(1)) == 31.25);
    ui.api().setSymbolRate(0.0);
    CHECK(H(0)->symbol_rate(C(0)) == 0.0);
    CHECK(H(0)->symbol_rate(nullptr) == 0.0);   // an unknown ctx reads as "none"
}

void testOpenRequests() {
    std::printf("[S2] open requests\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A"), plug(1, "B")});
    if (H(0) == nullptr || H(1) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    CHECK(CASCADE_HOST_HAS(H(0), request_open_url) && CASCADE_HOST_HAS(H(0), request_open_library_folder));
    CHECK(core.openRequests().empty());
    const std::uint64_t seq0 = core.openSeq();
    CHECK(H(0)->request_open_url(C(0), "https://example.org/get/lib?v=2#top") == CASCADE_API_OK);
    CHECK(H(0)->request_open_library_folder(C(0)) == CASCADE_API_OK);
    CHECK(H(1)->request_open_url(C(1), "https://other.example/") == CASCADE_API_OK);
    CHECK(core.openSeq() > seq0);
    std::vector<PluginApiCore::OpenRequest> list = core.openRequests();
    CHECK(list.size() == 3u);
    // a newer request of the same kind replaces the older one
    CHECK(H(0)->request_open_url(C(0), "https://example.org/second") == CASCADE_API_OK);
    list = core.openRequests();
    CHECK(list.size() == 3u);
    const std::string keyA = PluginUi::tuneKey(plug(0, "A"));
    const std::string keyB = PluginUi::tuneKey(plug(1, "B"));
    PluginApiCore::OpenRequest r;
    CHECK(!core.takeOpenRequest("no-such-key", false, r));
    CHECK(core.takeOpenRequest(keyA, false, r) && r.url == "https://example.org/second" && !r.folder && r.name == "A" && r.key == keyA);
    CHECK(!core.takeOpenRequest(keyA, false, r));   // used up
    CHECK(core.takeOpenRequest(keyA, true, r) && r.folder && r.url.empty());
    CHECK(!core.takeOpenRequest(keyA, true, r));
    CHECK(core.takeOpenRequest(keyB, false, r) && r.url == "https://other.example/");   // another plugin's request is its own
    CHECK(core.openRequests().empty());
}

void testOpenRefusals() {
    std::printf("[S3] every refused address\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    CHECK(H(0)->request_open_url(C(0), nullptr) == CASCADE_API_BAD_ARGUMENT);
    for (const char* bad : {"", "http://example.org/", "HTTPS://example.org/", "Https://example.org/", "file:///C:/Windows/System32/calc.exe", "javascript:alert(1)", "ftp://example.org/",
                            "mailto:a@b.c", "//example.org/", "example.org", "https:/example.org", "https://", "https:///path", "https://user@example.org/", "https://user:pw@example.org/",
                            "https://example.org/a b", "https://example.org/a\tb", "https://example.org/a\nb", "https://example.org/\\x", "https://exa\\mple.org/",
                            "https://example.org/caf\xC3\xA9", "https://example.org/\x7F", "https://.example.org/", "https://:443/", "https://-x.example/"}) {
        CHECK(H(0)->request_open_url(C(0), bad) == CASCADE_API_BAD_ARGUMENT);
    }
    const std::string base = "https://e.org/";
    CHECK(H(0)->request_open_url(C(0), (base + std::string(200 - base.size() + 1u, 'a')).c_str()) == CASCADE_API_OUT_OF_RANGE);   // 201 bytes
    CHECK(H(0)->request_open_url(C(0), std::string(5000, 'a').c_str()) == CASCADE_API_OUT_OF_RANGE);
    CHECK(core.openRequests().empty());   // nothing refused was held
    CHECK(H(0)->request_open_url(C(0), (base + std::string(200 - base.size(), 'a')).c_str()) == CASCADE_API_OK);   // exactly 200 bytes
    CHECK(core.openRequests().size() == 1u);
    std::int32_t why = 0;
    CHECK(PluginApiCore::acceptableOpenUrl("https://example.org/x", why) && why == CASCADE_API_OK);
    CHECK(!PluginApiCore::acceptableOpenUrl("http://example.org/x", why) && why == CASCADE_API_BAD_ARGUMENT);
}

void testOpenWho() {
    std::printf("[S4] who may\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    const std::string key = PluginUi::tuneKey(plug(0, "A"));
    CHECK(H(0)->request_open_url(C(0), "https://example.org/") == CASCADE_API_OK);
    CHECK(core.openRequests().size() == 1u);
    ui.setStopped({key});
    CHECK(H(0)->request_open_url(C(0), "https://example.org/") == CASCADE_API_DENIED);
    CHECK(H(0)->request_open_library_folder(C(0)) == CASCADE_API_DENIED);
    CHECK(core.openRequests().empty());   // a stopped plugin's held request is not offered
    ui.setStopped({});
    const CascadeHostApi* h = H(0);
    void* ctx = C(0);
    ui.rebuild({});
    CHECK(h->request_open_url(ctx, "https://example.org/") == CASCADE_API_DETACHED);
    CHECK(h->request_open_library_folder(ctx) == CASCADE_API_DETACHED);
    CHECK(core.openRequests().empty());
    PluginApiCore::OpenRequest r;
    CHECK(!core.takeOpenRequest(key, false, r));
}

void testMarkerCredit() {
    std::printf("[S5] marker credit\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    CHECK(offsetof(CascadeMarker, credit) + CASCADE_MARKER_CREDIT_CHARS == sizeof(CascadeMarker));   // the credit is the last member
    CascadeMarker m{};
    m.structSize = static_cast<std::uint32_t>(offsetof(CascadeMarker, credit));   // a plugin built before the field existed
    m.id = 1;
    m.kind = CASCADE_MARKER_POINT;
    m.freqHz = 14.074e6;
    std::snprintf(m.label, sizeof(m.label), "old");
    std::memset(m.credit, 'Z', sizeof(m.credit));   // bytes beyond its structSize are not its to give: they must NOT be read
    CHECK(H(0)->set_marker(C(0), &m) == CASCADE_API_OK);
    CascadeMarker n{};
    n.structSize = static_cast<std::uint32_t>(sizeof(CascadeMarker));
    n.id = 2;
    n.kind = CASCADE_MARKER_POINT;
    n.freqHz = 7.074e6;
    std::snprintf(n.label, sizeof(n.label), "new");
    std::snprintf(n.credit, sizeof(n.credit), "Data provided by HamQTH.com");
    CHECK(H(0)->set_marker(C(0), &n) == CASCADE_API_OK);
    CascadeMarker w = n;
    w.id = 3;
    std::memset(w.credit, 'C', sizeof(w.credit));   // filled to the last byte, unterminated
    CHECK(H(0)->set_marker(C(0), &w) == CASCADE_API_OK);
    CascadeMarker tooSmall{};
    tooSmall.structSize = static_cast<std::uint32_t>(offsetof(CascadeMarker, credit)) - 4u;
    tooSmall.id = 4;
    tooSmall.kind = CASCADE_MARKER_POINT;
    tooSmall.freqHz = 1.0e6;
    CHECK(H(0)->set_marker(C(0), &tooSmall) == CASCADE_API_BAD_ARGUMENT);
    std::vector<cascade::core::HostMarker> got;
    ui.api().markers(got);
    CHECK(got.size() == 3u);
    if (got.size() == 3u) {
        for (const cascade::core::HostMarker& h : got) {
            if (h.m.id == 1u) { CHECK(h.m.credit[0] == '\0'); }
            if (h.m.id == 2u) { CHECK(std::string(h.m.credit) == "Data provided by HamQTH.com"); }
            if (h.m.id == 3u) { CHECK(std::strlen(h.m.credit) == CASCADE_MARKER_CREDIT_CHARS - 1u); }
        }
    }
}
// wire-c end

}  // namespace

int main() {
    testTable();
    testGrant();
    testRefusals();
    testWho();
    testSymbolRate();
    testOpenRequests();
    testOpenRefusals();
    testOpenWho();
    testMarkerCredit();
    return testSummary("test_plugin_recorder");
}
