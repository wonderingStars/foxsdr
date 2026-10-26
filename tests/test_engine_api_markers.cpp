// test_engine_api_markers.cpp - THE WATERFALL'S MARKERS through the table (stub campaign B10; API 0.3 9.8; master 0.99.57), held against the real engine and its own
// passes - the way test_engine_api_settings holds the settings forms - with no window anywhere. The list and its numbering are core/freq_markers.hpp's
// (tests/test_freq_markers.cpp holds them alone); what is held HERE is what the engine does with them: the four ops, the list FREQ_MARKERS and its facts, who may send
// them, and the file the engine saves on a worker and loads at the next start.
//
// One group per rule; each was shown red first (against an engine whose marker ops answered UNSUPPORTED and which served no list) and then against a mutant of the code
// that holds it (docs/engine-stage4.md 6.22, B10):
//   M1  THE LIST AND ITS FACTS: empty at the start; a REMOTE session is DENIED the list and sees none of the facts;
//   M2  DROP: a new marker is OK with its number and "new" in applied[]; the list has it at the ABSOLUTE frequency given with the time the engine stamped; a second drop
//       within the merge reach is THAT marker (nothing added, nothing moved, nothing to save), one beyond it is the next; a frequency that is not positive is OUT_OF_RANGE;
//   M3  THE NUMBERS ARE NAMES: M1, M2, ... are never handed out again after a remove - and not after a restart either (the count is in the file); only CLEAR starts at M1;
//   M4  A NOTE: set, read whole (a 600-byte note, with a tab and a newline in it, survives; the list's name is the first 95 bytes), the same note again changes nothing, an
//       empty note clears it, an unknown number is NOT_FOUND;
//   M5  REMOVE AND CLEAR: an unknown number is NOT_FOUND and changes nothing; CLEAR of an empty list is NO_CHANGE;
//   M6  THE LIST IS FULL AT 200: the 201st drop is LIMIT, the list is unchanged;
//   M7  WHO MAY SEND THEM: the ops are application ops - a REMOTE session is DENIED all four - and a LOCAL session without the SETTINGS grant is DENIED; nothing moved;
//   M8  THE FILE: a drop, a note and a remove reach markers.json (saved on a worker, half a second after the change), and a new engine starts from it - the list, the notes
//       and the numbering;
//   M9  A DAMAGED FILE is reported (fact.markers.error, LOCAL only), kept aside, and the engine starts empty;
//   M10 A SETTLED ENGINE: listSeq does not move pass after pass, and an op that changes nothing does not move it either.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/freq_markers.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is. A hermetic engine (no front end) keeps its lists off the disk; the groups that are about the
// FILE start the engine the way a persistent front end does - loadBookmarks(), which loads bookmarks.json and markers.json and gives the engine their paths.
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void loadLists(Engine& e) { e.loadBookmarks(); }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kLocalGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS;

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Every place the engine could write is the process's own scratch tree - the markers' file included, which is what group M8 reads.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_markers_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

fs::path markersFile() { return fs::path(cascade::core::FreqMarkers::defaultPath()); }

// Every test starts from no file: the engine that follows loads nothing.
void freshFile() {
    std::error_code ec;
    fs::remove(markersFile(), ec);
    fs::create_directories(markersFile().parent_path(), ec);
}

std::string fact(testing::Session& s, const std::string& key, int32_t* status = nullptr) {
    char buf[8192];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    if (status != nullptr) { *status = n; }
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

void passes(cascade::engine::Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

// Submits ONE command - its text as the command's own when it fits, as long text beside it when it does not - and passes until its result lands.
testing::Outcome pumpedAs(cascade::engine::Engine& eng, FoxSession* session, const FoxCommand& c, const std::string* text = nullptr) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    int32_t n = 0;
    if (text != nullptr && text->size() >= FOXAPI_TEXT_CHARS) {
        n = api()->submit_text(session, &c, text->data(), static_cast<uint32_t>(text->size()), &sr);
    } else if (text != nullptr) {
        FoxCommand withText = c;
        std::snprintf(withText.text, sizeof(withText.text), "%s", text->c_str());
        n = api()->submit(session, &withText, 1, &sr);
    } else {
        n = api()->submit(session, &c, 1, &sr);
    }
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(session, r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

struct Direct {
    cascade::engine::Engine eng;
    explicit Direct(bool persistent = false) {
        eng.configureApi(kHooks);
        eng.initialise();
        if (persistent) { Access::loadLists(eng); }
        eng.controlPass();
    }
};

// ---- the commands, spelled once ----------------------------------------------------------------------------------------------------------
FoxCommand dropOp(double hz, double reachHz = 0.0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_MARKER_DROP, hz);
    c.num[1] = reachHz;
    return c;
}
FoxCommand removeOp(long long number) { return testing::cmd(FOXAPP_OP_MARKER_REMOVE, 0.0, number); }
FoxCommand clearOp() { return testing::cmd(FOXAPP_OP_MARKER_CLEAR); }
FoxCommand noteOp(long long number) { return testing::cmd(FOXAPP_OP_MARKER_NOTE, 0.0, number); }

testing::Outcome drop(cascade::engine::Engine& e, testing::Session& s, double hz, double reachHz = 0.0) { return pumpedAs(e, s.raw(), dropOp(hz, reachHz)); }
testing::Outcome remove(cascade::engine::Engine& e, testing::Session& s, long long n) { return pumpedAs(e, s.raw(), removeOp(n)); }
testing::Outcome clear(cascade::engine::Engine& e, testing::Session& s) { return pumpedAs(e, s.raw(), clearOp()); }
testing::Outcome note(cascade::engine::Engine& e, testing::Session& s, long long n, const std::string& text) { return pumpedAs(e, s.raw(), noteOp(n), &text); }

// The numbers in the list, in its order.
std::vector<long long> numbers(testing::Session& s) {
    std::vector<long long> out;
    const int32_t n = count(s, FOXAPI_LIST_FREQ_MARKERS);
    for (int32_t i = 0; i < n; ++i) { out.push_back(static_cast<long long>(item(s, FOXAPI_LIST_FREQ_MARKERS, static_cast<uint32_t>(i)).id)); }
    return out;
}

// The file as the next engine will read it, once the worker has written it (the save is half a second after the change, so this waits up to six seconds).
template <class F>
bool fileSays(cascade::engine::Engine& eng, F pred, int timeoutMs = 6000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        cascade::core::FreqMarkers m;
        std::string err;
        if (fs::exists(markersFile()) && m.load(markersFile().string(), err) && pred(m)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// M1 ----------------------------------------------------------------------------------------------------------------------------------------

void testListAndFacts() {
    freshFile();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 0);
    int32_t st = 0;
    const std::string v0 = fact(s, "markers.version", &st);
    CHECK_EQ(st >= 0, true);
    CHECK_EQ(fact(s, "markers.error"), std::string());

    CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 1);
    CHECK(fact(s, "markers.version") != v0);

    // A REMOTE SESSION: the list is DENIED and none of the facts is its to read (the notes are the local operator's words).
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    if (remote.ok()) {
        CHECK_EQ(count(remote, FOXAPI_LIST_FREQ_MARKERS), FOXAPI_DENIED);
        int32_t fst = 0;
        fact(remote, "markers.error", &fst);
        CHECK_EQ(fst, FOXAPI_DENIED);
        fact(remote, "markers.note.1", &fst);
        CHECK_EQ(fst, FOXAPI_DENIED);
    }
    std::printf("M1: the list is empty at the start, has the marker once dropped, and a REMOTE session is DENIED it and its facts\n");
}

// M2 ----------------------------------------------------------------------------------------------------------------------------------------

void testDrop() {
    freshFile();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);

    const std::int64_t before = static_cast<std::int64_t>(std::time(nullptr));
    uint64_t listSeq = s.state().listSeq;
    testing::Outcome o = drop(d.eng, s, 145500000.0, 2000.0);
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);  // M1
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 1);  // a new one
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 1);
    CHECK(s.state().listSeq != listSeq);
    FoxListItem it = item(s, FOXAPI_LIST_FREQ_MARKERS, 0);
    CHECK_EQ(static_cast<int>(it.id), 1);
    CHECK(std::fabs(it.value[0] - 145500000.0) < 0.5);  // ABSOLUTE hertz, exactly as given
    CHECK(it.value[1] >= static_cast<double>(before) && it.value[1] <= static_cast<double>(before) + 5.0);  // the engine's own clock
    CHECK_EQ(std::string(it.name), std::string());

    // THE MERGE: a second right-click on the same signal names the marker already there. Nothing is added, and nothing is moved or saved.
    const std::string version = fact(s, "markers.version");
    listSeq = s.state().listSeq;
    o = drop(d.eng, s, 145501500.0, 2000.0);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 1);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 0);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 1);
    CHECK_EQ(fact(s, "markers.version"), version);
    CHECK_EQ(s.state().listSeq, listSeq);
    CHECK(std::fabs(item(s, FOXAPI_LIST_FREQ_MARKERS, 0).value[0] - 145500000.0) < 0.5);  // the one that was there keeps its own frequency

    // Beyond the reach it is the next one; with no reach at all only the same frequency is the same marker.
    o = drop(d.eng, s, 145504000.0, 2000.0);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 1);
    o = drop(d.eng, s, 145504000.0);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 2);
    CHECK_EQ(static_cast<int>(o.result.applied[1]), 0);
    o = drop(d.eng, s, 145504001.0);
    CHECK_EQ(static_cast<int>(o.result.applied[0]), 3);
    passes(d.eng, 2);
    CHECK_EQ(numbers(s).size(), static_cast<std::size_t>(3));

    // A frequency that is not a frequency.
    for (const double bad : {0.0, -145.5e6}) {
        const testing::Outcome r = drop(d.eng, s, bad);
        CHECK(r.landed);
        CHECK_EQ(r.result.status, FOXAPI_OUT_OF_RANGE);
    }
    CHECK_EQ(numbers(s).size(), static_cast<std::size_t>(3));
    // ...and one that is not a number never reaches the engine (every numeric slot is checked at the door).
    const testing::Outcome nan = pumpedAs(d.eng, s.raw(), dropOp(std::nan("")));
    CHECK(nan.status() != FOXAPI_OK);
    CHECK_EQ(numbers(s).size(), static_cast<std::size_t>(3));
    std::printf("M2: a drop is the number and the absolute frequency, stamped by the engine; within the reach it is the marker already there; not positive is OUT_OF_RANGE\n");
}

// M3 ----------------------------------------------------------------------------------------------------------------------------------------

void testNumbersAreNames() {
    freshFile();
    {
        Direct d(true);
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        passes(d.eng, 3);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 144.8e6).result.applied[0]), 1);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 145.5e6).result.applied[0]), 2);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 146.5e6).result.applied[0]), 3);
        // M2 goes, and the next is M4 - "M3" written on a scrap of paper keeps meaning the same frequency, and "M2" never means another.
        CHECK_EQ(remove(d.eng, s, 2).status(), FOXAPI_OK);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 147.5e6).result.applied[0]), 4);
        // The LAST one goes too, and its number is still not handed out again.
        CHECK_EQ(remove(d.eng, s, 4).status(), FOXAPI_OK);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 148.5e6).result.applied[0]), 5);
        passes(d.eng, 2);
        const std::vector<long long> now = numbers(s);
        CHECK_EQ(now.size(), static_cast<std::size_t>(3));
        if (now.size() == 3u) {
            CHECK_EQ(now[0], 1);
            CHECK_EQ(now[1], 3);
            CHECK_EQ(now[2], 5);
        }
        // ...and not after a restart: the count is in the file.
        CHECK(fileSays(d.eng, [](const cascade::core::FreqMarkers& m) { return m.size() == 3u; }));
    }
    {
        Direct d(true);   // the next start
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        passes(d.eng, 3);
        CHECK_EQ(numbers(s).size(), static_cast<std::size_t>(3));
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 149.5e6).result.applied[0]), 6);
        // Only CLEAR starts the count again.
        CHECK_EQ(clear(d.eng, s).status(), FOXAPI_OK);
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 0);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 150.5e6).result.applied[0]), 1);
    }
    std::printf("M3: M2 and M4 are not handed out again after a remove (the last one included), not after a restart either; only CLEAR starts at M1\n");
}

// M4 ----------------------------------------------------------------------------------------------------------------------------------------

void testNote() {
    freshFile();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
    passes(d.eng, 2);

    uint64_t listSeq = s.state().listSeq;
    testing::Outcome o = note(d.eng, s, 1, "a birdie");
    CHECK_EQ(o.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(s.state().listSeq != listSeq);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_FREQ_MARKERS, 0).name), std::string("a birdie"));
    CHECK_EQ(fact(s, "markers.note.1"), std::string("a birdie"));

    // THE SAME NOTE AGAIN is no change: nothing is written.
    const std::string version = fact(s, "markers.version");
    listSeq = s.state().listSeq;
    CHECK_EQ(note(d.eng, s, 1, "a birdie").result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "markers.version"), version);
    CHECK_EQ(s.state().listSeq, listSeq);

    // THE WHOLE NOTE, as long text, with a tab and a newline in it: the list's name is cut at 95 bytes, the fact is not.
    std::string longNote = "first line\tcolumn two\nsecond line ";
    while (longNote.size() < 600u) { longNote += "0123456789"; }
    longNote.resize(600);
    CHECK_EQ(note(d.eng, s, 1, longNote).result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "markers.note.1"), longNote);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_FREQ_MARKERS, 0).name), longNote.substr(0, 95));

    // An empty note clears it; a number the list does not hold is NOT_FOUND and nothing moves.
    CHECK_EQ(note(d.eng, s, 1, "").result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "markers.note.1"), std::string());
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_FREQ_MARKERS, 0).name), std::string());
    const std::string v2 = fact(s, "markers.version");
    CHECK_EQ(note(d.eng, s, 9, "x").result.status, FOXAPI_NOT_FOUND);
    CHECK_EQ(note(d.eng, s, 0, "x").result.status, FOXAPI_NOT_FOUND);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "markers.version"), v2);
    std::printf("M4: a note is set, read whole (600 bytes, tab and newline kept; the list's name is its first 95), unchanged when repeated, cleared by an empty one; an unknown number is NOT_FOUND\n");
}

// M5 ----------------------------------------------------------------------------------------------------------------------------------------

void testRemoveAndClear() {
    freshFile();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(clear(d.eng, s).result.status, FOXAPI_NO_CHANGE);   // nothing to clear
    CHECK_EQ(remove(d.eng, s, 1).result.status, FOXAPI_NOT_FOUND);
    CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
    CHECK_EQ(drop(d.eng, s, 146.5e6).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const std::string version = fact(s, "markers.version");
    CHECK_EQ(remove(d.eng, s, 7).result.status, FOXAPI_NOT_FOUND);
    CHECK_EQ(remove(d.eng, s, 0).result.status, FOXAPI_NOT_FOUND);
    CHECK_EQ(remove(d.eng, s, -1).result.status, FOXAPI_NOT_FOUND);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "markers.version"), version);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 2);
    CHECK_EQ(remove(d.eng, s, 1).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(numbers(s).size(), static_cast<std::size_t>(1));
    CHECK_EQ(clear(d.eng, s).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 0);
    std::printf("M5: an unknown number is NOT_FOUND and moves nothing; CLEAR empties the list, and an empty list is NO_CHANGE\n");
}

// M6 ----------------------------------------------------------------------------------------------------------------------------------------

void testFull() {
    freshFile();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    for (std::size_t i = 0; i < cascade::core::FreqMarkers::kMaxMarkers; ++i) {
        // (a command batch is a queue of 256 at most: pass between the commands rather than flood it)
        const testing::Outcome o = drop(d.eng, s, 100.0e6 + 10000.0 * static_cast<double>(i));
        CHECK_EQ(o.status(), FOXAPI_OK);
    }
    passes(d.eng, 2);
    CHECK_EQ(static_cast<std::size_t>(count(s, FOXAPI_LIST_FREQ_MARKERS)), cascade::core::FreqMarkers::kMaxMarkers);
    const testing::Outcome o = drop(d.eng, s, 99.0e6);
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_LIMIT);
    passes(d.eng, 2);
    CHECK_EQ(static_cast<std::size_t>(count(s, FOXAPI_LIST_FREQ_MARKERS)), cascade::core::FreqMarkers::kMaxMarkers);
    // A drop that would only MERGE with a marker already there is no new marker and needs no room.
    const testing::Outcome same = drop(d.eng, s, 100.0e6, 10.0);
    CHECK_EQ(same.result.status, FOXAPI_OK);
    CHECK_EQ(static_cast<int>(same.result.applied[1]), 0);
    // Room again after a remove.
    CHECK_EQ(remove(d.eng, s, 5).status(), FOXAPI_OK);
    CHECK_EQ(drop(d.eng, s, 99.0e6).result.status, FOXAPI_OK);
    std::printf("M6: the 201st marker is LIMIT and changes nothing (a merging drop needs no room); a remove makes room\n");
}

// M7 ----------------------------------------------------------------------------------------------------------------------------------------

void testWhoMaySend() {
    freshFile();
    Direct d;
    testing::Session local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(local.ok());
    passes(d.eng, 3);
    CHECK_EQ(drop(d.eng, local, 145.5e6).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const std::string version = fact(local, "markers.version");

    // The grants are what every settings op needs: a LOCAL session with VIEW only is DENIED.
    testing::Session viewer(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    CHECK(viewer.ok());
    // A REMOTE session has the web remote's power and no more: the markers are application ops, which are the local operator's.
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE, "s3cret");
    CHECK(remote.ok());
    const std::string text = "x";
    for (testing::Session* who : {&viewer, &remote}) {
        CHECK_EQ(pumpedAs(d.eng, who->raw(), dropOp(146.5e6)).status(), FOXAPI_DENIED);
        CHECK_EQ(pumpedAs(d.eng, who->raw(), removeOp(1)).status(), FOXAPI_DENIED);
        CHECK_EQ(pumpedAs(d.eng, who->raw(), clearOp()).status(), FOXAPI_DENIED);
        CHECK_EQ(pumpedAs(d.eng, who->raw(), noteOp(1), &text).status(), FOXAPI_DENIED);
    }
    passes(d.eng, 2);
    CHECK_EQ(fact(local, "markers.version"), version);
    CHECK_EQ(count(local, FOXAPI_LIST_FREQ_MARKERS), 1);
    // (The ops are in kOps and application ops, and not in the web profile's closure of 38: test_apply_command and test_engine_api_03 hold the table itself.)
    for (const std::uint32_t op : {FOXAPP_OP_MARKER_DROP, FOXAPP_OP_MARKER_REMOVE, FOXAPP_OP_MARKER_CLEAR, FOXAPP_OP_MARKER_NOTE}) {
        CHECK(cascade::core::cmd::isKnownOp(op));
        CHECK(cascade::core::cmd::isAppOp(op));
    }
    std::printf("M7: all four ops are DENIED to a REMOTE session and to a session without the SETTINGS grant; nothing moved\n");
}

// M8 ----------------------------------------------------------------------------------------------------------------------------------------

void testTheFile() {
    freshFile();
    {
        Direct d(true);
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        passes(d.eng, 3);
        CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
        CHECK_EQ(drop(d.eng, s, 144.8e6).status(), FOXAPI_OK);
        CHECK_EQ(note(d.eng, s, 2, "the repeater").status(), FOXAPI_OK);
        CHECK(fileSays(d.eng, [](const cascade::core::FreqMarkers& m) { return m.size() == 2u && m.list()[1].note == "the repeater"; }));
        CHECK_EQ(remove(d.eng, s, 1).status(), FOXAPI_OK);
        CHECK(fileSays(d.eng, [](const cascade::core::FreqMarkers& m) { return m.size() == 1u; }));
    }
    {
        Direct d(true);
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        passes(d.eng, 3);
        const std::vector<long long> now = numbers(s);
        CHECK_EQ(now.size(), static_cast<std::size_t>(1));
        if (now.size() == 1u) { CHECK_EQ(now[0], 2); }
        CHECK_EQ(fact(s, "markers.note.2"), std::string("the repeater"));
        CHECK(std::fabs(item(s, FOXAPI_LIST_FREQ_MARKERS, 0).value[0] - 144.8e6) < 0.5);
        CHECK_EQ(static_cast<int>(drop(d.eng, s, 146.5e6).result.applied[0]), 3);   // M1 is gone, and the count was kept
    }
    std::printf("M8: a drop, a note and a remove reach markers.json on a worker, and the next engine starts from the list, the note and the numbering\n");
}

// M9 ----------------------------------------------------------------------------------------------------------------------------------------

void testDamagedFile() {
    freshFile();
    {
        std::ofstream f(markersFile(), std::ios::binary | std::ios::trunc);
        f << "this is not json {{{";
    }
    Direct d(true);
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_FREQ_MARKERS), 0);
    CHECK(!fact(s, "markers.error").empty());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    if (remote.ok()) {
        int32_t st = 0;
        fact(remote, "markers.error", &st);
        CHECK_EQ(st, FOXAPI_DENIED);
    }
    // The engine goes on: a drop works and the list is saved over a file that was kept aside.
    CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
    CHECK(fileSays(d.eng, [](const cascade::core::FreqMarkers& m) { return m.size() == 1u; }));
    std::printf("M9: a damaged markers.json is said in fact.markers.error (LOCAL only), the engine starts empty and goes on\n");
}

// M10 ---------------------------------------------------------------------------------------------------------------------------------------

void testSettled() {
    freshFile();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
    passes(d.eng, 5);
    const uint64_t listSeq = s.state().listSeq;
    for (int i = 0; i < 25; ++i) { d.eng.controlPass(); }
    CHECK_EQ(s.state().listSeq, listSeq);
    // An op that changes nothing moves nothing: a merging drop, an unknown remove, the same note.
    CHECK_EQ(drop(d.eng, s, 145.5e6).status(), FOXAPI_OK);
    CHECK_EQ(remove(d.eng, s, 9).result.status, FOXAPI_NOT_FOUND);
    CHECK_EQ(note(d.eng, s, 1, "").result.status, FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(s.state().listSeq, listSeq);
    std::printf("M10: a settled engine's listSeq does not move, and an op that changes nothing does not move it\n");
}

}  // namespace

int main() {
    std::printf("test_engine_api_markers\n");
    isolate();
    testListAndFacts();
    testDrop();
    testNumbersAreNames();
    testNote();
    testRemoveAndClear();
    testFull();
    testWhoMaySend();
    testTheFile();
    testDamagedFile();
    testSettled();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_markers");
}
