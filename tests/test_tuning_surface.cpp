// test_tuning_surface.cpp - THE TUNING SURFACE READS ITS ENGINE API SESSION
// (window port, group 6: drawToolbar, drawFrequencyReadout, applyKeyAction,
// drawCenterPanels and the bookmark markers' reader; engine-stage4.md 6.5, API 0.3
// section 8.5).
//
// What they draw is the table's: the state, a spectrum stream, two facts. This holds
// the window's side of that against the real engine, with no receiver started, no
// radio and no sound device (the spectrum comes from the engine's test seam,
// ApiHost::publishSpectrumForTest):
//   A  the pure rules the window restates, held against the model they restate: the
//      markers' binary search (core::FreqManager::range), the mute lamps (the
//      pipeline's own audioMuted), the typed frequency's range (what the table
//      answers at the edges), the decoders the waterfall's caption counts
//   B  the spectrum: one buffer, filled from the stream, follows the published
//      sequences exactly - one waterfall row for each new sequence, none for
//      NO_CHANGE, none twice, none skipped between reads; the buffer is sized once;
//      what a frame costs
//   C  the bookmark markers are copied when the store's version moves and not when
//      listSeq does, and are what the engine lists
//   D  the VFO drag's local copy is what the engine does with the command (every
//      edge of the limits), and it holds for exactly one pass
//   E  the typed entry: checked here as the table checks it, sent in the order that
//      matters, and a refusal the table alone judges appears on the red line a pass
//      later
//   F  the keys: every action sends the command it always sent (held against the old
//      expressions), a key held on every frame loses no step, and the keyboard is
//      dispatched after the frame's read of the state
//   G  every read the port replaced agrees with the read it replaced
//   H  a settled frame's reads allocate nothing, however many times a frame asks
//   I  the members drawn in real ImGui frames, which send nothing when idle
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/freq_manager.hpp"
#include "core/plugin_ui.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/key_bindings.hpp"
#include "gui/tuning_reads.hpp"
#include "gui/waterfall_view.hpp"
#include "imgui.h"
#include "imgui_internal.h"
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

using Clock = std::chrono::steady_clock;
double nsSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Tuning = AppWindow::TuningListCache;
    using Answer = std::pair<std::uint32_t, std::int32_t>;  // {op, status}
    using Sent = std::vector<cascade::core::cmd::QueuedCommand>;

    // The window looks, as a frame does: the engine publishes, the window reads (the state,
    // then the spectrum).
    static void look(AppWindow& a) {
        passseam::on(a.engine_, [&] { a.engine_.publishReceiverState(); });
        a.refreshUiState();
    }
    static void refresh(AppWindow& a) { a.refreshUiState(); }
    // The engine's part of one frame, in drawUi's order: the queue taken, the publish, the
    // status copy, the window's read of the state, the key when one is down (drawUi
    // dispatches the keyboard right after the read), the answers drained.
    static void frame(AppWindow& a, const KeyAction* key = nullptr) {
        passseam::custom(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            a.engine_.drainLocalCommands();
            (void)a.engine_.pumpPublish();
        });
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
        if (key != nullptr) { a.applyKeyAction(*key); }
        a.pumpTransmitKey();
    }
    // The pass as far as a submit needs it, and what the session answered, in order.
    static std::vector<Answer> answers(AppWindow& a) {
        passseam::pass(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            (void)a.engine_.pumpPublish();
        });
        std::vector<Answer> out;
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            for (int32_t i = 0; i < got; ++i) { out.push_back({r[i].op, r[i].status}); }
            if (got < 16) { break; }
        }
        return out;
    }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    // What the key checks leave queued is NOT applied: a key that starts the receiver or a take
    // would, on the next pass. The session is closed (it applies only what makes the transmitter
    // safer) and opened again, so the queue is empty and the next pass starts nothing.
    static void dropQueued(AppWindow& a) {
        a.closeUiSession();
        a.openUiSession();
        a.refreshUiState();
    }

    // The window's state and what it keeps.
    static FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static const Tuning& tuning(AppWindow& a) { return a.tuningLists(); }
    static const Tuning& cache(AppWindow& a) { return a.tuningCache_; }
    static const std::vector<float>& bins(AppWindow& a) { return a.lastFrame_.dbBins; }
    static std::uint64_t seq(AppWindow& a) { return a.lastFrame_.seq; }
    static std::uint64_t rows(AppWindow& a) { return a.waterfallLines_; }
    static int waterfallRows(AppWindow& a) { return a.waterfall_->filledRows(); }
    static bool arrived(AppWindow& a) { return a.spectrumArrived_; }
    static void readSpectrum(AppWindow& a) { a.readSpectrum(); }
    static void forgetFrame(AppWindow& a) { a.lastFrame_.seq = 0; }
    static std::int32_t scopeRead(AppWindow& a, float* out, std::uint32_t cap) {
        return a.engineApi_->read_scope(a.uiSession_, FOXAPI_SCOPE_AUDIO, nullptr, out, cap);
    }
    static std::int32_t rawRead(AppWindow& a, std::uint64_t since, FoxSpectrumInfo* info, float* out,
                                std::uint32_t cap) {
        return a.engineApi_->read_spectrum(a.uiSession_, since, info, out, cap);
    }
    static void publish(AppWindow& a, const std::vector<float>& bins, double centreHz, double spanHz) {
        passseam::on(a.engine_, [&] { a.engine_.api().publishSpectrumForTest(bins.data(), bins.size(), centreHz, spanHz); });
    }
    static const AppWindow::TuneLocal& local(AppWindow& a) { return a.tuneLocal_; }
    static void clock(AppWindow& a, double (*f)()) { a.uiClockS_ = f != nullptr ? f : &AppWindow::steadyNowS; }
    static double tunedOffset(AppWindow& a) { return a.tunedVfoOffsetHz(); }
    static double tunedBandwidth(AppWindow& a) { return a.tunedBandwidthHz(); }
    static void dragCentre(AppWindow& a, double wantAbsHz) { a.dragVfoCentre(wantAbsHz); }
    static void dragEdge(AppWindow& a, double mouseHz, double centreAbsHz) { a.dragVfoEdge(mouseHz, centreAbsHz); }
    static void commit(AppWindow& a, const char* text) { a.commitTypedFrequency(text); }
    static const std::string& refusal(AppWindow& a) { return a.freqEntryRefusal_; }
    static const std::string& redLine(AppWindow& a) { return a.statusLines_.sourceError; }
    static void key(AppWindow& a, KeyAction k) { a.applyKeyAction(k); }
    static void pumpKey(AppWindow& a) { a.pumpTransmitKey(); }
    static void drawToolbar(AppWindow& a) { a.drawToolbar(); }
    static void drawMarkers(AppWindow& a, float w, float h) { a.drawBookmarkMarkers(0.0f, 0.0f, w, h); }
    static void viewOf(AppWindow& a, double centreHz, double spanHz) { a.scale_.setSpan(centreHz, spanHz); }
    static double viewLow(AppWindow& a) { return a.scale_.viewLowHz(); }
    static double viewHigh(AppWindow& a) { return a.scale_.viewHighHz(); }
    // A change of the lists that touches nothing these tests read. A decoder line used to be it (it moved
    // listSeq once a line); since API 0.3 8.7 (D3) a line moves no list counter, so the preset note - a
    // status line the Decoders section letters - changes instead, and moves listSeq all the same.
    static void aLineAppears(AppWindow& a) {
        static unsigned n = 0;
        const std::string note = "note " + std::to_string(++n);
        passseam::on(a.engine_, [&] { a.engine_.presetNote_ = note; });
    }
    // The allocations one get_setting of a fact makes inside the table.
    static long factAllocations(AppWindow& a, const char* key) {
        char buf[512];
        const long mark = t_allocs;
        (void)a.engineApi_->get_setting(a.uiSession_, key, buf, sizeof(buf));
        return t_allocs - mark;
    }

    // The engine, for comparison with what the window reads of it (the reads the port replaced).
    struct Old {
        bool running, faulted, decoding, muteLamp, hardMuted;
        float volume, squelch;
        double tuned, sourceRate, bandwidth, channelRate, centre, offset, inputRate;
        int mode;
    };
    static Old oldReads(AppWindow& a) { return passseam::read<Old>(a.engine_, [&] { return oldReadsOnPass(a); }); }
    static Old oldReadsOnPass(AppWindow& a) {
        Engine& e = a.engine_;
        Old o{};
        o.running = e.pipeline_.running();
        o.faulted = e.pipeline_.faulted() || !e.testFault_.empty();
        o.decoding = o.running && e.pluginRunner_.activeCount() > 0;
        o.muteLamp = !e.muteSubjectText().empty() || e.userMuted_;
        o.hardMuted = e.pipeline_.audioMuted();
        o.volume = e.volume_;
        o.squelch = e.squelchDb_;
        o.tuned = e.currentAbsoluteHz();
        o.sourceRate = e.pipeline_.activeSource().sampleRateHz();
        o.bandwidth = e.vfoBandwidthHz_;
        o.channelRate = e.pipeline_.channelRateHz();
        o.centre = e.pipeline_.activeSource().centerFrequencyHz();
        o.offset = e.pipeline_.vfoOffsetHz();
        o.inputRate = e.pipeline_.inputRateHz();
        o.mode = e.modeIndex_;
        return o;
    }
    using Engine = cascade::engine::Engine;
    // Every engine field these read or write is the pass's: each goes through passseam::on/read (on the pass's own thread, with the
    // pass out of the way, when it free-runs; right here otherwise).
    static void setVolume(AppWindow& a, float v) { passseam::on(a.engine_, [&] { a.engine_.volume_ = v; }); }
    static void setSquelch(AppWindow& a, float v) { passseam::on(a.engine_, [&] { a.engine_.squelchDb_ = v; }); }
    static void setFault(AppWindow& a, const std::string& why) { passseam::on(a.engine_, [&] { a.engine_.testFault_ = why; }); }
    static bool setChainRate(AppWindow& a, double hz) {
        return passseam::read<bool>(a.engine_, [&] { return a.engine_.pipeline_.setInputRateHz(hz); });
    }
    static double engineOffset(AppWindow& a) {
        return passseam::read<double>(a.engine_, [&] { return a.engine_.pipeline_.vfoOffsetHz(); });
    }
    static double engineBandwidth(AppWindow& a) { return passseam::read<double>(a.engine_, [&] { return a.engine_.vfoBandwidthHz_; }); }
    static void command(AppWindow& a, const FoxCommand& c) { passseam::on(a.engine_, [&] { (void)a.engine_.applyCommand(c); }); }
    static void setSourceError(AppWindow& a, const std::string& s) { passseam::on(a.engine_, [&] { a.engine_.sourceError_ = s; }); }
    static void pumpMute(AppWindow& a) { passseam::mute(a.engine_, [&] { a.engine_.pumpAudioMute(); }); }
    static void muteBy(AppWindow& a, const std::string& name, double hz) { passseam::on(a.engine_, [&] { muteByOnPass(a, name, hz); }); }
    static void muteByOnPass(AppWindow& a, const std::string& name, double hz) {
        cascade::core::MutePlugin m;
        m.name = name;
        m.key = name + ".dll";
        m.running = true;
        m.mutes = true;
        cascade::core::MutePreset ps;
        ps.frequencyHz = hz;
        ps.label = name;
        m.presets.push_back(ps);
        a.engine_.muteStates_.clear();
        a.engine_.muteStates_.push_back(std::move(m));
    }
    static void noMuteBy(AppWindow& a) { passseam::on(a.engine_, [&] { a.engine_.muteStates_.clear(); }); }
    // The bookmark store, which the pass reads (the save, the lists): a change made in it, a COPY of its list, and its search.
    static void storeDo(AppWindow& a, const std::function<void(cascade::core::FreqManager&)>& change) {
        passseam::on(a.engine_, [&] { change(a.engine_.freqMgr_); });
    }
    static std::vector<cascade::core::Bookmark> storeList(AppWindow& a) {
        return passseam::read<std::vector<cascade::core::Bookmark>>(a.engine_, [&] { return a.engine_.freqMgr_.list(); });
    }
    static std::pair<std::size_t, std::size_t> storeRange(AppWindow& a, double lo, double hi) {
        return passseam::read<std::pair<std::size_t, std::size_t>>(a.engine_, [&] { return a.engine_.freqMgr_.range(lo, hi); });
    }

    // THE PASS HELD, free-running only: a piece of work posted to the pass (Engine::postToPass) that waits until the hold is let go.
    // A pass runs posted work at the head of its front part AFTER it has taken its queues (pumpFrameBegin: the local queue, the API's,
    // then runPassWork), so once the work has STARTED the pass that runs it has taken everything it will take, and no other pass can
    // start until it ends: what the window sends meanwhile is taken by the NEXT pass, after the release, and what it reads meanwhile is
    // known to PRECEDE that pass - which is what the P4 cases are about. Inline and stepped no pass runs unless the test asks for one,
    // so there is nothing to hold. Nothing may ask for a pass (on/read/pass/answers) while it is held: the window's own session reads
    // and submits only.
    struct Hold {
        std::mutex m;
        std::condition_variable cv;
        bool running = false;
        bool release = false;
    };
    static std::shared_ptr<Hold>& held() {
        static std::shared_ptr<Hold> h;
        return h;
    }
    static void holdPass(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free || held() != nullptr) { return; }
        passseam::attach(a.engine_);
        auto h = std::make_shared<Hold>();
        a.engine_.postToPass([h] {
            std::unique_lock<std::mutex> lk(h->m);
            h->running = true;
            h->cv.notify_all();
            h->cv.wait(lk, [&] { return h->release; });
        });
        std::unique_lock<std::mutex> lk(h->m);
        const bool took = h->cv.wait_for(lk, std::chrono::seconds(10), [&] { return h->running; });
        CHECK(took);
        held() = h;
    }
    static void releasePass(AppWindow&) {
        const std::shared_ptr<Hold> h = held();
        if (h == nullptr) { return; }
        held().reset();
        {
            std::lock_guard<std::mutex> lk(h->m);
            h->release = true;
        }
        h->cv.notify_all();
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::KeyAction;
using cascade::gui::MarkerRow;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

const std::string& textOf(const cmd::QueuedCommand& q, std::string& scratch) {
    scratch = cmd::textOf(q.c, q.longText);
    return scratch;
}

// One frame with `body` drawn inside a window; no engine pass.
template <class F>
void imframe(F body, float w = 1500.0f, float h = 400.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
    body();
    ImGui::End();
    ImGui::Render();
}

void fillFrame(std::vector<float>& f, int k) {
    for (std::size_t i = 0; i < f.size(); ++i) { f[i] = static_cast<float>(k) * 10.0f + static_cast<float>(i) * 0.001f; }
}

bool sameCommand(const FoxCommand& a, const FoxCommand& b) {
    return a.structSize == b.structSize && a.op == b.op && a.target == b.target && a.flags == b.flags &&
           a.ival[0] == b.ival[0] && a.ival[1] == b.ival[1] && a.num[0] == b.num[0] && a.num[1] == b.num[1] &&
           a.num[2] == b.num[2] && a.num[3] == b.num[3] && std::strcmp(a.text, b.text) == 0;
}

// --- A -----------------------------------------------------------------------

void testPureRules() {
    std::printf("  A1: the markers' search is core::FreqManager::range's\n");
    {
        std::mt19937_64 rng(20261014);
        long windows = 0;
        for (int round = 0; round < 6; ++round) {
            cc::FreqManager fm;
            const int n = round == 0 ? 0 : (round == 1 ? 1 : 40 + 300 * round);
            std::uniform_int_distribution<int> slot(0, 25);  // few distinct frequencies: many duplicates
            for (int i = 0; i < n; ++i) {
                cc::Bookmark b;
                b.name = "bm" + std::to_string(i);
                b.freqHz = 88.0e6 + slot(rng) * 0.1e6;
                fm.add(b);
            }
            std::vector<MarkerRow> rows;
            for (const cc::Bookmark& b : fm.list()) { rows.push_back({b.freqHz, b.favourite, b.name}); }
            std::vector<std::pair<double, double>> views = {
                {0.0, 0.0},       {88.0e6, 88.0e6}, {88.0e6, 90.5e6}, {90.5e6, 88.0e6},
                {1.0e9, 2.0e9},   {-1.0e9, 1.0e9},  {std::numeric_limits<double>::quiet_NaN(), 90.0e6},
                {90.0e6, std::numeric_limits<double>::quiet_NaN()},
                {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()}};
            for (const MarkerRow& r : rows) {
                for (const double e : {r.hz, std::nextafter(r.hz, 0.0), std::nextafter(r.hz, 1.0e18)}) {
                    views.push_back({e, e});
                    views.push_back({e, e + 0.25e6});
                    views.push_back({e - 0.25e6, e});
                }
            }
            std::uniform_real_distribution<double> any(87.0e6, 91.0e6);
            for (int i = 0; i < 300; ++i) {
                const double a = any(rng);
                views.push_back({a, a + std::uniform_real_distribution<double>(0.0, 1.0e6)(rng)});
            }
            for (const auto& v : views) {
                const auto want = fm.range(v.first, v.second);
                const auto got = cascade::gui::markerRange(rows, v.first, v.second);
                if (want != got) {
                    std::printf("      view [%.17g, %.17g]: wanted [%zu, %zu), got [%zu, %zu)\n", v.first, v.second,
                                want.first, want.second, got.first, got.second);
                }
                CHECK(want == got);
                ++windows;
            }
        }
        std::printf("      %ld views, equal\n", windows);
    }

    std::printf("  A2: the typed frequency's range is the table's\n");
    {
        AppWindow app;
        Access::look(app);
        (void)Access::answers(app);
        const double inf = std::numeric_limits<double>::infinity();
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<double> figures = {0.0,   std::nextafter(0.0, 1.0), 1.0e-9, 1.0,  1.0e6, 100.3e6, 5.999999999e9,
                                             6.0e9, std::nextafter(6.0e9, 1.0e18), 6.000001e9, 7.0e9, 1.0e12,
                                             -1.0,  -0.0,  nan,   inf, -inf};
        for (const double hz : figures) {
            Access::submit(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, hz));
            const auto got = Access::answers(app);
            CHECK_EQ(got.size(), 1u);
            const bool accepted = got.size() == 1u && got[0].second == FOXAPI_OK;
            const bool windowSays = cascade::gui::typedFrequencyTunable(hz);
            if (accepted != windowSays) { std::printf("      %.17g: table %d, window %d\n", hz, accepted, windowSays); }
            CHECK(accepted == windowSays);
        }
        std::printf("      %zu figures, the table and the window agree on each\n", figures.size());
    }

    std::printf("  A3: the mute lamps are the pipeline's\n");
    {
        AppWindow app;
        Access::look(app);
        const auto agree = [&](const char* what) {
            Access::pumpMute(app);   // the pass decides the mute, after the publish...
            Access::look(app);       // ...and the next publish carries it
            const auto& st = Access::state(app);
            const auto old = Access::oldReads(app);
            const bool lamp = cascade::gui::masterMuteLamp(st);
            const bool hard = cascade::gui::audioHardMuted(st);
            if (lamp != old.muteLamp || hard != old.hardMuted) {
                std::printf("      %s: lamp %d (was %d), hard %d (pipeline %d)\n", what, lamp, old.muteLamp, hard,
                            old.hardMuted);
            }
            CHECK(lamp == old.muteLamp);
            CHECK(hard == old.hardMuted);
        };
        agree("nothing");
        CHECK(!cascade::gui::masterMuteLamp(Access::state(app)));
        Access::command(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1));
        agree("the user's mute");
        CHECK(cascade::gui::masterMuteLamp(Access::state(app)));
        Access::command(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 0));
        agree("unmuted");
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 1090.0e6));
        Access::muteBy(app, "Alpha", 1090.0e6);
        agree("a plugin on its preset");
        CHECK(cascade::gui::masterMuteLamp(Access::state(app)));
        CHECK(std::string(Access::state(app).audioMutedBy) == "Alpha");
        Access::command(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1));
        agree("both");
        Access::command(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 0));
        Access::noMuteBy(app);
        agree("none again");
        // The third cause the meter's hard mute has: a keyed transmitter that is not monitored.
        // (Engine::updateAudioMute: transmitter_.transmitting() && !transmitMonitor_; the
        // published flags are TX_KEYED and, in txFlags, TX_MONITOR.)
        FoxReceiverState st{};
        CHECK(!cascade::gui::audioHardMuted(st));
        st.flags = FOXAPI_RX_TX_KEYED;
        CHECK(cascade::gui::audioHardMuted(st));
        CHECK(!cascade::gui::masterMuteLamp(st));  // (the lamp is the user's and the plugins')
        st.txFlags = FOXAPI_TX_MONITOR;
        CHECK(!cascade::gui::audioHardMuted(st));
        st.flags = FOXAPI_RX_MUTED;
        CHECK(cascade::gui::audioHardMuted(st) && cascade::gui::masterMuteLamp(st));
        st.flags = 0;
        std::snprintf(st.audioMutedBy, sizeof(st.audioMutedBy), "%s", "X");
        CHECK(cascade::gui::audioHardMuted(st) && cascade::gui::masterMuteLamp(st));
    }

    std::printf("  A4: the decoders the caption counts\n");
    {
        cascade::gui::FedDecoders f;
        f.add(FOXAPI_ITEM_ACTIVE, "idle one");  // running but not fed
        CHECK_EQ(f.count, 0);
        CHECK(f.first.empty());
        f.add(FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FED, "ADS-B");
        CHECK_EQ(f.count, 1);
        CHECK_EQ(f.first, std::string("ADS-B"));
        f.add(FOXAPI_ITEM_FED, "AIS");
        f.add(0u, "not fed");
        CHECK_EQ(f.count, 2);
        CHECK_EQ(f.first, std::string("ADS-B"));  // the first one fed, not the last
        f.clear();
        CHECK_EQ(f.count, 0);
        CHECK(f.first.empty());
    }
}

// --- B -----------------------------------------------------------------------

void testSpectrumFollowsTheSequences() {
    std::printf("  B: the spectrum buffer follows the published sequences\n");
    AppWindow app;
    Access::look(app);
    CHECK_EQ(Access::rows(app), 0u);
    CHECK(Access::bins(app).empty());
    for (int i = 0; i < 5; ++i) { Access::look(app); }
    CHECK_EQ(Access::rows(app), 0u);  // NO_CHANGE, five times: nothing fed

    std::vector<float> frame(1024);
    std::uint64_t published = 0;
    const float* data = nullptr;
    for (int k = 1; k <= 40; ++k) {
        fillFrame(frame, k);
        Access::publish(app, frame, 100.3e6, 2.0e6);
        ++published;
        Access::look(app);
        CHECK_EQ(Access::rows(app), published);
        CHECK_EQ(Access::seq(app), published);
        CHECK(Access::bins(app) == frame);
        CHECK(Access::arrived(app));
        if (k == 1) { data = Access::bins(app).data(); }
        const std::uint64_t seen = Access::rows(app);
        Access::look(app);  // again, nothing new: nothing fed, the frame stands
        CHECK_EQ(Access::rows(app), seen);
        CHECK(Access::bins(app) == frame);
    }
    CHECK(Access::bins(app).data() == data);  // the buffer was sized once
    CHECK_EQ(Access::waterfallRows(app), 40);
    std::printf("      40 frames, one look each: %llu waterfall rows for %llu sequences; the buffer never moved\n",
                static_cast<unsigned long long>(Access::rows(app)), static_cast<unsigned long long>(published));

    // Three frames published between two reads: ONE row, for the newest - the frame the window
    // never read is skipped, as getLatestFrame skipped it.
    for (int round = 0; round < 10; ++round) {
        for (int j = 0; j < 3; ++j) {
            fillFrame(frame, 100 + round * 3 + j);
            Access::publish(app, frame, 100.3e6, 2.0e6);
            ++published;
        }
        const std::uint64_t before = Access::rows(app);
        Access::look(app);
        CHECK_EQ(Access::rows(app), before + 1u);
        CHECK_EQ(Access::seq(app), published);
        CHECK(Access::bins(app) == frame);
    }
    CHECK_EQ(Access::waterfallRows(app), 50);

    // The engine's bin count is the buffer's size, and a different count is the only thing
    // that resizes it.
    for (const std::size_t n : {std::size_t{2048}, std::size_t{512}, std::size_t{16384}, std::size_t{1024}}) {
        std::vector<float> odd(n);
        fillFrame(odd, 7);
        Access::publish(app, odd, 100.3e6, 2.0e6);
        Access::look(app);
        CHECK_EQ(Access::bins(app).size(), n);
        CHECK(Access::bins(app) == odd);
    }

    // Allocations: a read of a new frame into a buffer already that size, and a read that
    // finds nothing, allocate nothing in the window.
    {
        long spent = 0;
        long frames = 0;
        for (int k = 0; k < 200; ++k) {
            fillFrame(frame, 500 + k);
            Access::publish(app, frame, 100.3e6, 2.0e6);
            const long mark = t_allocs;
            Access::refresh(app);
            spent += t_allocs - mark;
            ++frames;
        }
        for (int k = 0; k < 200; ++k) {
            const long mark = t_allocs;
            Access::refresh(app);
            spent += t_allocs - mark;
            ++frames;
        }
        std::printf("      %ld refreshes (half with a new frame, half without): %ld allocation(s)\n", frames, spent);
        CHECK_EQ(spent, 0L);
    }

    // What a frame costs: the copy out of the stream, and the whole of the window's read with
    // the waterfall's row. (Measured, not asserted: the machine decides the figure.)
    {
        fillFrame(frame, 900);
        Access::publish(app, frame, 100.3e6, 2.0e6);
        std::vector<float> dst(1024);
        FoxSpectrumInfo info{};
        info.structSize = sizeof(info);
        const int N = 20000;
        Clock::time_point t0 = Clock::now();
        for (int i = 0; i < N; ++i) { (void)Access::rawRead(app, 0, &info, dst.data(), 1024); }
        const double copyNs = nsSince(t0) / N;
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            Access::forgetFrame(app);
            Access::readSpectrum(app);
        }
        const double readNs = nsSince(t0) / N;
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) { Access::refresh(app); }
        const double idleNs = nsSince(t0) / N;
        std::printf("      per frame, 1024 bins (4096 bytes): read_spectrum copy %.0f ns; readSpectrum with the "
                    "waterfall row %.0f ns; a whole refresh with nothing new %.0f ns\n",
                    copyNs, readNs, idleNs);
    }
}

// --- C -----------------------------------------------------------------------

void testMarkersAreCopiedWhenTheStoreChanges() {
    std::printf("  C: the markers are copied when the store changes, not when listSeq moves\n");
    AppWindow app;
    Access::look(app);
    const auto& c = Access::tuning(app);
    CHECK(c.markers.empty());
    const std::uint32_t copies0 = c.markerCopies;
    CHECK_EQ(copies0, 1u);  // the first sight of the store

    // Lines a decoder prints move listSeq - the rail's cache re-reads its small lists on each -
    // and the store's version not at all.
    const std::uint64_t seq0 = c.listSeq;
    const std::uint32_t refreshes0 = c.refreshes;
    for (int i = 0; i < 25; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        (void)Access::tuning(app);
    }
    std::printf("      25 decoder lines: listSeq %llu -> %llu, %u refreshes, rows copied %u times\n",
                static_cast<unsigned long long>(seq0), static_cast<unsigned long long>(c.listSeq),
                c.refreshes - refreshes0, c.markerCopies - copies0);
    CHECK(c.listSeq != seq0);
    CHECK_EQ(c.refreshes - refreshes0, 25u);
    CHECK_EQ(c.markerCopies, copies0);

    // A store of two hundred, long names among them, a few starred.
    std::mt19937_64 rng(20261014);
    for (int i = 0; i < 200; ++i) {
        cc::Bookmark b;
        b.name = (i % 17 == 0) ? std::string(120, static_cast<char>('a' + i % 26)) + std::to_string(i)
                               : "Station " + std::to_string(i);
        b.freqHz = std::uniform_real_distribution<double>(80.0e6, 110.0e6)(rng);
        b.favourite = (i % 9 == 0);
        Access::storeDo(app, [&](cc::FreqManager& fm) { fm.add(b); });
    }
    Access::look(app);
    (void)Access::tuning(app);
    CHECK_EQ(c.markerCopies, copies0 + 1u);
    std::vector<cc::Bookmark> list = Access::storeList(app);  // (a copy, read again after each change below)
    CHECK_EQ(c.markers.size(), list.size());
    bool same = c.markers.size() == list.size();
    for (std::size_t i = 0; same && i < list.size(); ++i) {
        const std::string cut = list[i].name.substr(0, 95);  // the list item's name[96]
        same = c.markers[i].hz == list[i].freqHz && c.markers[i].favourite == list[i].favourite &&
               c.markers[i].name == cut;
    }
    CHECK(same);
    bool sorted = true;
    for (std::size_t i = 1; i < c.markers.size(); ++i) { sorted = sorted && c.markers[i - 1].hz <= c.markers[i].hz; }
    CHECK(sorted);

    // The draw's search over the copy is the model's over the store, on windows of the span.
    long windows = 0;
    for (int i = 0; i < 500; ++i) {
        const double lo = std::uniform_real_distribution<double>(79.0e6, 111.0e6)(rng);
        const double hi = lo + std::uniform_real_distribution<double>(0.0, 8.0e6)(rng);
        CHECK(cascade::gui::markerRange(c.markers, lo, hi) == Access::storeRange(app, lo, hi));
        ++windows;
    }

    // A star keeps the list's size and changes the store: copied again. A remove too.
    {
        cc::Bookmark b = list[10];
        b.favourite = !b.favourite;
        Access::storeDo(app, [&](cc::FreqManager& fm) { fm.updateAt(10, b); });
        list = Access::storeList(app);
        Access::look(app);
        (void)Access::tuning(app);
        CHECK_EQ(c.markerCopies, copies0 + 2u);
        CHECK(c.markers.size() > 10u);  // (never index a cache a failed copy left short)
        if (c.markers.size() > 10u) { CHECK_EQ(c.markers[10].favourite, b.favourite); }
        Access::storeDo(app, [&](cc::FreqManager& fm) { fm.removeAt(0); });
        list = Access::storeList(app);
        Access::look(app);
        (void)Access::tuning(app);
        CHECK_EQ(c.markerCopies, copies0 + 3u);
        CHECK_EQ(c.markers.size(), list.size());
    }
    // ...and a burst of decoder lines after it copies nothing more.
    for (int i = 0; i < 10; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        (void)Access::tuning(app);
    }
    CHECK_EQ(c.markerCopies, copies0 + 3u);
    std::printf("      200 bookmarks copied once, then once for a star and once for a remove; %ld windows searched\n",
                windows);

    // The cost of the one copy of a big import (measured).
    {
        std::vector<cc::Bookmark> many;
        many.reserve(33000);
        for (int i = 0; i < 33000; ++i) {
            cc::Bookmark b;
            b.name = "Channel " + std::to_string(i);
            b.freqHz = 100.0e6 + 1000.0 * i;
            many.push_back(std::move(b));
        }
        Access::storeDo(app, [&](cc::FreqManager& fm) { fm.addMany(std::move(many)); });
        Access::look(app);
        const Clock::time_point t0 = Clock::now();
        (void)Access::tuning(app);
        const double ms = nsSince(t0) / 1.0e6;
        std::printf("      a 33 000-entry import: %zu rows copied in %.1f ms, once\n", c.markers.size(), ms);
        CHECK_EQ(c.markerCopies, copies0 + 4u);
        CHECK(c.markers.size() > 32000u);
    }
}

// --- D -----------------------------------------------------------------------

void testTheDragsLocalCopyIsWhatTheEngineDoes() {
    std::printf("  D: the drag's local copy is what the engine does with the command, and holds one pass\n");
    AppWindow app;
    Access::look(app);
    (void)Access::answers(app);
    int checked = 0;
    const double rates[] = {2.0e6, 1.0e6, 250000.0, 2.4e6};
    for (const double chain : rates) {
        CHECK(Access::setChainRate(app, chain));
        Access::look(app);
        (void)Access::answers(app);
        CHECK_EQ(Access::tuning(app).inputRateHz, chain);  // the cache has the chain's rate
        for (int step = 0; step < cascade::gui::kBwCount; ++step) {
            Access::command(app, cmd::makeInt(FOXAPP_OP_SET_BANDWIDTH_STEP, step));
            Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, 0.0));
            Access::look(app);
            const double centre = Access::state(app).centreHz;
            for (const double d : {-1.0e12, -9.0e6, -3.0e6, -1.2e6, -2.0e5, -1.0, 0.0, 3.3e4, 2.5e5, 1.1e6, 5.0e6, 1.0e12}) {
                Access::dragCentre(app, centre + d);
                const double local = Access::tunedOffset(app);
                const auto got = Access::answers(app);
                const bool accepted = got.size() == 1u && got[0].first == FOXAPI_OP_SET_VFO_OFFSET && got[0].second == FOXAPI_OK;
                if (!accepted) { std::printf("      chain %.0f step %d d %.3g: not accepted\n", chain, step, d); }
                CHECK(accepted);
                if (Access::engineOffset(app) != local) {
                    std::printf("      chain %.0f step %d d %.3g: local %.17g, engine %.17g\n", chain, step, d, local,
                                Access::engineOffset(app));
                }
                CHECK_EQ(Access::engineOffset(app), local);
                Access::look(app);
                CHECK_EQ(Access::state(app).vfoOffsetHz, local);
                ++checked;
            }
            const double bandCentre = centre + Access::state(app).vfoOffsetHz;
            const double channel = Access::state(app).channelRateHz;
            for (const double m : {0.0, 1.0e3, 1.4e3, 1.5e3, 1.6e3, 2.0e4, 9.0e4, 0.45 * channel, 0.5 * channel, 1.0e6, 1.0e12}) {
                Access::dragEdge(app, bandCentre + m, bandCentre);
                const double local = Access::tunedBandwidth(app);
                const auto got = Access::answers(app);
                CHECK(got.size() == 1u && got[0].first == FOXAPP_OP_SET_BANDWIDTH_DRAG && got[0].second == FOXAPI_OK);
                if (Access::engineBandwidth(app) != local) {
                    std::printf("      chain %.0f step %d m %.3g: local %.17g, engine %.17g\n", chain, step, m, local,
                                Access::engineBandwidth(app));
                }
                CHECK_EQ(Access::engineBandwidth(app), local);
                Access::look(app);
                CHECK_EQ(Access::state(app).bandwidthHz, local);
                ++checked;
            }
        }
    }
    std::printf("      %d drag steps, over four chain rates and every bandwidth step: the local copy equals the engine's result\n",
                checked);

    // ONE PASS: used until the next read of the state, then the state's.
    Access::look(app);
    (void)Access::answers(app);
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, 0.0));
    Access::look(app);
    CHECK(!Access::local(app).offset.valid && !Access::local(app).bandwidth.valid);
    CHECK_EQ(Access::tunedOffset(app), 0.0);
    Access::dragCentre(app, Access::state(app).centreHz + 100000.0);
    CHECK(Access::local(app).offset.valid);
    CHECK_EQ(Access::tunedOffset(app), 100000.0);
    CHECK_EQ(Access::state(app).vfoOffsetHz, 0.0);  // the state has not seen it yet
    (void)Access::answers(app);                     // the engine applies it...
    CHECK_EQ(Access::tunedOffset(app), 100000.0);   // ...the copy still stands until the window reads again
    Access::look(app);
    CHECK(!Access::local(app).offset.valid);
    CHECK_EQ(Access::tunedOffset(app), Access::state(app).vfoOffsetHz);
    CHECK_EQ(Access::state(app).vfoOffsetHz, 100000.0);
    // A change made some other way after that is what is read: a copy that outlived its pass
    // would still say 100000.
    Access::command(app, cmd::makeNum(FOXAPP_OP_SET_VFO_OFFSET_FREE, 25000.0));
    Access::look(app);
    CHECK_EQ(Access::tunedOffset(app), 25000.0);
    // The bandwidth likewise.
    Access::dragEdge(app, Access::state(app).centreHz + 25000.0 + 40000.0, Access::state(app).centreHz + 25000.0);
    CHECK(Access::local(app).bandwidth.valid);
    CHECK_EQ(Access::tunedBandwidth(app), 80000.0);
    (void)Access::answers(app);
    Access::look(app);
    CHECK(!Access::local(app).bandwidth.valid);
    CHECK_EQ(Access::tunedBandwidth(app), Access::state(app).bandwidthHz);
    CHECK_EQ(Access::state(app).bandwidthHz, 80000.0);

    // =============================================================================================================================
    // THE COPY OUTLIVES A READ THAT PRECEDES THE PASS (the pump move's step P4; the survey's 1.6 A). With the pass on a thread of its own
    // the window can read the state between a drag's submit and the pass that lands it. The copy used to go on EVERY successful read
    // ("one pass, by which time the state carries the command"), so the passband snapped back for a frame and the next drag step
    // started from the old figure. It goes when the readback EQUALS it, or after kTuneLocalHoldS (gui/tune_local.hpp) - the rule the
    // radar scope's gain knob has.
    // =============================================================================================================================
    {
        static double s_now = 1000.0;
        Access::clock(app, +[] { return s_now; });
        Access::look(app);
        (void)Access::answers(app);
        Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, 0.0));
        Access::look(app);
        CHECK(!Access::local(app).offset.valid);
        Access::holdPass(app);  // (free-running: no pass of the thread's own may land the drag before the reads below)
        Access::dragCentre(app, Access::state(app).centreHz + 60000.0);
        CHECK(Access::local(app).offset.valid);
        // A READ BEFORE THE PASS: the state still says 0, and the window must go on showing the figure it sent (and take its next
        // step from it).
        Access::refresh(app);
        CHECK_EQ(Access::state(app).vfoOffsetHz, 0.0);
        CHECK(Access::local(app).offset.valid);
        CHECK_EQ(Access::tunedOffset(app), 60000.0);
        s_now += 0.2;
        Access::refresh(app);
        CHECK(Access::local(app).offset.valid);              // 0.2 s on, the readback still differs: the hold stands
        CHECK_EQ(Access::tunedOffset(app), 60000.0);
        Access::releasePass(app);
        // THE PASS LANDS IT: the next read shows the same figure, and the copy goes.
        (void)Access::answers(app);
        Access::look(app);
        CHECK_EQ(Access::state(app).vfoOffsetHz, 60000.0);
        CHECK(!Access::local(app).offset.valid);
        CHECK_EQ(Access::tunedOffset(app), 60000.0);
        // THE HOLD RUNS OUT when the engine took something else (a refusal, a limit the window does not know): the readback is the
        // truth. Send a figure, let the engine land a DIFFERENT one, and read.
        Access::dragCentre(app, Access::state(app).centreHz + 90000.0);
        CHECK_EQ(Access::tunedOffset(app), 90000.0);
        Access::submit(app, cmd::makeNum(FOXAPP_OP_SET_VFO_OFFSET_FREE, 5000.0));    // someone else tunes right after (the session's order)
        (void)Access::answers(app);
        s_now += 0.3;
        Access::look(app);
        CHECK_EQ(Access::state(app).vfoOffsetHz, 5000.0);
        CHECK(Access::local(app).offset.valid);              // 0.3 s since the send: the figure still stands...
        s_now += cascade::gui::kTuneLocalHoldS;
        Access::refresh(app);
        CHECK(!Access::local(app).offset.valid);              // ...and 0.8 s on it does not
        CHECK_EQ(Access::tunedOffset(app), 5000.0);
        // The bandwidth, the same.
        Access::holdPass(app);
        Access::dragEdge(app, Access::state(app).centreHz + 5000.0 + 30000.0, Access::state(app).centreHz + 5000.0);
        Access::refresh(app);
        CHECK(Access::local(app).bandwidth.valid);
        CHECK_EQ(Access::tunedBandwidth(app), 60000.0);
        Access::releasePass(app);
        (void)Access::answers(app);
        Access::look(app);
        CHECK(!Access::local(app).bandwidth.valid);
        CHECK_EQ(Access::tunedBandwidth(app), 60000.0);
        Access::clock(app, nullptr);
        std::printf("      a read before the pass keeps the sent figure; the pass landing it, or %.1f s, releases it\n", cascade::gui::kTuneLocalHoldS);
    }
}

// --- E -----------------------------------------------------------------------

void testTheTypedEntry() {
    std::printf("  E: the typed entry\n");
    AppWindow app;
    Access::look(app);
    (void)Access::answers(app);
    Access::Sent sent;
    Access::tap(app, &sent);
    std::string scratch;

    // A good figure: the old line cleared FIRST, then the tune, in that order; the tune lands.
    Access::setSourceError(app, "a radio error from before");
    Access::frame(app);
    CHECK_EQ(Access::redLine(app), std::string("a radio error from before"));
    Access::commit(app, "101.5");
    CHECK_EQ(sent.size(), 2u);
    if (sent.size() == 2u) {
        CHECK(sent[0].c.op == FOXAPP_OP_SET_SOURCE_ERROR && textOf(sent[0], scratch).empty());
        CHECK(sent[1].c.op == FOXAPI_OP_SET_FREQUENCY && sent[1].c.num[0] == 101.5e6);
    }
    const auto got = Access::answers(app);
    CHECK_EQ(got.size(), 2u);
    if (got.size() == 2u) {
        CHECK(got[0].first == FOXAPP_OP_SET_SOURCE_ERROR && got[0].second == FOXAPI_OK);
        CHECK(got[1].first == FOXAPI_OP_SET_FREQUENCY && got[1].second == FOXAPI_OK);
    }
    Access::frame(app);
    CHECK_EQ(Access::state(app).tunedHz, 101.5e6);
    CHECK(Access::redLine(app).empty());

    // What cannot be read, and what the table would refuse: one sentence on the red line and
    // no tune sent.
    for (const char* text : {"abc", "", "0", "7000 MHz", "6000.001", "-5", "1e400"}) {
        sent.clear();
        Access::commit(app, text);
        std::string want;
        want = std::string("could not read frequency \"") + text + "\"";
        const bool one = sent.size() == 1u && sent[0].c.op == FOXAPP_OP_SET_SOURCE_ERROR;
        if (!one) { std::printf("      \"%s\": %zu commands sent\n", text, sent.size()); }
        CHECK(one);
        if (one) { CHECK_EQ(textOf(sent[0], scratch), want); }
        Access::frame(app);
        CHECK_EQ(Access::redLine(app), want);
    }
    // ...and the edges that are fine.
    for (const char* text : {"6000", "0.000001", "100,300,000", "7.1 MHz", "433920k"}) {
        sent.clear();
        Access::commit(app, text);
        const bool two = sent.size() == 2u && sent[0].c.op == FOXAPP_OP_SET_SOURCE_ERROR &&
                         sent[1].c.op == FOXAPI_OP_SET_FREQUENCY;
        if (!two) { std::printf("      \"%s\": %zu commands sent\n", text, sent.size()); }
        CHECK(two);
        (void)Access::answers(app);
    }
    Access::frame(app);
    CHECK(Access::redLine(app).empty());

    // THE LINE'S TIMING, which the old code's comment got wrong. The line is drawn from the status
    // copy drawUi takes at the top of a frame, so an engine write made DURING a frame - the old
    // direct applyCommand - showed on the next frame; a session command made during a frame
    // lands at the top of the next one, before that copy: the same frame.
    Access::setSourceError(app, "");
    Access::frame(app);
    Access::command(app, cmd::makeText(FOXAPP_OP_SET_SOURCE_ERROR, "direct").c);  // (the old way, mid-frame)
    CHECK(Access::redLine(app).empty());                                          // not on this frame's copy
    Access::frame(app);
    CHECK_EQ(Access::redLine(app), std::string("direct"));                         // on the next
    sent.clear();
    Access::commit(app, "nonsense");
    CHECK(Access::redLine(app) == "direct");                                       // not on this frame's copy
    Access::frame(app);
    CHECK_EQ(Access::redLine(app), std::string("could not read frequency \"nonsense\""));  // on the next: the same frame

    // A REFUSAL ONLY THE TABLE JUDGES (a figure the window never sends - here a NaN): the answer
    // is read from the session by the frame's pumpTransmitKey into a slot, the readout puts it on
    // the red line, and the line shows it one pass after that.
    sent.clear();
    CHECK(Access::refusal(app).empty());
    Access::submit(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, std::numeric_limits<double>::quiet_NaN()));
    CHECK(Access::refusal(app).empty());      // sent, not answered yet
    Access::frame(app);                       // the pass answers it; pumpTransmitKey reads the answer
    std::printf("      the table's refusal of a NaN tune, kept for the readout: \"%s\"\n", Access::refusal(app).c_str());
    CHECK(!Access::refusal(app).empty());
    CHECK(Access::refusal(app).find("malformed") != std::string::npos);
    sent.clear();
    imframe([&] { Access::drawToolbar(app); });  // the readout says it
    CHECK(Access::refusal(app).empty());
    const bool posted = sent.size() == 1u && sent[0].c.op == FOXAPP_OP_SET_SOURCE_ERROR;
    CHECK(posted);
    std::string refused;
    if (posted) { refused = textOf(sent[0], scratch); }
    CHECK(refused.find("malformed") != std::string::npos);
    Access::frame(app);
    CHECK_EQ(Access::redLine(app), refused);
    // Said once: further frames of the readout send nothing.
    sent.clear();
    for (int i = 0; i < 3; ++i) { imframe([&] { Access::drawToolbar(app); }); }
    CHECK(sent.empty());
    // A NO_CHANGE or OK answer is not a refusal.
    Access::submit(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 100.3e6));
    Access::frame(app);
    CHECK(Access::refusal(app).empty());
    Access::tap(app, nullptr);
}

// --- F -----------------------------------------------------------------------

void testTheKeys() {
    std::printf("  F: the keys\n");
    AppWindow app;
    Access::look(app);
    (void)Access::answers(app);
    Access::Sent sent;
    Access::tap(app, &sent);
    // (Free-running: the keys below reach the session, and a start or a take must not be APPLIED - see dropQueued. Held until it
    // has dropped them; inline and stepped no pass runs here.)
    Access::holdPass(app);
    FoxReceiverState& st = Access::state(app);

    const auto one = [&](KeyAction k) -> FoxCommand {
        sent.clear();
        Access::key(app, k);
        if (sent.size() != 1u) {
            std::printf("      key %d: %zu commands\n", static_cast<int>(k), sent.size());
            CHECK(sent.size() == 1u);
            return FoxCommand{};
        }
        CHECK(sent[0].longText.empty());
        return sent[0].c;
    };
    const auto expect = [&](KeyAction k, const FoxCommand& want, const char* what) {
        const FoxCommand got = one(k);
        if (!sameCommand(got, want)) {
            std::printf("      %s: op %x ival %lld,%lld num %.17g vs op %x ival %lld,%lld num %.17g\n", what, got.op,
                        static_cast<long long>(got.ival[0]), static_cast<long long>(got.ival[1]), got.num[0], want.op,
                        static_cast<long long>(want.ival[0]), static_cast<long long>(want.ival[1]), want.num[0]);
        }
        CHECK(sameCommand(got, want));
    };

    // The toggles read the state's flags (the old code the engine's live ones).
    st.flags &= ~(FOXAPI_RX_RUNNING | FOXAPI_RX_MUTED | FOXAPI_RX_RECORDING_AUDIO);
    expect(KeyAction::StartStop, cmd::makeInt(FOXAPI_OP_RUN, 1), "start");
    st.flags |= FOXAPI_RX_RUNNING;
    expect(KeyAction::StartStop, cmd::makeInt(FOXAPI_OP_RUN, 0), "stop");
    expect(KeyAction::Mute, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1), "mute");
    st.flags |= FOXAPI_RX_MUTED;
    expect(KeyAction::Mute, cmd::makeInt(FOXAPI_OP_SET_MUTED, 0), "unmute");
    expect(KeyAction::Record, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1), "record");
    st.flags |= FOXAPI_RX_RECORDING_AUDIO;
    expect(KeyAction::Record, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 0), "stop recording");
    // (A plugin muting the audio is not the user's mute: the key toggles the user's.)
    st.flags &= ~FOXAPI_RX_MUTED;
    std::snprintf(st.audioMutedBy, sizeof(st.audioMutedBy), "%s", "Alpha");
    expect(KeyAction::Mute, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1), "mute while a plugin mutes");
    st.audioMutedBy[0] = '\0';

    // The eight modes: FOXAPI_DEMOD_* in the order of the keys.
    const std::pair<KeyAction, std::uint32_t> modes[] = {
        {KeyAction::ModeNFM, FOXAPI_DEMOD_NFM}, {KeyAction::ModeWFM, FOXAPI_DEMOD_WFM},
        {KeyAction::ModeAM, FOXAPI_DEMOD_AM},   {KeyAction::ModeDSB, FOXAPI_DEMOD_DSB},
        {KeyAction::ModeUSB, FOXAPI_DEMOD_USB}, {KeyAction::ModeCW, FOXAPI_DEMOD_CW},
        {KeyAction::ModeLSB, FOXAPI_DEMOD_LSB}, {KeyAction::ModeRAW, FOXAPI_DEMOD_RAW}};
    for (const auto& m : modes) { expect(m.first, cmd::makeInt(FOXAPI_OP_SET_MODE, m.second), "mode"); }

    // The steps: the tuning key's digit (cell 5, 10 kHz), and the visible span.
    {
        FoxCommand up = cmd::makeInt(FOXAPI_OP_STEP_TUNE, 1);
        up.num[0] = cascade::gui::digitPlaceHz(5);
        FoxCommand dn = cmd::makeInt(FOXAPI_OP_STEP_TUNE, -1);
        dn.num[0] = cascade::gui::digitPlaceHz(5);
        expect(KeyAction::TuneStepUp, up, "tune step up");
        expect(KeyAction::TuneStepDown, dn, "tune step down");
        sent.clear();
        Access::key(app, KeyAction::TuneSpanUp);  // no span known yet: nothing sent
        CHECK(sent.empty());
        Access::viewOf(app, 100.0e6, 2.0e6);
        const double span = Access::viewHigh(app) - Access::viewLow(app);
        CHECK(span > 0.0);
        FoxCommand su = cmd::makeInt(FOXAPI_OP_STEP_TUNE, 1);
        su.num[0] = span;
        FoxCommand sd = cmd::makeInt(FOXAPI_OP_STEP_TUNE, -1);
        sd.num[0] = span;
        expect(KeyAction::TuneSpanUp, su, "span up");
        expect(KeyAction::TuneSpanDown, sd, "span down");
    }
    // Free-running, what those keys queued goes before the pass runs again (the volume steps below need passes: their settings are
    // the pass's, and what they send is a volume or a squelch, which each next setting overwrites); inline and stepped it is still
    // queued, unapplied, until the dropQueued below.
    if (passseam::mode() == passseam::Mode::Free) { Access::dropQueued(app); }
    Access::releasePass(app);

    // The volume and squelch steps, against the old expressions (the engine's float, plus the step,
    // clamped) on floats of every kind: the state publishes the decimal a float was set to, and the
    // cast back must be the float.
    {
        std::mt19937_64 rng(20261014);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::uniform_real_distribution<float> db(-120.0f, 0.0f);
        std::vector<float> volumes = {0.0f, 1.0f, 0.05f, 0.35f, 0.5f, 0.95f, 0.97f, 0.99999994f, 1.0e-7f};
        std::vector<float> squelches = {-120.0f, -119.0f, 0.0f, -1.0f, -83.0f, -0.5f, -118.5f, -119.9f};
        for (int i = 0; i < 1500; ++i) {
            volumes.push_back(unit(rng));
            squelches.push_back(db(rng));
        }
        long bad = 0;
        for (const float v : volumes) {
            Access::setVolume(app, v);
            Access::look(app);
            for (const KeyAction k : {KeyAction::VolumeUp, KeyAction::VolumeDown}) {
                const float step = (k == KeyAction::VolumeUp) ? 0.05f : -0.05f;
                const FoxCommand got = one(k);
                const FoxCommand want = cmd::makeNum(FOXAPI_OP_SET_VOLUME, std::clamp(v + step, 0.0f, 1.0f));
                if (!sameCommand(got, want)) { ++bad; }
            }
        }
        for (const float q : squelches) {
            Access::setSquelch(app, q);
            Access::look(app);
            for (const KeyAction k : {KeyAction::SquelchUp, KeyAction::SquelchDown}) {
                const float step = (k == KeyAction::SquelchUp) ? 2.0f : -2.0f;
                const FoxCommand got = one(k);
                const FoxCommand want = cmd::makeNum(FOXAPI_OP_SET_SQUELCH, std::clamp(q + step, -120.0f, 0.0f));
                if (!sameCommand(got, want)) { ++bad; }
            }
        }
        std::printf("      %zu volumes and %zu squelch settings, up and down: %ld commands differ from the old formula\n",
                    volumes.size(), squelches.size(), bad);
        CHECK_EQ(bad, 0L);
    }

    Access::dropQueued(app);
    Access::tap(app, nullptr);
    // A KEY HELD DOWN loses no step, even repeating on every frame (what a low frame rate does):
    // each repeat starts from what the one before it did. The frame is drawUi's order - the pass,
    // the read of the state, then the keyboard.
    {
        Access::tap(app, nullptr);
        Access::setVolume(app, 0.3f);
        Access::frame(app);
        float expected = 0.3f;
        const KeyAction up = KeyAction::VolumeUp;
        for (int i = 0; i < 8; ++i) {
            Access::frame(app, &up);
            expected = std::clamp(expected + 0.05f, 0.0f, 1.0f);
        }
        Access::frame(app);  // the last command lands
        std::printf("      eight volume-up repeats, one a frame: volume %.9g, wanted %.9g\n",
                    static_cast<double>(Access::oldReads(app).volume), static_cast<double>(expected));
        CHECK_EQ(Access::oldReads(app).volume, expected);
        // The toggles likewise: a mute key pressed on three consecutive frames toggles three times.
        const KeyAction mute = KeyAction::Mute;
        Access::frame(app, &mute);
        Access::frame(app, &mute);
        Access::frame(app, &mute);
        Access::frame(app);
        CHECK(Access::oldReads(app).muteLamp);  // on, off, on
    }

    // WHERE THE KEYBOARD STANDS in the frame, read from the source: after the frame's read of the
    // state (refreshUiState) and before anything is drawn (drawToolbar). A key dispatched before
    // the read would start every repeat from a state a pass older.
    {
        std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/gui/app_window.cpp", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        const std::size_t body = text.find("void AppWindow::drawUi() {");
        CHECK(body != std::string::npos);
        const std::size_t end = body == std::string::npos ? body : text.find("\n}", body);  // (column 0: the function's end)
        const std::string drawUi = (body == std::string::npos || end == std::string::npos) ? std::string()
                                                                                         : text.substr(body, end - body);
        std::vector<std::size_t> dispatch, refreshAt, toolbar;
        for (std::size_t p = drawUi.find("dispatchKeyBindings();"); p != std::string::npos; p = drawUi.find("dispatchKeyBindings();", p + 1)) { dispatch.push_back(p); }
        for (std::size_t p = drawUi.find("\n    refreshUiState();"); p != std::string::npos; p = drawUi.find("\n    refreshUiState();", p + 1)) { refreshAt.push_back(p); }
        for (std::size_t p = drawUi.find("\n    drawToolbar();"); p != std::string::npos; p = drawUi.find("\n    drawToolbar();", p + 1)) { toolbar.push_back(p); }
        CHECK_EQ(dispatch.size(), 1u);
        CHECK_EQ(refreshAt.size(), 1u);
        CHECK_EQ(toolbar.size(), 1u);
        if (dispatch.size() == 1u && refreshAt.size() == 1u && toolbar.size() == 1u) {
            CHECK(refreshAt[0] < dispatch[0]);
            CHECK(dispatch[0] < toolbar[0]);
        }
        // ...and drawUi polls the pipeline for no frame any more: one read_spectrum, in
        // refreshUiState's readSpectrum, serves every view.
        CHECK(drawUi.find("getLatestFrame") == std::string::npos);
        std::size_t reads = 0;
        for (std::size_t p = text.find("read_spectrum("); p != std::string::npos; p = text.find("read_spectrum(", p + 1)) { ++reads; }
        const std::size_t rs = text.find("void AppWindow::readSpectrum() {");
        const std::size_t rsEnd = rs == std::string::npos ? rs : text.find("\n}", rs);
        std::size_t inside = 0;
        if (rs != std::string::npos && rsEnd != std::string::npos) {
            for (std::size_t p = text.find("read_spectrum(", rs); p != std::string::npos && p < rsEnd; p = text.find("read_spectrum(", p + 1)) { ++inside; }
        }
        std::printf("      read_spectrum( appears %zu times in app_window.cpp, %zu of them inside readSpectrum\n", reads, inside);
        CHECK(inside >= 1u);
        CHECK_EQ(reads, inside);
        CHECK(text.find("pipeline_.getLatestFrame(") == std::string::npos);
    }
    Access::tap(app, nullptr);
}

// --- G -----------------------------------------------------------------------

void testReplacedReadsAgree() {
    std::printf("  G: every read the port replaced agrees with the read it replaced\n");
    AppWindow app;
    Access::look(app);
    long compared = 0;
    const auto compare = [&](const char* what) {
        Access::look(app);
        const auto& st = Access::state(app);
        const auto o = Access::oldReads(app);
        const bool ok = ((st.flags & FOXAPI_RX_RUNNING) != 0u) == o.running &&
                        ((st.flags & FOXAPI_RX_FAULTED) != 0u) == o.faulted &&
                        ((st.flags & FOXAPI_RX_DECODER_ACTIVE) != 0u) == o.decoding &&
                        cascade::gui::masterMuteLamp(st) == o.muteLamp &&
                        static_cast<float>(st.volume) == o.volume && static_cast<float>(st.squelchDb) == o.squelch &&
                        st.tunedHz == o.tuned && st.sampleRateHz == o.sourceRate && st.bandwidthHz == o.bandwidth &&
                        st.channelRateHz == o.channelRate && st.centreHz == o.centre && st.vfoOffsetHz == o.offset &&
                        cascade::gui::railModeIndex(st.demodMode) == o.mode;
        if (!ok) { std::printf("      %s: a read differs\n", what); }
        CHECK(ok);
        CHECK_EQ(Access::tuning(app).inputRateHz, o.inputRate);
        ++compared;
    };
    compare("at rest");
    for (int mode = 1; mode <= 8; ++mode) {
        Access::command(app, cmd::makeInt(FOXAPI_OP_SET_MODE, mode));
        compare("a mode");
    }
    Access::setFault(app, "a worker died");
    compare("faulted");
    Access::setFault(app, "");
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_FREQUENCY, 433.92e6));
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_VFO_OFFSET, -12500.0));
    compare("tuned");
    Access::command(app, cmd::makeNum(FOXAPI_OP_SET_BANDWIDTH, 18000.0));
    compare("bandwidth");
    std::mt19937_64 rng(20261014);
    for (int i = 0; i < 300; ++i) {
        Access::setVolume(app, std::uniform_real_distribution<float>(0.0f, 1.0f)(rng));
        Access::setSquelch(app, std::uniform_real_distribution<float>(-120.0f, 0.0f)(rng));
        compare("random settings");
    }
    CHECK(Access::setChainRate(app, 1.0e6));
    compare("the chain on another rate than the source");
    CHECK(Access::tuning(app).inputRateHz != Access::state(app).sampleRateHz);
    std::printf("      %ld comparisons\n", compared);
}

// --- H -----------------------------------------------------------------------

void testNothingAllocatesOnceSettled() {
    std::printf("  H: a settled frame's reads allocate nothing\n");
    AppWindow app;
    Access::look(app);
    (void)Access::tuning(app);
    const long before = t_allocs;
    double sink = 0.0;
    for (int i = 0; i < 1000; ++i) {
        const auto& c = Access::tuning(app);
        sink += c.inputRateHz + static_cast<double>(c.fed.count);
        sink += Access::tunedOffset(app) + Access::tunedBandwidth(app);
        sink += static_cast<double>(cascade::gui::markerRange(c.markers, 90.0e6, 110.0e6).second);
        sink += cascade::gui::masterMuteLamp(Access::state(app)) ? 1.0 : 0.0;
        sink += cascade::gui::audioHardMuted(Access::state(app)) ? 1.0 : 0.0;
    }
    const long spent = t_allocs - before;
    std::printf("      1000 reads of an unchanged state: %ld allocation(s)%s\n", spent, sink < 0.0 ? "!" : "");
    CHECK_EQ(spent, 0L);
    // The toolbar's volume meter reads the audio tap through the table each frame: what that costs
    // (measured; nothing has been written to the tap, so it copies no samples).
    {
        static float window[2048];
        const int N = 20000;
        const Clock::time_point t0 = Clock::now();
        std::int32_t got = 0;
        for (int i = 0; i < N; ++i) { got += Access::scopeRead(app, window, 2048); }
        std::printf("      read_scope(audio, 2048 frames) %.0f ns a call (%d frames copied: the tap is empty)\n",
                    nsSince(t0) / N, got);
        CHECK_EQ(got, 0);
    }
    // A refresh for a line a decoder printed re-reads the small lists into strings that already
    // have the room: only the engine's own get_setting allocations remain (a short fact fits the
    // string's own buffer; the figures are counted around the window's call alone).
    Access::aLineAppears(app);
    Access::look(app);
    (void)Access::tuning(app);
    long inRefresh = 0;
    for (int i = 0; i < 100; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        const long mark = t_allocs;
        (void)Access::tuning(app);
        inRefresh += t_allocs - mark;
    }
    // ...what is left is the engine's, not the window's: each fact read through get_setting costs
    // what it costs inside the table (the key's temporary string, when the key is longer than the
    // string's own buffer), and a refresh reads two - so the window's own strings and vectors,
    // which keep their room, cost nothing.
    const long perRefresh = Access::factAllocations(app, "fact.input.rateHz") +
                            Access::factAllocations(app, "fact.bookmarks.version");
    std::printf("      100 refreshes for a decoder line: %ld allocation(s); the two get_setting calls cost %ld a "
                "refresh inside the table; the rows copied %u time(s) in all\n",
                inRefresh, perRefresh, Access::cache(app).markerCopies);
    CHECK_EQ(inRefresh, 100L * perRefresh);
    CHECK_EQ(Access::cache(app).markerCopies, 1u);
}

// --- I -----------------------------------------------------------------------

void testDrawn() {
    std::printf("  I: the members drawn in real frames\n");
    AppWindow app;
    Access::Sent sent;
    Access::tap(app, &sent);
    // Before the first read (the zero state), then after it.
    for (int looked = 0; looked < 2; ++looked) {
        if (looked == 1) { Access::look(app); }
        for (int i = 0; i < 3; ++i) { imframe([&] { Access::drawToolbar(app); }); }
    }
    // The markers: nothing in view draws nothing; a bookmark in view draws.
    Access::look(app);
    Access::viewOf(app, 100.0e6, 2.0e6);
    int vertsEmpty = 0;
    imframe([&] {
        const int mark = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        Access::drawMarkers(app, 900.0f, 300.0f);
        vertsEmpty = ImGui::GetWindowDrawList()->VtxBuffer.Size - mark;
    });
    CHECK_EQ(vertsEmpty, 0);
    cc::Bookmark b;
    b.name = "In view";
    b.freqHz = 100.3e6;
    b.favourite = true;
    Access::storeDo(app, [&](cc::FreqManager& fm) { fm.add(b); });
    Access::look(app);
    int vertsOne = 0;
    imframe([&] {
        const int mark = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        Access::drawMarkers(app, 900.0f, 300.0f);
        vertsOne = ImGui::GetWindowDrawList()->VtxBuffer.Size - mark;
    });
    std::printf("      the marker for one bookmark in view: %d vertices (none with an empty store)\n", vertsOne);
    CHECK(vertsOne > 0);
    // A bookmark out of view draws nothing more.
    Access::viewOf(app, 200.0e6, 2.0e6);
    int vertsOut = 0;
    imframe([&] {
        const int mark = ImGui::GetWindowDrawList()->VtxBuffer.Size;
        Access::drawMarkers(app, 900.0f, 300.0f);
        vertsOut = ImGui::GetWindowDrawList()->VtxBuffer.Size - mark;
    });
    CHECK_EQ(vertsOut, 0);
    // Drawing sent nothing, and an engine pass has nothing to answer.
    CHECK(sent.empty());
    CHECK_EQ(Access::answers(app).size(), 0u);
    std::printf("      the toolbar (the readout inside it) and the markers sent nothing in nine frames\n");
    Access::tap(app, nullptr);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_tuning_surface\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_tuning_surface_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testPureRules();
    testSpectrumFollowsTheSequences();
    testMarkersAreCopiedWhenTheStoreChanges();
    testTheDragsLocalCopyIsWhatTheEngineDoes();
    testTheTypedEntry();
    testTheKeys();
    testReplacedReadsAgree();
    testNothingAllocatesOnceSettled();
    testDrawn();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_tuning_surface");
}
