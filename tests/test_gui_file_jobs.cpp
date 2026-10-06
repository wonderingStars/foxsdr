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
//   * (0.99.65) the Bookmarks section's "Import", and a list dropped on the window:
//     the file the user typed read and parsed inline.
//   * (0.99.66) the AIRBAND section's presets ride the same two workers: Import into a
//     group (the group and the tick put on every row by pollDiskJobs, one preset per
//     remembered request) and Export CSV of a group (checkAnImportIntoAPreset... to
//     checkAnExportOfNothing...). The review's fixes follow them (checkATypedFrequency...
//     to checkAPresetImportSaysShiftValues...): a typed frequency is whole hertz, Add and
//     Import leave a listening monitor's choice alone, Remove preset stops the monitor that
//     plays that preset, a throwing export worker still answers the section that asked, and
//     a preset import says when SDR#'s converter Shift was not applied.
//   * (0.99.65) a patch Radio whose device is an I/Q recording: its header opened
//     inline in patchReconcile, when the patch starts or the node's device changes.
//     (Before the fix: worst frame gap 2514 ms and a hang report for the import,
//     2503 ms and a hang report for the recording; after, about 25 ms and none.)
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
#include <stdexcept>
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
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"
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
    // The Import button and a file dropped on the window both end here.
    static void importFile(AppWindow& a, const std::string& path) { a.importBookmarkFile(path); }
    static bool importPending(AppWindow& a) { return a.bookmarkImportPending(); }

    // THE AIRBAND SECTION'S PRESETS (0.99.66). Import into a group, the notes it and
    // the export say in that section, and the keys' own members.
    static void importInto(AppWindow& a, const std::string& path, const std::string& group, bool tick) {
        a.importBookmarkFile(path, cascade::core::ImportInto{group, tick});
    }
    static void exportGroup(AppWindow& a, const std::string& group) { a.exportGroupCsv(group); }
    static std::string presetNote(AppWindow& a) { return a.airbandPresetNote_; }
    static bool presetFolderKey(AppWindow& a) { return a.airbandPresetFolderKey_; }
    static std::string airbandGroup(AppWindow& a) { return a.airbandGroup_; }
    static void setPresetField(AppWindow& a, const std::string& name) {
        std::snprintf(a.airbandPresetName_, sizeof(a.airbandPresetName_), "%s", name.c_str());
    }
    static std::string presetName(AppWindow& a) { return a.airbandPresetName(); }
    // The monitor's state, forced: a window with no frame loop and no radio cannot LISTEN, but
    // the keys' own rules about a monitor that is listening are decided by these three members
    // alone (airbandRemovePreset, airbandShowGroup) and are checked against them.
    static void setListening(AppWindow& a, bool on) { a.airbandListening_ = on; }
    static bool listening(AppWindow& a) { return a.airbandListening_; }
    static void setAirbandGroup(AppWindow& a, const std::string& g) { a.airbandGroup_ = g; }
    static std::string airbandNote(AppWindow& a) { return a.airbandNote_; }
    static void setAirbandNote(AppWindow& a, const std::string& n) { a.airbandNote_ = n; }
    // The add row's Add key with the frequency as typed, the mode's index (0 AM, 1 NFM), a name.
    static void addManual(AppWindow& a, const char* mhz, int mode, const char* name) {
        std::snprintf(a.airbandAddMhz_, sizeof(a.airbandAddMhz_), "%s", mhz);
        std::snprintf(a.airbandAddName_, sizeof(a.airbandAddName_), "%s", name);
        a.airbandAddMode_ = mode;
        a.airbandAddManual();
    }
    // How many channels the monitor would play from what is ticked.
    static std::size_t wantedChannels(AppWindow& a) { return a.airbandWanted().size(); }
    // The channels the monitor would play, and the rows that name them (what airbandStart
    // takes from airbandWanted, and what the running monitor compares each frame).
    static std::vector<cascade::core::MonitorChannel> wanted(AppWindow& a,
                                                             std::vector<std::string>& names) {
        return a.airbandWanted(&names);
    }
    // The channel table airbandStart builds from that list - the receiver, the block plan and
    // the runner left out, which the heard-time flush does not read - and the heard time a
    // channel has behind it that the flush has not yet put into the list.
    static void openChannels(AppWindow& a) {
        std::vector<std::string> names;
        const std::vector<cascade::core::MonitorChannel> want = a.airbandWanted(&names);
        a.airbandChans_.clear();
        for (std::size_t i = 0; i < want.size(); ++i) {
            AppWindow::AirbandChan c;
            c.freqHz = want[i].freqHz;
            c.bandwidthHz = want[i].bandwidthHz;
            c.name = names[i];
            a.airbandChans_.push_back(std::move(c));
        }
    }
    static void setPendingHeard(AppWindow& a, const std::string& name, double seconds) {
        for (AppWindow::AirbandChan& c : a.airbandChans_) {
            if (c.name == name) { c.pendingHeardS = seconds; }
        }
    }
    static double pendingHeard(AppWindow& a) {
        double s = 0.0;
        for (const AppWindow::AirbandChan& c : a.airbandChans_) { s += c.pendingHeardS; }
        return s;
    }
    // The monitor's 20-second write of the heard time into the list.
    static void flushHeard(AppWindow& a) { a.airbandFlushHeard(); }
    // The Import key with the field and the path as typed.
    static void importPreset(AppWindow& a, const std::string& name, const std::string& path) {
        setPresetField(a, name);
        std::snprintf(a.airbandImportPath_, sizeof(a.airbandImportPath_), "%s", path.c_str());
        a.airbandImportPreset();
    }
    static void removePreset(AppWindow& a, const std::string& name) {
        setPresetField(a, name);
        a.airbandRemovePreset();
    }
    static void addRow(AppWindow& a, const std::string& name, double freqHz, const char* mode,
                       const std::string& group, bool ticked, bool favourite = false) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = freqHz;
        b.mode = mode;
        b.bandwidthHz = cascade::core::defaultBandwidthForMode(b.mode);
        b.group = group;
        b.scan = ticked;
        b.favourite = favourite;
        a.freqMgr_.add(std::move(b));
    }

    // The patch page's Radio node on an I/Q recording (0.99.65): reconciled once a
    // frame, as the page does, until its radio runs.
    static cascade::core::patch::NodeId addFileRadio(AppWindow& a, const std::string& path) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = true;  // no starter patch: this test builds its own
        const pc::NodeId id = a.patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
        if (pc::Node* n = a.patchGraph_.mutableNode(id)) {
            n->device = pc::makeIqFileKey(path);
            n->freqHz = 100.0e6;
            n->rateHz = 2.4e6;
            n->on = true;
        }
        return id;
    }
    static void reconcile(AppWindow& a) {
        a.patchRunning_ = true;
        a.patchWasOpen_ = true;  // the page's first-frame scan is not under test
        a.patchReconcile();
    }
    static bool radioRunning(AppWindow& a, cascade::core::patch::NodeId id) {
        return a.patchRadios_.count(id) != 0;
    }
    static bool radioPending(AppWindow& a, cascade::core::patch::NodeId id) {
        return a.patchRadioPending_.count(id) != 0;
    }
    static std::string radioError(AppWindow& a, cascade::core::patch::NodeId id) {
        const auto it = a.patchRadioError_.find(id);
        return it == a.patchRadioError_.end() ? std::string() : it->second;
    }
    static double nodeRate(AppWindow& a, cascade::core::patch::NodeId id) {
        const cascade::core::patch::Node* n = a.patchGraph_.find(id);
        return n == nullptr ? -1.0 : n->rateHz;
    }
    static void setNodeOn(AppWindow& a, cascade::core::patch::NodeId id, bool on) {
        if (cascade::core::patch::Node* n = a.patchGraph_.mutableNode(id)) { n->on = on; }
    }
    static void setNodeDevice(AppWindow& a, cascade::core::patch::NodeId id, const std::string& path) {
        if (cascade::core::patch::Node* n = a.patchGraph_.mutableNode(id)) {
            n->device = cascade::core::patch::makeIqFileKey(path);
        }
    }

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

// A frequency list to import: three CSV rows, or `n` rows starting at `first` MHz.
std::string writeCsv(const char* tag, int n, double firstMhz) {
    const fs::path p = g_scratch / (std::string(tag) + ".csv");
    std::ofstream f(p, std::ios::binary);
    f << "frequency,name,mode\n";
    for (int i = 0; i < n; ++i) {
        f << static_cast<long long>((firstMhz + 1.1 * i) * 1.0e6) << ",Import " << tag << " " << i
          << ",NFM\n";
    }
    return p.string();
}

void checkTheImportNeverHoldsAFrame() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "import-slow").string());
    const std::string csv = writeCsv("slow", 3, 101.0);
    auto disk = makeDisk(2500);
    Access::setHook(app, slowHook(disk));
    bool sawPending = false;
    stallCase("bookmark import", app, disk, [&] {
        Access::importFile(app, csv);
        sawPending = Access::importPending(app);
    });
    CHECK(sawPending);
    CHECK(!Access::importPending(app));
    // What was read, in the words the synchronous import used.
    CHECK(Access::bookmarks(app).size() == 3u);
    CHECK(Access::exportNote(app) == "CSV: 3 entries read, 3 added");
}

// An import asked for while one is out is REMEMBERED (a file dropped on the window
// is not lost), the last one asked for wins, and it is run when the first is back.
void checkASecondImportIsRemembered() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "import-again").string());
    const std::string a = writeCsv("first", 2, 100.0);
    const std::string b = writeCsv("second", 2, 120.0);
    const std::string c = writeCsv("third", 3, 140.0);
    auto disk = makeDisk(500);
    Access::setHook(app, slowHook(disk));
    Access::importFile(app, a);
    Access::importFile(app, b);  // remembered...
    Access::importFile(app, c);  // ...and replaced: only the last asked for runs
    CHECK(Access::importPending(app));
    CHECK(pumpApp(app, 12000.0, [&] { return disk->finished.load() == 2 && !Access::importPending(app); }));
    CHECK(disk->calls.load() == 2);
    CHECK(Access::bookmarks(app).size() == 5u);  // a's two and c's three
    CHECK(Access::exportNote(app) == "CSV: 3 entries read, 3 added");
}

// The same words as before for a file that can be read: one pass, one note.
void checkAnImportSaysWhatItSaid() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "import-ok").string());
    const std::string csv = writeCsv("ok", 4, 90.0);
    Access::importFile(app, "\"" + csv + "\"");  // a path pasted with Explorer's quotes
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::bookmarks(app).size() == 4u);
    CHECK(Access::exportNote(app) == "CSV: 4 entries read, 4 added");
    // A file that is not there: the reason, in the words it always had; nothing added.
    Access::importFile(app, (g_scratch / "no-such-list.csv").string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::exportNote(app).rfind("Could not import: ", 0) == 0);
    CHECK(Access::bookmarks(app).size() == 4u);
    // A folder squatting on the path: also a reason, also nothing added, no hang.
    const fs::path dirAsFile = g_scratch / "import-is-a-folder.csv";
    std::error_code ec;
    fs::create_directories(dirAsFile, ec);
    Access::importFile(app, dirAsFile.string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::exportNote(app).rfind("Could not import: ", 0) == 0);
    CHECK(Access::bookmarks(app).size() == 4u);
}

// --- THE AIRBAND SECTION'S PRESETS (0.99.66) --------------------------------------
//
// A preset is a group of the frequency list. Its Import is the Bookmarks section's, with
// the group and the tick put on every row on the window's thread; its Export CSV is the
// SDR# export's worker with another text; both say what they did in the AIRBAND section.

// Four rows whose files carry other groups and no tick: three the monitor can play
// (AM, NFM) and one it cannot (WFM).
std::string writeMixedCsv(const char* tag) {
    const fs::path p = g_scratch / (std::string(tag) + ".csv");
    std::ofstream f(p, std::ios::binary);
    f << "frequency_mhz,name,group,mode,bandwidth_hz\n"
         "156.800,Ch 16,Somewhere,NFM,12500\n"
         "156.375,Ch 75,Elsewhere,NFM,12500\n"
         "118.700,Tower,,AM,10000\n"
         "98.500,Broadcast,Radio,WFM,150000\n";
    return p.string();
}

const cascade::core::Bookmark* rowNamed(const std::vector<cascade::core::Bookmark>& list,
                                        const std::string& name) {
    for (const cascade::core::Bookmark& b : list) {
        if (b.name == name) { return &b; }
    }
    return nullptr;
}

// THE GROUP AND THE TICK ARE FORCED on every row, whatever the file wrote - and the rest
// of each row is the file's; the note is the AIRBAND section's and counts what the monitor
// plays; the Bookmarks section's own Import is untouched by all of it.
void checkAnImportIntoAPresetForcesTheGroupAndTheTick() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "preset-import").string());
    const std::string csv = writeMixedCsv("preset-mixed");
    Access::importInto(app, csv, "Harbour", true);
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
    CHECK(list.size() == 4u);
    for (const cascade::core::Bookmark& b : list) {
        CHECK(b.group == "Harbour");   // not Somewhere, Elsewhere, Radio, or none
        // The file had no tick: the rows the monitor plays (AM, NFM) are ticked; the WFM one is
        // not - ticked, it would be in the Scanner's list mode with no row in this section to
        // untick it by.
        CHECK(b.scan == (b.mode == "AM" || b.mode == "NFM"));
    }
    const cascade::core::Bookmark* ch16 = rowNamed(list, "Ch 16");
    const cascade::core::Bookmark* wfm = rowNamed(list, "Broadcast");
    CHECK(ch16 != nullptr && wfm != nullptr);
    if (ch16 != nullptr && wfm != nullptr) {
        CHECK(ch16->freqHz == 156800000.0 && ch16->mode == "NFM" && ch16->bandwidthHz == 12500.0);
        // A row the monitor cannot play is still a bookmark of the preset.
        CHECK(wfm->freqHz == 98500000.0 && wfm->mode == "WFM" && wfm->bandwidthHz == 150000.0);
        CHECK(!wfm->scan && ch16->scan);   // unticked, beside the NFM row that is
    }
    CHECK(Access::presetNote(app) ==
          "Preset \"Harbour\": 4 read, 4 new; the monitor can play 3 of the rows read (AM or NFM).");
    CHECK(Access::exportNote(app).empty());          // nothing under the Bookmarks section's Import
    CHECK(Access::airbandGroup(app) == "Harbour");   // the preset the rows joined is showing
    CHECK(!Access::presetFolderKey(app));

    // The same file again: nothing new, said so.
    Access::importInto(app, csv, "Harbour", true);
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::bookmarks(app).size() == 4u);
    CHECK(Access::presetNote(app) ==
          "Preset \"Harbour\": 4 read, 0 new; the monitor can play 3 of the rows read (AM or NFM).");

    // A file that is not there: the reason, in this section, and nothing added.
    Access::importInto(app, (g_scratch / "no-such-preset.csv").string(), "Harbour", true);
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::presetNote(app).rfind("Could not import: ", 0) == 0);
    CHECK(Access::exportNote(app).empty());
    CHECK(Access::bookmarks(app).size() == 4u);

    // THE BOOKMARKS SECTION'S IMPORT, and a dropped file, are as they were: the file's own
    // groups, nothing ticked, the note under that section's key.
    AppWindow plain;
    Access::setRecordDir(plain, (g_scratch / "preset-import-plain").string());
    Access::importFile(plain, csv);
    CHECK(pumpApp(plain, 5000.0, [&] { return !Access::importPending(plain); }));
    list = Access::bookmarks(plain);
    CHECK(list.size() == 4u);
    const cascade::core::Bookmark* tower = rowNamed(list, "Tower");
    const cascade::core::Bookmark* radio = rowNamed(list, "Broadcast");
    CHECK(tower != nullptr && radio != nullptr);
    if (tower != nullptr && radio != nullptr) {
        CHECK(tower->group.empty() && !tower->scan);
        CHECK(radio->group == "Radio" && !radio->scan);
    }
    CHECK(Access::exportNote(plain) == "CSV: 4 entries read, 4 added");
    CHECK(Access::presetNote(plain).empty());
    CHECK(Access::airbandGroup(plain).empty());
    // ...and a file that carries the tick keeps it (an exported preset, imported plain).
    {
        std::vector<cascade::core::Bookmark> one(2);
        one[0].name = "Kept";
        one[0].freqHz = 121.5e6;
        one[0].mode = "AM";
        one[0].bandwidthHz = 10000.0;
        one[0].scan = true;
        one[1].name = "Left";
        one[1].freqHz = 121.6e6;
        one[1].mode = "AM";
        one[1].bandwidthHz = 10000.0;
        const fs::path p = g_scratch / "preset-with-tick.csv";
        {
            std::ofstream f(p, std::ios::binary);
            f << cascade::core::exportCsv(one);
        }
        AppWindow ticked;
        Access::setRecordDir(ticked, (g_scratch / "preset-import-ticked").string());
        Access::importFile(ticked, p.string());
        CHECK(pumpApp(ticked, 5000.0, [&] { return !Access::importPending(ticked); }));
        const std::vector<cascade::core::Bookmark> got = Access::bookmarks(ticked);
        const cascade::core::Bookmark* kept = rowNamed(got, "Kept");
        const cascade::core::Bookmark* left = rowNamed(got, "Left");
        CHECK(kept != nullptr && left != nullptr);
        if (kept != nullptr && left != nullptr) { CHECK(kept->scan && !left->scan); }
    }
}

// AN IMPORT ASKED FOR WHILE ONE IS OUT KEEPS ITS OWN PRESET: remembered with the group it
// was pressed for, the last one asked for wins, and a plain one remembered behind a preset's
// is plain - the first's group does not leak into it, nor the second's into the first.
void checkASecondPresetImportKeepsItsOwnGroup() {
    {
        AppWindow app;
        Access::setRecordDir(app, (g_scratch / "preset-again").string());
        const std::string a = writeCsv("preset-first", 2, 100.0);
        const std::string b = writeCsv("preset-second", 2, 120.0);
        const std::string c = writeCsv("preset-third", 3, 140.0);
        auto disk = makeDisk(500);
        Access::setHook(app, slowHook(disk));
        Access::importInto(app, a, "Alpha", true);
        Access::importInto(app, b, "Beta", true);    // remembered...
        Access::importInto(app, c, "Gamma", false);  // ...and replaced, with ITS group and tick
        CHECK(Access::importPending(app));
        CHECK(pumpApp(app, 12000.0, [&] { return disk->finished.load() == 2 && !Access::importPending(app); }));
        CHECK(disk->calls.load() == 2);
        const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
        CHECK(list.size() == 5u);   // a's two and c's three; b's were never read
        std::size_t alpha = 0, gamma = 0, other = 0;
        for (const cascade::core::Bookmark& b2 : list) {
            if (b2.group == "Alpha" && b2.scan) { ++alpha; }
            else if (b2.group == "Gamma" && !b2.scan) { ++gamma; }
            else { ++other; }
        }
        CHECK(alpha == 2u && gamma == 3u && other == 0u);
        CHECK(Access::presetNote(app) ==
              "Preset \"Gamma\": 3 read, 3 new; the monitor can play 3 of the rows read (AM or NFM).");
    }
    {
        // A PLAIN import out, a preset's remembered behind it: the first is plain, the second is Beta's.
        AppWindow app;
        Access::setRecordDir(app, (g_scratch / "preset-again-2").string());
        const std::string a = writeCsv("preset-plain", 2, 100.0);
        const std::string b = writeCsv("preset-beta", 2, 120.0);
        auto disk = makeDisk(400);
        Access::setHook(app, slowHook(disk));
        Access::importFile(app, a);
        Access::importInto(app, b, "Beta", true);
        CHECK(pumpApp(app, 12000.0, [&] { return disk->finished.load() == 2 && !Access::importPending(app); }));
        const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
        CHECK(list.size() == 4u);
        std::size_t beta = 0, plain = 0;
        for (const cascade::core::Bookmark& b2 : list) {
            if (b2.group == "Beta" && b2.scan) { ++beta; }
            else if (b2.group.empty() && !b2.scan) { ++plain; }
        }
        CHECK(beta == 2u && plain == 2u);
        CHECK(Access::exportNote(app) == "CSV: 2 entries read, 2 added");   // the plain one's, where it asked
        CHECK(Access::presetNote(app) ==
              "Preset \"Beta\": 2 read, 2 new; the monitor can play 2 of the rows read (AM or NFM).");
    }
    {
        // A PRESET'S import out, a plain one remembered behind it: the second is NOT Alpha's.
        AppWindow app;
        Access::setRecordDir(app, (g_scratch / "preset-again-3").string());
        const std::string a = writeCsv("preset-alpha", 2, 100.0);
        const std::string b = writeCsv("preset-loose", 2, 120.0);
        auto disk = makeDisk(400);
        Access::setHook(app, slowHook(disk));
        Access::importInto(app, a, "Alpha", true);
        Access::importFile(app, b);
        CHECK(pumpApp(app, 12000.0, [&] { return disk->finished.load() == 2 && !Access::importPending(app); }));
        const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
        CHECK(list.size() == 4u);
        std::size_t alpha = 0, plain = 0;
        for (const cascade::core::Bookmark& b2 : list) {
            if (b2.group == "Alpha" && b2.scan) { ++alpha; }
            else if (b2.group.empty() && !b2.scan) { ++plain; }
        }
        CHECK(alpha == 2u && plain == 2u);
    }
}

// THE KEY'S OWN READING OF THE FIELD: the name trimmed, "Manual" when nothing is left, and
// the Import key's rows going to that name.
void checkThePresetNameAndTheImportKey() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "preset-name").string());
    CHECK(Access::presetName(app) == "Manual");   // the field's default
    Access::setPresetField(app, "  Marine  ");
    CHECK(Access::presetName(app) == "Marine");
    Access::setPresetField(app, "Tower Zone 2");
    CHECK(Access::presetName(app) == "Tower Zone 2");
    Access::setPresetField(app, "");
    CHECK(Access::presetName(app) == "Manual");
    Access::setPresetField(app, " \t ");
    CHECK(Access::presetName(app) == "Manual");

    Access::importPreset(app, "  Marine ", writeMixedCsv("preset-key"));
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
    CHECK(list.size() == 4u);
    for (const cascade::core::Bookmark& b : list) {
        CHECK(b.group == "Marine" && b.scan == (b.mode != "WFM"));   // the WFM row joins unticked
    }
    // An empty name is Manual.
    Access::importPreset(app, "", writeCsv("preset-key-manual", 2, 130.0));
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    std::size_t manual = 0;
    for (const cascade::core::Bookmark& b : Access::bookmarks(app)) { manual += (b.group == "Manual") ? 1u : 0u; }
    CHECK(manual == 2u);

    // Remove preset takes the group's rows out - every mode, ticked or not - and leaves the rest.
    CHECK(Access::airbandGroup(app) == "Manual");   // the last one imported into is showing
    Access::removePreset(app, "Marine");            // not the one showing: it keeps showing
    CHECK(Access::bookmarks(app).size() == 2u);
    CHECK(Access::presetNote(app) == "Removed 4 from \"Marine\"");
    CHECK(Access::airbandGroup(app) == "Manual");
    Access::removePreset(app, "Manual");            // the one showing: the list goes back to "every ticked"
    CHECK(Access::bookmarks(app).empty());
    CHECK(Access::presetNote(app) == "Removed 2 from \"Manual\"");
    CHECK(Access::airbandGroup(app).empty());
    Access::removePreset(app, "Manual");            // again: nothing there to remove
    CHECK(Access::presetNote(app) == "Removed 0 from \"Manual\"");
}

// EXPORT CSV writes the preset's rows - and only those - as the file the importer reads,
// tick and favourite and a comma in a name kept, on the SDR# export's worker; the note names the
// file and the section says so; the folder key follows a file that was written.
void checkAnExportOfAPresetWritesOnlyThatGroup() {
    AppWindow app;
    const fs::path dir = g_scratch / "preset-export";
    Access::setRecordDir(app, dir.string());
    Access::addRow(app, "Ch 16, north", 156.8e6, "NFM", "Harbour", true, true);
    Access::addRow(app, "Pilot \"VHF\"", 156.65e6, "AM", "Harbour", false);
    Access::addRow(app, "Other group", 118.7e6, "AM", "Other", true);
    Access::addRow(app, "Loose row", 98.5e6, "WFM", "", true);
    std::vector<cascade::core::Bookmark> want;
    for (const cascade::core::Bookmark& b : Access::bookmarks(app)) {
        if (b.group == "Harbour") { want.push_back(b); }
    }
    CHECK(want.size() == 2u);

    auto disk = makeDisk(300);
    Access::setHook(app, slowHook(disk));
    Access::exportGroup(app, "Harbour");
    CHECK(Access::exportPending(app));
    CHECK(Access::presetNote(app) == "Saving...");
    Access::exportGroup(app, "Harbour");   // pressed again: one export is ever out
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::exportPending(app); }));
    CHECK(disk->calls.load() == 1);

    const std::vector<fs::path> files = filesIn(dir, ".csv");
    CHECK(files.size() == 1u);
    CHECK(filesIn(dir, ".xml").empty());
    if (files.size() == 1u) {
        const std::string name = files[0].filename().string();
        CHECK(name.rfind("foxsdr-Harbour-", 0) == 0u);
        CHECK(name.size() > 4u && name.compare(name.size() - 4, 4, ".csv") == 0);
        const std::string text = readAll(files[0]);
        CHECK(text == cascade::core::exportCsv(want));
        CHECK(text.find("Other group") == std::string::npos);
        CHECK(text.find("Loose row") == std::string::npos);
        CHECK(Access::presetNote(app) == "Exported 2 to " + dir.string() + "/" + name);
        CHECK(Access::presetFolderKey(app));
        CHECK(Access::exportNote(app).empty());   // not under the Bookmarks section's keys

        // What the file holds comes back as the preset: the importer, into a window of its own.
        AppWindow back;
        Access::setRecordDir(back, (g_scratch / "preset-export-back").string());
        Access::importFile(back, files[0].string());
        CHECK(pumpApp(back, 5000.0, [&] { return !Access::importPending(back); }));
        const std::vector<cascade::core::Bookmark> got = Access::bookmarks(back);
        CHECK(got.size() == 2u);
        const cascade::core::Bookmark* a = rowNamed(got, "Ch 16, north");
        const cascade::core::Bookmark* b = rowNamed(got, "Pilot \"VHF\"");
        CHECK(a != nullptr && b != nullptr);
        if (a != nullptr && b != nullptr) {
            CHECK(a->group == "Harbour" && a->scan && a->favourite && a->mode == "NFM" && a->freqHz == 156800000.0);
            CHECK(b->group == "Harbour" && !b->scan && !b->favourite && b->mode == "AM" && b->freqHz == 156650000.0);
        }
    }

    // A name that is not a file name is made one: letters, digits, '-' and '_' stay.
    Access::addRow(app, "Tower row", 118.9e6, "AM", "Tower & Co. 1", true);
    Access::setHook(app, nullptr);
    Access::exportGroup(app, "Tower & Co. 1");
    CHECK(pumpApp(app, 8000.0, [&] { return !Access::exportPending(app); }));
    bool named = false;
    for (const fs::path& f : filesIn(dir, ".csv")) {
        named = named || f.filename().string().rfind("foxsdr-Tower___Co__1-", 0) == 0u;
    }
    CHECK(named);
}

// A GROUP WITH NO ROWS EXPORTS NOTHING and says so; a folder that cannot be written says
// that instead, and no key to open it is offered.
void checkAnExportOfNothingAndOfABlockedFolder() {
    AppWindow app;
    const fs::path dir = g_scratch / "preset-export-none";
    Access::setRecordDir(app, dir.string());
    Access::addRow(app, "Only row", 118.7e6, "AM", "Elsewhere", true);
    Access::exportGroup(app, "Nothing");
    CHECK(!Access::exportPending(app));   // no worker was started
    CHECK(Access::presetNote(app) == "Nothing to export: the preset \"Nothing\" has no rows.");
    CHECK(!Access::presetFolderKey(app));
    std::error_code ec;
    CHECK(filesIn(dir, ".csv").empty());
    CHECK(!fs::exists(dir, ec));          // not even the folder

    AppWindow blocked;
    Access::setRecordDir(blocked, blockedDir("preset-export-blocker"));
    Access::addRow(blocked, "Row", 118.7e6, "AM", "Harbour", true);
    Access::exportGroup(blocked, "Harbour");
    CHECK(pumpApp(blocked, 5000.0, [&] { return !Access::exportPending(blocked); }));
    CHECK(Access::presetNote(blocked).rfind("Could not write ", 0) == 0);
    CHECK(!Access::presetFolderKey(blocked));
    CHECK(Access::exportNote(blocked).empty());
    // And once the folder is there, the same window exports.
    Access::setRecordDir(blocked, (g_scratch / "preset-export-after").string());
    Access::exportGroup(blocked, "Harbour");
    CHECK(pumpApp(blocked, 5000.0, [&] { return Access::presetNote(blocked).rfind("Exported 1 to ", 0) == 0; }));
    CHECK(Access::presetFolderKey(blocked));
}

// --- THE 0.99.66 REVIEW'S FIXES, through the keys' own members ---------------------------

// A TYPED FREQUENCY IS THE LIST'S OWN CHANNEL: 128.050 typed into the add row is 128050000 Hz,
// not 128050000.00000001, so a ticked row an airport or a CSV put at 128050000 and the typed one
// are ONE channel for the monitor (it compares frequencies exactly), not two strips on one carrier.
void checkATypedFrequencyIsTheSameChannelAsTheListsOwn() {
    AppWindow app;
    Access::addRow(app, "Tower", 128050000.0, "AM", "KXYZ", true);
    Access::addManual(app, "128.050", 0, "Typed");
    const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
    CHECK(list.size() == 2u);
    const cascade::core::Bookmark* typed = rowNamed(list, "Typed");
    CHECK(typed != nullptr);
    if (typed != nullptr) {
        CHECK(typed->freqHz == 128050000.0);          // exactly: whole hertz
        CHECK(typed->group == "Manual" && typed->scan && typed->mode == "AM");
    }
    Access::setAirbandGroup(app, "");   // "every ticked": both rows count
    CHECK(Access::wantedChannels(app) == 1u);
    // Every 25 kHz channel typed the way a person types it lands on the integer the list holds.
    for (long long khz = 118000; khz <= 137000; khz += 25) {
        char text[32];
        std::snprintf(text, sizeof(text), "%lld.%03lld", khz / 1000, khz % 1000);
        Access::addRow(app, "Listed", static_cast<double>(khz * 1000), "AM", "Listed", true);
        Access::addManual(app, text, 0, "TypedAgain");
    }
    Access::setAirbandGroup(app, "");
    CHECK(Access::wantedChannels(app) == 761u);   // 761 frequencies, each typed AND listed: no doubles
}

// THE HEARD-TIME FLUSH DOES NOT HAND A FREQUENCY TO THE OTHER ROW: "High" (AM) and "HighNfm"
// (NFM) are both ticked on 120.300 MHz, which is one channel, named and moded by the first of
// them. The monitor writes the heard seconds of a channel into its row every 20 s through
// FreqManager::updateAt, which used to re-insert the row AFTER its peers on that frequency:
// "High" went behind "HighNfm", airbandWanted() named the NFM row, the running monitor saw a
// different set (it compares frequency, width and mode) and restarted on the NFM row, which was
// credited from then on. (CI's arm64 run 37462575079: High 19.95 s, HighNfm 6.08 s.)
void checkTheHeardTimeFlushKeepsTheChannelOnItsRow() {
    AppWindow app;
    Access::addRow(app, "High", 120.3e6, "AM", "Manual", true);
    Access::addRow(app, "HighNfm", 120.3e6, "NFM", "Manual", true);
    Access::addRow(app, "Low", 119.7e6, "AM", "Manual", true);
    Access::setAirbandGroup(app, "");   // "every ticked"

    std::vector<std::string> namesBefore;
    const std::vector<cascade::core::MonitorChannel> before = Access::wanted(app, namesBefore);
    CHECK(before.size() == 2u);          // 119.700 and 120.300: the NFM row is no third channel
    CHECK(namesBefore.size() == 2u);
    if (before.size() != 2u || namesBefore.size() != 2u) { return; }
    CHECK(namesBefore[0] == "Low");
    CHECK(namesBefore[1] == "High");     // the first ticked row on 120.300: the AM one
    CHECK(before[1].mode == cascade::core::MonitorMode::Am);

    // The monitor is listening to them, and "High" has been heard for 7.25 s since the last flush.
    Access::openChannels(app);
    Access::setPendingHeard(app, "High", 7.25);
    CHECK(Access::pendingHeard(app) == 7.25);
    Access::flushHeard(app);
    CHECK(Access::pendingHeard(app) == 0.0);

    // The seconds went to the row that was heard, and only to it.
    {
        const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
        const cascade::core::Bookmark* high = rowNamed(list, "High");
        const cascade::core::Bookmark* nfm = rowNamed(list, "HighNfm");
        const cascade::core::Bookmark* low = rowNamed(list, "Low");
        CHECK(high != nullptr && nfm != nullptr && low != nullptr);
        if (high != nullptr && nfm != nullptr && low != nullptr) {
            CHECK(high->heardSeconds == 7.25);
            CHECK(nfm->heardSeconds == 0.0);
            CHECK(low->heardSeconds == 0.0);
        }
    }

    // And the monitor's choice is the same set, so it keeps playing what it was playing: the
    // same rows by name, and the same frequency, width and mode for each (what airbandFrame compares).
    std::vector<std::string> namesAfter;
    const std::vector<cascade::core::MonitorChannel> after = Access::wanted(app, namesAfter);
    CHECK(namesAfter == namesBefore);
    CHECK(after.size() == before.size());
    for (std::size_t i = 0; i < after.size() && i < before.size(); ++i) {
        CHECK(after[i].freqHz == before[i].freqHz);
        CHECK(after[i].bandwidthHz == before[i].bandwidthHz);
        CHECK(after[i].mode == before[i].mode);
    }

    // A flush every 20 s for a long listen: still the same row, and every flush went to it.
    for (int i = 0; i < 5; ++i) {
        Access::setPendingHeard(app, "High", 1.0);
        Access::flushHeard(app);
    }
    std::vector<std::string> namesLater;
    Access::wanted(app, namesLater);
    CHECK(namesLater == namesBefore);
    const std::vector<cascade::core::Bookmark> list = Access::bookmarks(app);
    const cascade::core::Bookmark* high = rowNamed(list, "High");
    const cascade::core::Bookmark* nfm = rowNamed(list, "HighNfm");
    if (high != nullptr && nfm != nullptr) {
        CHECK(high->heardSeconds == 12.25);
        CHECK(nfm->heardSeconds == 0.0);
    } else {
        CHECK(high != nullptr && nfm != nullptr);
    }
}

// ADD AND IMPORT DO NOT CHANGE WHAT A LISTENING MONITOR PLAYS: with the monitor on, "every
// ticked" stays "every ticked" and another preset's rows stay unplayed - setting the group on
// show narrowed the playing set to the preset the row went to. Not listening, they put the
// preset on show as before, but only a preset the monitor has something to play in: an import of
// WFM rows alone adds bookmarks and leaves the combo where it was.
void checkAddAndImportKeepAListeningMonitorsChoice() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "listen-keep").string());
    Access::addRow(app, "Tower", 118.7e6, "AM", "KXYZ", true);

    // Not listening: Add puts its preset on show (the airport lookup's way).
    Access::addManual(app, "121.5", 1, "Pilot");
    CHECK(Access::airbandGroup(app) == "Manual");

    // Listening to "every ticked" (no group): Add leaves it so.
    Access::setAirbandGroup(app, "");
    Access::setListening(app, true);
    Access::addManual(app, "122.8", 0, "UNICOM");
    CHECK(Access::airbandGroup(app).empty());
    CHECK(Access::bookmarks(app).size() == 3u);   // ...and the row was added all the same

    // Listening to one preset: Add to another leaves the monitor on the first.
    Access::setAirbandGroup(app, "KXYZ");
    Access::addManual(app, "123.45", 0, "Elsewhere");
    CHECK(Access::airbandGroup(app) == "KXYZ");
    CHECK(Access::bookmarks(app).size() == 4u);

    // Import while listening: the rows join, the note is said, the monitor's choice stands.
    Access::importPreset(app, "Marine", writeMixedCsv("listen-keep"));
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::bookmarks(app).size() == 8u);
    CHECK(Access::presetNote(app).rfind("Preset \"Marine\": 4 read, 4 new;", 0) == 0);
    CHECK(Access::airbandGroup(app) == "KXYZ");

    // Not listening: an import that brings the monitor something to play puts its preset on show.
    Access::setListening(app, false);
    Access::importPreset(app, "Harbour", writeCsv("listen-keep-two", 2, 140.0));
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::airbandGroup(app) == "Harbour");

    // ...but not one that has nothing it can play: only WFM rows, added unticked, leave the
    // group on show exactly as it was.
    const fs::path wfmOnly = g_scratch / "listen-keep-wfm.csv";
    {
        std::ofstream f(wfmOnly, std::ios::binary);
        f << "frequency_mhz,name,group,mode,bandwidth_hz\n"
             "98.500,Radio One,,WFM,150000\n"
             "101.100,Radio Two,,WFM,150000\n";
    }
    Access::importPreset(app, "Broadcast", wfmOnly.string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::bookmarks(app).size() == 12u);   // both WFM rows are in the list...
    CHECK(Access::presetNote(app) ==
          "Preset \"Broadcast\": 2 read, 2 new; the monitor can play 0 of the rows read (AM or NFM).");
    CHECK(Access::airbandGroup(app) == "Harbour");   // ...and the combo did not move
    for (const cascade::core::Bookmark& b : Access::bookmarks(app)) {
        if (b.group == "Broadcast") { CHECK(!b.scan); }
    }
    // The same with nothing on show: still nothing on show.
    Access::setAirbandGroup(app, "");
    Access::importPreset(app, "Broadcast2", wfmOnly.string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::airbandGroup(app).empty());
}

// REMOVE PRESET ON THE PRESET THE MONITOR IS PLAYING stops it, and says why: with its group gone
// the monitor would otherwise be cut again from "every ticked row" of the whole list - a
// different set from the one chosen - or stopped with no reason when none was ticked.
void checkRemovingThePresetBeingListenedToStopsTheMonitor() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "listen-remove").string());
    Access::addRow(app, "Ch 16", 156.8e6, "NFM", "Marine", true);
    Access::addRow(app, "Ch 72", 156.625e6, "NFM", "Marine", true);
    Access::addRow(app, "Tower", 118.7e6, "AM", "Tower", true);

    // Listening to Marine, Marine removed: stopped, with the reason, and the other preset stays.
    Access::setAirbandGroup(app, "Marine");
    Access::setListening(app, true);
    Access::removePreset(app, "Marine");
    CHECK(!Access::listening(app));
    CHECK(Access::airbandNote(app) == "Stopped: the preset was removed.");
    CHECK(Access::presetNote(app) == "Removed 2 from \"Marine\"");
    CHECK(Access::airbandGroup(app).empty());
    CHECK(Access::bookmarks(app).size() == 1u);

    // Listening to Tower, ANOTHER preset removed (one that is not even there): it plays on, and
    // nothing is said about a stop.
    Access::setAirbandNote(app, "");
    Access::setAirbandGroup(app, "Tower");
    Access::setListening(app, true);
    Access::removePreset(app, "Marine");
    CHECK(Access::listening(app));
    CHECK(Access::airbandNote(app).empty());
    CHECK(Access::airbandGroup(app) == "Tower");
    CHECK(Access::presetNote(app) == "Removed 0 from \"Marine\"");

    // Listening to "every ticked" (no group), a preset removed: THIS key does not stop it - the
    // frame loop cuts the blocks again from what is still ticked, as it does for any tick changed.
    Access::addRow(app, "Ch 16", 156.8e6, "NFM", "Marine", true);
    Access::setAirbandGroup(app, "");
    Access::removePreset(app, "Marine");
    CHECK(Access::listening(app));
    Access::setListening(app, false);

    // Not listening, the listened-to preset's name removed: nothing to stop, no stop said.
    Access::addRow(app, "Ch 16", 156.8e6, "NFM", "Marine", true);
    Access::setAirbandNote(app, "");
    Access::setAirbandGroup(app, "Marine");
    Access::removePreset(app, "Marine");
    CHECK(!Access::listening(app));
    CHECK(Access::airbandNote(app).empty());
    CHECK(Access::airbandGroup(app).empty());
}

// A WORKER THAT THROWS still says it in the section that asked. The note's destination is
// kept on the window's thread when the export starts: a throw returns a default result, which
// used to send a preset's "Could not write" to the Bookmarks section and leave this section's
// "Saving..." up for good. And one export's destination does not follow into the next.
void checkAThrowingExportWorkerAnswersTheSectionThatAsked() {
    {
        AppWindow app;
        Access::setRecordDir(app, (g_scratch / "export-throws-preset").string());
        Access::addRow(app, "Row", 118.7e6, "AM", "Harbour", true);
        Access::setHook(app, [] { throw std::runtime_error("the disk threw"); });
        Access::exportGroup(app, "Harbour");
        CHECK(Access::presetNote(app) == "Saving...");
        CHECK(pumpApp(app, 5000.0, [&] { return !Access::exportPending(app); }));
        CHECK(Access::presetNote(app) != "Saving...");
        CHECK(Access::presetNote(app).rfind("Could not write ", 0) == 0);
        CHECK(!Access::presetFolderKey(app));
        CHECK(Access::exportNote(app).empty());   // not in the Bookmarks section
        // The next export, from the Bookmarks section, goes to that section.
        Access::setHook(app, nullptr);
        Access::addBookmarks(app, 2);
        Access::exportBookmarks(app);
        CHECK(pumpApp(app, 5000.0, [&] { return Access::exportNote(app).rfind("Exported ", 0) == 0; }));
        CHECK(Access::presetNote(app).rfind("Could not write ", 0) == 0);   // left as it was
    }
    {
        AppWindow app;
        Access::setRecordDir(app, (g_scratch / "export-throws-sdr").string());
        Access::addBookmarks(app, 2);
        Access::setHook(app, [] { throw std::runtime_error("the disk threw"); });
        Access::exportBookmarks(app);
        CHECK(pumpApp(app, 5000.0, [&] { return !Access::exportPending(app); }));
        CHECK(Access::exportNote(app).rfind("Could not write ", 0) == 0);
        CHECK(Access::presetNote(app).empty());   // not in the AIRBAND section
        // The next export, a preset's, goes to the AIRBAND section.
        Access::setHook(app, nullptr);
        Access::addRow(app, "Row", 118.7e6, "AM", "Harbour", true);
        Access::exportGroup(app, "Harbour");
        CHECK(pumpApp(app, 5000.0, [&] { return Access::presetNote(app).rfind("Exported 1 to ", 0) == 0; }));
        CHECK(Access::presetFolderKey(app));
    }
}

// THE PRESET IMPORT SAYS WHEN SDR#'s CONVERTER SHIFT WAS NOT APPLIED, as the Bookmarks section's
// does: an SDR# file with Shift values lands at the frequencies as written, and the note says so.
void checkAPresetImportSaysShiftValuesWereNotApplied() {
    AppWindow app;
    Access::setRecordDir(app, (g_scratch / "preset-shift").string());
    const fs::path shifted = g_scratch / "preset-shift.xml";
    {
        std::ofstream f(shifted, std::ios::binary);
        f << "<?xml version=\"1.0\"?><ArrayOfMemoryEntry>"
             "<MemoryEntry><Name>Up-converted</Name><Frequency>145500000</Frequency>"
             "<DetectorType>NFM</DetectorType><Shift>-100000000</Shift>"
             "<FilterBandwidth>12500</FilterBandwidth></MemoryEntry>"
             "<MemoryEntry><Name>Plain</Name><Frequency>118700000</Frequency>"
             "<DetectorType>AM</DetectorType><Shift>0</Shift>"
             "<FilterBandwidth>10000</FilterBandwidth></MemoryEntry>"
             "</ArrayOfMemoryEntry>";
    }
    Access::importPreset(app, "Marine", shifted.string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::bookmarks(app).size() == 2u);
    CHECK(Access::presetNote(app) ==
          "Preset \"Marine\": 2 read, 2 new; the monitor can play 2 of the rows read (AM or NFM). "
          "Converter Shift values were not applied.");
    CHECK(Access::exportNote(app).empty());

    // A row with no frequency AND a Shift: both sentences, the skipped one first.
    const fs::path both = g_scratch / "preset-shift-skipped.xml";
    {
        std::ofstream f(both, std::ios::binary);
        f << "<ArrayOfMemoryEntry>"
             "<MemoryEntry><Name>Up</Name><Frequency>156800000</Frequency>"
             "<DetectorType>NFM</DetectorType><Shift>-100000000</Shift></MemoryEntry>"
             "<MemoryEntry><Name>no frequency</Name></MemoryEntry>"
             "</ArrayOfMemoryEntry>";
    }
    Access::importPreset(app, "Marine", both.string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::presetNote(app) ==
          "Preset \"Marine\": 1 read, 1 new; the monitor can play 1 of the rows read (AM or NFM). "
          "Some rows of the file had no usable frequency. Converter Shift values were not applied.");

    // No Shift in the file: no such sentence.
    const fs::path clean = g_scratch / "preset-noshift.xml";
    {
        std::ofstream f(clean, std::ios::binary);
        f << "<ArrayOfMemoryEntry><MemoryEntry><Name>Clean</Name><Frequency>121500000</Frequency>"
             "<DetectorType>AM</DetectorType><Shift>0</Shift></MemoryEntry></ArrayOfMemoryEntry>";
    }
    Access::importPreset(app, "Marine", clean.string());
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::importPending(app); }));
    CHECK(Access::presetNote(app) ==
          "Preset \"Marine\": 1 read, 1 new; the monitor can play 1 of the rows read (AM or NFM).");
    CHECK(Access::presetNote(app).find("Shift") == std::string::npos);
}

// --- THE PATCH PAGE'S RADIO ON AN I/Q RECORDING ----------------------------------

// The path of the one recording makeRecording() left in `dir`.
std::string recordingIn(const fs::path& dir) {
    const std::vector<fs::path> f = filesIn(dir, ".wav");
    return f.empty() ? std::string() : f.front().string();
}

void checkAPatchRecordingOpenNeverHoldsAFrame() {
    const fs::path dir = g_scratch / "patch-rec";
    makeRecording(dir);
    AppWindow app;
    const auto id = Access::addFileRadio(app, recordingIn(dir));
    auto disk = makeDisk(2500);
    Access::setHook(app, slowHook(disk));
    HangWatchdog w;
    startWatchdog(w, reportDir("patch recording open"));
    const LoopStats s = runFrameLoop(w, 3600.0, [&] { Access::reconcile(app); });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    CHECK(!disk->askedFromCaller.load());
    CHECK(disk->calls.load() == 1);
    // What the synchronous open gave: the radio runs, the node took the
    // recording's rate (the 2.4 MS/s it was given is not what the file holds), and
    // nothing is said against it.
    CHECK(Access::radioRunning(app, id));
    CHECK(!Access::radioPending(app, id));
    CHECK(Access::nodeRate(app, id) == 250000.0);
    CHECK(Access::radioError(app, id).empty());
    std::printf("  patch radio recording open: %d frames in 3.6 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();
}

// A recording that is not there: the node says what it always said, once, and is not
// retried every frame - only when its device is changed, which then works.
void checkAMissingPatchRecordingSaysWhatItSaid() {
    const fs::path dir = g_scratch / "patch-rec-missing";
    makeRecording(dir);
    AppWindow app;
    const std::string missing = (g_scratch / "no-such-recording.wav").string();
    const auto id = Access::addFileRadio(app, missing);
    std::string want;
    CHECK(cascade::core::patch::openIqRecording(cascade::core::patch::makeIqFileKey(missing), 0.0,
                                                want) == nullptr);
    CHECK(!want.empty());
    auto disk = makeDisk(0);
    Access::setHook(app, slowHook(disk));
    CHECK(pumpApp(app, 5000.0, [&] {
        Access::reconcile(app);
        return !Access::radioError(app, id).empty() && !Access::radioPending(app, id);
    }));
    CHECK(Access::radioError(app, id) == want);
    CHECK(!Access::radioRunning(app, id));
    // Not retried every frame.
    for (int i = 0; i < 40; ++i) {
        Access::reconcile(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(disk->calls.load() == 1);
    // A recording that exists replaces it.
    Access::setNodeDevice(app, id, recordingIn(dir));
    CHECK(pumpApp(app, 5000.0, [&] {
        Access::reconcile(app);
        return Access::radioRunning(app, id);
    }));
    CHECK(Access::radioError(app, id).empty());
    CHECK(Access::nodeRate(app, id) == 250000.0);
}

// A node switched off while its recording opens: the answer is dropped, the file closed,
// and no radio appears.
void checkARecordingNodeSwitchedOffWhileOpeningIsDropped() {
    const fs::path dir = g_scratch / "patch-rec-off";
    makeRecording(dir);
    AppWindow app;
    const auto id = Access::addFileRadio(app, recordingIn(dir));
    auto disk = makeDisk(600);
    Access::setHook(app, slowHook(disk));
    Access::reconcile(app);
    CHECK(pumpApp(app, 3000.0, [&] { return disk->calls.load() == 1; }));
    Access::setNodeOn(app, id, false);
    CHECK(pumpApp(app, 5000.0, [&] {
        Access::reconcile(app);
        return disk->finished.load() == 1 && !Access::radioPending(app, id);
    }));
    CHECK(!Access::radioRunning(app, id));
    CHECK(Access::radioError(app, id).empty());
    // Switched on again, it opens.
    Access::setNodeOn(app, id, true);
    CHECK(pumpApp(app, 5000.0, [&] {
        Access::reconcile(app);
        return Access::radioRunning(app, id);
    }));
}

// The window destroyed while a recording is still opening does not wait for the disk.
void checkExitWhileAPatchRecordingOpens() {
    const fs::path dir = g_scratch / "patch-rec-exit";
    makeRecording(dir);
    auto disk = makeDisk(2500);
    double destroyMs = 0.0;
    {
        auto app = std::make_unique<AppWindow>();
        Access::addFileRadio(*app, recordingIn(dir));
        Access::setHook(*app, slowHook(disk));
        const double t0 = nowMs();
        Access::reconcile(*app);
        const double firstFrameMs = nowMs() - t0;
        std::printf("  patch recording open: first reconcile took %.0f ms\n", firstFrameMs);
        CHECK(firstFrameMs < 800.0);
        const double t1 = nowMs();
        app.reset();
        destroyMs = nowMs() - t1;
    }
    CHECK(destroyMs < 2000.0);
    const double until = nowMs() + 10000.0;
    while (nowMs() < until && disk->finished.load() < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(disk->finished.load() == 1);
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
        Access::importFile(*app, writeCsv("exit", 2, 130.0));
        CHECK(Access::imagePending(*app));
        CHECK(Access::exportPending(*app));
        CHECK(Access::importPending(*app));
        const double t0 = nowMs();
        app.reset();
        destroyMs = nowMs() - t0;
    }
    // Five wedged workers, each given the 250 ms grace in ~AppWindow and then let
    // go: well inside the 2.5 s the disk is taking.
    CHECK(destroyMs < 2000.0);
    std::printf("  exit with five workers wedged: ~AppWindow took %.0f ms\n", destroyMs);
    const double until = nowMs() + 10000.0;
    while (nowMs() < until && disk->finished.load() < 5) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(disk->finished.load() == 5);  // the abandoned workers ran to the end by themselves
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
    checkTheImportNeverHoldsAFrame();
    checkASecondImportIsRemembered();
    checkAnImportSaysWhatItSaid();
    checkAnImportIntoAPresetForcesTheGroupAndTheTick();
    checkASecondPresetImportKeepsItsOwnGroup();
    checkThePresetNameAndTheImportKey();
    checkAnExportOfAPresetWritesOnlyThatGroup();
    checkAnExportOfNothingAndOfABlockedFolder();
    checkATypedFrequencyIsTheSameChannelAsTheListsOwn();
    checkTheHeardTimeFlushKeepsTheChannelOnItsRow();
    checkAddAndImportKeepAListeningMonitorsChoice();
    checkRemovingThePresetBeingListenedToStopsTheMonitor();
    checkAThrowingExportWorkerAnswersTheSectionThatAsked();
    checkAPresetImportSaysShiftValuesWereNotApplied();
    checkAPatchRecordingOpenNeverHoldsAFrame();
    checkAMissingPatchRecordingSaysWhatItSaid();
    checkARecordingNodeSwitchedOffWhileOpeningIsDropped();
    checkExitWhileAPatchRecordingOpens();
    checkTheScreenshotNeverHoldsAFrame();
    checkAFailedScreenshotIsContained();
    checkExitWhileEveryWorkerIsWedged();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_gui_file_jobs");
}
