// The bookmark list and the waterfall markers are saved off the frame loop.
//
// WHAT WENT WRONG. AppWindow::flushBookmarkSave and flushMarkerSave ran from
// drawUi, once a frame, and when a save was due they did the whole of it on the
// thread that draws the window: create the settings folder, write a temporary
// file, flush it, rename it over bookmarks.json (or markers.json). That is the
// synchronous write that froze a window in 0.96.3 for the CONFIG file
// (gui/config_writer.hpp moved it) and the same disk call in the same folder
// (%APPDATA%\foxsdr, which a Store package redirects and OneDrive can sync). A
// bookmark star clicked, a marker's note typed, an imported list: any of them
// followed a second later by a window that did not draw for as long as the disk
// took. Found by the 0.99.64 audit of what the 0.99.63 Record fix left on the GUI
// thread (docs/DIAGNOSTICS.md, "The window does no disk work").
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK:
// the blocking step of a save is the writer function the window hands the saver,
// and a test binds one that sleeps and then does the REAL atomic write - a slow
// disk as far as the frame loop can tell, with the real file, the real bytes and
// the real rename behind it.
//
//   1. THE HARNESS CAN SEE THE FAULT. The same slow writer called on the thread
//      that heartbeats is reported by a real HangWatchdog; every "no report"
//      below means something because of this.
//   2. gui::BackgroundSaver ON ITS OWN: a request never waits; a burst ends as
//      the newest content, one write at a time and in the order asked, so an
//      older write can never land after a newer one; a failure comes back in the
//      writer's own words; the exit drain lands a write inside its bound and
//      abandons one that is not coming back.
//   3. THE REAL HANDLERS, through a real AppWindow, under a real HangWatchdog
//      (800 ms threshold) and a frame loop: a bookmark and a marker edited, the
//      two flushes called every frame as drawUi calls them, a 2.5 s write per
//      file. Before the fix the frame loop stalled for the whole write and the
//      watchdog filed a report (worst frame gap 2534 ms, two reports); after it
//      nothing waits. And around that, the cases a faster fix would have broken:
//      the last edit before quit is on disk, two quick edits end as the second,
//      a slow save never lets an older one land after a newer one, a save that
//      fails is reported in the words it always was, and the window is destroyed
//      without waiting for a write that is wedged.
//   4. THE WIRING, because no test can stage every slow disk: no source under
//      src/gui may save either list on the calling thread.
//
// WHAT IS NOT COVERED. Nothing here has met a genuinely slow disk. The red line
// the Bookmarks section draws from bookmarkError_ is ImGui output no test reads;
// the state it reads is checked.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
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

#include <imgui.h>
#include <imgui_internal.h>

#include "core/freq_manager.hpp"
#include "core/freq_markers.hpp"
#include "core/hang_watchdog.hpp"
#include "gui/app_window.hpp"
#include "gui/background_saver.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::FreqManager;
using cascade::core::FreqMarkers;
using cascade::core::HangWatchdog;
using cascade::gui::BackgroundSaver;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members.
struct AppWindowTestAccess {
    static void bindBookmarkWriter(AppWindow& a, ConfigWriter::Writer w) {
        a.bookmarkSaver_.bind(std::move(w));
    }
    static void bindMarkerWriter(AppWindow& a, ConfigWriter::Writer w) {
        a.markerSaver_.bind(std::move(w));
    }
    static void setPaths(AppWindow& a, const std::string& bookmarks, const std::string& markers) {
        a.bookmarkPath_ = bookmarks;
        a.markerPath_ = markers;
    }
    // What the "Add current" button does after the list changed.
    static void addBookmark(AppWindow& a, const std::string& name, double hz) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = "NFM";
        a.freqMgr_.add(std::move(b));
        a.saveBookmarks();
    }
    static void addMarker(AppWindow& a, double hz) { a.freqMarkers_.add(hz, 1700000000); }
    // What drawUi calls, once a frame.
    static void flush(AppWindow& a) {
        a.flushBookmarkSave(false);
        a.flushMarkerSave(false);
    }
    // What the teardown calls before the drain: the final request.
    static void flushNow(AppWindow& a) {
        a.flushBookmarkSave(true);
        a.flushMarkerSave(true);
    }
    // The exit drain, as run() calls it.
    static bool drain(AppWindow& a, int boundMs) {
        return a.drainListSaves(std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(boundMs));
    }
    static std::string bookmarkText(AppWindow& a) { return a.freqMgr_.serialize(); }
    static std::string markerText(AppWindow& a) { return a.freqMarkers_.serialize(); }
    static std::string bookmarkError(AppWindow& a) { return a.bookmarkError_; }
    static std::string markerError(AppWindow& a) { return a.markerError_; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// Per-user directories pointed at a scratch folder (process id in its name) and
// every network-facing URL at a port nothing listens on: constructing an
// AppWindow must not touch the owner's real FoxSDR folders.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_bookmark_save_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// --- a slow disk -------------------------------------------------------------

using RealWriter = bool (*)(const std::string&, const std::string&, std::string&);

// A writer that behaves like a slow disk: it takes `blockMs` to answer, records
// how many times and from which thread it was asked, what it was asked to write
// and in what order, notices being run twice at once, and then does the REAL
// atomic write (or fails, when asked to). Held by shared_ptr and captured by
// value, never by `this`: an abandoned worker outlives the test that started it.
struct SlowDisk {
    std::atomic<int> blockMs{0};
    std::atomic<int> calls{0};
    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<bool> askedFromCaller{false};
    std::atomic<bool> fails{false};
    const std::thread::id caller = std::this_thread::get_id();
    std::mutex m;
    std::vector<std::string> written;  // the text of each write, in the order they landed
};

BackgroundSaver::Writer slowWriter(std::shared_ptr<SlowDisk> d, RealWriter real) {
    return [d, real](const std::string& path, const std::string& text, std::string& error) -> bool {
        if (std::this_thread::get_id() == d->caller) { d->askedFromCaller.store(true); }
        const int live = d->concurrent.fetch_add(1) + 1;
        int seen = d->maxConcurrent.load();
        while (live > seen && !d->maxConcurrent.compare_exchange_weak(seen, live)) {}
        d->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(d->blockMs.load()));
        d->concurrent.fetch_sub(1);
        if (d->fails.load()) {
            error = "bookmarks: the disk went away";
            return false;
        }
        const bool ok = real(path, text, error);
        {
            std::lock_guard<std::mutex> lock(d->m);
            d->written.push_back(text);
        }
        return ok;
    };
}

std::shared_ptr<SlowDisk> makeDisk(int blockMs) {
    auto d = std::make_shared<SlowDisk>();
    d->blockMs.store(blockMs);
    return d;
}

std::vector<std::string> writtenOf(const std::shared_ptr<SlowDisk>& d) {
    std::lock_guard<std::mutex> lock(d->m);
    return d->written;
}

// --- the frame loop, as test_record_start drives it --------------------------

struct LoopStats {
    int frames = 0;
    double worstGapMs = 0.0;
};

// What AppWindow::run does between two presents: beat, do the frame's work,
// wait out the rest of the frame.
template <class Frame>
LoopStats runFrameLoop(HangWatchdog& w, double forMs, Frame&& frame) {
    LoopStats s;
    double last = nowMs();
    const double until = last + forMs;
    while (nowMs() < until) {
        w.heartbeat();
        frame();
        const double t = nowMs();
        if (t - last > s.worstGapMs) { s.worstGapMs = t - last; }
        last = t;
        ++s.frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return s;
}

fs::path reportDir(const std::string& tag) {
    const fs::path d = g_scratch / ("reports-" + tag);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

void startWatchdog(HangWatchdog& w, const fs::path& dir) {
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.start(dir.string(), 800);
    CHECK(w.running());
    // Healthy frames first, so a report afterwards cannot be blamed on the loop
    // never having started.
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);
}

// The window's own clock is ImGui's: the debounce reads ImGui::GetTime(), which
// moves once a frame in the product. Here the test moves it.
void setClockS(double s) { ImGui::GetCurrentContext()->Time = s; }

// Waits until `cond`, feeding the window's per-frame flush on a moving clock.
template <class Cond>
bool pumpApp(AppWindow& app, double ms, Cond&& cond) {
    const double t0 = nowMs();
    const double base = ImGui::GetCurrentContext()->Time;
    while (nowMs() < t0 + ms) {
        setClockS(base + (nowMs() - t0) / 1000.0);
        Access::flush(app);
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// --- 1. THE HARNESS CAN SEE THE FAULT -----------------------------------------

// THE CONTROL. The same slow writer, called where the flush used to call it: on
// the thread that heartbeats. If the harness cannot see THIS, a green result
// below means nothing.
void checkTheSynchronousSaveIsReported() {
    auto disk = makeDisk(2500);
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    std::string err;
    const auto write = slowWriter(disk, &FreqManager::writeFile);
    CHECK(write((g_scratch / "control" / "bookmarks.json").string(), "{}\n", err));
    CHECK(w.reportsWritten() == 1u);
    CHECK(disk->askedFromCaller.load());
    w.stop();
}

// --- 2. gui::BackgroundSaver ON ITS OWN ---------------------------------------

bool pollUntil(BackgroundSaver& s, bool& ok, std::string& err, double ms) {
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        if (s.poll(ok, err)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// A request never waits for the disk, however slow it is.
void checkARequestNeverWaits() {
    auto disk = makeDisk(2000);
    BackgroundSaver s("test", slowWriter(disk, &FreqManager::writeFile));
    const std::string path = (g_scratch / "saver-never" / "bookmarks.json").string();
    const double t0 = nowMs();
    for (int i = 0; i < 20; ++i) { s.request(path, "{\"n\":" + std::to_string(i) + "}\n"); }
    const double took = nowMs() - t0;
    CHECK(took < 300.0);
    CHECK(s.inFlight());
    bool ok = false;
    std::string err;
    CHECK(!s.poll(ok, err));  // not before the disk has answered
    CHECK(s.finishOrAbandon(std::chrono::milliseconds(6000)));
    CHECK(s.lastOk());
}

// A burst ends as the NEWEST content, one write at a time and in the order asked:
// the first request is written, the ones behind it collapse to the last, and an
// older write never lands after a newer one.
void checkABurstEndsAsTheNewestAndInOrder() {
    auto disk = makeDisk(400);
    BackgroundSaver s("test", slowWriter(disk, &FreqManager::writeFile));
    const std::string path = (g_scratch / "saver-burst" / "bookmarks.json").string();
    s.request(path, "A\n");
    // The disk is now busy with A; B and C arrive behind it.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    s.request(path, "B\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    s.request(path, "C\n");
    CHECK(s.hasQueued());
    CHECK(s.finishOrAbandon(std::chrono::milliseconds(6000)));
    const std::vector<std::string> order = writtenOf(disk);
    CHECK(order.size() == 2u);
    if (order.size() == 2u) {
        CHECK(order[0] == "A\n");
        CHECK(order[1] == "C\n");  // B was superseded before it ever started
    }
    CHECK(disk->maxConcurrent.load() == 1);
    CHECK(readAll(path) == "C\n");
}

// A failure comes back in the writer's own words, and the next success is a
// success (so a stale error is cleared by a later good save).
void checkAFailureComesBackInTheWritersWords() {
    auto disk = makeDisk(50);
    disk->fails.store(true);
    BackgroundSaver s("test", slowWriter(disk, &FreqManager::writeFile));
    const std::string path = (g_scratch / "saver-fail" / "bookmarks.json").string();
    bool ok = true;
    std::string err;
    s.request(path, "x\n");
    CHECK(pollUntil(s, ok, err, 3000.0));
    CHECK(!ok);
    CHECK(err == "bookmarks: the disk went away");
    disk->fails.store(false);
    s.request(path, "y\n");
    CHECK(pollUntil(s, ok, err, 3000.0));
    CHECK(ok);
    CHECK(err.empty());
    CHECK(readAll(path) == "y\n");
}

// The exit drain lands a write inside its bound, and abandons - without waiting
// out the disk - one that is not coming back; the abandoned one finishes by
// itself.
void checkTheDrainLandsOrAbandons() {
    {
        auto disk = makeDisk(300);
        BackgroundSaver s("test", slowWriter(disk, &FreqManager::writeFile));
        const std::string path = (g_scratch / "saver-drain" / "bookmarks.json").string();
        s.request(path, "last\n");
        const double t0 = nowMs();
        CHECK(s.finishOrAbandon(std::chrono::milliseconds(1500)));
        CHECK(nowMs() - t0 < 1200.0);
        CHECK(s.lastOk());
        CHECK(readAll(path) == "last\n");
    }
    {
        auto disk = makeDisk(2500);
        const std::string path = (g_scratch / "saver-abandon" / "bookmarks.json").string();
        double tookMs = 0.0;
        {
            BackgroundSaver s("test", slowWriter(disk, &FreqManager::writeFile));
            s.request(path, "late\n");
            const double t0 = nowMs();
            CHECK(!s.finishOrAbandon(std::chrono::milliseconds(300)));
            tookMs = nowMs() - t0;
            CHECK(!s.inFlight());
        }
        // Bounded by the bound, not by the 2500 ms the disk is taking.
        CHECK(tookMs < 1200.0);
        CHECK(disk->concurrent.load() == 1);  // still inside the disk: abandoned, not joined
        const double until = nowMs() + 6000.0;
        while (nowMs() < until && writtenOf(disk).empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(writtenOf(disk).size() == 1u);  // the abandoned worker finished on its own
        CHECK(readAll(path) == "late\n");
    }
}

// --- 3. THE REAL HANDLERS, THROUGH A REAL WINDOW -------------------------------

// THE FIELD HANG, through the code the star, the marker menu and drawUi reach: a
// 2.5 s write per file against an 800 ms threshold. The loop must keep beating,
// the watchdog must write nothing, and the saves must still arrive - a fix that
// merely dropped them would pass everything but the last of these.
void checkAppWindowSavesNeverHoldAFrame() {
    AppWindow app;
    const fs::path dir = g_scratch / "app-slow";
    const std::string bm = (dir / "bookmarks.json").string();
    const std::string mk = (dir / "markers.json").string();
    Access::setPaths(app, bm, mk);
    auto bmDisk = makeDisk(2500);
    auto mkDisk = makeDisk(2500);
    Access::bindBookmarkWriter(app, slowWriter(bmDisk, &FreqManager::writeFile));
    Access::bindMarkerWriter(app, slowWriter(mkDisk, &FreqMarkers::writeFile));

    HangWatchdog w;
    startWatchdog(w, reportDir("appwindow"));
    const double t0 = nowMs();
    bool first = true;
    const LoopStats s = runFrameLoop(w, 4200.0, [&] {
        setClockS((nowMs() - t0) / 1000.0);
        if (first) {
            first = false;
            Access::addBookmark(app, "Tower", 118.5e6);
            Access::addMarker(app, 145.5e6);
        }
        Access::flush(app);
    });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 150);
    CHECK(s.worstGapMs < 800.0);
    // Both asked once, from a thread that is not the frame loop's.
    CHECK(bmDisk->calls.load() == 1);
    CHECK(mkDisk->calls.load() == 1);
    CHECK(!bmDisk->askedFromCaller.load());
    CHECK(!mkDisk->askedFromCaller.load());
    std::printf("  AppWindow saves: %d frames in 4.2 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();

    // THE SAVES ARRIVED - the loop ran 4.2 s against 1 s debounce + 2.5 s write -
    // and are the files a synchronous save wrote: the window's own text.
    CHECK(readAll(bm) == Access::bookmarkText(app));
    CHECK(readAll(mk) == Access::markerText(app));
    CHECK(readAll(bm).find("Tower") != std::string::npos);
}

// THE DATA, each case against the class: nothing a faster fix would have bought
// by losing something.
void checkNoEditIsLost() {
    const fs::path dir = g_scratch / "app-data";
    const std::string bm = (dir / "bookmarks.json").string();
    const std::string mk = (dir / "markers.json").string();

    // --- two quick edits end as the second, one write at a time, in order -----
    {
        AppWindow app;
        Access::setPaths(app, bm, mk);
        auto disk = makeDisk(600);
        Access::bindBookmarkWriter(app, slowWriter(disk, &FreqManager::writeFile));
        setClockS(10.0);
        Access::addBookmark(app, "First", 118.0e6);
        const std::string afterFirst = Access::bookmarkText(app);
        setClockS(11.5);  // past the one-second debounce
        Access::flush(app);  // the first save starts, and the disk is now busy
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        // A second edit while the first write is still in flight: debounced, then
        // handed over behind it.
        Access::addBookmark(app, "Second", 119.0e6);
        const std::string afterSecond = Access::bookmarkText(app);
        setClockS(13.0);
        Access::flush(app);
        CHECK(pumpApp(app, 6000.0, [&] { return writtenOf(disk).size() == 2u; }));
        const std::vector<std::string> order = writtenOf(disk);
        CHECK(order.size() == 2u);
        if (order.size() == 2u) {
            CHECK(order[0] == afterFirst);   // landed first, in the order asked
            CHECK(order[1] == afterSecond);  // and the newest landed last
        }
        CHECK(disk->maxConcurrent.load() == 1);
        CHECK(readAll(bm) == afterSecond);
        CHECK(readAll(bm).find("Second") != std::string::npos);
    }

    // --- the last edit before quit is on disk afterwards ----------------------
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        AppWindow app;
        Access::setPaths(app, bm, mk);
        auto bmDisk = makeDisk(400);
        auto mkDisk = makeDisk(400);
        Access::bindBookmarkWriter(app, slowWriter(bmDisk, &FreqManager::writeFile));
        Access::bindMarkerWriter(app, slowWriter(mkDisk, &FreqMarkers::writeFile));
        setClockS(20.0);
        Access::addBookmark(app, "Before quit", 121.5e6);
        Access::addMarker(app, 145.8e6);
        // Quit lands inside the debounce window: nothing has been requested yet,
        // which is exactly the case the exit flush exists for.
        Access::flush(app);
        CHECK(bmDisk->calls.load() == 0);
        Access::flushNow(app);  // run()'s final request
        const double t0 = nowMs();
        CHECK(Access::drain(app, 1500));  // run()'s bounded drain: landed
        CHECK(nowMs() - t0 < 1400.0);
        CHECK(readAll(bm) == Access::bookmarkText(app));
        CHECK(readAll(mk) == Access::markerText(app));
        CHECK(readAll(bm).find("Before quit") != std::string::npos);
    }

    // --- an edit made while a slow save is in flight, then quit: the drain
    //     collects the one in flight AND the one behind it --------------------
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        AppWindow app;
        Access::setPaths(app, bm, mk);
        auto disk = makeDisk(500);
        Access::bindBookmarkWriter(app, slowWriter(disk, &FreqManager::writeFile));
        setClockS(30.0);
        Access::addBookmark(app, "One", 100.0e6);
        Access::flushNow(app);  // in flight, 500 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Access::addBookmark(app, "Two", 101.0e6);
        Access::flushNow(app);  // held behind it
        CHECK(Access::drain(app, 3000));
        const std::vector<std::string> order = writtenOf(disk);
        CHECK(order.size() == 2u);
        CHECK(disk->maxConcurrent.load() == 1);
        CHECK(readAll(bm) == Access::bookmarkText(app));
        CHECK(readAll(bm).find("Two") != std::string::npos);
    }

    // --- a drain whose bound runs out abandons the write and returns at the
    //     bound, not at the disk ----------------------------------------------
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
        AppWindow app;
        Access::setPaths(app, bm, mk);
        auto disk = makeDisk(2200);
        Access::bindBookmarkWriter(app, slowWriter(disk, &FreqManager::writeFile));
        setClockS(40.0);
        Access::addBookmark(app, "Wedged", 102.0e6);
        Access::flushNow(app);
        const double t0 = nowMs();
        CHECK(!Access::drain(app, 300));
        CHECK(nowMs() - t0 < 1200.0);
    }
}

// A save that fails is reported in the words it always was - the words the
// synchronous save gave for the same inputs - a frame or more later, and the
// next good save clears it.
void checkAFailedSaveIsReportedAsItAlwaysWas() {
    // A plain FILE squatting on the folder's path: the real failure.
    const fs::path blocker = g_scratch / "app-fail" / "blocker";
    std::error_code ec;
    fs::create_directories(blocker.parent_path(), ec);
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "x";
    }
    const std::string badBm = (blocker / "sub" / "bookmarks.json").string();
    const std::string badMk = (blocker / "sub" / "markers.json").string();

    // The words, as the synchronous save gave them.
    FreqManager inlineMgr;
    std::string inlineBmErr;
    CHECK(!inlineMgr.save(badBm, inlineBmErr));
    CHECK(inlineBmErr.rfind("bookmarks: cannot create directory", 0) == 0);
    FreqMarkers inlineMk;
    std::string inlineMkErr;
    CHECK(!inlineMk.save(badMk, inlineMkErr));
    CHECK(inlineMkErr.rfind("markers: cannot create directory", 0) == 0);

    AppWindow app;
    auto bmDisk = makeDisk(300);
    auto mkDisk = makeDisk(300);
    Access::bindBookmarkWriter(app, slowWriter(bmDisk, &FreqManager::writeFile));
    Access::bindMarkerWriter(app, slowWriter(mkDisk, &FreqMarkers::writeFile));
    Access::setPaths(app, badBm, badMk);
    setClockS(50.0);
    Access::addBookmark(app, "Nowhere", 123.0e6);
    Access::addMarker(app, 146.0e6);
    setClockS(52.0);
    Access::flush(app);
    CHECK(Access::bookmarkError(app).empty());  // not before the disk has answered
    CHECK(pumpApp(app, 6000.0, [&] {
        return !Access::bookmarkError(app).empty() && !Access::markerError(app).empty();
    }));
    CHECK(Access::bookmarkError(app) == inlineBmErr);
    CHECK(Access::markerError(app) == inlineMkErr);

    // Fixed, and edited again: the next successful save clears the stale error.
    const fs::path good = g_scratch / "app-fail" / "good";
    Access::setPaths(app, (good / "bookmarks.json").string(), (good / "markers.json").string());
    setClockS(60.0);
    Access::addBookmark(app, "Somewhere", 124.0e6);
    Access::addMarker(app, 147.0e6);
    setClockS(62.0);
    CHECK(pumpApp(app, 6000.0, [&] {
        return Access::bookmarkError(app).empty() && Access::markerError(app).empty();
    }));
    CHECK(readAll(good / "bookmarks.json").find("Somewhere") != std::string::npos);
}

// APPLICATION EXIT WHILE A SAVE IS WEDGED: the window is destroyed without
// waiting for the disk (bounded by the quit grace, not by the 2.5 s the disk is
// taking), and the abandoned worker owns what it uses, so it finishes by itself.
void checkExitWhileASaveIsWedged() {
    const fs::path dir = g_scratch / "app-exit";
    const std::string bm = (dir / "bookmarks.json").string();
    const std::string mk = (dir / "markers.json").string();
    auto bmDisk = makeDisk(2500);
    auto mkDisk = makeDisk(2500);
    double destroyMs = 0.0;
    std::string bookmarkTextAtExit;
    {
        auto app = std::make_unique<AppWindow>();
        Access::setPaths(*app, bm, mk);
        Access::bindBookmarkWriter(*app, slowWriter(bmDisk, &FreqManager::writeFile));
        Access::bindMarkerWriter(*app, slowWriter(mkDisk, &FreqMarkers::writeFile));
        setClockS(70.0);
        Access::addBookmark(*app, "Wedged", 125.0e6);
        Access::addMarker(*app, 148.0e6);
        Access::flushNow(*app);
        bookmarkTextAtExit = Access::bookmarkText(*app);
        const double t0 = nowMs();
        app.reset();
        destroyMs = nowMs() - t0;
    }
    // Two wedged saves, each given the 250 ms grace in ~AppWindow and then let
    // go: well inside the 2.5 s the disk is taking.
    CHECK(destroyMs < 1800.0);
    std::printf("  exit with two saves wedged: ~AppWindow took %.0f ms\n", destroyMs);
    const double until = nowMs() + 8000.0;
    while (nowMs() < until && (writtenOf(bmDisk).empty() || writtenOf(mkDisk).empty())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(writtenOf(bmDisk).size() == 1u);
    CHECK(writtenOf(mkDisk).size() == 1u);
    CHECK(readAll(bm) == bookmarkTextAtExit);
}

// --- 4. THE WIRING ------------------------------------------------------------

std::string readFileText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string t = ss.str();
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    return t;
}

// The text of the function whose definition begins with `signature`, up to the
// closing brace in column 0.
std::string functionBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::string withoutLineComments(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t eol = text.find('\n', i);
        const std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string line = text.substr(i, end - i);
        const std::size_t slashes = line.find("//");
        if (slashes != std::string::npos) { line.erase(slashes); }
        out += line;
        out += '\n';
        i = end + 1;
    }
    return out;
}

// Every way a save of either list reaches the disk from a function that runs on
// the GUI thread: the synchronous save, the blocking half called directly, a
// stream, the filesystem.
const std::vector<std::string>& blockingSaves() {
    static const std::vector<std::string> calls = {
        ".save(", "::save(", "writeFile(", "std::ofstream", "std::filesystem::", "fs::", "fopen(",
    };
    return calls;
}

std::vector<std::string> blockingSavesIn(const std::string& body) {
    std::vector<std::string> found;
    const std::string code = withoutLineComments(body);
    for (const std::string& call : blockingSaves()) {
        if (code.find(call) != std::string::npos) { found.push_back(call); }
    }
    return found;
}

void checkNothingSavesAListOnTheGuiThread() {
    // THE CONTROL: the scan sees the call it exists to forbid, on the function as
    // it was before the fix.
    const std::string before =
        "void AppWindow::flushBookmarkSave(bool force) {\n"
        "    bookmarkSaveDirty_ = false;\n"
        "    std::string err;\n"
        "    if (freqMgr_.save(bookmarkPath_, err)) {\n"
        "        bookmarkError_.clear();\n"
        "    }\n"
        "}\n";
    CHECK(!blockingSavesIn(functionBody(before, "void AppWindow::flushBookmarkSave(")).empty());
    // ...and a comment ABOUT a save is not one.
    CHECK(blockingSavesIn("void f() {\n    // freqMgr_.save(p) was here\n    int x = 1;\n}\n")
              .empty());

    const fs::path gui = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    // The flushes live in app_window_disk_work.cpp (the 0.99.64 file for the
    // members that hand disk work to a worker); saveBookmarks is still in
    // app_window.cpp. Both are searched.
    const std::string source =
        readFileText(gui / "app_window_disk_work.cpp") + "\n" + readFileText(gui / "app_window.cpp");
    CHECK(!source.empty());
    for (const char* sig : {"void AppWindow::flushBookmarkSave(", "void AppWindow::flushMarkerSave(",
                            "void AppWindow::saveBookmarks("}) {
        const std::string body = functionBody(source, sig);
        CHECK(body.size() > 100);
        const std::vector<std::string> hits = blockingSavesIn(body);
        for (const std::string& h : hits) { std::printf("  %s asks the disk: %s\n", sig, h.c_str()); }
        CHECK(hits.empty());
    }
    // Both flushes hand the text to their saver, which is the only way out.
    CHECK(functionBody(source, "void AppWindow::flushBookmarkSave(")
              .find("bookmarkSaver_.request(") != std::string::npos);
    CHECK(functionBody(source, "void AppWindow::flushMarkerSave(")
              .find("markerSaver_.request(") != std::string::npos);
    // And no file under src/gui saves either list directly, anywhere.
    int direct = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(gui, ec)) {
        const fs::path p = entry.path();
        if (p.extension() != ".cpp" && p.extension() != ".hpp") { continue; }
        const std::string code = withoutLineComments(readFileText(p));
        for (const char* call : {"freqMgr_.save(", "freqMarkers_.save("}) {
            for (std::size_t at = code.find(call); at != std::string::npos;
                 at = code.find(call, at + 1)) {
                std::printf("  %s saves a list on the calling thread: %s\n",
                            p.filename().string().c_str(), call);
                ++direct;
            }
        }
    }
    CHECK(direct == 0);
}

}  // namespace

int main() {
    isolate();
    ImGui::CreateContext();
    checkNothingSavesAListOnTheGuiThread();
    checkTheSynchronousSaveIsReported();
    checkARequestNeverWaits();
    checkABurstEndsAsTheNewestAndInOrder();
    checkAFailureComesBackInTheWritersWords();
    checkTheDrainLandsOrAbandons();
    checkAppWindowSavesNeverHoldAFrame();
    checkNoEditIsLost();
    checkAFailedSaveIsReportedAsItAlwaysWas();
    checkExitWhileASaveIsWedged();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_bookmark_save_async");
}
