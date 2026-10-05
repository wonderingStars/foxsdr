// What FoxSDR does when a FILE it reads or writes goes wrong (0.99.65).
//
// docs/DIAGNOSTICS.md, "The window does no disk work", moved the GUI thread's disk
// calls to workers; this is the other half of that audit: a test of what each file
// does when the disk is not well - full, read-only, gone, locked by another program,
// or holding a damaged copy of what it should hold. Each case runs through the real
// code at the lowest level that still shows the reaction, and asserts the WHOLE
// reaction: no crash, no hang (every call is timed), the file left either complete
// or untouched, the right words returned for the caller to show, and no temporary
// file left behind. Where the reaction is not a good one the test PINS WHAT HAPPENS
// TODAY and says so on its output line ("FINDING"): the point is that it is read
// first, and that a fix has to change a line here on purpose.
//
//   1. THE WRITERS (settings, bookmarks, markers): a write that fails part way, a
//      read-only folder, a target another program holds open, a directory squatting
//      on the target, a parent that is a file, and a drive that is not there.
//   2. THE LOADERS (settings, bookmarks, markers, a band plan): a file truncated to
//      zero, cut mid-token, replaced by a directory, held open by another program.
//   3. WHAT THE APPLICATION DOES WITH A DAMAGED FILE: the window is built on one,
//      and the next save is made - is the damaged file kept, or is it gone?
//   4. A READ-ONLY SETTINGS FOLDER, at start-up and at exit.
//   5. THE LOG FILE held by another program at start-up, and a plugin file that
//      cannot be deleted.
//   6. THE EXPORT, THE SCREENSHOT AND THE PICTURE SAVE when the write fails.
//
// WHAT COULD NOT BE REACHED. A disk that fills part way through a write: no
// unprivileged way to fill a volume under a test, so the writers carry a seam
// (core/write_fault.hpp) that sets the stream's badbit after the bytes are written -
// the state a failed write leaves it in. A removable drive pulled mid-write: a
// drive letter that is not there stands in for the destination being gone BEFORE
// the write; Windows will not let a folder be removed under an open file, so
// "disappears DURING" is reached only for a recording (tests/test_failure_recording.cpp).
//
// Folders are made read-only with icacls on scratch folders this test creates, and
// every one is put back before the test ends (and by an atexit hook if it dies).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
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

#include <imgui.h>

#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/freq_manager.hpp"
#include "core/freq_markers.hpp"
#include "core/host_image.hpp"
#include "core/image_write.hpp"
#include "core/plugin_cleanup.hpp"
#include "core/plugin_repo.hpp"
#include "core/write_fault.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;
using cascade::core::FreqManager;
using cascade::core::FreqMarkers;

namespace cascade::gui {

struct AppWindowTestAccess {
    // The settings.
    static void saveNow(AppWindow& a) { a.saveConfigNow(); }
    static void maybeSave(AppWindow& a, double nowS) { a.maybeSaveConfig(nowS); }
    static void pollConfig(AppWindow& a) { a.pollConfigWriter(); }
    static bool configInFlight(AppWindow& a) { return a.configWriter_.inFlight(); }
    static bool configLastOk(AppWindow& a) { return a.configWriter_.lastOk(); }
    static std::string configLastError(AppWindow& a) { return a.configWriter_.lastError(); }
    static unsigned configCompleted(AppWindow& a) { return a.configWriter_.completed(); }
    static bool configDrain(AppWindow& a, std::chrono::milliseconds bound) {
        return a.configWriter_.finishOrAbandon(bound);
    }
    static void bindConfigWriter(AppWindow& a, cascade::gui::ConfigWriter::Writer w) {
        a.configWriter_.bind(std::move(w));
    }
    // A change currentConfig() sees (a plain flag it writes out).
    static void touchSettings(AppWindow& a) { a.ppmCorrectionOn_ = !a.ppmCorrectionOn_; }
    static void addBookmarkToView(AppWindow& a, const std::string& name, double hz) {
        addBookmark(a, name, hz);
        a.bookmarkView_.push_back(static_cast<std::uint32_t>(a.freqMgr_.list().size() - 1));
    }
    // The lists.
    static std::string bookmarkError(AppWindow& a) { return a.bookmarkError_; }
    static std::string markerError(AppWindow& a) { return a.markerError_; }
    static std::size_t bookmarkCount(AppWindow& a) { return a.freqMgr_.list().size(); }
    static std::size_t markerCount(AppWindow& a) { return a.freqMarkers_.size(); }
    static void addBookmark(AppWindow& a, const std::string& name, double hz) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = "NFM";
        a.freqMgr_.add(std::move(b));
        a.bookmarkSaveDirty_ = true;
        a.bookmarkSaveDueS_ = 0.0;
    }
    static void addMarker(AppWindow& a, double hz) { a.freqMarkers_.add(hz, 1700000000); }
    static void flushLists(AppWindow& a) {
        a.flushBookmarkSave(true);
        a.flushMarkerSave(true);
    }
    static bool listsInFlight(AppWindow& a) {
        return a.bookmarkSaver_.inFlight() || a.markerSaver_.inFlight();
    }
    static void bindBookmarkSaver(AppWindow& a, cascade::gui::ConfigWriter::Writer w) {
        a.bookmarkSaver_.bind(std::move(w));
    }
    // The export, the picture and the screenshot.
    static void setRecordDir(AppWindow& a, std::string d) { a.recordDir_ = std::move(d); }
    static void poll(AppWindow& a) { a.pollDiskJobs(); }
    static void exportBookmarks(AppWindow& a) { a.exportBookmarksForSdrSharp(); }
    static bool exportPending(AppWindow& a) { return a.bookmarkExportPending(); }
    static std::string note(AppWindow& a) { return a.bookmarkImportNote_; }
    static void saveImage(AppWindow& a, const cascade::core::HostImage& im) { a.saveImageBmp(im); }
    static bool imagePending(AppWindow& a) { return a.imageSavePending(); }
    static std::string imageNote(AppWindow& a) { return a.imageSaveNote_; }
    static void shot(AppWindow& a, const std::string& dir, const cascade::core::HostImage& im) {
        a.shotAddPicture(dir + "/shot-1.bmp", im);
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_failure_files_" + std::to_string(pid));
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

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool existsNow(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Temporary files a writer left in `dir` (anything ending ".tmp").
std::vector<std::string> tempFilesIn(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".tmp") { out.push_back(e.path().filename().string()); }
    }
    return out;
}

// --- the faults ----------------------------------------------------------------

// WRITABILITY IS DECIDED BY WRITING, never by an attribute or `test -w` (which lie
// about Windows folders): a probe file is made and removed.
bool canWriteIn(const fs::path& dir) {
    const fs::path probe = dir / "writable-probe.tmp";
    {
        std::ofstream f(probe, std::ios::binary);
        if (!f) { return false; }
        f << "x";
        f.flush();
        if (!f) { return false; }
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

#if defined(_WIN32)
bool runHidden(const std::string& cmdLine) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmdLine;
    if (!::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &si, &pi)) {
        return false;
    }
    ::WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return code == 0;
}
#endif

std::set<std::string>& denied() {
    static std::set<std::string> s;
    return s;
}

// A scratch folder nobody (this user included) may add a file to. Only ever applied to
// folders this test made, and undone by allowWrite() - and at exit if the test dies.
bool denyWrite(const fs::path& dir) {
#if defined(_WIN32)
    const char* user = std::getenv("USERNAME");
    if (user == nullptr || *user == '\0') { return false; }
    const std::string cmd = "icacls \"" + dir.string() + "\" /deny \"" + user + ":(WD,AD)\"";
    if (!runHidden(cmd)) { return false; }
    denied().insert(dir.string());
    return true;
#else
    (void)dir;
    return false;
#endif
}

void allowWrite(const fs::path& dir) {
#if defined(_WIN32)
    const char* user = std::getenv("USERNAME");
    if (user == nullptr) { return; }
    runHidden("icacls \"" + dir.string() + "\" /remove:d \"" + user + "\"");
    denied().erase(dir.string());
#else
    (void)dir;
#endif
}

void allowAllDenied() {
    const std::set<std::string> copy = denied();
    for (const std::string& d : copy) { allowWrite(fs::path(d)); }
}

// A folder made read-only for the life of the guard.
struct ReadOnly {
    fs::path dir;
    bool took = false;
    explicit ReadOnly(const fs::path& d) : dir(d) {
        took = denyWrite(dir) && !canWriteIn(dir);  // proved by writing, not assumed
    }
    ~ReadOnly() { allowWrite(dir); }
    ReadOnly(const ReadOnly&) = delete;
    ReadOnly& operator=(const ReadOnly&) = delete;
};

// Another program holding a file open. `shareDelete` false is what a program that opened
// it normally does: reads and writes still work for others, rename and delete do not.
// `exclusive` shares nothing: nobody else can open it at all.
struct Held {
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
    Held(const fs::path& p, bool exclusive) {
        h = ::CreateFileW(p.wstring().c_str(), GENERIC_READ,
                          exclusive ? 0 : (FILE_SHARE_READ | FILE_SHARE_WRITE), nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    void release() {
        if (h != INVALID_HANDLE_VALUE) {
            ::CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }
    ~Held() { release(); }
#else
    Held(const fs::path&, bool) {}
    bool ok() const { return false; }
    void release() {}
#endif
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
};

// A drive letter nothing is mounted on: the destination "being gone".
std::string absentDrive() {
#if defined(_WIN32)
    const DWORD mask = ::GetLogicalDrives();
    for (char c = 'Z'; c >= 'D'; --c) {
        if ((mask & (1u << (c - 'A'))) == 0) { return std::string(1, c) + ":"; }
    }
#endif
    return {};
}

// The writer that fails: sets the stream's badbit, as a full disk leaves it.
const char* g_failWriter = nullptr;
void failThisWriter(const char* writer, std::ostream& out) {
    if (g_failWriter != nullptr && std::strcmp(writer, g_failWriter) == 0) {
        out.setstate(std::ios::badbit);
    }
}

// --- 1. THE WRITERS --------------------------------------------------------------

struct WriterCase {
    const char* name;
    const char* faultName;
    std::function<bool(const std::string&, const std::string&, std::string&)> write;
};

const std::vector<WriterCase>& writers() {
    static const std::vector<WriterCase> v = {
        {"settings", "config", &ConfigStore::writeFile},
        {"bookmarks", "bookmarks", &FreqManager::writeFile},
        {"markers", "markers", &FreqMarkers::writeFile},
    };
    return v;
}

void checkWriter(const WriterCase& w) {
    const std::string tag = w.name;
    const fs::path dir = g_scratch / ("w-" + tag);
    const fs::path target = dir / "file.json";
    const std::string kOld = "{\"old\": true}\n";
    const std::string kNew = "{\"new\": true}\n";
    std::string err;
    CHECK(w.write(target.string(), kOld, err));
    CHECK(readText(target) == kOld);

    // A write that fails PART WAY (a full disk): reported, the old file whole, no temp.
    g_failWriter = w.faultName;
    cascade::core::setWriteFaultHookForTest(&failThisWriter);
    double t0 = nowMs();
    CHECK(!w.write(target.string(), kNew, err));
    CHECK(nowMs() - t0 < 5000.0);
    cascade::core::setWriteFaultHookForTest(nullptr);
    g_failWriter = nullptr;
    CHECK(contains(err, "write to temp file"));
    CHECK(readText(target) == kOld);
    CHECK(tempFilesIn(dir).empty());
    std::printf("  %s, write fails part way: \"%s\"; old file whole; no temp left\n", w.name,
                err.substr(0, 40).c_str());

    // A READ-ONLY FOLDER: reported, old file whole, no temp; and works once it is back.
    {
        ReadOnly ro(dir);
        if (!ro.took) {
            std::printf("  %s, read-only folder: NOT REACHED (the folder could not be made read-only)\n",
                        w.name);
        } else {
            t0 = nowMs();
            CHECK(!w.write(target.string(), kNew, err));
            CHECK(nowMs() - t0 < 5000.0);
            CHECK(contains(err, "cannot create temp file"));
            CHECK(readText(target) == kOld);
            CHECK(tempFilesIn(dir).empty());
            std::printf("  %s, read-only folder: \"%s\"; old file whole\n", w.name,
                        err.substr(0, 40).c_str());
        }
    }
    CHECK(w.write(target.string(), kNew, err));
    CHECK(readText(target) == kNew);

    // THE TARGET HELD OPEN BY ANOTHER PROGRAM (no share-delete): the rename is refused,
    // reported, the old file whole, the temp removed - and it works once it is let go.
    {
        Held lock(target, /*exclusive=*/false);
        if (!lock.ok()) {
            std::printf("  %s, target held open: NOT REACHED\n", w.name);
        } else {
            t0 = nowMs();
            CHECK(!w.write(target.string(), kOld, err));
            CHECK(nowMs() - t0 < 5000.0);
            CHECK(contains(err, "atomic replace"));
            CHECK(tempFilesIn(dir).empty());
            CHECK(readText(target) == kNew);
            std::printf("  %s, target held open: \"%s\"; old file whole; no temp left\n", w.name,
                        err.substr(0, 40).c_str());
        }
    }
    CHECK(w.write(target.string(), kOld, err));
    CHECK(readText(target) == kOld);

    // A DIRECTORY SQUATTING ON THE TARGET.
    {
        const fs::path squat = dir / "squat.json";
        std::error_code ec;
        fs::create_directories(squat, ec);
        t0 = nowMs();
        CHECK(!w.write(squat.string(), kNew, err));
        CHECK(nowMs() - t0 < 5000.0);
        CHECK(!err.empty());
        CHECK(fs::is_directory(squat, ec));
        CHECK(tempFilesIn(dir).empty());
        std::printf("  %s, a folder on the target: \"%s\"\n", w.name, err.substr(0, 40).c_str());
    }

    // THE PARENT IS A FILE.
    {
        const fs::path blocker = dir / "blocker";
        writeText(blocker, "x");
        t0 = nowMs();
        CHECK(!w.write((blocker / "sub" / "file.json").string(), kNew, err));
        CHECK(nowMs() - t0 < 5000.0);
        CHECK(contains(err, "cannot create directory"));
    }

    // THE DESTINATION IS GONE: a drive that is not there.
    {
        const std::string drive = absentDrive();
        if (drive.empty()) {
            std::printf("  %s, drive gone: NOT REACHED (no free drive letter)\n", w.name);
        } else {
            t0 = nowMs();
            CHECK(!w.write(drive + "\\foxsdr\\file.json", kNew, err));
            const double took = nowMs() - t0;
            CHECK(took < 10000.0);
            CHECK(!err.empty());
            std::printf("  %s, drive gone: \"%s\" in %.0f ms\n", w.name, err.substr(0, 40).c_str(),
                        took);
        }
    }
}

// --- 2. THE LOADERS --------------------------------------------------------------

// A realistic saved file, cut at `cut` bytes (0 = empty).
std::string cutOf(const std::string& whole, std::size_t cut) { return whole.substr(0, cut); }

struct LoadOutcome {
    bool ok = false;
    std::string error;
    double ms = 0.0;
};

template <class Load>
LoadOutcome timedLoad(Load&& load) {
    LoadOutcome o;
    const double t0 = nowMs();
    o.ok = load(o.error);
    o.ms = nowMs() - t0;
    return o;
}

void report(const char* file, const char* what, const LoadOutcome& o, const char* after) {
    std::printf("  %s, %s: load %s%s%s [%s]\n", file, what, o.ok ? "ok" : "FAILS",
                o.error.empty() ? "" : " - ", o.error.substr(0, 60).c_str(), after);
}

void checkLoaders() {
    const fs::path dir = g_scratch / "loaders";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // The real thing, saved by the real code, so "cut mid-token" is a real cut.
    AppConfig cfg;
    cfg.sourceKind = "siggen";
    cfg.centerHz = 123456000.0;
    const std::string wholeCfg = ConfigStore::serialize(cfg);
    FreqManager mgr;
    for (int i = 0; i < 5; ++i) {
        cascade::core::Bookmark b;
        b.name = "Station " + std::to_string(i);
        b.freqHz = 100.0e6 + 1.0e6 * i;
        b.mode = "NFM";
        mgr.add(std::move(b));
    }
    const std::string wholeBm = mgr.serialize();
    FreqMarkers mk;
    for (int i = 0; i < 4; ++i) { mk.add(90.0e6 + 1.0e6 * i, 1700000000); }
    const std::string wholeMk = mk.serialize();
    const std::string wholePlan =
        "{\"name\":\"Test plan\",\"bands\":[{\"startHz\":88000000,\"endHz\":108000000,"
        "\"name\":\"FM\",\"service\":\"broadcast\"}]}\n";

    struct Variant {
        const char* what;
        std::function<void(const fs::path&, const std::string& whole)> make;
    };
    const std::vector<Variant> variants = {
        {"zero length", [](const fs::path& p, const std::string&) { writeText(p, ""); }},
        {"cut mid-token",
         [](const fs::path& p, const std::string& w) {
             // Back to the middle of a quoted token: inside a key or a value.
             std::size_t cut = w.size() * 6 / 10;
             while (cut > 1 && w[cut - 1] != 'a' && w[cut - 1] != 'e') { --cut; }
             writeText(p, cutOf(w, cut));
         }},
        {"replaced by a directory",
         [](const fs::path& p, const std::string&) {
             std::error_code e;
             fs::remove(p, e);
             fs::create_directories(p, e);
         }},
    };

    for (const Variant& v : variants) {
        // SETTINGS.
        {
            const fs::path p = dir / (std::string("config-") + std::to_string(&v - variants.data()) + ".json");
            v.make(p, wholeCfg);
            AppConfig out;
            out.centerHz = 1.0;  // proves the loader resets what it is given
            const LoadOutcome o = timedLoad([&](std::string& e) { return ConfigStore::load(p.string(), out, e); });
            CHECK(!o.ok);
            CHECK(!o.error.empty());
            CHECK(o.ms < 5000.0);
            CHECK(out.centerHz == AppConfig{}.centerHz);  // defaults, never a half-read file
            report("settings", v.what, o, "defaults kept");
        }
        // BOOKMARKS.
        {
            const fs::path p = dir / (std::string("bm-") + std::to_string(&v - variants.data()) + ".json");
            v.make(p, wholeBm);
            FreqManager m;
            cascade::core::Bookmark b;
            b.name = "already here";
            b.freqHz = 50.0e6;
            m.add(b);
            const LoadOutcome o = timedLoad([&](std::string& e) { return m.load(p.string(), e); });
            CHECK(o.ms < 5000.0);
            char after[64];
            std::snprintf(after, sizeof after, "list now %zu entries", m.list().size());
            report("bookmarks", v.what, o, after);
            // Pinned: what a damaged file leaves in the list. (See the application-level
            // finding below for what the NEXT SAVE then does with it.)
            if (!o.ok) { CHECK(!o.error.empty()); }
        }
        // MARKERS.
        {
            const fs::path p = dir / (std::string("mk-") + std::to_string(&v - variants.data()) + ".json");
            v.make(p, wholeMk);
            FreqMarkers m;
            m.add(70.0e6, 1700000000);
            const LoadOutcome o = timedLoad([&](std::string& e) { return m.load(p.string(), e); });
            CHECK(o.ms < 5000.0);
            char after[64];
            std::snprintf(after, sizeof after, "list now %zu markers", m.size());
            report("markers", v.what, o, after);
            if (!o.ok) { CHECK(!o.error.empty()); }
        }
        // A BAND PLAN.
        {
            const fs::path p = dir / (std::string("plan-") + std::to_string(&v - variants.data()) + ".json");
            v.make(p, wholePlan);
            cascade::core::BandPlan plan;
            const LoadOutcome o = timedLoad([&](std::string& e) { return plan.loadFile(p.string(), e); });
            CHECK(!o.ok);
            CHECK(!o.error.empty());
            CHECK(o.ms < 5000.0);
            report("band plan", v.what, o, "contents unchanged");
        }
    }

    // HELD OPEN BY ANOTHER PROGRAM so that it cannot be read (no sharing at all).
    {
        const fs::path p = dir / "held-config.json";
        writeText(p, wholeCfg);
        Held lock(p, /*exclusive=*/true);
        if (!lock.ok()) {
            std::printf("  settings, held exclusively: NOT REACHED\n");
        } else {
            AppConfig out;
            const LoadOutcome o = timedLoad([&](std::string& e) { return ConfigStore::load(p.string(), out, e); });
            CHECK(!o.ok);
            CHECK(contains(o.error, "cannot open"));
            CHECK(o.ms < 5000.0);
            report("settings", "held exclusively", o, "defaults kept");
        }
        const fs::path b = dir / "held-bm.json";
        writeText(b, wholeBm);
        Held lockB(b, true);
        if (lockB.ok()) {
            FreqManager m;
            const LoadOutcome o = timedLoad([&](std::string& e) { return m.load(b.string(), e); });
            CHECK(!o.ok);
            CHECK(o.ms < 5000.0);
            report("bookmarks", "held exclusively", o, "");
        }
        const fs::path k = dir / "held-mk.json";
        writeText(k, wholeMk);
        Held lockK(k, true);
        if (lockK.ok()) {
            FreqMarkers m;
            const LoadOutcome o = timedLoad([&](std::string& e) { return m.load(k.string(), e); });
            CHECK(!o.ok);
            CHECK(o.ms < 5000.0);
            report("markers", "held exclusively", o, "");
        }
    }
}

// --- 3. WHAT THE APPLICATION DOES WITH A DAMAGED FILE ------------------------------

// The files the application's start-up reads sit under %APPDATA%\foxsdr; each case gets
// a folder of its own so one case's save cannot be mistaken for another's.
fs::path appDataFor(const char* tag) {
    const fs::path root = g_scratch / (std::string("app-") + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "foxsdr", ec);
    setEnv("APPDATA", root.string());
    return root / "foxsdr";
}

std::vector<std::string> namesIn(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) { out.push_back(e.path().filename().string()); }
    std::sort(out.begin(), out.end());
    return out;
}

bool waitFor(const std::function<bool()>& cond, double ms) {
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// A saved file that is then cut, built the way the application builds it.
std::string damagedSettings(AppConfig& cfg) {
    cfg.sourceKind = "siggen";
    cfg.centerHz = 123456000.0;
    const std::string whole = ConfigStore::serialize(cfg);
    return whole.substr(0, whole.size() * 6 / 10);
}

void checkASavedDamagedSettingsFileIsLost() {
    const fs::path app = appDataFor("settings");
    const fs::path cfgPath = app / "config.json";
    AppConfig cfg;
    const std::string cut = damagedSettings(cfg);
    writeText(cfgPath, cut);
    {
        AppWindow w(cfgPath.string());
        // The reaction at start-up: the window is up, on defaults (the construction
        // defaults are KEPT) - and nothing in the window, the log or a report says the
        // file was bad: the reason goes to stderr (the console, when one is open).
        Access::saveNow(w);  // what the clean exit does
        CHECK(waitFor([&] { Access::pollConfig(w); return !Access::configInFlight(w); }, 5000.0));
        CHECK(Access::configLastOk(w));
    }
    const std::string after = readText(cfgPath);
    AppConfig back;
    std::string err;
    const bool valid = ConfigStore::load(cfgPath.string(), back, err);
    const std::vector<std::string> names = namesIn(app);
    CHECK(valid);
    CHECK(after != cut);
    // FINDING, PINNED: the damaged file is overwritten by the first save and no copy of
    // it is kept - a cut-off settings file that held the user's whole configuration
    // costs all of it, silently.
    CHECK(names.size() == 1u);
    std::printf("  FINDING settings: a cut-off config.json (%zu bytes) is replaced by defaults on the "
                "first save; files left beside it: %zu (no backup)\n",
                cut.size(), names.size());
}

void checkASavedDamagedBookmarkFileIsLost() {
    const fs::path app = appDataFor("bookmarks");
    FreqManager mgr;
    for (int i = 0; i < 5; ++i) {
        cascade::core::Bookmark b;
        b.name = "Station " + std::to_string(i);
        b.freqHz = 100.0e6 + 1.0e6 * i;
        b.mode = "NFM";
        mgr.add(std::move(b));
    }
    const std::string whole = mgr.serialize();
    const std::string cut = whole.substr(0, whole.size() * 6 / 10);
    const fs::path bmPath = app / "bookmarks.json";
    writeText(bmPath, cut);
    {
        AppWindow w((app / "config.json").string());
        CHECK(!Access::bookmarkError(w).empty());  // the red line in the Bookmarks section
        CHECK(Access::bookmarkCount(w) == 0u);
        Access::addBookmarkToView(w, "A new one", 144.8e6);
        Access::flushLists(w);
        CHECK(waitFor([&] { Access::flushLists(w); return !Access::listsInFlight(w); }, 5000.0));
        // The red line goes when the save succeeds: the user is told the file was bad only
        // until the moment it is destroyed.
        CHECK(Access::bookmarkError(w).empty());
    }
    FreqManager back;
    std::string err;
    CHECK(back.load(bmPath.string(), err));
    CHECK(back.list().size() == 1u);
    const std::vector<std::string> names = namesIn(app);
    std::printf("  FINDING bookmarks: a cut-off bookmarks.json (%zu of %zu bytes, 5 entries) is replaced "
                "by the first edit's one-entry list; the red error clears with that save; no backup "
                "(files beside it: %zu)\n",
                cut.size(), whole.size(), names.size());
}

void checkASavedDamagedMarkerFileIsLost() {
    const fs::path app = appDataFor("markers");
    FreqMarkers mk;
    for (int i = 0; i < 4; ++i) { mk.add(90.0e6 + 1.0e6 * i, 1700000000); }
    const std::string whole = mk.serialize();
    const std::string cut = whole.substr(0, whole.size() * 6 / 10);
    const fs::path mkPath = app / "markers.json";
    writeText(mkPath, cut);
    {
        AppWindow w((app / "config.json").string());
        CHECK(!Access::markerError(w).empty());
        CHECK(Access::markerCount(w) == 0u);
        Access::addMarker(w, 118.0e6);
        Access::flushLists(w);
        CHECK(waitFor([&] { Access::flushLists(w); return !Access::listsInFlight(w); }, 5000.0));
        CHECK(Access::markerError(w).empty());
    }
    FreqMarkers back;
    std::string err;
    CHECK(back.load(mkPath.string(), err));
    CHECK(back.size() == 1u);
    std::printf("  FINDING markers: a cut-off markers.json (%zu of %zu bytes, 4 markers) is replaced by the "
                "first marker dropped; the error clears with that save; no backup\n",
                cut.size(), whole.size());
}

// --- 4. A READ-ONLY SETTINGS FOLDER ------------------------------------------------

void checkAReadOnlySettingsFolder() {
    const fs::path app = appDataFor("readonly");
    const fs::path cfgPath = app / "config.json";
    std::string err;
    AppConfig seed;
    seed.sourceKind = "siggen";
    CHECK(ConfigStore::save(cfgPath.string(), seed, err));
    const std::string seedText = readText(cfgPath);

    ReadOnly ro(app);
    if (!ro.took) {
        std::printf("  read-only settings folder: NOT REACHED (the folder could not be made read-only)\n");
        return;
    }
    // AT START-UP: the window comes up on what the file holds; nothing is written.
    const double t0 = nowMs();
    std::atomic<int> attempts{0};
    {
        AppWindow w(cfgPath.string());
        const double ctorMs = nowMs() - t0;
        CHECK(ctorMs < 30000.0);
        Access::bindConfigWriter(w, [&attempts](const std::string& p, const std::string& t, std::string& e) {
            attempts.fetch_add(1);
            return ConfigStore::writeFile(p, t, e);
        });
        CHECK(readText(cfgPath) == seedText);

        // A change, as the user makes one. The debounce window passes; the write is refused.
        Access::touchSettings(w);
        double sim = 0.0;
        int failures = 0;
        for (; sim < 20.0; sim += 0.25) {
            Access::maybeSave(w, sim);
            Access::pollConfig(w);
            if (Access::configCompleted(w) > static_cast<unsigned>(failures)) {
                failures = static_cast<int>(Access::configCompleted(w));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        waitFor([&] { Access::pollConfig(w); return !Access::configInFlight(w); }, 3000.0);
        CHECK(attempts.load() >= 1);
        CHECK(!Access::configLastOk(w));
        CHECK(contains(Access::configLastError(w), "cannot create temp file"));
        std::printf("  read-only settings folder: %d write attempts in 20 simulated seconds, each refused "
                    "(\"%s\"); reported on stderr only\n",
                    attempts.load(), Access::configLastError(w).substr(0, 36).c_str());
        // FINDING, PINNED: the save is retried every debounce window for as long as the
        // folder stays read-only - at least twice in 20 s - and nothing in the window or
        // the log says so (one stderr line per attempt).
        CHECK(attempts.load() >= 2);

        // AT EXIT: the final save is refused too; it is waited for within its bound and
        // reported - the window does not hang.
        Access::saveNow(w);
        const double e0 = nowMs();
        const bool landed = Access::configDrain(w, cascade::gui::ConfigWriter::kSaveBound);
        const double exitMs = nowMs() - e0;
        CHECK(landed);
        CHECK(!Access::configLastOk(w));
        CHECK(exitMs < 5000.0);
        std::printf("  read-only settings folder at exit: the final save refused in %.0f ms, window closes\n",
                    exitMs);
    }
    CHECK(readText(cfgPath) == seedText);
    CHECK(tempFilesIn(app).empty());
}

// --- 5. THE LOG FILE AND A PLUGIN FILE HELD BY ANOTHER PROGRAM -----------------------

void checkTheLogFileHeldAtStartUp() {
    const fs::path dir = g_scratch / "log-held";
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeText(dir / "foxsdr.log", "earlier session\n");
    auto& log = cascade::core::DiagLog::instance();
    {
        Held lock(dir / "foxsdr.log", /*exclusive=*/true);
        if (!lock.ok()) {
            std::printf("  log file held at start-up: NOT REACHED\n");
            return;
        }
        const double t0 = nowMs();
        log.configure(dir.string(), true);
        const double took = nowMs() - t0;
        CHECK(took < 5000.0);
        // FINDING, PINNED: the Diagnostics switch is on and the file could not be opened,
        // and nothing says so - the switch stays on, no file is written, no line is logged
        // about it (the ring still works, which is all a freeze report needs).
        CHECK(!log.fileEnabled());
        CHECK(log.filePath().empty());
        cascade::core::diagLogf("failure-test: a line while the log file is held");
        bool inRing = false;
        for (const std::string& l : log.ringSnapshot()) {
            if (l.find("a line while the log file is held") != std::string::npos) { inRing = true; }
        }
        CHECK(inRing);
        std::printf("  FINDING log file held at start-up: configure() returned in %.0f ms with the file "
                    "OFF and no message anywhere; the ring still takes lines\n",
                    took);
    }
    // Let go, and it comes back at the next configure.
    log.configure(dir.string(), true);
    CHECK(log.fileEnabled());
    cascade::core::diagLogf("failure-test: a line after the log file was released");
    log.configure("", false);
    CHECK(readText(dir / "foxsdr.log").find("after the log file was released") != std::string::npos);
    log.resetForTest();
}

void checkAPluginFileThatCannotBeDeleted() {
    const fs::path dir = g_scratch / "plugin-locked";
    const std::string name = "adsb-1.0.0.dll";
    writeText(dir / name, "not really a module");
    std::string err;
    {
        Held lock(dir / name, /*exclusive=*/false);  // a loaded module: readable, not deletable
        if (!lock.ok()) {
            std::printf("  plugin file held: NOT REACHED\n");
            return;
        }
        // The Remove key.
        const double t0 = nowMs();
        cascade::core::PluginRepo repo;
        CHECK(!repo.remove(dir.string(), name, err));
        CHECK(nowMs() - t0 < 5000.0);
        CHECK(contains(err, "cannot delete"));
        CHECK(existsNow(dir / name));
        std::printf("  plugin Remove on a held file: \"%s\"; the file stays\n", err.substr(0, 28).c_str());
        // The clean-up of superseded copies: queued for the next start, said once.
        cascade::core::SupersededPlugin s;
        s.file = name;
        s.name = "adsb";
        s.version = "1.0.0";
        s.keptFile = "adsb-1.1.0.dll";
        s.keptVersion = "1.1.0";
        const cascade::core::PluginCleanupResult r = cascade::core::removeSupersededPlugins(
            dir.string(), {s}, cascade::core::defaultPluginFileRemover());
        CHECK(r.removed.empty());
        CHECK(r.queued.size() == 1u);
        CHECK(r.failed.empty());
        CHECK(cascade::core::loadPendingRemovals(dir.string()).size() == 1u);
        std::printf("  plugin clean-up on a held file: queued for the next start (%zu queued, %zu failed)\n",
                    r.queued.size(), r.failed.size());
    }
    // Let go: the next attempt deletes it and the queue empties.
    cascade::core::SupersededPlugin s;
    s.file = name;
    const cascade::core::PluginCleanupResult r = cascade::core::removeSupersededPlugins(
        dir.string(), {s}, cascade::core::defaultPluginFileRemover());
    CHECK(r.removed.size() == 1u);
    CHECK(!existsNow(dir / name));
    CHECK(cascade::core::loadPendingRemovals(dir.string()).empty());
}

// --- 6. THE EXPORT, THE PICTURE AND THE SCREENSHOT -----------------------------------

cascade::core::HostImage testImage() {
    cascade::core::HostImage im;
    im.plugin = "APT decoder";
    im.width = 16;
    im.height = 8;
    im.format = CASCADE_IMAGE_RGB24;
    im.complete = true;
    im.pixels.assign(16u * 8u * 3u, 0x7F);
    return im;
}

void checkTheExportWhenTheWriteFails() {
    AppWindow app;
    Access::addBookmarkToView(app, "One", 100.0e6);
    Access::addBookmarkToView(app, "Two", 101.0e6);
    // A write that fails part way.
    {
        const fs::path dir = g_scratch / "export-fail";
        Access::setRecordDir(app, dir.string());
        g_failWriter = "export";
        cascade::core::setWriteFaultHookForTest(&failThisWriter);
        Access::exportBookmarks(app);
        CHECK(waitFor([&] { Access::poll(app); return !Access::exportPending(app); }, 5000.0));
        cascade::core::setWriteFaultHookForTest(nullptr);
        g_failWriter = nullptr;
        CHECK(Access::note(app).rfind("Could not write ", 0) == 0);
        std::printf("  export, write fails part way: \"%s\"\n", Access::note(app).substr(0, 40).c_str());
    }
    // A read-only recordings folder.
    {
        const fs::path dir = g_scratch / "export-ro";
        std::error_code ec;
        fs::create_directories(dir, ec);
        ReadOnly ro(dir);
        if (ro.took) {
            Access::setRecordDir(app, dir.string());
            Access::exportBookmarks(app);
            CHECK(waitFor([&] { Access::poll(app); return !Access::exportPending(app); }, 5000.0));
            CHECK(Access::note(app).rfind("Could not write ", 0) == 0);
            CHECK(namesIn(dir).empty());
            std::printf("  export, read-only folder: \"%s\"; nothing written\n",
                        Access::note(app).substr(0, 40).c_str());
        } else {
            std::printf("  export, read-only folder: NOT REACHED\n");
        }
    }
    // The folder is a file.
    {
        const fs::path blocker = g_scratch / "export-blocker";
        writeText(blocker, "x");
        Access::setRecordDir(app, (blocker / "sub").string());
        Access::exportBookmarks(app);
        CHECK(waitFor([&] { Access::poll(app); return !Access::exportPending(app); }, 5000.0));
        CHECK(Access::note(app).rfind("Could not write ", 0) == 0);
    }
}

void checkThePictureSaveAndTheScreenshotWhenTheWriteFails() {
    AppWindow app;
    const fs::path dir = g_scratch / "image-fail";
    Access::setRecordDir(app, dir.string());
    // THE PICTURE: reported on screen, in its own words.
    g_failWriter = "bmp";
    cascade::core::setWriteFaultHookForTest(&failThisWriter);
    Access::saveImage(app, testImage());
    CHECK(waitFor([&] { Access::poll(app); return !Access::imagePending(app); }, 5000.0));
    CHECK(Access::imageNote(app).rfind("Save failed: ", 0) == 0);
    CHECK(contains(Access::imageNote(app), "failed part way through"));
    std::vector<std::string> files = namesIn(dir);
    // FINDING, PINNED: the picture that failed to write is left on disk under the name a
    // good one would have had.
    CHECK(files.size() == 1u);
    std::printf("  picture save, write fails part way: \"%s\"; files left in the folder: %zu (the "
                "failed picture is NOT removed)\n",
                Access::imageNote(app).substr(0, 24).c_str(), files.size());

    // THE SCREENSHOT: said in the log only, and the log line names the path.
    const fs::path shotDir = g_scratch / "shot-fail";
    std::error_code ec;
    fs::create_directories(shotDir, ec);
    Access::shot(app, shotDir.string(), testImage());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    Access::poll(app);
    cascade::core::setWriteFaultHookForTest(nullptr);
    g_failWriter = nullptr;
    bool said = false;
    bool namesAPath = false;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find("shot: writing") != std::string::npos) {
            said = true;
            if (l.find(shotDir.filename().string()) != std::string::npos) { namesAPath = true; }
        }
    }
    CHECK(said);
    std::printf("  screenshot, write fails part way: said in the log %s; FINDING: the log line %s the "
                "folder\n",
                said ? "yes" : "NO", namesAPath ? "NAMES" : "does not name");
}

}  // namespace

int main() {
    isolate();
    std::atexit(&allowAllDenied);
    ImGui::CreateContext();
    std::printf("test_failure_files\n");
    for (const WriterCase& w : writers()) { checkWriter(w); }
    checkLoaders();
    checkASavedDamagedSettingsFileIsLost();
    checkASavedDamagedBookmarkFileIsLost();
    checkASavedDamagedMarkerFileIsLost();
    checkAReadOnlySettingsFolder();
    checkTheLogFileHeldAtStartUp();
    checkAPluginFileThatCannotBeDeleted();
    checkTheExportWhenTheWriteFails();
    checkThePictureSaveAndTheScreenshotWhenTheWriteFails();
    allowAllDenied();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_failure_files");
}
