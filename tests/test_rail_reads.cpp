// test_rail_reads.cpp - THE RECEIVER RAIL AND THE DISPLAY READ THEIR ENGINE API
// SESSION (window port, group 5: drawMenuColumn's footer, drawRadioSection,
// drawStereoRdsControls, drawAudioFilterSection, drawSinksSection,
// drawDisplaySection, drawLanguageSection's country picker and
// drawBandPlanOverlay; engine-stage4.md 6.5, API 0.3 section 8.4).
//
// What they draw is the table's state, lists and facts. This holds the window's
// side of that against the real engine and the shipped band plans, with nothing
// on the desk opened:
//   A  the pure rules the window restates, held against the model they restate:
//      the narrowest band under a frequency and the bands a view shows
//      (core::BandPlan::at / ::visible, on every shipped plan and the merge of
//      them all), the mode key from demodMode, and the Language section's note
//   B  the cache follows the engine: the plans, the loaded plan's name and rows,
//      the audio outputs, the two status lines
//   C  the loaded plan's rows are copied ONCE: listSeq also moves when a decoder
//      prints a line, and that re-reads the small lists and not the rows
//   D  WHAT A FRAME COSTS once nothing has changed: the reads allocate nothing,
//      however many times a frame asks
//   E  the members DRAWN in real ImGui frames: they draw, and drawing sends
//      nothing
//   F  what the members draw, as text, from the state they read (the RDS and
//      stereo lines, the notch readout, the muted-by line, the watchdog note,
//      the band plan caption and the footer)
//   G  the commands the members send, built as they build them, accepted
//      through the window's session and applied: the state the rail reads back
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/band_plan.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/rail_reads.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // LogToBuffer: what a frame drew, as text
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

// --- allocations on THIS thread (the window and the test share it) ------------
namespace {
thread_local long t_allocs = 0;
}
void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

const std::string kPlans = std::string(CASCADE_SOURCE_DIR) + "/resources/bandplans";

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Rail = AppWindow::RailListCache;
    // The shipped band plans, read from the source tree (the tests' executables
    // have none beside them), and loaded as the engine loads them.
    static void usePlans(AppWindow& a) {
        a.engine_.bandPlanDirForTest_ = kPlans;
        a.engine_.loadBandPlan();
    }
    // The window looks, as a frame does: the engine publishes, the window reads.
    static void look(AppWindow& a) {
        passseam::on(a.engine_, [&] { a.engine_.publishReceiverState(); });
        a.refreshUiState();
    }
    // The engine's pass as far as a submit needs it, the answers drained as a
    // frame's pumpTransmitKey does: how many commands the session had taken.
    static int pass(AppWindow& a) {
        passseam::pass(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            (void)a.engine_.pumpPublish();
        });
        int answered = 0;
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            answered += got;
            if (got < 16) { break; }
        }
        return answered;
    }
    static const Rail& rail(AppWindow& a) { return a.railLists(); }
    // The engine's pass, and the statuses of the answers the session was given for what it submitted.
    static std::vector<int32_t> passStatuses(AppWindow& a) {
        passseam::pass(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            (void)a.engine_.pumpPublish();
        });
        std::vector<int32_t> out;
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            for (int32_t i = 0; i < got; ++i) { out.push_back(r[i].status); }
            if (got < 16) { break; }
        }
        return out;
    }
    // The allocations one get_setting of a fact makes inside the table.
    static long factAllocations(AppWindow& a, const char* key) {
        char buf[512];
        const long mark = t_allocs;
        (void)a.engineApi_->get_setting(a.uiSession_, key, buf, sizeof(buf));
        return t_allocs - mark;
    }
    static void submit(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) { a.uiSubmit(q); }
    // A change of the lists that touches nothing these tests read. A decoder line used to be it (it moved
    // listSeq once a line); since API 0.3 8.7 (D3) a line moves no list counter, so the preset note - a
    // status line the Decoders section letters - changes instead, and moves listSeq all the same.
    static void aLineAppears(AppWindow& a) {
        static unsigned n = 0;
        a.engine_.presetNote_ = "note " + std::to_string(++n);
    }
    // The engine's side, for comparison.
    static const cascade::core::BandPlan& plan(AppWindow& a) { return a.engine_.bandPlan_; }
    static const std::vector<cascade::core::PlanInfo>& choices(AppWindow& a) { return a.engine_.bandPlanChoices_; }
    static const std::string& selection(AppWindow& a) { return a.engine_.bandPlanSelection_; }
    static const std::vector<cascade::sink::AudioDevice>& outputs(AppWindow& a) { return a.engine_.devices_; }
    static int outputIndex(AppWindow& a) { return a.engine_.deviceIndex_; }
    static void setAudioHealth(AppWindow& a, const std::string& note) { a.engine_.audioHealthNote_ = note; }
    static void setBandPlanError(AppWindow& a, const std::string& note) { a.engine_.bandPlanError_ = note; }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    // The members, drawn.
    static void drawRadio(AppWindow& a) { a.drawRadioSection(); }
    static void drawFilters(AppWindow& a) { a.drawAudioFilterSection(); }
    static void drawSinks(AppWindow& a) { a.drawSinksSection(); }
    static void drawDisplay(AppWindow& a) { a.drawDisplaySection(); }
    static void drawLanguage(AppWindow& a) { a.drawLanguageSection(); }
    static void drawStereo(AppWindow& a) { a.drawStereoRdsControls(); }
    static void drawOverlay(AppWindow& a) { a.drawBandPlanOverlay(0.0f, 0.0f, 900.0f, 300.0f); }
    // The engine's stand-ins and settings, set as the table's test hooks and commands do.
    static void rds(AppWindow& a, const cascade::core::RdsSnapshot* heard) {
        a.engine_.testRdsOn_ = heard != nullptr;
        if (heard != nullptr) { a.engine_.testRds_ = *heard; }
    }
    static void pilot(AppWindow& a, bool locked) {
        a.engine_.testPilotOn_ = true;
        a.engine_.testPilotLocked_ = locked;
    }
    static void autoNotch(AppWindow& a, bool engaged, double hz) {
        a.engine_.testAutoNotchOn_ = true;
        a.engine_.testAutoNotchEngaged_ = engaged;
        a.engine_.testAutoNotchHz_ = hz;
    }
    static void command(AppWindow& a, const FoxCommand& c) { (void)a.engine_.applyCommand(c); }
    static void mutedBy(AppWindow& a, std::vector<std::string> names) { a.engine_.mutedBy_ = std::move(names); }
    static void country(AppWindow& a, const std::string& code) { a.countrySetting_ = code; }
    static void ask(AppWindow& a, const cascade::gui::BandPlanAsk& k) { a.bandPlanAsk_ = k; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::BandRow;
namespace cc = cascade::core;

namespace {

// One frame with `body` drawn inside a window; no engine pass (a draw alone
// sends commands to the session, nothing applies them until `Access::pass`).
template <class F>
void frame(F body) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(520, 900));
    ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    body();
    ImGui::End();
    ImGui::Render();
}

std::vector<BandRow> rowsOf(const cc::BandPlan& plan) {
    std::vector<BandRow> rows;
    for (const cc::BandEntry& e : plan.entries()) { rows.push_back({e.startHz, e.endHz, e.name, e.service}); }
    return rows;
}

// --- A -----------------------------------------------------------------------

void testBandRulesAgainstTheModel() {
    std::printf("  A1: the narrowest band and the bands a view shows, against core::BandPlan\n");
    std::vector<std::pair<std::string, std::string>> loads;  // {label, selection id}
    loads.push_back({"every plan merged", ""});
    for (const cc::PlanInfo& p : cc::BandPlan::available(kPlans)) { loads.push_back({p.name, p.id}); }
    CHECK(loads.size() > 5);
    std::mt19937_64 rng(20261013);
    long frequencies = 0;
    long windows = 0;
    for (const auto& [label, id] : loads) {
        cc::BandPlan plan;
        std::string err;
        CHECK(plan.loadSelection(kPlans, id, err));
        const std::vector<BandRow> rows = rowsOf(plan);
        CHECK(!rows.empty());
        // The probes: every edge, just inside and just outside it, and random points.
        std::vector<double> hz = {0.0, -1.0, 1.0e3, 1.0e12, std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()};
        for (const BandRow& r : rows) {
            for (const double e : {r.startHz, r.endHz}) {
                hz.push_back(e);
                hz.push_back(std::nextafter(e, 0.0));
                hz.push_back(std::nextafter(e, 1.0e18));
            }
        }
        std::uniform_real_distribution<double> any(0.0, 6.0e9);
        for (int i = 0; i < 400; ++i) { hz.push_back(any(rng)); }
        for (const double f : hz) {
            const cc::BandEntry* want = plan.at(f);
            const BandRow* got = cascade::gui::bandRowAt(rows, f);
            const bool same = (want == nullptr) == (got == nullptr) &&
                              (want == nullptr || (want->startHz == got->startHz && want->endHz == got->endHz &&
                                                   want->name == got->name && want->service == got->service));
            if (!same) { std::printf("      %s: at(%.17g) differs\n", label.c_str(), f); }
            CHECK(same);
            ++frequencies;
        }
        // The views: edges against edges, inverted, empty, non-finite, random.
        std::vector<std::pair<double, double>> views = {{0.0, 0.0},
                                                         {1.0e8, 1.0e8},
                                                         {2.0e8, 1.0e8},
                                                         {std::numeric_limits<double>::quiet_NaN(), 1.0e8},
                                                         {1.0e8, std::numeric_limits<double>::quiet_NaN()},
                                                         {0.0, 6.0e9}};
        for (const BandRow& r : rows) {
            views.push_back({r.startHz, r.endHz});                         // exactly the band
            views.push_back({r.endHz, r.endHz + 1.0e6});                   // only touching its stop
            views.push_back({r.startHz - 1.0e6, r.startHz});               // only touching its start
            views.push_back({std::nextafter(r.endHz, 0.0), r.endHz + 1.0});  // overlapping by a hair
        }
        for (int i = 0; i < 200; ++i) {
            const double lo = any(rng);
            views.push_back({lo, lo + std::uniform_real_distribution<double>(0.0, 2.0e8)(rng)});
        }
        for (const auto& [lo, hi] : views) {
            const std::vector<const cc::BandEntry*> want = plan.visible(lo, hi);
            std::vector<const BandRow*> got;
            for (const BandRow& r : rows) {
                if (cascade::gui::bandRowVisible(r, lo, hi)) { got.push_back(&r); }
            }
            bool same = want.size() == got.size() && cascade::gui::bandRowsVisible(rows, lo, hi) == want.size();
            for (std::size_t i = 0; same && i < want.size(); ++i) {
                // The same rows in the same order (entries() order is the draw order).
                same = want[i]->name == got[i]->name && want[i]->startHz == got[i]->startHz &&
                       want[i]->endHz == got[i]->endHz;
            }
            if (!same) { std::printf("      %s: visible(%.17g, %.17g) differs\n", label.c_str(), lo, hi); }
            CHECK(same);
            ++windows;
        }
    }
    std::printf("      %zu plans, %ld frequencies and %ld views held against BandPlan\n", loads.size(), frequencies,
                windows);
}

void testModeIndex() {
    std::printf("  A2: the mode key from demodMode\n");
    CHECK_EQ(cascade::gui::railModeIndex(0), 0);  // the zero state before the first read: NFM, never -1
    for (std::uint32_t d = 1; d <= 8; ++d) {
        CHECK_EQ(cascade::gui::railModeIndex(d), static_cast<int>(d) - 1);
        // ...the exact inverse of the engine's own mapping.
        CHECK_EQ(cascade::gui::abiDemodForModeIndex(cascade::gui::railModeIndex(d)), d);
    }
    CHECK_EQ(cascade::gui::railModeIndex(9), 7);
    CHECK_EQ(cascade::gui::railModeIndex(0xFFFFFFFFu), 7);
}

void testPlanNote() {
    std::printf("  A3: the Language section's note about the band plan\n");
    using cascade::gui::BandPlanAsk;
    using cascade::gui::bandPlanNoteDue;
    // Nothing asked: the note is due exactly when the plan in force is not the country's.
    BandPlanAsk none;
    CHECK(bandPlanNoteDue(none, "uk", "us", 10, false));
    CHECK(!bandPlanNoteDue(none, "uk", "uk", 10, false));
    // The frame the country was picked on: never (its plan is not applied yet).
    CHECK(!bandPlanNoteDue(none, "uk", "us", 10, true));
    // Asked for the UK on frame 10 while the US was in force: the state still shows
    // the US for a frame or two (the engine's pass has not run) - no note...
    const BandPlanAsk ask{"uk", "us", 10};
    CHECK(!bandPlanNoteDue(ask, "uk", "us", 11, false));
    CHECK(!bandPlanNoteDue(ask, "uk", "us", 10 + cascade::gui::kBandPlanLandFrames - 1, false));
    // ...it lands: the plan IS the country's, no note...
    CHECK(!bandPlanNoteDue(ask, "uk", "uk", 12, false));
    // ...the Display section picks another plan afterwards: the note, at once...
    CHECK(bandPlanNoteDue(ask, "uk", "japan", 12, false));
    // ...a request the engine never applied is still a note, in the end...
    CHECK(bandPlanNoteDue(ask, "uk", "us", 10 + cascade::gui::kBandPlanLandFrames, false));
    // ...and a DIFFERENT country picked since is not held by the earlier ask.
    CHECK(bandPlanNoteDue(ask, "japan", "us", 11, false));
}

// --- B -----------------------------------------------------------------------

void testCacheFollowsTheEngine() {
    std::printf("  B: the rail's cache follows the engine\n");
    AppWindow app;
    Access::usePlans(app);
    Access::look(app);
    const Access::Rail& r = Access::rail(app);

    // The plans: the engine's own choices, in its order, one of them ACTIVE.
    const auto& choices = Access::choices(app);
    CHECK(choices.size() > 5);
    CHECK_EQ(r.plans.size(), choices.size());
    for (std::size_t i = 0; i < choices.size() && i < r.plans.size(); ++i) {
        CHECK_EQ(r.plans[i].id, choices[i].id);
        CHECK_EQ(r.plans[i].name, choices[i].name);
    }
    CHECK(r.activePlan >= 0);
    CHECK_EQ(r.plans[static_cast<std::size_t>(r.activePlan)].id, Access::selection(app));
    CHECK_EQ(Access::selection(app), std::string("world"));
    // The loaded plan: its name is the engine's (not the ACTIVE entry's, once a
    // chain is loaded), and its rows are the engine's, row for row.
    CHECK_EQ(r.bandPlanName, Access::plan(app).name());
    CHECK_EQ(r.bands.size(), Access::plan(app).entries().size());
    CHECK(!r.bands.empty());
    for (std::size_t i = 0; i < r.bands.size(); ++i) {
        const cc::BandEntry& e = Access::plan(app).entries()[i];
        CHECK(r.bands[i].startHz == e.startHz && r.bands[i].endHz == e.endHz);
        CHECK(r.bands[i].name == e.name && r.bands[i].service == e.service);
    }

    // A country picks the UK: the same command the pickers send. The window sees
    // the chain's name and the UK's rows after the engine's pass.
    const std::uint32_t readsBefore = r.bandReads;
    Access::submit(app, cc::cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, "uk"));
    CHECK_EQ(Access::pass(app), 1);
    Access::look(app);
    (void)Access::rail(app);
    CHECK_EQ(r.plans[static_cast<std::size_t>(r.activePlan)].id, std::string("uk"));
    CHECK_EQ(r.bandPlanName, std::string("ITU Region 1 + United Kingdom"));
    CHECK(r.plans[static_cast<std::size_t>(r.activePlan)].name != r.bandPlanName);
    CHECK_EQ(r.bandPlanName, Access::plan(app).name());
    CHECK_EQ(r.bands.size(), Access::plan(app).entries().size());
    CHECK_EQ(r.bandReads, readsBefore + 1u);  // the plan changed: its rows were read again
    for (std::size_t i = 0; i < r.bands.size(); ++i) {
        CHECK(r.bands[i].name == Access::plan(app).entries()[i].name);
    }
    // No selection at all (the legacy setting): every plan merged, none ACTIVE.
    Access::submit(app, cc::cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, ""));
    CHECK_EQ(Access::pass(app), 1);
    Access::look(app);
    (void)Access::rail(app);
    CHECK_EQ(r.activePlan, -1);
    CHECK_EQ(r.bands.size(), Access::plan(app).entries().size());
    CHECK(r.bandPlanName.find(" + ") != std::string::npos);

    // The audio outputs: the engine's list, the one it has chosen ACTIVE.
    const auto& outs = Access::outputs(app);
    CHECK_EQ(r.sinks.size(), outs.size());
    for (std::size_t i = 0; i < outs.size() && i < r.sinks.size(); ++i) {
        CHECK_EQ(r.sinks[i].index, outs[i].index);
        CHECK(outs[i].name.compare(0, r.sinks[i].name.size(), r.sinks[i].name) == 0);
        CHECK(r.sinks[i].name.size() <= 95u);
        if (outs[i].name.size() <= 95u) { CHECK_EQ(r.sinks[i].name, outs[i].name); }
    }
    CHECK_EQ(r.activeSink, outs.empty() ? -1 : Access::outputIndex(app));
    std::printf("      %zu plans, %zu bands loaded, %zu audio outputs (active %d)\n", r.plans.size(), r.bands.size(),
                r.sinks.size(), r.activeSink);

    // The two status lines: said, whole, and taken down.
    CHECK_EQ(r.audioHealth, std::string());
    CHECK_EQ(r.bandPlanError, std::string());
    Access::setAudioHealth(app, "output device stopped and was restarted (1 time)");
    Access::setBandPlanError(app, "plan x: bad json");
    Access::look(app);
    (void)Access::rail(app);
    CHECK_EQ(r.audioHealth, std::string("output device stopped and was restarted (1 time)"));
    CHECK_EQ(r.bandPlanError, std::string("plan x: bad json"));
    Access::setAudioHealth(app, "");
    Access::setBandPlanError(app, "");
    Access::look(app);
    (void)Access::rail(app);
    CHECK_EQ(r.audioHealth, std::string());
    CHECK_EQ(r.bandPlanError, std::string());
}

// --- C -----------------------------------------------------------------------

void testRowsAreCopiedOnce() {
    std::printf("  C: a line that moves listSeq re-reads the small lists, not the rows\n");
    AppWindow app;
    Access::usePlans(app);
    Access::look(app);
    const Access::Rail& r = Access::rail(app);
    CHECK(!r.bands.empty());
    const std::uint32_t rows = r.bandReads;
    const std::uint32_t refreshes = r.refreshes;
    const std::uint64_t seq = r.listSeq;
    for (int i = 0; i < 25; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        (void)Access::rail(app);
    }
    std::printf("      listSeq %llu -> %llu; %u refreshes, rows read %u times\n", static_cast<unsigned long long>(seq),
                static_cast<unsigned long long>(r.listSeq), r.refreshes - refreshes, r.bandReads - rows);
    CHECK(r.listSeq != seq);
    CHECK_EQ(r.refreshes - refreshes, 25u);
    CHECK_EQ(r.bandReads, rows);
}

// --- D -----------------------------------------------------------------------

void testNothingAllocatesOnceSettled() {
    std::printf("  D: a settled frame's reads allocate nothing\n");
    AppWindow app;
    Access::usePlans(app);
    Access::look(app);
    (void)Access::rail(app);
    const double hz = 98.0e6;
    const long before = t_allocs;
    for (int i = 0; i < 1000; ++i) {
        const Access::Rail& r = Access::rail(app);
        (void)cascade::gui::bandRowAt(r.bands, hz);
        (void)cascade::gui::bandRowsVisible(r.bands, 90.0e6, 110.0e6);
    }
    const long spent = t_allocs - before;
    std::printf("      1000 reads of an unchanged state: %ld allocation(s)\n", spent);
    CHECK_EQ(spent, 0L);
    // A refresh with nothing different to say (a line, say) re-reads the small
    // lists into strings that already have the room.
    Access::aLineAppears(app);
    Access::look(app);
    (void)Access::rail(app);
    // What the rail's own refresh costs, counted around it alone (the engine's publish
    // and the session's read, which the window does not own, are outside the count).
    long inRefresh = 0;
    for (int i = 0; i < 100; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        const long mark = t_allocs;
        (void)Access::rail(app);
        inRefresh += t_allocs - mark;
    }
    // ...and what is left is the engine's, not the window's: a fact read through
    // get_setting allocates once inside the table (the text it returns), and the
    // refresh makes three such calls - so the window's own strings and vectors, which
    // keep their room, cost nothing.
    const long perFact = Access::factAllocations(app, "fact.status.audioHealth");
    std::printf("      100 refreshes for a decoder line: %ld allocation(s); one get_setting costs %ld, three a refresh\n",
                inRefresh, perFact);
    CHECK_EQ(inRefresh, 100L * 3L * perFact);
}

// --- G -----------------------------------------------------------------------

// The commands the eight members send, as they build them (each expression is the
// member's own: the port's review compared the old window's `engine_.submitCommand(...)`
// arguments with the ported `uiSubmit(...)` ones, 16 of 17 identical text and the 17th
// the same id from another source): through the window's session, accepted by the
// table's validation, applied by the engine, and read back in the state the rail draws.
void testTheCommandsReachTheEngine() {
    std::printf("  G: the controls' commands are accepted through the session and land in the state\n");
    namespace cmd = cascade::core::cmd;
    AppWindow app;
    Access::usePlans(app);
    Access::look(app);
    (void)Access::passStatuses(app);  // (whatever the constructor sent)

    // One command, one pass: the status the table answered, then the window's read of the result.
    const auto send = [&](const cmd::QueuedCommand& q, const char* what) {
        Access::submit(app, q);
        const std::vector<int32_t> got = Access::passStatuses(app);
        Access::look(app);
        const bool ok = got.size() == 1u && got[0] == FOXAPI_OK;
        if (!ok) { std::printf("      %s: answers %zu, first status %d\n", what, got.size(), got.empty() ? 99 : got[0]); }
        CHECK(ok);
    };
    const auto sendInt = [&](const FoxCommand& c, const char* what) { send(cmd::QueuedCommand{c, {}}, what); };
    const FoxReceiverState& st = Access::state(app);

    // drawRadioSection
    sendInt(cmd::makeInt(FOXAPI_OP_SET_MODE, 3 + 1), "SET_MODE");  // the DSB key: i = 3, and the member sends i + 1
    CHECK_EQ(st.demodMode, 4u);
    CHECK_EQ(cascade::gui::railModeIndex(st.demodMode), 3);
    sendInt(cmd::makeInt(FOXAPI_OP_SET_MODE, 1 + 1), "SET_MODE WFM");
    CHECK_EQ(cascade::gui::railModeIndex(st.demodMode), 1);
    const float vfoKhz = -125.0f;
    sendInt(cmd::makeNum(FOXAPP_OP_SET_VFO_OFFSET_FREE, 1000.0 * static_cast<double>(vfoKhz)), "SET_VFO_OFFSET_FREE");
    CHECK_EQ(st.vfoOffsetHz, -125000.0);
    CHECK_EQ(static_cast<float>(st.vfoOffsetHz / 1000.0), vfoKhz);  // what the slider will read back
    for (int i = 0; i < cascade::gui::kBwCount; ++i) {
        sendInt(cmd::makeInt(FOXAPP_OP_SET_BANDWIDTH_STEP, i), "SET_BANDWIDTH_STEP");
        CHECK_EQ(st.bandwidthHz, cascade::gui::kBwHz[i]);
        CHECK_EQ(cascade::gui::bandwidthStepIndex(st.bandwidthHz), i);  // the step the combo ticks
    }
    const float squelch = -83.0f;
    sendInt(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, squelch), "SET_SQUELCH");
    CHECK_EQ(static_cast<float>(st.squelchDb), squelch);
    sendInt(cmd::makeInt(FOXAPI_OP_SET_DEEMPHASIS, 1), "SET_DEEMPHASIS");
    CHECK_EQ(st.deemphasis, 1u);

    // drawStereoRdsControls
    sendInt(cmd::makeInt(FOXAPI_OP_SET_STEREO, 0), "SET_STEREO 0");
    CHECK((st.flags & FOXAPI_RX_STEREO_ENABLED) == 0u);
    sendInt(cmd::makeInt(FOXAPI_OP_SET_STEREO, 1), "SET_STEREO 1");
    CHECK((st.flags & FOXAPI_RX_STEREO_ENABLED) != 0u);

    // drawAudioFilterSection
    sendInt(cmd::makeInt(FOXAPI_OP_SET_NR, 1, 0), "SET_NR");
    CHECK((st.flags & FOXAPI_RX_NR) != 0u);
    const float nrStrength = 0.25f;
    sendInt(cmd::makeNum(FOXAPP_OP_SET_NR_STRENGTH, nrStrength), "SET_NR_STRENGTH");
    CHECK_EQ(static_cast<float>(st.nrStrength), nrStrength);
    sendInt(cmd::makeInt(FOXAPI_OP_SET_NOTCH, 1, 0), "SET_NOTCH");
    CHECK((st.flags & FOXAPI_RX_NOTCH) != 0u);
    const float notchHz = 1500.0f;
    sendInt(cmd::makeNum(FOXAPP_OP_SET_NOTCH_FREQUENCY, notchHz), "SET_NOTCH_FREQUENCY");
    CHECK_EQ(static_cast<float>(st.notchHz), notchHz);
    const float notchQ = 40.0f;
    sendInt(cmd::makeNum(FOXAPP_OP_SET_NOTCH_Q, notchQ), "SET_NOTCH_Q");
    CHECK_EQ(static_cast<float>(st.notchQ), notchQ);
    sendInt(cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, 1), "SET_AUTO_NOTCH");
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH) != 0u);
    sendInt(cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, 0), "SET_AUTO_NOTCH 0");
    sendInt(cmd::makeInt(FOXAPI_OP_SET_NOTCH, 0, 0), "SET_NOTCH 0");
    sendInt(cmd::makeInt(FOXAPI_OP_SET_NR, 0, 0), "SET_NR 0");
    CHECK((st.flags & (FOXAPI_RX_NR | FOXAPI_RX_NOTCH | FOXAPI_RX_AUTO_NOTCH)) == 0u);

    // drawDisplaySection
    const float dbMin = -130.0f;
    const float dbMax = -10.0f;
    sendInt(cmd::makeNum(FOXAPP_OP_SET_DISPLAY_MIN, dbMin), "SET_DISPLAY_MIN");
    sendInt(cmd::makeNum(FOXAPP_OP_SET_DISPLAY_MAX, dbMax), "SET_DISPLAY_MAX");
    CHECK_EQ(static_cast<float>(st.dbMin), dbMin);
    CHECK_EQ(static_cast<float>(st.dbMax), dbMax);
    // ...and the plan pickers' command, by the id the rail's cache holds.
    const Access::Rail& rail = Access::rail(app);
    std::size_t uk = rail.plans.size();
    for (std::size_t i = 0; i < rail.plans.size(); ++i) {
        if (rail.plans[i].id == "uk") { uk = i; }
    }
    CHECK(uk < rail.plans.size());
    if (uk < rail.plans.size()) {
        send(cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, rail.plans[uk].id), "SET_BAND_PLAN");
        const Access::Rail& after = Access::rail(app);  // (the window's next read: the lists moved)
        CHECK(after.activePlan >= 0);
        if (after.activePlan >= 0) { CHECK_EQ(after.plans[static_cast<std::size_t>(after.activePlan)].id, std::string("uk")); }
    }

    // (drawSinksSection's AUDIO_DEVICE, by the PortAudio index the cache keeps, is not sent:
    // it opens this machine's sound device. It is one of the 16 commands whose text is
    // unchanged, and tests/test_apply_command.cpp holds the engine's handling of it.)
    for (const Access::Rail::Sink& s : rail.sinks) { CHECK(s.index >= 0); }
}

// --- F -----------------------------------------------------------------------

// What a frame drew, as text: ImGui's own log of the text its widgets rendered.
template <class F>
std::string drawnText(F body) {
    std::string out;
    frame([&] {
        ImGui::LogToBuffer();
        body();
        // (read before LogFinish, which empties the buffer)
        out = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    });
    return out;
}

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

void testWhatIsDrawn() {
    std::printf("  F: what the members draw, from the state they read\n");
    AppWindow app;
    Access::usePlans(app);
    Access::look(app);

    // --- the WFM stereo and RDS controls: the readout the table's RDS members give ---
    {
        std::string t = drawnText([&] { Access::drawStereo(app); });
        CHECK(has(t, "Stereo"));
        CHECK(has(t, "MONO"));
        CHECK(has(t, "RDS: no data"));
        CHECK(!has(t, "pilot"));

        cc::RdsSnapshot heard;
        heard.synced = true;
        Access::rds(app, &heard);
        Access::look(app);
        t = drawnText([&] { Access::drawStereo(app); });
        CHECK(has(t, "RDS: syncing..."));
        CHECK(!has(t, "RDS: no data"));

        // Everything the decoder has recovered.
        heard.state.piValid = true;
        heard.state.pi = 0x2C41;
        heard.state.psValid = true;
        heard.state.ps = "CASCADE!";
        heard.state.radioText = "STEREO RDS TEST";
        heard.state.tp = true;
        heard.state.ta = true;
        heard.state.pty = 10;
        heard.state.groupsDecoded = 321;
        heard.state.blockErrors = 12;
        Access::rds(app, &heard);
        Access::look(app);
        t = drawnText([&] { Access::drawStereo(app); });
        CHECK(has(t, "PS  CASCADE!"));
        CHECK(has(t, "PI  2C41   PTY 10  TP  TA"));
        CHECK(has(t, "STEREO RDS TEST"));
        CHECK(has(t, "groups 321 | block errors 12"));
        CHECK(!has(t, "RDS: syncing...") && !has(t, "RDS: no data"));
        // A field not complete yet is dashes, and the codes alone have no TP/TA.
        heard.state.psValid = false;
        heard.state.ps.clear();
        heard.state.tp = heard.state.ta = false;
        heard.state.radioText.clear();
        Access::rds(app, &heard);
        Access::look(app);
        t = drawnText([&] { Access::drawStereo(app); });
        CHECK(has(t, "PS  --------"));
        CHECK(has(t, "PI  2C41   PTY 10") && !has(t, "TP") && !has(t, "TA"));
        heard.state.piValid = false;
        heard.state.psValid = true;
        heard.state.ps = "ABCDEFGH";
        Access::rds(app, &heard);
        Access::look(app);
        t = drawnText([&] { Access::drawStereo(app); });
        CHECK(has(t, "PS  ABCDEFGH"));
        CHECK(has(t, "PI  ----"));
        Access::rds(app, nullptr);

        // The pilot: locked with stereo not active is the forced-mono state (the
        // lock is the decoder's; the lamp is its AND the user's).
        Access::pilot(app, true);
        Access::look(app);
        t = drawnText([&] { Access::drawStereo(app); });
        CHECK(has(t, "ST (forced mono)"));
        CHECK(has(t, "pilot 0.00"));
        CHECK(!has(t, "MONO") || has(t, "ST (forced mono)"));
        Access::pilot(app, false);
        Access::look(app);
        t = drawnText([&] { Access::drawStereo(app); });
        CHECK(!has(t, "ST (forced mono)") && !has(t, "pilot"));
    }

    // --- the audio filters: the auto-notch's readout ---------------------------------
    {
        std::string t = drawnText([&] { Access::drawFilters(app); });
        CHECK(has(t, "Noise reduction") && has(t, "Notch") && has(t, "Auto notch"));
        CHECK(!has(t, "on ") && !has(t, "searching"));
        Access::command(app, cc::cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, 1));
        Access::look(app);
        t = drawnText([&] { Access::drawFilters(app); });
        CHECK(has(t, "searching"));
        Access::autoNotch(app, true, 1000.0);
        Access::look(app);
        t = drawnText([&] { Access::drawFilters(app); });
        CHECK(has(t, "on 1000 Hz"));
        CHECK(!has(t, "searching"));
        // Engaged with the filter switched off says nothing: the readout is the switch's.
        Access::command(app, cc::cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, 0));
        Access::look(app);
        t = drawnText([&] { Access::drawFilters(app); });
        CHECK(!has(t, "on 1000 Hz") && !has(t, "searching"));
    }

    // --- the Sinks section: the watchdog's note and who mutes ------------------------
    {
        std::string t = drawnText([&] { Access::drawSinks(app); });
        CHECK(has(t, "Device") || has(t, "No audio output devices"));
        CHECK(!has(t, "Muted by") && !has(t, "restarted"));
        Access::mutedBy(app, {"Alpha", "Beta", "Gamma"});
        Access::look(app);
        t = drawnText([&] { Access::drawSinks(app); });
        CHECK(has(t, "Muted by Alpha, Beta and Gamma"));
        CHECK(has(t, "sound returns when the plugin stops"));
        Access::mutedBy(app, {});
        Access::look(app);
        t = drawnText([&] { Access::drawSinks(app); });
        CHECK(!has(t, "Muted by"));
        Access::setAudioHealth(app, "output device stopped and was restarted (1 time)");
        Access::look(app);
        t = drawnText([&] { Access::drawSinks(app); });
        CHECK(has(t, "output device stopped and was restarted (1 time)"));
        Access::setAudioHealth(app, "");
        Access::look(app);
        t = drawnText([&] { Access::drawSinks(app); });
        CHECK(!has(t, "restarted"));
    }

    // --- the Display section: the loaded plan's caption, and its error ----------------
    {
        (void)Access::pass(app);  // (what the frames above sent: the audio filters' first sighting)
        const Access::Rail& r = Access::rail(app);
        std::string caption = Access::plan(app).name() + " (" + std::to_string(Access::plan(app).entries().size()) + " bands)";
        std::string t = drawnText([&] { Access::drawDisplay(app); });
        std::printf("      Display's caption: \"%s\"\n", caption.c_str());
        CHECK(has(t, caption));
        CHECK(r.bands.size() == Access::plan(app).entries().size());
        // A chain: the caption names the plan LOADED, not the one entry that is ACTIVE.
        Access::submit(app, cc::cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, "uk"));
        CHECK_EQ(Access::pass(app), 1);
        Access::look(app);
        caption = "ITU Region 1 + United Kingdom (" + std::to_string(Access::plan(app).entries().size()) + " bands)";
        t = drawnText([&] { Access::drawDisplay(app); });
        CHECK(has(t, caption));
        CHECK(!has(t, "United Kingdom (" + std::to_string(Access::plan(app).entries().size()) + " bands)") ||
              has(t, "ITU Region 1 + United Kingdom ("));
        // An error replaces the caption.
        Access::setBandPlanError(app, "uk.json: bad json");
        Access::look(app);
        t = drawnText([&] { Access::drawDisplay(app); });
        CHECK(has(t, "uk.json: bad json"));
        CHECK(!has(t, "bands)"));
        Access::setBandPlanError(app, "");
        Access::look(app);
    }
    // ...and with no plan installed at all, the section says so.
    {
        AppWindow bare;
        Access::look(bare);
        const std::string t = drawnText([&] { Access::drawDisplay(bare); });
        CHECK(has(t, "no band plan installed"));
    }

    // --- the Language section: the country's plan, and the note about it --------------
    {
        Access::country(app, "GB");
        Access::submit(app, cc::cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, "world"));
        CHECK_EQ(Access::pass(app), 1);
        Access::look(app);
        CHECK_EQ(Access::rail(app).plans[static_cast<std::size_t>(Access::rail(app).activePlan)].id, std::string("world"));
        // The country wants "uk" and the plan in force is "world": the line names the
        // country's plan, and the note says the Display section chose another.
        std::string t = drawnText([&] { Access::drawLanguage(app); });
        CHECK(has(t, "Band plan: United Kingdom"));
        CHECK(has(t, "Display has since been set to a different band plan."));
        // The plan it asked for is on its way: no note...
        Access::ask(app, cascade::gui::BandPlanAsk{"uk", "world", ImGui::GetFrameCount()});
        t = drawnText([&] { Access::drawLanguage(app); });
        CHECK(has(t, "Band plan: United Kingdom"));
        CHECK(!has(t, "Display has since been set"));
        // ...unless it never arrives...
        Access::ask(app, cascade::gui::BandPlanAsk{"uk", "world", ImGui::GetFrameCount() - cascade::gui::kBandPlanLandFrames});
        t = drawnText([&] { Access::drawLanguage(app); });
        CHECK(has(t, "Display has since been set to a different band plan."));
        // ...and once it has landed there is nothing to say.
        Access::ask(app, cascade::gui::BandPlanAsk{});
        Access::submit(app, cc::cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, "uk"));
        CHECK_EQ(Access::pass(app), 1);
        Access::look(app);
        t = drawnText([&] { Access::drawLanguage(app); });
        CHECK(has(t, "Band plan: United Kingdom"));
        CHECK(!has(t, "Display has since been set"));
        Access::country(app, "");
    }
}

// --- E -----------------------------------------------------------------------

void testDrawn() {
    std::printf("  E: the members drawn in real frames\n");
    AppWindow app;
    Access::usePlans(app);
    struct Member {
        const char* name;
        void (*draw)(AppWindow&);
    };
    const Member members[] = {
        {"drawRadioSection", &Access::drawRadio},
        {"drawStereoRdsControls", &Access::drawStereo},
        {"drawAudioFilterSection", &Access::drawFilters},
        {"drawSinksSection", &Access::drawSinks},
        {"drawDisplaySection", &Access::drawDisplay},
        {"drawLanguageSection", &Access::drawLanguage},
        {"drawBandPlanOverlay", &Access::drawOverlay},
    };
    // The zero state, every member in ONE frame, as the rail draws them.
    // (In the order the rail draws them: the stereo and RDS controls sit inside
    // the Radio section, before the audio filters' row.)
    std::printf("      all in one frame, before the first read\n");
    frame([&] {
        for (const Member& m : members) { m.draw(app); }
    });
    // Drawn before the window has ever looked (the zero state: NFM, nothing
    // running, no lists), then after it has, each in a frame of its own.
    for (int looked = 0; looked < 2; ++looked) {
        if (looked == 1) { Access::look(app); }
        for (const Member& m : members) {
            std::printf("      %s, %s\n", m.name, looked == 0 ? "before the first read" : "after it");
            for (int i = 0; i < 3; ++i) { frame([&] { m.draw(app); }); }
        }
    }
    // Drawing alone sent nothing but the audio filters' first sighting, which names
    // its panel to the usage report once (not a setting): the first pass answers
    // it, and three more frames of everything send nothing.
    const int firstSightings = Access::pass(app);
    for (int i = 0; i < 3; ++i) {
        frame([&] {
            for (const Member& m : members) { m.draw(app); }
        });
    }
    CHECK_EQ(Access::pass(app), 0);
    std::printf("      first sightings answered: %d; three more frames of all seven sent nothing\n", firstSightings);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_rail_reads\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_rail_reads_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // The audio filters' row starts closed; the capture hook opens it (read once).
    setEnv("FOXSDR_OPEN_SECTIONS", "Audio filters");
    setEnv("FOXSDR_OPEN_LANGUAGE", "1");  // ...and so does the Language section's

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testBandRulesAgainstTheModel();
    testModeIndex();
    testPlanNote();
    testCacheFollowsTheEngine();
    testRowsAreCopiedOnce();
    testNothingAllocatesOnceSettled();
    testTheCommandsReachTheEngine();
    testWhatIsDrawn();
    testDrawn();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_rail_reads");
}
