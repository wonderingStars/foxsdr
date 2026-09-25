/*
 * NO THEME HIDES A FUNCTION - proved by running the application once per theme.
 *
 * WHY THIS EXISTS (themes, 2026-09-25). Six themes change the palette, the
 * typefaces, the corner radii, the counter's size and whether its tuner
 * switches are there, and "every control stays" is the owner's rule for all of
 * them. A test of the palette cannot see a rail section that stopped being
 * drawn, a status card skipped because enlarged figures made it too tall, or a
 * meter dropped because an enlarged counter pushed it off the deck - and a
 * screenshot shows any of those as a design choice. So this runs the real
 * application (--frames) once per theme with the interface census on
 * (gui/ui_census.hpp): the rail is walked through its five banks with every
 * section opened, and every function-bearing part - bank keys, rail sections
 * and switch rows, lettered keys, the transport, lamps, counter, volume dial,
 * both meters, every status card - notes itself.
 *
 * TWO CLAIMS, at a 1600 x 1000 window and at the 1280 x 720 a fresh install
 * opens with:
 *   - every theme draws EXACTLY the parts today's bench draws at that size;
 *   - the deck's parts never overlap, and both meters are on it - including
 *     the enlarged counter with its switches, the tallest and widest deck -
 *     and the mute banner, drawn in every run through a test seam, lies whole
 *     on the bar and on none of them.
 *
 * Isolated like every other test that starts the application: its own config
 * (CASCADE_CONFIG_TEST), APPDATA / LOCALAPPDATA / XDG dirs in a scratch folder,
 * and every report URL at a dead port. The signal generator is the source and
 * the receiver is never started.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
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
#define WIN32_LEAN_AND_MEAN
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

// Both copies of the environment: the child inherits the OS block; the CRT copy
// is kept in step so nothing in this process reads a stale value.
void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.c_str());
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
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
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
};

struct Layout {
    const char* name;
    const char* theme;
    const char* face;
    int counterScale;
    bool switches;
    const char* readings;
};

constexpr int kFrames = 40;

Census census(const fs::path& dir, const Layout& l, const char* windowSize) {
    Census c;
    const std::string tag = std::string(l.name) + "-" + windowSize;
    const fs::path cfg = dir / (tag + ".json");
    const fs::path outFile = dir / (tag + ".census");
    {
        std::ofstream f(cfg);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \""
          << l.theme << "\", \"tunerDisplayStyle\": \"" << l.face
          << "\", \"counterScale\": " << l.counterScale
          << ", \"counterSwitches\": " << (l.switches ? "true" : "false")
          << ", \"readingsScale\": " << l.readings << " }\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", outFile.string());
    setEnv("FOXSDR_WINDOW_SIZE", windowSize);
    const std::string out = run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        out.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), out.c_str());
        return c;
    }
    std::ifstream in(outFile);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("item ", 0) == 0) {
            c.items.insert(line.substr(5));
        } else if (line.rfind("rect ", 0) == 0) {
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect r;
            ss >> name >> r.x0 >> r.y0 >> r.x1 >> r.y1;
            c.rects[name] = r;
        }
    }
    c.ok = !c.items.empty();
    return c;
}

bool overlaps(const Rect& a, const Rect& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("cascade-theme-census-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "local", ec);
    fs::create_directories(dir / "diag", ec);
    setEnv("APPDATA", (dir / "appdata").string());
    setEnv("LOCALAPPDATA", (dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (dir / "local").string());
    setEnv("XDG_STATE_HOME", (dir / "local").string());
    setEnv("FOXSDR_DIAG_DIR", (dir / "diag").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_OPEN_SECTIONS", "all");
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    // THE MUTE BANNER, DRAWN IN EVERY RUN. It appears only while a decoder the
    // user kept running holds the sound down, so an ordinary census never sees
    // it - and Bench Classic XL's first cut laid it across the lower half of
    // the enlarged figures at 1600 x 1000 and 1280 x 720 (today's bench put it
    // over its own switch row at 1280). The seam draws it naming this decoder,
    // at the length the real banner has, so it is placed and measured with
    // the deck's other parts.
    //
    // FOUR DECODERS NAMED, not one (repair round 2): the banner's fallback was
    // clipped to its place, and with four names on today's own 1280 x 720 bar
    // the clip took the "Stop plugin" key away altogether. The longest banner
    // is the one that proves the key survives.
    setEnv("FOXSDR_FORCE_MUTE_BANNER",
           "ADS-B Decoder, AIS Decoder, APRS Decoder and Nearby Signal Catch");

    // The six presets as a user who PICKED each one has them (the counter and
    // readings sizes a pick brings), and the two layouts no preset ships but a
    // user can set: the enlarged counter WITH its switches (the tallest,
    // widest deck) and today's counter without them.
    const Layout presets[] = {
        {"today", "today", "nixie", 1, true, "1.0"},
        {"classic-xl", "classic-xl", "nixie", 2, false, "1.4"},
        {"night", "night", "neon", 1, true, "1.0"},
        {"glass", "glass", "plain", 1, true, "1.0"},
        {"daylight", "daylight", "plain", 1, true, "1.0"},
        {"field", "field", "plain", 1, true, "1.0"},
        {"xl-switches", "classic-xl", "nixie", 2, true, "1.4"},
        {"bare", "today", "nixie", 1, false, "1.0"},
        // Every reading enlarged on today's deck - the layout that first showed
        // WEB ACCESS dropping off the status column at 1280 x 720 - and the
        // largest of everything on a theme lettered in another typeface.
        {"big-readings", "today", "nixie", 1, true, "1.4"},
        {"daylight-xl", "daylight", "plain", 2, true, "1.4"},
    };

    // The parts every run must have drawn, whatever the theme.
    std::vector<std::string> required = {"deck:stop",       "deck:lamp0",       "deck:lamp1",
                                         "deck:lamp2",      "deck:lamp3",       "deck:counter",
                                         "deck:volume",     "deck:meter.rate",  "deck:meter.volume",
                                         "deck:mute"};
    for (int b = 0; b < 5; ++b) { required.push_back("bank:" + std::to_string(b)); }
    const char* deckParts[] = {"deck:stop",   "deck:lamp0",   "deck:lamp1",      "deck:lamp2",
                               "deck:lamp3",  "deck:counter", "deck:volume",     "deck:meter.rate",
                               "deck:meter.volume", "deck:mute"};

    for (const char* size : {"1600x1000", "1280x720"}) {
        std::printf("  window %s\n", size);
        const Census base = census(dir, presets[0], size);
        CHECK(base.ok);
        if (!base.ok) { continue; }
        std::size_t sections = 0;
        for (const std::string& s : base.items) {
            if (s.rfind("section:", 0) == 0) { ++sections; }
        }
        std::printf("    today: %zu parts, %zu rail sections\n", base.items.size(), sections);
        // A census that saw only the first bank would be a census of nothing.
        CHECK(sections >= 15);
        for (const Layout& l : presets) {
            const Census c = (&l == &presets[0]) ? base : census(dir, l, size);
            CHECK(c.ok);
            if (!c.ok) { continue; }
            // EVERY PART TODAY'S BENCH DRAWS, AND NOTHING IT DOES NOT.
            for (const std::string& s : base.items) {
                if (c.items.count(s) == 0) {
                    std::printf("    %s is MISSING %s\n", l.name, s.c_str());
                    CHECK(c.items.count(s) == 1);
                }
            }
            for (const std::string& s : c.items) {
                if (base.items.count(s) == 0) {
                    std::printf("    %s draws an extra part %s\n", l.name, s.c_str());
                    CHECK(base.items.count(s) == 1);
                }
            }
            for (const std::string& r : required) {
                if (c.items.count(r) == 0) { std::printf("    %s lacks %s\n", l.name, r.c_str()); }
                CHECK(c.items.count(r) == 1);
            }
            // THE DECK'S PARTS STAND APART - every one of them measured: a part
            // with no rectangle is a failure here, never a pair quietly skipped.
            for (const char* part : deckParts) {
                if (c.rects.count(part) == 0) {
                    std::printf("    %s: %s drew no rectangle\n", l.name, part);
                }
                CHECK(c.rects.count(part) == 1);
            }
            // The banner is on the deck, whole: inside the bar, not clipped by it.
            {
                const auto bar = c.rects.find("deck:bar");
                const auto mute = c.rects.find("deck:mute");
                CHECK(bar != c.rects.end());
                if (bar != c.rects.end() && mute != c.rects.end()) {
                    const Rect& o = bar->second;
                    const Rect& m = mute->second;
                    const bool inside = m.x0 >= o.x0 && m.y0 >= o.y0 && m.x1 <= o.x1 && m.y1 <= o.y1;
                    if (!inside) {
                        std::printf("    %s: the mute banner (%.0f,%.0f)-(%.0f,%.0f) leaves the bar "
                                    "(%.0f,%.0f)-(%.0f,%.0f)\n",
                                    l.name, m.x0, m.y0, m.x1, m.y1, o.x0, o.y0, o.x1, o.y1);
                    }
                    CHECK(inside);
                }
            }
            // THE KEY IS WHOLE, READABLE AND ON NOTHING. Measured from the key
            // ImGui actually laid out, against the clip the banner is drawn
            // under: a key outside its clip is a key partly or wholly cut away,
            // and a cut-away ImGui item cannot be clicked.
            {
                const auto key = c.rects.find("deck:mute.key");
                const auto clip = c.rects.find("deck:mute.clip");
                const auto bar = c.rects.find("deck:bar");
                CHECK(key != c.rects.end());
                CHECK(clip != c.rects.end());
                if (key != c.rects.end() && clip != c.rects.end() && bar != c.rects.end()) {
                    const Rect& k = key->second;
                    const Rect& o = clip->second;
                    const Rect& b = bar->second;
                    const float e = 0.01f;
                    const bool whole = k.x0 >= o.x0 - e && k.y0 >= o.y0 - e && k.x1 <= o.x1 + e &&
                                       k.y1 <= o.y1 + e && k.x0 >= b.x0 && k.y0 >= b.y0 &&
                                       k.x1 <= b.x1 && k.y1 <= b.y1;
                    const float px = k.y1 - k.y0;  // a small key's height IS its lettering
                    if (!whole || px < 13.0f - 1.0e-3f) {
                        std::printf("    %s: the Stop plugin key (%.1f,%.1f)-(%.1f,%.1f), %.1f px, "
                                    "clip (%.1f,%.1f)-(%.1f,%.1f)%s%s\n",
                                    l.name, k.x0, k.y0, k.x1, k.y1, px, o.x0, o.y0, o.x1, o.y1,
                                    whole ? "" : " CLIPPED", px < 13.0f ? " TOO SMALL" : "");
                    }
                    CHECK(whole);
                    CHECK(px >= 13.0f - 1.0e-3f);
                    for (const char* part : deckParts) {
                        if (std::string(part) == "deck:mute") { continue; }
                        const auto p = c.rects.find(part);
                        if (p == c.rects.end()) { continue; }  // failed above
                        const bool on = overlaps(k, p->second);
                        if (on) { std::printf("    %s: the Stop plugin key is on %s\n", l.name, part); }
                        CHECK(!on);
                    }
                }
            }
            for (std::size_t i = 0; i < std::size(deckParts); ++i) {
                for (std::size_t j = i + 1; j < std::size(deckParts); ++j) {
                    const auto a = c.rects.find(deckParts[i]);
                    const auto b = c.rects.find(deckParts[j]);
                    if (a == c.rects.end() || b == c.rects.end()) { continue; }  // failed above
                    const bool hit = overlaps(a->second, b->second);
                    if (hit) {
                        std::printf("    %s: %s overlaps %s\n", l.name, deckParts[i],
                                    deckParts[j]);
                    }
                    CHECK(!hit);
                }
            }
            std::printf("    %-11s %zu parts\n", l.name, c.items.size());
        }
    }

    const int rc = testSummary("test_theme_census");
    if (rc == 0) { fs::remove_all(dir, ec); }
    return rc;
}
