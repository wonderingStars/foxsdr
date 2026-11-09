/*
 * A BAND SWITCH MOVES THE PICTURE AND THE MODE, THROUGH THE APPLICATION (a beta tester, an HF operator, 2026-11-09): "if I am on the
 * 7 MHz band and switch to the 3.5 MHz band, the waterfall display remains on 7 MHz - I am forced to move the waterfall with the mouse;
 * the transition should happen automatically. Also, when switching bands the LSB/USB mode should be set automatically: 7 MHz should show
 * LSB, 14 MHz USB; the operator stays free to change the mode for that band afterward." This runs cascade on the signal generator (a 2 MS/s
 * baseband, so the retunes are a miniature of the report: the zoom width and the band edges are what matter, not the MHz) with the
 * application's own input script, and reads back the UI census (the picture as it was last drawn, rx:view:khz) and the config the run
 * saved (the mode):
 *
 *   recentre   zoomed about the tuned 7.100 MHz (10 notches: a 145 kHz picture) and then typing 7.900 MHz - a retune the new baseband
 *              (6.9 to 8.9 MHz) still holds the old picture in, so nothing used to move it - lands the picture on 7.900 MHz at the same
 *              width (core/view_follow.hpp; the rule is pinned without a window in tests/test_view_follow.cpp);
 *   switch     the report's own words: zoomed on 7.100 MHz (40 m, USB set by hand), typing 3.620 MHz lands in LSB (80 m's convention) with
 *              the picture on 3.620 MHz, still 145 kHz wide;
 *   click      a click on the spectrum in another band (7.300 MHz, the 41 m broadcast band) adopts that band's mode (AM) - the path the
 *              tester takes after panning the waterfall by hand, which used to leave the mode alone;
 *   same band  typing 7.040 MHz from 7.100 MHz in USB leaves USB alone (an FT8 setup survives a retype within the band);
 *   off        with "Follow the band plan's mode when retuning" off (bandPlanFollowMode false in the settings), typing 3.620 MHz leaves USB.
 *
 * The band plan is the UK's (bandPlanSelection "uk"): the default WORLD plan carries no 80 m or 40 m entry (the three ITU regions disagree
 * about their edges), so on it no mode can follow a switch between them - a run on the world plan is printed for the record, not checked.
 *
 * Isolated like every test that starts the application: a private APPDATA, telemetry points at a black hole.
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
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Census {
    bool ok = false;
    std::map<std::string, Rect> rects;
    cascade::core::AppConfig saved;
    bool hasRect(const std::string& k) const { return rects.count(k) == 1; }
    Rect rect(const std::string& k) const {
        const auto it = rects.find(k);
        return it == rects.end() ? Rect{} : it->second;
    }
};

// What the picture looked like on the LAST frame: [low, high] and the passband's centre, in kHz from the source's centre, and the
// passband's width (the same census line tests/test_passband_slide_app.cpp reads).
struct View {
    bool drawn = false;
    double lowKhz = 0, highKhz = 0, tunedKhz = 0, bandKhz = 0;
    double spanKhz() const { return highKhz - lowKhz; }
    bool containsBand() const {
        return tunedKhz - 0.5 * bandKhz >= lowKhz - 0.01 && tunedKhz + 0.5 * bandKhz <= highKhz + 0.01;
    }
};
View viewOf(const Census& c) {
    View v;
    if (!c.hasRect("rx:view:khz")) { return v; }
    const Rect r = c.rect("rx:view:khz");
    v.drawn = true;
    v.lowKhz = r.x0;
    v.highKhz = r.y0;
    v.tunedKhz = r.x1;
    v.bandKhz = r.y1;
    return v;
}

fs::path g_dir;

// THE RECEIVER IS A USB LISTENER ON THE 2 MS/s GENERATOR AT 7.100 MHz (the 40 m band of the UK plan), the VFO on the centre.
constexpr double kCentreHz = 7100000.0;
constexpr double kRateHz = 2000000.0;
constexpr double kZoomPerNotch = 1.3;
constexpr int kZoomNotches = 10;
const double kZoomedSpanKhz = (kRateHz / 1000.0) / std::pow(kZoomPerNotch, kZoomNotches);   // 145.1 kHz

struct Run {
    std::string tag;
    const char* mode = "USB";
    std::string script;
    int frames = 90;
    const char* plan = "uk";
    const char* extraJson = "";   // more settings, each ", \"key\": value"
};

Census once(const Run& r) {
    Census c;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path scriptFile = g_dir / (r.tag + ".script");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false, \"bandPlanSelection\": \""
          << r.plan << "\", \"mode\": \"" << r.mode << "\", \"bandwidthHz\": 3000, \"centerHz\": " << static_cast<long long>(kCentreHz)
          << ", \"sampleRateHz\": " << static_cast<long long>(kRateHz) << ", \"vfoOffsetHz\": 0" << r.extraJson << " }\n";
    }
    {
        std::ofstream f(scriptFile, std::ios::binary | std::ios::trunc);
        f << r.script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1600x900");
    setEnv("FOXSDR_INPUT_SCRIPT", r.script.empty() ? "" : scriptFile.string());
    setEnv("FOXSDR_SCRIPT_TRACE", r.script.empty() ? "" : (g_dir / (r.tag + ".trace")).string());
    setEnv("CASCADE_DECODE_TEST", "1");
    const std::string out = run("\"" + exePath() + "\" --frames " + std::to_string(r.frames) + " 2>&1");
    if (out.find("rendered " + std::to_string(r.frames) + " frames") == std::string::npos || out.find("ui census written") == std::string::npos) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), out.c_str());
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
    std::string err;
    if (!cascade::core::ConfigStore::load(cfg.string(), c.saved, err)) {
        std::printf("  %s: the saved config did not load: %s\n", r.tag.c_str(), err.c_str());
        return c;
    }
    c.ok = true;
    return c;
}

std::string at(int f, float x, float y) {
    std::ostringstream s;
    s << f << " screen " << x << ' ' << y << '\n';
    return s.str();
}
std::string wheel(int f, float n) { return std::to_string(f) + " wheel " + std::to_string(n) + "\n"; }
std::string click(int f) { return std::to_string(f) + " down\n" + std::to_string(f + 2) + " up\n"; }
// Click a figure of the counter (it opens the typed editor, seeded and all selected), type, Enter.
std::string typeFrequency(int f, const Rect& tube, const char* text) {
    std::ostringstream s;
    s << f << " screen " << tube.cx() << ' ' << tube.cy() << "\n" << f + 2 << " down\n" << f + 4 << " up\n" << f + 10 << " text " << text << "\n"
      << f + 14 << " key enter\n";
    return s.str();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_band_follow_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-bandfollow-" + std::to_string(pid()));
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
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");

    // --- the counter's figures and the spectrum, found; a run that does nothing shows the whole span on the band -----------------------------
    const Census learn = once({"learn", "USB", "", 60});
    CHECK(learn.ok && learn.hasRect("cnt:tube:0") && learn.hasRect("trc:spec:receiver"));
    if (!learn.ok || !learn.hasRect("cnt:tube:0") || !learn.hasRect("trc:spec:receiver")) { return testSummary("test_band_follow_app"); }
    const Rect tube = learn.rect("cnt:tube:0");
    const Rect sp = learn.rect("trc:spec:receiver");
    const View lv = viewOf(learn);
    std::printf("    spectrum %.0f,%.0f..%.0f,%.0f; picture %.1f..%.1f kHz, band at %.1f kHz; mode %s\n", sp.x0, sp.y0, sp.x1, sp.y1, lv.lowKhz,
                lv.highKhz, lv.tunedKhz, learn.saved.mode.c_str());
    CHECK(lv.drawn && lv.containsBand());
    CHECK_NEAR(lv.spanKhz(), kRateHz / 1000.0, 1.0);
    CHECK(learn.saved.mode == "USB");
    // Where a frequency sits on the spectrum while the whole span is showing, and a height off the tuned band.
    const auto xAtOffset = [&](double offsetHz) { return static_cast<float>(sp.x0 + (0.5 + offsetHz / kRateHz) * (sp.x1 - sp.x0)); };
    const float sy = sp.y0 + 0.30f * (sp.y1 - sp.y0);
    // Ten notches of the wheel over the tuned band: a 145 kHz picture about 7.100 MHz.
    const std::string zoomed = at(20, xAtOffset(0.0), sy) + wheel(24, static_cast<float>(kZoomNotches));

    // --- RECENTRE: zoomed, then a retune the new baseband still holds the old picture in -------------------------------------------------------
    {
        const Census z = once({"zoomed", "USB", zoomed, 60});
        const View zv = viewOf(z);
        std::printf("    zoomed %d notches: picture %.1f..%.1f kHz (span %.1f)\n", kZoomNotches, zv.lowKhz, zv.highKhz, zv.spanKhz());
        CHECK(z.ok && zv.drawn && zv.containsBand());
        CHECK_NEAR(zv.spanKhz(), kZoomedSpanKhz, 1.0);

        const Census c = once({"recentre", "USB", zoomed + typeFrequency(40, tube, "7.900")});
        const View v = viewOf(c);
        std::printf("    ...then typed 7.900 MHz: tuned %.0f Hz; picture %.1f..%.1f kHz from the new centre (span %.1f), band at %.1f kHz; mode %s\n",
                    c.saved.centerHz + c.saved.vfoOffsetHz, v.lowKhz, v.highKhz, v.spanKhz(), v.tunedKhz, c.saved.mode.c_str());
        CHECK(c.ok && v.drawn);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 7900000.0, 1.0);   // the tune happened
        CHECK(v.containsBand());                                              // the picture went with it
        CHECK_NEAR(v.spanKhz(), kZoomedSpanKhz, 1.0);                         // at the same width
        CHECK(c.saved.mode == "USB");                                         // 7.9 MHz is in no band of the plan: the mode is left alone
    }

    // --- THE SWITCH THE TESTER DESCRIBED: 7 MHz to the 3.5 MHz band, zoomed, in USB ----------------------------------------------------------
    {
        const Census c = once({"switch", "USB", zoomed + typeFrequency(40, tube, "3.620")});
        const View v = viewOf(c);
        std::printf("    zoomed on 7.100 MHz in USB, typed 3.620 MHz: tuned %.0f Hz; picture %.1f..%.1f kHz (span %.1f), band at %.1f kHz; mode %s\n",
                    c.saved.centerHz + c.saved.vfoOffsetHz, v.lowKhz, v.highKhz, v.spanKhz(), v.tunedKhz, c.saved.mode.c_str());
        CHECK(c.ok && v.drawn);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 3620000.0, 1.0);
        CHECK(v.containsBand());
        CHECK_NEAR(v.spanKhz(), kZoomedSpanKhz, 1.0);
        CHECK(c.saved.mode == "LSB");
    }

    // --- CLICK: a click on the spectrum in another band takes that band's mode --------------------------------------------------------------
    {
        // +200 kHz from the centre: 7.300 MHz, inside the 41 m broadcast band (7.2 to 7.45 MHz), whose convention is AM.
        const Census c = once({"click", "USB", at(20, xAtOffset(200000.0), sy) + click(24), 70});
        std::printf("    clicked at 7.300 MHz from 7.100 MHz in USB: tuned %.0f Hz; mode %s\n", c.saved.centerHz + c.saved.vfoOffsetHz, c.saved.mode.c_str());
        CHECK(c.ok);
        CHECK(c.saved.centerHz + c.saved.vfoOffsetHz > 7250000.0 && c.saved.centerHz + c.saved.vfoOffsetHz < 7400000.0);
        CHECK(c.saved.mode == "AM");
    }

    // --- THE SAME BAND: the mode is the person's --------------------------------------------------------------------------------------------
    {
        const Census c = once({"same", "USB", typeFrequency(20, tube, "7.040"), 70});
        std::printf("    typed 7.040 MHz from 7.100 MHz in USB: tuned %.0f Hz; mode %s\n", c.saved.centerHz + c.saved.vfoOffsetHz, c.saved.mode.c_str());
        CHECK(c.ok);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 7040000.0, 1.0);
        CHECK(c.saved.mode == "USB");
    }

    // --- FOLLOW OFF: the setting holds the mode still across a switch ------------------------------------------------------------------------
    {
        const Census c = once({"off", "USB", typeFrequency(20, tube, "3.620"), 70, "uk", ", \"bandPlanFollowMode\": false"});
        std::printf("    follow off, typed 3.620 MHz from 7.100 MHz in USB: tuned %.0f Hz; mode %s; saved bandPlanFollowMode %d\n",
                    c.saved.centerHz + c.saved.vfoOffsetHz, c.saved.mode.c_str(), c.saved.bandPlanFollowMode ? 1 : 0);
        CHECK(c.ok);
        CHECK_NEAR(c.saved.centerHz + c.saved.vfoOffsetHz, 3620000.0, 1.0);
        CHECK(c.saved.mode == "USB");
        CHECK(!c.saved.bandPlanFollowMode);   // and the setting survives the run
    }

    // --- FOR THE RECORD: the default WORLD plan has no 80 m or 40 m entry, so no mode can follow this switch on it ------------------------------
    {
        const Census c = once({"world", "USB", typeFrequency(20, tube, "3.620"), 70, "world"});
        std::printf("    (world plan) typed 3.620 MHz from 7.100 MHz in USB: tuned %.0f Hz; mode %s - not checked\n",
                    c.saved.centerHz + c.saved.vfoOffsetHz, c.saved.mode.c_str());
        CHECK(c.ok);
    }

    const int rc = testSummary("test_band_follow_app");
    if (rc == 0) {
        fs::remove_all(g_dir, ec);
    } else {
        std::printf("  kept for inspection: %s\n", g_dir.string().c_str());
    }
    return rc;
}
