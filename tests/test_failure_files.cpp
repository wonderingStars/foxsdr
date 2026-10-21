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
//   3. A DAMAGED FILE IS KEPT ASIDE, NEVER SAVED OVER (0.99.65): the window is built on
//      one and a save is made - the damaged bytes are in a ".bad-<time>" copy, byte for
//      byte; with the rename refused the file is untouched after a save and after exit;
//      four damaged starts leave three copies; a first run and an empty file leave none.
//   4. A READ-ONLY SETTINGS FOLDER, at start-up and at exit - and the retry backs off
//      (0.99.65): a simulated hour is about eighteen attempts, a success resets it, the
//      failure is logged once per run, the exit save still tries once.
//   5. THE LOG FILE held by another program at start-up, and a plugin file that
//      cannot be deleted.
//   6. THE EXPORT, THE SCREENSHOT AND THE PICTURE SAVE when the write fails - the failed
//      picture is removed and no log line names a path - and two saves inside one
//      second are two files, a stranger's file under the name never overwritten.
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
#include <exception>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
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
//
// WINDOWS: an icacls deny of "add file" and "add folder" for this user. POSIX: the permission bits -
// the write bits taken off the folder (or file), which stops a file being created in it, renamed in
// it or removed from it. Run as root the bits stop nothing, so every caller PROVES the folder is
// read-only by writing a probe file (canWriteIn) and reports NOT REACHED when it is not.
bool denyWrite(const fs::path& dir) {
#if defined(_WIN32)
    const char* user = std::getenv("USERNAME");
    if (user == nullptr || *user == '\0') { return false; }
    const std::string cmd = "icacls \"" + dir.string() + "\" /deny \"" + user + ":(WD,AD)\"";
    if (!runHidden(cmd)) { return false; }
    denied().insert(dir.string());
    return true;
#else
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    if (ec) { return false; }
    denied().insert(dir.string());
    return true;
#endif
}

void allowWrite(const fs::path& dir) {
#if defined(_WIN32)
    const char* user = std::getenv("USERNAME");
    if (user == nullptr) { return; }
    runHidden("icacls \"" + dir.string() + "\" /remove:d \"" + user + "\"");
    denied().erase(dir.string());
#else
    std::error_code ec;
    fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
    denied().erase(dir.string());
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

// WHAT STOPS A FILE FROM BEING RENAMED, on either platform. Windows: another program holds it open
// without share-delete. POSIX has no such thing - a file that is open can be renamed, replaced and
// unlinked - so what stands in for it is the permission on its FOLDER (a rename needs write access
// there), made read-only for the life of the block. Either way the file can still be READ, which is
// what lets a loader see it is damaged before the rename is refused.
struct RenameBlock {
#if defined(_WIN32)
    Held held;
    RenameBlock(const fs::path& file, const fs::path&) : held(file, /*exclusive=*/false) {}
    bool ok() const { return held.ok(); }
    void release() { held.release(); }
#else
    fs::path dir;
    bool took = false;
    RenameBlock(const fs::path&, const fs::path& d) : dir(d) {
        took = denyWrite(dir) && !canWriteIn(dir);  // proved by writing, not assumed (root: not reached)
    }
    bool ok() const { return took; }
    void release() {
        if (took) { allowWrite(dir); }
        took = false;
    }
    ~RenameBlock() { release(); }
#endif
    RenameBlock(const RenameBlock&) = delete;
    RenameBlock& operator=(const RenameBlock&) = delete;
};

// WHAT STOPS A FILE FROM BEING OPENED FOR APPEND. Windows: held exclusively by another program.
// POSIX: the file's own write bits taken off (a file nobody may write is one fopen("ab") refuses),
// proved by trying to open it.
struct OpenBlock {
#if defined(_WIN32)
    Held held;
    explicit OpenBlock(const fs::path& file) : held(file, /*exclusive=*/true) {}
    bool ok() const { return held.ok(); }
    void release() { held.release(); }
#else
    fs::path file;
    bool took = false;
    explicit OpenBlock(const fs::path& f) : file(f) {
        if (denyWrite(file)) {
            std::ofstream probe(file, std::ios::app | std::ios::binary);
            took = !probe;  // root can still write: then there is nothing to test
            if (!took) { allowWrite(file); }
        }
    }
    bool ok() const { return took; }
    void release() {
        if (took) { allowWrite(file); }
        took = false;
    }
    ~OpenBlock() { release(); }
#endif
    OpenBlock(const OpenBlock&) = delete;
    OpenBlock& operator=(const OpenBlock&) = delete;
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

// --- 3. A DAMAGED FILE IS KEPT ASIDE, AND NEVER SAVED OVER (0.99.65) ------------------
//
// config.json, bookmarks.json and markers.json: when one exists, is not empty and FAILS TO LOAD, the
// application renames it to "<name>.bad-<UTC yyyymmdd-hhmmss>" beside it at start-up, before anything
// can save over it, and keeps the three newest such copies. If the rename is refused (the file is
// held by another program, the folder is read-only) the saver for that file is told not to write for
// the rest of the session, and the damaged file stays exactly as it was. A missing file (a first run)
// and an empty one have nothing to keep. The band plan is never written by the application, so it
// has nothing to protect.

// The files the application's start-up reads sit under %APPDATA%\foxsdr; each case gets
// a folder of its own so one case's save cannot be mistaken for another's.
fs::path appDataFor(const std::string& tag) {
    const fs::path root = g_scratch / (std::string("app-") + tag);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "foxsdr", ec);
    setEnv("APPDATA", root.string());           // where Windows keeps them
    setEnv("XDG_CONFIG_HOME", root.string());   // and where POSIX does (ConfigStore::defaultPath)
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

// How many lines in the diagnostic log (its in-memory ring) contain `needle`.
int logCount(const char* needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// Every line in the ring that names the scratch folder, which stands for "a path": log lines never
// name one.
std::vector<std::string> linesNamingAPath() {
    std::vector<std::string> out;
    const std::string root = g_scratch.string();
    const std::string leaf = g_scratch.filename().string();
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(root) != std::string::npos || l.find(leaf) != std::string::npos) { out.push_back(l); }
    }
    return out;
}

// The ".bad-" copies of `leaf` in `dir`, oldest name first.
std::vector<std::string> badCopiesOf(const fs::path& dir, const std::string& leaf) {
    std::vector<std::string> out;
    for (const std::string& n : namesIn(dir)) {
        if (n.rfind(leaf + ".bad-", 0) == 0) { out.push_back(n); }
    }
    return out;
}

// "<leaf>.bad-yyyymmdd-hhmmss" with an optional "-N" after it.
bool looksLikeABadCopy(const std::string& name, const std::string& leaf) {
    const std::string head = leaf + ".bad-";
    if (name.rfind(head, 0) != 0) { return false; }
    const std::string rest = name.substr(head.size());
    if (rest.size() < 15) { return false; }
    for (std::size_t i = 0; i < 15; ++i) {
        const char c = rest[i];
        if (i == 8) {
            if (c != '-') { return false; }
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return rest.size() == 15 || rest[15] == '-';
}

// One of the three files, and how to make the application save it.
struct Kind {
    std::string which;  // the word the log line starts with
    std::string leaf;   // the file's name in the settings folder
    std::string whole;  // a good file's text
    // Makes the application save this file, and waits for the save to land or be refused.
    std::function<void(AppWindow&)> save;
    // Whether the red line the Bookmarks / Markers section shows is still up.
    std::function<bool(AppWindow&)> errorUp;
    // Whether `path` holds a file this kind loads.
    std::function<bool(const fs::path&)> loads;
};

std::vector<Kind> kinds() {
    std::vector<Kind> v;
    {
        AppConfig cfg;
        cfg.sourceKind = "siggen";
        cfg.centerHz = 123456000.0;
        Kind k;
        k.which = "settings";
        k.leaf = "config.json";
        k.whole = ConfigStore::serialize(cfg);
        k.save = [](AppWindow& w) {
            Access::saveNow(w);  // what the clean exit does
            waitFor([&] { Access::pollConfig(w); return !Access::configInFlight(w); }, 5000.0);
        };
        k.errorUp = [](AppWindow&) { return true; };  // settings have no red line: the log says it
        k.loads = [](const fs::path& p) {
            AppConfig back;
            std::string err;
            return ConfigStore::load(p.string(), back, err);
        };
        v.push_back(std::move(k));
    }
    {
        FreqManager mgr;
        for (int i = 0; i < 5; ++i) {
            cascade::core::Bookmark b;
            b.name = "Station " + std::to_string(i);
            b.freqHz = 100.0e6 + 1.0e6 * i;
            b.mode = "NFM";
            mgr.add(std::move(b));
        }
        Kind k;
        k.which = "bookmarks";
        k.leaf = "bookmarks.json";
        k.whole = mgr.serialize();
        k.save = [](AppWindow& w) {
            Access::addBookmarkToView(w, "A new one", 144.8e6);
            Access::flushLists(w);
            waitFor([&] { Access::flushLists(w); return !Access::listsInFlight(w); }, 5000.0);
        };
        k.errorUp = [](AppWindow& w) { return !Access::bookmarkError(w).empty(); };
        k.loads = [](const fs::path& p) {
            FreqManager back;
            std::string err;
            return back.load(p.string(), err);
        };
        v.push_back(std::move(k));
    }
    {
        FreqMarkers mk;
        for (int i = 0; i < 4; ++i) { mk.add(90.0e6 + 1.0e6 * i, 1700000000); }
        Kind k;
        k.which = "markers";
        k.leaf = "markers.json";
        k.whole = mk.serialize();
        k.save = [](AppWindow& w) {
            Access::addMarker(w, 118.0e6);
            Access::flushLists(w);
            waitFor([&] { Access::flushLists(w); return !Access::listsInFlight(w); }, 5000.0);
        };
        k.errorUp = [](AppWindow& w) { return !Access::markerError(w).empty(); };
        k.loads = [](const fs::path& p) {
            FreqMarkers back;
            std::string err;
            return back.load(p.string(), err);
        };
        v.push_back(std::move(k));
    }
    return v;
}

std::string cutMidToken(const std::string& whole) { return whole.substr(0, whole.size() * 6 / 10); }

// A cut-off file is kept beside the live one, byte for byte, and the live file is a good new one.
void checkADamagedFileIsKeptAside(const Kind& k) {
    const fs::path app = appDataFor(k.which);
    const fs::path live = app / k.leaf;
    const std::string cut = cutMidToken(k.whole);
    writeText(live, cut);
    const int keptBefore = logCount("was kept aside");
    {
        AppWindow w((app / "config.json").string());
        // The red line the lists already show is still up (nothing new on screen).
        if (k.which != "settings") { CHECK(k.errorUp(w)); }
        k.save(w);
    }
    const std::vector<std::string> bad = badCopiesOf(app, k.leaf);
    CHECK(bad.size() == 1u);
    if (bad.size() == 1u) {
        CHECK(looksLikeABadCopy(bad[0], k.leaf));
        CHECK(readText(app / bad[0]) == cut);  // the damaged bytes, byte for byte
    }
    CHECK(k.loads(live));                      // and the live file is a good, new one
    CHECK(readText(live) != cut);
    CHECK(logCount("was kept aside") == keptBefore + 1);
    // ONE line, saying which file and how big, and naming no path.
    bool said = false;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(k.which + ": a damaged file (" + std::to_string(cut.size()) + " bytes) was kept aside") !=
            std::string::npos) {
            said = true;
        }
    }
    CHECK(said);
    CHECK(linesNamingAPath().empty());
    std::printf("  %s: a cut-off file (%zu bytes) is kept as a .bad-<time> copy, byte for byte; the live file "
                "is a good new one; one log line\n",
                k.which.c_str(), cut.size());
}

// The rename is REFUSED (another program holds the file): nothing is saved over it this session, the
// damaged file is exactly what it was after a save attempt and after exit, and the log says so once.
void checkARefusedRenameLeavesTheDamagedFileAlone(const Kind& k) {
    const fs::path app = appDataFor(k.which + "-held");
    const fs::path live = app / k.leaf;
    const std::string cut = cutMidToken(k.whole);
    writeText(live, cut);
    const int refusedBefore = logCount("could not be kept aside");
    const int keptBefore = logCount("was kept aside");
    {
        RenameBlock lock(live, app);  // loads fine (still readable), cannot be renamed
        if (!lock.ok()) {
            std::printf("  %s, rename refused: NOT REACHED\n", k.which.c_str());
            return;
        }
        AppWindow w((app / "config.json").string());
        // LET GO, so that it is OUR POLICY and not the held file that keeps the save away: a write
        // that were tried would now succeed.
        lock.release();
        k.save(w);
        if (k.which != "settings") { CHECK(k.errorUp(w)); }  // the red line stays up
        CHECK(readText(live) == cut);                        // untouched after the save attempt
        // A second request, as the debounce makes: still nothing.
        k.save(w);
        CHECK(readText(live) == cut);
    }
    CHECK(readText(live) == cut);                            // and after exit
    CHECK(badCopiesOf(app, k.leaf).empty());
    CHECK(tempFilesIn(app).empty());
    CHECK(logCount("could not be kept aside") == refusedBefore + 1);  // once
    CHECK(logCount("was kept aside") == keptBefore);
    CHECK(linesNamingAPath().empty());
    std::printf("  %s, rename refused: the damaged file is byte for byte what it was after a save attempt "
                "and after exit; no copy, no temp; one log line\n",
                k.which.c_str());
}

// Four damaged starts leave THREE copies, the three newest.
void checkOnlyTheThreeNewestCopiesAreKept(const Kind& k) {
    const fs::path app = appDataFor(k.which + "-four");
    const fs::path live = app / k.leaf;
    std::vector<std::string> damaged;
    for (int i = 0; i < 4; ++i) {
        damaged.push_back(cutMidToken(k.whole) + "#" + std::to_string(i));
        writeText(live, damaged.back());
        AppWindow w((app / "config.json").string());  // each start finds a damaged file
    }
    const std::vector<std::string> bad = badCopiesOf(app, k.leaf);
    CHECK(bad.size() == 3u);
    if (bad.size() == 3u) {
        // Oldest name first: the copies of the second, third and fourth starts.
        CHECK(readText(app / bad[0]) == damaged[1]);
        CHECK(readText(app / bad[1]) == damaged[2]);
        CHECK(readText(app / bad[2]) == damaged[3]);
    }
    std::printf("  %s: four damaged starts leave %zu .bad copies, the three newest\n", k.which.c_str(),
                bad.size());
}

// A first run (no file) and an empty file have nothing to keep.
void checkAFirstRunAndAnEmptyFileKeepNothing(const Kind& k) {
    {
        const fs::path app = appDataFor(k.which + "-first");
        {
            AppWindow w((app / "config.json").string());
            k.save(w);
        }
        CHECK(badCopiesOf(app, k.leaf).empty());
        CHECK(k.loads(app / k.leaf));
    }
    {
        const fs::path app = appDataFor(k.which + "-empty");
        writeText(app / k.leaf, "");
        const int keptBefore = logCount("was kept aside");
        const int refusedBefore = logCount("could not be kept aside");
        {
            AppWindow w((app / "config.json").string());
            k.save(w);
        }
        CHECK(badCopiesOf(app, k.leaf).empty());
        CHECK(k.loads(app / k.leaf));  // an empty file was nothing to lose: the save wrote a good one
        CHECK(logCount("was kept aside") == keptBefore);
        CHECK(logCount("could not be kept aside") == refusedBefore);
    }
    std::printf("  %s: a first run and a zero-byte file leave no copy and say nothing\n", k.which.c_str());
}

void checkDamagedFilesAreKeptAside() {
    for (const Kind& k : kinds()) {
        checkADamagedFileIsKeptAside(k);
        checkARefusedRenameLeavesTheDamagedFileAlone(k);
        checkOnlyTheThreeNewestCopiesAreKept(k);
        checkAFirstRunAndAnEmptyFileKeepNothing(k);
    }
}

// --- 4. A READ-ONLY SETTINGS FOLDER (0.99.65: the retry backs off) --------------------

// The writer the window's settings use, counting its attempts against a SIMULATED clock.
struct Attempts {
    std::mutex m;
    std::vector<double> at;
    std::atomic<double> sim{0.0};
    void note() {
        std::lock_guard<std::mutex> lock(m);
        at.push_back(sim.load());
    }
    std::vector<double> times() {
        std::lock_guard<std::mutex> lock(m);
        return at;
    }
};

void checkAReadOnlySettingsFolder() {
    const fs::path app = appDataFor("readonly");
    const fs::path cfgPath = app / "config.json";
    std::string err;
    AppConfig seed;
    seed.sourceKind = "siggen";
    CHECK(ConfigStore::save(cfgPath.string(), seed, err));
    const std::string seedText = readText(cfgPath);

    if (!denyWrite(app) || canWriteIn(app)) {
        allowWrite(app);
        std::printf("  read-only settings folder: NOT REACHED (the folder could not be made read-only)\n");
        return;
    }
    Attempts attempts;
    const int loggedBefore = logCount("settings could not be saved");
    {
        // AT START-UP: the window comes up on what the file holds; nothing is written.
        const double t0 = nowMs();
        AppWindow w(cfgPath.string());
        CHECK(nowMs() - t0 < 30000.0);
        Access::bindConfigWriter(w, [&attempts](const std::string& p, const std::string& t, std::string& e) {
            attempts.note();
            return ConfigStore::writeFile(p, t, e);
        });
        CHECK(readText(cfgPath) == seedText);

        // A CHANGE, and then a SIMULATED HOUR of a folder that stays read-only. The simulated clock
        // is the one maybeSaveConfig is given, so an hour costs a few real seconds.
        Access::touchSettings(w);
        double sim = 0.0;
        for (; sim < 3600.0; sim += 1.0) {
            attempts.sim.store(sim);
            Access::maybeSave(w, sim);
            Access::pollConfig(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        waitFor([&] { Access::pollConfig(w); return !Access::configInFlight(w); }, 3000.0);
        const std::vector<double> at = attempts.times();
        CHECK(!Access::configLastOk(w));
        CHECK(contains(Access::configLastError(w), "cannot create temp file"));
        std::printf("  read-only settings folder: %zu write attempts in a simulated hour, each refused\n",
                    at.size());
        // BOUNDED: the wait doubles from the debounce window up to five minutes, so an hour holds
        // about eighteen attempts - not one every two seconds (about 1800).
        CHECK(at.size() >= 10u);
        CHECK(at.size() <= 22u);
        // THE GAPS GROW, to the cap and no further.
        std::vector<double> gaps;
        for (std::size_t i = 1; i < at.size(); ++i) { gaps.push_back(at[i] - at[i - 1]); }
        bool growing = true;
        for (std::size_t i = 1; i < gaps.size(); ++i) {
            if (gaps[i] + 1.0 < gaps[i - 1]) { growing = false; }
        }
        CHECK(growing);
        if (!gaps.empty()) {
            CHECK(gaps.front() <= 8.0);
            CHECK(gaps.back() >= 250.0);
            CHECK(gaps.back() <= 305.0);
            std::printf("  gaps between attempts: first %.0f s, last %.0f s (cap 300 s)\n", gaps.front(),
                        gaps.back());
        }
        // THE FAILURE IS LOGGED ONCE for the whole run of failures, and names no path.
        CHECK(logCount("settings could not be saved") == loggedBefore + 1);
        CHECK(linesNamingAPath().empty());

        // THE FIRST SUCCESS RESETS IT: the folder is writable again, and the next scheduled attempt lands.
        allowWrite(app);
        CHECK(canWriteIn(app));
        bool saved = false;
        for (double s = sim; s < sim + 400.0 && !saved; s += 1.0) {
            attempts.sim.store(s);
            Access::maybeSave(w, s);
            Access::pollConfig(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            saved = Access::configCompleted(w) > 0u && Access::configLastOk(w) && !Access::configInFlight(w);
            sim = s;
        }
        CHECK(saved);
        // ...and a NEW run of failures is retried at the debounce window again, not at five minutes,
        // and is logged again.
        CHECK(denyWrite(app));
        CHECK(!canWriteIn(app));
        Access::touchSettings(w);
        const std::size_t before = attempts.times().size();
        double firstGap = -1.0;
        const double changedAt = sim;
        for (double s = sim + 1.0; s < sim + 60.0; s += 1.0) {
            attempts.sim.store(s);
            Access::maybeSave(w, s);
            Access::pollConfig(w);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (firstGap < 0.0 && attempts.times().size() > before) { firstGap = s - changedAt; }
        }
        waitFor([&] { Access::pollConfig(w); return !Access::configInFlight(w); }, 3000.0);
        CHECK(firstGap > 0.0);
        CHECK(firstGap <= 6.0);
        // ...and the wait after the first failure of the NEW run starts again from the debounce
        // window (about 4 s), not from where the last run had got to (five minutes): a second
        // attempt comes within a minute.
        const std::vector<double> run2 = attempts.times();
        double secondGap = -1.0;
        if (run2.size() >= before + 2) { secondGap = run2[before + 1] - run2[before]; }
        CHECK(secondGap > 0.0);
        CHECK(secondGap <= 9.0);
        CHECK(logCount("settings could not be saved") == loggedBefore + 2);
        std::printf("  after a success the first retry came %.0f s after the change and the second %.0f s "
                    "after that (not five minutes), and the new run of failures was logged again\n",
                    firstGap, secondGap);

        // AT EXIT: the final save still tries once, whatever the back-off - refused, bounded, no hang.
        const std::size_t beforeExit = attempts.times().size();
        Access::saveNow(w);
        const double e0 = nowMs();
        const bool landed = Access::configDrain(w, cascade::gui::ConfigWriter::kSaveBound);
        const double exitMs = nowMs() - e0;
        CHECK(landed);
        CHECK(!Access::configLastOk(w));
        CHECK(exitMs < 5000.0);
        CHECK(attempts.times().size() == beforeExit + 1);
        std::printf("  read-only settings folder at exit: one more attempt, refused in %.0f ms, window "
                    "closes\n",
                    exitMs);
    }
    allowWrite(app);
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
        OpenBlock lock(dir / "foxsdr.log");
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

// TWO SAVES INSIDE ONE SECOND ARE TWO FILES (0.99.65, core/unique_file.hpp). A picture is named for its
// plugin and the second it was saved, an export for the second: the second save used to open the same
// name over the first, and a file another program had left there was opened over too. Each now takes
// "name.ext", then "name-2.ext" ..., on the worker, and the note names the file that is on disk.
std::time_t alignToTheSecond() {
    const std::time_t tick = std::time(nullptr);
    while (std::time(nullptr) == tick) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    return std::time(nullptr);
}

std::string stampWithDashes(std::time_t t) {
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tmv);
    return buf;
}

void checkTwoPictureSavesInOneSecondAreTwoFiles() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        AppWindow app;
        const fs::path dir = g_scratch / ("pictures-" + std::to_string(attempt));
        Access::setRecordDir(app, dir.string());
        const std::time_t second = alignToTheSecond();
        const std::string stamp = stampWithDashes(second);
        // A file another program left under the name the first save would get.
        writeText(dir / ("APT_decoder-" + stamp + ".bmp"), "FOREIGN-PICTURE");
        Access::saveImage(app, testImage());
        CHECK(waitFor([&] { Access::poll(app); return !Access::imagePending(app); }, 5000.0));
        const std::string first = Access::imageNote(app);
        Access::saveImage(app, testImage());
        CHECK(waitFor([&] { Access::poll(app); return !Access::imagePending(app); }, 5000.0));
        const std::string second2 = Access::imageNote(app);
        if (stampWithDashes(std::time(nullptr)) != stamp) { continue; }  // the second ticked over
        const std::vector<std::string> names = namesIn(dir);
        CHECK(names.size() == 3u);  // the stranger's, the first save's, the second's
        CHECK(readText(dir / ("APT_decoder-" + stamp + ".bmp")) == "FOREIGN-PICTURE");
        CHECK(first.rfind("Saved ", 0) == 0);
        CHECK(second2.rfind("Saved ", 0) == 0);
        CHECK(first != second2);
        // The note names the file that is on disk, for each.
        CHECK(fs::exists(fs::path(first.substr(6))));
        CHECK(fs::exists(fs::path(second2.substr(6))));
        CHECK(fs::path(first.substr(6)).filename().string() == "APT_decoder-" + stamp + "-2.bmp");
        CHECK(fs::path(second2.substr(6)).filename().string() == "APT_decoder-" + stamp + "-3.bmp");
        std::printf("  two picture saves inside one second beside a stranger's file: %zu files, the "
                    "stranger's untouched, each note names the file on disk\n",
                    names.size());
        return;
    }
    std::printf("  two picture saves inside one second: NOT REACHED (the second ticked over every time)\n");
}

void checkTwoExportsInOneSecondAreTwoFiles() {
    for (int attempt = 0; attempt < 4; ++attempt) {
        AppWindow app;
        const fs::path dir = g_scratch / ("exports-" + std::to_string(attempt));
        Access::setRecordDir(app, dir.string());
        Access::addBookmarkToView(app, "One", 100.0e6);
        Access::addBookmarkToView(app, "Two", 101.0e6);
        const std::time_t second = alignToTheSecond();
        const std::string stamp = stampWithDashes(second);
        Access::exportBookmarks(app);
        CHECK(waitFor([&] { Access::poll(app); return !Access::exportPending(app); }, 5000.0));
        const std::string first = Access::note(app);
        Access::exportBookmarks(app);
        CHECK(waitFor([&] { Access::poll(app); return !Access::exportPending(app); }, 5000.0));
        const std::string second2 = Access::note(app);
        if (stampWithDashes(std::time(nullptr)) != stamp) { continue; }
        const std::vector<std::string> names = namesIn(dir);
        CHECK(names.size() == 2u);
        const std::string base = "foxsdr-frequencies-" + stamp;
        CHECK(first == "Exported 2 to " + dir.string() + "/" + base + ".xml");
        CHECK(second2 == "Exported 2 to " + dir.string() + "/" + base + "-2.xml");
        CHECK(std::find(names.begin(), names.end(), base + ".xml") != names.end());
        CHECK(std::find(names.begin(), names.end(), base + "-2.xml") != names.end());
        std::printf("  two exports inside one second: %zu files, the notes name the files on disk\n",
                    names.size());
        return;
    }
    std::printf("  two exports inside one second: NOT REACHED (the second ticked over every time)\n");
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

// A WRITE THAT FAILS leaves no file behind, and a log line never names a path (0.99.65). Before this, the
// picture that failed to write was left on disk under the name a good one would have had, and the
// screenshot's log line carried the folder's path inside the writer's own sentence.
void checkThePictureSaveAndTheScreenshotWhenTheWriteFails() {
    AppWindow app;
    const fs::path dir = g_scratch / "image-fail";
    Access::setRecordDir(app, dir.string());
    // THE PICTURE: reported on screen, in its own words - and nothing is left on disk.
    g_failWriter = "bmp";
    cascade::core::setWriteFaultHookForTest(&failThisWriter);
    Access::saveImage(app, testImage());
    CHECK(waitFor([&] { Access::poll(app); return !Access::imagePending(app); }, 5000.0));
    CHECK(Access::imageNote(app).rfind("Save failed: ", 0) == 0);
    CHECK(contains(Access::imageNote(app), "failed part way through"));
    std::vector<std::string> files = namesIn(dir);
    CHECK(files.empty());
    std::printf("  picture save, write fails part way: \"%s\"; files left in the folder: %zu\n",
                Access::imageNote(app).substr(0, 24).c_str(), files.size());

    // THE SCREENSHOT, the same failure: said in the log WITHOUT the path, and the picture removed.
    const fs::path shotDir = g_scratch / "shot-fail";
    std::error_code ec;
    fs::create_directories(shotDir, ec);
    const int saidBefore = logCount("shot: a screenshot could not be written");
    Access::shot(app, shotDir.string(), testImage());
    CHECK(waitFor([&] {
        Access::poll(app);
        return logCount("shot: a screenshot could not be written") > saidBefore;
    }, 5000.0));
    cascade::core::setWriteFaultHookForTest(nullptr);
    g_failWriter = nullptr;
    CHECK(logCount("shot: a screenshot could not be written") == saidBefore + 1);
    CHECK(namesIn(shotDir).empty());  // the picture that failed is gone; the window list beside it is not
    // The cause is in the words ("failed part way"), the folder is not.
    bool saidTheCause = false;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find("shot: a screenshot could not be written") != std::string::npos &&
            l.find("part way") != std::string::npos) {
            saidTheCause = true;
        }
    }
    CHECK(saidTheCause);
    CHECK(linesNamingAPath().empty());

    // THE SAME WITH A FOLDER THAT IS NOT THERE: the cause is "could not be opened", still no path.
    const int openBefore = logCount("shot: a screenshot could not be written");
    Access::shot(app, (g_scratch / "no-such-shot-folder").string(), testImage());
    CHECK(waitFor([&] {
        Access::poll(app);
        return logCount("shot: a screenshot could not be written") > openBefore;
    }, 5000.0));
    CHECK(linesNamingAPath().empty());
    std::printf("  screenshot, write fails: one log line saying why, no path in it; the failed picture is "
                "removed\n");
}

}  // namespace

// A case that THROWS is a failed check with its message, never a terminate: an uncaught exception
// ended test_failure_files on Linux in 0.08 s and took every later case with it (0.99.65).
#define GUARDED(call)                                                              \
    do {                                                                           \
        try {                                                                      \
            call;                                                                  \
        } catch (const std::exception& e) {                                        \
            std::printf("  EXCEPTION in %s: %s\n", #call, e.what());                \
            CHECK(false);                                                          \
        } catch (...) {                                                            \
            std::printf("  EXCEPTION in %s: (not a std::exception)\n", #call);      \
            CHECK(false);                                                          \
        }                                                                          \
    } while (0)

int main() {
    isolate();
    std::atexit(&allowAllDenied);
    ImGui::CreateContext();
    std::printf("test_failure_files\n");
    for (const WriterCase& w : writers()) { GUARDED(checkWriter(w)); }
    GUARDED(checkLoaders());
    GUARDED(checkDamagedFilesAreKeptAside());
    GUARDED(checkAReadOnlySettingsFolder());
    GUARDED(checkTheLogFileHeldAtStartUp());
    GUARDED(checkAPluginFileThatCannotBeDeleted());
    GUARDED(checkTheExportWhenTheWriteFails());
    GUARDED(checkTwoPictureSavesInOneSecondAreTwoFiles());
    GUARDED(checkTwoExportsInOneSecondAreTwoFiles());
    GUARDED(checkThePictureSaveAndTheScreenshotWhenTheWriteFails());
    allowAllDenied();
    ImGui::DestroyContext();
    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_failure_files");
}
