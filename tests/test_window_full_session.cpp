// test_window_full_session.cpp - THE WINDOW'S COMMANDS AGAINST A SESSION THAT IS FULL (the deep pre-merge review, batch B, findings 3 to 6; docs/engine-stage4.md 6.32).
//
// The window talks to the engine through ONE session, and the session holds at most FOXAPI_MAX_PENDING (256) ordinary commands outstanding - a command counts until its RESULT has been
// read, and the window reads at most 64 results a frame (UiAnswers::kMaxResults). A command that finds the session full is answered FOXAPI_BUSY: not taken, no ticket, no answer.
// uiSubmitTicket's answer to that is to drop it (counted, said once) - or, for the setters whose newest figure supersedes the older (latestWins), to hold the newest one and offer it
// again each frame. Four places in the window treated BUSY as if it were not there, and each is a way of getting a figure wrong with nothing said:
//
//   A  (finding 5) A HELD "NEWEST WINS" SETTER IS CANCELLED BY A NEWER FIGURE THE SESSION ACCEPTED. The older figure was held when the session was full; the stall ended mid-frame and the
//      hand's newest figure went in directly; the next frame's flush then sent the STALE one after it, and if the gesture had ended nothing sent a newer - the volume, the dB window
//      or the squelch ended at the older figure
//   B  (finding 6) A STATE SENT ONCE ON AN EDGE IS RECORDED AS SENT ONLY WHEN THE SESSION TOOK IT. The plugin-use table, the transmit page's open flag and a panel's name were marked
//      sent before the submit and the answer was ignored, so a BUSY left the window believing it had told the engine - an AUTO plugin whose window was open stayed asleep, the
//      engine's transmit-page flag stayed wrong (the saved state, and the address box the same command seeds), and the panel was never named in the usage report. (The patch page's flag retried correctly.)
//   C  (finding 3) TICK ALL / UNTICK ALL LOSE NO ROW. The key sent one command a row, all in the frame of the press: a group of more than a session's room ticked the first 256 and
//      DROPPED the rest - and said "the pass is not keeping up" about the window's own burst. It is a paced run now: a few rows a frame, offered again when the session is full
//   D  (finding 4) A CHATTY TRACK-INFO PLUGIN CANNOT STARVE THE CONTROLS. sendTrackInfoText submitted one command for every line the plugin's poll_text yielded - up to 32 polls of 4 KB a
//      frame - and, being first in the frame, the lines took every slot the 64 results polled a frame freed: every control command after them was refused and dropped for as long
//      as the plugin kept talking. A per-frame cap and a bounded backlog (the engine's ring is 500 lines) now
//   E  the session closes with a Tick all still going out: the run is let go
//   W  the frame this test runs is drawUi's (a source scan of the order drawUi calls the members in)
//
// NOT RUN AGAINST A FREE-RUNNING PASS (FOXSDR_TEST_PASS=free): a pass of the engine's own drains the session while the test fills it, so "full" is not a state the test can hold there
// (test_interface_half_app G skips for the same reason). The inline and the stepped runs are the leg that holds it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_run.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "imgui.h"
#include "pass_seam.hpp"
#include "test_check.hpp"
#include "test_host.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace cmd = cascade::core::cmd;

namespace {
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// --- the track-info plugin that will not stop talking ------------------------------------------------------------------------------
std::string g_said;   // what it has left to say: lines, each ending in a newline
std::size_t g_saidFrom = 0;
void* fakeCreate() { return reinterpret_cast<void*>(1); }
std::int32_t fakeGetInfo(void*, const char*, std::uint32_t, CascadeTrackInfo*) { return CASCADE_INFO_MISSING; }
void fakeRelease(void*, const CascadeTrackInfo*) {}
std::int32_t fakePollText(void*, char* buf, size_t cap) {
    if (g_saidFrom >= g_said.size()) { return 0; }
    const std::size_t n = std::min<std::size_t>(cap, g_said.size() - g_saidFrom);
    std::memcpy(buf, g_said.data() + g_saidFrom, n);
    g_saidFrom += n;
    return static_cast<std::int32_t>(n);
}
void fakeDestroy(void*) {}
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
void say(int lines, int first = 0) {
    g_said.clear();
    g_saidFrom = 0;
    for (int i = 0; i < lines; ++i) {
        char l[64];
        std::snprintf(l, sizeof(l), "track-info line %04d\n", first + i);
        g_said += l;
    }
}
std::string lineText(int i) {
    char l[64];
    std::snprintf(l, sizeof(l), "track-info line %04d", i);
    return l;
}
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static constexpr int kTickPerFrame = AppWindow::kTickRowsPerFrame;
    static constexpr int kLinesPerFrame = AppWindow::kTrackInfoLinesPerFrame;

    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
    }
    // THE ENGINE'S PASS (a whole one; the answers are NOT read here - a command counts against the session until its result is read) and the window's read of the state.
    static void pump(AppWindow& a) {
        passseam::whole(th::engine(a), [&] { th::engine(a).pump(); });
        a.refreshUiState();
    }
    // ONE FRAME, in drawUi's order as far as these members go: the track-info lines first, the pass, the patch and transmit pages' flags, the answers polled (and the held setters
    // offered again), the state read, the answers acted on, the plugin-use report (the plugin windows' poll), and the Airband section's once-a-frame half at the end.
    static void frame(AppWindow& a) {
        a.sendTrackInfoText();
        passseam::whole(th::engine(a), [&] { th::engine(a).pump(); });
        a.syncPatchPageOpen();
        a.syncTransmitPageOpen();
        a.pollUiAnswers();
        a.refreshUiState();
        a.actOnUiAnswers(true);
        a.reportPluginUse();
        a.airbandFrame();
    }
    // A STALLED FRAME: the pass does not run and no answer is read, so the session stays as full as it was - only the window's own once-a-frame work moves.
    static void stalledFrame(AppWindow& a) {
        a.sendTrackInfoText();
        a.syncPatchPageOpen();
        a.syncTransmitPageOpen();
        a.reportPluginUse();
        a.airbandFrame();
    }
    // Every result the session holds, read and thrown away (after a pass): the session is empty.
    static void drain(AppWindow& a) {
        pump(a);
        for (;;) {
            FoxCommandResult r[64];
            for (auto& x : r) { x.structSize = sizeof(x); }
            if (a.engineApi_->poll_results(a.uiSession_, r, 64) <= 0) { break; }
        }
        a.uiAnswers_.nResults = 0;
        a.uiAnswers_.nEvents = 0;
    }
    // `k` results read and thrown away, with no frame's flush: k places in the session.
    static int readResults(AppWindow& a, int k) {
        FoxCommandResult r[64];
        for (auto& x : r) { x.structSize = sizeof(x); }
        return a.engineApi_->poll_results(a.uiSession_, r, static_cast<std::uint32_t>(k));
    }
    // The session filled to its cap with commands nobody wants the answers of (SET_STEREO 1: an ordinary command - not one of the safer kinds, not a newest-wins setter - that the
    // engine answers NO_CHANGE and that changes nothing). Returns how many it took.
    static int fill(AppWindow& a) {
        const std::uint64_t drops = a.uiBusyDrops_;
        int n = 0;
        while (a.uiBusyDrops_ == drops && n < 2000) {
            a.uiSubmit(cmd::makeInt(FOXAPI_OP_SET_STEREO, 1));
            ++n;
        }
        return n - 1;
    }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static std::uint64_t submitTicket(AppWindow& a, const FoxCommand& c) { return a.uiSubmitTicket(cmd::QueuedCommand{c, {}}); }
    static std::uint64_t busyDrops(AppWindow& a) { return a.uiBusyDrops_; }
    static std::size_t held(AppWindow& a) { return a.uiHeld_.size(); }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static void tap(AppWindow& a, std::vector<cmd::QueuedCommand>* log) { a.uiSubmitTap_ = log; }
    // B
    static void reportPluginUse(AppWindow& a) { a.reportPluginUse(); }
    static const std::string& useSent(AppWindow& a) { return a.pluginUseSent_; }
    static bool& decoderWindowOpen(AppWindow& a) { return a.decoderWindowOpen_; }
    static std::size_t engineReports(AppWindow& a, const std::string& file) {
        return passseam::read<std::size_t>(th::engine(a), [&] {
            const auto it = th::engine(a).pluginUseReported_.find(cascade::core::pluginRunId(file));
            return it == th::engine(a).pluginUseReported_.end() ? std::size_t{0} : static_cast<std::size_t>(it->second);
        });
    }
    static void setTransmitPageOpen(AppWindow& a, bool open) { a.setTransmitPageOpen(open); }
    static bool windowTransmitOpen(AppWindow& a) { return a.transmitPageOpen_; }
    static bool engineTransmitOpen(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).transmitOpen_; });
    }
    static bool& patchOpen(AppWindow& a) { return a.patchOpen_; }
    static bool patchSent(AppWindow& a) { return a.patchPageOpenSent_; }
    static void notePanel(AppWindow& a, const char* name) { a.uiNotePanel(name); }
    static std::size_t notedByWindow(AppWindow& a) { return a.uiNotedPanels_.size(); }
    static std::vector<std::string> enginePanels(AppWindow& a) {
        return passseam::read<std::vector<std::string>>(th::engine(a), [&] { return th::engine(a).telemetryPanels_; });
    }
    static const std::vector<PluginRow>& pluginRows(AppWindow& a) { return a.pluginLists().plugins; }
    // C
    static void addRows(AppWindow& a, const std::string& group, int n, double firstMHz) {
        std::string text;
        for (int i = 0; i < n; ++i) {
            char line[200];
            std::snprintf(line, sizeof(line), "%.6f\tAM\t10000.000\t0\t%s\t%s-%d\n", (firstMHz + 0.0125 * i) * 1.0e6, group.c_str(), group.c_str(), i);
            text += line;
        }
        const cmd::QueuedCommand q = cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, text, 1);
        passseam::on(th::engine(a), [&] { (void)th::engine(a).applyCommand(q.c, q.longText); });
    }
    static const std::vector<BookmarkRow>& markers(AppWindow& a) { return a.tuningLists().markers; }
    static void tickAll(AppWindow& a, const std::vector<std::uint32_t>& rows, bool on) { a.airbandTickAll(rows, on); }
    static bool tickRunActive(AppWindow& a) { return a.airband_.ticks.active(); }
    static void closeSession(AppWindow& a) { a.closeUiSession(); }
    // D
    static void attachTrackInfo(AppWindow& a, const CascadeTrackInfoApi* api) { a.trackInfo_.attach(api); }
    static void sendTrackInfoText(AppWindow& a) { a.sendTrackInfoText(); }
    static std::vector<std::string> aircraftInfoLines(AppWindow& a) {
        return passseam::read<std::vector<std::string>>(th::engine(a), [&] {
            std::vector<std::string> out;
            for (const cascade::core::DecodedLine& l : th::engine(a).decoderLog_) {
                if (l.plugin == "Aircraft info") { out.push_back(l.text); }
            }
            return out;
        });
    }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

int countOf(const std::vector<cmd::QueuedCommand>& log, std::size_t from, std::uint32_t op) {
    int n = 0;
    for (std::size_t i = from; i < log.size(); ++i) { n += log[i].c.op == op ? 1 : 0; }
    return n;
}

// The indexes (into the window's one copy of BOOKMARKS) of the rows of a group, in the list's order: what the Airband section's row view holds for it.
std::vector<std::uint32_t> rowsOfGroup(AppWindow& app, const std::string& group) {
    std::vector<std::uint32_t> rows;
    const auto& list = A::markers(app);
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].group == group) { rows.push_back(static_cast<std::uint32_t>(i)); }
    }
    return rows;
}
std::size_t tickedIn(AppWindow& app, const std::vector<std::uint32_t>& rows) {
    std::size_t n = 0;
    const auto& list = A::markers(app);
    for (const std::uint32_t i : rows) { n += (i < list.size() && list[i].ticked) ? 1u : 0u; }
    return n;
}

// --- A -----------------------------------------------------------------------------------------

void testNewestWins(AppWindow& app) {
    std::printf("  A: a held newest-wins setter is cancelled by a newer figure the session accepted\n");
    // CONTROL: a figure the full session refused is HELD, a newer one replaces it, and the room that comes sends it - the hold does its job.
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.30));
    CHECK_EQ(A::held(app), 1u);
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.35));
    CHECK_EQ(A::held(app), 1u);   // replaced, not stacked
    A::drain(app);
    A::frame(app);   // the flush
    A::frame(app);
    CHECK_EQ(A::held(app), 0u);
    CHECK_NEAR(A::state(app).volume, 0.35, 1e-6);

    // THE FINDING: the stall ends mid-frame - the hand's newest figure finds room and is accepted directly - and a later frame's flush must not send the older one after it.
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.60));   // refused: held
    CHECK_EQ(A::held(app), 1u);
    A::pump(app);                                                // the pass answers the 256 ...
    CHECK_EQ(A::readResults(app, 1), 1);                         // ... and ONE is read, with no frame's flush in between: one place in the session
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.40));   // the hand's newest figure: taken directly
    CHECK_EQ(A::held(app), 0u);                                  // the older figure is not held any more
    A::frame(app);                                               // the next frames' flushes
    A::frame(app);
    A::frame(app);
    CHECK_NEAR(A::state(app).volume, 0.40, 1e-6);                // ...and the volume is the newest figure, not the older one sent after it
    A::drain(app);

    // IT IS THE SAME OP'S FIGURE THAT IS SUPERSEDED, and no other: a squelch held beside the volume is still sent when the volume's newer figure is taken.
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.20));
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_SQUELCH, -50.0));
    CHECK_EQ(A::held(app), 2u);
    A::pump(app);
    CHECK_EQ(A::readResults(app, 1), 1);
    A::submit(app, cmd::makeNum(FOXAPI_OP_SET_VOLUME, 0.25));
    CHECK_EQ(A::held(app), 1u);                                  // the squelch only
    A::frame(app);
    A::frame(app);
    A::frame(app);
    CHECK_EQ(A::held(app), 0u);
    CHECK_NEAR(A::state(app).volume, 0.25, 1e-6);
    CHECK_NEAR(A::state(app).squelchDb, -50.0, 1e-6);            // sent, and not lost with the volume's
    A::drain(app);
}

// --- B -----------------------------------------------------------------------------------------

void testEdgesAreRecordedWhenTaken(AppWindow& app) {
    std::printf("  B: a state sent once on an edge is recorded as sent only when the session took it\n");
    // B1. THE PLUGIN-USE TABLE: the Decoder output window is open, and the stage-one text decoder is wanted by it ("<file> TAB 8").
    const std::string file = std::filesystem::path(FIXTURE_STAGE1_PLUGIN).filename().string();
    A::frame(app);
    A::frame(app);
    bool fitted = false;
    for (const cascade::gui::PluginRow& r : A::pluginRows(app)) { fitted = fitted || (r.file == file && r.loaded); }
    CHECK(fitted);
    A::decoderWindowOpen(app) = true;
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::reportPluginUse(app);                                     // refused: the session is full
    CHECK(A::useSent(app).empty());                              // ...so the table has NOT been sent
    CHECK_EQ(A::engineReports(app, file), 0u);
    A::drain(app);                                               // room
    A::frame(app);                                               // the next frame offers it again
    CHECK_EQ(A::useSent(app), file + "\t8\n");
    A::frame(app);
    CHECK_EQ(A::engineReports(app, file), 8u);                   // the engine has it: the plugin is wanted, and wakes
    A::decoderWindowOpen(app) = false;
    A::frame(app);
    A::frame(app);

    // B2. THE TRANSMIT PAGE'S OPEN FLAG. CONTROL: with room, the click's own call tells the engine at once (the next pass has it), and a flag the engine has is not sent again.
    A::drain(app);
    std::vector<cmd::QueuedCommand> tap;
    A::tap(app, &tap);
    A::setTransmitPageOpen(app, true);
    A::pump(app);
    CHECK(A::engineTransmitOpen(app));
    A::frame(app);
    A::frame(app);
    A::frame(app);
    CHECK_EQ(countOf(tap, 0, FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN), 1);   // once, not once a frame
    A::setTransmitPageOpen(app, false);
    A::pump(app);
    CHECK(!A::engineTransmitOpen(app));
    A::tap(app, nullptr);
    // THE FINDING: the operator opens the page while the session is full.
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::setTransmitPageOpen(app, true);
    CHECK(A::windowTransmitOpen(app));                           // the page opens, whatever the session says
    CHECK(!A::engineTransmitOpen(app));                          // ...and the engine has not been told
    A::drain(app);
    A::frame(app);                                               // the frames offer it again
    A::frame(app);
    CHECK(A::engineTransmitOpen(app));                           // and the engine knows
    // ...and so for the page closing while the session is full.
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::setTransmitPageOpen(app, false);
    CHECK(!A::windowTransmitOpen(app));
    CHECK(A::engineTransmitOpen(app));
    A::drain(app);
    A::frame(app);
    A::frame(app);
    CHECK(!A::engineTransmitOpen(app));

    // B3. A PANEL'S NAME for the usage report: noted once a run - when the engine has it.
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    A::notePanel(app, "radar");
    CHECK_EQ(A::notedByWindow(app), 0u);                         // refused: it is not a name the engine has been told
    CHECK(A::enginePanels(app).empty());
    A::drain(app);
    A::notePanel(app, "radar");                                  // the page draws again next frame and says it again
    CHECK_EQ(A::notedByWindow(app), 1u);
    A::notePanel(app, "radar");                                  // ...and then once a run: nothing more is sent
    A::pump(app);
    const std::vector<std::string> panels = A::enginePanels(app);
    CHECK_EQ(panels.size(), 1u);
    CHECK(!panels.empty() && panels[0] == "radar");
    A::drain(app);

    // B4. THE PATCH PAGE'S FLAG already retried; it is OFFERED now, as the three above are, so a refusal is no drop: nothing is counted against the pass for a command the window keeps.
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    const std::uint64_t drops = A::busyDrops(app);
    A::patchOpen(app) = !A::patchOpen(app);                      // the operator switches views
    for (int i = 0; i < 3; ++i) { A::stalledFrame(app); }
    CHECK_EQ(A::busyDrops(app), drops);
    CHECK(A::patchOpen(app) != A::patchSent(app));              // still to be told
    A::drain(app);
    A::frame(app);
    CHECK(A::patchOpen(app) == A::patchSent(app));
    A::patchOpen(app) = !A::patchOpen(app);                      // and back
    A::frame(app);
    A::frame(app);
    CHECK(A::patchOpen(app) == A::patchSent(app));
    A::drain(app);
}

// --- C -----------------------------------------------------------------------------------------

void testTickAllLosesNoRow(AppWindow& app) {
    std::printf("  C: Tick all and Untick all lose no row: a few a frame, offered again when the session is full\n");
    A::addRows(app, "Big", 600, 118.0);
    A::addRows(app, "Big2", 300, 125.0);
    A::frame(app);
    A::frame(app);
    const std::vector<std::uint32_t> rows = rowsOfGroup(app, "Big");
    const std::vector<std::uint32_t> rows2 = rowsOfGroup(app, "Big2");
    CHECK_EQ(rows.size(), 600u);
    CHECK_EQ(rows2.size(), 300u);
    CHECK_EQ(tickedIn(app, rows), 0u);

    // C1. THE PRESS, on an empty session: 600 rows, three times what a session holds.
    A::drain(app);
    const std::uint64_t drops = A::busyDrops(app);
    std::vector<cmd::QueuedCommand> tap;
    A::tap(app, &tap);
    A::tickAll(app, rows, true);
    CHECK_EQ(countOf(tap, 0, FOXAPP_OP_BOOKMARK_TICK), 0);       // the press itself sends nothing: it starts a run
    int worst = 0;
    int frames = 0;
    do {
        const std::size_t mark = tap.size();
        A::frame(app);
        worst = std::max(worst, countOf(tap, mark, FOXAPP_OP_BOOKMARK_TICK));
    } while (A::tickRunActive(app) && ++frames < 200);
    A::frame(app);   // the last batch is applied and read
    A::frame(app);
    std::printf("      600 rows: %d frames, the most a frame sent %d (the cap is %d), %zu rows ticked, %llu command(s) dropped\n", frames + 1, worst, A::kTickPerFrame, tickedIn(app, rows),
                static_cast<unsigned long long>(A::busyDrops(app) - drops));
    CHECK(worst <= A::kTickPerFrame);
    CHECK_EQ(tickedIn(app, rows), 600u);                         // every one
    CHECK_EQ(A::busyDrops(app), drops);                          // and nothing dropped, nothing said about the pass

    // A press on a settled list sends only the rows that differ: Tick all on a group that is all ticked says nothing at all.
    for (int i = 0; i < 4; ++i) { A::frame(app); }
    {
        const std::size_t mark = tap.size();
        A::tickAll(app, rows, true);
        A::frame(app);
        CHECK(!A::tickRunActive(app));
        CHECK_EQ(countOf(tap, mark, FOXAPP_OP_BOOKMARK_TICK), 0);
    }

    // C3. UNTICK ALL, then TICK ALL while the first is still going out: the second must cover the rows the first has sent and the list has not shown yet.
    A::tickAll(app, rows, false);
    for (int i = 0; i < 3; ++i) { A::frame(app); }
    CHECK(A::tickRunActive(app));                                // ~96 rows sent, ~500 to go; the last batch is queued, not applied, not shown
    A::tickAll(app, rows, true);
    for (frames = 0; frames < 200 && A::tickRunActive(app); ++frames) { A::frame(app); }
    A::frame(app);
    A::frame(app);
    CHECK_EQ(tickedIn(app, rows), 600u);                         // the newest press wins, for every row
    A::tickAll(app, rows, false);
    for (frames = 0; frames < 200 && A::tickRunActive(app); ++frames) { A::frame(app); }
    A::frame(app);
    A::frame(app);
    CHECK_EQ(tickedIn(app, rows), 0u);                           // Untick all is the same run

    // C2. THE PRESS AGAINST A SESSION THAT IS FULL AND NOT DRAINING (a stalled pass): the rows wait their turn.
    A::drain(app);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    const std::uint64_t full = A::busyDrops(app);
    A::tickAll(app, rows2, true);
    for (int i = 0; i < 5; ++i) { A::stalledFrame(app); }
    CHECK_EQ(A::busyDrops(app), full);                           // none refused-and-forgotten, none counted: the run is waiting for room
    CHECK(A::tickRunActive(app));
    CHECK_EQ(tickedIn(app, rows2), 0u);
    A::drain(app);                                               // the stall ends
    for (frames = 0; frames < 200 && A::tickRunActive(app); ++frames) { A::frame(app); }
    A::frame(app);
    A::frame(app);
    CHECK_EQ(tickedIn(app, rows2), 300u);                        // and the whole group is ticked

    // C4. THE OPPOSITE KEY IN THE FRAME AFTER THE LAST ROW WENT OUT: the run is over, but the pass has not applied its last batch and the list does not show it, so the press
    // still covers every row (the run is "settled" only a few frames after its last row).
    A::drain(app);
    A::tickAll(app, rows2, false);
    for (frames = 0; frames < 200 && A::tickRunActive(app); ++frames) { A::frame(app); }   // the last batch has just gone out
    CHECK(!A::tickRunActive(app));
    A::tickAll(app, rows2, true);                                // ... and the list still shows its rows ticked
    for (frames = 0; frames < 200 && A::tickRunActive(app); ++frames) { A::frame(app); }
    A::frame(app);
    A::frame(app);
    CHECK_EQ(tickedIn(app, rows2), 300u);                        // the newest press wins for the rows of the last batch too
    A::tap(app, nullptr);
    A::drain(app);
}

// --- D -----------------------------------------------------------------------------------------

void testTrackInfoLines(AppWindow& app) {
    std::printf("  D: a chatty track-info plugin: a few lines a frame, none lost before the ring's bound, the controls never starved\n");
    const CascadeTrackInfoApi api = fakeApi();
    A::attachTrackInfo(app, &api);
    A::drain(app);
    A::frame(app);

    // D1. 300 lines in one drain (a failing lookup per track, say): a frame forwards a few, the controls still find room, and every line arrives, in order.
    say(300);
    const std::uint64_t total0 = A::state(app).decoderLinesTotal;
    std::vector<cmd::QueuedCommand> tap;
    A::tap(app, &tap);
    A::sendTrackInfoText(app);
    const int firstFrame = countOf(tap, 0, FOXAPP_OP_TRACK_INFO_LINE);
    std::printf("      300 lines said at once: the first frame forwarded %d (the cap is %d)\n", firstFrame, A::kLinesPerFrame);
    CHECK(firstFrame <= A::kLinesPerFrame);
    CHECK(firstFrame >= 1);
    CHECK(A::submitTicket(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1)) != 0u);   // a control command, right after the lines: taken
    for (int i = 0; i < 80; ++i) { A::frame(app); }
    CHECK_EQ(A::state(app).decoderLinesTotal, total0 + 300u);
    const std::vector<std::string> got = A::aircraftInfoLines(app);
    CHECK_EQ(got.size(), 300u);
    bool inOrder = got.size() == 300u;
    for (int i = 0; inOrder && i < 300; ++i) { inOrder = got[static_cast<std::size_t>(i)] == lineText(i); }
    CHECK(inOrder);
    A::submit(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 0));
    A::frame(app);

    // D2. A plugin that says more than the engine's ring holds: the window keeps the newest 500 (the oldest are dropped, said once), and passes them on a few a frame.
    A::drain(app);
    say(1200, 0);
    const std::uint64_t total1 = A::state(app).decoderLinesTotal;
    const std::size_t said0 = [] {
        std::size_t n = 0;
        for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) { n += (l.find("track-info plugin") != std::string::npos && l.find("dropped") != std::string::npos) ? 1u : 0u; }
        return n;
    }();
    for (int i = 0; i < 120; ++i) { A::frame(app); }
    CHECK_EQ(A::state(app).decoderLinesTotal, total1 + 500u);
    const std::vector<std::string> kept = A::aircraftInfoLines(app);
    CHECK_EQ(kept.size(), 500u);
    CHECK(!kept.empty() && kept.front() == lineText(700));
    CHECK(!kept.empty() && kept.back() == lineText(1199));
    std::size_t said = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) { said += (l.find("track-info plugin") != std::string::npos && l.find("dropped") != std::string::npos) ? 1u : 0u; }
    CHECK_EQ(said - said0, 1u);                                  // ONE line in the log about it, not one a frame

    // D3. A SESSION THAT IS FULL: the lines wait, they are not dropped (twenty of them, well inside the bound).
    A::drain(app);
    A::frame(app);
    const std::uint64_t total2 = A::state(app).decoderLinesTotal;
    say(20, 2000);
    CHECK_EQ(A::fill(app), static_cast<int>(FOXAPI_MAX_PENDING));
    const std::uint64_t dropsBefore = A::busyDrops(app);
    for (int i = 0; i < 6; ++i) { A::stalledFrame(app); }        // the pass is stalled: nothing leaves, nothing is read
    CHECK_EQ(A::busyDrops(app), dropsBefore);                    // and nothing was dropped or counted against the pass
    A::drain(app);                                               // the stall ends
    for (int i = 0; i < 20; ++i) { A::frame(app); }
    CHECK_EQ(A::state(app).decoderLinesTotal, total2 + 20u);
    const std::vector<std::string> waited = A::aircraftInfoLines(app);
    CHECK(waited.size() >= 20u);
    bool tailInOrder = waited.size() >= 20u;
    for (int i = 0; tailInOrder && i < 20; ++i) { tailInOrder = waited[waited.size() - 20u + static_cast<std::size_t>(i)] == lineText(2000 + i); }
    CHECK(tailInOrder);

    // D4. THE PLUGIN GOES: what it had said and the window had not yet passed on goes with it - a different plugin's lines are not served as this one's.
    A::drain(app);
    A::frame(app);
    const std::uint64_t total3 = A::state(app).decoderLinesTotal;
    say(50, 3000);
    A::frame(app);                                               // 8 of the 50 are passed on
    A::attachTrackInfo(app, nullptr);                            // the plugin is detached
    A::frame(app);
    A::frame(app);
    say(0);
    A::attachTrackInfo(app, &api);                               // ...another (here, a silent one) is attached
    for (int i = 0; i < 20; ++i) { A::frame(app); }
    CHECK_EQ(A::state(app).decoderLinesTotal, total3 + static_cast<std::uint64_t>(A::kLinesPerFrame));   // the eight of the one frame it was attached for, and no more
    const std::vector<std::string> after = A::aircraftInfoLines(app);
    CHECK(after.empty() || after.back() != lineText(3049));      // the last of its fifty never arrives
    A::tap(app, nullptr);
    A::attachTrackInfo(app, nullptr);
    A::drain(app);
}

// --- E -----------------------------------------------------------------------------------------

void testSessionClosesWithARunOut(AppWindow& app) {
    std::printf("  E: the session closes with a Tick all still going out: the run is let go, not kept for a session that is gone\n");
    A::drain(app);
    A::addRows(app, "Gone", 200, 130.0);
    A::frame(app);
    A::frame(app);
    const std::vector<std::uint32_t> rows = rowsOfGroup(app, "Gone");
    CHECK_EQ(rows.size(), 200u);
    A::tickAll(app, rows, true);
    A::frame(app);
    CHECK(A::tickRunActive(app));
    A::closeSession(app);
    CHECK(!A::tickRunActive(app));
}

// --- W -----------------------------------------------------------------------------------------

// THE FRAME THIS TEST RUNS IS THE WINDOW'S: drawUi calls the members in the order frame() above does (the ones that matter here). A source scan, because drawUi needs a GL context
// and cannot be run - the same way tests/test_engine_shell_guard.cpp holds the order of the frame's look.
void testTheFrameIsDrawUis() {
    std::printf("  W: drawUi calls the members this test's frame calls, in its order\n");
    std::ifstream in(std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp", std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    const std::size_t head = text.find("void AppWindow::drawUi() {");
    CHECK(head != std::string::npos);
    const std::size_t end = head == std::string::npos ? std::string::npos : text.find("\n}\n", head);
    CHECK(end != std::string::npos);
    if (head == std::string::npos || end == std::string::npos) { return; }
    // (comment lines dropped: what a comment says about a call is not the call)
    std::string body;
    {
        std::istringstream lines(text.substr(head, end - head));
        for (std::string l; std::getline(lines, l);) {
            const std::size_t first = l.find_first_not_of(" \t");
            if (first == std::string::npos || l.compare(first, 2, "//") == 0) { continue; }
            const std::size_t slashes = l.find("//");
            body += (slashes == std::string::npos ? l : l.substr(0, slashes)) + "\n";
        }
    }
    std::size_t at = 0;
    for (const char* call : {"sendTrackInfoText();", "syncPatchPageOpen();", "syncTransmitPageOpen();", "pollUiAnswers();", "refreshUiState();", "actOnUiAnswers();", "airbandFrame();"}) {
        const std::size_t found = body.find(call, at);
        if (found == std::string::npos) { std::printf("FAIL: drawUi does not call %s after the call before it\n", call); }
        CHECK(found != std::string::npos);
        if (found != std::string::npos) { at = found + 1; }
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_window_full_session\n");
    if (passseam::mode() == passseam::Mode::Free) {
        std::printf("  skipped - free-running pass (the session cannot be held full)\n");
        return 0;
    }
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / ("foxsdr_full_session_" + std::to_string(pid));
    std::filesystem::create_directories(scratch / "plugins");
    g_pluginDir = (scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    A::install();
    std::error_code ec;
    const std::filesystem::path fixture(FIXTURE_STAGE1_PLUGIN);
    std::filesystem::copy_file(fixture, std::filesystem::path(g_pluginDir) / fixture.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    CHECK(!ec);

    testTheFrameIsDrawUis();
    {
        th::TestWindow app;
        A::frame(app);
        A::frame(app);
        testNewestWins(app);
        testEdgesAreRecordedWhenTaken(app);
        testTickAllLosesNoRow(app);
        testTrackInfoLines(app);
        testSessionClosesWithARunOut(app);   // (last: it closes the window's session)
    }

    ImGui::DestroyContext();
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_window_full_session");
}
