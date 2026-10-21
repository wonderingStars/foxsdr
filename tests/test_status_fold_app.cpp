/*
 * THE STATUS COLUMN FOLDS AWAY, IN THE REAL APPLICATION (0.99.74 beta
 * feedback: "Just like you added the << and >> to expand and collapse the
 * Function Select panel on the left side, can you do this for the right side
 * Status panel?"). The geometry and the config are pinned in
 * tests/test_status_fold.cpp; this runs cascade itself, at 1366 x 768 and
 * 1920 x 1080, on the receiver view (the only view that has a status column),
 * and checks from the UI census and the config it saved:
 *
 *   open      a fresh config: the column is open, the ">>" key sits at the
 *             left end of its title row, and the receiver view ends where the
 *             column begins;
 *   fold      one press of ">>": the column is a strip, the receiver view has
 *             grown right by the width freed, and the config says folded;
 *   folded    a folded config: folded from the first frame, the receiver view
 *             is as wide as after the press, and the way back is on the strip;
 *   unfold    one press of "<<" on the strip: the column is open again, the
 *             receiver view is back to its open width, and the config says so;
 *   narrow    a window too narrow for the full column but not for the strip
 *             (1100 x 700): the open column is dropped, as it always was, and
 *             the folded strip is still drawn - a strip needs 34 px, not 750.
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
    bool statusCollapsed = false;  // what the application saved
    bool railCollapsed = false;    // ...and the rail's, which must not follow
};

constexpr int kFrames = 70;
fs::path g_dir;

// One run of the application on the receiver view, as given. The generator is
// the source (no hardware), and nothing else about the config matters here.
Result once(const std::string& tag, const std::string& script, const std::string& size,
            const std::string& extraConfig) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\""
          << extraConfig << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", size);
    setEnv("FOXSDR_PATCH_FILE", "");  // the default patch, in the isolated profile
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
    r.statusCollapsed = saved.statusCollapsed;
    r.railCollapsed = saved.railCollapsed;
    r.ok = r.rects.count("view:receiver") == 1;
    if (!r.ok) { std::printf("  %s: no receiver view rectangle\n", tag.c_str()); }
    return r;
}

bool has(const Result& r, const char* rect) { return r.rects.count(rect) == 1; }

// A click on a key's centre, then the pointer parked well away from it.
std::string press(const Rect& k) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "20 screen %.0f %.0f\n22 down\n24 up\n26 screen %.0f %.0f\n",
                  k.cx(), k.cy(), k.cx() - 400.0f, k.cy() + 300.0f);
    return buf;
}

void atSize(const std::string& size) {
    namespace g = cascade::gui;
    std::printf("  at %s\n", size.c_str());

    // --- open: a fresh config ------------------------------------------------
    const Result open = once("open-" + size, "", size, "");
    CHECK(open.ok);
    CHECK(has(open, "statuscol:column"));
    CHECK(has(open, "statuskey:fold"));
    if (!open.ok || !has(open, "statuscol:column") || !has(open, "statuskey:fold")) { return; }
    const Rect col = open.rects.at("statuscol:column");
    const Rect key = open.rects.at("statuskey:fold");
    const Rect view = open.rects.at("view:receiver");
    std::printf("    open: column %.0f..%.0f, key %.0f..%.0f, receiver view %.0f..%.0f\n", col.x0,
                col.x1, key.x0, key.x1, view.x0, view.x1);
    CHECK(open.items.count("statuscol:open") == 1);
    CHECK(open.items.count("statuscol:folded") == 0);
    CHECK(open.items.count("key:statusfold") == 1);
    CHECK(open.items.count("key:statusunfold") == 0);
    CHECK_NEAR(col.x1 - col.x0, g::kStatusW, 1.0);
    // The key: in the column, at its LEFT end, in the title row at the top.
    CHECK(key.x0 >= col.x0 && key.x1 < col.cx());
    CHECK(key.y0 >= col.y0 && key.y1 <= col.y0 + 32.0f);
    CHECK(view.x1 <= col.x0 + 0.5f);  // the spectrum is left of the column
    CHECK(!open.statusCollapsed);
    CHECK(!open.railCollapsed);

    // --- fold: one press of ">>" -----------------------------------------------
    const Result fold = once("fold-" + size, press(key), size, "");
    CHECK(fold.ok);
    CHECK(has(fold, "statuscol:column"));
    if (!fold.ok || !has(fold, "statuscol:column")) { return; }
    const Rect strip = fold.rects.at("statuscol:column");
    const Rect wider = fold.rects.at("view:receiver");
    std::printf("    folded: strip %.0f..%.0f, receiver view %.0f..%.0f (was %.0f..%.0f)\n",
                strip.x0, strip.x1, wider.x0, wider.x1, view.x0, view.x1);
    CHECK(fold.items.count("statuscol:folded") == 1);  // (the frames before the press drew it open)
    CHECK(fold.items.count("key:statusunfold") == 1);
    CHECK_NEAR(strip.x1 - strip.x0, g::kStatusFoldedW, 1.0);
    // THE SPECTRUM TAKES THE FREED WIDTH, all of it, and stays where it was
    // on the left.
    CHECK_NEAR(wider.x1 - view.x1, g::kStatusW - g::kStatusFoldedW, 1.5);
    CHECK_NEAR(wider.x0, view.x0, 0.5);
    CHECK(fold.statusCollapsed);  // remembered
    CHECK(!fold.railCollapsed);   // and the other panel did not follow it
    CHECK(has(fold, "statuskey:unfold"));

    // --- folded: from the config, on the receiver view -----------------------------
    const Result folded = once("folded-" + size, "", size, ", \"statusCollapsed\": true");
    CHECK(folded.ok);
    CHECK(has(folded, "statuskey:unfold"));
    if (folded.ok) {
        const Rect fv = folded.rects.at("view:receiver");
        std::printf("    folded config: receiver view %.0f..%.0f\n", fv.x0, fv.x1);
        CHECK(folded.items.count("statuscol:open") == 0);  // folded from the first frame
        CHECK(folded.items.count("statuscol:folded") == 1);
        CHECK(folded.items.count("key:statusfold") == 0);
        CHECK_NEAR(fv.x1 - view.x1, g::kStatusW - g::kStatusFoldedW, 1.5);
        CHECK(folded.statusCollapsed);
    }

    // --- unfold: one press of "<<" on the strip ---------------------------------
    if (has(folded, "statuskey:unfold") && has(folded, "statuscol:column")) {
        const Rect k = folded.rects.at("statuskey:unfold");
        const Rect stripRect = folded.rects.at("statuscol:column");
        CHECK(k.x0 >= stripRect.x0 && k.x1 <= stripRect.x1);  // the way back is on the strip
        const Result unfold = once("unfold-" + size, press(k), size, ", \"statusCollapsed\": true");
        CHECK(unfold.ok);
        CHECK(unfold.items.count("statuscol:open") == 1);
        CHECK(!unfold.statusCollapsed);
        if (unfold.ok && has(unfold, "statuscol:column")) {
            CHECK_NEAR(unfold.rects.at("statuscol:column").x1 - unfold.rects.at("statuscol:column").x0,
                       g::kStatusW, 1.0);
            CHECK_NEAR(unfold.rects.at("view:receiver").x1, view.x1, 0.5);  // back where it was
        }
    } else {
        CHECK(has(folded, "statuskey:unfold"));
    }
}

// THE BOUNDARY: a window with room for the strip and not for the full column.
// The full column still needs 230 + the 520-px floor under the spectrum; the
// strip needs 34 + 520, so a folded column survives a window the open one
// does not.
void atNarrow() {
    namespace g = cascade::gui;
    const std::string size = "1100x700";
    std::printf("  at %s (room for the strip, not for the column)\n", size.c_str());
    const Result open = once("narrow-open", "", size, "");
    const Result folded = once("narrow-folded", "", size, ", \"statusCollapsed\": true");
    CHECK(open.ok);
    CHECK(folded.ok);
    if (!open.ok || !folded.ok) { return; }
    const Rect ov = open.rects.at("view:receiver");
    const Rect fv = folded.rects.at("view:receiver");
    std::printf("    receiver view %.0f..%.0f open (column %s), %.0f..%.0f folded (strip %s)\n",
                ov.x0, ov.x1, has(open, "statuscol:column") ? "drawn" : "dropped", fv.x0, fv.x1,
                has(folded, "statuscol:column") ? "drawn" : "dropped");
    // Open: dropped on a window this narrow, exactly as before the fold key.
    CHECK(open.items.count("statuscol:open") == 0);
    CHECK(!has(open, "statuscol:column"));
    // Folded: the strip is there, with the key that opens it, and the spectrum
    // gave up only the strip's width.
    CHECK(folded.items.count("statuscol:folded") == 1);
    CHECK(has(folded, "statuscol:column"));
    CHECK(has(folded, "statuskey:unfold"));
    if (has(folded, "statuscol:column")) {
        CHECK_NEAR(folded.rects.at("statuscol:column").x1 - folded.rects.at("statuscol:column").x0,
                   g::kStatusFoldedW, 1.0);
        CHECK(fv.x1 < ov.x1);
    }
}

}  // namespace

int main() {
    std::printf("test_status_fold_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-status-fold-" + std::to_string(pid()));
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
    atNarrow();

    const int rc = testSummary("test_status_fold_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
