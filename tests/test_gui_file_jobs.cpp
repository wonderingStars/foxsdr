// The GUI's save-a-file and read-a-folder buttons do not make a frame wait.
//
// WHAT WENT WRONG. Four click handlers did their file-system calls on the thread
// that draws the window, each against a folder that can be anywhere the user (or a
// plugin's output) put it - the recordings folder in a synchronised or network
// profile, a screenshot folder named by an environment variable:
//
//   * F12 / the Screenshot key: a BMP, then a ".windows.txt", written inline in
//     the frame loop's own screenshot block.
//   * the plugin picture's "Save as BMP": create_directories on the recordings
//     folder, then the BMP.
//   * the Bookmarks section's "Export for SDR#": create_directories on the
//     recordings folder, then the XML.
//   * the patch Radio's device list (opened, or "Look for radios"): the recordings
//     folders listed and up to kMaxRecordingOpens recording headers opened.
//
// Found by the 0.99.64 audit of what the 0.99.63 Record fix left on the GUI thread
// (docs/DIAGNOSTICS.md, "The window does no disk work").
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK:
// AppWindow carries one seam for it, a hook its workers call immediately before
// they touch the disk, and a test binds one that sleeps - a slow disk as far as the
// frame loop can tell, with the real files, the real BMP encoder and the real
// header parser behind it.
//
//   1. THE HARNESS CAN SEE THE FAULT. A sleeping hook called on the thread that
//      heartbeats is reported by a real HangWatchdog.
//   2. THE REAL HANDLERS, through a real AppWindow, under a real HangWatchdog
//      (800 ms threshold) and a frame loop, a 2.5 s slow disk each. Before the fix
//      the frame loop stalled for the whole disk time and the watchdog filed a
//      report (worst frame gap: recordings list 2505 ms, picture 2501 ms, SDR#
//      export 2514 ms, screenshot 2503 ms); after it nothing waits, and what was
//      asked for still arrives - the file, in the bytes the synchronous handler
//      wrote, and the note, in its words.
//   3. THE EDGES, each against the class: a save that fails says what it said, a
//      second press while one is out changes nothing (and a second listing is
//      remembered and run), and a window destroyed while all of them are wedged
//      does not wait for the disk.
//
// WHAT IS NOT COVERED. The disabled keys and the "Saving..." note are ImGui output
// no test reads; the state they read (the pending accessors, the note members) is
// checked. Nothing here has met a genuinely slow disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#include "core/freq_import.hpp"
#include "core/hang_watchdog.hpp"
#include "core/host_image.hpp"
#include "core/image_write.hpp"
#include "core/patch_recordings.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members.
struct AppWindowTestAccess {
    static void setHook(AppWindow& a, std::function<void()> h) { a.diskHookForTest_ = std::move(h); }
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }
    // One frame's worth of the poll.
    static void poll(AppWindow& a) { a.pollDiskJobs(); }

    static void listRecordings(AppWindow& a) { a.patchListRecordings(); }
    static std::vector<cascade::core::patch::RecordingInfo> recordings(AppWindow& a) {
        return a.patchRecordings_;
    }
    static std::size_t headerReads(AppWindow& a) { return a.patchRecordingCache_.opens; }

    static void saveImage(AppWindow& a, const cascade::core::HostImage& im) { a.saveImageBmp(im); }
    static bool imagePending(AppWindow& a) { return a.imageSavePending(); }
    static std::string imageNote(AppWindow& a) { return a.imageSaveNote_; }
    static std::string imageNotePlugin(AppWindow& a) { return a.imageSaveNotePlugin_; }

    static void addBookmarks(AppWindow& a, int n) {
        for (int i = 0; i < n; ++i) {
            cascade::core::Bookmark b;
            b.name = "Station " + std::to_string(i);
            b.freqHz = 100.0e6 + 1.0e5 * i;
            b.mode = "NFM";
            a.freqMgr_.add(std::move(b));
            a.bookmarkView_.push_back(static_cast<std::uint32_t>(i));
        }
    }
    static std::vector<cascade::core::Bookmark> bookmarks(AppWindow& a) { return a.freqMgr_.list(); }
    static void exportBookmarks(AppWindow& a) { a.exportBookmarksForSdrSharp(); }
    static bool exportPending(AppWindow& a) { return a.bookmarkExportPending(); }
    static std::string exportNote(AppWindow& a) { return a.bookmarkImportNote_; }

    // What the screenshot block does once the frame's pictures are taken.
    static void shot(AppWindow& a, const std::string& dir, int n, const cascade::core::HostImage& im,
                     const std::string& windows) {
        a.shotAddPicture(dir + "/shot-" + std::to_string(n) + ".bmp", im);
        a.shotAddText(dir + "/shot-" + std::to_string(n) + ".windows.txt", windows);
        a.shotFlush();
    }
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

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_gui_file_jobs_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    // The patch's second recordings folder is read from here: a test must not
    // inherit the owner's.
    setEnv("FOXSDR_PATCH_SAMPLES", "");
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- a slow disk -------------------------------------------------------------

struct SlowDisk {
    std::atomic<int> blockMs{0};
    std::atomic<int> calls{0};
    std::atomic<int> finished{0};
    std::atomic<bool> askedFromCaller{false};
    const std::thread::id caller = std::this_thread::get_id();
};

std::shared_ptr<SlowDisk> makeDisk(int blockMs) {
    auto d = std::make_shared<SlowDisk>();
    d->blockMs.store(blockMs);
    return d;
}

std::function<void()> slowHook(std::shared_ptr<SlowDisk> d) {
    return [d] {
        if (std::this_thread::get_id() == d->caller) { d->askedFromCaller.store(true); }
        d->calls.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(d->blockMs.load()));
        d->finished.fetch_add(1);
    };
}

struct LoopStats {
    int frames = 0;
    double worstGapMs = 0.0;
};

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
    for (int i = 0; i < 50; ++i) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(w.reportsWritten() == 0u);
}

// Waits for `cond` while feeding the window's per-frame poll.
template <class Cond>
bool pumpApp(AppWindow& app, double ms, Cond&& cond) {
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        Access::poll(app);
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

cascade::core::HostImage testImage() {
    cascade::core::HostImage im;
    im.plugin = "APT decoder";
    im.width = 16;
    im.height = 8;
    im.format = CASCADE_IMAGE_RGB24;
    im.complete = true;
    im.pixels.assign(16u * 8u * 3u, 0x7F);
    for (std::size_t i = 0; i < im.pixels.size(); ++i) {
        im.pixels[i] = static_cast<std::uint8_t>(i * 7u);
    }
    return im;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<fs::path> filesIn(const fs::path& dir, const char* ext = nullptr) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && (ext == nullptr || e.path().extension() == ext)) {
            out.push_back(e.path());
        }
    }
    return out;
}

// A real I/Q recording to list.
void makeRecording(const fs::path& dir) {
    cascade::core::Recorder rec;
    std::string err;
    CHECK(rec.start(cascade::core::RecordKind::BasebandIq, dir.string(), 250000.0, err));
    std::vector<std::complex<float>> s(2000, std::complex<float>(0.25f, -0.25f));
    rec.writeIq(s.data(), s.size());
    rec.stop();
}

// A folder that cannot be made: a plain FILE squatting on its path.
std::string blockedDir(const char* tag) {
    const fs::path blocker = g_scratch / tag;
    std::error_code ec;
    fs::create_directories(blocker.parent_path(), ec);
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "x";
    }
    return (blocker / "sub").string();
}

// --- 1. THE HARNESS CAN SEE THE FAULT -----------------------------------------

void checkTheSynchronousCallIsReported() {
    auto disk = makeDisk(2500);
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    slowHook(disk)();  // what a handler did on the thread that heartbeats
    CHECK(w.reportsWritten() == 1u);
    CHECK(disk->askedFromCaller.load());
    w.stop();
}

// --- 2. THE REAL HANDLERS, THROUGH A REAL WINDOW --------------------------------

// Runs `press` on the first frame of a loop that also polls, as drawUi does, and
// checks what the watchdog and the frame gaps said.
template <class Press>
void stallCase(const char* what, AppWindow& app, std::shared_ptr<SlowDisk> disk, Press&& press) {
    HangWatchdog w;
    startWatchdog(w, reportDir(what));
    bool first = true;
    const LoopStats s = runFrameLoop(w, 3600.0, [&] {
        if (first) {
            first = false;
            press();
        }
        Access::poll(app);
    });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    CHECK(!disk->askedFromCaller.load());
    CHECK(disk->calls.load() == 1);
    std::printf("  %s: %d frames in 3.6 s, worst gap %.0f ms, hang reports %u\n", what, s.frames,
                s.worstGapMs, w.reportsWritten());
    w.stop();
}

void checkTheRecordingsListNeverHoldsAFrame() {
    AppWindow app;
    const fs::path dir = g_scratch / "list-slow";
    makeRecording(dir);
    Access::setRecordDir(app, dir.string());
    auto disk = makeDisk(2500);
    Access::setHook(app, slowHook(disk));
    // The list is asked for twice on the first frame (the key pressed again): the
    // second is remembered and run when the first comes back, so the disk is asked
    // twice in all, and the first is the one the loop's check counts.
    {
        HangWatchdog w;
        startWatchdog(w, reportDir("recordings list"));
        bool first = true;
        const LoopStats s = runFrameLoop(w, 6400.0, [&] {
            if (first) {
                first = false;
                Access::listRecordings(app);
                Access::listRecordings(app);
            }
            Access::poll(app);
        });
        CHECK(w.reportsWritten() == 0u);
        CHECK(s.frames > 200);
        CHECK(s.worstGapMs < 800.0);
        CHECK(!disk->askedFromCaller.load());
        CHECK(disk->calls.load() == 2);
        std::printf("  recordings list: %d frames in 6.4 s, worst gap %.0f ms, hang reports %u\n",
                    s.frames, s.worstGapMs, w.reportsWritten());
        w.stop();
    }
    // The list arrived, and is the list a synchronous listing makes.
    const auto got = Access::recordings(app);
    const auto direct = cascade::core::patch::listIqRecordings({dir.string()});
    CHECK(got.size() == 1u);
    CHECK(direct.size() == 1u);
    if (got.size() == 1u && direct.size() == 1u) {
        CHECK(got[0].path == direct[0].path);
        CHECK(got[0].key == direct[0].key);
        CHECK(got[0].rateHz == direct[0].rateHz);
        CHECK(got[0].rateHz == 250000.0);
    }
    // One file, read once: the second listing went through the cache the first
    // one grew.
    CHECK(Access::headerReads(app) == 1u);
}

// A listing asked for while one is out is run when it comes back: a recording made
// meanwhile is in the second list, and the cache means its neighbour is not read
// again.
void checkASecondListingIsRemembered() {
    AppWindow app;
    const fs::path dir = g_scratch / "list-again";
    makeRecording(dir);
    Access::setRecordDir(app, dir.string());
    auto disk = makeDisk(1500);
    Access::setHook(app, slowHook(disk));
    Access::listRecordings(app);
    // A second recording appears while the disk is busy; the user presses Look
    // for radios again.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // a different file name
    makeRecording(dir);
    Access::listRecordings(app);
    // The first worker lists the folder only AFTER its disk answers (the hook is
    // the disk), so it already sees both files; what is being proved is that the
    // second press was not lost - a second listing is made when the first comes
    // back - and that it cost no header read the cache had.
    CHECK(pumpApp(app, 12000.0, [&] {
        return Access::recordings(app).size() == 2u && disk->finished.load() == 2;
    }));
    CHECK(disk->calls.load() == 2);
    // Two listings, two header reads: each file once.
    CHECK(Access::headerReads(app) == 2u);
}

void checkThePictureSaveNeverHoldsAFrame() {
    AppWindow app;
    const fs::path dir = g_scratch / "image-slow";
    Access::setRecordDir(app, dir.string());
    auto disk = makeDisk(2500);
    Access::setHook(app, slowHook(disk));
    bool sawSaving = false;
    stallCase("picture save", app, disk, [&] {
        Access::saveImage(app, testImage());
        Access::saveImage(app, testImage());  // pressed again: one save is ever out
        sawSaving = Access::imageNote(app) == "Saving..." && Access::imagePending(app);
    });
    CHECK(sawSaving);
    CHECK(!Access::imagePending(app));
    // The file is the BMP the synchronous handler wrote, and the note says where.
    const std::vector<fs::path> files = filesIn(dir, ".bmp");
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        const fs::path ref = g_scratch / "image-ref.bmp";
        std::string err;
        CHECK(cascade::core::writeBmp24(testImage(), ref.string(), err));
        CHECK(readAll(files[0]) == readAll(ref));
        CHECK(files[0].filename().string().rfind("APT_decoder-", 0) == 0);
        CHECK(Access::imageNote(app) == "Saved " + files[0].string());
    }
    CHECK(Access::imageNotePlugin(app) == "APT decoder");
}

void checkAFailedPictureSaveSaysWhatItSaid() {
    AppWindow app;
    Access::setRecordDir(app, blockedDir("image-blocker"));
    Access::saveImage(app, testImage());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::imagePending(app); }));
    // The synchronous handler ignored create_directories' failure and let the BMP
    // writer say so: the same sentence, a frame or more later.
    CHECK(Access::imageNote(app).rfind("Save failed: cannot open \"", 0) == 0);
    CHECK(Access::imageNotePlugin(app) == "APT decoder");
    // And the key works again.
    Access::setRecordDir(app, (g_scratch / "image-after").string());
    Access::saveImage(app, testImage());
    CHECK(pumpApp(app, 5000.0, [&] { return Access::imageNote(app).rfind("Saved ", 0) == 0; }));
}

void checkTheExportNeverHoldsAFrame() {
    AppWindow app;
    const fs::path dir = g_scratch / "export-slow";
    Access::setRecordDir(app, dir.string());
    Access::addBookmarks(app, 3);
    auto disk = makeDisk(2500);
    Access::setHook(app, slowHook(disk));
    bool sawSaving = false;
    stallCase("SDR# export", app, disk, [&] {
        Access::exportBookmarks(app);
        Access::exportBookmarks(app);  // pressed again: one export is ever out
        sawSaving = Access::exportNote(app) == "Saving..." && Access::exportPending(app);
    });
    CHECK(sawSaving);
    CHECK(!Access::exportPending(app));
    // The file is the XML the synchronous handler wrote, and the note says where.
    const std::vector<fs::path> files = filesIn(dir, ".xml");
    CHECK(files.size() == 1u);
    if (files.size() == 1u) {
        CHECK(readAll(files[0]) == cascade::core::exportSdrSharpXml(Access::bookmarks(app)));
        CHECK(Access::exportNote(app) ==
              "Exported 3 to " + dir.string() + "/" + files[0].filename().string());
    }
}

void checkAFailedExportSaysWhatItSaid() {
    AppWindow app;
    Access::setRecordDir(app, blockedDir("export-blocker"));
    Access::addBookmarks(app, 2);
    Access::exportBookmarks(app);
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::exportPending(app); }));
    CHECK(Access::exportNote(app).rfind("Could not write ", 0) == 0);
    Access::setRecordDir(app, (g_scratch / "export-after").string());
    Access::exportBookmarks(app);
    CHECK(pumpApp(app, 5000.0,
                  [&] { return Access::exportNote(app).rfind("Exported 2 to ", 0) == 0; }));
}

void checkTheScreenshotNeverHoldsAFrame() {
    AppWindow app;
    const fs::path dir = g_scratch / "shot-slow";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto disk = makeDisk(2500);
    Access::setHook(app, slowHook(disk));
    const std::string windows = "0\t0\t800\t600\tDebug\n10\t20\t300\t200\tSource\n";
    stallCase("screenshot", app, disk, [&] {
        Access::shot(app, dir.string(), 1, testImage(), windows);
        // F12 again while the first is still being written: skipped, said in the
        // log, never a second worker and never a stalled frame.
        Access::shot(app, dir.string(), 2, testImage(), windows);
    });
    const std::vector<fs::path> bmps = filesIn(dir, ".bmp");
    CHECK(bmps.size() == 1u);
    if (bmps.size() == 1u) {
        const fs::path ref = g_scratch / "shot-ref.bmp";
        std::string err;
        CHECK(cascade::core::writeBmp24(testImage(), ref.string(), err));
        CHECK(readAll(bmps[0]) == readAll(ref));
    }
    CHECK(readAll(dir / "shot-1.windows.txt") == windows);
    CHECK(!fs::exists(dir / "shot-2.bmp", ec));
    // Free again once it has been written.
    disk->blockMs.store(0);
    Access::shot(app, dir.string(), 3, testImage(), windows);
    CHECK(pumpApp(app, 8000.0, [&] { return fs::exists(dir / "shot-3.bmp", ec); }));
}

// A picture that cannot be written (the folder is not there) is a log line and
// nothing else: no throw reaches the frame loop and the next one still works.
void checkAFailedScreenshotIsContained() {
    AppWindow app;
    const fs::path dir = g_scratch / "shot-nowhere";
    Access::shot(app, (dir / "missing").string(), 1, testImage(), "x\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Access::poll(app);
    std::error_code ec;
    fs::create_directories(dir, ec);
    Access::shot(app, dir.string(), 2, testImage(), "y\n");
    CHECK(pumpApp(app, 5000.0, [&] { return fs::exists(dir / "shot-2.bmp", ec); }));
    CHECK(!fs::exists(dir / "missing" / "shot-1.bmp", ec));
}

// --- 3. THE EDGES --------------------------------------------------------------

// APPLICATION EXIT WITH EVERY WORKER WEDGED: the window is destroyed without
// waiting for the disk (bounded by the quit grace for each, not by the 2.5 s the
// disk is taking), and the abandoned workers finish by themselves.
void checkExitWhileEveryWorkerIsWedged() {
    auto disk = makeDisk(2500);
    double destroyMs = 0.0;
    {
        auto app = std::make_unique<AppWindow>();
        const fs::path dir = g_scratch / "exit";
        makeRecording(dir);
        Access::setRecordDir(*app, dir.string());
        Access::addBookmarks(*app, 2);
        Access::setHook(*app, slowHook(disk));
        Access::listRecordings(*app);
        Access::saveImage(*app, testImage());
        Access::exportBookmarks(*app);
        Access::shot(*app, dir.string(), 1, testImage(), "z\n");
        CHECK(Access::imagePending(*app));
        CHECK(Access::exportPending(*app));
        const double t0 = nowMs();
        app.reset();
        destroyMs = nowMs() - t0;
    }
    // Four wedged workers, each given the 250 ms grace in ~AppWindow and then let
    // go: well inside the 2.5 s the disk is taking.
    CHECK(destroyMs < 2000.0);
    std::printf("  exit with four workers wedged: ~AppWindow took %.0f ms\n", destroyMs);
    const double until = nowMs() + 10000.0;
    while (nowMs() < until && disk->finished.load() < 4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(disk->finished.load() == 4);  // the abandoned workers ran to the end by themselves
}

}  // namespace

int main() {
    isolate();
    ImGui::CreateContext();
    checkTheSynchronousCallIsReported();
    checkTheRecordingsListNeverHoldsAFrame();
    checkASecondListingIsRemembered();
    checkThePictureSaveNeverHoldsAFrame();
    checkAFailedPictureSaveSaysWhatItSaid();
    checkTheExportNeverHoldsAFrame();
    checkAFailedExportSaysWhatItSaid();
    checkTheScreenshotNeverHoldsAFrame();
    checkAFailedScreenshotIsContained();
    checkExitWhileEveryWorkerIsWedged();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_gui_file_jobs");
}
