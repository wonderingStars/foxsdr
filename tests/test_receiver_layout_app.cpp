/*
 * THE RECEIVER'S WATERFALL ENDS WHERE ITS VIEW DOES. drawCenterPanels sized
 * the spectrum, the splitter and the waterfall to add up to the height
 * available, but ImGui puts ItemSpacing between the three, so the waterfall
 * was laid out about 12 px past the bottom of the view it sits in (1600x900:
 * view to y=900.0, waterfall to y=911.8) and its last rows - with the lower
 * edge of its SCROLL/decode plate - were drawn out of sight under the
 * cabinet. Found 2026-09-30 while putting marker tabs along the waterfall's
 * foot.
 *
 * This runs cascade itself and reads the UI census: at two window sizes, and
 * after the splitter has been dragged, the waterfall's rectangle
 * ("fmk:wf:receiver") must end on the receiver view's bottom edge - inside it,
 * and not short of it either, since the panels are meant to fill the view.
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

// An EMPTY value unsets the variable (see test_main_view for why).
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
};

struct Census {
    bool ok = false;
    std::map<std::string, Rect> rects;
};

fs::path g_dir;

Census once(const std::string& tag, const std::string& size, const std::string& script) {
    Census c;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    const fs::path scriptFile = g_dir / (tag + ".script");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false }\n";
    }
    {
        std::ofstream f(scriptFile, std::ios::binary | std::ios::trunc);
        f << script;
    }
    constexpr int kFrames = 45;
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", size);
    setEnv("FOXSDR_INPUT_SCRIPT", script.empty() ? "" : scriptFile.string());
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_PATCH_START", "");
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    if (out.find("rendered " + std::to_string(kFrames) + " frames") == std::string::npos ||
        out.find("ui census written") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string name;
        Rect rc;
        ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
        c.rects[name] = rc;
    }
    c.ok = c.rects.count("view:receiver") == 1 && c.rects.count("fmk:wf:receiver") == 1 &&
           c.rects.count("rx:splitter") == 1;
    if (!c.ok) { std::printf("  %s: no receiver view or waterfall in the census\n", tag.c_str()); }
    return c;
}

// The waterfall ends on the view's bottom edge: not past it (hidden rows),
// not more than a pixel short of it (a gap the panels should fill).
bool endsOnViewBottom(const Census& c, const char* tag) {
    const Rect& v = c.rects.at("view:receiver");
    const Rect& w = c.rects.at("fmk:wf:receiver");
    std::printf("    %s: view to y=%.2f, waterfall %.2f..%.2f\n", tag, v.y1, w.y0, w.y1);
    return w.y1 <= v.y1 + 0.5f && w.y1 >= v.y1 - 1.0f && w.y0 >= v.y0;
}

}  // namespace

int main() {
    std::printf("test_receiver_layout_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-rxlayout-" + std::to_string(pid()));
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

    const Census big = once("1600x900", "1600x900", "");
    CHECK(big.ok);
    if (big.ok) { CHECK(endsOnViewBottom(big, "1600x900")); }

    const Census small = once("1280x720", "1280x720", "");
    CHECK(small.ok);
    if (small.ok) { CHECK(endsOnViewBottom(small, "1280x720")); }

    // THE SPLITTER STILL DRAGS, and the waterfall still fits afterwards: grab
    // the splitter's middle and pull it up 120 px.
    if (big.ok) {
        const Rect& w = big.rects.at("fmk:wf:receiver");
        const Rect& sp = big.rects.at("rx:splitter");
        const float gx = 0.5f * (sp.x0 + sp.x1);
        const float gy = 0.5f * (sp.y0 + sp.y1);
        // Spectrum, gap, splitter, gap, waterfall: the splitter sits between
        // the other two, clear of both.
        CHECK(sp.y1 < w.y0);
        std::ostringstream s;
        s << "15 screen " << gx << ' ' << gy << "\n"
          << "17 down\n";
        for (int i = 1; i <= 6; ++i) { s << 17 + i << " screen " << gx << ' ' << gy - 20.0f * i << "\n"; }
        s << "25 up\n";
        const Census dragged = once("dragged", "1600x900", s.str());
        CHECK(dragged.ok);
        if (dragged.ok) {
            const Rect& dw = dragged.rects.at("fmk:wf:receiver");
            std::printf("    dragged: waterfall top %.2f -> %.2f\n", w.y0, dw.y0);
            CHECK(dw.y0 < w.y0 - 60.0f);  // the drag moved the split
            CHECK(endsOnViewBottom(dragged, "dragged"));
        }
    }

    const int rc = testSummary("test_receiver_layout_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
