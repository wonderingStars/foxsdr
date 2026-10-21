/*
 * THE FUNCTION SELECT RAIL FOLDS AWAY, IN THE REAL APPLICATION (0.99.49
 * beta feedback: a small laptop, where the rail is 384 of 1366 pixels). The
 * geometry and the config are pinned in tests/test_rail_fold.cpp; this runs
 * cascade itself, at 1366 x 768 and 1920 x 1080, and checks from the UI
 * census and the config it saved:
 *
 *   open      a fresh config: the rail is open, the "<<" key sits in its
 *             title row, and the patch view starts right of the rail;
 *   fold      one press of "<<": the rail is a strip, the patch view has
 *             grown left by the width freed, and the config says folded;
 *   receiver  a folded config on the receiver view: folded from the first
 *             frame, and the receiver's view takes the width too;
 *   unfold    one press of ">>" on the strip: the rail is open again and the
 *             config says so.
 *
 * The presses are the application's own input script, aimed from the census.
 * Isolated like every other test that starts the application.
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
#include "gui/app_window.hpp"
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
    bool railCollapsed = false;              // ...and the rail's state
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
            const std::string& extraConfig, float panX = 0.0f, float panY = 0.0f) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\""
          << extraConfig << " }\n";
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
    r.railCollapsed = saved.railCollapsed;
    r.ok = r.rects.count("rail:column") == 1;
    if (!r.ok) { std::printf("  %s: no rail rectangle\n", tag.c_str()); }
    return r;
}

bool has(const Result& r, const char* rect) { return r.rects.count(rect) == 1; }

std::string press(const Rect& k) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "20 screen %.0f %.0f\n22 down\n24 up\n26 screen %.0f %.0f\n",
                  k.cx(), k.cy(), k.cx() + 400.0f, k.cy() + 300.0f);
    return buf;
}

void atSize(const std::string& size) {
    namespace g = cascade::gui;
    std::printf("  at %s\n", size.c_str());

    // --- open: a fresh config ------------------------------------------------
    const Result open = once("open-" + size, "", size, "");
    CHECK(open.ok);
    CHECK(has(open, "railkey:fold"));
    CHECK(has(open, "view:patch"));
    if (!open.ok || !has(open, "railkey:fold") || !has(open, "view:patch")) { return; }
    const Rect rail = open.rects.at("rail:column");
    const Rect key = open.rects.at("railkey:fold");
    const Rect view = open.rects.at("view:patch");
    std::printf("    open: rail %.0f..%.0f, key %.0f..%.0f, patch view from %.0f\n", rail.x0,
                rail.x1, key.x0, key.x1, view.x0);
    CHECK(open.items.count("rail:open") == 1);
    CHECK(open.items.count("rail:folded") == 0);
    CHECK(open.items.count("key:railfold") == 1);
    CHECK_NEAR(rail.x1 - rail.x0, g::kMenuWidth, 1.0);
    // The key: in the rail, at its right, in the title row at the top.
    CHECK(key.x0 > rail.cx() && key.x1 <= rail.x1);
    CHECK(key.y0 >= rail.y0 && key.y1 <= rail.y0 + 32.0f);
    CHECK(view.x0 >= rail.x1);
    CHECK(!open.railCollapsed);

    // --- fold: one press of "<<" -----------------------------------------------
    const Result fold = once("fold-" + size, press(key), size, "");
    CHECK(fold.ok);
    CHECK(has(fold, "view:patch"));
    if (!fold.ok || !has(fold, "view:patch")) { return; }
    const Rect strip = fold.rects.at("rail:column");
    const Rect wider = fold.rects.at("view:patch");
    std::printf("    folded: strip %.0f..%.0f, patch view %.0f..%.0f (was %.0f..%.0f)\n", strip.x0,
                strip.x1, wider.x0, wider.x1, view.x0, view.x1);
    CHECK(fold.items.count("rail:folded") == 1);  // (the frames before the press drew it open)
    CHECK_NEAR(strip.x1 - strip.x0, g::kRailFoldedW, 1.0);
    // THE MAIN AREA TAKES THE FREED WIDTH, all of it.
    CHECK_NEAR(view.x0 - wider.x0, g::kMenuWidth - g::kRailFoldedW, 1.5);
    CHECK_NEAR(wider.x1, view.x1, 0.5);
    CHECK(fold.railCollapsed);  // remembered
    CHECK(has(fold, "railkey:unfold"));

    // --- receiver: folded from the config, on the receiver view -----------------
    const Result recv =
        once("receiver-" + size, "", size, ", \"mainView\": \"receiver\", \"railCollapsed\": true");
    const Result recvOpen = once("receiver-open-" + size, "", size, ", \"mainView\": \"receiver\"");
    CHECK(recv.ok && recvOpen.ok);
    CHECK(has(recv, "view:receiver") && has(recvOpen, "view:receiver"));
    if (has(recv, "view:receiver") && has(recvOpen, "view:receiver")) {
        const Rect rv = recv.rects.at("view:receiver");
        const Rect rvOpen = recvOpen.rects.at("view:receiver");
        std::printf("    receiver: view from %.0f folded, %.0f open\n", rv.x0, rvOpen.x0);
        CHECK(recv.items.count("rail:open") == 0);  // folded from the first frame
        CHECK(recv.items.count("rail:folded") == 1);
        CHECK_NEAR(rvOpen.x0 - rv.x0, g::kMenuWidth - g::kRailFoldedW, 1.5);
        CHECK(recv.railCollapsed);
    }

    // --- unfold: one press of ">>" on the strip ---------------------------------
    if (has(recv, "railkey:unfold")) {
        const Rect k = recv.rects.at("railkey:unfold");
        CHECK(k.x0 >= strip.x0 && k.x1 <= strip.x1);  // the way back is on the strip
        const Result unfold = once("unfold-" + size, press(k), size,
                                   ", \"mainView\": \"receiver\", \"railCollapsed\": true");
        CHECK(unfold.ok);
        CHECK(unfold.items.count("rail:open") == 1);
        CHECK(!unfold.railCollapsed);
        if (unfold.ok) {
            CHECK_NEAR(unfold.rects.at("rail:column").x1 - unfold.rects.at("rail:column").x0,
                       g::kMenuWidth, 1.0);
        }
    } else {
        CHECK(has(recv, "railkey:unfold"));
    }
}

}  // namespace

int main() {
    std::printf("test_rail_fold_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-rail-fold-" + std::to_string(pid()));
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

    const int rc = testSummary("test_rail_fold_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
