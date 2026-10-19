/*
 * SLIDING THE PASSBAND WITH SHIFT + WHEEL, THROUGH THE APPLICATION (0.99.82). A Linux user on
 * 0.99.81 asked, on foxsdr.com, for three things: the wheel with a modifier over the spectrum or the
 * waterfall to slide the demodulator's passband along the band; a double-click on a signal to tune
 * to it; and the waterfall to follow the passband as it moves. This runs cascade with the
 * application's own input script (the `shiftwheel` verb is the wheel with Shift held, three frames,
 * as the pinch is) on the signal generator, and reads back the UI census and the config the run
 * saved:
 *
 *   slide     Shift + wheel over the spectrum, or over the waterfall, steps the tuned frequency by
 *             the mode's raster (USB: 1 kHz) per notch - up for a wheel away from the user - and
 *             lands on the raster; a run of notches is one move;
 *   plain     the wheel without Shift is still the zoom (the offset is left alone, the span
 *             narrows) and Ctrl + wheel, the touchpad's pinch, still zooms;
 *   carry     a slide that runs past the end of what the source delivers moves the SOURCE (the same
 *             retune the counter uses) and leaves the passband at the end of the span;
 *   follow    a slide that takes the passband out of a zoomed picture pans the picture after it,
 *             keeping the zoom; so does one that carries the source;
 *   truce     a slide during a drag of the band is ignored: the hand is the only writer;
 *   double    a double-click on a signal tunes to it on the mode's raster (it always did, with the
 *             first of its two clicks). On the WATERFALL it then unzooms, as the tip says. On the
 *             SPECTRUM it does not: the first click puts the band under the pointer, so the second
 *             press lands on the band and starts a drag instead of a double-click (a finding of
 *             0.99.82, recorded rather than changed; see the report with this change).
 *
 * Each run is a real process. The arithmetic is pinned without a window in
 * tests/test_passband_slide.cpp and tests/test_freq_scale.cpp.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <chrono>
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
    cascade::core::AppConfig saved;
    bool hasRect(const std::string& k) const { return rects.count(k) == 1; }
    Rect rect(const std::string& k) const {
        const auto it = rects.find(k);
        return it == rects.end() ? Rect{} : it->second;
    }
};

fs::path g_dir;

// THE RECEIVER IS A USB LISTENER ON A 2 MS/s GENERATOR AT 100 MHz, so a wheel notch is 1 kHz, the
// span is 99.0 to 101.0 MHz, the passband is 3 kHz wide and the VFO's reach is +/- 998.5 kHz.
constexpr double kCentreHz = 100000000.0;
constexpr double kRateHz = 2000000.0;
constexpr double kBandwidthHz = 3000.0;
constexpr double kStepHz = 1000.0;
constexpr double kReachHz = 0.5 * kRateHz - 0.5 * kBandwidthHz;

struct Run {
    std::string tag;
    double offsetHz = 300000.0;   // where the VFO starts, from the centre
    std::string script;
    int frames = 70;
};

Census once(const Run& r) {
    Census c;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path scriptFile = g_dir / (r.tag + ".script");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"bandPlanOverlay\": false, \"mode\": \"USB\", "
             "\"bandwidthHz\": "
          << kBandwidthHz << ", \"centerHz\": " << static_cast<long long>(kCentreHz)
          << ", \"sampleRateHz\": " << static_cast<long long>(kRateHz)
          << ", \"vfoOffsetHz\": " << static_cast<long long>(r.offsetHz) << " }\n";
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
    const auto t0 = std::chrono::steady_clock::now();
    const std::string out =
        run("\"" + exePath() + "\" --frames " + std::to_string(r.frames) + " 2>&1");
    std::printf("  [run %s: %d frames, %.1f s]\n", r.tag.c_str(), r.frames,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (out.find("rendered " + std::to_string(r.frames) + " frames") == std::string::npos ||
        out.find("ui census written") == std::string::npos) {
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
std::string shiftWheel(int f, float n) {
    return std::to_string(f) + " shiftwheel " + std::to_string(n) + "\n";
}
std::string ctrlWheel(int f, float n) {
    return std::to_string(f) + " ctrlwheel " + std::to_string(n) + "\n";
}
// A click: down on frame f, up on f + 2 (the helper the other app tests use).
std::string click(int f) {
    return std::to_string(f) + " down\n" + std::to_string(f + 2) + " up\n";
}
std::string step(const char* what, int f) { return std::to_string(f) + " " + what + "\n"; }

// What the view looked like on the LAST frame: [low, high] and the passband's centre, in kHz from
// the source's centre, and the passband's width.
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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_passband_slide_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-passband-" + std::to_string(pid()));
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

    // --- the panels, found, and the receiver as configured ---------------------------------------
    const Census learn = once({"learn", 300000.0, "", 40});
    CHECK(learn.ok && learn.hasRect("fmk:wf:receiver") && learn.hasRect("trc:spec:receiver"));
    if (!learn.ok || !learn.hasRect("fmk:wf:receiver") || !learn.hasRect("trc:spec:receiver")) {
        return testSummary("test_passband_slide_app");
    }
    const Rect wf = learn.rect("fmk:wf:receiver");
    const Rect sp = learn.rect("trc:spec:receiver");
    std::printf("    waterfall %.0f,%.0f..%.0f,%.0f; spectrum %.0f,%.0f..%.0f,%.0f\n", wf.x0, wf.y0,
                wf.x1, wf.y1, sp.x0, sp.y0, sp.x1, sp.y1);
    CHECK_NEAR(learn.saved.vfoOffsetHz, 300000.0, 0.5);
    CHECK_NEAR(learn.saved.centerHz, kCentreHz, 0.5);
    // Left of the tuned band (60% along the full span), high in each panel.
    const float sx = sp.x0 + 0.20f * (sp.x1 - sp.x0);
    const float sy = sp.y0 + 0.30f * (sp.y1 - sp.y0);
    const float wx = wf.x0 + 0.20f * (wf.x1 - wf.x0);
    const float wy = wf.y0 + 0.30f * (wf.y1 - wf.y0);
    // Where the band is, at a given offset from the centre, when the whole span is showing.
    auto xAtOffset = [&](double offsetHz) {
        return static_cast<float>(sp.x0 + (0.5 + offsetHz / kRateHz) * (sp.x1 - sp.x0));
    };

    // --- SLIDE: three notches up on the spectrum, two down on the waterfall ------------------------
    {
        const Census up = once({"slide-up", 300000.0, at(20, sx, sy) + shiftWheel(24, 3.0f)});
        std::printf("    Shift+wheel +3 on the spectrum: offset %.0f Hz, centre %.0f Hz\n",
                    up.saved.vfoOffsetHz, up.saved.centerHz);
        CHECK(up.ok);
        CHECK_NEAR(up.saved.vfoOffsetHz, 300000.0 + 3.0 * kStepHz, 0.5);
        CHECK_NEAR(up.saved.centerHz, kCentreHz, 0.5);   // inside the span: the source stays put
        const View v = viewOf(up);
        CHECK(v.drawn && v.containsBand());
        CHECK_NEAR(v.spanKhz(), kRateHz / 1000.0, 0.01); // and the picture was not zoomed by it

        const Census down = once({"slide-down", 300000.0, at(20, wx, wy) + shiftWheel(24, -2.0f)});
        std::printf("    Shift+wheel -2 on the waterfall: offset %.0f Hz\n", down.saved.vfoOffsetHz);
        CHECK(down.ok);
        CHECK_NEAR(down.saved.vfoOffsetHz, 300000.0 - 2.0 * kStepHz, 0.5);
        CHECK(viewOf(down).containsBand());
    }

    // --- THE RASTER: an off-raster passband (Shift-click leaves one) lands on the raster -----------
    {
        const Census off = once({"slide-raster", 300400.0, at(20, sx, sy) + shiftWheel(24, 1.0f)});
        std::printf("    from +300400 Hz, one notch up: offset %.0f Hz\n", off.saved.vfoOffsetHz);
        CHECK(off.ok);
        CHECK_NEAR(off.saved.vfoOffsetHz, 301000.0, 0.5);   // the next raster point above
        const Census offDown =
            once({"slide-raster-d", 300400.0, at(20, sx, sy) + shiftWheel(24, -1.0f)});
        CHECK(offDown.ok);
        CHECK_NEAR(offDown.saved.vfoOffsetHz, 300000.0, 0.5);   // and the next below
    }

    // --- PLAIN: the wheel is still the zoom, and so is the pinch ------------------------------------
    {
        const Census zoom = once({"plain-wheel", 300000.0, at(20, sx, sy) + wheel(24, 3.0f)});
        const View v = viewOf(zoom);
        std::printf("    plain wheel +3: offset %.0f Hz, span %.1f kHz\n", zoom.saved.vfoOffsetHz,
                    v.spanKhz());
        CHECK(zoom.ok && v.drawn);
        CHECK_NEAR(zoom.saved.vfoOffsetHz, 300000.0, 0.5);          // not slid
        CHECK_NEAR(v.spanKhz(), (kRateHz / 1000.0) / std::pow(1.3, 3.0), 1.0);   // zoomed
        const Census pinch = once({"plain-pinch", 300000.0, at(20, sx, sy) + ctrlWheel(24, 3.0f)});
        const View p = viewOf(pinch);
        CHECK(pinch.ok && p.drawn);
        CHECK_NEAR(pinch.saved.vfoOffsetHz, 300000.0, 0.5);
        CHECK_NEAR(p.spanKhz(), (kRateHz / 1000.0) / std::pow(1.3, 3.0), 1.0);
    }

    // --- CARRY: past the end of the span the source moves and the passband stays at the end ---------
    {
        const double start = 997000.0;
        const Census carry = once({"carry", start, at(20, sx, sy) + shiftWheel(24, 5.0f)});
        // Tuned 100.997 MHz + 5 kHz; the passband at the end of its reach; the source carries it.
        std::printf("    from +997 kHz, five notches up: offset %.0f Hz, centre %.0f Hz\n",
                    carry.saved.vfoOffsetHz, carry.saved.centerHz);
        CHECK(carry.ok);
        CHECK_NEAR(carry.saved.vfoOffsetHz, kReachHz, 0.5);
        CHECK_NEAR(carry.saved.centerHz + carry.saved.vfoOffsetHz,
                   kCentreHz + start + 5.0 * kStepHz, 0.5);
        CHECK(carry.saved.centerHz > kCentreHz);
        CHECK(viewOf(carry).containsBand());
        // ...and the same going down the other way.
        const Census low = once({"carry-down", -start, at(20, sx, sy) + shiftWheel(24, -5.0f)});
        CHECK(low.ok);
        CHECK_NEAR(low.saved.vfoOffsetHz, -kReachHz, 0.5);
        CHECK_NEAR(low.saved.centerHz + low.saved.vfoOffsetHz,
                   kCentreHz - start - 5.0 * kStepHz, 0.5);
        CHECK(low.saved.centerHz < kCentreHz);
    }

    // --- FOLLOW: zoom in on the band, slide it out of the picture, and the picture goes after it -----
    {
        // Ten notches of zoom about the band itself: a 145 kHz picture with the band in it.
        const float bx = xAtOffset(300000.0);
        const std::string zoomed = at(20, bx, sy) + wheel(24, 10.0f);
        const Census z = once({"zoomed", 300000.0, zoomed, 50});
        const View zv = viewOf(z);
        std::printf("    zoomed 10 notches about the band: picture %.1f..%.1f kHz (span %.1f)\n",
                    zv.lowKhz, zv.highKhz, zv.spanKhz());
        CHECK(z.ok && zv.drawn && zv.containsBand());
        CHECK(zv.spanKhz() < 200.0);

        // 90 notches = 90 kHz, more than the picture's half-width: the band leaves the picture.
        const Census f = once({"follow", 300000.0, zoomed + shiftWheel(30, 90.0f), 90});
        const View fv = viewOf(f);
        std::printf("    ...then Shift+wheel +90: offset %.0f Hz; picture %.1f..%.1f kHz (span %.1f), "
                    "band at %.1f kHz\n",
                    f.saved.vfoOffsetHz, fv.lowKhz, fv.highKhz, fv.spanKhz(), fv.tunedKhz);
        CHECK(f.ok && fv.drawn);
        CHECK_NEAR(f.saved.vfoOffsetHz, 300000.0 + 90.0 * kStepHz, 0.5);
        CHECK(fv.containsBand());                       // the picture followed the passband
        CHECK_NEAR(fv.spanKhz(), zv.spanKhz(), 0.5);    // and kept its zoom
        CHECK(fv.lowKhz > zv.lowKhz + 50.0);            // by panning up the band, not by unzooming

        // The same at the end of the span, where the SOURCE has to be carried as well: the zoom
        // survives the retune and the picture is still on the band.
        const double nearEnd = 990000.0;
        const float ex = xAtOffset(nearEnd);
        const std::string zoomedEnd = at(20, ex, sy) + wheel(24, 10.0f);
        const Census e = once({"follow-carry", nearEnd, zoomedEnd + shiftWheel(30, 30.0f), 90});
        const View ev = viewOf(e);
        std::printf("    zoomed at +990 kHz, Shift+wheel +30: centre %.0f Hz, offset %.0f Hz; picture "
                    "%.1f..%.1f kHz (span %.1f)\n",
                    e.saved.centerHz, e.saved.vfoOffsetHz, ev.lowKhz, ev.highKhz, ev.spanKhz());
        CHECK(e.ok && ev.drawn);
        CHECK(e.saved.centerHz > kCentreHz);
        CHECK_NEAR(e.saved.centerHz + e.saved.vfoOffsetHz, kCentreHz + nearEnd + 30.0 * kStepHz, 0.5);
        CHECK(ev.containsBand());
        CHECK(ev.spanKhz() < 200.0);                    // still zoomed after the source moved
    }

    // --- TRUCE: a slide while the hand has the band is ignored ----------------------------------------
    {
        const float bx = xAtOffset(300000.0);
        const Census held = once({"truce", 300000.0,
                                  at(20, bx, sy) + step("down", 24) + shiftWheel(28, 5.0f) +
                                      step("up", 36)});
        std::printf("    Shift+wheel +5 while dragging the band: offset %.0f Hz\n",
                    held.saved.vfoOffsetHz);
        CHECK(held.ok);
        CHECK_NEAR(held.saved.vfoOffsetHz, 300000.0, 1.0);   // the hand moved nothing, the wheel nothing
    }

    // --- DOUBLE-CLICK: tunes to the signal on the raster, then unzooms --------------------------------
    {
        // 20% along the span is 99.6 MHz, i.e. offset -600 kHz; the pixel is not exactly on a raster
        // point, so the tuned offset is within half a notch of where the pixel is.
        const double frac = (sx - sp.x0) / (sp.x1 - sp.x0);
        const double offsetAtPixel = (frac - 0.5) * kRateHz;
        const Census dbl = once({"double", 300000.0, at(20, sx, sy) + click(24) + click(28), 60});
        std::printf("    double-click at 20%%: offset %.0f Hz (pixel is %.0f Hz)\n",
                    dbl.saved.vfoOffsetHz, offsetAtPixel);
        CHECK(dbl.ok);
        // (Within a notch of the pixel: the census rectangle and the pointer are floats, and the
        // pixel is a quarter of a kHz from a rounding boundary.)
        CHECK_NEAR(dbl.saved.vfoOffsetHz, offsetAtPixel, 1.0 * kStepHz + 1.0);
        CHECK_NEAR(std::fmod(std::fabs(dbl.saved.centerHz + dbl.saved.vfoOffsetHz), kStepHz), 0.0, 0.5);
        // On the waterfall, after a zoom, a double-click tunes to the signal that was clicked AND
        // unzooms. (Through 0.99.81 the second press's release tuned again, to whatever the same
        // pixel meant in the picture the unzoom had just put back: the VFO ended up on another signal
        // altogether, 600 kHz from the one clicked.)
        const float bx = xAtOffset(300000.0);
        const std::string zoomIn = at(20, bx, sy) + wheel(24, 10.0f) + at(30, wx, wy);
        const Census zoomOnly = once({"zoomed-only", 300000.0, zoomIn, 50});
        const View before = viewOf(zoomOnly);
        const double wfFrac = (wx - wf.x0) / (wf.x1 - wf.x0);
        const double clickedKhz = before.lowKhz + wfFrac * before.spanKhz();
        const Census zdbl = once({"double-zoomed", 300000.0, zoomIn + click(34) + click(38), 70});
        const View v = viewOf(zdbl);
        std::printf("    double-click on the zoomed waterfall: signal clicked at %+.1f kHz; span %.1f kHz, "
                    "offset %.0f Hz\n",
                    clickedKhz, v.spanKhz(), zdbl.saved.vfoOffsetHz);
        CHECK(zdbl.ok && v.drawn && zoomOnly.ok && before.drawn);
        CHECK_NEAR(v.spanKhz(), kRateHz / 1000.0, 0.01);
        CHECK_NEAR(zdbl.saved.vfoOffsetHz, clickedKhz * 1000.0, 1.0 * kStepHz + 1.0);
    }

    return testSummary("test_passband_slide_app");
}
