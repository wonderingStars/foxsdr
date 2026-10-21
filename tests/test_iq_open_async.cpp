// The I/Q-file Open button does not make a frame wait for the disk.
//
// WHAT WENT WRONG. The Source section's Open button for an I/Q file made an
// IqFileSource and called open() on it in the click handler, on the thread that
// draws the window: an ifstream opened on the path the user typed and the RIFF
// header walked with seeks and reads. The path is the user's own - a recording on
// a network share, a synchronised folder, an external drive that has gone to
// sleep - and every one of those calls waits for as long as the disk takes (the
// header walk is bounded in COUNT, IqFileSource::kMaxHeaderChunks, and its own
// comment says what that cannot fix: "ONE read against a dead SMB share still
// parks for that filesystem's own timeout"). Found by the 0.99.64 audit of what
// the 0.99.63 Record fix left on the GUI thread (docs/DIAGNOSTICS.md, "The window
// does no disk work").
//
// WHAT IS TESTED, and at which level. NOTHING HERE DEPENDS ON A REAL SLOW DISK:
// IqFileSource has a seam for exactly this - the virtual readHeaderRaw its header
// walk reads through - and a test makes a source whose first header read sleeps
// and then does the REAL read: a slow disk as far as the frame loop can tell,
// with the real file, the real header parse and the real install behind it.
//
//   1. THE HARNESS CAN SEE THE FAULT. The same slow open called on the thread
//      that heartbeats is reported by a real HangWatchdog.
//   2. THE REAL HANDLER, through a real AppWindow, under a real HangWatchdog
//      (800 ms threshold) and a frame loop: Open pressed, a 2.5 s header read.
//      Before the fix the frame loop stalled for the whole read and the watchdog
//      filed a report (worst frame gap 2506 ms); after it nothing waits, the
//      source is installed on the frame that finds the file open, and what it
//      installs is the source the synchronous Open installed.
//   3. THE EDGES, each against the class: a file that will not open says what the
//      synchronous open said, a second Open while one is out is ignored, a result
//      that arrives after the user chose another source is dropped and its file
//      closed, and a window destroyed while the read is wedged does not wait for
//      it.
//   4. THE WIRING: the Open handler's open() is inside the worker's closure.
//
// WHAT IS NOT COVERED. The disabled key and the "Waiting for the disk" line are
// ImGui output no test reads; the state they read (the job's pending() and
// elapsedS()) is checked. The start-up restore of a saved file (applyConfig)
// still opens it inline, before the first frame - see docs/DIAGNOSTICS.md.
// Nothing here has met a genuinely slow disk.
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
#include <memory>
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

#include "core/hang_watchdog.hpp"
#include "core/recorder.hpp"
#include "gui/app_window.hpp"
#include "source/iq_file_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using cascade::source::IqFileSource;

namespace {

struct SlowDisk {
    std::atomic<int> blockMs{0};
    std::atomic<int> calls{0};
    std::atomic<bool> askedFromCaller{false};
    const std::thread::id caller = std::this_thread::get_id();
};

// An IqFileSource whose first header read waits like a slow disk, then does the
// REAL read.
class SlowIqFile : public IqFileSource {
public:
    explicit SlowIqFile(std::shared_ptr<SlowDisk> d) : disk_(std::move(d)) {}

protected:
    std::size_t readHeaderRaw(unsigned char* dst, std::size_t bytes) override {
        if (!slept_) {
            slept_ = true;
            if (std::this_thread::get_id() == disk_->caller) { disk_->askedFromCaller.store(true); }
            disk_->calls.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(disk_->blockMs.load()));
        }
        return IqFileSource::readHeaderRaw(dst, bytes);
    }

private:
    std::shared_ptr<SlowDisk> disk_;
    bool slept_ = false;
};

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members.
struct AppWindowTestAccess {
    static void bindFactory(AppWindow& a, std::function<std::unique_ptr<IqFileSource>()> f) {
        a.iqFileFactory_ = std::move(f);
    }
    // The path box is only drawn while the combo sits on the "IQ file" row, so
    // typing a path is also choosing that row.
    static void setPath(AppWindow& a, const std::string& p) {
        a.sourceSel_ = 1;
        std::snprintf(a.iqPath_, sizeof(a.iqPath_), "%s", p.c_str());
    }
    static void open(AppWindow& a) { a.openIqFile(); }
    // One frame's worth of the poll.
    static void poll(AppWindow& a) { a.pollIqOpen(); }
    static bool pending(AppWindow& a) { return a.iqOpenPending(); }
    // What a successful open does after the header is read.
    static void installOpened(AppWindow& a, std::unique_ptr<IqFileSource> f, const std::string& p) {
        a.installOpenedIqFile(std::move(f), p);
    }
    // Another source installed meanwhile: every install moves the generation.
    static void moveOn(AppWindow& a) { ++a.sourceGen_; }
    static std::string sourceKind(AppWindow& a) { return a.sourceKind_; }
    static std::string sourceError(AppWindow& a) { return a.sourceError_; }
    static std::string openPath(AppWindow& a) { return a.iqOpenPath_; }
    static double rateHz(AppWindow& a) { return a.pipeline_.activeSource().sampleRateHz(); }
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_iq_open_" + std::to_string(pid));
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

// A real I/Q recording: 4000 frames at 250 kS/s, written by the application's own
// recorder, so the file is exactly what the Open button is meant to take.
std::string makeRecording(const char* tag) {
    cascade::core::Recorder rec;
    std::string err;
    const std::string dir = (g_scratch / tag).string();
    CHECK(rec.start(cascade::core::RecordKind::BasebandIq, dir, 250000.0, err));
    std::vector<std::complex<float>> s(4000);
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = {0.25f * static_cast<float>(i % 7), -0.125f * static_cast<float>(i % 5)};
    }
    rec.writeIq(s.data(), s.size());
    rec.stop();
    return rec.path();
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

// --- 1. THE HARNESS CAN SEE THE FAULT -----------------------------------------

void checkTheSynchronousOpenIsReported() {
    auto disk = std::make_shared<SlowDisk>();
    disk->blockMs.store(2500);
    const std::string path = makeRecording("control");
    HangWatchdog w;
    startWatchdog(w, reportDir("control"));
    SlowIqFile f(disk);
    CHECK(f.open(path));  // the old Open press
    CHECK(w.reportsWritten() == 1u);
    CHECK(disk->askedFromCaller.load());
    w.stop();
}

// --- 2. THE REAL HANDLER, THROUGH A REAL WINDOW --------------------------------

// What a successful Open leaves, as the synchronous Open left it: the file is the
// source in use, at the file's rate, with no error showing, and the box's path is
// the one remembered for the config.
void checkInstalledAsTheSynchronousOpenInstalledIt(AppWindow& app, const std::string& path) {
    CHECK(Access::sourceKind(app) == "file");
    CHECK(Access::sourceError(app).empty());
    CHECK(Access::openPath(app) == path);
    CHECK(Access::rateHz(app) == 250000.0);
}

// THE FIELD HANG, through the code the button reaches: a 2.5 s header read
// against an 800 ms threshold. The loop must keep beating, the watchdog must write
// nothing, nothing is installed before the file is open, and the file must still
// arrive - a fix that merely dropped the open would pass everything but the last
// of these.
void checkAppWindowOpenNeverHoldsAFrame() {
    auto disk = std::make_shared<SlowDisk>();
    disk->blockMs.store(2500);
    const std::string path = makeRecording("app-slow");
    AppWindow app;
    Access::bindFactory(app, [disk]() -> std::unique_ptr<IqFileSource> {
        return std::make_unique<SlowIqFile>(disk);
    });
    Access::setPath(app, path);

    HangWatchdog w;
    startWatchdog(w, reportDir("appwindow"));
    const double t0 = nowMs();
    bool first = true;
    bool installedTooEarly = false;
    bool sawPending = false;
    const LoopStats s = runFrameLoop(w, 3600.0, [&] {
        if (first) {
            first = false;
            Access::open(app);
            // The key pressed again, and the same handler from a key binding:
            // one open is ever out.
            Access::open(app);
            Access::open(app);
        }
        Access::poll(app);
        if (Access::pending(app)) { sawPending = true; }
        if (nowMs() - t0 < 2300.0 && Access::sourceKind(app) == "file") { installedTooEarly = true; }
    });
    CHECK(w.reportsWritten() == 0u);
    CHECK(s.frames > 100);
    CHECK(s.worstGapMs < 800.0);
    CHECK(sawPending);
    CHECK(!installedTooEarly);
    CHECK(!disk->askedFromCaller.load());
    CHECK(disk->calls.load() == 1);  // one open, however often it was asked
    CHECK(!Access::pending(app));
    checkInstalledAsTheSynchronousOpenInstalledIt(app, path);
    std::printf("  AppWindow Open: %d frames in 3.6 s, worst gap %.0f ms, hang reports %u\n",
                s.frames, s.worstGapMs, w.reportsWritten());
    w.stop();
}

// The same file through the synchronous path - what the button used to do - and
// the new one end in the same state.
void checkTheAsyncOpenEqualsTheSyncOpen() {
    const std::string path = makeRecording("equal");
    AppWindow viaWorker;
    Access::setPath(viaWorker, path);
    Access::open(viaWorker);
    CHECK(pumpApp(viaWorker, 5000.0, [&] { return Access::sourceKind(viaWorker) == "file"; }));
    checkInstalledAsTheSynchronousOpenInstalledIt(viaWorker, path);

    AppWindow direct;
    auto file = std::make_unique<IqFileSource>();
    CHECK(file->open(path));
    Access::installOpened(direct, std::move(file), path);
    checkInstalledAsTheSynchronousOpenInstalledIt(direct, path);
    CHECK(Access::rateHz(viaWorker) == Access::rateHz(direct));
}

// --- 3. THE EDGES --------------------------------------------------------------

// A file that will not open says what the synchronous open said - for a file that
// is not there and for one that is not a WAV - after the open has come back, not
// before; nothing is installed; and Open works again.
void checkAFailedOpenSaysWhatTheSyncOpenSaid() {
    const std::string missing = (g_scratch / "no-such-recording.wav").string();
    const std::string notWav = (g_scratch / "not-a-wav.wav").string();
    {
        std::ofstream f(notWav, std::ios::binary);
        f << "this is not a RIFF file at all, just text long enough to be read";
    }
    for (const std::string& path : {missing, notWav}) {
        IqFileSource direct;
        CHECK(!direct.open(path));
        const std::string inlineWords = direct.lastError();
        CHECK(!inlineWords.empty());

        auto disk = std::make_shared<SlowDisk>();
        disk->blockMs.store(400);
        AppWindow app;
        Access::bindFactory(app, [disk]() -> std::unique_ptr<IqFileSource> {
            return std::make_unique<SlowIqFile>(disk);
        });
        Access::setPath(app, path);
        Access::open(app);
        Access::poll(app);
        CHECK(Access::sourceError(app).empty());  // not before the open is back
        CHECK(Access::pending(app));
        CHECK(pumpApp(app, 5000.0, [&] { return !Access::pending(app); }));
        CHECK(Access::sourceError(app) == inlineWords);
        CHECK(Access::sourceKind(app) != "file");
        std::printf("  late failure: \"%s\"\n", Access::sourceError(app).c_str());
        // And the key is free again: a good file opens.
        const std::string good = makeRecording("after-failure");
        Access::setPath(app, good);
        disk->blockMs.store(0);
        Access::open(app);
        CHECK(pumpApp(app, 5000.0, [&] { return Access::sourceKind(app) == "file"; }));
        CHECK(Access::sourceError(app).empty());  // a success clears the red line
    }
}

std::atomic<int> g_destroyed{0};

// A source whose destruction is counted: the file an open leaves behind when its
// result is dropped (or abandoned at quit) must be closed.
class CountedIqFile : public SlowIqFile {
public:
    explicit CountedIqFile(std::shared_ptr<SlowDisk> d) : SlowIqFile(std::move(d)) {}
    ~CountedIqFile() override { g_destroyed.fetch_add(1); }
};

// A result that arrives after the user chose another source is dropped, and its
// file is closed: the choice the user made last is the one that stands.
void checkAResultAfterTheChoiceMovedOnIsDropped() {
    g_destroyed.store(0);
    auto disk = std::make_shared<SlowDisk>();
    disk->blockMs.store(600);
    const std::string path = makeRecording("moved-on");
    AppWindow app;
    Access::bindFactory(app, [disk]() -> std::unique_ptr<IqFileSource> {
        return std::make_unique<CountedIqFile>(disk);
    });
    Access::setPath(app, path);
    Access::open(app);
    CHECK(Access::pending(app));
    Access::moveOn(app);  // another source was installed meanwhile
    CHECK(pumpApp(app, 5000.0, [&] { return !Access::pending(app); }));
    CHECK(Access::sourceKind(app) != "file");
    const double until = nowMs() + 4000.0;
    while (nowMs() < until && g_destroyed.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_destroyed.load() == 1);  // closed, off the GUI thread
}

// APPLICATION EXIT WITH THE READ WEDGED: the window is destroyed without waiting
// for the disk (bounded by the quit grace, not by the 2.5 s the disk is taking),
// and the file the open finally returns is closed.
void checkExitWhileTheOpenIsWedged() {
    g_destroyed.store(0);
    auto disk = std::make_shared<SlowDisk>();
    disk->blockMs.store(2500);
    const std::string path = makeRecording("exit");
    double destroyMs = 0.0;
    {
        auto app = std::make_unique<AppWindow>();
        Access::bindFactory(*app, [disk]() -> std::unique_ptr<IqFileSource> {
            return std::make_unique<CountedIqFile>(disk);
        });
        Access::setPath(*app, path);
        Access::open(*app);
        CHECK(Access::pending(*app));
        const double t0 = nowMs();
        app.reset();
        destroyMs = nowMs() - t0;
    }
    CHECK(destroyMs < 1500.0);
    std::printf("  exit with the open wedged: ~AppWindow took %.0f ms\n", destroyMs);
    const double until = nowMs() + 8000.0;
    while (nowMs() < until && g_destroyed.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(g_destroyed.load() == 1);
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

std::string functionBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// The Open handler reaches open() only from inside the closure it hands the
// worker; the button reaches nothing but the handler.
void checkTheOpenIsNotMadeOnTheGuiThread() {
    // THE CONTROL: on the handler as it was before the fix, open() is not inside a
    // worker's closure, and the scan says so.
    const std::string before =
        "void AppWindow::openIqFile() {\n"
        "    auto file = std::make_unique<IqFileSource>();\n"
        "    if (!file->open(iqPath_)) { return; }\n"
        "}\n";
    CHECK(withoutLineComments(functionBody(before, "void AppWindow::openIqFile()"))
              .find("iqOpen.request(") == std::string::npos);

    // The handler lives in app_window_disk_work.cpp; the button is in app_window.cpp.
    const fs::path gui = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    const std::string source = withoutLineComments(readFileText(gui / "app_window_disk_work.cpp"));
    const std::string window = withoutLineComments(readFileText(gui / "app_window.cpp"));
    const std::string body = functionBody(source, "void AppWindow::openIqFile()");
    CHECK(body.size() > 300);
    const std::size_t request = body.find("iqOpen.request(");
    const std::size_t open = body.find("file->open(");
    CHECK(request != std::string::npos);
    CHECK(open != std::string::npos);
    CHECK(request != std::string::npos && open != std::string::npos && request < open);
    // The poll installs; it opens nothing itself.
    CHECK(functionBody(source, "void AppWindow::pollIqOpen()").find("->open(") == std::string::npos);
    // The button is the handler and nothing more.
    CHECK(window.find("if (ImGui::Button(trId(\"Open\"))) { openIqFile(); }") != std::string::npos);
}

}  // namespace

int main() {
    isolate();
    checkTheOpenIsNotMadeOnTheGuiThread();
    checkTheSynchronousOpenIsReported();
    checkAppWindowOpenNeverHoldsAFrame();
    checkTheAsyncOpenEqualsTheSyncOpen();
    checkAFailedOpenSaysWhatTheSyncOpenSaid();
    checkAResultAfterTheChoiceMovedOnIsDropped();
    checkExitWhileTheOpenIsWedged();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_iq_open_async");
}
