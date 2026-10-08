/*
 * THE DECODER OUTPUT WINDOW OPENS WHERE IT CAN BE SEEN (report df5aff7da93dc88c, 0.99.71).
 * Runs cascade itself, isolated like every test that starts the application, in ONE viewport
 * (FOXSDR_SINGLE_VIEWPORT=1, so every window is inside the main window's framebuffer and the UI
 * census reads each one's rectangle), and presses a decoder's preset the way the key on the rail
 * does, or opens the window by the capture seam, and checks where the window is:
 *
 *   preset     a preset pressed on a text decoder that has no window of its own opens the shared
 *              DECODER OUTPUT window (the log says so) and the window is WHOLLY INSIDE the main
 *              window and at least 600 px wide - not a 19 px strip of brass at the right edge;
 *   small      the same on a 1000 x 700 main window, and on one at the application's minimum
 *              (700 x 450), where the window is held to what fits and is still readable (>= 240);
 *   seam       FOXSDR_OPEN_DECODER_OUTPUT=1 opens it through the SAME placement (that seam used
 *              to force a position inside the main window, which hid the defect from every
 *              capture), with no preset and so no "preset: opened" line;
 *   picture    a plugin's picture window (the image probe, an image decoder the receiver runs)
 *              opens inside the main window too, one stagger step down and right of the Decoder
 *              output window when both are open, and its census name is image:window:<plugin>;
 *   reset      RESET WINDOW SIZES in the Fitted modules window puts a window that was dragged
 *              away back at the rectangle it opened at - position as well as size.
 *
 * THE FIELD REPORT. A French tester, 0.99.71, Windows 11 at 125 % scaling, main window 1618 x 947
 * at (192, 82) on a 1920-wide screen: "in POCSAG and DMR mode the table does not open to show the
 * data, a partial vertical bar appears". His log held "preset: opened the Decoder output window
 * for DMR Monitor" and nothing else about windows. The window was placed at the main window's
 * right edge plus 166 px, looking at no monitor and no main window, so on a main window within
 * about 185 px of the screen's right edge it was off the screen and ImGui's clamp left exactly 19
 * px of it - its brass margin - showing. Measured here before the fix: a 1600 x 1000 main window,
 * the window at x 1583 (of 1602), 19 px of it in the window; as real operating-system windows,
 * the main window ending at x 5121 on a 5120-wide screen, the Decoder output window at x 5101.
 *
 * The text decoder is the probe module of tests/fixtures/rescan_probe_plugin.cpp: a real decoder
 * the host really loads, declaring CASCADE_CAP_DECODER and no window of its own. The picture
 * decoder is tests/fixtures/image_probe_plugin.cpp, which never offers a picture.
 *
 * NO NETWORK: every telemetry address is a discard port and the update check is off.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

#include "gui/page_geometry.hpp"
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
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

struct Result {
    bool ok = false;
    // THE MAIN WINDOW AS THE APPLICATION DREW IT, from the window list its own self-capture
    // writes beside the picture (the "##cascade_root" row): the size a run ASKS the main window
    // to be is not the size it is (the caption and frame add a few pixels, and differently on
    // each platform), and "inside the main window" has to be asked of the real one.
    bool haveVp = false;
    Rect vp;
    std::map<std::string, Rect> rects;
    std::string out;  // what the process printed
    std::string log;  // the diagnostic log it wrote
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
};

fs::path g_dir;
std::string g_probeDll;  // the text decoder fixture, copied into the scratch plugins folder
std::string g_imageDll;  // the picture decoder fixture, copied in for the picture runs

fs::path pluginsDir() { return g_dir / "local" / "foxsdr" / "plugins"; }

// THE PLUGINS FOLDER AS THE NEXT RUN FINDS IT: the text decoder, the picture decoder, or both.
void installPlugins(bool text, bool image) {
    std::error_code ec;
    fs::remove_all(pluginsDir(), ec);
    fs::create_directories(pluginsDir(), ec);
    if (text) {
        fs::copy_file(g_probeDll, pluginsDir() / (std::string("rescan_probe_a") + kExt),
                      fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    if (image) {
        fs::copy_file(g_imageDll, pluginsDir() / (std::string("image_probe") + kExt),
                      fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
}

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

// Every file under `dir`, one after another: the diagnostic log lives in a subfolder of
// FOXSDR_DIAG_DIR and this does not care what it is called.
std::string readTree(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) { all += readFile(it->path()) + "\n"; }
    }
    return all;
}

struct RunOptions {
    int frames = 100;
    std::string windowSize = "1600x1000";
    std::string press = "Rescan Probe a";  // FOXSDR_PRESS_PRESET: the plugin whose preset is pressed
    bool decodeTest = false;   // CASCADE_DECODE_TEST: start the receiver, so image decoders run
    bool openBySeam = false;   // FOXSDR_OPEN_DECODER_OUTPUT
    bool openFitted = false;   // FOXSDR_OPEN_FITTED_MODULES, in a rectangle inside the main window
    std::string script;        // FOXSDR_INPUT_SCRIPT, empty = none
};

// One run of the application.
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
    std::ostringstream c;
    c << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
         "\"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": \"receiver\", "
         "\"railBank\": 2, \"bandPlanOverlay\": false";
    if (o.openFitted) {
        // The Fitted modules window opens a stagger slot past the main window's right edge when
        // nothing is saved, which in a single viewport is outside the only window there is.
        c << ", \"fittedModulesX\": 60, \"fittedModulesY\": 50, \"fittedModulesWidth\": 1400, "
             "\"fittedModulesHeight\": 880";
    }
    c << " }\n";
    writeFile(cfg, c.str());
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    writeFile(scriptPath, o.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_WINDOW_SIZE", o.windowSize);
    setEnv("FOXSDR_PRESS_PRESET", o.press);
    setEnv("CASCADE_DECODE_TEST", o.decodeTest ? "1" : "");
    setEnv("FOXSDR_OPEN_DECODER_OUTPUT", o.openBySeam ? "1" : "");
    setEnv("FOXSDR_OPEN_FITTED_MODULES", o.openFitted ? "1" : "");
    setEnv("FOXSDR_INPUT_SCRIPT", o.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    // One self-capture, ten frames before the end: its window list is where the main window's
    // real rectangle is read from.
    const int shotFrame = o.frames - 10;
    const fs::path shotDir = g_dir / (tag + "-shot");
    {
        std::error_code ec;
        fs::remove_all(shotDir, ec);
        fs::create_directories(shotDir, ec);
    }
    setEnv("FOXSDR_SHOT_DIR", shotDir.string());
    setEnv("FOXSDR_SHOT_AT_FRAME", std::to_string(shotFrame));
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(o.frames) + " 2>&1");
    r.log = readTree(diag);
    const bool rendered = r.out.find("rendered " + std::to_string(o.frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
    // "rect <name> x0 y0 x1 y1", and the name may hold spaces (image:window:<a plugin's name>),
    // so the four numbers are taken from the right.
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) { tok.push_back(t); }
        if (tok.size() < 5) { continue; }
        Rect rc;
        const std::size_t n = tok.size();
        rc.x0 = std::strtof(tok[n - 4].c_str(), nullptr);
        rc.y0 = std::strtof(tok[n - 3].c_str(), nullptr);
        rc.x1 = std::strtof(tok[n - 2].c_str(), nullptr);
        rc.y1 = std::strtof(tok[n - 1].c_str(), nullptr);
        std::string name = tok[0];
        for (std::size_t i = 1; i + 4 < n; ++i) { name += " " + tok[i]; }
        r.rects[name] = rc;
    }
    // "<x> <y> <w> <h> <window name>", tab separated, one window per line.
    std::ifstream wl(shotDir / ("shot-" + std::to_string(shotFrame) + ".windows.txt"));
    while (std::getline(wl, line)) {
        if (line.find("##cascade_root") == std::string::npos) { continue; }
        float x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(line.c_str(), "%f %f %f %f", &x, &y, &w, &h) == 4 && w > 0.0f && h > 0.0f) {
            r.vp = Rect{x, y, x + w, y + h};
            r.haveVp = true;
        }
    }
    r.ok = true;
    return r;
}

const char* const kPresetLine = "preset: opened the Decoder output window for Rescan Probe a";

// THE CHECKS EVERY PLACEMENT SHARES: the Decoder output window the census saw is inside the main
// window IN FULL, and wide enough to read. "Wide" is asked of the part that is INSIDE the main
// window, which is the part a person sees: the window used to be 720 wide in every run and 19 px
// of it on screen. Returns the window's rectangle (nullptr when the census never saw one).
const Rect* checkWindowInside(const Result& r, const char* what, float minW) {
    CHECK(r.haveVp);
    const Rect* w = r.rect("decoder:window");
    CHECK(w != nullptr);
    if (w == nullptr || !r.haveVp) {
        std::printf("    %s: decoder:window was never reported, or the main window's rectangle was not\n", what);
        return w;
    }
    const Rect& vp = r.vp;
    const float visibleW = std::max(0.0f, std::min(w->x1, vp.x1) - std::max(w->x0, vp.x0));
    std::printf("    %s: decoder:window x %.0f..%.0f y %.0f..%.0f (%.0f x %.0f); %.0f px of its width inside the "
                "main window (x %.0f..%.0f y %.0f..%.0f)\n",
                what, w->x0, w->x1, w->y0, w->y1, w->w(), w->h(), visibleW, vp.x0, vp.x1, vp.y0, vp.y1);
    CHECK(visibleW >= minW);
    CHECK(w->w() >= minW);
    CHECK(w->x0 >= vp.x0);
    CHECK(w->y0 >= vp.y0);
    CHECK(w->x1 <= vp.x1);
    CHECK(w->y1 <= vp.y1);
    return w;
}

bool within(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// A scripted drag of the pointer from `from` to `to`, in four steps, starting at `frame`.
std::string drag(int frame, float fromX, float fromY, float toX, float toY) {
    std::string s;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n", frame, fromX, fromY, frame + 2);
    s += buf;
    for (int i = 1; i <= 4; ++i) {
        const float f = static_cast<float>(i) / 4.0f;
        std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n", frame + 2 + 2 * i, fromX + (toX - fromX) * f,
                      fromY + (toY - fromY) * f);
        s += buf;
    }
    std::snprintf(buf, sizeof(buf), "%d up\n", frame + 12);
    s += buf;
    return s;
}

std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2,
                  frame + 4);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_decoder_window_app\n");
    if (argc < 3) {
        std::printf("usage: test_decoder_window_app <text probe module> <image probe module>\n");
        return 2;
    }
    g_probeDll = argv[1];
    g_imageDll = argv[2];
    g_dir = fs::temp_directory_path() / ("cascade-decoderwin-" + std::to_string(pid()));
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
    // A fake package identity puts the plugins folder under LOCALAPPDATA (the scratch tree), not
    // beside the executable in the build tree.
    setEnv("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");
    installPlugins(true, false);

    // --- A PRESET PRESSED, on a main window as large as the one in the field --------------------
    {
        RunOptions o;
        const Result r = once("preset", o);
        CHECK(r.ok);
        if (r.ok) {
            // THE PRESS OPENED THE SHARED WINDOW, and the log says so (the one line the field
            // report had).
            CHECK(r.log.find(kPresetLine) != std::string::npos);
            checkWindowInside(r, "1600 x 1000", 600.0f);
        }
    }

    // --- A SMALLER MAIN WINDOW, and the smallest the application allows -------------------------
    {
        RunOptions o;
        o.windowSize = "1000x700";
        const Result r = once("small", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.log.find(kPresetLine) != std::string::npos);
            checkWindowInside(r, "1000 x 700", 240.0f);
        }
        // 700 x 450 is too small for the page's preferred 720 x 520: it is held to the main
        // window less a margin on each side (and never under the page floor, 240 x 140).
        RunOptions t;
        t.windowSize = "700x450";
        const Result rt = once("tiny", t);
        CHECK(rt.ok);
        if (rt.ok) {
            const Rect* w = checkWindowInside(rt, "700 x 450", 240.0f);
            if (w != nullptr && rt.haveVp) {
                CHECK(w->w() < 720.0f);
                CHECK(within(w->w(), rt.vp.w() - 2.0f * cascade::gui::kPageInsideMargin, 1.0f));
                CHECK(within(w->h(), rt.vp.h() - 2.0f * cascade::gui::kPageInsideMargin, 1.0f));
            }
        }
    }

    // --- THE CAPTURE SEAM opens it through the same placement -----------------------------------
    {
        RunOptions o;
        o.press.clear();
        o.openBySeam = true;
        const Result r = once("seam", o);
        CHECK(r.ok);
        if (r.ok) {
            checkWindowInside(r, "seam, 1600 x 1000", 600.0f);
            // No preset was pressed, so no preset line: the two ways in are not the same event.
            CHECK(r.log.find(kPresetLine) == std::string::npos);
        }
    }

    // --- A PICTURE WINDOW, and the two together --------------------------------------------------
    //
    // The plugin's image window opened at the same anchor (one stagger slot nearer the main
    // window's edge than the Decoder output window). The receiver is started (CASCADE_DECODE_TEST)
    // because an image decoder is only listed, and its window only drawn, while one is running.
    // Its census name is "image:window:" and the plugin's own name, spaces and all.
    {
        installPlugins(true, true);
        RunOptions o;
        o.press = "Image Probe";
        o.decodeTest = true;
        const Result r = once("picture", o);
        CHECK(r.ok);
        if (r.ok) {
            CHECK(r.haveVp);
            const Rect* p = r.rect("image:window:Image Probe");
            CHECK(p != nullptr);
            if (p != nullptr && r.haveVp) {
                std::printf("    image:window:Image Probe x %.0f..%.0f y %.0f..%.0f (%.0f x %.0f)\n", p->x0, p->x1, p->y0,
                            p->y1, p->w(), p->h());
                CHECK(p->w() >= 600.0f);
                CHECK(p->x0 >= r.vp.x0 && p->x1 <= r.vp.x1 && p->y0 >= r.vp.y0 && p->y1 <= r.vp.y1);
            }
            // A picture decoder has a window of its own, so its press does not open the shared
            // Decoder output window.
            CHECK(r.log.find("preset: opened the Decoder output window for Image Probe") == std::string::npos);
            CHECK(r.rect("decoder:window") == nullptr);
        }

        // BOTH OPEN: the Decoder output window (slot 0, centred) and the picture window (slot 1,
        // one step down and right of it), each wholly inside the main window and neither hiding the
        // other's rail.
        RunOptions both = o;
        both.openBySeam = true;
        const Result rb = once("picture-and-output", both);
        CHECK(rb.ok);
        if (rb.ok) {
            const Rect* d = rb.rect("decoder:window");
            const Rect* p = rb.rect("image:window:Image Probe");
            CHECK(d != nullptr);
            CHECK(p != nullptr);
            if (d != nullptr && p != nullptr && rb.haveVp) {
                std::printf("    together: decoder:window x %.0f y %.0f, image:window x %.0f y %.0f\n", d->x0, d->y0, p->x0,
                            p->y0);
                CHECK(within(p->x0 - d->x0, cascade::gui::kPageInsideStagger, 1.0f));
                CHECK(within(p->y0 - d->y0, cascade::gui::kPageInsideStagger, 1.0f));
                CHECK(d->x0 >= rb.vp.x0 && d->x1 <= rb.vp.x1 && d->y0 >= rb.vp.y0 && d->y1 <= rb.vp.y1);
                CHECK(p->x0 >= rb.vp.x0 && p->x1 <= rb.vp.x1 && p->y0 >= rb.vp.y0 && p->y1 <= rb.vp.y1);
            }
        }
        installPlugins(true, false);
    }

    // --- RESET WINDOW SIZES puts a dragged window back where it opened --------------------------
    //
    // The layout run gives the rectangle the window opens at and the RESET key's; the drag run
    // moves the window by the rail (and says that it moved); the reset run presses RESET after the
    // drag and must end where the layout run did, in position AND size.
    {
        RunOptions lay;
        lay.openFitted = true;
        lay.frames = 110;
        const Result l = once("reset-layout", lay);
        CHECK(l.ok);
        const Rect* d0 = l.rect("decoder:window");
        const Rect* resetKey = l.rect("fitted:reset");
        CHECK(d0 != nullptr);
        CHECK(resetKey != nullptr);
        if (l.ok && d0 != nullptr && resetKey != nullptr) {
            const Rect home = *d0;
            const Rect key = *resetKey;
            // The rail is the top strip of the window, left of its keys: eight pixels down and a
            // fifth of the way across is on it and clear of the 4 px resize band.
            const float gx = home.x0 + home.w() * 0.2f;
            const float gy = home.y0 + 8.0f;
            const float dx = 150.0f;
            const float dy = 90.0f;

            RunOptions dragged = lay;
            dragged.script = drag(45, gx, gy, gx + dx, gy + dy);
            const Result d = once("reset-drag", dragged);
            CHECK(d.ok);
            const Rect* d1 = d.rect("decoder:window");
            CHECK(d1 != nullptr);
            if (d.ok && d1 != nullptr) {
                std::printf("    opened at x %.0f y %.0f; after the drag x %.0f y %.0f\n", home.x0, home.y0, d1->x0,
                            d1->y0);
                // THE CONTROL: without the reset, the window stays where it was dragged to, so
                // the reset run below ends where it does BECAUSE of the reset.
                CHECK(within(d1->x0, home.x0 + dx, 6.0f));
                CHECK(within(d1->y0, home.y0 + dy, 6.0f));
                CHECK(within(d1->w(), home.w(), 1.0f));
            }

            RunOptions reset = dragged;
            reset.script = dragged.script + click(75, key);
            const Result rr = once("reset-press", reset);
            CHECK(rr.ok);
            const Rect* d2 = rr.rect("decoder:window");
            CHECK(d2 != nullptr);
            if (rr.ok && d2 != nullptr) {
                std::printf("    after RESET WINDOW SIZES x %.0f y %.0f (%.0f x %.0f)\n", d2->x0, d2->y0, d2->w(),
                            d2->h());
                CHECK(within(d2->x0, home.x0, 1.0f));
                CHECK(within(d2->y0, home.y0, 1.0f));
                CHECK(within(d2->w(), home.w(), 1.0f));
                CHECK(within(d2->h(), home.h(), 1.0f));
            }
        }
    }

    std::error_code ec2;
    fs::remove_all(g_dir, ec2);
    return testSummary("test_decoder_window_app");
}
