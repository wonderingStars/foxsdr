/*
 * THE "Import a frequency list" WINDOW, END TO END (0.99.80).
 *
 * A field report from an SWL (French interface, FoxSDR 0.99.78): "frequency export works fine, but
 * import does not. When I type the name of the file to load, it shows an error (import impossible:
 * cannot open)". Export for SDR# writes into the recordings folder and shows the name; the Import box
 * wanted a path and opened a bare name beside the program, where it never was. The owner's request:
 * "make a drag and drop window for it when they try to import". Pressing Import with the box empty,
 * and any import that comes back with no rows, now opens a window with a drop zone, a Browse key
 * and Cancel; and a bare file name is looked for in the recordings folder.
 *
 * Runs cascade itself, isolated like every test that starts the application (APPDATA, LOCALAPPDATA
 * and USERPROFILE in a scratch tree - the recordings folder is <USERPROFILE>/Documents/SDR-recordings,
 * so a file the test writes there is where Export would have put one), in ONE viewport. TOOLS >
 * Bookmarks is opened with a name in the import box by FOXSDR_BOOKMARK_IMPORT, and the keys are
 * pressed by the input script (FOXSDR_INPUT_SCRIPT) at the rectangles the UI census measured in a
 * first run. A native dialog cannot be pressed from a test, so Browse's answer is the seam
 * FOXSDR_FILE_PICK (bounded --frames runs only), as in test_plugin_pathpick_app.cpp.
 *
 * THE CENSUS KEEPS THE LAST RECTANGLE A PART WAS DRAWN AT, for the whole run, so it says that the
 * window WAS drawn and never that it still is. What it is at the end of a run is read from the
 * screenshot's window list: FOXSDR_SHOT_AT_FRAME writes shot-<n>.windows.txt beside the picture, one
 * line per ImGui window drawn in that frame, and the window is up when its name is on a line.
 *
 * WHAT IS PROVED, scenario by scenario (each one a real process):
 *
 *   layout   the Bookmarks section notes its field and its Import key in the census, and with no press
 *            there is no window (the control for everything below);
 *   failure  Import pressed with a name that is not in the recordings folder: the window opens
 *            (drop zone at least 380 x 110, Browse, Cancel, all inside the window, and up at the
 *            end of the run); the control, stopped before the key is released, shows none;
 *   empty    the box cleared and Import pressed: the window opens, and NO import was attempted (the
 *            log has no "an import ... failed" line) - it opened because the box was empty; the
 *            control, stopped before the key is released, shows none;
 *   chosen   Browse answered with a CSV of three rows: the three rows are in the bookmarks file the
 *            run leaves, and the window has closed itself;
 *   cancel   Browse answered with a cancelled dialog: no rows, and the window is still up;
 *   escape   the window closes on Escape;
 *   close    the window closes on Cancel;
 *   bare     FOXSDR_BOOKMARK_IMPORT names a CSV that is in the recordings folder: Import reads it
 *            (three rows saved) and no window ever opens;
 *   airband  the AIRBAND section's Import with its box empty opens the window (nothing was imported),
 *            and Browse's file joins the section's preset: three rows, all in the group "Manual",
 *            all ticked;
 *   shapes   (0.99.82, ledger S47; stub campaign B8 - tests/test_gui_file_jobs.cpp has the full table, and is
 *            B19's) what a person pastes or drags in: a full path in single quotes with the space a paste
 *            leaves, a file:// address, a name with a folder part (lists/nested.csv, in the recordings
 *            folder) and a bare name in double quotes. Each is a file that exists: three rows come in and no
 *            window opens. The window cleans the path BEFORE the op.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// Set, or (empty) removed.
void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

// Set and EMPTY, which setEnv cannot spell (it removes the variable): the child sees
// FOXSDR_FILE_PICK present with no text, which the seam reads as "the dialog was cancelled".
void setEnvEmpty(const char* name) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, "");
#else
    ::setenv(name, "", 1);
#endif
}

std::string exePath() {
#if defined(_WIN32)
    return std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    return std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
}

std::string run(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;        // what the process printed
    std::string bookmarks;  // the bookmarks file as the run left it
    std::string windows;    // the window list of the screenshot frame
    std::string log;        // the diagnostic log
    bool windowUp = false;  // the "Import a frequency list" window is on that list
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    int itemsWithPrefix(const std::string& prefix) const {
        int n = 0;
        for (const std::string& s : items) { n += s.rfind(prefix, 0) == 0 ? 1 : 0; }
        return n;
    }
    int rectsWithPrefix(const std::string& prefix) const {
        int n = 0;
        for (const auto& kv : rects) { n += kv.first.rfind(prefix, 0) == 0 ? 1 : 0; }
        return n;
    }
    // How many times `needle` is in the bookmarks file the run left.
    int count(const std::string& needle) const {
        int n = 0;
        for (std::size_t at = bookmarks.find(needle); at != std::string::npos; at = bookmarks.find(needle, at + 1)) {
            ++n;
        }
        return n;
    }
    int rows() const { return count("\"freqHz\""); }
};

fs::path g_dir;

const int kWidth = 1600;
const int kHeight = 1000;
const char* const kWindowName = "Import a frequency list###import_list";

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

fs::path recordingsDir() { return g_dir / "home" / "Documents" / "SDR-recordings"; }
fs::path bookmarksFile() { return g_dir / "appdata" / "foxsdr" / "bookmarks.json"; }

// Three NFM rows: what a Browse that chooses a file, and a bare name found in the recordings folder, read.
std::string threeRows(const std::string& tag) {
    std::string s = "frequency,name,mode\n";
    for (int i = 0; i < 3; ++i) {
        s += std::to_string(100000000 + 1100000 * i) + "," + tag + " " + std::to_string(i) + ",NFM\n";
    }
    return s;
}

struct RunOptions {
    int frames = 40;
    std::string script;        // FOXSDR_INPUT_SCRIPT, empty = none
    std::string pick;          // FOXSDR_FILE_PICK: the path "chosen"
    bool pickEmpty = false;    // FOXSDR_FILE_PICK present and empty: the dialog cancelled
    std::string importName;    // FOXSDR_BOOKMARK_IMPORT: opens TOOLS > Bookmarks with this in the box
    int shotAt = 0;            // FOXSDR_SHOT_AT_FRAME: read the window list of that frame (0 = none)
    int railBank = 1;          // the rail's bank the config opens on (2 = TOOLS, where AIRBAND is)
    std::string openSections;  // FOXSDR_OPEN_SECTIONS: "airband" opens that section
};

Result once(const std::string& tag, const RunOptions& o) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    const fs::path shots = g_dir / (tag + "-shots");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
        fs::remove_all(shots, ec);
        fs::create_directories(shots, ec);
        fs::remove(bookmarksFile(), ec);  // every run begins with an empty list
    }
    writeFile(cfg,
              "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
              "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
              "\"receiver\", \"railBank\": " +
                  std::to_string(o.railBank) + ", \"bandPlanOverlay\": false }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, o.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", o.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    setEnv("FOXSDR_BOOKMARK_IMPORT", o.importName);
    setEnv("FOXSDR_OPEN_SECTIONS", o.openSections);
    setEnv("FOXSDR_SHOT_DIR", o.shotAt > 0 ? shots.string() : std::string());
    setEnv("FOXSDR_SHOT_AT_FRAME", o.shotAt > 0 ? std::to_string(o.shotAt) : std::string());
    if (o.pickEmpty) {
        setEnvEmpty("FOXSDR_FILE_PICK");
    } else {
        setEnv("FOXSDR_FILE_PICK", o.pick);
    }
    std::printf("  scenario %s: launching %d frames\n", tag.c_str(), o.frames);
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(o.frames) + " 2>&1");
    r.bookmarks = readFile(bookmarksFile());
    const bool rendered = r.out.find("rendered " + std::to_string(o.frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        std::fflush(stdout);
        return r;
    }
    // The diagnostic log: wherever the run put foxsdr.log under its diagnostics folder.
    {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(diag, ec)) {
            if (e.is_regular_file(ec) && e.path().filename() == "foxsdr.log") { r.log = readFile(e.path()); }
        }
    }
    if (o.shotAt > 0) {
        r.windows = readFile(shots / ("shot-" + std::to_string(o.shotAt) + ".windows.txt"));
        r.windowUp = r.windows.find(kWindowName) != std::string::npos;
        if (r.windows.empty()) {
            std::printf("  %s: no window list was written for frame %d\n", tag.c_str(), o.shotAt);
            std::fflush(stdout);
            return r;
        }
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) { tok.push_back(t); }
        if (tok.size() < 5) { continue; }
        // The name may hold spaces; the four numbers are the last four.
        const std::size_t n = tok.size();
        Rect rc;
        rc.x0 = std::strtof(tok[n - 4].c_str(), nullptr);
        rc.y0 = std::strtof(tok[n - 3].c_str(), nullptr);
        rc.x1 = std::strtof(tok[n - 2].c_str(), nullptr);
        rc.y1 = std::strtof(tok[n - 1].c_str(), nullptr);
        std::string name = tok[0];
        for (std::size_t i = 1; i + 4 < n; ++i) { name += " " + tok[i]; }
        r.rects[name] = rc;
    }
    r.ok = true;
    return r;
}

// A click at the centre of `at`: the pointer arrives at `frame`, goes down two frames later and up
// two after that - the release at frame + 4 is what presses a key.
std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2,
                  frame + 4);
    return buf;
}

bool insideWindow(const Rect& r) {
    return r.x0 >= 0.0f && r.y0 >= 0.0f && r.x1 <= static_cast<float>(kWidth) && r.y1 <= static_cast<float>(kHeight);
}

// The three parts of the window, all noted, all inside the application window, the zone at least
// as large as the brief says.
void checkWindowParts(const Result& r, const char* what) {
    const Rect* zone = r.rect("import:dropzone");
    const Rect* browse = r.rect("import:browse");
    const Rect* cancel = r.rect("import:cancel");
    CHECK(zone != nullptr);
    CHECK(browse != nullptr);
    CHECK(cancel != nullptr);
    if (zone == nullptr || browse == nullptr || cancel == nullptr) { return; }
    std::printf("    %s: drop zone %.0fx%.0f at %.0f,%.0f; Browse at %.0f,%.0f; Cancel at %.0f,%.0f\n", what, zone->w(),
                zone->h(), zone->x0, zone->y0, browse->x0, browse->y0, cancel->x0, cancel->y0);
    std::fflush(stdout);
    CHECK(insideWindow(*zone) && insideWindow(*browse) && insideWindow(*cancel));
    CHECK(zone->w() >= 380.0f && zone->h() >= 110.0f);
    // Browse and Cancel are keys with a body to press, one below the other, under the zone, centred on it.
    CHECK(browse->w() > 20.0f && browse->h() > 8.0f && cancel->w() > 20.0f && cancel->h() > 8.0f);
    CHECK(browse->y0 >= zone->y1 && cancel->y0 >= browse->y1);
    CHECK(std::abs(browse->cx() - zone->cx()) < 3.0f && std::abs(cancel->cx() - zone->cx()) < 3.0f);
}

void checkNoWindowParts(const Result& r, const char* what) {
    std::printf("    %s: %d import window parts noted\n", what, r.rectsWithPrefix("import:"));
    std::fflush(stdout);
    CHECK(r.rectsWithPrefix("import:") == 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_import_dialog_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-importdialog-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "home", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", std::to_string(kWidth) + "x" + std::to_string(kHeight));
    // A fake package identity puts the plugins folder under LOCALAPPDATA (the scratch tree), not
    // beside the executable in the build tree.
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    for (const char* name : {"FOXSDR_OPEN_DECODERS", "FOXSDR_PATCH_START", "FOXSDR_PATCH_FILE", "FOXSDR_OPEN_PLUGIN_STORE",
                             "FOXSDR_OPEN_FITTED_MODULES", "FOXSDR_OPEN_DECODER_OUTPUT", "FOXSDR_PRESS_PRESET",
                             "FOXSDR_WINDOW_POS", "CASCADE_DECODE_TEST", "CASCADE_PLUGIN_TEST"}) {
        setEnv(name, "");
    }

    // The file a Browse "chooses", and the file Export would have left in the recordings folder.
    const fs::path picked = g_dir / "picked.csv";
    writeFile(picked, threeRows("Picked"));
    const std::string kMissing = "no-such-frequencies.csv";  // in neither the program's folder nor the recordings folder
    const std::string kInRecordings = "exported-list.csv";
    writeFile(recordingsDir() / kInRecordings, threeRows("Exported"));

    // --- LAYOUT: the section's field and Import key are noted; no press, no window -------------------
    Rect importKey, importField;
    {
        RunOptions o;
        o.frames = 40;
        o.importName = kMissing;
        o.pickEmpty = true;  // the seam present, so Browse is drawn on any runner; nothing is pressed
        o.shotAt = 30;
        const Result r = once("layout", o);
        CHECK(r.ok);
        if (r.ok) {
            const Rect* k = r.rect("bookmarks:import");
            const Rect* f = r.rect("bookmarks:importpath");
            CHECK(k != nullptr && f != nullptr);
            if (k != nullptr && f != nullptr) {
                importKey = *k;
                importField = *f;
                std::printf("    Import key at %.0f,%.0f - %.0f,%.0f; its box at %.0f,%.0f - %.0f,%.0f (window %dx%d)\n",
                            k->x0, k->y0, k->x1, k->y1, f->x0, f->y0, f->x1, f->y1, kWidth, kHeight);
                std::fflush(stdout);
                CHECK(insideWindow(*k) && insideWindow(*f));
                CHECK(k->w() > 20.0f && k->h() > 8.0f);
            }
            checkNoWindowParts(r, "layout (nothing pressed)");
            CHECK(!r.windowUp);
            CHECK(r.rows() == 0);
        }
    }
    if (importKey.x1 <= importKey.x0) {
        std::printf("  no Import key was found; the scenarios that press it cannot run\n");
        return testSummary("test_import_dialog_app");
    }

    // --- FAILURE: Import pressed with a name that is not there opens the window ----------------------
    Rect browse, cancel;
    const std::string pressImport = click(20, importKey);  // pointer 20, down 22, up 24: the press
    {
        RunOptions o;
        o.frames = 70;
        o.script = pressImport;
        o.importName = kMissing;
        o.pickEmpty = true;
        o.shotAt = 60;
        const Result r = once("failure", o);
        CHECK(r.ok);
        if (r.ok) {
            checkWindowParts(r, "failure");
            CHECK(r.windowUp);  // and it is still up at frame 60
            CHECK(r.rows() == 0);
            // The import WAS attempted: the log says it failed (the file's kind, never its name).
            CHECK(r.log.find("bookmarks: an import (.csv) failed: cannot open \"(the file)\"") != std::string::npos);
            CHECK(r.log.find(kMissing) == std::string::npos);  // PRIVACY.md: a log never names a file
            if (const Rect* b = r.rect("import:browse")) { browse = *b; }
            if (const Rect* c = r.rect("import:cancel")) { cancel = *c; }
        }
        // CONTROL: stopped before the key is released (down at 22, up at 24): nothing opens.
        RunOptions before = o;
        before.frames = 23;
        before.shotAt = 21;
        const Result b = once("failure-control", before);
        CHECK(b.ok);
        if (b.ok) {
            checkNoWindowParts(b, "failure control");
            CHECK(!b.windowUp);
        }
    }

    // --- EMPTY: the box cleared, Import pressed: the window opens and no import was attempted -------
    {
        // The box is clicked into, selected whole and emptied; then the key is pressed.
        const std::string clear = click(10, importField) + "16 key ctrl+a\n18 key backspace\n";
        RunOptions o;
        o.frames = 70;
        o.script = clear + click(24, importKey);
        o.importName = kMissing;
        o.pickEmpty = true;
        o.shotAt = 60;
        const Result r = once("empty", o);
        CHECK(r.ok);
        if (r.ok) {
            checkWindowParts(r, "empty");
            CHECK(r.windowUp);
            // No import ran: the box was empty, so nothing could fail. Without the clearing, the same
            // press is the FAILURE scenario's (an import that fails - and the log says so).
            CHECK(!r.log.empty());
            CHECK(r.log.find("an import") == std::string::npos);
        }
        // CONTROL: the same run stopped before the final press is released: the box is empty and nothing opened.
        RunOptions before = o;
        before.frames = 27;
        before.shotAt = 25;
        const Result b = once("empty-control", before);
        CHECK(b.ok);
        if (b.ok) {
            checkNoWindowParts(b, "empty control");
            CHECK(!b.windowUp);
        }
    }

    if (browse.x1 <= browse.x0 || cancel.x1 <= cancel.x0) {
        std::printf("  no Browse or Cancel key was found; the scenarios that press them cannot run\n");
        return testSummary("test_import_dialog_app");
    }

    // --- CHOSEN: Browse answered with a file: its rows are imported at once and the window closes ----
    {
        RunOptions o;
        o.frames = 90;
        o.script = pressImport + click(40, browse);
        o.importName = kMissing;
        o.pick = picked.string();
        o.shotAt = 80;
        const Result r = once("chosen", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    chosen: %d rows saved; window %s\n", r.rows(), r.windowUp ? "still up" : "closed");
            std::fflush(stdout);
            CHECK(r.rows() == 3);
            CHECK(r.bookmarks.find("Picked 0") != std::string::npos);
            CHECK(!r.windowUp);  // the import came back with rows: the window closed itself
        }
        // CONTROL: stopped before Browse is released (down at 42, up at 44): the failure's window, no rows.
        RunOptions before = o;
        before.frames = 43;
        before.shotAt = 41;
        const Result b = once("chosen-control", before);
        CHECK(b.ok);
        if (b.ok) {
            CHECK(b.rows() == 0);
            CHECK(b.windowUp);
        }
    }

    // --- CANCEL: Browse answered with a cancelled dialog: nothing is read and the window stays ------
    {
        RunOptions o;
        o.frames = 90;
        o.script = pressImport + click(40, browse);
        o.importName = kMissing;
        o.pickEmpty = true;
        o.shotAt = 80;
        const Result r = once("cancel", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    cancel: %d rows saved; window %s\n", r.rows(), r.windowUp ? "still up" : "closed");
            std::fflush(stdout);
            CHECK(r.rows() == 0);
            checkWindowParts(r, "cancel");
            CHECK(r.windowUp);
        }
    }

    // --- ESCAPE and CANCEL close it ------------------------------------------------------------------
    {
        RunOptions o;
        o.frames = 90;
        o.script = pressImport + "40 key escape\n";
        o.importName = kMissing;
        o.pickEmpty = true;
        o.shotAt = 80;
        const Result r = once("escape", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    escape: window %s\n", r.windowUp ? "still up" : "closed");
            std::fflush(stdout);
            CHECK(!r.windowUp);
            CHECK(r.rect("import:dropzone") != nullptr);  // it had been up
        }

        RunOptions closeKey = o;
        closeKey.script = pressImport + click(40, cancel);
        const Result c = once("close", closeKey);
        CHECK(c.ok);
        if (c.ok) {
            std::printf("    close (Cancel key): window %s\n", c.windowUp ? "still up" : "closed");
            std::fflush(stdout);
            CHECK(!c.windowUp);
            CHECK(c.rect("import:dropzone") != nullptr);
        }
    }

    // --- BARE: a name that is in the recordings folder is read, and no window opens -----------------
    {
        RunOptions o;
        o.frames = 70;
        o.script = pressImport;
        o.importName = kInRecordings;
        o.pickEmpty = true;
        o.shotAt = 60;
        const Result r = once("bare", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    bare: %d rows saved; window parts noted: %d\n", r.rows(), r.rectsWithPrefix("import:"));
            std::fflush(stdout);
            CHECK(r.rows() == 3);
            CHECK(r.bookmarks.find("Exported 0") != std::string::npos);
            checkNoWindowParts(r, "bare");
            CHECK(!r.windowUp);
        }
        // CONTROL: stopped before the key is released: nothing was read.
        RunOptions before = o;
        before.frames = 23;
        before.shotAt = 21;
        const Result b = once("bare-control", before);
        CHECK(b.ok);
        if (b.ok) { CHECK(b.rows() == 0); }
    }

    // --- SHAPES: the paths people paste, each a file that exists ------------------------------------------------
    {
        const std::string recFile = (recordingsDir() / kInRecordings).generic_string();
        writeFile(recordingsDir() / "lists" / "nested.csv", threeRows("Nested"));
        struct Shape {
            const char* tag;
            std::string typed;
            const char* firstRow;
        };
        const Shape shapes[] = {
            {"shape-quoted", "'" + recFile + "' ", "Exported 0"},
            // A full path with a root and the Enter that ended the pasted line: it goes to the engine as the window cleaned it, never as typed
            // (a path with a root is not looked for, so nothing else would clean it; and not spaces - Windows drops those from a path itself,
            // so a mutant that sent them on would pass).
            {"shape-enter", recFile + "\r\n", "Exported 0"},
            {"shape-uri", std::string("file://") + (recFile[0] == '/' ? "" : "/") + recFile, "Exported 0"},
            {"shape-folder", "lists/nested.csv", "Nested 0"},
            {"shape-dquoted-bare", "\"" + kInRecordings + "\"", "Exported 0"},
        };
        for (const Shape& sh : shapes) {
            RunOptions o;
            o.frames = 70;
            o.script = pressImport;
            o.importName = sh.typed;
            o.pickEmpty = true;
            o.shotAt = 60;
            const Result r = once(sh.tag, o);
            CHECK(r.ok);
            if (r.ok) {
                std::printf("    %s: %d rows saved; window parts noted: %d\n", sh.tag, r.rows(), r.rectsWithPrefix("import:"));
                std::fflush(stdout);
                CHECK(r.rows() == 3);
                CHECK(r.bookmarks.find(sh.firstRow) != std::string::npos);
                checkNoWindowParts(r, sh.tag);
                CHECK(!r.windowUp);
            }
        }
    }

    // --- AIRBAND: the preset's Import with its box empty opens the window FOR THE PRESET -------------
    // The Import key of the AIRBAND section (TOOLS bank, FOXSDR_OPEN_SECTIONS=airband) is no longer
    // disabled by an empty box. Browse's file joins the section's preset (here the default, "Manual"):
    // every row in that group and ticked, whatever the file said.
    Rect airImport, airBrowse;
    {
        RunOptions o;
        o.frames = 40;
        o.railBank = 2;
        o.openSections = "airband";
        o.pickEmpty = true;
        o.shotAt = 30;
        const Result r = once("airband-layout", o);
        CHECK(r.ok);
        if (r.ok) {
            const Rect* k = r.rect("airband:import");
            CHECK(k != nullptr && r.rect("airband:importpath") != nullptr);
            if (k != nullptr) { airImport = *k; }
            checkNoWindowParts(r, "airband layout (nothing pressed)");
            CHECK(!r.windowUp);
        }
    }
    if (airImport.x1 > airImport.x0) {
        RunOptions o;
        o.frames = 70;
        o.railBank = 2;
        o.openSections = "airband";
        o.pickEmpty = true;
        o.script = click(20, airImport);
        o.shotAt = 60;
        const Result r = once("airband-empty", o);
        CHECK(r.ok);
        if (r.ok) {
            checkWindowParts(r, "airband empty");
            CHECK(r.windowUp);
            CHECK(r.log.find("an import") == std::string::npos);  // nothing was imported: the box was empty
            if (const Rect* b = r.rect("import:browse")) { airBrowse = *b; }
        }
    }
    if (airBrowse.x1 > airBrowse.x0) {
        RunOptions o;
        o.frames = 90;
        o.railBank = 2;
        o.openSections = "airband";
        o.pick = picked.string();
        o.script = click(20, airImport) + click(40, airBrowse);
        o.shotAt = 80;
        const Result r = once("airband-chosen", o);
        CHECK(r.ok);
        if (r.ok) {
            std::printf("    airband chosen: %d rows saved, %d in Manual, %d ticked; window %s\n", r.rows(),
                        r.count("\"group\": \"Manual\""), r.count("\"scan\": true"), r.windowUp ? "still up" : "closed");
            std::fflush(stdout);
            CHECK(r.rows() == 3);
            CHECK(r.count("\"group\": \"Manual\"") == 3);  // the preset's group, which the file did not name
            CHECK(r.count("\"scan\": true") == 3);         // ticked: they are NFM rows the monitor plays
            CHECK(!r.windowUp);
        }
    }

    std::error_code rm;
    const int rc = testSummary("test_import_dialog_app");
    if (rc == 0) { fs::remove_all(g_dir, rm); }
    return rc;
}
