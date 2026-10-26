// test_engine_api_airband.cpp - THE AIRBAND MONITOR AND THE SCANNER'S LIST MODE in the engine (stub campaign B6, AIRBAND-A; 0.99.60 - 0.99.71),
// held against the real engine through the C table and the engine's own passes - the way test_engine_api_store holds the store's half - with no
// window anywhere. The receiver is the built-in signal generator (two tones: 300 kHz above the centre and 500 kHz below it, over a noise floor), which
// is exactly what the real binary's test_airband_app listens to; the DSP threads, the patch runner and the squelch are the product's own.
//
// One group per rule the batch carries; each was shown red (against an engine whose ops answered UNSUPPORTED, before the engine served them) and then
// against a mutant of the code that holds it (docs/engine-stage4.md 6.22, B6):
//   A1  THE TICK: BOOKMARK_TICK sets and clears a row's tick (BOOKMARKS' TICKED flag), by id, leaves the row where it is among its peers on one
//       frequency, changes and writes nothing when the row is as asked, refuses an unknown id, and is a LOCAL session's;
//   A2  ROWS TYPED IN: BOOKMARK_ADD_ROWS adds a typed row (named by its frequency when nameless, a name in use gets " (2)"), a list as an import
//       does (a duplicate skipped), refuses text with no row in it, and judges a typed row against the open radio's range - a row outside it is
//       OUT_OF_RANGE with the sentence that says what the radio covers, and nothing is added;
//   B1  LISTEN AND STOP: LISTEN plays the ticked AM rows - one block, one centre, the channel table, the block's line; the tone 300 kHz above the
//       centre opens the squelch of the 120.300 channel and not the 119.700 one; STOP puts it all away; nothing ticked, or only a WFM row, is a
//       refusal that says so;
//   B2  THE BLOCK PLAN: channels too far apart for one capture are cut into blocks and the second scanner walks them (and NEXT BLOCK moves on);
//   B3  ONE CHANNEL PER FREQUENCY, AND A WFM ROW IS NOT PLAYED;
//   C1  THE HAND-BACK: LISTEN with the patch view showing closes it and waits for the patch to let go before it starts - the pass that took the request
//       is not listening, a patch that is RUNNING is stopped first - and the patch view showing again stops the monitor (the two never share the
//       receiver's one runner);
//   C1b THE HAND-BACK OF A LENT RADIO: a running patch was given the receiver's live radio (the receiver runs on the generator meanwhile) and LISTEN
//       waits for the radio to be handed back by a reopen - the monitor is never listening while the receiver is still on the generator;
//   C2  THE OTHER RULES THAT STOP IT: the receiver stopped, the receiver retuned by hand, a scan started (and a LISTEN stops a scan), the preset
//       removed, TUNE ONE; and what leaves its choice alone: a row added to another preset;
//   D1  THE SCANNER'S LIST MODE: the ticked rows in frequency order, each tuned with its own mode and width, the scan following the ticks, a start
//       with nothing ticked refused, a range scan leaving list mode;
//   E1  THE HEARD TIME: the seconds a channel's squelch is open are credited to ITS row - the first ticked row on the frequency - and to no other,
//       accumulate across sessions, and reach the list the engine saves;
//   F1  THE NFM DE-EMPHASIS AND THE LEVELS: an NFM channel is built with the 50 us network and the level that matches an AM one; an AM channel with
//       neither; the squelch op rebuilds the block with its value;
//   G1  THE OPS' EDGES: every action's refusals, the grants, a REMOTE session.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
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
#include "core/airband_monitor.hpp"
#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/freq_import.hpp"
#include "core/freq_manager.hpp"
#include "core/patch_devices.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;
namespace pc = cascade::core::patch;
namespace cc = cascade::core;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is (tests/test_engine_api_03.cpp and test_engine_api_store.cpp have the same one).
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installFakeRadio() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static void removeFakeRadio() {
        Engine::testHooks_.makeDevice = nullptr;
        Engine::testHooks_.nativeScan = nullptr;
        Engine::testHooks_.soapyScan = nullptr;
    }
    static void seed(Engine& e, std::vector<cascade::core::Bookmark> rows) { e.freqMgr_.addMany(std::move(rows)); }
    static std::vector<Engine::AirbandBuilt> built(Engine& e) { return e.airbandBuilt_; }
    static bool patchPageOpen(Engine& e) { return e.patchPageOpen_; }
    static void setBookmarkPath(Engine& e, const std::string& p) { e.bookmarkPath_ = p; }
    static const cascade::core::FreqManager& list(Engine& e) { return e.freqMgr_; }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
constexpr std::uint64_t kLocalGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;

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

// The process's own scratch tree for every place the engine could write (a hermetic engine keeps its lists out of the user's folders anyway).
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_airband_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

// ---- the table -------------------------------------------------------------------------------------------------------------------------

std::string fact(testing::Session& s, const std::string& key) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

// The entry of a list whose name is `name`: a zeroed item (id 0) when there is none.
FoxListItem named(testing::Session& s, uint32_t list, const std::string& name) {
    const int32_t n = count(s, list);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, list, static_cast<uint32_t>(i));
        if (name == it.name) { return it; }
    }
    return FoxListItem{};
}

FoxListItem row(testing::Session& s, const std::string& name) { return named(s, FOXAPI_LIST_BOOKMARKS, name); }
FoxListItem chan(testing::Session& s, const std::string& name) { return named(s, FOXAPI_LIST_AIRBAND_CHANNELS, name); }
bool ticked(testing::Session& s, const std::string& name) { return (row(s, name).flags & FOXAPI_ITEM_TICKED) != 0u; }
bool open(testing::Session& s, const std::string& name) { return (chan(s, name).flags & FOXAPI_ITEM_ACTIVE) != 0u; }
double heard(testing::Session& s, const std::string& name) { return row(s, name).value[3]; }
bool listening(testing::Session& s) { return fact(s, "airband.listening") == "1"; }

template <class F>
bool passUntil(Engine& eng, F done, int timeoutMs = 8000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void passes(Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

FoxCommand op(std::uint32_t code, long long i0 = 0, long long i1 = 0, double n0 = 0.0, const char* text = nullptr) {
    FoxCommand c = testing::cmd(code, n0, i0, text);
    c.ival[1] = i1;
    return c;
}

// Submits ONE command (its text beside it when `text` is long) and passes until its result lands.
testing::Outcome pumpedAs(Engine& eng, FoxSession* session, const FoxCommand& c, const std::string* longText = nullptr) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = longText != nullptr
                          ? api()->submit_text(session, &c, longText->data(), static_cast<std::uint32_t>(longText->size()), &sr)
                          : api()->submit(session, &c, 1, &sr);
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
testing::Outcome pumped(Engine& eng, testing::Session& s, const FoxCommand& c) { return pumpedAs(eng, s.raw(), c); }
testing::Outcome pumpedText(Engine& eng, testing::Session& s, const FoxCommand& c, const std::string& text) { return pumpedAs(eng, s.raw(), c, &text); }

struct Direct {
    Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

// ---- the commands, spelled once ---------------------------------------------------------------------------------------------------------
FoxCommand tickOp(std::uint64_t id, int on) { return op(FOXAPP_OP_BOOKMARK_TICK, static_cast<long long>(id), on); }
FoxCommand listenOp(const std::string& preset) { return op(FOXAPP_OP_AIRBAND, 1, 0, 0.0, preset.c_str()); }
FoxCommand stopOp() { return op(FOXAPP_OP_AIRBAND, 0); }
FoxCommand pageOp(int on) { return op(FOXAPP_OP_SET_PATCH_PAGE_OPEN, on); }

cc::Bookmark bm(const char* name, double hz, const char* mode, double bw, bool tick, const char* group = "") {
    cc::Bookmark b;
    b.name = name;
    b.freqHz = hz;
    b.mode = mode;
    b.bandwidthHz = bw;
    b.scan = tick;
    b.group = group;
    return b;
}

// The two rows 600 kHz apart the real binary's test_airband_app listens to: the generator's tone is 300 kHz above the centre they share (120.000),
// so 120.300 hears it and 119.700 does not.
std::vector<cc::Bookmark> twoRows() {
    return {bm("Low", 119.7e6, "AM", 10000.0, true), bm("High", 120.3e6, "AM", 10000.0, true), bm("Unticked", 121.5e6, "AM", 10000.0, false)};
}

// The page is closed and the receiver is running before the test starts to listen, so a test about the monitor is not also one about the hand-back.
void receiverView(Direct& d, testing::Session& s) {
    CHECK_EQ(pumped(d.eng, s, pageOp(0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
}

// ---- A1: the tick ------------------------------------------------------------------------------------------------------------------------

void testTick() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // "HighNfm" shares 120.300 with "High": the monitor plays the FIRST of them, and a tick must not reorder them.
    Access::seed(d.eng, {bm("A", 119.7e6, "AM", 10000.0, false), bm("High", 120.3e6, "AM", 10000.0, true),
                         bm("HighNfm", 120.3e6, "NFM", 12500.0, false)});
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), 3);
    CHECK(!ticked(s, "A"));
    CHECK(ticked(s, "High"));
    CHECK_EQ(fact(s, "scanner.ticked"), std::string("1"));

    const std::uint64_t idA = row(s, "A").id;
    CHECK(idA != 0u);
    const std::string v0 = fact(s, "bookmarks.version");
    CHECK_EQ(pumped(d.eng, s, tickOp(idA, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(ticked(s, "A"));
    CHECK_EQ(fact(s, "scanner.ticked"), std::string("2"));
    CHECK(fact(s, "bookmarks.version") != v0);
    // ASKED FOR WHAT IT ALREADY IS: nothing changes and nothing is written (the store's version does not move).
    const std::string v1 = fact(s, "bookmarks.version");
    CHECK_EQ(pumped(d.eng, s, tickOp(idA, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "bookmarks.version"), v1);
    // Unticked again.
    CHECK_EQ(pumped(d.eng, s, tickOp(idA, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!ticked(s, "A"));
    CHECK_EQ(fact(s, "scanner.ticked"), std::string("1"));

    // THE TICK KEEPS THE ROW WHERE IT IS: a peer on the same frequency is ticked, and it stays behind the first one (the monitor plays the first).
    const std::uint64_t idNfm = row(s, "HighNfm").id;
    CHECK_EQ(pumped(d.eng, s, tickOp(idNfm, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(ticked(s, "HighNfm"));
    const int32_t n = count(s, FOXAPI_LIST_BOOKMARKS);
    CHECK_EQ(n, 3);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_BOOKMARKS, 1).name), std::string("High"));
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_BOOKMARKS, 2).name), std::string("HighNfm"));

    // An id the list does not hold; and only a LOCAL session's.
    CHECK_EQ(pumped(d.eng, s, tickOp(987654321u, 1)).status(), FOXAPI_NOT_FOUND);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, kLocalGrants, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(pumped(d.eng, remote, tickOp(idA, 1)).status(), FOXAPI_DENIED);
    passes(d.eng, 2);
    CHECK(!ticked(s, "A"));
    std::printf("A1: ticked, unticked, repeated (nothing written), a peer on one frequency ticked in place, unknown id and a REMOTE session refused\n");
}

// ---- A2: rows typed in -------------------------------------------------------------------------------------------------------------------

std::string rowLine(double hz, const char* mode, double bw, int tick, const char* group, const char* name) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%.0f\t%s\t%.0f\t%d\t%s\t%s", hz, mode, bw, tick, group, name);
    return buf;
}

void testAddRows() {
  {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), 0);

    // A ROW TYPED IN: ticked, in its preset, named by its frequency when nameless.
    testing::Outcome r = pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(121.5e6, "NFM", 12500.0, 1, "Manual", ""));
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 1.0);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), 1);
    const FoxListItem typed = row(s, "121.5000 MHz");
    CHECK(typed.id != 0u);
    CHECK((typed.flags & FOXAPI_ITEM_TICKED) != 0u);
    CHECK_EQ(std::string(typed.detail), std::string("Manual"));
    CHECK_EQ(typed.value[0], 121.5e6);
    CHECK_EQ(typed.value[1], static_cast<double>(FOXAPI_DEMOD_NFM));
    CHECK_EQ(typed.value[2], 12500.0);
    // A name in use gets " (2)", as the list's own add does; a name typed is kept.
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(121.5e6, "NFM", 12500.0, 1, "Manual", "121.5000 MHz")).status(), FOXAPI_OK);
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(122.8e6, "AM", 10000.0, 1, "Manual", "Unicom")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), 3);
    CHECK(row(s, "121.5000 MHz (2)").id != 0u);
    CHECK(row(s, "Unicom").id != 0u);

    // A LIST (an airport's rows): as an import adds - a row identical in name, frequency and group to one already there is skipped.
    const std::string list = rowLine(118.1e6, "AM", 10000.0, 1, "KABC", "KABC TWR") + "\n" + rowLine(121.5e6, "NFM", 12500.0, 0, "Manual", "121.5000 MHz") +
                             "\n" + rowLine(119.0e6, "AM", 10000.0, 0, "KABC", "KABC ATIS");
    r = pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 1), list);
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 2.0);   // the middle line is the row typed first, again
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), 5);
    CHECK(ticked(s, "KABC TWR"));
    CHECK(!ticked(s, "KABC ATIS"));   // the tick the line gave it
    // Importing it again adds nothing.
    r = pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 1), list);
    CHECK_EQ(r.status(), FOXAPI_OK);
    CHECK_EQ(r.result.applied[0], 0.0);

    // TEXT WITH NO ROW IN IT, a frequency that is no number, a negative one, a bad ival: nothing is added.
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), std::string("nonsense")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(-1.0, "AM", 10000.0, 1, "", "x")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), std::string("abc\tAM\t10000\t1\t\tx")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 7), rowLine(121.5e6, "AM", 1.0, 1, "", "x")).status(), FOXAPI_OUT_OF_RANGE);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), 5);
    // A mode the monitor does not play is kept as a row all the same (the list is the user's); a width of 0 takes the mode's default.
    r = pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(99.5e6, "WFM", 0.0, 0, "Manual", "Radio"));
    CHECK_EQ(r.status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(row(s, "Radio").value[2], cc::defaultBandwidthForMode("WFM"));
    std::printf("A2: a typed row named by its frequency, a name in use renumbered, a list added as an import adds, bad text refused\n");
  }

    // THE RADIO'S RANGE: a typed row outside what the open radio covers is refused with the sentence that says what it covers, and nothing is
    // added; the same row goes in under the generator, which publishes no range.
    Access::installFakeRadio();
    Direct f;
    testing::Session fk(api(), f.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(fk.ok());
    CHECK_EQ(pumped(f.eng, fk, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    const auto fakeId = [&] {
        for (int32_t i = 0; i < count(fk, FOXAPI_LIST_DEVICES); ++i) {
            const FoxListItem it = item(fk, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i));
            if (std::string(it.detail) == engine_api_test::kFakeRadioId) { return true; }
        }
        return false;
    };
    CHECK(passUntil(f.eng, fakeId));
    CHECK_EQ(pumped(f.eng, fk, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
    CHECK(passUntil(f.eng, [&] { return (fk.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
    const testing::Outcome r = pumpedText(f.eng, fk, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(1.8e9, "AM", 10000.0, 1, "Manual", "Too high"));
    std::printf("A2: outside the radio's range: status %d, \"%s\"\n", r.status(), r.result.message);
    CHECK_EQ(r.status(), FOXAPI_OUT_OF_RANGE);
    CHECK(std::string(r.result.message).find("cannot tune") != std::string::npos);
    CHECK(std::string(r.result.message).find("1.766 GHz") != std::string::npos);   // the range it does cover
    passes(f.eng, 2);
    CHECK_EQ(count(fk, FOXAPI_LIST_BOOKMARKS), 0);
    // A row inside it is added; a LIST is not judged (an airport's table may hold what this radio cannot reach, and the rows are still the list's).
    CHECK_EQ(pumpedText(f.eng, fk, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0), rowLine(121.5e6, "AM", 10000.0, 1, "Manual", "In range")).status(), FOXAPI_OK);
    CHECK_EQ(pumpedText(f.eng, fk, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 1), rowLine(1.8e9, "AM", 10000.0, 1, "KABC", "Far")).status(), FOXAPI_OK);
    passes(f.eng, 2);
    CHECK_EQ(count(fk, FOXAPI_LIST_BOOKMARKS), 2);
    Access::removeFakeRadio();
}

// ---- B1: listen and stop -----------------------------------------------------------------------------------------------------------------

void testListenAndStop() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::seed(d.eng, twoRows());
    // (An unticked WFM neighbour and a ticked one: neither is played.)
    Access::seed(d.eng, {bm("Broadcast", 121.8e6, "WFM", 150000.0, true)});
    passes(d.eng, 2);
    receiverView(d, s);

    CHECK(!listening(s));
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 0);
    const testing::Outcome l = pumped(d.eng, s, listenOp(""));
    CHECK_EQ(l.status(), FOXAPI_OK);
    CHECK_EQ(l.result.applied[0], 1.0);   // listening at once: the page was closed and the receiver was running
    passes(d.eng, 2);
    CHECK(listening(s));
    CHECK((s.state().flags & FOXAPI_RX_RUNNING) != 0u);
    // THE PLAN: one block holds both, the receiver sits on the centre between them (nearest the block's middle that keeps both off the DC
    // spike), and the table lists both channels - not the WFM row - in frequency order, both in the block on the air.
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 2);
    CHECK(passUntil(d.eng, [&] { return s.state().centreHz == 120.0e6; }));
    const FoxListItem low = item(s, FOXAPI_LIST_AIRBAND_CHANNELS, 0);
    const FoxListItem high = item(s, FOXAPI_LIST_AIRBAND_CHANNELS, 1);
    CHECK_EQ(std::string(low.name), std::string("Low"));
    CHECK_EQ(std::string(high.name), std::string("High"));
    CHECK_EQ(low.value[0], 119.7e6);
    CHECK_EQ(high.value[0], 120.3e6);
    CHECK_EQ(high.value[1], 10000.0);
    CHECK((low.flags & FOXAPI_ITEM_ON_AIR) != 0u);
    CHECK((high.flags & FOXAPI_ITEM_ON_AIR) != 0u);
    CHECK_EQ(low.id, 1u);
    CHECK_EQ(high.id, 2u);
    CHECK_EQ(fact(s, "airband.block"), std::string("All 2 channels at once, 119.700-120.300 MHz"));
    CHECK_EQ(fact(s, "airband.blocks"), std::string("1"));
    CHECK_EQ(fact(s, "airband.note"), std::string());
    // THE SQUELCH: the generator's tone, 300 kHz above the centre, opens 120.300's squelch - and only that one's.
    const bool opened = passUntil(d.eng, [&] { return open(s, "High"); });
    std::printf("B1: the tone 300 kHz above the centre opened High's squelch: %s; Low %s; High's level %.1f dB, Low's %.1f dB\n", opened ? "yes" : "NO",
                open(s, "Low") ? "open" : "shut", chan(s, "High").value[2], chan(s, "Low").value[2]);
    CHECK(opened);
    CHECK(!open(s, "Low"));
    CHECK(chan(s, "High").value[2] > chan(s, "Low").value[2]);

    // NEXT BLOCK with one block has nowhere to go.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 2)).status(), FOXAPI_NO_CHANGE);
    // LISTEN again on the same preset is already so.
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    CHECK(listening(s));

    // STOP puts it all away: the table is empty, the note is empty, and a second STOP has nothing to stop.
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 0);
    CHECK_EQ(fact(s, "airband.block"), std::string());
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_NO_CHANGE);

    // NOTHING TO PLAY: nothing ticked in the preset, and a preset with only a WFM row, are the refusal that says so.
    testing::Outcome none = pumped(d.eng, s, listenOp("No such preset"));
    CHECK_EQ(none.status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(std::string(none.result.message), std::string("Tick at least one AM or NFM frequency to listen to."));
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string("Tick at least one AM or NFM frequency to listen to."));
    CHECK_EQ(pumped(d.eng, s, tickOp(row(s, "Low").id, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, tickOp(row(s, "High").id, 0)).status(), FOXAPI_OK);
    none = pumped(d.eng, s, listenOp(""));   // only the ticked WFM row is left ticked
    CHECK_EQ(none.status(), FOXAPI_NO_CHANGE);
    CHECK(!listening(s));
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// ---- B2: the block plan ------------------------------------------------------------------------------------------------------------------

void testBlockPlan() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // 118.1 and 125.0 are 6.9 MHz apart: no 2.4 MS/s capture holds both (90% of it is 2.16 MHz), so they are two blocks and the second scanner
    // walks their centres.
    Access::seed(d.eng, {bm("Tower", 118.1e6, "AM", 10000.0, true), bm("Approach", 125.0e6, "AM", 10000.0, true)});
    passes(d.eng, 2);
    receiverView(d, s);
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(listening(s));
    CHECK_EQ(fact(s, "airband.blocks"), std::string("2"));
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 2);
    CHECK_EQ(fact(s, "airband.block"), std::string("Block 1 of 2: 1 channels, 118.100-118.100 MHz"));
    // The scanner's first tick tunes block 0; a block nobody talks in is left after the dwell for the next.
    CHECK(passUntil(d.eng, [&] { return fact(s, "airband.block").rfind("Block 2 of 2", 0) == 0; }, 10000));
    const double centre = s.state().centreHz;
    std::printf("B2: block 2 of 2 on the air, centre %.3f MHz, line \"%s\"\n", centre / 1e6, fact(s, "airband.block").c_str());
    CHECK(std::fabs(centre - 125.0e6) < 10000.0);
    CHECK_EQ(fact(s, "airband.block"), std::string("Block 2 of 2: 1 channels, 125.000-125.000 MHz"));
    CHECK((chan(s, "Approach").flags & FOXAPI_ITEM_ON_AIR) != 0u);
    CHECK((chan(s, "Tower").flags & FOXAPI_ITEM_ON_AIR) == 0u);   // the plan lists both; the block on the air is the one
    // NEXT BLOCK moves on at once, and wraps.
    const std::string before = fact(s, "airband.block");
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 2)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "airband.block") != before; }, 6000));
    CHECK(listening(s));
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// ---- B3: one channel per frequency, and a WFM row is not played -------------------------------------------------------------------------------

void testOneChannelPerFrequency() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // "High" (AM) and "HighNfm" (NFM) are ticked on ONE frequency, "High" first in the list; the WFM row is ticked and not played.
    Access::seed(d.eng, {bm("Low", 119.7e6, "AM", 10000.0, true), bm("High", 120.3e6, "AM", 10000.0, true),
                         bm("HighNfm", 120.3e6, "NFM", 12500.0, true), bm("Broadcast", 121.8e6, "WFM", 150000.0, true)});
    passes(d.eng, 2);
    receiverView(d, s);
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(listening(s));
    // ONE channel on 120.300, named by the first row, in the first row's mode and width.
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 2);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_AIRBAND_CHANNELS, 1).name), std::string("High"));
    CHECK_EQ(item(s, FOXAPI_LIST_AIRBAND_CHANNELS, 1).value[1], 10000.0);
    CHECK_EQ(chan(s, "HighNfm").id, 0u);
    CHECK_EQ(chan(s, "Broadcast").id, 0u);
    const auto built = Access::built(d.eng);
    CHECK_EQ(built.size(), 2u);
    for (const auto& b : built) { CHECK(b.freqHz != 121.8e6); }
    if (built.size() == 2u) { CHECK(!built[1].nfm); }   // the first row's mode: AM
    CHECK_EQ(fact(s, "airband.block"), std::string("All 2 channels at once, 119.700-120.300 MHz"));
    std::printf("B3: two ticked rows on one frequency are one channel (\"%s\"); the ticked WFM row is not on the air\n",
                item(s, FOXAPI_LIST_AIRBAND_CHANNELS, 1).name);

    // ONLY WFM ROWS TICKED: nothing to play, whatever the list says.
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    for (const char* n : {"Low", "High", "HighNfm"}) { CHECK_EQ(pumped(d.eng, s, tickOp(row(s, n).id, 0)).status(), FOXAPI_OK); }
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_NO_CHANGE);
    CHECK(!listening(s));
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// ---- C1: the hand-back -------------------------------------------------------------------------------------------------------------------

// A radio on the generator, a channel above its centre and a spectrum display on the radio, wired: a patch that can run on a radio of its own.
pc::Graph runnablePatch() {
    pc::Graph g;
    const pc::NodeId radio = g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 40.0f, 40.0f);
    const pc::NodeId chn = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
    const pc::NodeId demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 320.0f);
    if (pc::Node* n = g.mutableNode(radio)) {
        n->device = pc::kGeneratorKey;
        n->freqHz = 145.0e6;
        n->centreChosen = true;
        n->rateHz = 2.4e6;
    }
    if (pc::Node* n = g.mutableNode(chn)) { n->freqHz = 145.05e6; }
    CHECK(g.connect(radio, 0, chn, 0) == pc::Connect::Ok);
    CHECK(g.connect(chn, 0, demod, 0) == pc::Connect::Ok);
    const pc::NodeId display = g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 520.0f);
    CHECK(g.connect(radio, 0, display, 0) == pc::Connect::Ok);
    return g;
}

void testHandBack() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::seed(d.eng, twoRows());
    passes(d.eng, 2);
    CHECK(Access::patchPageOpen(d.eng));   // the patch view is the window's default face: an engine nobody has told is showing it

    // LISTEN WITH THE PATCH VIEW SHOWING: the engine switches to the receiver view (the patch view closes), says it is waiting for the patch to
    // let go, and the monitor starts on a later pass - not in the one that took the request.
    testing::Outcome l = pumped(d.eng, s, listenOp(""));
    CHECK_EQ(l.status(), FOXAPI_OK);
    CHECK_EQ(l.result.applied[0], 2.0);
    CHECK(!Access::patchPageOpen(d.eng));
    // NOT YET: the pass that took the request closed the patch view and the patch has not let go - the monitor is waiting, not listening.
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.pending"), std::string("1"));
    CHECK(passUntil(d.eng, [&] { return listening(s); }));
    CHECK_EQ(fact(s, "airband.pending"), std::string("0"));
    CHECK(passUntil(d.eng, [&] { return open(s, "High"); }));   // it is on the air, not merely said to be
    std::printf("B/C1: LISTEN with the patch view showing: waiting, then listening, High's squelch open: %s\n", open(s, "High") ? "yes" : "NO");

    // THE PATCH VIEW SHOWING AGAIN stops the monitor (the two never share the receiver's runner), and says why.
    CHECK_EQ(pumped(d.eng, s, pageOp(1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string("Stopped: the patch view took the radio."));
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 0);

    // A LISTEN THAT IS STILL WAITING is cancelled by the view coming back, the user's hand winning over the wish.
    CHECK_EQ(pumped(d.eng, s, listenOp("")).result.applied[0], 2.0);
    CHECK_EQ(pumped(d.eng, s, pageOp(1)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.pending"), std::string("0"));
    CHECK_EQ(fact(s, "airband.note"), std::string("Stopped: the patch view took the radio."));
    CHECK(Access::patchPageOpen(d.eng));

    // A PATCH THAT IS RUNNING IS HANDED BACK: START it on its own radio, then LISTEN - the patch stops, its radio closes, and the monitor plays.
    const pc::Graph g = runnablePatch();
    const std::string doc = pc::graphCommandText(g);
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_PATCH_GRAPH_DOCUMENT), doc).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_PATCH_RADIOS) == 1 && (s.state().flags & FOXAPI_RX_PATCH_RUNNING) != 0u; }));
    l = pumped(d.eng, s, listenOp(""));
    CHECK_EQ(l.status(), FOXAPI_OK);
    CHECK_EQ(l.result.applied[0], 2.0);
    CHECK(passUntil(d.eng, [&] { return listening(s); }));
    CHECK((s.state().flags & FOXAPI_RX_PATCH_RUNNING) == 0u);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);
    CHECK(passUntil(d.eng, [&] { return open(s, "High"); }));
    CHECK(!Access::patchPageOpen(d.eng));
    std::printf("C1: a running patch handed back: patch stopped, its radio closed, the monitor listening and High open\n");
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// THE RECEIVER'S OWN RADIO, LENT TO A RUNNING PATCH: a patch that runs takes the receiver's live radio (the Radio node whose device is empty is given it) and
// the receiver runs on the signal generator until the patch stops; stopping hands the radio back by REOPENING it on a worker. LISTEN with such a patch showing
// must wait for that: a monitor that started the moment the view closed would be cutting its blocks for the generator, on the generator's rate and centre, and
// then be thrown off it by the reopen.
void testHandBackOfALentRadio() {
    Access::installFakeRadio();
    {
        Direct d;
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
        CHECK(s.ok());
        Access::seed(d.eng, twoRows());
        passes(d.eng, 2);
        // The receiver on the (fake) radio.
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
        const auto fakeListed = [&] {
            for (int32_t i = 0; i < count(s, FOXAPI_LIST_DEVICES); ++i) {
                if (std::string(item(s, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i)).detail) == engine_api_test::kFakeRadioId) { return true; }
            }
            return false;
        };
        CHECK(passUntil(d.eng, fakeListed));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
        const std::string radioName = s.state().deviceName;
        CHECK(radioName != std::string("Signal generator"));

        // A patch whose Radio has no device of its own, so it is given the receiver's: a channel above the centre it is carried, a spectrum on the radio.
        pc::Graph g;
        const pc::NodeId radio = g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 40.0f, 40.0f);
        const pc::NodeId chn = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId display = g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 520.0f);
        if (pc::Node* n = g.mutableNode(chn)) { n->freqHz = 100.05e6; }
        CHECK(g.connect(radio, 0, chn, 0) == pc::Connect::Ok);
        CHECK(g.connect(radio, 0, display, 0) == pc::Connect::Ok);
        CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_PATCH_GRAPH_DOCUMENT), pc::graphCommandText(g)).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_PATCH_RUNNING) != 0u && count(s, FOXAPI_LIST_PATCH_RADIOS) == 1; }));
        // The radio is the patch's; the receiver is on the generator.
        CHECK_EQ(std::string(s.state().deviceName), std::string("Signal generator"));
        std::printf("C1b: the patch runs on the receiver's lent radio (%s); the receiver is on the generator\n", radioName.c_str());

        // LISTEN: the patch view closes, the patch stops, the radio is handed back by a reopen - and only then does the monitor start. It is never
        // listening while the receiver is still on the generator.
        const testing::Outcome l = pumped(d.eng, s, listenOp(""));
        CHECK_EQ(l.status(), FOXAPI_OK);
        CHECK_EQ(l.result.applied[0], 2.0);
        // NOT YET: this pass closed the patch view and stopped the patch, and the radio is on its way back by a reopen. The receiver is still on the
        // generator, and a monitor that is already listening is listening there.
        CHECK(!listening(s));
        CHECK_EQ(fact(s, "airband.pending"), std::string("1"));
        bool listeningOnTheGenerator = false;
        int passesWaiting = 0;
        const bool started = passUntil(d.eng, [&] {
            if (!listening(s)) {
                ++passesWaiting;
                return false;
            }
            if (std::string(s.state().deviceName) == "Signal generator") { listeningOnTheGenerator = true; }
            return true;
        }, 12000);
        CHECK(started);
        std::printf("C1b: listening after %d pass(es) of waiting; on the generator at any point: %s; the receiver is on \"%s\"\n", passesWaiting,
                    listeningOnTheGenerator ? "YES" : "no", s.state().deviceName);
        CHECK(!listeningOnTheGenerator);
        CHECK_EQ(std::string(s.state().deviceName), radioName);
        CHECK((s.state().flags & FOXAPI_RX_PATCH_RUNNING) == 0u);
        CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);
        CHECK(passUntil(d.eng, [&] { return s.state().centreHz == 120.0e6; }));
        CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 2);
        CHECK_EQ(fact(s, "airband.block"), std::string("All 2 channels at once, 119.700-120.300 MHz"));
        CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
        s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
    }
    Access::removeFakeRadio();
}

// ---- C2: the other rules that stop it ---------------------------------------------------------------------------------------------------------

void testStopRules() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::seed(d.eng, twoRows());
    passes(d.eng, 2);
    receiverView(d, s);
    const auto start = [&](const char* preset) {
        CHECK_EQ(pumped(d.eng, s, listenOp(preset)).status(), FOXAPI_OK);
        passes(d.eng, 2);
        CHECK(listening(s));
    };

    // THE RECEIVER WAS RETUNED BY HAND: a centre the monitor did not tune to is the user's hand on the dial.
    start("");
    CHECK(passUntil(d.eng, [&] { return s.state().centreHz != 0.0 && s.state().centreHz != 100.0e6; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_CENTRE, 100.5e6)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string("Stopped: the receiver was retuned."));

    // THE RECEIVER STOPPED.
    start("");
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string("Stopped: the receiver stopped."));
    // ...and LISTEN starts the receiver itself.
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(listening(s));
    CHECK((s.state().flags & FOXAPI_RX_RUNNING) != 0u);

    // A SCAN STARTED stands the monitor down (one radio, one plan), without a note; and a LISTEN stops a scan.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 2)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string());
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(listening(s));
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    passes(d.eng, 2);
    // A range scan, from a browser or the desk, does the same.
    start("");
    FoxCommand rangeRun = testing::cmd(FOXAPI_OP_SCANNER_RUN, 88.0e6, 1);
    rangeRun.num[1] = 108.0e6;
    rangeRun.num[2] = 100.0e3;
    CHECK_EQ(pumped(d.eng, s, rangeRun).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!listening(s));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCANNER_RUN, 0, 0)).status(), FOXAPI_OK);

    // THE PRESET WAS REMOVED while the monitor plays it: it stops, saying why, and does not start again on "every ticked row" of the rest.
    Access::seed(d.eng, {bm("Marine", 156.8e6, "NFM", 12500.0, true, "Harbour")});
    passes(d.eng, 2);
    start("Harbour");
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 1);
    CHECK_EQ(fact(s, "airband.preset"), std::string("Harbour"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, 0, 0, "Harbour")).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string("Stopped: the preset was removed."));
    CHECK_EQ(fact(s, "airband.preset"), std::string());
    CHECK_EQ(row(s, "Marine").id, 0u);
    std::printf("C2: retuned, stopped, a scan started, a scan stopped by LISTEN, the preset removed: every one stopped the monitor with its words\n");

    // WHAT LEAVES ITS CHOICE ALONE: a row added to ANOTHER preset (an airport looked up while listening) does not narrow, widen or restart it.
    Access::seed(d.eng, {bm("Marine", 156.8e6, "NFM", 12500.0, true, "Harbour")});
    passes(d.eng, 2);
    start("Harbour");
    CHECK(chan(s, "Marine").id != 0u);
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 1), rowLine(118.1e6, "AM", 10000.0, 1, "KABC", "KABC TWR")).status(), FOXAPI_OK);
    passes(d.eng, 4);
    CHECK(listening(s));
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 1);
    CHECK_EQ(fact(s, "airband.preset"), std::string("Harbour"));
    // A row added to the preset it IS playing joins it: the blocks are cut again, the monitor still on the air.
    CHECK_EQ(pumpedText(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 1), rowLine(156.6e6, "NFM", 12500.0, 1, "Harbour", "Ch 12")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return listening(s) && count(s, FOXAPI_LIST_AIRBAND_CHANNELS) == 2; }));
    // A change of preset is a restart on the new one.
    CHECK_EQ(pumped(d.eng, s, listenOp("KABC")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(listening(s));
    CHECK_EQ(count(s, FOXAPI_LIST_AIRBAND_CHANNELS), 1);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_AIRBAND_CHANNELS, 0).name), std::string("KABC TWR"));

    // TUNE ONE: a click on a row of the section tunes the receiver to that row alone and the monitor stops, saying so.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 6, static_cast<long long>(row(s, "Marine").id))).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(!listening(s));
    CHECK_EQ(fact(s, "airband.note"), std::string("Tuned to Marine alone."));
    // (a tune the monitor made a moment ago is paced: the receiver lands on it a pass or two later)
    CHECK(passUntil(d.eng, [&] { return std::fabs(s.state().tunedHz - 156.8e6) < 1.0; }, 4000));
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 6, 987654321)).status(), FOXAPI_NOT_FOUND);
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// ---- D1: the scanner's list mode ---------------------------------------------------------------------------------------------------------

void testListMode() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::seed(d.eng, {bm("Air", 119.7e6, "AM", 10000.0, true), bm("Marine", 156.8e6, "NFM", 12500.0, true),
                         bm("Radio", 99.5e6, "WFM", 150000.0, true), bm("Quiet", 121.5e6, "AM", 10000.0, false)});
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "scanner.listMode"), std::string("0"));
    CHECK_EQ(fact(s, "scanner.ticked"), std::string("3"));
    // Short timings, so the walk fits the test: 20 ms dwell, no hold, no resume.
    FoxCommand cfg = testing::cmd(FOXAPI_OP_SCANNER_CONFIG);
    cfg.num[0] = 20.0;
    cfg.num[1] = 0.0;
    cfg.num[2] = 0.0;
    cfg.num[3] = 0.0;
    CHECK_EQ(pumped(d.eng, s, cfg).status(), FOXAPI_OK);

    // LIST MODE ON stops nothing that is not running, and says so in a fact; a start walks the TICKED rows in frequency order, each tuned
    // with its own mode and width.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "scanner.listMode"), std::string("1"));
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 2)).status(), FOXAPI_OK);
    struct Stop {
        std::uint32_t mode;
        double bw;
    };
    std::map<long long, Stop> seen;       // tuned Hz -> what the receiver was set to there
    std::vector<long long> order;         // distinct stops, in the order met
    const auto sample = [&] {
        const FoxReceiverState st = s.state();
        const long long hz = std::llround(st.tunedHz);
        if (seen.count(hz) == 0u) {
            seen[hz] = {st.demodMode, st.bandwidthHz};
            order.push_back(hz);
        }
    };
    CHECK(passUntil(d.eng, [&] {
        sample();
        return order.size() >= 3u;
    }, 10000));
    std::printf("C/D1: the list walk met:");
    for (const long long hz : order) { std::printf(" %.3f", static_cast<double>(hz) / 1e6); }
    std::printf(" MHz\n");
    // The ticked rows, lowest first, each with its row's mode and width - and never the unticked one.
    CHECK(seen.count(99500000) != 0u);
    CHECK(seen.count(119700000) != 0u);
    CHECK(seen.count(156800000) != 0u);
    CHECK(seen.count(121500000) == 0u);
    if (seen.count(119700000) != 0u) { CHECK_EQ(seen[119700000].mode, FOXAPI_DEMOD_AM); }
    if (seen.count(156800000) != 0u) { CHECK_EQ(seen[156800000].mode, FOXAPI_DEMOD_NFM); }
    if (seen.count(99500000) != 0u) { CHECK_EQ(seen[99500000].mode, FOXAPI_DEMOD_WFM); }
    if (seen.count(119700000) != 0u) { CHECK_EQ(seen[119700000].bw, 10000.0); }
    if (seen.count(156800000) != 0u) { CHECK_EQ(seen[156800000].bw, 12500.0); }
    // ASCENDING, wrapping: the first three distinct stops are the three rows, lowest first (the scan began at the first row).
    CHECK_EQ(order.size() >= 3u ? order[0] : 0LL, 99500000LL);
    CHECK_EQ(order.size() >= 3u ? order[1] : 0LL, 119700000LL);
    CHECK_EQ(order.size() >= 3u ? order[2] : 0LL, 156800000LL);

    // THE SCAN FOLLOWS THE TICKS: a row unticked while it runs is never visited again.
    CHECK_EQ(pumped(d.eng, s, tickOp(row(s, "Marine").id, 0)).status(), FOXAPI_OK);
    passes(d.eng, 4);
    CHECK_EQ(fact(s, "scanner.ticked"), std::string("2"));
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
    bool marineAgain = false;
    (void)passUntil(d.eng, [&] {
        if (std::llround(s.state().tunedHz) == 156800000LL) { marineAgain = true; }
        return false;
    }, 700);
    CHECK(!marineAgain);
    // ...and a row ticked while it runs joins it.
    CHECK_EQ(pumped(d.eng, s, tickOp(row(s, "Quiet").id, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return std::llround(s.state().tunedHz) == 121500000LL; }, 6000));
    // NOTHING TICKED stops it.
    for (const char* n : {"Air", "Radio", "Quiet"}) { CHECK_EQ(pumped(d.eng, s, tickOp(row(s, n).id, 0)).status(), FOXAPI_OK); }
    passes(d.eng, 4);
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
    // ...and a start with nothing ticked is refused, saying so.
    const testing::Outcome refused = pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 2));
    CHECK_EQ(refused.status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(std::string(refused.result.message), std::string("no frequency is ticked"));

    // A RANGE SCAN LEAVES LIST MODE (the range is what the command carried); range mode by the op does too, and a running scan stops.
    CHECK_EQ(fact(s, "scanner.listMode"), std::string("1"));
    CHECK_EQ(pumped(d.eng, s, tickOp(row(s, "Air").id, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 2)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
    FoxCommand run = testing::cmd(FOXAPI_OP_SCANNER_RUN, 88.0e6, 1);
    run.num[1] = 108.0e6;
    run.num[2] = 100.0e3;
    CHECK_EQ(pumped(d.eng, s, run).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "scanner.listMode"), std::string("0"));
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK((s.state().flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);   // list mode chosen: the running range scan stops
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "scanner.listMode"), std::string("0"));
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_SCANNER_LIST, 9)).status(), FOXAPI_OUT_OF_RANGE);
}

// ---- E1: the heard time ------------------------------------------------------------------------------------------------------------------

void testHeardTime() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // "High" and "HighNfm" share 120.300: the channel is credited to the FIRST row, never to the second.
    Access::seed(d.eng, {bm("Low", 119.7e6, "AM", 10000.0, true), bm("High", 120.3e6, "AM", 10000.0, true),
                         bm("HighNfm", 120.3e6, "NFM", 12500.0, true)});
    const std::string path = (g_scratch / "heard-bookmarks.json").string();
    Access::setBookmarkPath(d.eng, path);
    passes(d.eng, 2);
    receiverView(d, s);

    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return open(s, "High"); }));
    // The figure moves LIVE, ahead of the list: the table's channel entry carries what has been heard and not yet written.
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(passUntil(d.eng, [&] { return chan(s, "High").value[3] >= 0.6; }, 12000));
    const double pending = chan(s, "High").value[3];
    CHECK_EQ(chan(s, "Low").value[3], 0.0);
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // STOP wrote it: the row carries it, and no other row does.
    const double h1 = heard(s, "High");
    std::printf("E1: heard %.2f s live, %.2f s written to High at STOP (%.2f s of wall clock since the squelch first opened); Low %.2f, HighNfm %.2f\n",
                pending, h1, wall, heard(s, "Low"), heard(s, "HighNfm"));
    CHECK(h1 >= 0.6);
    CHECK(h1 <= wall + 1.5);   // a second per pass at the very most, never more than the clock allows (plus the stretch before the first read)
    CHECK_EQ(heard(s, "Low"), 0.0);
    CHECK_EQ(heard(s, "HighNfm"), 0.0);
    CHECK_EQ(chan(s, "High").id, 0u);   // the table is empty again: nothing is "heard, not yet written"

    // IT ACCUMULATES: a second session adds to the first's.
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return chan(s, "High").value[3] >= 0.4; }, 12000));
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const double h2 = heard(s, "High");
    CHECK(h2 >= h1 + 0.4);
    CHECK_EQ(heard(s, "HighNfm"), 0.0);

    // THE LIST THE ENGINE SAVES CARRIES IT (written on a worker from a snapshot taken on the pass): read the file back with the list's own loader.
    CHECK(passUntil(d.eng, [&] {
        cc::FreqManager fm;
        std::string err;
        if (!fm.load(path, err) || fm.list().empty()) { return false; }
        for (const cc::Bookmark& b : fm.list()) {
            if (b.name == "High") { return b.heardSeconds >= h2 - 0.01; }
        }
        return false;
    }, 10000));
    cc::FreqManager saved;
    std::string err;
    CHECK(saved.load(path, err));
    for (const cc::Bookmark& b : saved.list()) {
        if (b.name == "High") { CHECK(b.heardSeconds >= h2 - 0.01); }
        if (b.name == "Low" || b.name == "HighNfm") { CHECK_EQ(b.heardSeconds, 0.0); }
    }
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// ---- F1: the NFM de-emphasis and the levels -----------------------------------------------------------------------------------------------

void testNfmAndSquelch() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    // "Low" AM, "High" NFM at the 12.5 kHz a row is added with: the NFM channel runs the receiver's own 50 us network and is levelled to the AM one.
    Access::seed(d.eng, {bm("Low", 119.7e6, "AM", 10000.0, true), bm("High", 120.3e6, "NFM", 12500.0, true)});
    passes(d.eng, 2);
    receiverView(d, s);
    CHECK_EQ(pumped(d.eng, s, listenOp("")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(listening(s));
    auto built = Access::built(d.eng);
    CHECK_EQ(built.size(), 2u);
    if (built.size() == 2u) {
        std::printf("F1: built: Low nfm=%d de-emphasis %.1f us level %.3f; High nfm=%d de-emphasis %.1f us level %.3f\n", built[0].nfm ? 1 : 0,
                    built[0].deemphasisUs, built[0].mixLevel, built[1].nfm ? 1 : 0, built[1].deemphasisUs, built[1].mixLevel);
        CHECK(!built[0].nfm);
        CHECK(built[1].nfm);
        CHECK_EQ(built[0].freqHz, 119.7e6);
        CHECK_EQ(built[1].freqHz, 120.3e6);
        // AN AM CHANNEL'S AUDIO IS AN ENVELOPE: nothing to undo, nothing run, and mixed as it is.
        CHECK(std::fabs(built[0].deemphasisUs) < 1e-6);
        CHECK_EQ(built[0].mixLevel, 1.0);
        // AN NFM CHANNEL runs the receiver's default 50 us network, and is mixed at the level that makes it as loud as an AM one (at the channel rate the receiver's rate gives).
        CHECK(std::fabs(built[1].deemphasisUs - cc::kNfmDeemphasisUs) < 1e-6);
        CHECK(std::fabs(built[1].deemphasisUs - 50.0) < 1e-6);
        const double outRate = cc::patch::chooseChannelRate(s.state().sampleRateHz).rateHz;   // 2 MS/s, whole-decimated to about 48 kHz
        CHECK(std::fabs(built[1].mixLevel - static_cast<double>(cc::nfmMixLevel(12500.0, outRate))) < 1e-4);
        CHECK(built[1].mixLevel > 1.0);
        CHECK_EQ(built[0].squelchDb, -55.0f);
    }
    CHECK(passUntil(d.eng, [&] { return open(s, "High"); }));   // the NFM channel hears the tone too

    // THE SQUELCH OP rebuilds the block with the new threshold (every strip is built with it), clamps to -110..0, and reads back.
    testing::Outcome sq = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 4, 0, -70.0));
    CHECK_EQ(sq.status(), FOXAPI_OK);
    CHECK_EQ(sq.result.applied[1], -70.0);
    CHECK_EQ(fact(s, "airband.squelchDb"), std::string("-70.0"));
    passes(d.eng, 2);
    built = Access::built(d.eng);
    CHECK_EQ(built.size(), 2u);
    for (const auto& b : built) { CHECK_EQ(b.squelchDb, -70.0f); }
    // (still the NFM de-emphasis after a rebuild)
    if (built.size() == 2u) { CHECK(std::fabs(built[1].deemphasisUs - 50.0) < 1e-6); }
    CHECK(listening(s));
    sq = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 4, 0, -300.0));
    CHECK_EQ(sq.status(), FOXAPI_OK);
    CHECK_EQ(sq.result.applied[1], -110.0);
    CHECK((sq.result.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    sq = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 4, 0, 5.0));
    CHECK_EQ(sq.result.applied[1], 0.0);
    // ABOVE THE NOISE sets it from the channels' own levels: 8 dB over the median, inside the squelch's range.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 4, 0, -55.0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return chan(s, "Low").value[2] > -199.0; }));
    const testing::Outcome noise = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 3));
    CHECK_EQ(noise.status(), FOXAPI_OK);
    CHECK(noise.result.applied[1] >= -110.0 && noise.result.applied[1] <= 0.0);
    CHECK(noise.result.applied[1] != -55.0);
    // HOLD: clamped to 0..60 s, read back.
    const testing::Outcome hold = pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 5, 0, 90.0));
    CHECK_EQ(hold.status(), FOXAPI_OK);
    CHECK_EQ(hold.result.applied[1], 60.0);
    CHECK_EQ(fact(s, "airband.holdS"), std::string("60.0"));
    CHECK(listening(s));
    CHECK_EQ(pumped(d.eng, s, stopOp()).status(), FOXAPI_OK);
    // Not listening: nothing to rebuild or measure.
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 3)).status(), FOXAPI_NO_CHANGE);
    s.run(testing::cmd(FOXAPI_OP_RUN, 0, 0));
}

// ---- G1: the ops' edges --------------------------------------------------------------------------------------------------------------------

void testEdges() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kLocalGrants);
    CHECK(s.ok());
    Access::seed(d.eng, twoRows());
    passes(d.eng, 2);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, 9)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_AIRBAND, -1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 0)).status(), FOXAPI_BAD_ARGUMENT);   // no text at all: no row
    // A REMOTE session may not (every FOXAPP_OP_* is a LOCAL session's); neither may a session with no TUNE grant start the monitor or the scan.
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, kLocalGrants, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(pumped(d.eng, remote, listenOp("")).status(), FOXAPI_DENIED);
    CHECK_EQ(pumped(d.eng, remote, op(FOXAPP_OP_SCANNER_LIST, 2)).status(), FOXAPI_DENIED);
    CHECK_EQ(pumpedText(d.eng, remote, op(FOXAPP_OP_BOOKMARK_ADD_ROWS, 1), rowLine(121.5e6, "AM", 10000.0, 1, "", "x")).status(), FOXAPI_DENIED);
    testing::Session noTune(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    CHECK(noTune.ok());
    CHECK_EQ(pumped(d.eng, noTune, listenOp("")).status(), FOXAPI_DENIED);
    CHECK_EQ(pumped(d.eng, noTune, op(FOXAPP_OP_SCANNER_LIST, 2)).status(), FOXAPI_DENIED);
    CHECK_EQ(pumped(d.eng, noTune, tickOp(row(s, "Low").id, 0)).status(), FOXAPI_OK);   // the tick is a setting, not a tune
    passes(d.eng, 2);
    CHECK(!listening(s));
    // The web profile (the browser page's own session) has none of these either.
    std::int32_t st = 0;
    FoxSession* web = d.eng.api().openInternalSession(cascade::engine::InternalKind::Web, "the web server", &st);
    CHECK_EQ(st, FOXAPI_OK);
    if (web != nullptr) {
        CHECK_EQ(pumpedAs(d.eng, web, listenOp("")).status(), FOXAPI_DENIED);
        CHECK_EQ(pumpedAs(d.eng, web, tickOp(row(s, "High").id, 0)).status(), FOXAPI_DENIED);
        api()->close_session(web);
    }
    // A list moves under a listening monitor: an id that is gone is NOT_FOUND for TUNE ONE, and the monitor goes on.
    std::printf("G1: out-of-range actions, a REMOTE session, a session without TUNE and the web profile refused\n");
}

}  // namespace

int main() {
    std::printf("test_engine_api_airband\n");
    isolate();
    testTick();
    testAddRows();
    testListenAndStop();
    testBlockPlan();
    testOneChannelPerFrequency();
    testHandBack();
    testHandBackOfALentRadio();
    testStopRules();
    testListMode();
    testHeardTime();
    testNfmAndSquelch();
    testEdges();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_airband");
}
