/*
 * THE INTERFACE SIZE REACHES THE RAIL, IN THE REAL APPLICATION (0.99.44, a
 * beta tester on a 4K monitor: "The fonts are too small").
 *
 * gui/ui_scale.* holds the arithmetic and tests/test_ui_scale.cpp pins it; what
 * no test could see is whether the WINDOW's geometry goes through it. Every
 * hard-coded layout pixel in the window is a multiply by a float that is 1.0f
 * at 100%, so a site that forgot the multiply is bit-identical to a site that
 * did it, at the only size any other test runs. This test runs cascade itself
 * at 100% and at 150% in a window big enough for both and checks, from the UI
 * census it writes:
 *
 *   rail      the FUNCTION SELECT column is 1.5 times as wide, and so is each
 *             of the five bank keys and the two view keys, their pitch, and
 *             the height they are drawn at;
 *   deck      the STOP dome is 1.5 times the size;
 *   nothing   draws over anything else at 150% that it did not draw over at
 *             100%, nothing leaves the window, and the patch view starts to
 *             the right of the widened rail;
 *   config    the choice is the window's half of the config: "150" in the
 *             saved config opens at 150% with no environment override and is
 *             written back as "150"; FOXSDR_UI_SCALE is the capture seam that
 *             overrides it for one run and is never saved.
 *
 * Isolated from the real config like every test that starts the application.
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
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

struct Result {
    bool ok = false;
    std::map<std::string, Rect> rects;
    std::string savedScale;  // AppConfig::interfaceScale as the run saved it
    std::string diag;        // the run's own diagnostics log
};

constexpr int kFrames = 70;
constexpr const char* kSize = "2560x1440";
fs::path g_dir;

// `envScale` empty: FOXSDR_UI_SCALE is not set and the config decides. `windowPos` empty:
// FOXSDR_WINDOW_POS is not set.
Result once(const std::string& tag, const std::string& envScale, const std::string& cfgScale,
            const std::string& windowPos = "", const std::string& extraConfig = "") {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \""
          << cfgScale << "\"" << extraConfig << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", kSize);
    setEnv("FOXSDR_UI_SCALE", envScale);
    setEnv("FOXSDR_INPUT_SCRIPT", "");
    setEnv("FOXSDR_WINDOW_POS", windowPos);
    // A diagnostics folder of its own per run, so the log read back is this run's.
    const fs::path diagDir = g_dir / ("diag-" + tag);
    fs::create_directories(diagDir);
    setEnv("FOXSDR_DIAG_DIR", diagDir.string());
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
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
    r.savedScale = saved.interfaceScale;
    {
        std::ifstream in(diagDir / "logs" / "foxsdr.log");
        std::ostringstream all;
        all << in.rdbuf();
        r.diag = all.str();
    }
    r.ok = true;
    return r;
}

bool has(const Result& r, const std::string& name) { return r.rects.count(name) == 1; }

bool overlap(const Rect& a, const Rect& b) {
    // Half a pixel of slack: rects that touch at an edge are not drawn over each other.
    return a.x0 < b.x1 - 0.5f && b.x0 < a.x1 - 0.5f && a.y0 < b.y1 - 0.5f && b.y0 < a.y1 - 0.5f;
}

// The ratio of one measure at 150% to the same at 100%, which must be 1.5. A measure that is a
// GAP or an INSET (a few pixels between two big things) is compared with a tight tolerance:
// the width of a key is mostly the column it shares, so a gap left unscaled moves a key's width
// by only a percent and a half and would pass the loose one (found by a break-it run).
void scaled(const char* what, float at100, float at150, double tol = 0.03) {
    const double ratio = at100 > 0.0f ? static_cast<double>(at150) / static_cast<double>(at100) : 0.0;
    if (std::fabs(ratio - 1.5) > tol) {
        std::printf("    %s: %.2f at 100%%, %.2f at 150%% (x%.3f, wanted x1.5)\n", what, at100,
                    at150, ratio);
    }
    CHECK_NEAR(ratio, 1.5, tol);
}

}  // namespace

int main() {
    std::printf("test_ui_scale_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-ui-scale-" + std::to_string(pid()));
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

    const Result a = once("s100", "100", "100");
    const Result b = once("s150", "150", "100");
    CHECK(a.ok);
    CHECK(b.ok);
    if (!a.ok || !b.ok) { return testSummary("test_ui_scale_app"); }

    // --- the rail and the keys under its title -------------------------------------
    std::printf("  at 100%% and 150%% in a %s window\n", kSize);
    for (int i = 0; i < 5; ++i) {
        const std::string k = "bank:" + std::to_string(i);
        CHECK(has(a, k) && has(b, k));
        if (!has(a, k) || !has(b, k)) { continue; }
        scaled((k + " width").c_str(), a.rects.at(k).w(), b.rects.at(k).w());
        scaled((k + " height").c_str(), a.rects.at(k).h(), b.rects.at(k).h());
    }
    CHECK(has(a, "bank:0") && has(a, "bank:1") && has(b, "bank:0") && has(b, "bank:1"));
    if (has(b, "bank:0") && has(b, "bank:1")) {
        scaled("bank key pitch", a.rects.at("bank:1").x0 - a.rects.at("bank:0").x0,
               b.rects.at("bank:1").x0 - b.rects.at("bank:0").x0);
    }
    // The spacing between and around the keys, each exactly 1.5 times (tight tolerance).
    CHECK(has(a, "rail:column") && has(b, "rail:column") && has(a, "viewkey:0") && has(b, "viewkey:0"));
    if (has(b, "rail:column") && has(b, "bank:0") && has(b, "bank:1") && has(b, "viewkey:1")) {
        scaled("gap between bank keys", a.rects.at("bank:1").x0 - a.rects.at("bank:0").x1,
               b.rects.at("bank:1").x0 - b.rects.at("bank:0").x1, 0.02);
        scaled("inset of the bank keys", a.rects.at("bank:0").x0 - a.rects.at("rail:column").x0,
               b.rects.at("bank:0").x0 - b.rects.at("rail:column").x0, 0.02);
        scaled("bank keys to view keys", a.rects.at("viewkey:0").y0 - a.rects.at("bank:0").y1,
               b.rects.at("viewkey:0").y0 - b.rects.at("bank:0").y1, 0.02);
        scaled("inset of the view keys", a.rects.at("viewkey:0").x0 - a.rects.at("rail:column").x0,
               b.rects.at("viewkey:0").x0 - b.rects.at("rail:column").x0, 0.02);
        scaled("gap between view keys", a.rects.at("viewkey:1").x0 - a.rects.at("viewkey:0").x1,
               b.rects.at("viewkey:1").x0 - b.rects.at("viewkey:0").x1, 0.02);
    }
    if (has(a, "railkey:fold") && has(b, "railkey:fold")) {
        scaled("rail fold key width", a.rects.at("railkey:fold").w(), b.rects.at("railkey:fold").w(), 0.02);
        scaled("rail fold key height", a.rects.at("railkey:fold").h(), b.rects.at("railkey:fold").h(), 0.02);
        scaled("rail fold key inset from the column's right edge",
               a.rects.at("rail:column").x1 - a.rects.at("railkey:fold").x1,
               b.rects.at("rail:column").x1 - b.rects.at("railkey:fold").x1, 0.02);
    }
    CHECK(has(a, "railkey:fold") && has(b, "railkey:fold"));
    for (int i = 0; i < 2; ++i) {
        const std::string k = "viewkey:" + std::to_string(i);
        CHECK(has(a, k) && has(b, k));
        if (!has(a, k) || !has(b, k)) { continue; }
        scaled((k + " width").c_str(), a.rects.at(k).w(), b.rects.at(k).w());
        scaled((k + " height").c_str(), a.rects.at(k).h(), b.rects.at(k).h());
    }
    CHECK(has(a, "rail:column") && has(b, "rail:column"));
    if (has(a, "rail:column") && has(b, "rail:column")) {
        CHECK_NEAR(a.rects.at("rail:column").w(), cascade::gui::kMenuWidth, 1.0);
        CHECK_NEAR(b.rects.at("rail:column").w(), cascade::gui::kMenuWidth * 1.5, 1.5);
    }

    // --- the deck's STOP dome ---------------------------------------------------------
    CHECK(has(a, "deck:stop") && has(b, "deck:stop"));
    if (has(a, "deck:stop") && has(b, "deck:stop")) {
        scaled("deck:stop width", a.rects.at("deck:stop").w(), b.rects.at("deck:stop").w());
    }

    // --- the receiver view's STATUS column and its fold key ------------------------------
    {
        const Result ra = once("r100", "100", "100", "", ", \"mainView\": \"receiver\"");
        const Result rb = once("r150", "150", "100", "", ", \"mainView\": \"receiver\"");
        CHECK(ra.ok && rb.ok);
        CHECK(has(ra, "statuscol:column") && has(rb, "statuscol:column"));
        CHECK(has(ra, "statuskey:fold") && has(rb, "statuskey:fold"));
        if (has(ra, "statuscol:column") && has(rb, "statuscol:column") && has(ra, "statuskey:fold") &&
            has(rb, "statuskey:fold")) {
            CHECK_NEAR(ra.rects.at("statuscol:column").w(), cascade::gui::kStatusW, 1.0);
            scaled("status column width", ra.rects.at("statuscol:column").w(),
                   rb.rects.at("statuscol:column").w(), 0.02);
            scaled("status fold key width", ra.rects.at("statuskey:fold").w(),
                   rb.rects.at("statuskey:fold").w(), 0.02);
            scaled("status fold key inset from the column's left edge",
                   ra.rects.at("statuskey:fold").x0 - ra.rects.at("statuscol:column").x0,
                   rb.rects.at("statuskey:fold").x0 - rb.rects.at("statuscol:column").x0, 0.02);
        }
        // The receiver's view and the column beside it share the window without a gap closing
        // or an overlap: the view ends where the column begins, at either size.
        if (has(rb, "view:receiver") && has(rb, "statuscol:column")) {
            CHECK(rb.rects.at("view:receiver").x1 <= rb.rects.at("statuscol:column").x0 + 0.5f);
        }
    }

    // --- nothing draws over anything it did not draw over at 100%, nothing leaves ----
    const std::vector<std::string> names = [&] {
        std::vector<std::string> v;
        for (const auto& kv : b.rects) {
            const std::string& n = kv.first;
            if (n.rfind("bank:", 0) == 0 || n.rfind("viewkey:", 0) == 0 ||
                n.rfind("deck:", 0) == 0 || n.rfind("patchpart:", 0) == 0 ||
                n.rfind("railkey:", 0) == 0) {
                // deck:bar and deck:mute.clip are containers, not parts.
                if (n == "deck:bar" || n == "deck:mute.clip") { continue; }
                v.push_back(n);
            }
        }
        return v;
    }();
    CHECK(names.size() >= 20);
    int drawnOver = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            const std::string& p = names[i];
            const std::string& q = names[j];
            if (!overlap(b.rects.at(p), b.rects.at(q))) { continue; }
            if (has(a, p) && has(a, q) && overlap(a.rects.at(p), a.rects.at(q))) { continue; }
            std::printf("    %s is drawn over %s at 150%% and was not at 100%%\n", p.c_str(),
                        q.c_str());
            ++drawnOver;
        }
    }
    CHECK(drawnOver == 0);
    for (const std::string& n : names) {
        const Rect& r = b.rects.at(n);
        CHECK(r.x0 >= 0.0f && r.y0 >= 0.0f && r.x1 <= 2560.5f && r.y1 <= 1440.5f);
    }
    // The view keys sit under the bank keys; the patch view starts right of the rail.
    if (has(b, "bank:0") && has(b, "viewkey:0")) {
        CHECK(b.rects.at("viewkey:0").y0 >= b.rects.at("bank:0").y1);
    }
    if (has(b, "rail:column") && has(b, "view:patch")) {
        CHECK(b.rects.at("view:patch").x0 >= b.rects.at("rail:column").x1);
        if (has(a, "rail:column") && has(a, "view:patch")) {
            // The gap between the rail and the view is ImGui's item spacing, which
            // the style scales with everything else: it grows by at most S and
            // never shrinks. (It is the GAP that is compared, not the view's left
            // edge: the cabinet's margin outside the rail grows with S too.)
            const float gap100 = a.rects.at("view:patch").x0 - a.rects.at("rail:column").x1;
            const float gap150 = b.rects.at("view:patch").x0 - b.rects.at("rail:column").x1;
            CHECK(gap150 >= gap100 - 0.5f);
            CHECK(gap150 <= gap100 * 1.5f + 1.0f);
            // ...and the rail starts one cabinet margin in, a margin that scales too.
            CHECK(b.rects.at("rail:column").x0 >= a.rects.at("rail:column").x0);
        }
    }

    // --- the config is the window's half: opens at the saved size, writes it back ------
    const Result c = once("cfg150", "", "150");
    CHECK(c.ok);
    if (c.ok) {
        CHECK(c.savedScale == "150");
        CHECK(has(c, "bank:0") && has(b, "bank:0"));
        if (has(c, "bank:0") && has(b, "bank:0")) {
            CHECK_NEAR(c.rects.at("bank:0").w(), b.rects.at("bank:0").w(), 0.5);
        }
    }
    // The env seam is for one run: the saved "100" survives a run that asked for 150.
    CHECK(b.savedScale == "100");

    // --- the capture seam FOXSDR_WINDOW_POS is READ (the pending gui tests set it) ------
    // The application logs where the window ended up once its caption is removed:
    // "frame: caption removed; window X,Y WxH client WxH at CX,CY". With the seam set the
    // client area is within a window frame's width of the asked-for position; without it
    // the window is wherever the window manager put it, which is not that.
    {
        const Result p = once("pos", "100", "100", "310,260");
        CHECK(p.ok);
        int cx = -1000, cy = -1000;
        const std::size_t at = p.diag.find("caption removed;");
        if (at != std::string::npos) {
            const std::size_t atc = p.diag.find(" at ", at);
            if (atc != std::string::npos) {
                std::sscanf(p.diag.c_str() + atc, " at %d,%d", &cx, &cy);
            }
        }
        std::printf("    FOXSDR_WINDOW_POS=310,260: client area at %d,%d\n", cx, cy);
        CHECK(std::abs(cx - 310) <= 16);
        CHECK(std::abs(cy - 260) <= 48);
        // ...and the 100% run above, with no seam, is not there.
        int ax = -1000, ay = -1000;
        const std::size_t at0 = a.diag.find("caption removed;");
        if (at0 != std::string::npos) {
            const std::size_t atc = a.diag.find(" at ", at0);
            if (atc != std::string::npos) {
                std::sscanf(a.diag.c_str() + atc, " at %d,%d", &ax, &ay);
            }
        }
        CHECK(std::abs(ax - 310) > 16 || std::abs(ay - 260) > 48);
    }

    const int rc = testSummary("test_ui_scale_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
