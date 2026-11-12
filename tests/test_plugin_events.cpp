// test_plugin_events.cpp - DECODE EVENTS: the additive host calls a plugin makes to publish a structured event and to read what other plugins published
// (plugin_abi.h: CascadeEvent, CascadeHostApi::post_event / read_events / events_seq, appended after host API level 2; core/plugin_api.*; brief item 13,
// "AD3"). Stand-in plugins built the way test_plugin_schedule_hint.cpp builds its fakes. The engine's feed of its own decoder lines into the same ring is
// tests/test_engine_api_events.cpp's.
//
//   E1  THE TABLE GREW AT THE END: nothing before the new members moved; an old host table (without them) is told apart by CASCADE_HOST_HAS
//   E2  POST AND READ: a post is copied (the plugin reusing its buffer changes nothing), stamped with the plugin's name, a sequence number from 1 and the
//       host's time; another plugin reads it; "after" and "cap" are honoured, oldest first; a short out struct is filled as far as it goes
//   E3  EVERY REFUSAL: NULL, a short struct, a kind that is empty, upper case, has a space, or is not terminated, a non-finite frequency, one below 0 or
//       above 6 GHz; DETACHED once the plugin is gone; control characters in the text become spaces and a long text is cut on a whole character
//   E4  THE RING: 1,100 posts leave the newest 1,024, sequence numbers never reused, events_seq is the newest
//   E5  PRIVATE: a plugin that sets CASCADE_FLAG_PRIVATE_DATA reads its own events and no other plugin does; neither do the host's text events for it
//   E6  THE HOST'S OWN EVENTS (kind "text") are readable by every plugin, with the frequency
//   E7  THREADS: four writers and a reader at once - every sequence number once, ascending, no event torn
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <thread>
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

struct Captured {
    const CascadeHostApi* api = nullptr;
};
Captured g_cap[3];

template <int I>
void attachI(const CascadeHostApi* h) {
    g_cap[I].api = h;
}
const CascadeHostClientApi kHc[3] = {{sizeof(CascadeHostClientApi), &attachI<0>}, {sizeof(CascadeHostClientApi), &attachI<1>}, {sizeof(CascadeHostClientApi), &attachI<2>}};

LoadedPlugin plug(int i, const char* name, bool priv = false) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.capabilities = CASCADE_CAP_HOST_CLIENT | (priv ? CASCADE_FLAG_PRIVATE_DATA : 0u);
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

CascadeEvent ev(const char* kind, const char* text, double hz = 0.0) {
    CascadeEvent e{};
    e.structSize = sizeof(e);
    e.frequencyHz = hz;
    std::snprintf(e.kind, sizeof(e.kind), "%s", kind);
    std::snprintf(e.text, sizeof(e.text), "%s", text);
    return e;
}

std::vector<CascadeEvent> readAll(int who, std::uint64_t after = 0, std::uint32_t cap = 4096) {
    std::vector<CascadeEvent> out(cap);
    for (auto& e : out) { e.structSize = sizeof(CascadeEvent); }
    const std::int32_t n = H(who)->read_events(C(who), after, out.data(), cap);
    if (n < 0) { return {}; }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

void testTable() {
    std::printf("[E1] the table\n");
    static_assert(offsetof(CascadeHostApi, post_event) == offsetof(CascadeHostApi, clear_schedule_hints) + sizeof(void*), "appended after the schedule hints");
    static_assert(offsetof(CascadeHostApi, read_events) == offsetof(CascadeHostApi, post_event) + sizeof(void*), "adjacent");
    static_assert(offsetof(CascadeHostApi, events_seq) == offsetof(CascadeHostApi, read_events) + sizeof(void*), "adjacent");
    static_assert(sizeof(CascadeHostApi) == offsetof(CascadeHostApi, events_seq) + sizeof(void*), "and last");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "A")});
    const CascadeHostApi* h = H(0);
    CHECK(h != nullptr);
    if (h == nullptr) { return; }
    CHECK(h->structSize == sizeof(CascadeHostApi));
    CHECK(CASCADE_HOST_HAS(h, post_event) && CASCADE_HOST_HAS(h, read_events) && CASCADE_HOST_HAS(h, events_seq));
    CHECK(h->apiLevel == CASCADE_HOST_API_LEVEL);   // the struct size is the version: the level did not move
    // an OLD host's table ends before post_event: a plugin that asks CASCADE_HOST_COVERS sees it is not there
    CascadeHostApi old = *h;
    old.structSize = static_cast<std::uint32_t>(offsetof(CascadeHostApi, post_event));
    CHECK(!CASCADE_HOST_COVERS(&old, post_event));
    CHECK(CASCADE_HOST_COVERS(h, post_event));
}

void testPostRead() {
    std::printf("[E2] post and read\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "Alpha"), plug(1, "Beta")});
    CHECK(H(0) != nullptr && H(1) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr) { return; }
    CHECK(H(1)->events_seq(C(1)) == 0u);
    CascadeEvent e = ev("message", "first", 148.5e6);
    CHECK(H(0)->post_event(C(0), &e) == CASCADE_API_OK);
    std::snprintf(e.text, sizeof(e.text), "%s", "REUSED BUFFER");   // the plugin reuses its buffer: the copy the host took does not change
    CHECK(H(0)->post_event(C(0), &e) == CASCADE_API_OK);
    CascadeEvent third = ev("alert", "third");
    CHECK(H(0)->post_event(C(0), &third) == CASCADE_API_OK);
    CHECK(H(1)->events_seq(C(1)) == 3u);
    std::vector<CascadeEvent> got = readAll(1);
    CHECK(got.size() == 3u);
    if (got.size() == 3u) {
        CHECK(got[0].seq == 1u && got[1].seq == 2u && got[2].seq == 3u);   // oldest first, from 1
        CHECK(std::string(got[0].plugin) == "Alpha");
        CHECK(std::string(got[0].kind) == "message" && std::string(got[0].text) == "first");
        CHECK(std::string(got[1].text) == "REUSED BUFFER");
        CHECK(std::fabs(got[0].frequencyHz - 148.5e6) < 1.0);
        CHECK(got[0].unixMs > 1'000'000'000'000LL);   // the host's clock, stamped by the host
        CHECK(got[0].flags == 0u);
        CHECK(got[0].structSize == sizeof(CascadeEvent));
    }
    // "after" and "cap"
    CHECK(readAll(1, 1).size() == 2u);
    CHECK(readAll(1, 3).empty());
    CHECK(readAll(1, 0, 2).size() == 2u);
    CHECK(readAll(1, 0, 2)[1].seq == 2u);
    {   // cap 0 asks for nothing and gets nothing
        CascadeEvent one{};
        one.structSize = sizeof(one);
        CHECK(H(1)->read_events(C(1), 0, &one, 0) == 0);
    }
    // a plugin reads its own events too, and the posting plugin's name is the host's, not the plugin's claim
    CascadeEvent lie = ev("message", "who am I");
    std::snprintf(lie.plugin, sizeof(lie.plugin), "%s", "SomeoneElse");
    lie.seq = 999;
    lie.unixMs = 5;
    lie.flags = 0xFFFFFFFFu;
    CHECK(H(0)->post_event(C(0), &lie) == CASCADE_API_OK);
    got = readAll(0, 3);
    CHECK(got.size() == 1u);
    if (!got.empty()) {
        CHECK(std::string(got[0].plugin) == "Alpha");
        CHECK(got[0].seq == 4u);
        CHECK(got[0].unixMs > 1'000'000'000'000LL);
        CHECK(got[0].flags == 0u);
    }
    {   // a short out struct is filled as far as it goes, and its size is left as the reader set it
        std::vector<CascadeEvent> out(1);
        out[0].structSize = static_cast<std::uint32_t>(offsetof(CascadeEvent, text) + CASCADE_EVENT_TEXT_CHARS);   // exactly the minimum
        CHECK(H(1)->read_events(C(1), 0, out.data(), 1) == 1);
        CHECK(out[0].structSize == offsetof(CascadeEvent, text) + CASCADE_EVENT_TEXT_CHARS);
        CHECK(std::string(out[0].text) == "first");
        out[0].structSize = 16;   // too short to hold the text: refused, nothing written
        CHECK(H(1)->read_events(C(1), 0, out.data(), 1) == CASCADE_API_BAD_ARGUMENT);
    }
}

void testRefusals() {
    std::printf("[E3] every refusal\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "Alpha")});
    CHECK(H(0) != nullptr);
    if (H(0) == nullptr) { return; }
    const CascadeHostApi* h = H(0);
    void* c = C(0);
    CascadeEvent good = ev("message", "ok");
    CHECK(h->post_event(c, nullptr) == CASCADE_API_BAD_ARGUMENT);
    CascadeEvent shortOne = good;
    shortOne.structSize = 20;
    CHECK(h->post_event(c, &shortOne) == CASCADE_API_BAD_ARGUMENT);
    for (const char* bad : {"", "Message", "two words", "ALERT", "bad!", "tab\t"}) {
        CascadeEvent b = ev(bad, "x");
        CHECK(h->post_event(c, &b) == CASCADE_API_BAD_ARGUMENT);
    }
    {   // a kind that fills the field with no terminator
        CascadeEvent b = good;
        std::memset(b.kind, 'a', sizeof(b.kind));
        CHECK(h->post_event(c, &b) == CASCADE_API_BAD_ARGUMENT);
    }
    {
        CascadeEvent b = ev("message", "x", kNaN);
        CHECK(h->post_event(c, &b) == CASCADE_API_BAD_ARGUMENT);
        b.frequencyHz = -1.0;
        CHECK(h->post_event(c, &b) == CASCADE_API_OUT_OF_RANGE);
        b.frequencyHz = 6.1e9;
        CHECK(h->post_event(c, &b) == CASCADE_API_OUT_OF_RANGE);
        b.frequencyHz = 6.0e9;
        CHECK(h->post_event(c, &b) == CASCADE_API_OK);   // the boundary is in
        b.frequencyHz = 0.0;
        CHECK(h->post_event(c, &b) == CASCADE_API_OK);
    }
    CHECK(h->events_seq(c) == 2u);   // only the two good ones were kept
    // control characters become spaces; a text that fills the field with no terminator is cut; a multi-byte character is never split
    {
        CascadeEvent b = ev("message", "");
        std::snprintf(b.text, sizeof(b.text), "line one\r\nline\ttwo\x01\x7f""end");
        CHECK(h->post_event(c, &b) == CASCADE_API_OK);
        std::memset(b.text, 'x', sizeof(b.text));   // no terminator at all
        CHECK(h->post_event(c, &b) == CASCADE_API_OK);
        std::memset(b.text, 'a', sizeof(b.text));   // 190 ASCII bytes, then a 2-byte character that does not fit
        b.text[CASCADE_EVENT_TEXT_CHARS - 3] = 'b';
        b.text[CASCADE_EVENT_TEXT_CHARS - 2] = static_cast<char>(0xC3);
        b.text[CASCADE_EVENT_TEXT_CHARS - 1] = static_cast<char>(0xA9);
        CHECK(h->post_event(c, &b) == CASCADE_API_OK);
        const std::vector<CascadeEvent> got = readAll(0, 2);
        CHECK(got.size() == 3u);
        if (got.size() == 3u) {
            CHECK(std::string(got[0].text) == "line one  line two  end");
            CHECK(std::strlen(got[1].text) <= CASCADE_EVENT_TEXT_CHARS - 1u);
            const std::string t = got[2].text;
            CHECK(t.size() <= CASCADE_EVENT_TEXT_CHARS - 1u);
            CHECK(t.empty() || (static_cast<unsigned char>(t.back()) & 0x80u) == 0u || (static_cast<unsigned char>(t.back()) & 0xC0u) == 0x80u);   // not a lead byte left dangling
        }
    }
    // gone: a plugin that is no longer live gets DETACHED from every call
    ui.rebuild({});
    CHECK(h->post_event(c, &good) == CASCADE_API_DETACHED);
    CascadeEvent one{};
    one.structSize = sizeof(one);
    CHECK(h->read_events(c, 0, &one, 1) == CASCADE_API_DETACHED);
}

void testRing() {
    std::printf("[E4] the ring\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "Alpha"), plug(1, "Beta")});
    CHECK(H(0) != nullptr && H(1) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr) { return; }
    for (int i = 1; i <= 1100; ++i) {
        char t[32];
        std::snprintf(t, sizeof(t), "n%d", i);
        CascadeEvent e = ev("message", t);
        CHECK(H(0)->post_event(C(0), &e) == CASCADE_API_OK);
    }
    CHECK(H(1)->events_seq(C(1)) == 1100u);
    const std::vector<CascadeEvent> got = readAll(1);
    CHECK(got.size() == CASCADE_EVENT_RING);
    if (got.size() == CASCADE_EVENT_RING) {
        CHECK(got.front().seq == 77u && got.back().seq == 1100u);   // the newest 1,024 of 1,100: the gap is visible in the first seq
        CHECK(std::string(got.front().text) == "n77" && std::string(got.back().text) == "n1100");
    }
    CHECK(readAll(1, 1090).size() == 10u);
}

void testPrivate() {
    std::printf("[E5] private plugins' events are their own\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "Open"), plug(1, "Secret", true), plug(2, "Reader")});
    CHECK(H(0) != nullptr && H(1) != nullptr && H(2) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr || H(2) == nullptr) { return; }
    CascadeEvent a = ev("message", "public one");
    CascadeEvent s = ev("message", "serial 4711");
    CascadeEvent b = ev("message", "public two");
    CHECK(H(0)->post_event(C(0), &a) == CASCADE_API_OK);
    CHECK(H(1)->post_event(C(1), &s) == CASCADE_API_OK);
    CHECK(H(0)->post_event(C(0), &b) == CASCADE_API_OK);
    // the host's own line of the private plugin (the engine posts every decoder line it drains), and of an ordinary one
    ui.api().hostPostEvent("Secret", true, CASCADE_EVENT_KIND_TEXT, "decoded id 4711", 0.0);
    ui.api().hostPostEvent("Open", false, CASCADE_EVENT_KIND_TEXT, "decoded fine", 0.0);
    const std::vector<CascadeEvent> other = readAll(2);
    CHECK(other.size() == 3u);   // public one, public two, decoded fine
    for (const CascadeEvent& e : other) { CHECK(std::string(e.text).find("4711") == std::string::npos); }
    const std::vector<CascadeEvent> open = readAll(0);
    CHECK(open.size() == 3u);
    const std::vector<CascadeEvent> own = readAll(1);   // the private plugin reads everything it may: its own two events and all the public ones
    CHECK(own.size() == 5u);
    int priv = 0;
    for (const CascadeEvent& e : own) { priv += (e.flags & CASCADE_EVENT_FLAG_PRIVATE) != 0u ? 1 : 0; }
    CHECK(priv == 2);
    // the sequence numbers still count the events a reader may not see (a gap is not a loss)
    CHECK(H(2)->events_seq(C(2)) == 5u);
    CHECK(other[0].seq == 1u && other[1].seq == 3u && other[2].seq == 5u);
}

void testHostText() {
    std::printf("[E6] the host's own events\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "Reader")});
    CHECK(H(0) != nullptr);
    if (H(0) == nullptr) { return; }
    ui.api().hostPostEvent("POCSAG", false, CASCADE_EVENT_KIND_TEXT, "1234567: HELLO\nWORLD", 148.0125e6, 1'790'000'123'456LL);
    const std::vector<CascadeEvent> got = readAll(0);
    CHECK(got.size() == 1u);
    if (!got.empty()) {
        CHECK(std::string(got[0].plugin) == "POCSAG" && std::string(got[0].kind) == CASCADE_EVENT_KIND_TEXT);
        CHECK(std::string(got[0].text) == "1234567: HELLO WORLD");
        CHECK(std::fabs(got[0].frequencyHz - 148.0125e6) < 1.0);
        CHECK(got[0].unixMs == 1'790'000'123'456LL);
    }
}

void testThreads() {
    std::printf("[E7] four writers and a reader\n");
    PluginUi ui;
    ui.setServices(services());
    ui.rebuild({plug(0, "W0"), plug(1, "W1"), plug(2, "Reader")});
    CHECK(H(0) != nullptr && H(1) != nullptr && H(2) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr || H(2) == nullptr) { return; }
    constexpr int kEach = 200;
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w] {
            const int who = w % 2;
            for (int i = 0; i < kEach; ++i) {
                char t[64];
                std::snprintf(t, sizeof(t), "w%d-%d", w, i);
                CascadeEvent e = ev("message", t);
                H(who)->post_event(C(who), &e);
            }
        });
    }
    std::uint64_t last = 0;
    std::set<std::uint64_t> seen;
    std::thread reader([&] {
        while (!stop.load()) {
            const std::vector<CascadeEvent> got = readAll(2, last, 128);
            for (const CascadeEvent& e : got) {
                if (e.seq <= last) { ++torn; }
                last = e.seq;
                seen.insert(e.seq);
                if (std::string(e.text).rfind('w', 0) != 0u || std::string(e.kind) != "message") { ++torn; }
            }
            std::this_thread::yield();
        }
    });
    for (auto& t : writers) { t.join(); }
    stop.store(true);
    reader.join();
    // drain what the reader had not reached yet
    for (const CascadeEvent& e : readAll(2, last)) { seen.insert(e.seq); }
    CHECK(torn.load() == 0);
    CHECK(seen.size() == static_cast<std::size_t>(4 * kEach));   // every sequence number once: none lost, none repeated
    CHECK(H(2)->events_seq(C(2)) == static_cast<std::uint64_t>(4 * kEach));
}

}  // namespace

int main() {
    testTable();
    testPostRead();
    testRefusals();
    testRing();
    testPrivate();
    testHostText();
    testThreads();
    return testSummary("test_plugin_events");
}
