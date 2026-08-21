// Tests for gui/track_info_cache.hpp - the join between the maps and a
// track-info plugin - driven through a scriptable fake of the C table, because
// the properties that matter here are all about the CALLS: that PENDING is
// re-asked, that READY is copied once and released once, that MISSING is
// remembered so the plugin is never asked again, and that third-party strings
// are bounded before the GUI renders them every frame.
//
// ALSO gui/basemap_cache.hpp's drainText, which is the same code draining the
// same kind of plugin against the same GUI thread and had the same unbounded
// loop. Its bound lives beside its twin's rather than in a file of its own so
// that changing one and not the other is visible in a single place.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstring>
#include <map>
#include <string>

#include "gui/basemap_cache.hpp"
#include "gui/track_info_cache.hpp"
#include "test_check.hpp"

using cascade::gui::BasemapCache;
using cascade::gui::TrackInfoCache;

namespace {

// The fake plugin. C function pointers cannot capture, so the script lives in
// globals reset by resetFake().
struct Scripted {
    std::int32_t result = CASCADE_INFO_PENDING;
    std::string registration, typeCode, typeName, manufacturer, operatorName, country;
};
std::map<std::string, Scripted> g_script;
int g_calls = 0;
int g_releases = 0;
int g_creates = 0;
int g_destroys = 0;

void* fakeCreate() {
    ++g_creates;
    return reinterpret_cast<void*>(1);
}
std::int32_t fakeGetInfo(void*, const char* id, std::uint32_t, CascadeTrackInfo* out) {
    ++g_calls;
    const auto it = g_script.find(id != nullptr ? id : "");
    if (it == g_script.end()) {
        return CASCADE_INFO_MISSING;
    }
    if (it->second.result != CASCADE_INFO_READY) {
        return it->second.result;
    }
    out->registration = it->second.registration.c_str();
    out->typeCode = it->second.typeCode.c_str();
    out->typeName = it->second.typeName.c_str();
    out->manufacturer = it->second.manufacturer.c_str();
    out->operatorName = it->second.operatorName.c_str();
    out->country = it->second.country.c_str();
    return CASCADE_INFO_READY;
}
void fakeRelease(void*, const CascadeTrackInfo*) { ++g_releases; }
std::int32_t fakePollText(void*, char* buf, size_t cap) {
    // One line, once. Enough to prove the drain path assembles lines.
    static bool sent = false;
    if (sent || cap < 8) {
        return 0;
    }
    sent = true;
    std::memcpy(buf, "hello\n", 6);
    return 6;
}
void fakeDestroy(void*) { ++g_destroys; }

// A plugin that never stops talking. Its own table (not fakeApi's) so the
// one-shot static in fakePollText stays untouched by these tests.
//
// It DOES stop eventually - g_floodLeft is finite - because a genuinely
// endless fake turns the unfixed code's failure into a hung test rather than a
// named one, and a hang proves nothing about the bound.
long g_floodLeft = 0;
long g_floodServed = 0;
std::int32_t floodPollText(void*, char* buf, size_t cap) {
    if (g_floodLeft <= 0 || cap < 8) {
        return 0;
    }
    --g_floodLeft;
    ++g_floodServed;
    std::memcpy(buf, "0123456\n", 8);
    return 8;
}

CascadeTrackInfoApi floodApi() {
    CascadeTrackInfoApi t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTrackInfoApi));
    t.create = &fakeCreate;
    t.get_info = &fakeGetInfo;
    t.release_info = &fakeRelease;
    t.poll_text = &floodPollText;
    t.destroy = &fakeDestroy;
    return t;
}

// The same never-stops-talking plugin, wearing the basemap table. Separate
// counters from the track-info flood so the two bounds are measured
// independently - a shared counter would let one test's polls satisfy the
// other's assertion.
long g_bmFloodLeft = 0;
long g_bmFloodServed = 0;
int g_bmCreates = 0;
int g_bmDestroys = 0;

void* bmCreate() {
    ++g_bmCreates;
    return reinterpret_cast<void*>(1);
}
std::int32_t bmGetTile(void*, std::uint32_t, std::uint32_t, std::uint32_t, CascadeTile*) {
    return CASCADE_TILE_MISSING;
}
void bmReleaseTile(void*, const CascadeTile*) {}
std::int32_t bmFloodPollText(void*, char* buf, size_t cap) {
    if (g_bmFloodLeft <= 0 || cap < 8) {
        return 0;
    }
    --g_bmFloodLeft;
    ++g_bmFloodServed;
    std::memcpy(buf, "0123456\n", 8);
    return 8;
}
void bmDestroy(void*) { ++g_bmDestroys; }

CascadeBasemapApi basemapFloodApi() {
    CascadeBasemapApi b{};
    b.structSize = static_cast<std::uint32_t>(sizeof(CascadeBasemapApi));
    b.attribution = "(c) test";
    b.minZoom = 0;
    b.maxZoom = 19;
    b.tileSize = 256;
    b.create = &bmCreate;
    b.get_tile = &bmGetTile;
    b.release_tile = &bmReleaseTile;
    b.poll_text = &bmFloodPollText;
    b.destroy = &bmDestroy;
    return b;
}

CascadeTrackInfoApi fakeApi() {
    CascadeTrackInfoApi t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTrackInfoApi));
    t.create = &fakeCreate;
    t.get_info = &fakeGetInfo;
    t.release_info = &fakeRelease;
    t.poll_text = &fakePollText;
    t.destroy = &fakeDestroy;
    return t;
}

void resetFake() {
    g_script.clear();
    g_calls = 0;
    g_releases = 0;
    g_creates = 0;
    g_destroys = 0;
}

void testPendingReadyMissingAndCaching() {
    resetFake();
    const CascadeTrackInfoApi api = fakeApi();
    TrackInfoCache c;
    c.attach(&api);
    CHECK(c.active());
    CHECK(g_creates == 1);

    // PENDING is not cached: each ask reaches the plugin, which is what tells
    // it the answer is still wanted.
    g_script["4CA123"].result = CASCADE_INFO_PENDING;
    CHECK(c.get("4CA123", 1) == nullptr);
    CHECK(c.get("4CA123", 1) == nullptr);
    CHECK(g_calls == 2);

    // READY is copied, RELEASED exactly once, and cached: the third ask does
    // not reach the plugin at all.
    Scripted& s = g_script["4CA123"];
    s.result = CASCADE_INFO_READY;
    s.registration = "EI-DCL";
    s.typeCode = "B738";
    s.typeName = "Boeing 737-8AS";
    s.operatorName = "Ryanair";
    s.country = "Ireland";
    const TrackInfoCache::Info* d = c.get("4CA123", 1);
    CHECK(d != nullptr);
    if (d != nullptr) {
        CHECK(d->known);
        CHECK(d->registration == "EI-DCL");
        CHECK(d->typeName == "Boeing 737-8AS");
        CHECK(d->operatorName == "Ryanair");
        CHECK(d->country == "Ireland");
    }
    CHECK(g_releases == 1);
    const int callsAfterReady = g_calls;
    CHECK(c.get("4CA123", 1) == d);
    CHECK(g_calls == callsAfterReady);

    // MISSING is a real answer: cached as known == false, plugin asked once.
    g_script["ABCDEF"].result = CASCADE_INFO_MISSING;
    const TrackInfoCache::Info* m = c.get("ABCDEF", 1);
    CHECK(m != nullptr && !m->known);
    const int callsAfterMissing = g_calls;
    CHECK(c.get("ABCDEF", 1) == m);
    CHECK(g_calls == callsAfterMissing);
    // And no release for an answer that carried no borrow.
    CHECK(g_releases == 1);

    c.detach();
    CHECK(g_destroys == 1);
    CHECK(!c.active());
    CHECK(c.get("4CA123", 1) == nullptr);  // detached: nothing served
}

void testFieldsAreBounded() {
    resetFake();
    const CascadeTrackInfoApi api = fakeApi();
    TrackInfoCache c;
    c.attach(&api);

    Scripted& s = g_script["A00001"];
    s.result = CASCADE_INFO_READY;
    s.operatorName.assign(4096, 'x');  // a plugin that fills every byte
    const TrackInfoCache::Info* d = c.get("A00001", 1);
    CHECK(d != nullptr && d->known);
    if (d != nullptr) {
        CHECK(d->operatorName.size() == TrackInfoCache::kMaxFieldBytes);
    }
}

void testEvictionIsBoundedAndOldestFirst() {
    resetFake();
    const CascadeTrackInfoApi api = fakeApi();
    TrackInfoCache c;
    c.attach(&api);

    char id[16];
    for (std::size_t i = 0; i < TrackInfoCache::kMaxEntries + 1; ++i) {
        std::snprintf(id, sizeof(id), "%06zX", i);
        Scripted& s = g_script[id];
        s.result = CASCADE_INFO_READY;
        s.registration = id;
        CHECK(c.get(id, 1) != nullptr);
    }
    CHECK(c.size() == TrackInfoCache::kMaxEntries);
    // The FIRST entry was the oldest and is gone: asking again reaches the
    // plugin afresh rather than serving a freed pointer.
    const int before = g_calls;
    CHECK(c.get("000000", 1) != nullptr);
    CHECK(g_calls == before + 1);
}

void testDrainTextAssemblesLines() {
    resetFake();
    const CascadeTrackInfoApi api = fakeApi();
    TrackInfoCache c;
    c.attach(&api);
    const std::vector<std::string> lines = c.drainText();
    CHECK(lines.size() == 1);
    if (!lines.empty()) {
        CHECK(lines[0] == "hello");
    }
}

void testDrainTextIsBoundedPerCall() {
    // THE FAILURE THIS GUARDS. drainText runs on the GUI thread, once per
    // frame, and polled the plugin until it said "nothing left". A plugin that
    // produces text faster than 60 Hz can drain it - a buggy one in a loop, or
    // a hostile one deliberately - never says that, so the frame never ends
    // and the whole application stops responding. Bounding the drain gives the
    // frame back; whatever is still queued is drained by the next frame,
    // exactly like the tile server's per-frame cap.
    resetFake();
    const CascadeTrackInfoApi api = floodApi();
    TrackInfoCache c;
    c.attach(&api);

    g_floodLeft = 200000;  // far more than any per-frame bound may take
    g_floodServed = 0;
    const std::vector<std::string> first = c.drainText();

    // The bound is on the POLLS, which is what costs the frame; each poll here
    // carries exactly one line, so the line count is the visible proxy.
    CHECK(g_floodServed <= TrackInfoCache::kMaxPollsPerDrain);
    CHECK(first.size() <= TrackInfoCache::kMaxPollsPerDrain);
    if (g_floodServed > TrackInfoCache::kMaxPollsPerDrain) {
        std::printf("FAIL one drainText call polled %ld times (bound %zu)\n", g_floodServed,
                    TrackInfoCache::kMaxPollsPerDrain);
    }
    // Bounded, not broken: it still delivered something, and the next frame
    // picks up where this one stopped rather than losing the backlog.
    CHECK(!first.empty());
    const long servedAfterFirst = g_floodServed;
    const std::vector<std::string> second = c.drainText();
    CHECK(!second.empty());
    CHECK(g_floodServed > servedAfterFirst);

    // And a plugin with nothing to say still costs exactly one poll.
    g_floodLeft = 0;
    const long before = g_floodServed;
    CHECK(c.drainText().empty());
    CHECK(g_floodServed == before);
}

void testBasemapDrainTextIsBoundedPerCall() {
    // THE SAME FAILURE, THE OTHER CACHE. BasemapCache::drainText also runs on
    // the GUI thread once per frame, and also polled the plugin until it said
    // "nothing left" - so a tile server whose plugin logs a line per failed
    // request faster than a frame drains them ends the frame never. This is
    // the more likely of the two to happen by accident: a wrong tile URL makes
    // every request fail, and failures are exactly what a plugin logs.
    const CascadeBasemapApi api = basemapFloodApi();
    g_bmCreates = 0;
    g_bmDestroys = 0;
    BasemapCache c;
    c.attach(&api);
    CHECK(c.active());
    CHECK(g_bmCreates == 1);

    g_bmFloodLeft = 200000;  // far more than any per-frame bound may take
    g_bmFloodServed = 0;
    const std::vector<std::string> first = c.drainText();

    // The bound is on the POLLS, which is what costs the frame; each poll here
    // carries exactly one line, so the line count is the visible proxy.
    CHECK(g_bmFloodServed <= BasemapCache::kMaxPollsPerDrain);
    CHECK(first.size() <= BasemapCache::kMaxPollsPerDrain);
    if (g_bmFloodServed > static_cast<long>(BasemapCache::kMaxPollsPerDrain)) {
        std::printf("FAIL one BasemapCache::drainText polled %ld times (bound %zu)\n",
                    g_bmFloodServed, BasemapCache::kMaxPollsPerDrain);
    }
    // Bounded, not broken: the frame still got status text, and the backlog is
    // taken by the next frame rather than dropped.
    CHECK(!first.empty());
    const long servedAfterFirst = g_bmFloodServed;
    const std::vector<std::string> second = c.drainText();
    CHECK(!second.empty());
    CHECK(g_bmFloodServed > servedAfterFirst);

    // And a plugin with nothing to say still costs exactly one poll.
    g_bmFloodLeft = 0;
    const long before = g_bmFloodServed;
    CHECK(c.drainText().empty());
    CHECK(g_bmFloodServed == before);

    c.detach();
    CHECK(g_bmDestroys == 1);
}

}  // namespace

int main() {
    testPendingReadyMissingAndCaching();
    testFieldsAreBounded();
    testEvictionIsBoundedAndOldestFirst();
    testDrainTextAssemblesLines();
    testDrainTextIsBoundedPerCall();
    testBasemapDrainTextIsBoundedPerCall();
    return testSummary("test_track_info_cache");
}
