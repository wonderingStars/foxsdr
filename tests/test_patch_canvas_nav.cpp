/*
 * MOVING ROUND THE PATCH CANVAS, IN THE REAL APPLICATION (0.99.49 beta
 * feedback). A tester on a small laptop could not pan the patch at all: the
 * canvas panned only on a middle-button drag, and a touchpad has no middle
 * button. The maths is pinned in tests/test_patch_view_nav.cpp; this runs
 * cascade itself with a two-node patch and checks, from the patch the
 * application saved (CASCADE_CONFIG_TEST), what each gesture did:
 *
 *   still      no input: the view and the nodes are where the file put them;
 *   dragempty  a LEFT drag on empty canvas pans the view by the drag, and
 *              moves no node;
 *   dragnode   a left drag that starts on a node's title bar moves that node
 *              and does NOT pan;
 *   wheel      a plain wheel pans vertically and no longer zooms;
 *   wheelh     a sideways two-finger scroll pans horizontally;
 *   ctrlwheel  Ctrl+wheel (a touchpad pinch) zooms about the pointer;
 *   zoomin     the canvas's "+" key zooms in about the canvas centre;
 *   fit        with both nodes scrolled far off screen, "Fit" brings every
 *              node back inside the canvas.
 *
 * The presses are the application's own input script (FOXSDR_INPUT_SCRIPT),
 * aimed at the census rectangles of the canvas and its keys. Isolated like
 * every other test that starts the application; the patch's radio is on the
 * signal generator and switched off, and the patch is never started.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

Result once(const std::string& tag, const std::string& script, float panX = 0.0f,
            float panY = 0.0f) {
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
    setEnv("FOXSDR_WINDOW_SIZE", "1280x720");
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

const cascade::core::patch::Node* nodeNamed(const Result& r, const char* name) {
    for (const cascade::core::patch::Node& n : r.patch.graph.nodes()) {
        if (n.name == name) { return &n; }
    }
    return nullptr;
}

std::string press(int frame, float x, float y) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n", frame, x, y, frame + 2);
    return buf;
}

// A left drag from (x, y) by (dx, dy), in four steps, released at the end.
std::string drag(int frame, float x, float y, float dx, float dy) {
    std::string s = press(frame, x, y);
    char buf[64];
    for (int k = 1; k <= 4; ++k) {
        std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n", frame + 2 + 3 * k,
                      x + dx * static_cast<float>(k) / 4.0f, y + dy * static_cast<float>(k) / 4.0f);
        s += buf;
    }
    std::snprintf(buf, sizeof buf, "%d up\n", frame + 18);
    return s + buf;
}

std::string click(int frame, const Rect& r) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%d screen %.0f %.0f\n%d down\n%d up\n", frame, r.cx(), r.cy(),
                  frame + 2, frame + 4);
    return buf;
}

}  // namespace

int main() {
    std::printf("test_patch_canvas_nav\n");
    g_dir = fs::temp_directory_path() / ("cascade-canvas-nav-" + std::to_string(pid()));
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

    // --- still: where everything starts ----------------------------------------
    const Result still = once("still", "");
    CHECK(still.ok);
    if (!still.ok) { return testSummary("test_patch_canvas_nav"); }
    const Rect canvas = still.rects.at("patch:canvas");
    const cascade::core::patch::Node* radio0 = nodeNamed(still, "Radio");
    const cascade::core::patch::Node* chan0 = nodeNamed(still, "Channel");
    CHECK(radio0 != nullptr && chan0 != nullptr);
    if (radio0 == nullptr || chan0 == nullptr) { return testSummary("test_patch_canvas_nav"); }
    CHECK_NEAR(still.patch.panX, 0.0, 1e-3);
    CHECK_NEAR(still.patch.panY, 0.0, 1e-3);
    CHECK_NEAR(still.patch.zoom, 1.0, 1e-4);
    std::printf("  canvas %.0f..%.0f x %.0f..%.0f\n", canvas.x0, canvas.x1, canvas.y0, canvas.y1);
    // Somewhere on the canvas with no node, port or wire near it: its
    // bottom-left corner (the view keys are bottom-RIGHT).
    const float emptyX = canvas.x0 + 30.0f;
    const float emptyY = canvas.y1 - 30.0f;
    // The premise: nothing there to grab, at the view the file opens with.
    CHECK(pv::pressTarget(still.patch.graph,
                          pv::Vec2{emptyX - canvas.x0, emptyY - canvas.y0}, 1.0f,
                          7.0f) == pv::PressOn::Empty);

    // --- dragempty: a left drag on empty canvas pans ---------------------------
    {
        const Result r = once("dragempty", drag(20, emptyX, emptyY, 120.0f, -70.0f));
        CHECK(r.ok);
        std::printf("  dragempty: view %.1f, %.1f x %.3f\n", r.patch.panX, r.patch.panY,
                    r.patch.zoom);
        CHECK_NEAR(r.patch.panX, 120.0, 2.0);
        CHECK_NEAR(r.patch.panY, -70.0, 2.0);
        CHECK_NEAR(r.patch.zoom, 1.0, 1e-4);
        const cascade::core::patch::Node* n = nodeNamed(r, "Radio");
        CHECK(n != nullptr && n->x == radio0->x && n->y == radio0->y);  // no node moved
    }

    // --- dragnode: a drag that starts on a node moves the node, not the view ---
    {
        const float hx = canvas.x0 + radio0->x + 30.0f;
        const float hy = canvas.y0 + radio0->y + pv::kHeaderHeight * 0.5f;
        const Result r = once("dragnode", drag(20, hx, hy, 100.0f, 50.0f));
        CHECK(r.ok);
        const cascade::core::patch::Node* n = nodeNamed(r, "Radio");
        CHECK(n != nullptr);
        if (n != nullptr) {
            std::printf("  dragnode: Radio %.1f,%.1f -> %.1f,%.1f; view %.1f, %.1f\n", radio0->x,
                        radio0->y, n->x, n->y, r.patch.panX, r.patch.panY);
            CHECK_NEAR(n->x - radio0->x, 100.0, 2.0);
            CHECK_NEAR(n->y - radio0->y, 50.0, 2.0);
        }
        CHECK_NEAR(r.patch.panX, 0.0, 1e-3);  // DID NOT PAN
        CHECK_NEAR(r.patch.panY, 0.0, 1e-3);
    }

    // --- wheel: a plain wheel pans, and does not zoom --------------------------
    char buf[160];
    {
        std::snprintf(buf, sizeof buf, "20 screen %.0f %.0f\n24 wheel -2\n", emptyX, emptyY);
        const Result r = once("wheel", buf);
        CHECK(r.ok);
        std::printf("  wheel -2: view %.1f, %.1f x %.3f\n", r.patch.panX, r.patch.panY,
                    r.patch.zoom);
        CHECK_NEAR(r.patch.panY, -2.0 * pv::kWheelPanPx, 0.5);
        CHECK_NEAR(r.patch.panX, 0.0, 1e-3);
        CHECK_NEAR(r.patch.zoom, 1.0, 1e-4);
    }

    // --- wheelh: a sideways two-finger scroll pans sideways --------------------
    {
        std::snprintf(buf, sizeof buf, "20 screen %.0f %.0f\n24 wheelh 1\n", emptyX, emptyY);
        const Result r = once("wheelh", buf);
        CHECK(r.ok);
        CHECK_NEAR(r.patch.panX, pv::kWheelPanPx, 0.5);
        CHECK_NEAR(r.patch.panY, 0.0, 1e-3);
    }

    // --- ctrlwheel: a pinch zooms about the pointer ----------------------------
    {
        std::snprintf(buf, sizeof buf, "20 screen %.0f %.0f\n24 ctrlwheel 1\n", emptyX, emptyY);
        const Result r = once("ctrlwheel", buf);
        CHECK(r.ok);
        std::printf("  ctrlwheel 1: view %.1f, %.1f x %.3f\n", r.patch.panX, r.patch.panY,
                    r.patch.zoom);
        CHECK_NEAR(r.patch.zoom, pv::kWheelZoomStep, 1e-3);
        // The world point under the pointer stayed there: canvas-relative
        // anchor a, pan p = a - a * zoom.
        const float ax = emptyX - canvas.x0;
        const float ay = emptyY - canvas.y0;
        CHECK_NEAR(r.patch.panX, ax - ax * pv::kWheelZoomStep, 1.0);
        CHECK_NEAR(r.patch.panY, ay - ay * pv::kWheelZoomStep, 1.0);
    }

    // --- zoomin: the "+" key ----------------------------------------------------
    const auto keyIn = still.rects.find("patchviewkey:1");
    const auto keyFit = still.rects.find("patchviewkey:2");
    CHECK(keyIn != still.rects.end());
    CHECK(keyFit != still.rects.end());
    CHECK(still.rects.count("patchviewkey:0") == 1);
    if (keyIn != still.rects.end()) {
        // Inside the canvas, in its bottom-right corner.
        CHECK(keyIn->second.x0 > canvas.cx() && keyIn->second.x1 < canvas.x1);
        CHECK(keyIn->second.y0 > canvas.cy() && keyIn->second.y1 < canvas.y1);
        const Result r = once("zoomin", click(20, keyIn->second));
        CHECK(r.ok);
        CHECK_NEAR(r.patch.zoom, pv::kKeyZoomStep, 1e-3);
    }

    // --- fit: both nodes far off screen, then Fit -------------------------------
    if (keyFit != still.rects.end()) {
        const Result before = once("offscreen", "", -3000.0f, -2500.0f);
        const Result r = once("fit", click(20, keyFit->second), -3000.0f, -2500.0f);
        CHECK(before.ok && r.ok);
        std::printf("  fit: view %.1f, %.1f x %.3f\n", r.patch.panX, r.patch.panY, r.patch.zoom);
        CHECK_NEAR(before.patch.panX, -3000.0, 1e-2);  // the premise: off screen
        const pv::View v{pv::Vec2{canvas.x0 + r.patch.panX, canvas.y0 + r.patch.panY},
                         r.patch.zoom};
        for (const cascade::core::patch::Node& n : r.patch.graph.nodes()) {
            const pv::Vec2 s = pv::nodeSize(n);
            const pv::Vec2 a = pv::worldToScreen(v, pv::Vec2{n.x, n.y});
            const pv::Vec2 b = pv::worldToScreen(v, pv::Vec2{n.x + s.x, n.y + s.y});
            CHECK(a.x >= canvas.x0 && a.y >= canvas.y0 && b.x <= canvas.x1 && b.y <= canvas.y1);
        }
        CHECK(r.patch.zoom <= pv::kFitMaxZoom + 1e-4f);
    }

    const int rc = testSummary("test_patch_canvas_nav");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
