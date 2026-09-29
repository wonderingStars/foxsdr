/*
 * THE PATCH PAGE'S INFORMATION PANE, MOVED UP, IN THE REAL APPLICATION
 * (0.99.49 beta feedback). On a small laptop the patch page's information
 * column - "This patch can run", "Nothing selected", the help - took a
 * full-height strip of the canvas's width while the top right of the page,
 * beside the parts bin, sat empty. The rule is pinned in
 * tests/test_patch_page_layout.cpp; this runs cascade itself, at a small
 * laptop's 1366 x 768 and at 1920 x 1080, and checks what it drew (the
 * interface census):
 *
 *   still     nothing selected: the pane is in the top band, at its right,
 *             beside - not under - the parts bin, above the canvas, and of a
 *             fixed height; the canvas has the page's whole width below it;
 *             there is no inspector drawer;
 *   selected  a click on a node's title bar: the drawer with its controls
 *             appears at the canvas's right, the old column's width, and the
 *             canvas gives it exactly that room;
 *   folded    its fold key: the drawer is a narrow strip, the canvas has
 *             nearly the whole width back, and the key is inside the strip.
 *
 * The presses are the application's own input script, aimed from the census.
 * Isolated like every other test that starts the application; the patch's
 * radio is on the signal generator, switched off, and never started.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "core/patch_io.hpp"
#include "gui/patch_view_math.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace pv = cascade::gui::patch;

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

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    cascade::core::patch::LoadResult patch;  // what the application saved
};

constexpr int kFrames = 70;
fs::path g_dir;

// Two nodes, wired: a Radio on the generator, switched OFF so nothing opens,
// and a Channel. The view is as given.
std::string patchText(float panX, float panY) {
    std::ostringstream o;
    o << "foxsdr-patch 6\n"
      << "view " << panX << ' ' << panY << " 1\n"
      << "node 1 0 0 40 40 0 0 100000000 0 - siggen 2000000 1 -50 0 Radio\n"
      << "node 2 1 0 420 250 0 0 100100000 0 - - 0 0 -50 1 Channel\n"
      << "wire 1 0 2 0\n";
    return o.str();
}

Result once(const std::string& tag, const std::string& script, const std::string& size,
            float panX = 0.0f, float panY = 0.0f) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\" }\n";
    }
    const fs::path pp = g_dir / (tag + ".patch");
    {
        std::ofstream f(pp, std::ios::binary | std::ios::trunc);
        f << patchText(panX, panY);
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", size);
    setEnv("FOXSDR_PATCH_FILE", pp.string());
    if (script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (tag + ".script");
        std::ofstream f(sp);
        f << script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    const bool scripted = script.empty() || out.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return r;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { r.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            r.rects[name] = rc;
        }
    }
    cascade::core::AppConfig saved;
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", tag.c_str(), err.c_str());
        return r;
    }
    r.patch = cascade::core::patch::parse(saved.patch);
    r.ok = r.patch.ok && r.rects.count("patch:canvas") == 1;
    if (!r.ok) { std::printf("  %s: no saved patch or no canvas rectangle\n", tag.c_str()); }
    return r;
}

bool has(const Result& r, const char* rect) { return r.rects.count(rect) == 1; }

void atSize(const std::string& size) {
    std::printf("  at %s\n", size.c_str());
    const std::string tag = size;
    const Result still = once("still-" + tag, "", size);
    CHECK(still.ok);
    CHECK(has(still, "view:patch"));
    if (!still.ok || !has(still, "view:patch")) { return; }
    const Rect canvas = still.rects.at("patch:canvas");
    // THE CANVAS HAS THE PAGE'S WHOLE WIDTH: from the page's left, out to the
    // same edge as the pane - no column beside it.
    const Rect page = still.rects.at("view:patch");
    CHECK(canvas.x0 - page.x0 < 20.0f);
    CHECK(page.x1 - canvas.x1 < 20.0f);
    CHECK(still.items.count("patch:inspector") == 0);
    CHECK(!has(still, "patch:inspector"));
    CHECK(has(still, "patch:info"));
    if (!has(still, "patch:info")) { return; }
    const Rect info = still.rects.at("patch:info");
    std::printf("    info %.0f..%.0f x %.0f..%.0f, canvas %.0f..%.0f x %.0f..%.0f\n", info.x0,
                info.x1, info.y0, info.y1, canvas.x0, canvas.x1, canvas.y0, canvas.y1);

    // THE PANE IS IN THE TOP BAND, AT ITS RIGHT: above the canvas, flush with
    // the canvas's right edge, beside the transport and parts rows.
    CHECK(info.y1 <= canvas.y0);
    CHECK(std::fabs(info.x1 - canvas.x1) <= 2.0f);
    CHECK(info.x0 > canvas.x0 + (canvas.x1 - canvas.x0) * 0.4f);
    // ...of a fixed height, not a full-height column.
    CHECK(info.y1 - info.y0 >= pv::kPatchInfoMinH - 0.5f);
    CHECK(info.y1 - info.y0 < 200.0f);
    // ...and BESIDE the parts bin, never under it: every part key is left of
    // the pane and inside its band.
    for (int i = 0; i < 7; ++i) {
        const std::string key = "patchpart:" + std::to_string(i);
        CHECK(has(still, key.c_str()));
        if (!has(still, key.c_str())) { continue; }
        const Rect k = still.rects.at(key);
        CHECK(k.x1 < info.x0);
        CHECK(k.y1 <= canvas.y0);
    }

    // --- selected: the drawer with the node's controls ------------------------
    char script[160];
    std::snprintf(script, sizeof script, "20 screen %.0f %.0f\n22 down\n24 up\n26 screen %.0f %.0f\n",
                  canvas.x0 + 40.0f + 60.0f, canvas.y0 + 40.0f + pv::kHeaderHeight * 0.5f,
                  canvas.x0 + 30.0f, canvas.y1 - 30.0f);
    const Result sel = once("selected-" + tag, script, size);
    CHECK(sel.ok);
    CHECK(has(sel, "patch:inspector"));
    CHECK(has(sel, "patchinspfold"));
    if (!sel.ok || !has(sel, "patch:inspector") || !has(sel, "patchinspfold")) { return; }
    const Rect drawer = sel.rects.at("patch:inspector");
    const Rect selCanvas = sel.rects.at("patch:canvas");
    std::printf("    selected: drawer %.0f..%.0f, canvas to %.0f\n", drawer.x0, drawer.x1,
                selCanvas.x1);
    CHECK_NEAR(drawer.x1 - drawer.x0, pv::kPatchInspectorW, 1.0);
    CHECK(drawer.x0 > selCanvas.x1);
    CHECK(std::fabs(drawer.x1 - canvas.x1) <= 2.0f);
    CHECK(drawer.y0 >= info.y1);  // below the pane, beside the canvas
    CHECK_NEAR(selCanvas.x1, canvas.x1 - pv::kPatchInspectorW - pv::kPatchPaneGap, 1.0);
    // The pane stays where it was.
    CHECK_NEAR(sel.rects.at("patch:info").y1, info.y1, 0.5);
    const Rect fold = sel.rects.at("patchinspfold");
    CHECK(fold.x0 >= drawer.x0 && fold.x1 <= drawer.x1);

    // --- folded: a strip, and the canvas has its width back -------------------
    std::snprintf(script, sizeof script,
                  "20 screen %.0f %.0f\n22 down\n24 up\n30 screen %.0f %.0f\n32 down\n34 up\n"
                  "36 screen %.0f %.0f\n",
                  canvas.x0 + 40.0f + 60.0f, canvas.y0 + 40.0f + pv::kHeaderHeight * 0.5f,
                  fold.cx(), fold.cy(), canvas.x0 + 30.0f, canvas.y1 - 30.0f);
    const Result folded = once("folded-" + tag, script, size);
    CHECK(folded.ok);
    CHECK(has(folded, "patch:inspector"));
    if (!folded.ok || !has(folded, "patch:inspector")) { return; }
    const Rect strip = folded.rects.at("patch:inspector");
    std::printf("    folded: strip %.0f..%.0f, canvas to %.0f\n", strip.x0, strip.x1,
                folded.rects.at("patch:canvas").x1);
    CHECK_NEAR(strip.x1 - strip.x0, pv::kPatchInspectorFoldedW, 1.0);
    CHECK_NEAR(folded.rects.at("patch:canvas").x1,
               canvas.x1 - pv::kPatchInspectorFoldedW - pv::kPatchPaneGap, 1.0);
    const Rect key = folded.rects.at("patchinspfold");
    CHECK(key.x0 >= strip.x0 && key.x1 <= strip.x1);  // the way back is on the strip
}

}  // namespace

int main() {
    std::printf("test_patch_info_pane\n");
    g_dir = fs::temp_directory_path() / ("cascade-info-pane-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "diag", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (g_dir / "diag").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_PATCH_START", "");

    atSize("1366x768");
    atSize("1920x1080");

    const int rc = testSummary("test_patch_info_pane");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
