// test_recorder_bookmarks_scanner.cpp - THE RECORDER, THE BOOKMARKS AND THE SCANNER READ THEIR ENGINE
// API SESSION (window port, group 7: drawRecorderSection, drawBookmarksSection, rebuildBookmarkView,
// drawScannerSection and commitScannerDraft; engine-stage4.md 6.5, API 0.3 section 8.6).
//
// What the three sections draw is the table's: the state's flags and its scanner members, the
// RECORDINGS list read live, one copy of BOOKMARKS and two status lines. This holds the window's side
// of that against the real engine, with no receiver started, no radio and no sound device:
//   A  the pure rules the window restates, held against the model they restate: the bookmark view's
//      filter (the rule rebuildBookmarkView had) over a 10 000-entry list, the group names (against
//      a std::map and against what BOOKMARK_GROUPS answers), the samples a take's bytes make (against
//      the real Recorder), the scanner form's values and the commands its edits send (against the
//      old expressions, for every combination of edited fields)
//   B  the window's ONE copy of BOOKMARKS follows fact.bookmarks.version and not listSeq: a decoder
//      line copies nothing and rebuilds nothing, an add, a star and a remove copy once each, a
//      33 000-entry import once; each row whole; what the copy costs
//   C  the view and the groups are derived from that copy: they follow it, a chosen group stays
//      chosen by name and is dropped when its group goes, and a view nothing moved costs nothing
//   D  the scanner's readout and form follow the engine through all four states, field by field
//   E  the recorder: the take's bytes and seconds are live (listSeq stands still), the samples are
//      the recorder's own count, and the note under an audio take is the engine's mute subject
//   F  a settled window's reads allocate nothing, 1000 times over
//   G  the members drawn in real ImGui frames send nothing when idle; and every widget of each
//      section CLICKED sends exactly the commands it should, and no widget sends any other
//   H  each command a section sends, as the section built it (captured from G), accepted through
//      the window's session and applied: the state the sections read back moves
//   I  the scanner's pending-edit flush stands after the frame's read of the state in drawUi
//      (read from the source)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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
#include "core/recorder.hpp"
#include "core/scanner.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "gui/section_reads.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // LogToBuffer, GImGui->HoveredId
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
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::filesystem::path g_scratch;

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Tuning = AppWindow::TuningListCache;
    using Answer = std::pair<std::uint32_t, std::int32_t>;  // {op, status}
    using Sent = std::vector<cascade::core::cmd::QueuedCommand>;
    using Engine = cascade::engine::Engine;

    // One frame's engine part, in drawUi's order: the session's queue taken (a command submitted
    // since lands here), the scanner's frame when asked, the publish, the status copy, the window's
    // read of the state.
    // A RECORDING'S FILE OPENS ON A WORKER (0.99.63): a take just asked for is armed on the pass that collects the file. Waits (bounded) until no
    // start is out, polling as the pass does, so a test that asked for a take then reads it "recording".
    static bool recordStartsOut(AppWindow& a) {
        return passseam::read<bool>(rawEngine(a.engine_), [&] {
            return rawEngine(a.engine_).iqStart_.pending() || rawEngine(a.engine_).audioStart_.pending() || rawEngine(a.engine_).iqStartQueued_ ||
                   rawEngine(a.engine_).audioStartQueued_;
        });
    }
    static void settleRecordStarts(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (recordStartsOut(a) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            passseam::custom(rawEngine(a.engine_), [&] { rawEngine(a.engine_).pollRecordStarts(rawEngine(a.engine_).clockS()); });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static void lookOnce(AppWindow& a, bool scannerTick) {
        passseam::custom(rawEngine(a.engine_), [&] {
            rawEngine(a.engine_).pumpFrameBegin();
            if (scannerTick) { rawEngine(a.engine_).scannerFrame(); }
            (void)rawEngine(a.engine_).pumpPublish();
        });
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    static void look(AppWindow& a, bool scannerTick = false) {
        lookOnce(a, scannerTick);
        // The file of a take this look just asked for opens on a worker: wait for it, then look again, so the take is armed and published
        // (the product's next pass does the same).
        if (recordStartsOut(a)) {
            settleRecordStarts(a);
            lookOnce(a, false);
        }
    }
    // The answers the session has been given since it was last asked, in order.
    static std::vector<Answer> answers(AppWindow& a) {
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
    static void submit(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    // What a click left queued is NOT applied: the session is closed and opened again, so the
    // queue is empty and the next pass applies nothing of it.
    static void dropQueued(AppWindow& a) {
        a.closeUiSession();
        a.openUiSession();
        a.refreshUiState();
    }

    // The window's state and what it keeps.
    static FoxReceiverState& state(AppWindow& a) { return a.uiState_; }
    static const Tuning& tuning(AppWindow& a) { return a.tuningLists(); }
    static const Tuning& cache(AppWindow& a) { return a.tuningCache_; }
    static const AppWindow::TakeNow take(AppWindow& a, int index, double kind) { return a.readTake(index, kind); }
    static int takeIq(AppWindow& a) { return a.statusTakeIq_; }
    static int takeAudio(AppWindow& a) { return a.statusTakeAudio_; }
    static void refresh(AppWindow& a) { a.refreshUiState(); }
    static void refreshStatus(AppWindow& a) { a.refreshStatusLists(); }
    static const std::string& fact(AppWindow& a, const char* key) { return a.uiFact(key); }
    static std::string& note(AppWindow& a) { return a.statusLines_.bookmarkNote; }
    static std::string& errorLine(AppWindow& a) { return a.statusLines_.bookmarkError; }

    // The bookmark view.
    static void rebuild(AppWindow& a) { a.rebuildBookmarkView(); }
    static std::vector<std::uint32_t>& view(AppWindow& a) { return a.bookmarkView_; }
    static const std::vector<std::string>& groups(AppWindow& a) { return a.bookmarkGroups_; }
    static int& groupSel(AppWindow& a) { return a.bookmarkGroupSel_; }
    static bool& favOnly(AppWindow& a) { return a.bookmarkFavOnly_; }
    static char* filter(AppWindow& a) { return a.bookmarkFilter_; }
    static char* importPath(AppWindow& a) { return a.bookmarkImportPath_; }
    static char* nameField(AppWindow& a) { return a.bookmarkName_; }
    static std::uint32_t viewCopies(AppWindow& a) { return a.bookmarkViewCopies_; }
    static bool& markersOn(AppWindow& a) { return a.bookmarkMarkers_; }

    // The scanner form.
    static double& draft(AppWindow& a, int i) { return a.scanDraft_[i]; }
    static unsigned& pending(AppWindow& a) { return a.scanDraftPending_; }
    static void commit(AppWindow& a, unsigned bits, bool reconfigure) { a.commitScannerDraft(bits, reconfigure); }
    static void flush(AppWindow& a) { a.flushScannerDraft(); }
    // A file dropped on the window: the glfw callback records the path, the frame takes it.
    static void drop(AppWindow& a, const std::string& path) {
        a.pendingDropPath_ = path;
        a.submitPendingDrop();
    }
    static const std::string& pendingDrop(AppWindow& a) { return a.pendingDropPath_; }

    // The members, drawn.
    static void drawRecorder(AppWindow& a) { a.drawRecorderSection(); }
    static void drawBookmarks(AppWindow& a) { a.drawBookmarksSection(); }
    static void drawScanner(AppWindow& a) { a.drawScannerSection(); }

    // The engine, for comparison and for what a hermetic window cannot make (a signal, a sample flow). EVERY REACH is made on the
    // pass's thread with the pass out of the way (passseam::read / on; the pump move's P6) - the store's list as a COPY, the
    // recorders fed there (a Recorder's writes must never overlap the pass's start or stop of it) - or, the Held forms, from this
    // thread while a PassHold parks the pass.
    static void storeDo(AppWindow& a, const std::function<void(cascade::core::FreqManager&)>& fn) {
        passseam::on(rawEngine(a.engine_), [&] { fn(rawEngine(a.engine_).freqMgr_); });
    }
    static std::vector<cascade::core::Bookmark> storeList(AppWindow& a) {
        return passseam::read<std::vector<cascade::core::Bookmark>>(
            rawEngine(a.engine_), [&]() -> std::vector<cascade::core::Bookmark> { return rawEngine(a.engine_).freqMgr_.list(); });
    }
    static std::size_t storeSizeHeld(AppWindow& a) { return rawEngine(a.engine_).freqMgr_.list().size(); }
    static void writeAudio(AppWindow& a, const float* s, std::size_t n) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).audioRecorder_.writeAudio(s, n); });
    }
    static void writeIq(AppWindow& a, const std::complex<float>* s, std::size_t n) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).iqRecorder_.writeIq(s, n); });
    }
    static void setSquelch(AppWindow& a, float db) { passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).squelchDb_ = db; }); }
    // Where the scanner is, and then the squelch set - one step on the pass (free-running, a pass between the two would step the
    // scanner off the frequency the test names).
    static double scannerHzThenSquelch(AppWindow& a, float db) {
        return passseam::read<double>(rawEngine(a.engine_), [&] {
            const double hz = rawEngine(a.engine_).scanner_.currentHz();
            rawEngine(a.engine_).squelchDb_ = db;
            return hz;
        });
    }
    static void muteBy(AppWindow& a, std::vector<std::string> names) {
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).mutedBy_ = std::move(names); });
    }
    static void muteByHeld(AppWindow& a, std::vector<std::string> names) { rawEngine(a.engine_).mutedBy_ = std::move(names); }
    // A change of the lists that touches nothing these tests read. A decoder line used to be it (it moved
    // listSeq once a line); since API 0.3 8.7 (D3) a line moves no list counter, so the preset note - a
    // status line the Decoders section letters - changes instead, and moves listSeq all the same.
    static void aLineAppears(AppWindow& a) {
        static unsigned n = 0;
        const std::string line = "note " + std::to_string(++n);
        passseam::on(rawEngine(a.engine_), [&] { rawEngine(a.engine_).presetNote_ = line; });
    }
    static std::string recordDir(AppWindow& a) { return passseam::read<std::string>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).recordDir_; }); }
    static cascade::core::Scanner::Params engineParams(AppWindow& a) {
        return passseam::read<cascade::core::Scanner::Params>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scannerParams(); });
    }
    static double engineStartMhz(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanStartMhz_; }); }
    static double engineStopMhz(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanStopMhz_; }); }
    static double engineStepKhz(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanStepKhz_; }); }
    static double engineDwell(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanDwellMs_; }); }
    static double engineHold(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanHoldMs_; }); }
    static double engineResume(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanResumeMs_; }); }
    static double engineListen(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).scanListenMs_; }); }
    static double scannerHzHeld(AppWindow& a) { return rawEngine(a.engine_).scanner_.currentHz(); }
    static double scannerHz(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return scannerHzHeld(a); }); }
    static bool scannerActiveHeld(AppWindow& a) { return rawEngine(a.engine_).scanner_.active(); }
    static bool scannerActive(AppWindow& a) { return passseam::read<bool>(rawEngine(a.engine_), [&] { return scannerActiveHeld(a); }); }
    static double tuned(AppWindow& a) { return passseam::read<double>(rawEngine(a.engine_), [&] { return rawEngine(a.engine_).currentAbsoluteHz(); }); }
    // THE PASS HELD OUT OF THE WAY for a block of the test's own steps. Inline and stepped no pass runs between them anyway;
    // free-running a second thread parks the pass inside passseam::on (Engine::runOnPass: work run at the pass's head, after
    // its command queues and before its scanner tick and its publish) from the constructor to the destructor - so what the
    // window submits in that block is not applied in it (and a dropQueued in it drops it, as inline), the published state is
    // the last pass's whole, and this thread may read the engine directly (the Held forms above).
    class PassHold {
    public:
        explicit PassHold(AppWindow& a) : a_(a) {
            if (passseam::mode() != passseam::Mode::Free) { return; }
            passseam::attach(rawEngine(a_.engine_));
            holder_ = std::thread([this] {
                passseam::on(rawEngine(a_.engine_), [this] {
                    std::unique_lock<std::mutex> lk(m_);
                    held_ = true;
                    cv_.notify_all();
                    cv_.wait(lk, [this] { return release_; });
                });
            });
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return held_; });
        }
        ~PassHold() {
            if (!holder_.joinable()) { return; }
            {
                std::lock_guard<std::mutex> lk(m_);
                release_ = true;
            }
            cv_.notify_all();
            holder_.join();
        }
        PassHold(const PassHold&) = delete;
        PassHold& operator=(const PassHold&) = delete;

    private:
        AppWindow& a_;
        std::thread holder_;
        std::mutex m_;
        std::condition_variable cv_;
        bool held_ = false;
        bool release_ = false;
    };
    // The window's read again, made under a PassHold, free-running only: the look's own read may have been followed by a pass of
    // the thread's own; this one is of the state no pass is changing while the hold stands. (Inline and stepped no pass has
    // run since the look: nothing to read again, and nothing is.)
    static void relook(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free) { return; }
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    // A look made under a PassHold: inline and stepped exactly look(); free-running the pass is parked, so the publish is made
    // here, on this thread, and the window reads it.
    static void lookHeld(AppWindow& a) {
        if (passseam::mode() != passseam::Mode::Free) {
            look(a);
            return;
        }
        (void)rawEngine(a.engine_).publishReceiverState();
        a.statusLines_ = rawEngine(a.engine_).statusText();
        a.refreshUiState();
    }
    // The allocations one get_setting of a fact makes inside the table.
    static long factAllocations(AppWindow& a, const char* key) {
        char buf[512];
        const long mark = t_allocs;
        (void)a.engineApi_->get_setting(a.uiSession_, key, buf, sizeof(buf));
        return t_allocs - mark;
    }
    // BOOKMARK_GROUPS as the table answers it, for the comparison with the window's own derivation.
    static std::vector<std::pair<std::string, double>> list21(AppWindow& a) {
        std::vector<std::pair<std::string, double>> out;
        const int32_t n = a.engineApi_->read_list(a.uiSession_, FOXAPI_LIST_BOOKMARK_GROUPS, 0, nullptr);
        for (int32_t i = 0; i < n; ++i) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            a.engineApi_->read_list(a.uiSession_, FOXAPI_LIST_BOOKMARK_GROUPS, static_cast<uint32_t>(i), &it);
            out.push_back({it.name, it.value[0]});
        }
        return out;
    }
    static FoxListItem bookmarkItem(AppWindow& a, uint32_t i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        a.engineApi_->read_list(a.uiSession_, FOXAPI_LIST_BOOKMARKS, i, &it);
        return it;
    }
    static std::int32_t count(AppWindow& a, uint32_t list) { return a.engineApi_->read_list(a.uiSession_, list, 0, nullptr); }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::BookmarkRow;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

// One frame with `body` drawn inside a window; no engine pass.
template <class F>
void frame(F body, float w = 520.0f, float h = 900.0f) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("panel", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    body();
    ImGui::End();
    ImGui::Render();
}

// What a frame drew, as text: ImGui's own log of the text its widgets rendered.
template <class F>
std::string drawnText(F body) {
    std::string out;
    frame([&] {
        ImGui::LogToBuffer();
        body();
        out = ImGui::GetCurrentContext()->LogBuffer.c_str();  // (read before LogFinish, which empties it)
        ImGui::LogFinish();
    });
    return out;
}

bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

bool sameCommand(const FoxCommand& a, const FoxCommand& b) {
    return a.structSize == b.structSize && a.op == b.op && a.target == b.target && a.flags == b.flags &&
           a.ival[0] == b.ival[0] && a.ival[1] == b.ival[1] && a.num[0] == b.num[0] && a.num[1] == b.num[1] &&
           a.num[2] == b.num[2] && a.num[3] == b.num[3] && std::strcmp(a.text, b.text) == 0;
}

// A command, as one line: what the comparison of a set of them prints and sorts by.
std::string describe(const cmd::QueuedCommand& q) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld num=%.17g,%.17g,%.17g,%.17g text=[%s]", cmd::opName(q.c.op),
                  static_cast<long long>(q.c.ival[0]), static_cast<long long>(q.c.ival[1]), q.c.num[0], q.c.num[1],
                  q.c.num[2], q.c.num[3], q.longText.empty() ? q.c.text : q.longText.c_str());
    return buf;
}

// --- a long list ------------------------------------------------------------------------------

std::vector<cc::Bookmark> bigList(int n, unsigned seed) {
    static const char* const words[] = {"Radio",  "Tower", "ATIS",  "Ground", "Weather", "Ham",
                                        "Repeater", "Marine", "Police", "Fire",  "Coast",   "Rail",
                                        "Bus",    "Taxi",  "Air",   "Bravo",  "Echo",    "Tango"};
    static const char* const groups[] = {"",         "",         "",           "Airband",      "Marine",
                                         "Ham 2 m",  "Ham 70 cm", "Public safety", "Broadcast FM", "Rail",
                                         "Weather",  "ISM",      "caf\xC3\xA9", "Zulu",         "alpha"};
    static const char* const modes[] = {"NFM", "AM", "WFM", "USB", "LSB", "CW"};
    std::mt19937_64 rng(seed);
    std::vector<cc::Bookmark> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        cc::Bookmark b;
        b.name = std::string(words[rng() % (sizeof(words) / sizeof(words[0]))]) + " " + std::to_string(rng() % 5000);
        b.freqHz = 25.0e6 + static_cast<double>(rng() % 1675000) * 1000.0 + static_cast<double>(rng() % 10) * 100.0;
        b.group = groups[rng() % (sizeof(groups) / sizeof(groups[0]))];
        b.mode = modes[rng() % (sizeof(modes) / sizeof(modes[0]))];
        b.bandwidthHz = 2500.0 + static_cast<double>(rng() % 150) * 1000.0;
        b.favourite = rng() % 9 == 0;
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<BookmarkRow> rowsOf(const std::vector<cc::Bookmark>& list) {
    std::vector<BookmarkRow> rows;
    rows.reserve(list.size());
    for (const cc::Bookmark& b : list) {
        BookmarkRow r;
        r.hz = b.freqHz;
        r.favourite = b.favourite;
        r.name = b.name;
        r.id = b.id;
        r.group = b.group;
        r.mode = cmd::foxDemodFromName(b.mode.c_str());
        r.bandwidthHz = b.bandwidthHz;
        rows.push_back(std::move(r));
    }
    return rows;
}

// THE MODEL of the view: the rule rebuildBookmarkView had before the port, over the store's list.
std::vector<std::uint32_t> modelView(const std::vector<cc::Bookmark>& list, const std::string& group, bool favOnly,
                                     const std::string& filter) {
    std::string needle = filter;
    for (char& c : needle) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const auto contains = [](const std::string& hay, const std::string& n) {
        if (n.empty()) { return true; }
        const auto it = std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        });
        return it != hay.end();
    };
    std::vector<std::uint32_t> out;
    char mhz[32];
    for (std::size_t i = 0; i < list.size(); ++i) {
        const cc::Bookmark& b = list[i];
        if (favOnly && !b.favourite) { continue; }
        if (!group.empty() && b.group != group) { continue; }
        if (!needle.empty()) {
            std::snprintf(mhz, sizeof(mhz), "%.6f", b.freqHz / 1e6);
            if (!contains(b.name, needle) && !contains(b.group, needle) &&
                std::strncmp(mhz, needle.c_str(), needle.size()) != 0) {
                continue;
            }
        }
        out.push_back(static_cast<std::uint32_t>(i));
    }
    return out;
}

// What the copy holds, in bytes: the rows, and the heap of each string past the small buffer.
std::size_t bytesOf(const std::vector<BookmarkRow>& rows) {
    std::size_t n = rows.capacity() * sizeof(BookmarkRow);
    for (const BookmarkRow& r : rows) {
        n += r.name.capacity() > 15 ? r.name.capacity() + 1 : 0;
        n += r.group.capacity() > 15 ? r.group.capacity() + 1 : 0;
    }
    return n;
}

// --- A -----------------------------------------------------------------------------------------

void testPureRules() {
    std::printf("  A1: the view's filter is the rule rebuildBookmarkView had (10 000 entries)\n");
    {
        cc::FreqManager fm;
        CHECK_EQ(fm.addMany(bigList(10000, 20261014)), 10000u);
        const std::vector<BookmarkRow> rows = rowsOf(fm.list());
        const char* const filters[] = {"",     "radio", "RADIO", "tow", "atis", "145.5", "145",   "100.",
                                       "0",    "9",     "z",     "no-such-thing", "caf\xC3\xA9", "ALPHA", " ", "ra", "1",
                                       "ham 7", "[", "Weather", "wEaTh", "25.0", "1699"};
        const char* const groups[] = {"", "Airband", "Marine", "alpha", "caf\xC3\xA9", "does-not-exist"};
        long combos = 0;
        long shown = 0;
        std::vector<std::uint32_t> got;
        for (const char* f : filters) {
            for (const char* g : groups) {
                for (int fav = 0; fav < 2; ++fav) {
                    cascade::gui::bookmarkView(rows, g, fav != 0, f, got);
                    const std::vector<std::uint32_t> want = modelView(fm.list(), g, fav != 0, f);
                    if (got != want) { std::printf("      filter [%s] group [%s] fav %d: %zu vs %zu\n", f, g, fav, got.size(), want.size()); }
                    CHECK(got == want);
                    ++combos;
                    shown += static_cast<long>(want.size());
                }
            }
        }
        std::printf("      %ld filters x groups x favourites held; %ld rows shown in all\n", combos, shown);
        CHECK(shown > 0);
    }

    std::printf("  A2: the group names are the model's, and what BOOKMARK_GROUPS answers\n");
    {
        cc::FreqManager fm;
        fm.addMany(bigList(10000, 77));
        const std::vector<BookmarkRow> rows = rowsOf(fm.list());
        std::map<std::string, int> model;
        for (const cc::Bookmark& b : fm.list()) {
            if (!b.group.empty()) { ++model[b.group]; }
        }
        const std::vector<std::string> names = cascade::gui::bookmarkGroupNames(rows);
        CHECK_EQ(names.size(), model.size());
        std::size_t k = 0;
        bool same = names.size() == model.size();
        for (const auto& kv : model) { same = same && k < names.size() && names[k++] == kv.first; }
        CHECK(same);
        CHECK(names.size() >= 8u);
        // ...and none with an empty list or no groups.
        CHECK(cascade::gui::bookmarkGroupNames({}).empty());
        std::vector<BookmarkRow> none(3);
        CHECK(cascade::gui::bookmarkGroupNames(none).empty());
    }

    std::printf("  A3: the samples a take's bytes make are the recorder's own count\n");
    {
        const std::filesystem::path dir = g_scratch / "takes_a3";
        cc::Recorder audio;
        std::string err;
        CHECK(audio.start(cc::RecordKind::Audio, dir.string(), 48000.0, err));
        std::vector<float> a(5000, 0.25f);
        for (const std::size_t n : {1u, 2u, 7u, 480u, 4097u, 5000u}) {
            audio.writeAudio(a.data(), n);
            CHECK_EQ(cascade::gui::takeSamples(static_cast<double>(audio.bytesWritten()), cascade::gui::kAudioFrameBytes),
                     audio.samplesWritten());
        }
        CHECK(audio.samplesWritten() > 9000u);
        audio.stop();
        cc::Recorder iq;
        CHECK(iq.start(cc::RecordKind::BasebandIq, dir.string(), 2.4e6, err));
        std::vector<std::complex<float>> z(5000, std::complex<float>(0.1f, -0.2f));
        for (const std::size_t n : {1u, 3u, 480u, 4097u, 5000u}) {
            iq.writeIq(z.data(), n);
            CHECK_EQ(cascade::gui::takeSamples(static_cast<double>(iq.bytesWritten()), cascade::gui::kIqFrameBytes),
                     iq.samplesWritten());
        }
        CHECK(iq.samplesWritten() > 9000u);
        iq.stop();
        CHECK_EQ(cascade::gui::takeSamples(-5.0, 8), 0u);
        CHECK_EQ(cascade::gui::takeSamples(0.0, 8), 0u);
        CHECK_EQ(cascade::gui::takeSamples(7.0, 8), 0u);
        CHECK_EQ(cascade::gui::takeSamples(16.0, 8), 2u);
        CHECK_EQ(cascade::gui::takeSamples(16.0, 0), 0u);
    }

    std::printf("  A4: the commands the scanner form sends are the ones it always sent (every combination)\n");
    {
        AppWindow app;
        Access::look(app);
        Access::Sent sent;
        Access::tap(app, &sent);
        FoxReceiverState& st = Access::state(app);
        st.scanDwellMs = 41;
        st.scanHoldMs = 42;
        st.scanResumeMs = 43;
        st.scanListenMs = 44;
        const double drafts[7] = {88.5, 108.25, 12.5, 11.0, 22.0, 33.0, 55.5};
        for (int i = 0; i < 7; ++i) { Access::draft(app, i) = drafts[i]; }
        long cases = 0;
        for (int running = 0; running < 2; ++running) {
            st.flags = running != 0 ? FOXAPI_RX_SCANNER_ACTIVE : 0u;
            for (int reconfigure = 0; reconfigure < 2; ++reconfigure) {
                for (unsigned bits = 0; bits < 128u; ++bits) {
                    sent.clear();
                    Access::commit(app, bits, reconfigure != 0);
                    // THE MODEL: the old commitScannerDraft, line for line, from the engine's own values.
                    std::vector<FoxCommand> want;
                    if ((bits & 0x78u) != 0) {
                        FoxCommand timing = cmd::make(FOXAPI_OP_SCANNER_CONFIG);
                        timing.num[0] = (bits & 0x08u) != 0 ? drafts[3] : 41.0;
                        timing.num[1] = (bits & 0x10u) != 0 ? drafts[4] : 42.0;
                        timing.num[2] = (bits & 0x20u) != 0 ? drafts[5] : 43.0;
                        timing.num[3] = (bits & 0x40u) != 0 ? drafts[6] : 44.0;
                        want.push_back(timing);
                    }
                    const std::int64_t rangeBits = static_cast<std::int64_t>(bits & 0x07u);
                    const bool reconfigureRange = reconfigure != 0 && running != 0;
                    if (rangeBits != 0 || (reconfigureRange && (bits & 0x78u) == 0)) {
                        FoxCommand range = cmd::make(FOXAPP_OP_SCANNER_RANGE);
                        range.ival[0] = rangeBits | (reconfigureRange ? 8 : 0);
                        range.num[0] = drafts[0] * 1.0e6;
                        range.num[1] = drafts[1] * 1.0e6;
                        range.num[2] = drafts[2] * 1.0e3;
                        want.push_back(range);
                    }
                    bool same = sent.size() == want.size();
                    for (std::size_t i = 0; same && i < want.size(); ++i) {
                        same = sameCommand(sent[i].c, want[i]) && sent[i].longText.empty();
                    }
                    if (!same) { std::printf("      bits %02x reconfigure %d running %d: %zu sent, %zu wanted\n", bits, reconfigure, running, sent.size(), want.size()); }
                    CHECK(same);
                    ++cases;
                }
            }
        }
        std::printf("      %ld combinations of edited fields, reconfigure and running held against the old expressions\n", cases);
        Access::tap(app, nullptr);
        Access::dropQueued(app);
    }

    std::printf("  A5: START carries the range the engine stores; the form shows the engine's values\n");
    {
        AppWindow app;
        Access::look(app);
        // The engine's stored range and timings, set through the table as the web remote sets them.
        FoxCommand range = cmd::make(FOXAPP_OP_SCANNER_RANGE);
        range.ival[0] = 7;
        range.num[0] = 144.39e6;  // not exact in megahertz
        range.num[1] = 146.0e6;
        range.num[2] = 12.5e3;
        Access::submit(app, range);
        FoxCommand timing = cmd::make(FOXAPI_OP_SCANNER_CONFIG);
        timing.num[0] = 33.0;
        timing.num[1] = 1500.0;
        timing.num[2] = 250.0;
        timing.num[3] = 20000.0;
        Access::submit(app, timing);
        Access::look(app);
        const FoxCommand start = cascade::gui::scannerStartCommand(Access::state(app));
        const cc::Scanner::Params p = Access::engineParams(app);
        CHECK_EQ(start.op, static_cast<std::uint32_t>(FOXAPI_OP_SCANNER_RUN));
        CHECK_EQ(start.ival[0], 1);
        CHECK_EQ(start.num[0], p.startHz);  // the very doubles the old code read from scannerParams()
        CHECK_EQ(start.num[1], p.stopHz);
        CHECK_EQ(start.num[2], p.stepHz);
        double form[7];
        cascade::gui::scannerFormValues(Access::state(app), form);
        const double want[7] = {Access::engineStartMhz(app), Access::engineStopMhz(app), Access::engineStepKhz(app),
                                Access::engineDwell(app),    Access::engineHold(app),    Access::engineResume(app),
                                Access::engineListen(app)};
        for (int i = 0; i < 7; ++i) {
            // (the range makes the trip through Hz and back: the same number to the last digit or so)
            CHECK(std::fabs(form[i] - want[i]) <= 1e-12 * std::fabs(want[i]));
            if (i >= 3) { CHECK_EQ(form[i], want[i]); }
        }
        std::printf("      form %.17g %.17g %.17g | %.0f %.0f %.0f %.0f\n", form[0], form[1], form[2], form[3], form[4],
                    form[5], form[6]);
    }
}

// --- B -----------------------------------------------------------------------------------------

void testCopyIsMadeWhenTheStoreChanges() {
    std::printf("  B: the one copy of BOOKMARKS follows the store's version, not listSeq; each row whole\n");
    AppWindow app;
    Access::look(app);
    const auto& c = Access::cache(app);
    (void)Access::tuning(app);
    Access::rebuild(app);
    const std::uint32_t copies0 = c.markerCopies;
    CHECK(c.markers.empty());
    CHECK_EQ(Access::viewCopies(app), copies0);

    // Lines a decoder prints move listSeq and the store's version not at all: nothing is copied and the
    // view is not built again (it is poisoned first, so a rebuild would show).
    Access::view(app).assign(3, 99u);
    const std::uint64_t seq0 = Access::listSeq(app);
    for (int i = 0; i < 25; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        (void)Access::tuning(app);
        Access::rebuild(app);
    }
    std::printf("      25 decoder lines: listSeq %llu -> %llu, rows copied %u times, the view rebuilt %s\n",
                static_cast<unsigned long long>(seq0), static_cast<unsigned long long>(Access::listSeq(app)),
                c.markerCopies - copies0, Access::view(app).size() == 3u ? "never" : "AT LEAST ONCE");
    CHECK(Access::listSeq(app) != seq0);
    CHECK_EQ(c.markerCopies, copies0);
    CHECK_EQ(Access::view(app).size(), 3u);

    // A store of three hundred: long names and groups (cut by the table), modes (one no code names),
    // bandwidths, a few starred.
    std::vector<cc::Bookmark> many = bigList(300, 5);
    many[3].name = std::string(120, 'n') + "!";
    many[4].group = std::string(170, 'g');
    many[5].mode = "ZZ-FUTURE";
    Access::storeDo(app, [&](cc::FreqManager& s) { s.addMany(std::move(many)); });
    Access::look(app);
    (void)Access::tuning(app);
    CHECK_EQ(c.markerCopies, copies0 + 1u);
    std::vector<cc::Bookmark> list = Access::storeList(app);  // (a copy: taken again after each change below)
    CHECK_EQ(c.markers.size(), list.size());
    bool whole = c.markers.size() == list.size();
    for (std::size_t i = 0; whole && i < list.size(); ++i) {
        const BookmarkRow& r = c.markers[i];
        const cc::Bookmark& b = list[i];
        const bool ok = r.id == b.id && r.hz == b.freqHz && r.favourite == b.favourite &&
                        r.name == b.name.substr(0, 95) && r.group == b.group.substr(0, 159) &&
                        r.mode == cmd::foxDemodFromName(b.mode.c_str()) && r.bandwidthHz == b.bandwidthHz;
        if (!ok) { std::printf("      row %zu differs\n", i); }
        whole = whole && ok;
    }
    CHECK(whole);
    std::uint32_t odd = 0;
    for (std::size_t i = 0; i < list.size(); ++i) { odd += list[i].mode == "ZZ-FUTURE" ? c.markers[i].mode + 1u : 0u; }
    CHECK_EQ(odd, 1u);  // a mode no code names reads 0
    // ...and the view and groups follow the copy.
    Access::rebuild(app);
    CHECK_EQ(Access::view(app).size(), list.size());
    CHECK_EQ(Access::viewCopies(app), c.markerCopies);

    // A star keeps the list's size and changes the store: copied again. A remove too. A burst of decoder
    // lines after them copies nothing more.
    {
        cc::Bookmark b = list[10];
        b.favourite = !b.favourite;
        Access::storeDo(app, [&](cc::FreqManager& s) { s.updateAt(10, b); });
        list = Access::storeList(app);
        Access::look(app);
        (void)Access::tuning(app);
        CHECK_EQ(c.markerCopies, copies0 + 2u);
        CHECK(c.markers.size() > 10u);
        std::size_t at = 0;
        for (std::size_t i = 0; i < list.size(); ++i) { at = list[i].id == b.id ? i : at; }
        if (c.markers.size() > at) { CHECK_EQ(c.markers[at].favourite, b.favourite); }
        Access::storeDo(app, [](cc::FreqManager& s) { s.removeAt(0); });
        list = Access::storeList(app);
        Access::look(app);
        (void)Access::tuning(app);
        CHECK_EQ(c.markerCopies, copies0 + 3u);
        CHECK_EQ(c.markers.size(), list.size());
        Access::rebuild(app);
        CHECK_EQ(Access::view(app).size(), list.size());
    }
    Access::view(app).assign(2, 77u);
    for (int i = 0; i < 10; ++i) {
        Access::aLineAppears(app);
        Access::look(app);
        (void)Access::tuning(app);
        Access::rebuild(app);
    }
    CHECK_EQ(c.markerCopies, copies0 + 3u);
    CHECK_EQ(Access::view(app).size(), 2u);

    // The cost of the one copy of a big import (measured), and of what it holds.
    {
        std::vector<cc::Bookmark> big = bigList(33000, 33);
        Access::storeDo(app, [&](cc::FreqManager& s) { s.addMany(std::move(big)); });
        Access::look(app);
        const Clock::time_point t0 = Clock::now();
        (void)Access::tuning(app);
        const double copyMs = msSince(t0);
        CHECK_EQ(c.markerCopies, copies0 + 4u);
        CHECK(c.markers.size() > 32000u);
        const Clock::time_point t1 = Clock::now();
        Access::rebuild(app);
        const double viewMs = msSince(t1);
        const std::size_t bytes = bytesOf(c.markers);
        std::printf("      a 33 000-entry import: %zu rows copied in %.1f ms (once); %zu bytes in all, %zu a row (a row is %zu "
                    "bytes before its strings); the groups and the view built in %.1f ms\n",
                    c.markers.size(), copyMs, bytes, bytes / c.markers.size(), sizeof(BookmarkRow), viewMs);
        CHECK(copyMs < 2000.0);
        CHECK_EQ(Access::viewCopies(app), c.markerCopies);
    }
}

// --- C -----------------------------------------------------------------------------------------

void testViewAndGroupsAreDerivedFromTheCopy() {
    std::printf("  C: the view and the groups follow the copy; a chosen group stays chosen by name\n");
    AppWindow app;
    Access::look(app);
    std::vector<cc::Bookmark> big = bigList(10000, 20261014);
    Access::storeDo(app, [&](cc::FreqManager& s) { s.addMany(std::move(big)); });
    Access::look(app);
    Access::rebuild(app);

    // The groups are what BOOKMARK_GROUPS answers on the same list (names and, by the same rule, their order).
    const auto table = Access::list21(app);
    CHECK_EQ(table.size(), Access::groups(app).size());
    bool same = table.size() == Access::groups(app).size();
    for (std::size_t i = 0; same && i < table.size(); ++i) { same = table[i].first == Access::groups(app)[i]; }
    CHECK(same);
    CHECK(table.size() >= 8u);
    std::printf("      10 000 entries: %zu groups, the window's and the table's the same\n", table.size());

    // The view is the model's, over the store, for a filter, a group and favourites.
    Access::groupSel(app) = 3;
    Access::favOnly(app) = false;
    std::strcpy(Access::filter(app), "radio");
    Access::rebuild(app);
    const std::string chosen = Access::groups(app)[2];
    CHECK(Access::view(app) == modelView(Access::storeList(app), chosen, false, "radio"));
    CHECK(!Access::view(app).empty());
    Access::favOnly(app) = true;
    std::strcpy(Access::filter(app), "145");
    Access::groupSel(app) = 0;
    Access::rebuild(app);
    CHECK(Access::view(app) == modelView(Access::storeList(app), "", true, "145"));
    // The group is picked again, with an empty filter.
    Access::favOnly(app) = false;
    Access::filter(app)[0] = '\0';
    Access::groupSel(app) = 3;
    Access::rebuild(app);
    CHECK(Access::view(app) == modelView(Access::storeList(app), chosen, false, ""));

    // A group that sorts before all the others is added: the chosen group moves one place in the list
    // and is still the chosen one, by name.
    {
        cc::Bookmark b;
        b.name = "First of all";
        b.freqHz = 50.0e6;
        b.group = "0-first";
        Access::storeDo(app, [&](cc::FreqManager& s) { s.add(b); });
        Access::look(app);
        Access::rebuild(app);
        CHECK_EQ(Access::groups(app)[0], std::string("0-first"));
        CHECK_EQ(Access::groupSel(app), 4);
        CHECK_EQ(Access::groups(app)[3], chosen);
        CHECK(Access::view(app) == modelView(Access::storeList(app), chosen, false, ""));
    }
    // The chosen group is removed (through the session, as the key does): the choice is gone, "every group".
    {
        Access::submit(app, cmd::makeText(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, chosen));
        Access::look(app);
        const auto answers = Access::answers(app);
        CHECK_EQ(answers.size(), 1u);
        Access::rebuild(app);
        CHECK_EQ(Access::groupSel(app), 0);
        for (const std::string& g : Access::groups(app)) { CHECK(g != chosen); }
        CHECK(Access::view(app) == modelView(Access::storeList(app), "", false, ""));
        const auto after = Access::list21(app);
        CHECK_EQ(after.size(), Access::groups(app).size());
    }

    // A view nothing moved costs nothing: 1000 rebuilds over a 33 000-entry list allocate nothing and are
    // a few comparisons each.
    std::vector<cc::Bookmark> more = bigList(33000, 9);
    Access::storeDo(app, [&](cc::FreqManager& s) { s.addMany(std::move(more)); });
    Access::look(app);
    Access::rebuild(app);
    std::strcpy(Access::filter(app), "Weather");
    Access::rebuild(app);
    const std::size_t shown = Access::view(app).size();
    const long mark = t_allocs;
    const Clock::time_point t0 = Clock::now();
    for (int i = 0; i < 1000; ++i) { Access::rebuild(app); }
    const double ms = msSince(t0);
    const long allocs = t_allocs - mark;
    std::printf("      1000 rebuilds of an unchanged view over %zu entries (%zu shown): %ld allocations, %.2f ms in all\n",
                Access::cache(app).markers.size(), shown, allocs, ms);
    CHECK_EQ(allocs, 0L);
    CHECK(ms < 200.0);
    CHECK_EQ(Access::view(app).size(), shown);
    std::strcpy(Access::filter(app), "Weather ");  // a character more: built again
    Access::rebuild(app);
    CHECK(Access::view(app).size() <= shown);
    CHECK(Access::view(app) == modelView(Access::storeList(app), "", false, "Weather "));
}

// --- D -----------------------------------------------------------------------------------------

void testScannerFollowsTheEngine() {
    std::printf("  D: the scanner's readout and form follow the engine through all four states\n");
    AppWindow app;
    Access::look(app);
    const auto drawn = [&] { return drawnText([&] { Access::drawScanner(app); }); };

    // Idle, as Scanner::Params{} has it: the readout and the seven fields.
    std::string t = drawn();
    CHECK(has(t, "Idle | 88.0000 MHz"));
    const cc::Scanner::Params def;
    const double defForm[7] = {def.startHz / 1e6, def.stopHz / 1e6, def.stepHz / 1e3, def.dwellMs,
                               def.holdMs,        def.resumeMs,     def.listenMs};
    for (int i = 0; i < 7; ++i) { CHECK_EQ(Access::draft(app, i), defForm[i]); }

    // A range and timings arrive from elsewhere (the web remote): the form shows each of the seven the
    // frame after, each its own (the values are all different, so one shown in another's place is seen).
    FoxCommand range = cmd::make(FOXAPP_OP_SCANNER_RANGE);
    range.ival[0] = 7;
    range.num[0] = 144.0e6;
    range.num[1] = 146.0e6;
    range.num[2] = 12.5e3;
    Access::submit(app, range);
    FoxCommand timing = cmd::make(FOXAPI_OP_SCANNER_CONFIG);
    timing.num[0] = 0.0;       // dwell: a step every tick
    timing.num[1] = 60000.0;   // hold
    timing.num[2] = 70.0;      // resume
    timing.num[3] = 9000.0;    // listen
    Access::submit(app, timing);
    Access::look(app);
    t = drawn();
    const double form[7] = {144.0, 146.0, 12.5, 0.0, 60000.0, 70.0, 9000.0};
    for (int i = 0; i < 7; ++i) {
        if (Access::draft(app, i) != form[i]) { std::printf("      field %d shows %.6g, wanted %.6g\n", i, Access::draft(app, i), form[i]); }
        CHECK_EQ(Access::draft(app, i), form[i]);
    }
    CHECK(has(t, "Idle | 88.0000 MHz"));  // (the scanner's frequency has not moved: no scan has run)

    // Started: Scanning, at the start of the range.
    // (Free-running, the look is followed by the pass HELD, the window's read made again and the draw and the scanner's own
    // figures made under it: the scanner steps on every tick of a pass of its own - a dwell of 0 - and the readout is held
    // to where the scanner IS, at a moment no pass is moving it.)
    Access::submit(app, cascade::gui::scannerStartCommand(Access::state(app)));
    Access::look(app, true);
    std::optional<Access::PassHold> hold(std::in_place, app);
    Access::relook(app);
    t = drawn();
    std::printf("      after START: \"%s\"\n", t.c_str());
    if (passseam::mode() == passseam::Mode::Free) {
        // STEPPED-ONLY: "at the start of the range" is the readout exactly ONE scanner tick after START (the tick that emits the
        // start), and a free-running pass ticks the scanner on its own clock - with a dwell of 0 every tick after that one steps -
        // so how many ticks precede the window's read is the thread's, not the test's. Held inline and stepped; here the
        // readout is held to the scanner's own frequency instead (as the stepping below is).
        std::printf("      (free-running pass: \"Scanning | 144.0000 MHz\" right after START is tick-exact, checked inline and stepped only)\n");
        char now[64];
        std::snprintf(now, sizeof(now), "Scanning | %.4f MHz", Access::scannerHzHeld(app) / 1.0e6);
        CHECK(has(t, now));
    } else {
        CHECK(has(t, "Scanning | 144.0000 MHz"));
    }
    CHECK(Access::scannerActiveHeld(app));
    CHECK(has(t, "Stop scan") && has(t, "Skip") && !has(t, "Start scan"));
    hold.reset();
    // Stepping: the readout is where the scanner is, not where the range starts.
    std::set<std::string> seen;
    for (int i = 0; i < 6; ++i) {
        Access::look(app, true);
        hold.emplace(app);
        Access::relook(app);
        t = drawn();
        char want[64];
        std::snprintf(want, sizeof(want), "Scanning | %.4f MHz", Access::scannerHzHeld(app) / 1.0e6);
        hold.reset();
        CHECK(has(t, want));
        seen.insert(want);
    }
    std::printf("      stepping: %zu different readouts in 6 passes\n", seen.size());
    CHECK(seen.size() >= 4u);

    // A signal is found, then it goes quiet.
    const double heardAt = Access::scannerHzThenSquelch(app, -1000.0f);
    Access::look(app, true);
    t = drawn();
    char want[96];
    std::snprintf(want, sizeof(want), "Paused (signal) | %.4f MHz", heardAt / 1.0e6);
    CHECK(has(t, want));
    Access::setSquelch(app, 20.0f);
    Access::look(app, true);
    t = drawn();
    std::snprintf(want, sizeof(want), "Holding | %.4f MHz", heardAt / 1.0e6);
    CHECK(has(t, want));

    // Stopped: the lamp and the keys go back, the frequency stays where the scan left it.
    Access::submit(app, cmd::makeInt(FOXAPI_OP_SCANNER_RUN, 0));
    Access::look(app, true);
    t = drawn();
    std::snprintf(want, sizeof(want), "Idle | %.4f MHz", heardAt / 1.0e6);
    CHECK(has(t, want));
    CHECK(has(t, "Start scan") && !has(t, "Stop scan"));
    CHECK(!Access::scannerActive(app));
}

// --- E -----------------------------------------------------------------------------------------

void testRecorderIsLive() {
    std::printf("  E: the recorder's bytes and seconds are live; the samples are the recorder's count\n");
    AppWindow app;
    Access::look(app);
    const auto drawn = [&] { return drawnText([&] { Access::drawRecorder(app); }); };
    std::string t = drawn();
    CHECK(has(t, "Record IQ") && has(t, "Record audio"));
    CHECK(!has(t, "Stop IQ") && !has(t, "samples"));
    CHECK(has(t, Access::recordDir(app)));  // the folder is shown, from the fact

    // An audio take: started through the session, then fed (the hermetic window runs no pipeline, so the test
    // writes what the DSP thread would).
    Access::look(app);
    (void)Access::answers(app);  // (the first sighting of the panel, applied and answered)
    Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1));
    Access::look(app);
    const auto started = Access::answers(app);
    CHECK_EQ(started.size(), 1u);
    CHECK(started.size() == 1u && started[0].first == FOXAPI_OP_RECORD_AUDIO && started[0].second == FOXAPI_OK);
    CHECK((Access::state(app).flags & FOXAPI_RX_RECORDING_AUDIO) != 0u);
    t = drawn();
    // (the section found the take in RECORDINGS itself: nothing else asked the list)
    CHECK(Access::takeAudio(app) >= 0);
    CHECK_EQ(Access::takeIq(app), -1);
    CHECK(has(t, "Stop audio") && has(t, "Audio 0.") && has(t, " s | 0 samples | 0.0 MB"));
    CHECK(has(t, "press Play"));  // not running: the recorders are not being fed
    const std::uint64_t listSeq0 = Access::listSeq(app);

    std::vector<float> block(48000, 0.5f);
    Access::writeAudio(app, block.data(), block.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    t = drawn();
    std::printf("      after 48000 frames and 150 ms: \"%s\"\n", [&] {
        const std::size_t at = t.find("Audio ");
        return at == std::string::npos ? std::string() : t.substr(at, t.find('\n', at) - at);
    }().c_str());
    CHECK(has(t, "48000 samples | 0.1 MB"));
    {
        // The seconds drawn are the take's own (150 ms and more have passed).
        const std::size_t at = t.find("Audio ");
        const double drawnSeconds = at == std::string::npos ? -1.0 : std::strtod(t.c_str() + at + 6, nullptr);
        CHECK(drawnSeconds >= 0.1 && drawnSeconds < 5.0);
    }
    const auto take = Access::take(app, Access::takeAudio(app), 2.0);
    CHECK(take.active);
    CHECK_EQ(take.bytes, 96000.0);
    CHECK(take.seconds >= 0.14 && take.seconds < 5.0);
    // Nothing the list is built from moved: listSeq stands where it stood.
    CHECK_EQ(Access::listSeq(app), listSeq0);
    Access::look(app);
    CHECK_EQ(Access::listSeq(app), listSeq0);
    Access::writeAudio(app, block.data(), 1000);
    CHECK_EQ(Access::take(app, Access::takeAudio(app), 2.0).bytes, 98000.0);
    // The seconds are the recorder's own clock: a later read is later.
    const double s1 = Access::take(app, Access::takeAudio(app), 2.0).seconds;
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK(Access::take(app, Access::takeAudio(app), 2.0).seconds > s1 + 0.04);

    // The take is silent: a plugin mutes the audio (its names say so), and the section says whose.
    CHECK(!has(drawn(), "Muted by"));
    // (The stand-in for the plugins' mute is written and published with the pass HELD: free-running, a pass of its own
    // recomputes mutedBy_ from the plugins' presets - none here - at its mute part, and would clear it before any look.)
    std::optional<Access::PassHold> hold(std::in_place, app);
    Access::muteByHeld(app, {"ADS-B", "AIS"});
    Access::lookHeld(app);
    t = drawn();
    hold.reset();
    CHECK(has(t, "Muted by ADS-B and AIS - this take is silent"));
    Access::muteBy(app, {});
    Access::look(app);
    CHECK(!has(drawn(), "Muted by"));
    // The user's own mute silences the take as well, and names no plugin: there is nobody to say.
    Access::submit(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 1));
    Access::look(app);
    CHECK((Access::state(app).flags & FOXAPI_RX_MUTED) != 0u);
    CHECK(!has(drawn(), "Muted by"));
    Access::submit(app, cmd::makeInt(FOXAPI_OP_SET_MUTED, 0));
    Access::look(app);

    // An I/Q take beside it.
    Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_IQ, 1));
    Access::look(app);
    std::vector<std::complex<float>> iq(2400, std::complex<float>(0.1f, 0.2f));
    Access::writeIq(app, iq.data(), iq.size());
    t = drawn();
    CHECK(Access::takeIq(app) >= 0);
    CHECK(has(t, "Stop IQ") && has(t, "IQ ") && has(t, "2400 samples | 0.0 MB"));
    CHECK_EQ(Access::take(app, Access::takeIq(app), 1.0).bytes, 19200.0);
    // ...and a take that ended is not shown as running, whatever its counters hold.
    Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 0));
    Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_IQ, 0));
    Access::look(app);
    t = drawn();
    CHECK(has(t, "Record IQ") && has(t, "Record audio"));
    CHECK(!has(t, "samples"));
    CHECK(!Access::take(app, Access::takeAudio(app), 2.0).active);
    // A take index that is not a recorder of that kind is an empty take.
    CHECK_EQ(Access::take(app, -1, 2.0).bytes, 0.0);
    CHECK_EQ(Access::take(app, Access::takeAudio(app), 1.0).bytes, 0.0);  // the audio entry read as an I/Q take
}

// --- F -----------------------------------------------------------------------------------------

void testNothingAllocatesOnceSettled() {
    std::printf("  F: a settled window's reads allocate nothing\n");
    AppWindow app;
    Access::look(app);
    std::vector<cc::Bookmark> some = bigList(2000, 4);
    Access::storeDo(app, [&](cc::FreqManager& s) { s.addMany(std::move(some)); });
    Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1));
    Access::submit(app, cascade::gui::scannerStartCommand(Access::state(app)));
    Access::look(app, true);
    Access::look(app, true);
    std::vector<float> block(480, 0.1f);
    Access::writeAudio(app, block.data(), block.size());
    // Settled: one of each, so the caches have their room.
    (void)Access::tuning(app);
    Access::rebuild(app);
    Access::refreshStatus(app);
    (void)Access::take(app, Access::takeAudio(app), 2.0);
    (void)Access::fact(app, "fact.recorder.dir");
    const long mark = t_allocs;
    for (int i = 0; i < 1000; ++i) {
        Access::refresh(app);
        (void)Access::tuning(app);
        Access::rebuild(app);
        Access::refreshStatus(app);
        (void)Access::take(app, Access::takeAudio(app), 2.0);
        (void)Access::fact(app, "fact.recorder.dir");
        double form[7];
        cascade::gui::scannerFormValues(Access::state(app), form);
        (void)form;
    }
    const long allocs = t_allocs - mark;
    std::printf("      1000 rounds of state, copy, view, take and fact: %ld allocations\n", allocs);
    CHECK_EQ(allocs, 0L);
    Access::dropQueued(app);
}

// --- J -----------------------------------------------------------------------------------------

void testTheBookmarkLinesAreDrawn() {
    std::printf("  J: the Bookmarks section letters the engine's two status lines\n");
    AppWindow app;
    Access::look(app);
    Access::note(app) = "Exported 3 to somewhere/foxsdr-frequencies.xml";
    Access::errorLine(app).clear();
    std::string t = drawnText([&] { Access::drawBookmarks(app); });
    CHECK(has(t, "Exported 3 to somewhere/foxsdr-frequencies.xml"));
    Access::note(app).clear();
    Access::errorLine(app) = "bookmarks.json could not be saved";
    t = drawnText([&] { Access::drawBookmarks(app); });
    CHECK(has(t, "bookmarks.json could not be saved"));
    CHECK(!has(t, "Exported 3 to"));
    // Both set: the note above the view, the red line at the foot of the section.
    Access::note(app) = "A note";
    t = drawnText([&] { Access::drawBookmarks(app); });
    CHECK(has(t, "A note") && has(t, "bookmarks.json could not be saved"));
    CHECK(t.find("A note") < t.find("shown of"));
    CHECK(t.find("bookmarks.json could not be saved") > t.find("shown of"));
    // ...and the lines are the engine's own: an import that cannot read its file says so through them.
    Access::note(app).clear();
    Access::errorLine(app).clear();
    Access::submit(app, cmd::makeText(FOXAPP_OP_BOOKMARK_IMPORT_FILE, (g_scratch / "no-such-file.xml").string()));
    Access::look(app);
    t = drawnText([&] { Access::drawBookmarks(app); });
    CHECK(has(t, "Could not import"));
}

// --- K -----------------------------------------------------------------------------------------

// A FILE DROPPED ON THE WINDOW (the pump move's step P4). drawUi used to apply the import with a direct applyCommand, in the middle of the
// frame's own pump - which a pass on a thread of its own would race (the key rules and the bookmark store are the pass's). The frame takes
// the dropped path as the SAME COMMAND the Import key sends, on the window's session (AppWindow::submitPendingDrop), and the pass applies it.
void testADroppedFileIsImportedThroughTheSession() {
    std::printf("  K: a file dropped on the window is imported through the session\n");
    AppWindow app;
    Access::look(app);
    const std::filesystem::path csv = g_scratch / "dropped.csv";
    {
        std::ofstream f(csv, std::ios::binary);
        f << "frequency,name\n100100000,Dropped One\n100200000,Dropped Two\n";
    }
    const std::size_t before = Access::storeList(app).size();
    Access::Sent sent;
    Access::tap(app, &sent);
    // (The pass HELD from the drop to the checks that it was sent and not applied: free-running, a pass of its own would
    // apply the import within milliseconds, and "not applied" is "not applied BY THE DROP".)
    std::optional<Access::PassHold> hold(std::in_place, app);
    Access::drop(app, csv.string());
    // Sent, not applied: the command is on the session, the engine's store has not moved.
    CHECK_EQ(sent.size(), 1u);
    if (sent.size() == 1u) {
        CHECK_EQ(sent[0].c.op, static_cast<std::uint32_t>(FOXAPP_OP_BOOKMARK_IMPORT_FILE));
        CHECK_EQ(sent[0].longText.empty() ? std::string(sent[0].c.text) : sent[0].longText, csv.string());
    }
    CHECK_EQ(Access::storeSizeHeld(app), before);
    CHECK(Access::pendingDrop(app).empty());              // the drop was taken (a second frame would not import it again)
    hold.reset();
    Access::look(app);                                    // the pass applies it
    const auto got = Access::answers(app);
    bool answered = false;
    for (const auto& r : got) { answered = answered || (r.first == FOXAPP_OP_BOOKMARK_IMPORT_FILE && r.second == FOXAPI_OK); }
    CHECK(answered);
    CHECK_EQ(Access::storeList(app).size(), before + 2u);
    // Nothing to take: nothing sent.
    sent.clear();
    Access::drop(app, std::string());
    CHECK(sent.empty());
    Access::tap(app, nullptr);
    std::printf("      the drop became one APP_BOOKMARK_IMPORT_FILE on the session; the engine imported two rows when its pass ran\n");
}

// --- I -----------------------------------------------------------------------------------------

// drawUi commits a scanner edit left pending by a section that stopped being drawn. The commit carries the
// engine's own values for the fields nobody edited, from the state - so it stands AFTER the frame's read of
// the state (refreshUiState), and once: before it the state is a pass old and a timing set by the frame before
// would be set back.
void testFlushStandsAfterTheRead() {
    std::printf("  I: drawUi flushes the scanner's pending edit after the frame's read of the state\n");
    std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/gui/app_window.cpp", std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::size_t body = text.find("void AppWindow::drawUi() {");
    CHECK(body != std::string::npos);
    const std::size_t end = text.find("\nvoid AppWindow::", body + 10);
    const std::string drawUi = (body == std::string::npos || end == std::string::npos) ? std::string() : text.substr(body, end - body);
    CHECK(!drawUi.empty());
    std::vector<std::size_t> flushAt, refreshAt, keysAt;
    for (std::size_t p = drawUi.find("flushScannerDraft();"); p != std::string::npos; p = drawUi.find("flushScannerDraft();", p + 1)) { flushAt.push_back(p); }
    for (std::size_t p = drawUi.find("\n    refreshUiState();"); p != std::string::npos; p = drawUi.find("\n    refreshUiState();", p + 1)) { refreshAt.push_back(p); }
    for (std::size_t p = drawUi.find("dispatchKeyBindings();"); p != std::string::npos; p = drawUi.find("dispatchKeyBindings();", p + 1)) { keysAt.push_back(p); }
    // (the call in a comment does not count: only a line that is the call)
    std::size_t calls = 0;
    std::size_t callAt = 0;
    for (const std::size_t p : flushAt) {
        const std::size_t lineStart = drawUi.rfind('\n', p) + 1;
        if (drawUi.compare(lineStart, 4, "    ") == 0 && drawUi[lineStart + 4] == 'f') {
            ++calls;
            callAt = p;
        }
    }
    CHECK_EQ(calls, 1u);
    CHECK_EQ(refreshAt.size(), 1u);
    CHECK_EQ(keysAt.size(), 1u);
    if (calls == 1u && refreshAt.size() == 1u && keysAt.size() == 1u) {
        CHECK(callAt > refreshAt[0]);
        CHECK(callAt < keysAt[0]);
        std::printf("      flushScannerDraft() at %zu, refreshUiState() at %zu, dispatchKeyBindings() at %zu in drawUi\n", callAt,
                    refreshAt[0], keysAt[0]);
    }
}

// --- G -----------------------------------------------------------------------------------------

struct Spot {
    ImGuiID id;
    float x, y;
    std::string window;  // the window the pointer was over (a debugging aid for a failed sweep)
};

// The distinct widgets of what `body` draws, top to bottom, each at the first point of a grid the pointer finds it at.
template <class F>
std::vector<Spot> sweep(F body) {
    std::vector<Spot> out;
    ImGuiIO& io = ImGui::GetIO();
    for (float y = 4.0f; y < 880.0f; y += 4.0f) {
        for (float x = 6.0f; x < 510.0f; x += 12.0f) {
            io.AddMousePosEvent(x, y);
            frame(body);
            frame(body);
            const ImGuiID h = GImGui->HoveredId;
            if (h == 0) { continue; }
            bool seen = false;
            for (const Spot& s : out) { seen = seen || s.id == h; }
            if (!seen) {
                out.push_back({h, x, y, GImGui->HoveredWindow != nullptr ? GImGui->HoveredWindow->Name : ""});
            }
        }
    }
    io.AddMousePosEvent(510.0f, 890.0f);
    frame(body);
    return out;
}

// One click at (x, y) and then one on empty ground (which deactivates a field and closes a popup).
template <class F>
void clickAt(F body, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame(body);
    io.AddMouseButtonEvent(0, true);
    frame(body);
    io.AddMouseButtonEvent(0, false);
    frame(body);
    io.AddMousePosEvent(510.0f, 890.0f);
    frame(body);
    io.AddMouseButtonEvent(0, true);
    frame(body);
    io.AddMouseButtonEvent(0, false);
    frame(body);
}

// Every widget but the section's own header clicked once, and the commands each sent (except the usage report's).
template <class F>
std::vector<std::string> clickEverything(AppWindow& app, F body, std::vector<cmd::QueuedCommand>* captured = nullptr,
                                         const std::function<void()>& after = {}) {
    Access::Sent sent;
    Access::tap(app, &sent);
    for (int i = 0; i < 3; ++i) { frame(body); }  // the first sightings
    const std::vector<Spot> spots = sweep(body);
    std::vector<std::string> out;
    std::printf("      %zu widgets found:", spots.size());
    for (const Spot& sp : spots) { std::printf(" (%.0f,%.0f %s)", sp.x, sp.y, sp.window.c_str()); }
    std::printf("\n");
    for (std::size_t i = 0; i < spots.size(); ++i) {
        // The window's title bar and the section's header row and key (the top band, down to the first
        // control) are not the buttons this clicks: the header folds the section, and a folding section
        // draws as a drawer that the rail's own flush ends, which a frame of this test has no part of.
        if (spots[i].y < 56.0f) { continue; }
        sent.clear();
        clickAt(body, spots[i].x, spots[i].y);
        if (after) { after(); }  // (a checkbox or a combo changed what the next widget is)
        for (const auto& q : sent) {
            if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
            out.push_back(describe(q));
            if (captured != nullptr) { captured->push_back(q); }
        }
    }
    Access::tap(app, nullptr);
    std::sort(out.begin(), out.end());
    return out;
}

// What the clicks of G sent, kept for H to replay.
struct Captured {
    std::vector<cmd::QueuedCommand> recorder, scanner, bookmarks;
};
Captured g_cap;

// Three bookmarks in two groups, the first starred; their ids, in the store's order.
std::vector<std::uint64_t> seedThree(AppWindow& app) {
    std::vector<cc::Bookmark> three(3);
    three[0].name = "Alpha";
    three[0].freqHz = 100.3e6;
    three[0].group = "Broadcast";
    three[0].favourite = true;
    three[1].name = "Bravo";
    three[1].freqHz = 118.1e6;
    three[1].mode = "AM";
    three[1].bandwidthHz = 8333.0;
    three[1].group = "Airband";
    three[2].name = "Charlie";
    three[2].freqHz = 145.5e6;
    three[2].mode = "NFM";
    three[2].group = "Airband";
    Access::storeDo(app, [&](cc::FreqManager& s) { s.addMany(std::move(three)); });
    Access::look(app);
    Access::rebuild(app);
    std::vector<std::uint64_t> id;
    for (const cc::Bookmark& b : Access::storeList(app)) { id.push_back(b.id); }
    return id;
}

std::string expect(const char* op, long long i0, long long i1, double n0, double n1, double n2, double n3,
                   const std::string& text) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld num=%.17g,%.17g,%.17g,%.17g text=[%s]", op,
                  static_cast<long long>(i0), static_cast<long long>(i1), n0, n1, n2, n3, text.c_str());
    return buf;
}

void checkSet(std::vector<std::string> got, std::vector<std::string> want, const char* what) {
    std::sort(want.begin(), want.end());
    const bool ok = got == want;
    if (!ok) {
        std::printf("      %s: the commands the clicks sent differ\n", what);
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : want) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(ok);
    std::printf("      %s: %zu command(s), as expected\n", what, got.size());
}

void testDrawnAndClicked() {
    std::printf("  G: the members drawn in real frames; every widget clicked\n");
    // --- idle: three frames of everything send only the first sightings, then nothing ---
    {
        AppWindow app;
        Access::look(app);
        Access::Sent sent;
        Access::tap(app, &sent);
        const auto all = [&] {
            Access::drawRecorder(app);
            Access::drawBookmarks(app);
            Access::drawScanner(app);
        };
        // Before the first read (the zero state), then after it.
        for (int looked = 0; looked < 2; ++looked) {
            if (looked == 1) { Access::look(app); }
            for (int i = 0; i < 3; ++i) { frame(all); }
        }
        std::set<std::string> names;
        for (const auto& q : sent) {
            CHECK_EQ(q.c.op, static_cast<std::uint32_t>(FOXAPP_OP_NOTE_PANEL));
            names.insert(q.c.text);
        }
        std::printf("      three sections, six frames: first sightings %s\n", [&] {
            std::string s;
            for (const auto& n : names) { s += (s.empty() ? "" : ", ") + n; }
            return s;
        }().c_str());
        CHECK(names == (std::set<std::string>{"bookmarks", "recorder", "scanner"}));
        CHECK_EQ(sent.size(), 3u);
        sent.clear();
        for (int i = 0; i < 5; ++i) { frame(all); }
        CHECK(sent.empty());
        Access::tap(app, nullptr);
    }

    // --- the recorder ---
    {
        AppWindow app;
        Access::look(app);
        const auto body = [&] { Access::drawRecorder(app); };
        std::printf("    recorder, idle:\n");
        // (Each sweep to its dropQueued with the pass HELD: what the clicks queue is dropped, not applied, as inline -
        // free-running, a pass of its own would apply each click before the next is made, and the sweep's widgets move.)
        std::optional<Access::PassHold> hold(std::in_place, app);
        auto got = clickEverything(app, body, &g_cap.recorder);
        checkSet(got, {expect("RECORD_IQ", 1, 0, 0, 0, 0, 0, ""), expect("RECORD_AUDIO", 1, 0, 0, 0, 0, 0, "")}, "idle");
        Access::dropQueued(app);
        hold.reset();
        // Both takes running: the same two buttons say stop.
        Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_IQ, 1));
        Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1));
        Access::look(app);
        Access::refreshStatus(app);
        std::printf("    recorder, both takes running:\n");
        hold.emplace(app);
        got = clickEverything(app, body, &g_cap.recorder);
        checkSet(got, {expect("RECORD_IQ", 0, 0, 0, 0, 0, 0, ""), expect("RECORD_AUDIO", 0, 0, 0, 0, 0, 0, "")}, "taping");
        Access::dropQueued(app);
        hold.reset();
    }

    // --- the scanner ---
    {
        AppWindow app;
        Access::look(app);
        const auto body = [&] { Access::drawScanner(app); };
        std::printf("    scanner, idle:\n");
        std::optional<Access::PassHold> hold(std::in_place, app);
        auto got = clickEverything(app, body, &g_cap.scanner);
        checkSet(got, {expect("SCANNER_RUN", 1, 0, 88.0e6, 108.0e6, 100.0e3, 0, "")}, "idle");
        Access::dropQueued(app);
        hold.reset();
        Access::submit(app, cascade::gui::scannerStartCommand(Access::state(app)));
        Access::look(app, true);
        std::printf("    scanner, running:\n");
        hold.emplace(app);
        got = clickEverything(app, body, &g_cap.scanner);
        checkSet(got, {expect("SCANNER_RUN", 0, 0, 0, 0, 0, 0, ""), expect("SCANNER_SKIP", 0, 0, 0, 0, 0, 0, "")}, "running");
        Access::dropQueued(app);
        hold.reset();
    }

    // --- the bookmarks: three rows (two groups), a name typed for Add current, an import path ---
    {
        AppWindow app;
        Access::look(app);
        const std::vector<std::uint64_t> id = seedThree(app);
        const std::vector<cc::Bookmark> list = Access::storeList(app);  // (a copy: nothing below changes the store - the clicks are dropped)
        std::strcpy(Access::nameField(app), "Mine");
        const auto body = [&] { Access::drawBookmarks(app); };
        std::string idsText;
        for (const auto i : id) { idsText += std::to_string(i) + " "; }

        std::printf("    bookmarks, every group shown, no import path:\n");
        std::vector<std::string> want = {expect("BOOKMARK_ADD", 0, 0, 0, 0, 0, 0, "Mine"),
                                         expect("APP_BOOKMARK_EXPORT", 0, 0, 0, 0, 0, 0, idsText)};
        for (int i = 0; i < 3; ++i) {
            const long long ident = static_cast<long long>(id[static_cast<std::size_t>(i)]);
            const bool fav = list[static_cast<std::size_t>(i)].favourite;
            want.push_back(expect("BOOKMARK_FAVOURITE", ident, fav ? 0 : 1, 0, 0, 0, 0, ""));
            want.push_back(expect("BOOKMARK_TUNE", ident, 0, 0, 0, 0, 0, ""));
            want.push_back(expect("BOOKMARK_REMOVE", ident, 0, 0, 0, 0, 0, ""));
        }
        const auto restore = [&] {
            Access::favOnly(app) = false;
            Access::markersOn(app) = true;
        };
        std::optional<Access::PassHold> hold(std::in_place, app);
        auto got = clickEverything(app, body, &g_cap.bookmarks, restore);
        checkSet(got, want, "every group");
        restore();
        Access::groupSel(app) = 0;
        Access::filter(app)[0] = '\0';
        Access::dropQueued(app);
        hold.reset();

        // One group chosen and an import path typed: the group's removal and the import appear; only that group's rows are shown.
        Access::groupSel(app) = 1;  // "Airband" (sorted first)
        std::strcpy(Access::importPath(app), "C:/lists/frequencies.xml");
        Access::rebuild(app);
        CHECK_EQ(Access::groups(app)[0], std::string("Airband"));
        CHECK_EQ(Access::view(app).size(), 2u);
        std::string airIds;
        for (const std::uint32_t r : Access::view(app)) { airIds += std::to_string(list[r].id) + " "; }
        want = {expect("BOOKMARK_ADD", 0, 0, 0, 0, 0, 0, "Mine"),
                expect("APP_BOOKMARK_IMPORT_FILE", 0, 0, 0, 0, 0, 0, "C:/lists/frequencies.xml"),
                expect("APP_BOOKMARK_EXPORT", 0, 0, 0, 0, 0, 0, airIds),
                expect("BOOKMARK_REMOVE_GROUP", 0, 0, 0, 0, 0, 0, "Airband")};
        for (int i = 1; i < 3; ++i) {
            const long long ident = static_cast<long long>(id[static_cast<std::size_t>(i)]);
            want.push_back(expect("BOOKMARK_FAVOURITE", ident, 1, 0, 0, 0, 0, ""));
            want.push_back(expect("BOOKMARK_TUNE", ident, 0, 0, 0, 0, 0, ""));
            want.push_back(expect("BOOKMARK_REMOVE", ident, 0, 0, 0, 0, 0, ""));
        }
        std::printf("    bookmarks, the group Airband chosen, an import path typed:\n");
        // (clicking "Remove this group" sets the choice back to every group, which the view would then show
        //  in full; the choice is put back after each click so the rows the sweep found are still the rows)
        hold.emplace(app);
        got = clickEverything(app, body, &g_cap.bookmarks, [&] {
            restore();
            Access::groupSel(app) = 1;
        });
        checkSet(got, want, "the group Airband chosen");
        Access::dropQueued(app);
        hold.reset();
    }
}

// --- H -----------------------------------------------------------------------------------------

// Each command the sections sent when clicked (G's), exactly as the section built it, submitted on the window's
// session in a fresh window and applied by the engine's pass: accepted (OK), and the state the sections read
// back is where the command put it.
void testTheCommandsReachTheEngine() {
    std::printf("  H: each command the sections sent, as built, is accepted through the session and applied\n");
    std::set<std::string> done;
    int distinct = 0;
    const auto replay = [&](const std::vector<cmd::QueuedCommand>& commands) {
        for (const cmd::QueuedCommand& q : commands) {
            if (!done.insert(describe(q)).second) { continue; }
            ++distinct;
            AppWindow app;
            Access::look(app);
            const std::vector<std::uint64_t> id = seedThree(app);
            const std::size_t n0 = Access::storeList(app).size();
            const std::uint64_t op = q.c.op;
            // The state a stop or a skip needs to find.
            if (op == FOXAPI_OP_RECORD_IQ && q.c.ival[0] == 0) { Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_IQ, 1)); }
            if (op == FOXAPI_OP_RECORD_AUDIO && q.c.ival[0] == 0) { Access::submit(app, cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, 1)); }
            if ((op == FOXAPI_OP_SCANNER_RUN && q.c.ival[0] == 0) || op == FOXAPI_OP_SCANNER_SKIP) {
                Access::submit(app, cascade::gui::scannerStartCommand(Access::state(app)));
            }
            Access::look(app, true);
            (void)Access::answers(app);
            Access::submit(app, q);
            Access::look(app, true);
            const auto answers = Access::answers(app);
            if (answers.size() != 1u || answers[0].second != FOXAPI_OK) {
                std::printf("      %s: %zu answers, first %d\n", describe(q).c_str(), answers.size(),
                            answers.empty() ? 0 : answers[0].second);
            }
            CHECK_EQ(answers.size(), 1u);
            if (answers.size() == 1u) { CHECK_EQ(answers[0].second, FOXAPI_OK); }
            const FoxReceiverState& st = Access::state(app);
            const std::vector<cc::Bookmark> list = Access::storeList(app);
            switch (op) {
                case FOXAPI_OP_RECORD_IQ: CHECK_EQ((st.flags & FOXAPI_RX_RECORDING_IQ) != 0u, q.c.ival[0] != 0); break;
                case FOXAPI_OP_RECORD_AUDIO: CHECK_EQ((st.flags & FOXAPI_RX_RECORDING_AUDIO) != 0u, q.c.ival[0] != 0); break;
                case FOXAPI_OP_SCANNER_RUN:
                    CHECK_EQ((st.flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u, q.c.ival[0] != 0);
                    if (q.c.ival[0] != 0) {
                        CHECK_EQ(st.scanStartHz, q.c.num[0]);
                        CHECK_EQ(st.scanStopHz, q.c.num[1]);
                        CHECK_EQ(st.scanStepHz, q.c.num[2]);
                        CHECK_EQ(st.scannerState, FOXAPI_SCANNER_SCANNING);
                    }
                    break;
                case FOXAPI_OP_SCANNER_SKIP: CHECK_EQ(st.scannerState, FOXAPI_SCANNER_SCANNING); break;
                case FOXAPI_OP_BOOKMARK_ADD:
                    CHECK_EQ(list.size(), n0 + 1u);
                    CHECK(std::any_of(list.begin(), list.end(), [&](const cc::Bookmark& b) { return b.name == q.c.text; }));
                    break;
                case FOXAPI_OP_BOOKMARK_FAVOURITE:
                    for (const cc::Bookmark& b : list) {
                        if (b.id == static_cast<std::uint64_t>(q.c.ival[0])) { CHECK_EQ(b.favourite, q.c.ival[1] != 0); }
                    }
                    break;
                case FOXAPI_OP_BOOKMARK_TUNE:
                    for (const cc::Bookmark& b : list) {
                        if (b.id == static_cast<std::uint64_t>(q.c.ival[0])) {
                            CHECK(std::fabs(st.tunedHz - b.freqHz) < 1.0);
                            CHECK_EQ(st.demodMode, cmd::foxDemodFromName(b.mode.c_str()));
                        }
                    }
                    break;
                case FOXAPI_OP_BOOKMARK_REMOVE:
                    CHECK_EQ(list.size(), n0 - 1u);
                    for (const cc::Bookmark& b : list) { CHECK(b.id != static_cast<std::uint64_t>(q.c.ival[0])); }
                    break;
                case FOXAPI_OP_BOOKMARK_REMOVE_GROUP:
                    CHECK_EQ(list.size(), n0 - 2u);
                    for (const cc::Bookmark& b : list) { CHECK(b.group != q.c.text); }
                    CHECK(has(Access::note(app), "Removed 2 from"));
                    break;
                case FOXAPP_OP_BOOKMARK_EXPORT:
                    CHECK(Access::note(app).rfind("Exported ", 0) == 0);
                    CHECK(has(Access::note(app), "foxsdr-frequencies-"));
                    break;
                case FOXAPP_OP_BOOKMARK_IMPORT_FILE: CHECK(has(Access::note(app), "Could not import")); break;
                default: CHECK(false); break;
            }
            Access::dropQueued(app);
        }
    };
    replay(g_cap.recorder);
    replay(g_cap.scanner);
    replay(g_cap.bookmarks);
    std::printf("      %d distinct commands replayed\n", distinct);
    CHECK_EQ(distinct, 21);  // 4 recorder, 3 scanner, 14 bookmark: add, import, 2 exports, remove group, 3 x (star, tune, remove)
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_recorder_bookmarks_scanner\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_rbs_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    // The three sections start closed; the capture hook opens them (read once).
    setEnv("FOXSDR_OPEN_SECTIONS", "Recorder,Bookmarks,Scanner");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testPureRules();
    testCopyIsMadeWhenTheStoreChanges();
    testViewAndGroupsAreDerivedFromTheCopy();
    testScannerFollowsTheEngine();
    testRecorderIsLive();
    testNothingAllocatesOnceSettled();
    testTheBookmarkLinesAreDrawn();
    testFlushStandsAfterTheRead();
    testADroppedFileIsImportedThroughTheSession();
    testDrawnAndClicked();
    testTheCommandsReachTheEngine();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_recorder_bookmarks_scanner");
}
