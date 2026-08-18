// Tests for gui/track_info_cache.hpp - the join between the maps and a
// track-info plugin - driven through a scriptable fake of the C table, because
// the properties that matter here are all about the CALLS: that PENDING is
// re-asked, that READY is copied once and released once, that MISSING is
// remembered so the plugin is never asked again, and that third-party strings
// are bounded before the GUI renders them every frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstring>
#include <map>
#include <string>

#include "gui/track_info_cache.hpp"
#include "test_check.hpp"

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

}  // namespace

int main() {
    testPendingReadyMissingAndCaching();
    testFieldsAreBounded();
    testEvictionIsBoundedAndOldestFirst();
    testDrainTextAssemblesLines();
    return testSummary("test_track_info_cache");
}
