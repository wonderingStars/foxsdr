// test_plugin_schedule_hint.cpp - THE PLUGIN'S SIDE OF TIMED RECORDINGS: the additive host call a plugin makes to say "this pass will be worth recording"
// (plugin_abi.h: CascadeScheduleHint, CascadeHostApi::set_schedule_hint and clear_schedule_hints, appended after host API level 2; core/plugin_api.*).
// The satellites plugin is not changed by this lane (it lives in another repository); this is the proof that what it will call behaves, with a
// stand-in plugin built the way test_plugin_api.cpp builds its fakes. The engine's side (the facts and the op that makes an entry from a hint the
// user picks) is tests/test_engine_api_schedule.cpp's.
//
//   H1  THE TABLE GREW AT THE END: nothing before the new members moved; an old host table (without them) is told apart by CASCADE_HOST_HAS; the
//       apiLevel is unchanged (the struct size is the version, as the header says).
//   H2  A HINT IS COPIED: the plugin reusing its buffer changes nothing; replaced by id; a short struct is read as far as it goes; the texts are cut
//       to whole characters.
//   H3  EVERY REFUSAL: NULL, a short struct, non-finite numbers, a span that ends before it starts or runs over 24 h, a frequency outside (0, 6 GHz),
//       an elevation outside 0..90, a bandwidth outside 0..20 MHz, an unknown kind or mode, a date outside 2020..2100.
//   H4  THE LIMIT: sixteen per plugin and the seventeenth is LIMIT; a replacement at the limit is fine; one plugin's hints are not another's.
//   H5  THEY ARE FORGOTTEN with the plugin (the marker contract): clear, and a rebuild that drops the plugin.
//   H6  NOTHING IS DONE BY A HINT: no tune, no control, no log line is produced by posting one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
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

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

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

struct Counts {
    std::atomic<int> tune{0}, time{0};
};

HostServices services(Counts* n) {
    HostServices s;
    s.centreHz = [] { return 145.0e6; };
    s.sampleRateHz = [] { return 2.4e6; };
    s.tune = [n](double) {
        ++n->tune;
        return static_cast<std::int32_t>(CASCADE_TUNE_OK);
    };
    s.unixTimeMs = [n] {
        ++n->time;
        return static_cast<std::int64_t>(1'790'000'000'000LL);
    };
    return s;
}

constexpr std::int64_t kAos = 1'790'000'600'000LL;   // a pass in 2026
CascadeScheduleHint hint(std::uint32_t id, const char* label = "METEOR-M2 4") {
    CascadeScheduleHint h{};
    h.structSize = sizeof(h);
    h.id = id;
    h.kind = CASCADE_SCHEDULE_HINT_PASS;
    h.demodMode = CASCADE_DEMOD_NFM;
    h.aosUnixMs = kAos + static_cast<std::int64_t>(id) * 7000000LL;
    h.losUnixMs = h.aosUnixMs + 600'000;
    h.maxElDeg = 42.0;
    h.freqHz = 137.9e6;
    h.bandwidthHz = 144000.0;
    std::snprintf(h.label, sizeof(h.label), "%s", label);
    std::snprintf(h.decoderName, sizeof(h.decoderName), "%s", "lrpt-decoder");
    return h;
}

std::vector<PluginApiCore::HostScheduleHint> listed(PluginUi& ui) {
    std::vector<PluginApiCore::HostScheduleHint> v;
    ui.api().scheduleHints(v);
    return v;
}

// A host table as an OLD host built it: everything up to and including settings_seq, nothing after.
struct OldTail {
    std::uint8_t bytes[offsetof(CascadeHostApi, set_schedule_hint)];
};

void testTable() {
    std::printf("[H1] the table\n");
    // The new members are the LAST two, appended after settings_seq: no existing member moved.
    static_assert(offsetof(CascadeHostApi, set_schedule_hint) >= offsetof(CascadeHostApi, settings_seq) + sizeof(uint64_t (*)(void*)), "appended after settings_seq");
    static_assert(offsetof(CascadeHostApi, clear_schedule_hints) == offsetof(CascadeHostApi, set_schedule_hint) + sizeof(void*), "the two are adjacent");
    // (the decode events were appended after them - tests/test_plugin_events.cpp holds that - so the pair is followed by exactly the three event members)
    static_assert(sizeof(CascadeHostApi) == offsetof(CascadeHostApi, clear_schedule_hints) + sizeof(void*) + 3u * sizeof(void*), "and followed by the three event members only");
    Counts n;
    PluginUi ui;
    ui.setServices(services(&n));
    ui.rebuild({plug(0, "Sat")});
    const CascadeHostApi* h = H(0);
    CHECK(h != nullptr);
    if (h == nullptr) { return; }
    CHECK(h->structSize == sizeof(CascadeHostApi));
    CHECK(CASCADE_HOST_HAS(h, set_schedule_hint) && CASCADE_HOST_HAS(h, clear_schedule_hints));
    CHECK(h->apiLevel == CASCADE_HOST_API_LEVEL);   // not bumped: the size is the version
    // an old host's table: the macros say no, and a plugin that checks never touches the member
    CascadeHostApi old{};
    old.structSize = static_cast<std::uint32_t>(offsetof(CascadeHostApi, set_schedule_hint));
    CHECK(!CASCADE_HOST_COVERS(&old, set_schedule_hint) && !CASCADE_HOST_HAS(&old, set_schedule_hint));
    CHECK(CASCADE_HOST_COVERS(&old, settings_seq));
    CHECK(sizeof(OldTail) == offsetof(CascadeHostApi, set_schedule_hint));
    // the struct is laid out for any compiler: 8-byte aligned, fixed size
    CHECK(sizeof(CascadeScheduleHint) == 4 * 4 + 8 + 8 + 8 + 8 + 8 + 2 * CASCADE_SCHEDULE_LABEL_CHARS);
}

void testCopy() {
    std::printf("[H2] copying\n");
    Counts n;
    PluginUi ui;
    ui.setServices(services(&n));
    ui.rebuild({plug(0, "Sat")});
    const CascadeHostApi* h = H(0);
    void* c = C(0);
    const std::uint64_t seq0 = ui.api().scheduleHintsSeq();
    CascadeScheduleHint a = hint(1, "NOAA 19");
    CHECK(h->set_schedule_hint(c, &a) == CASCADE_API_OK);
    CHECK(ui.api().scheduleHintsSeq() > seq0);
    // the plugin reuses its buffer: the host's copy does not change
    std::snprintf(a.label, sizeof(a.label), "%s", "CHANGED");
    a.freqHz = 1.0;
    std::vector<PluginApiCore::HostScheduleHint> v = listed(ui);
    CHECK(v.size() == 1u);
    if (!v.empty()) {
        CHECK(v[0].plugin == "Sat.dll" && std::string(v[0].hint.label) == "NOAA 19" && v[0].hint.freqHz == 137.9e6);
        CHECK(v[0].hint.structSize == sizeof(CascadeScheduleHint) && std::string(v[0].hint.decoderName) == "lrpt-decoder");
    }
    // replaced by id: still one
    CascadeScheduleHint b = hint(1, "NOAA 19 again");
    b.maxElDeg = 71.0;
    const std::uint64_t seq1 = ui.api().scheduleHintsSeq();
    CHECK(h->set_schedule_hint(c, &b) == CASCADE_API_OK);
    CHECK(ui.api().scheduleHintsSeq() > seq1);
    v = listed(ui);
    CHECK(v.size() == 1u && !v.empty() && std::string(v[0].hint.label) == "NOAA 19 again" && v[0].hint.maxElDeg == 71.0);
    // a different id is another hint
    CascadeScheduleHint d = hint(2);
    CHECK(h->set_schedule_hint(c, &d) == CASCADE_API_OK);
    CHECK(listed(ui).size() == 2u);
    // a plugin built against a SHORTER struct (an older revision of the header): its bytes are read as far as they go, the rest is zero
    struct Short {
        std::uint32_t structSize, id, kind, demodMode;
        std::int64_t aosUnixMs, losUnixMs;
        double maxElDeg, freqHz;
    } s{};
    s.structSize = sizeof(Short);   // a short struct is below the minimum this level takes
    s.id = 9;
    s.kind = CASCADE_SCHEDULE_HINT_PASS;
    s.aosUnixMs = kAos;
    s.losUnixMs = kAos + 1000;
    s.freqHz = 100e6;
    CHECK(h->set_schedule_hint(c, reinterpret_cast<const CascadeScheduleHint*>(&s)) == CASCADE_API_BAD_ARGUMENT);
    // a LONGER struct (a newer plugin on this host): the host takes what it knows
    struct Long {
        CascadeScheduleHint base;
        char extra[64];
    } l{};
    l.base = hint(3);
    l.base.structSize = sizeof(Long);
    std::memset(l.extra, 0x7F, sizeof(l.extra));
    CHECK(h->set_schedule_hint(c, &l.base) == CASCADE_API_OK);
    v = listed(ui);
    bool found3 = false;
    for (const auto& e : v) {
        if (e.hint.id == 3) { found3 = true; CHECK(e.hint.structSize == sizeof(CascadeScheduleHint)); }
    }
    CHECK(found3);
    // texts filled to the last byte (no terminator) are re-terminated, and cut at a whole UTF-8 character
    CascadeScheduleHint t = hint(4);
    std::memset(t.label, 'x', sizeof(t.label));
    for (std::size_t i = 0; i < sizeof(t.decoderName); ++i) { t.decoderName[i] = (i % 2 == 0) ? static_cast<char>(0xC3) : static_cast<char>(0xA9); }   // "e-acute" repeated, last byte is a lead byte
    CHECK(h->set_schedule_hint(c, &t) == CASCADE_API_OK);
    v = listed(ui);
    for (const auto& e : v) {
        if (e.hint.id != 4) { continue; }
        CHECK(std::strlen(e.hint.label) == CASCADE_SCHEDULE_LABEL_CHARS - 1);
        CHECK(std::strlen(e.hint.decoderName) < CASCADE_SCHEDULE_LABEL_CHARS && std::strlen(e.hint.decoderName) % 2 == 0);   // whole characters only
    }
}

void testRefusals() {
    std::printf("[H3] refusals\n");
    Counts n;
    PluginUi ui;
    ui.setServices(services(&n));
    ui.rebuild({plug(0, "Sat")});
    const CascadeHostApi* h = H(0);
    void* c = C(0);
    CHECK(h->set_schedule_hint(c, nullptr) == CASCADE_API_BAD_ARGUMENT);
    CascadeScheduleHint x = hint(1);
    x.structSize = 8;
    CHECK(h->set_schedule_hint(c, &x) == CASCADE_API_BAD_ARGUMENT);
    struct Case {
        const char* what;
        std::function<void(CascadeScheduleHint&)> mutate;
        std::int32_t want;
    };
    const Case cases[] = {
        {"NaN frequency", [](CascadeScheduleHint& h) { h.freqHz = kNaN; }, CASCADE_API_BAD_ARGUMENT},
        {"infinite elevation", [](CascadeScheduleHint& h) { h.maxElDeg = kInf; }, CASCADE_API_BAD_ARGUMENT},
        {"NaN bandwidth", [](CascadeScheduleHint& h) { h.bandwidthHz = kNaN; }, CASCADE_API_BAD_ARGUMENT},
        {"ends before it starts", [](CascadeScheduleHint& h) { h.losUnixMs = h.aosUnixMs - 1; }, CASCADE_API_BAD_ARGUMENT},
        {"zero length", [](CascadeScheduleHint& h) { h.losUnixMs = h.aosUnixMs; }, CASCADE_API_BAD_ARGUMENT},
        {"over 24 hours", [](CascadeScheduleHint& h) { h.losUnixMs = h.aosUnixMs + 24LL * 3600 * 1000 + 1; }, CASCADE_API_OUT_OF_RANGE},
        {"frequency zero", [](CascadeScheduleHint& h) { h.freqHz = 0.0; }, CASCADE_API_OUT_OF_RANGE},
        {"frequency negative", [](CascadeScheduleHint& h) { h.freqHz = -1.0; }, CASCADE_API_OUT_OF_RANGE},
        {"frequency 7 GHz", [](CascadeScheduleHint& h) { h.freqHz = 7.0e9; }, CASCADE_API_OUT_OF_RANGE},
        {"elevation 91", [](CascadeScheduleHint& h) { h.maxElDeg = 91.0; }, CASCADE_API_OUT_OF_RANGE},
        {"elevation -1", [](CascadeScheduleHint& h) { h.maxElDeg = -1.0; }, CASCADE_API_OUT_OF_RANGE},
        {"bandwidth 21 MHz", [](CascadeScheduleHint& h) { h.bandwidthHz = 21.0e6; }, CASCADE_API_OUT_OF_RANGE},
        {"bandwidth negative", [](CascadeScheduleHint& h) { h.bandwidthHz = -5.0; }, CASCADE_API_OUT_OF_RANGE},
        {"unknown kind", [](CascadeScheduleHint& h) { h.kind = 9; }, CASCADE_API_OUT_OF_RANGE},
        {"kind 0", [](CascadeScheduleHint& h) { h.kind = 0; }, CASCADE_API_OUT_OF_RANGE},
        {"mode 9", [](CascadeScheduleHint& h) { h.demodMode = 9; }, CASCADE_API_OUT_OF_RANGE},
        {"date in 1999", [](CascadeScheduleHint& h) { h.aosUnixMs = 920'000'000'000LL; h.losUnixMs = h.aosUnixMs + 1000; }, CASCADE_API_OUT_OF_RANGE},
        {"date in 2200", [](CascadeScheduleHint& h) { h.aosUnixMs = 7'300'000'000'000LL; h.losUnixMs = h.aosUnixMs + 1000; }, CASCADE_API_OUT_OF_RANGE},
    };
    for (const Case& k : cases) {
        CascadeScheduleHint x2 = hint(1);
        k.mutate(x2);
        const std::int32_t r = h->set_schedule_hint(c, &x2);
        CHECK(r == k.want);
        if (r != k.want) { std::printf("      %s: %d (wanted %d)\n", k.what, r, k.want); }
    }
    CHECK(listed(ui).empty());   // none of them was kept
    // the boundaries that ARE allowed
    CascadeScheduleHint ok = hint(1);
    ok.maxElDeg = 0.0;
    ok.bandwidthHz = 0.0;
    ok.demodMode = 0;
    ok.freqHz = 1.0;
    ok.losUnixMs = ok.aosUnixMs + 24LL * 3600 * 1000;
    CHECK(h->set_schedule_hint(c, &ok) == CASCADE_API_OK);
    ok.id = 2;
    ok.maxElDeg = 90.0;
    ok.bandwidthHz = 20.0e6;
    ok.demodMode = 8;
    ok.freqHz = 5.99e9;
    CHECK(h->set_schedule_hint(c, &ok) == CASCADE_API_OK);
    CHECK(listed(ui).size() == 2u);
    // no ctx: detached / unknown contexts answer without touching anything
    CHECK(h->set_schedule_hint(nullptr, &ok) != CASCADE_API_OK);
    CHECK(h->clear_schedule_hints(nullptr) != CASCADE_API_OK);
}

void testLimitAndForget() {
    std::printf("[H4/H5] the limit, and forgetting\n");
    Counts n;
    PluginUi ui;
    ui.setServices(services(&n));
    ui.rebuild({plug(0, "Sat"), plug(1, "Other")});
    const CascadeHostApi* h = H(0);
    const CascadeHostApi* g = H(1);
    void* c = C(0);
    void* c1 = C(1);
    for (std::uint32_t i = 1; i <= CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN; ++i) {
        CascadeScheduleHint x = hint(i);
        CHECK(h->set_schedule_hint(c, &x) == CASCADE_API_OK);
    }
    CascadeScheduleHint over = hint(CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN + 1);
    CHECK(h->set_schedule_hint(c, &over) == CASCADE_API_LIMIT);
    CascadeScheduleHint again = hint(5, "replaced at the limit");
    CHECK(h->set_schedule_hint(c, &again) == CASCADE_API_OK);          // a replacement is not an addition
    CHECK(listed(ui).size() == static_cast<std::size_t>(CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN));
    // another plugin has its own sixteen
    CascadeScheduleHint other = hint(1, "OTHER");
    CHECK(g->set_schedule_hint(c1, &other) == CASCADE_API_OK);
    const auto all = listed(ui);
    CHECK(all.size() == static_cast<std::size_t>(CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN) + 1u);
    int fromOther = 0;
    for (const auto& e : all) { fromOther += e.plugin == "Other.dll" ? 1 : 0; }
    CHECK(fromOther == 1);
    // clear removes this plugin's only
    const std::uint64_t seq = ui.api().scheduleHintsSeq();
    CHECK(h->clear_schedule_hints(c) == CASCADE_API_OK);
    CHECK(ui.api().scheduleHintsSeq() > seq);
    CHECK(listed(ui).size() == 1u);
    CHECK(h->clear_schedule_hints(c) == CASCADE_API_OK);              // already empty: fine, and the counter does not move
    const std::uint64_t seq2 = ui.api().scheduleHintsSeq();
    CHECK(h->clear_schedule_hints(c) == CASCADE_API_OK);
    CHECK(ui.api().scheduleHintsSeq() == seq2);
    // the plugin is dropped by a rebuild: its hints go with it (nothing is left behind for a plugin that is gone)
    CascadeScheduleHint x = hint(1);
    CHECK(h->set_schedule_hint(c, &x) == CASCADE_API_OK);
    CHECK(listed(ui).size() == 2u);
    ui.rebuild({plug(1, "Other")});
    const auto left = listed(ui);
    CHECK(left.size() == 1u && !left.empty() && left[0].plugin == "Other.dll");
    // H6: nothing was done by any of it
    CHECK(n.tune.load() == 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testTable();
    testCopy();
    testRefusals();
    testLimitAndForget();
    return testSummary("test_plugin_schedule_hint");
}
