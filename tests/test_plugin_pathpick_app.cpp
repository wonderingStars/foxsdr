/*
 * THE BROWSE KEY OF A PATH SETTING, END TO END (0.99.74, CASCADE_SETTING_PATH).
 *
 * The owner, 2026-10-12: "have you made it easy for the user to add the nrsc5 path to the sdr
 * software". A plugin that drives a program the user installs themselves needs that program's
 * location, and the host drew every setting as a text box - the user had to type or paste a full
 * path. A setting of kind PATH now gets a BROWSE key beside its box, and the key opens the
 * system's file dialog.
 *
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1), with the REAL settings fixture module (tests/fixtures/
 * settings_ui_plugin.cpp: a text decoder whose form has a PATH field "tool_path") fitted in a
 * scratch plugins folder, and the DECODE bank open with the "Turn on and off plugins" section
 * forced open (FOXSDR_OPEN_DECODERS), where the plugin's form is drawn under its keys. A native
 * dialog cannot be pressed from a test, so the dialog's ANSWER is the test seam FOXSDR_FILE_PICK
 * (bounded --frames runs only): a path is the file "chosen", an empty value is the dialog
 * cancelled. Everything else - the key being drawn, the scripted press landing on it, the path
 * being committed into the plugin's settings and saved in the config file - is the real code.
 *
 * WHAT IS PROVED, scenario by scenario (each one a real process):
 *
 *   layout   the plugin's form draws exactly ONE BROWSE key, noted in the UI census as
 *            settings:browse:tool_path with a rectangle that is inside the window - and not one
 *            for the plugin's TEXT, NUMBER or BOOL fields;
 *   chosen   a scripted press on that rectangle with FOXSDR_FILE_PICK naming a path stores the
 *            path in the plugin's settings (the config file the run leaves holds it under the
 *            plugin's name), spaces and all;
 *   control  the same run stopped BEFORE the release of the press stores nothing - so it is the
 *            press, not the start-up, that stored it;
 *   cancel   the same press with FOXSDR_FILE_PICK set EMPTY (the dialog cancelled) stores
 *            nothing;
 *   again    a second launch from the config the "chosen" run left shows the path and the key
 *            still drawn (the value survives a restart, and the box is not what stored it);
 *   typed    (stub campaign B9, which this scenario is MINE for: the form's edit is a command now, answered by the engine a frame or more later)
 *            a text box is committed when it is LEFT, once and whole, and never per keystroke: "G4" then "A" typed into the callsign box
 *            and Enter pressed makes the plugin's settings ONE saved edit (the engine's log line, which names the field and the length and
 *            never the text, is there once, and says 3 bytes) and the config holds "G4A"; the same run stopped BEFORE Enter - typed, not left -
 *            saved nothing; a number that is not one is refused and saves nothing, and the engine's verdict reaches the box (the census
 *            says settings:message:power:2 - NotANumber - which only the ANSWER of the command can have caused); and a box typed in whose
 *            surface stops drawing before it is left (the "Plugin settings" line folded under it) is committed all the same, once.
 *
 * Where the host has a file dialog the key is drawn WITHOUT the seam too (Windows always); the
 * seam is set in every run here so the Linux runners, which may have neither zenity nor kdialog,
 * draw the key as well. The dialog itself is tests/test_file_pick.cpp's.
 *
 * THE DECODE BANK FOLDS A PLUGIN'S FORM into a closed "Plugin settings" node (0.99.74, tester's
 * request: "could those settings be put in a collapsed settings sub menu?"), so the form - and the
 * key in it - is not drawn until that node is opened. A first run (folded) reads the node's
 * rectangle from the census (decoders:settingsnode) and checks the key is NOT drawn while it is
 * closed; every later run begins with a scripted click on that rectangle (g_unfoldScript), and
 * the press on the key follows it.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
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
// Win32 only touches the process environment block the child inherits - _putenv_s("") would
// delete it. (cmd.exe passes an empty variable on to its child: measured 2026-10-12.)
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

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;     // what the process printed
    std::string config;  // the settings file as the run left it
    std::string log;     // the diagnostic log (a run with diagnostics on)
    bool has(const std::string& item) const { return items.count(item) != 0; }
    // How many lines of the log say `what`.
    int logLines(const std::string& what) const {
        int n = 0;
        for (std::size_t at = log.find(what); at != std::string::npos; at = log.find(what, at + what.size())) { ++n; }
        return n;
    }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    int itemsWithPrefix(const std::string& prefix) const {
        int n = 0;
        for (const std::string& s : items) { n += s.rfind(prefix, 0) == 0 ? 1 : 0; }
        return n;
    }
};

fs::path g_dir;
std::string g_fixtureDll;  // the settings fixture module

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

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

// The settings fixture is the only module fitted.
void installPlugin() {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    fs::copy_file(g_fixtureDll, pluginsDir() / (std::string("settings_ui_fixture") + kExt),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

struct RunOptions {
    int frames = 40;
    std::string script;       // FOXSDR_INPUT_SCRIPT, empty = none
    std::string pick;         // FOXSDR_FILE_PICK: the path "chosen"
    bool pickEmpty = false;   // FOXSDR_FILE_PICK present and empty: the dialog cancelled
    bool keepConfig = false;  // keep the config the previous run with this tag wrote
    bool diagnostics = false; // the run keeps its diagnostic log (read back into Result::log)
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
    if (!o.keepConfig) {
        writeFile(cfg,
                  "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": "
                  "\"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
                  "\"receiver\", \"railBank\": 1, \"bandPlanOverlay\": false" +
                      std::string(o.diagnostics ? ", \"diagnosticsEnabled\": true" : "") + " }\n");
    }
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
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(o.frames) + " 2>&1");
    r.config = readFile(cfg);
    r.log = readFile(diag / "logs" / "foxsdr.log");
    const bool rendered = r.out.find("rendered " + std::to_string(o.frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
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

// Ahead of any press, the scripted click that opens the plugin's "Plugin settings" fold on the
// DECODE bank (see the header). The node's rectangle is MEASURED by the first run below (it moves
// with the window and the type size), so this is filled in once that run has read it.
std::string g_unfoldScript;

const std::string kStoredPath = "C:/FoxSDR test/nrsc5/nrsc5.exe";  // a space: a path needs one

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_plugin_pathpick_app\n");
    if (argc < 2) {
        std::printf("usage: test_plugin_pathpick_app <settings fixture module>\n");
        return 2;
    }
    g_fixtureDll = argv[1];
    g_dir = fs::temp_directory_path() / ("cascade-pathpick-" + std::to_string(pid()));
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
    setEnv("FOXSDR_OPEN_DECODERS", "1");  // the "Turn on and off plugins" section, forced open
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    // --- FOLDED: the form is closed, so no key is drawn; the node is where the unfold click goes ----
    Rect node;
    {
        installPlugin();
        RunOptions o;
        o.frames = 40;
        o.pickEmpty = true;  // the seam present (so a key would be drawn on any runner); nothing is pressed
        const Result r = once("folded", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("decoders:settings:folded"));
            CHECK(!r.has("settings:browse:tool_path"));  // folded: the form (and its key) is not drawn
            const Rect* n = r.rect("decoders:settingsnode");
            CHECK(n != nullptr);
            if (n != nullptr) {
                node = *n;
                std::printf("    folded: node at %.0f,%.0f - %.0f,%.0f\n", n->x0, n->y0, n->x1, n->y1);
                CHECK(n->x0 >= 0.0f && n->y0 >= 0.0f && n->x1 <= static_cast<float>(kWidth) &&
                      n->y1 <= static_cast<float>(kHeight));
            }
        }
    }
    if (node.x1 <= node.x0) {
        std::printf("  no \"Plugin settings\" node was found; the scenarios that open it cannot run\n");
        return testSummary("test_plugin_pathpick_app");
    }
    // The click that opens it: the pointer arrives at frame 15, the release at 19 is the press.
    g_unfoldScript = click(15, node);

    // --- LAYOUT: one key, for the PATH field and no other -------------------------------------------
    Rect browse;
    Rect callsignBox;
    Rect powerBox;
    {
        installPlugin();
        RunOptions o;
        o.frames = 40;
        o.script = g_unfoldScript;
        o.pickEmpty = true;  // the seam present (so the key is drawn on any runner); nothing is pressed after the unfold
        const Result r = once("layout", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.has("settings:browse:tool_path"));
            CHECK(r.itemsWithPrefix("settings:browse:") == 1);  // TEXT, NUMBER and BOOL have none
            CHECK(r.rect("settings:browse:callsign") == nullptr);
            CHECK(r.rect("settings:browse:power") == nullptr);
            CHECK(r.rect("settings:browse:report") == nullptr);
            const Rect* cb = r.rect("settings:box:callsign");
            const Rect* pb = r.rect("settings:box:power");
            CHECK(cb != nullptr && pb != nullptr);
            if (cb != nullptr) { callsignBox = *cb; }
            if (pb != nullptr) { powerBox = *pb; }
            const Rect* p = r.rect("settings:browse:tool_path");
            CHECK(p != nullptr);
            if (p != nullptr) {
                browse = *p;
                std::printf("    BROWSE key at %.0f,%.0f - %.0f,%.0f in a %dx%d window\n", p->x0, p->y0, p->x1, p->y1,
                            kWidth, kHeight);
                // A key the person can reach: wholly inside the window, with a body to press.
                CHECK(p->x0 >= 0.0f && p->y0 >= 0.0f && p->x1 <= static_cast<float>(kWidth) &&
                      p->y1 <= static_cast<float>(kHeight));
                CHECK(p->x1 - p->x0 > 20.0f && p->y1 - p->y0 > 8.0f);
            }
            // Nothing stored by a run that pressed nothing.
            CHECK(r.config.find("\"tool_path\"") == std::string::npos);
        }
    }
    if (browse.x1 <= browse.x0) {
        std::printf("  no BROWSE key was found; the scenarios that press it cannot run\n");
        return testSummary("test_plugin_pathpick_app");
    }

    // --- CHOSEN: the press stores the path the dialog answered ---------------------------------------
    {
        installPlugin();
        RunOptions o;
        o.frames = 46;
        o.pick = kStoredPath;
        o.script = g_unfoldScript + click(30, browse);
        const Result r = once("chosen", o);
        CHECK(r.ok);
        if (r.ok) {
            const bool stored =
                r.config.find("\"tool_path\": \"" + kStoredPath + "\"") != std::string::npos;
            std::printf("    chosen: config %s the path\n", stored ? "holds" : "does NOT hold");
            CHECK(stored);
            // Under the plugin's name, which is where the plugin's own settings_get reads it.
            CHECK(r.config.find("\"Settings Form Fixture\"") != std::string::npos);
        }

        // CONTROL: stopped before the release of the press (down at 32, up at 34): nothing is stored.
        RunOptions before = o;
        before.frames = 33;
        const Result b = once("control", before);
        CHECK(b.ok);
        if (b.ok) { CHECK(b.config.find("\"tool_path\"") == std::string::npos); }

        // CANCEL: the same press, the dialog cancelled: nothing is stored.
        RunOptions cancel = o;
        cancel.pick.clear();
        cancel.pickEmpty = true;
        const Result c = once("cancel", cancel);
        CHECK(c.ok);
        if (c.ok) {
            std::printf("    cancelled: the config %s\n",
                        c.config.find("\"tool_path\"") == std::string::npos ? "holds no tool_path"
                                                                           : "HOLDS a tool_path");
            CHECK(c.config.find("\"tool_path\"") == std::string::npos);
        }

        // AGAIN: the next launch, from the config the "chosen" run left. The key is still drawn and
        // the value is still there - and it was not stored by this run (nothing is pressed in it).
        RunOptions again;
        again.frames = 40;
        again.script = g_unfoldScript;  // the fold is closed at every launch, so open it again
        again.keepConfig = true;
        again.pickEmpty = true;
        const Result n = once("chosen", again);
        CHECK(n.ok);
        if (n.ok) {
            CHECK(n.has("settings:browse:tool_path"));
            CHECK(n.config.find("\"tool_path\": \"" + kStoredPath + "\"") != std::string::npos);
        }
    }

    // --- TYPED: a text box is committed when it is LEFT, once and whole (stub campaign B9) --------------------------------------
    //
    // The pointer arrives at the callsign box at frame 30 and presses it (down 32, up 34); "G4" is typed at 40 and "A" at 44 - two separate
    // keystrokes, in two frames, with the box still being typed in - and Enter at 50 leaves it. A form that committed per keystroke would save
    // "G4" and then "G4A": two lines in the log, two moves of the plugin's settings_seq, and the plugin reading a callsign that was never one.
    if (callsignBox.x1 > callsignBox.x0 && powerBox.x1 > powerBox.x0) {
        const std::string saved = "plugin settings: Settings Form Fixture - callsign saved";
        installPlugin();
        RunOptions o;
        o.frames = 70;
        o.diagnostics = true;
        o.pickEmpty = true;
        o.script = g_unfoldScript + click(30, callsignBox) + "40 text G4\n44 text A\n50 key enter\n";
        const Result r = once("typed", o);
        CHECK(r.ok);
        if (r.ok) {
            const int lines = r.logLines(saved);
            std::printf("    typed: the log says the callsign was saved %d time(s); config %s \"G4A\"\n", lines,
                        r.config.find("\"callsign\": \"G4A\"") != std::string::npos ? "holds" : "does NOT hold");
            CHECK(lines == 1);                                                      // ONE edit, not one a keystroke
            CHECK(r.logLines(saved + " (3 bytes)") == 1);                           // the whole of it
            CHECK(r.logLines(saved + " (2 bytes)") == 0);                           // never "G4"
            CHECK(r.config.find("\"callsign\": \"G4A\"") != std::string::npos);    // and it is what the plugin's store holds
        }

        // CONTROL: the same run stopped before Enter - typed, not left - saves nothing at all.
        RunOptions before = o;
        before.frames = 47;
        const Result b = once("typed-control", before);
        CHECK(b.ok);
        if (b.ok) {
            CHECK(b.logLines("plugin settings:") == 0);
            CHECK(b.config.find("\"callsign\"") == std::string::npos);
        }

        // REFUSED: "12x" into the number is not one - answered, nothing saved, the config does not hold it.
        RunOptions bad = o;
        bad.script = g_unfoldScript + click(30, powerBox) + "40 text 12x\n50 key enter\n";
        const Result x = once("typed-refused", bad);
        CHECK(x.ok);
        if (x.ok) {
            CHECK(x.logLines("plugin settings:") == 0);
            CHECK(x.config.find("12x") == std::string::npos);
            CHECK(x.has("settings:message:power:2"));  // the verdict - NotANumber - came back and the box says so
        }
        // ...and an accepted edit says nothing of the kind.
        CHECK(!r.has("settings:message:callsign:0") && r.itemsWithPrefix("settings:message:") == 0);

        // FLUSHED: "G4A" typed, and then the "Plugin settings" line itself pressed (frame 50): the node sits ABOVE the box, so the same press folds it
        // and the box is never drawn again - ImGui's own deactivation cannot fire for it. The frame's flush commits it: one saved edit, 3 bytes.
        RunOptions flushed = o;
        flushed.script = g_unfoldScript + click(30, callsignBox) + "40 text G4A\n" + click(50, node);
        const Result f = once("typed-flushed", flushed);
        CHECK(f.ok);
        if (f.ok) {
            std::printf("    flushed: the log says the callsign was saved %d time(s) when its fold shut\n", f.logLines(saved));
            CHECK(f.logLines(saved) == 1);
            CHECK(f.logLines(saved + " (3 bytes)") == 1);
            CHECK(f.config.find("\"callsign\": \"G4A\"") != std::string::npos);
            CHECK(f.has("decoders:settings:folded"));
        }
    }

    std::error_code rm;
    fs::remove_all(g_dir, rm);
    return testSummary("test_plugin_pathpick_app");
}
