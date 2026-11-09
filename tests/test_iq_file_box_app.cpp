/*
 * THE I/Q-FILE ROW'S BOX, BROWSE KEY AND OPEN, IN THE REAL APPLICATION (2026-10-29).
 *
 * A tester, FoxSDR 0.99.101 on Windows 10: "Can the IQ-file source be fixed: 1. the Open does
 * nothing 2. Not possible to paste in a WAV-file in the IQ-file field." The row was a text box
 * and an Open key that only tried the box's text (over an empty box: a red "cannot open file: "
 * further down, and no log line - his log had no I/Q line at all), with no way to choose a file;
 * and a path pasted from Explorer's "Copy as path" came in quotes the open refused.
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1), on the SIGNAL PATH bank with the Source section open, with a real
 * recording (A, written by the application's own recorder) as the saved I/Q file so the row's
 * controls are drawn from the first frame. A native dialog cannot be pressed from a test, so the
 * dialog's ANSWER is the FOXSDR_FILE_PICK seam (bounded --frames runs only), as every test of a
 * Browse key has it; a scripted paste puts its text on the SCRIPTED clipboard (gui/paste_files.hpp),
 * never the desk's, and taps Ctrl+V. Everything else - the box, the keys, the press landing, the
 * paste running through Dear ImGui's own shortcut and buffer checks, the worker's open, the config
 * the run leaves - is the real code.
 *
 * WHAT IS PROVED, scenario by scenario (each one a real process), read from the UI census, the
 * diagnostic log the run leaves (FOXSDR_DIAG_DIR) and the config it saves at exit:
 *
 *   layout    the box, the Browse key and the Open key are drawn (source:iqpath, source:iqbrowse,
 *             source:iqopen), inside the window, box then keys on one row; the saved config still
 *             names A; the log says A was restored.
 *   browse    a scripted press on Browse with the dialog answering B: the config names B, the log
 *             says a recording was chosen and the file opened at 250000 S/s.
 *   empty     the box clicked, emptied (Ctrl+A, Backspace) and Open pressed, the dialog answering
 *             B: the config names B - Open over an empty box IS the dialog.
 *   cancel    the same with the dialog cancelled: the config still names A, nothing else opened,
 *             the log says the dialog was closed without a choice.
 *   paste     the box clicked, emptied, "B" PASTED in double quotes (what "Copy as path" gives), and
 *             Open pressed, with a dialog that would cancel if asked: the config names B and no
 *             dialog was asked - the paste landed, the quotes came off, the open ran.
 *   control   the paste scenario stopped before the Open press: the config still names A, so it is
 *             the press, not the paste or the start-up, that opened B.
 *
 * What is NOT covered here: the system clipboard itself (a copied file read as its path is a few
 * Win32 calls in gui/paste_files.cpp, around the pure parser test_iq_file_box.cpp pins), and the
 * native dialog (tests/test_file_pick.cpp drives the real one).
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <complex>
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

#include "core/config.hpp"
#include "core/recorder.hpp"
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

// Set and EMPTY, which setEnv cannot spell: the seam reads it as "the dialog was cancelled".
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
    std::string out;         // what the process printed
    std::string log;         // the diagnostic log the run left
    std::string savedKind;   // the config as the run left it: sourceKind
    std::string savedPath;   // ...and iqFilePath
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    bool logged(const std::string& s) const { return log.find(s) != std::string::npos; }
};

fs::path g_dir;
std::string g_recA, g_recB;  // forward slashes: they go into JSON and a script as they are

const int kWidth = 1600;
const int kHeight = 1000;

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

// A real I/Q recording: 4000 frames at 250 kS/s, by the application's own recorder.
std::string makeRecording(const char* tag) {
    cascade::core::Recorder rec;
    std::string err;
    const std::string dir = (g_dir / tag).string();
    CHECK(rec.start(cascade::core::RecordKind::BasebandIq, dir, 250000.0, err));
    std::vector<std::complex<float>> s(4000);
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = {0.25f * static_cast<float>(i % 7), -0.125f * static_cast<float>(i % 5)};
    }
    rec.writeIq(s.data(), s.size());
    rec.stop();
    std::string p = rec.path();
    for (char& c : p) {
        if (c == '\\') { c = '/'; }
    }
    return p;
}

struct RunOptions {
    int frames = 60;
    std::string script;      // FOXSDR_INPUT_SCRIPT, empty = none
    std::string pick;        // FOXSDR_FILE_PICK: the path "chosen"
    bool pickEmpty = false;  // FOXSDR_FILE_PICK present and empty: the dialog cancelled
};

Result once(const std::string& tag, const RunOptions& o) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path diag = g_dir / (tag + "-diag");
    {
        std::error_code ec;
        fs::remove_all(diag, ec);
        fs::create_directories(diag, ec);
    }
    // The saved source is recording A, so the row's controls are drawn from the first frame.
    writeFile(cfg,
              "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"file\", "
              "\"iqFilePath\": \"" + g_recA + "\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
              "\"language\": \"en\", \"mainView\": \"receiver\", \"railBank\": 0, \"bandPlanOverlay\": false }\n");
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, o.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", o.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    if (o.pickEmpty) {
        setEnvEmpty("FOXSDR_FILE_PICK");
    } else {
        setEnv("FOXSDR_FILE_PICK", o.pick);
    }
    std::printf("  scenario %s: launching %d frames\n", tag.c_str(), o.frames);
    std::fflush(stdout);
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(o.frames) + " 2>&1");
    const bool rendered = r.out.find("rendered " + std::to_string(o.frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    const bool scripted = o.script.empty() || r.out.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        std::fflush(stdout);
        return r;
    }
    {
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
    }
    {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(diag, ec)) {
            if (e.is_regular_file(ec) && e.path().filename() == "foxsdr.log") { r.log = readFile(e.path()); }
        }
    }
    cascade::core::AppConfig saved;
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
        return r;
    }
    r.savedKind = saved.sourceKind;
    r.savedPath = saved.iqFilePath;
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

}  // namespace

int main() {
    std::printf("test_iq_file_box_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-iq-file-box-" + std::to_string(pid()));
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
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");  // stay on the SIGNAL PATH bank the config chose
    setEnv("FOXSDR_OPEN_SECTIONS", "Source");
    for (const char* v : {"FOXSDR_PATCH_START", "FOXSDR_PATCH_FILE", "FOXSDR_OPEN_PLUGIN_STORE",
                          "FOXSDR_OPEN_FITTED_MODULES", "FOXSDR_OPEN_DECODER_OUTPUT", "FOXSDR_OPEN_DECODERS",
                          "FOXSDR_PRESS_PRESET", "FOXSDR_SHOT_DIR", "FOXSDR_SHOT_AT_FRAME", "FOXSDR_WINDOW_POS",
                          "FOXSDR_BOOKMARK_IMPORT", "CASCADE_DECODE_TEST", "CASCADE_PLUGIN_TEST"}) {
        setEnv(v, "");
    }
    g_recA = makeRecording("rec-a");
    g_recB = makeRecording("rec-b");
    CHECK(!g_recA.empty() && !g_recB.empty() && g_recA != g_recB);

    // --- LAYOUT: the box and both keys, on one row, nothing pressed ---------------------------------
    Rect box, browse, open;
    {
        RunOptions o;
        o.pickEmpty = true;  // the seam present, so Browse is drawn on any runner; nothing is pressed
        const Result r = once("layout", o);
        CHECK(r.ok);
        if (r.ok) {
            const Rect* b = r.rect("source:iqpath");
            const Rect* w = r.rect("source:iqbrowse");
            const Rect* k = r.rect("source:iqopen");
            CHECK(b != nullptr && w != nullptr && k != nullptr);
            if (b != nullptr && w != nullptr && k != nullptr) {
                box = *b;
                browse = *w;
                open = *k;
                std::printf("    box %.0f,%.0f-%.0f,%.0f  Browse %.0f,%.0f-%.0f,%.0f  Open %.0f,%.0f-%.0f,%.0f\n",
                            b->x0, b->y0, b->x1, b->y1, w->x0, w->y0, w->x1, w->y1, k->x0, k->y0, k->x1, k->y1);
                CHECK(insideWindow(*b) && insideWindow(*w) && insideWindow(*k));
                CHECK(b->w() > 60.0f && w->w() > 20.0f && k->w() > 20.0f && k->h() > 8.0f);
                // Box, then Browse, then Open, left to right on one row.
                CHECK(b->x1 <= w->x0 && w->x1 <= k->x0);
                CHECK(std::abs(b->cy() - k->cy()) < k->h());
            }
            CHECK(r.savedKind == "file");
            CHECK(r.savedPath == g_recA);
            CHECK(r.logged("source: restored an I/Q file at 250000 S/s"));
            CHECK(!r.logged("source: opened an I/Q file"));  // nothing was pressed
        }
    }
    if (box.w() <= 0.0f || browse.w() <= 0.0f || open.w() <= 0.0f) {
        std::printf("  the row's parts were not all found; the scenarios that press them cannot run\n");
        return testSummary("test_iq_file_box_app");
    }

    // --- BROWSE: the key, the dialog answering B ---------------------------------------------------
    {
        RunOptions o;
        o.pick = g_recB;
        o.script = click(20, browse);
        const Result r = once("browse", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.savedKind == "file");
            CHECK(r.savedPath == g_recB);
            CHECK(r.logged("source: a recording was chosen in the I/Q file dialog"));
            CHECK(r.logged("source: opened an I/Q file at 250000 S/s"));
            CHECK(!r.logged("rec-b"));  // never the file's name
        }
    }

    // The box clicked at 20, emptied with Ctrl+A at 26 and Backspace at 28.
    const std::string emptyBox = click(20, box) + "26 key ctrl+a\n28 key backspace\n";

    // --- EMPTY: Open over an empty box is the dialog -----------------------------------------------
    {
        RunOptions o;
        o.pick = g_recB;
        o.script = emptyBox + click(32, open);
        const Result r = once("empty", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.savedKind == "file");
            CHECK(r.savedPath == g_recB);
            CHECK(r.logged("source: a recording was chosen in the I/Q file dialog"));
            CHECK(r.logged("source: opened an I/Q file at 250000 S/s"));
            CHECK(!r.logged("did not open"));
        }
    }

    // --- CANCEL: the same, the dialog dismissed: nothing changes ------------------------------------
    {
        RunOptions o;
        o.pickEmpty = true;
        o.script = emptyBox + click(32, open);
        const Result r = once("cancel", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.savedKind == "file");
            CHECK(r.savedPath == g_recA);
            CHECK(r.logged("source: the I/Q file dialog was closed without a choice"));
            CHECK(!r.logged("source: opened an I/Q file"));
            CHECK(!r.logged("did not open"));  // and "" was never handed to the worker
        }
    }

    // --- PASTE: a quoted path pasted, then Open: no dialog asked ------------------------------------
    const std::string pasteB = emptyBox + "30 paste \"" + g_recB + "\"\n";
    {
        RunOptions o;
        o.pickEmpty = true;  // a dialog that would cancel if asked
        o.script = pasteB + click(36, open);
        const Result r = once("paste", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.savedKind == "file");
            CHECK(r.savedPath == g_recB);
            CHECK(r.logged("source: opened an I/Q file at 250000 S/s"));
            CHECK(!r.logged("dialog"));
            CHECK(!r.logged("did not open"));
        }
    }

    // --- CONTROL: the same paste, stopped before the Open press: A stays ------------------------------
    {
        RunOptions o;
        o.pickEmpty = true;
        o.script = pasteB;
        o.frames = 50;
        const Result r = once("control", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.savedKind == "file");
            CHECK(r.savedPath == g_recA);
            CHECK(!r.logged("source: opened an I/Q file"));
        }
    }

    fs::remove_all(g_dir, ec);
    return testSummary("test_iq_file_box_app");
}
