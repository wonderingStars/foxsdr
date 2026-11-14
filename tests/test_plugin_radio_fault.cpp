// test_plugin_radio_fault.cpp - the two host calls appended in engine fixes 7 (plugin_abi.h CascadeHostApi::radio_index and report_fault; core/plugin_api.*; core/companion_fault.*).
//
//   F1  THE TABLE GREW AT THE END: nothing before the two moved, they are the last two members; an OLD host table (cut before them) is told apart by CASCADE_HOST_HAS / COVERS; the
//       API level did not move; an old PLUGIN (one that never reads past the members it knows) is untouched because a bigger table only adds bytes after what it reads
//   F2  RADIO INDEX: 1 for a plugin on radio 1, the radio the engine puts it on (3, 8), never 0, each plugin its own; DETACHED (negative) when the plugin is gone
//   F3  REPORT FAULT, EVERY REFUSAL: kind 0 and 5 and a huge kind, a count of 0, a count over the maximum (the maximum itself is fine); nothing refused is kept
//   F4  THE ONE-A-MINUTE RULE: the same kind from the same plugin inside 60 s is BUSY (and adds nothing), after 60 s it is accepted; another kind and another plugin are not held by it;
//       the sums the pass takes are the accepted counts only, per kind, and taken once
//   F5  A PLUGIN THAT IS GONE: DETACHED, nothing kept
//   F6  THE SWITCH: OFF by default - what plugins reported is dropped, nothing is counted and no report is filed; ON (a test sets it) - each kind with a sum is counted in the health
//       ledger under its own word and filed as an issue report of that word, once a session, and the report text carries nothing of the plugin
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/companion_fault.hpp"
#include "core/crash_handler.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_ui.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginApiCore;
using cascade::core::PluginUi;
namespace health = cascade::core::health;
namespace companion = cascade::core::companion;
namespace issue = cascade::core::issue;

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

std::int64_t g_nowMs = 1'000'000;
std::int64_t fakeClock() { return g_nowMs; }

std::array<std::uint64_t, 4> take(PluginApiCore& core) {
    std::array<std::uint64_t, 4> s{};
    core.takeFaultReports(s);
    return s;
}

void testTable() {
    std::printf("[F1] the table\n");
    static_assert(offsetof(CascadeHostApi, radio_index) == offsetof(CascadeHostApi, request_open_library_folder) + sizeof(void*), "appended after the open requests");
    static_assert(offsetof(CascadeHostApi, report_fault) == offsetof(CascadeHostApi, radio_index) + sizeof(void*), "adjacent");
    static_assert(sizeof(CascadeHostApi) == offsetof(CascadeHostApi, report_fault) + sizeof(void*), "and last");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    const CascadeHostApi* h = H(0);
    CHECK(h != nullptr);
    if (h == nullptr) { return; }
    CHECK(h->structSize == sizeof(CascadeHostApi));
    CHECK(CASCADE_HOST_HAS(h, radio_index) && CASCADE_HOST_HAS(h, report_fault));
    CHECK(h->apiLevel == CASCADE_HOST_API_LEVEL);   // the struct size is the version: the level did not move
    // an OLD host's table: the same bytes cut where the table used to end
    CascadeHostApi old = *h;
    old.structSize = static_cast<std::uint32_t>(offsetof(CascadeHostApi, radio_index));
    CHECK(!CASCADE_HOST_COVERS(&old, radio_index) && !CASCADE_HOST_HAS(&old, report_fault));
    CHECK(CASCADE_HOST_HAS(&old, request_open_library_folder));   // what it did have is still there
    // everything that came before still answers as before
    CHECK(CASCADE_HOST_HAS(h, symbol_rate) && CASCADE_HOST_HAS(h, recorder_start) && CASCADE_HOST_HAS(h, post_event));
    // an old PLUGIN was built against a smaller struct: it reads the first `structSize` bytes of ITS header, every offset of which is unchanged (the members before ours did not move)
    CHECK(offsetof(CascadeHostApi, request_open_library_folder) + sizeof(void*) <= offsetof(CascadeHostApi, radio_index));
}

void testRadioIndex() {
    std::printf("[F2] radio index\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A"), plug(1, "B")});
    if (H(0) == nullptr || H(1) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    CHECK(H(0)->radio_index(C(0)) == 1);   // radio 1 until the user says otherwise
    CHECK(H(1)->radio_index(C(1)) == 1);
    core.setClientRadio(PluginUi::tuneKey(plug(0, "A")), 3);
    CHECK(H(0)->radio_index(C(0)) == 3);
    CHECK(H(1)->radio_index(C(1)) == 1);   // each plugin its own
    core.setClientRadio(PluginUi::tuneKey(plug(1, "B")), 8);
    CHECK(H(1)->radio_index(C(1)) == 8);
    core.setClientRadio(PluginUi::tuneKey(plug(0, "A")), 1);
    CHECK(H(0)->radio_index(C(0)) == 1);
    CHECK(H(0)->radio_index(C(0)) > 0 && H(1)->radio_index(C(1)) > 0);   // never 0
    // NULL context: refused, not a crash
    CHECK(H(0)->radio_index(nullptr) < 0);
    // gone
    const CascadeHostApi* h = H(0);
    void* ctx = C(0);
    ui.rebuild({});
    CHECK(h->radio_index(ctx) == CASCADE_API_DETACHED);
}

void testRefusals() {
    std::printf("[F3] every refusal\n");
    PluginApiCore::setFaultClockForTest(&fakeClock);
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    (void)take(core);
    for (const std::uint32_t kind : {0u, 5u, 6u, 255u, 0xFFFFFFFFu}) { CHECK(H(0)->report_fault(C(0), kind, 1) == CASCADE_API_BAD_ARGUMENT); }
    for (const std::uint32_t kind : {CASCADE_FAULT_CRASH, CASCADE_FAULT_STALL, CASCADE_FAULT_BREAK, CASCADE_FAULT_GAVE_UP}) {
        CHECK(H(0)->report_fault(C(0), kind, 0) == CASCADE_API_BAD_ARGUMENT);
        CHECK(H(0)->report_fault(C(0), kind, CASCADE_FAULT_COUNT_MAX + 1u) == CASCADE_API_OUT_OF_RANGE);
        CHECK(H(0)->report_fault(C(0), kind, 0xFFFFFFFFu) == CASCADE_API_OUT_OF_RANGE);
    }
    CHECK(H(0)->report_fault(nullptr, CASCADE_FAULT_CRASH, 1) < 0);
    const std::array<std::uint64_t, 4> none = take(core);
    CHECK(none == (std::array<std::uint64_t, 4>{0, 0, 0, 0}));   // nothing refused was kept, and a refusal does not start the minute
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_CRASH, CASCADE_FAULT_COUNT_MAX) == CASCADE_API_OK);   // the largest count is fine
    CHECK(take(core)[0] == CASCADE_FAULT_COUNT_MAX);
    PluginApiCore::setFaultClockForTest(nullptr);
}

void testMinuteRule() {
    std::printf("[F4] one a minute per plugin and kind\n");
    PluginApiCore::setFaultClockForTest(&fakeClock);
    g_nowMs = 5'000'000;
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A"), plug(1, "B")});
    if (H(0) == nullptr || H(1) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    (void)take(core);
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_CRASH, 2) == CASCADE_API_OK);
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_CRASH, 5) == CASCADE_API_BUSY);   // the same plugin, the same kind, inside the minute
    g_nowMs += PluginApiCore::kFaultMinGapMs - 1;
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_CRASH, 5) == CASCADE_API_BUSY);   // 59.999 s
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_STALL, 1) == CASCADE_API_OK);     // another kind is not held by it
    CHECK(H(1)->report_fault(C(1), CASCADE_FAULT_CRASH, 7) == CASCADE_API_OK);     // another plugin is not held by it
    g_nowMs += 1;
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_CRASH, 3) == CASCADE_API_OK);     // a minute on: accepted again
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_BREAK, 4) == CASCADE_API_OK);
    CHECK(H(0)->report_fault(C(0), CASCADE_FAULT_GAVE_UP, 1) == CASCADE_API_OK);
    // the sums are the accepted counts only: crash 2 + 7 + 3, stall 1, break 4, gave up 1
    const std::array<std::uint64_t, 4> got = take(core);
    CHECK(got[0] == 12 && got[1] == 1 && got[2] == 4 && got[3] == 1);
    CHECK(take(core) == (std::array<std::uint64_t, 4>{0, 0, 0, 0}));   // taken once
    PluginApiCore::setFaultClockForTest(nullptr);
}

void testGone() {
    std::printf("[F5] a plugin that is gone\n");
    PluginApiCore::setFaultClockForTest(&fakeClock);
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    if (H(0) == nullptr) { CHECK(false); return; }
    PluginApiCore& core = ui.api();
    (void)take(core);
    const CascadeHostApi* h = H(0);
    void* ctx = C(0);
    ui.rebuild({});
    CHECK(h->report_fault(ctx, CASCADE_FAULT_CRASH, 1) == CASCADE_API_DETACHED);
    CHECK(take(core) == (std::array<std::uint64_t, 4>{0, 0, 0, 0}));
    PluginApiCore::setFaultClockForTest(nullptr);
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<fs::path> issueFiles(const fs::path& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().filename().string().rfind("issue-", 0) == 0) { v.push_back(e.path()); }
    }
    return v;
}

void testSwitch(const fs::path& dir) {
    std::printf("[F6] the switch\n");
    auto ledger = health::globalLedger();
    ledger->reset();
    ledger->arm("", cascade::core::newInstallId(), false);
    cascade::core::CrashHandlerConfig on;
    on.crashDir = dir.string();
    on.enabled = true;
    on.minidump = false;
    on.exitAfterReport = false;
    cascade::core::installCrashHandlers(on);
    issue::resetIssueReportsForTest();

    // THE DEFAULT IS OFF: the constant the release ships with
    companion::setReportsOnForTest(-1);
    CHECK(!companion::kCompanionFaultReportsOn);
    CHECK(!companion::reportsOn());
    const std::array<std::uint64_t, 4> sums{3, 2, 1, 1};
    CHECK(companion::file(sums) == 0);
    CHECK(ledger->counts().empty());
    CHECK(issueFiles(dir).empty());
    CHECK(issue::takeNewlyFiled() == 0);

    // ON: counted under each word and filed once a session
    companion::setReportsOnForTest(1);
    CHECK(companion::reportsOn());
    CHECK(companion::file(sums) == 4);
    const auto counts = ledger->counts();
    CHECK(counts.count("recovered.companioncrash") == 1 && counts.at("recovered.companioncrash") == 3);
    CHECK(counts.count("recovered.companionstall") == 1 && counts.at("recovered.companionstall") == 2);
    CHECK(counts.count("recovered.companionbreak") == 1 && counts.at("recovered.companionbreak") == 1);
    CHECK(counts.count("recovered.companiongaveup") == 1 && counts.at("recovered.companiongaveup") == 1);
    const std::vector<fs::path> files = issueFiles(dir);
    CHECK(files.size() == 4);
    std::string all;
    for (const fs::path& f : files) { all += readAll(f); }
    // each report's reason is the word's fixed sentence; nothing of a plugin is in any of them
    for (const health::Recovered w : {health::Recovered::CompanionCrash, health::Recovered::CompanionStall, health::Recovered::CompanionBreak, health::Recovered::CompanionGaveUp}) {
        CHECK(all.find(issue::issueReason(w)) != std::string::npos);
    }
    CHECK(all.find("C:/plugins") == std::string::npos && all.find("A.dll") == std::string::npos && all.find("blamed-plugin:") == std::string::npos);
    // once a session per word: the same again writes no new file but is still counted
    CHECK(companion::file(sums) == 4);
    CHECK(issueFiles(dir).size() == 4);
    CHECK(ledger->counts().at("recovered.companioncrash") == 6);
    // nothing reported: nothing done
    CHECK(companion::file(std::array<std::uint64_t, 4>{0, 0, 0, 0}) == 0);

    // and back off: held back again
    companion::setReportsOnForTest(0);
    CHECK(companion::file(sums) == 0);
    CHECK(ledger->counts().at("recovered.companioncrash") == 6);
    companion::setReportsOnForTest(-1);
    ledger->reset();
    issue::resetIssueReportsForTest();
}

}  // namespace

int main() {
    std::printf("test_plugin_radio_fault\n");
    const fs::path dir = fs::temp_directory_path() / ("cascade-radio-fault-" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    testTable();
    testRadioIndex();
    testRefusals();
    testMinuteRule();
    testGone();
    testSwitch(dir);
    fs::remove_all(dir, ec);
    return testSummary("test_plugin_radio_fault");
}
