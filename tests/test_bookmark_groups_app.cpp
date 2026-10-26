/*
 * GROUPS OF THE USER'S OWN IN THE BOOKMARKS SECTION, driven through the
 * controls a user touches (a tester's request, 2026-10: an airport's tower,
 * ground and approach kept together under "MILANO MALPENSA", another's under
 * "MILANO LINATE"). Until this change a group could only arrive with an
 * imported file or an airport lookup. Runs cascade itself, isolated like every
 * test that starts the application, with a scripted pointer and keyboard, and
 * checks what the run saved into bookmarks.json:
 *
 *   add       "MILANO MALPENSA" typed into the group box and Add current
 *             pressed twice: both bookmarks land in that group (the box is
 *             not cleared by Add, so a run of frequencies shares one group);
 *   no group  a row right-clicked and "No group" chosen in its menu: that row
 *             alone leaves its group, its neighbour in the same group stays;
 *   new group " LUGANO " typed into the menu's own field and Enter pressed:
 *             the row joins "LUGANO" - the blanks around a typed name are not
 *             part of it;
 *   rename    the group chosen in the group list, "Rename this group" pressed,
 *             the field's seeded name replaced by "LIMC" and Enter pressed:
 *             every row of the group is renamed, the row outside it is not.
 *
 * Every click is aimed at the rectangle a first, unscripted run of the same
 * layout reported for that control (a menu's controls at the rectangles a run
 * that opened the menu reported), so nothing here depends on where the rail
 * happens to put the section or where ImGui opens a popup.
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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/freq_manager.hpp"
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
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;
};

fs::path g_dir;

fs::path bookmarksPath() { return g_dir / "appdata" / "foxsdr" / "bookmarks.json"; }

void writeBookmarks(const std::string& body) {
    std::error_code ec;
    fs::create_directories(bookmarksPath().parent_path(), ec);
    std::ofstream f(bookmarksPath(), std::ios::binary | std::ios::trunc);
    f << body;
}

// Two of Malpensa's and one loose row, in frequency order TWR (119.0), GND
// (121.9), LOOSE (145.5) - the order the rows are drawn in, so "row 0" is TWR.
const char* const kThreeRows =
    "{ \"schemaVersion\": 1, \"bookmarks\": [\n"
    "  { \"name\": \"TWR\", \"freqHz\": 119000000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,"
    " \"group\": \"MILANO MALPENSA\" },\n"
    "  { \"name\": \"GND\", \"freqHz\": 121900000.0, \"mode\": \"AM\", \"bandwidthHz\": 10000.0,"
    " \"group\": \"MILANO MALPENSA\" },\n"
    "  { \"name\": \"LOOSE\", \"freqHz\": 145500000.0, \"mode\": \"NFM\", \"bandwidthHz\": 12500.0 }\n"
    "] }\n";

// One run of the application. `script` empty = no scripted input.
Result once(const std::string& tag, int frames, const std::string& script) {
    Result r;
    const fs::path cfg = g_dir / (tag + ".json");
    const fs::path census = g_dir / (tag + ".census");
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, "
             "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", "
             "\"mainView\": \"receiver\", \"railBank\": 2, \"bandPlanOverlay\": false }\n";
    }
    const std::string scriptPath = (g_dir / (tag + ".script")).string();
    {
        std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
        f << script;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_INPUT_SCRIPT", scriptPath);
    setEnv("FOXSDR_SCRIPT_TRACE", (g_dir / (tag + ".trace")).string());
    r.out = run("\"" + exePath() + "\" --frames " + std::to_string(frames) + " 2>&1");
    const bool rendered = r.out.find("rendered " + std::to_string(frames) + " frames") != std::string::npos;
    const bool written = r.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", tag.c_str(), r.out.c_str());
        return r;
    }
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
    r.ok = true;
    return r;
}

std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(),
                  frame + 2, frame + 4);
    return buf;
}

std::string rightClick(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d rdown\n%d rup\n", frame, at.cx(), at.cy(),
                  frame + 2, frame + 4);
    return buf;
}

// A control the layout pass must have reported, inside the 1600 x 1000 window:
// a rectangle off the bottom of the window would take the click nowhere and
// the failure would read as "the group did not change". A control that is not
// there IS a failure (CHECK), not a skipped scenario: run against a build
// without these controls the test goes red here instead of passing on fewer
// checks.
bool have(const Result& r, const char* name) {
    const auto it = r.rects.find(name);
    if (it == r.rects.end()) {
        std::printf("  the layout run reported no \"%s\"\n", name);
        CHECK(it != r.rects.end());
        return false;
    }
    const Rect& rc = it->second;
    const bool inside = rc.x1 > rc.x0 && rc.y1 > rc.y0 && rc.y1 <= 1000.0f && rc.x1 <= 1600.0f;
    if (!inside) {
        std::printf("  \"%s\" is not inside the window: %.0f %.0f %.0f %.0f\n", name, rc.x0, rc.y0, rc.x1, rc.y1);
    }
    CHECK(inside);
    return inside;
}

// The saved list, as (name -> group); empty on a load failure, which is
// reported.
std::map<std::string, std::string> savedGroups() {
    std::map<std::string, std::string> out;
    cascade::core::FreqManager m;
    std::string err;
    if (!m.load(bookmarksPath().string(), err)) {
        std::printf("  bookmarks.json did not load: %s\n", err.c_str());
        CHECK(false);
        return out;
    }
    for (const cascade::core::Bookmark& b : m.list()) { out[b.name] = b.group; }
    return out;
}

}  // namespace

int main() {
    std::printf("test_bookmark_groups_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-bmgroups-" + std::to_string(pid()));
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
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_OPEN_SECTIONS", "Bookmarks");
    // The census walks the rail through every bank unless told to hold the
    // one the config chose (TOOLS, where the Bookmarks section is).
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_BOOKMARK_IMPORT", "");
    setEnv("CASCADE_DECODE_TEST", "");

    // --- add: the group box names the group Add current puts a bookmark in ---
    {
        writeBookmarks("{ \"schemaVersion\": 1, \"bookmarks\": [] }\n");
        const Result layout = once("add-layout", 60, "");
        CHECK(layout.ok);
        CHECK(layout.items.count("bookmarks:rows:0") == 1);
        // An empty list has no groups, so the arrow beside the box is not drawn.
        CHECK(layout.rects.count("bookmarks:grouppick") == 0);
        if (layout.ok && have(layout, "bookmarks:group") && have(layout, "bookmarks:add")) {
            std::string s = click(40, layout.rects.at("bookmarks:group"));
            s += "46 text MILANO MALPENSA\n";
            s += click(52, layout.rects.at("bookmarks:add"));
            s += click(62, layout.rects.at("bookmarks:add"));
            const Result r = once("add", 100, s);
            CHECK(r.ok);
            CHECK(r.items.count("bookmarks:rows:2") == 1);
            // Now that a group exists the arrow is drawn beside the box.
            CHECK(r.rects.count("bookmarks:grouppick") == 1);
            const auto groups = savedGroups();
            CHECK(groups.size() == 2u);
            for (const auto& [name, group] : groups) {
                if (group != "MILANO MALPENSA") {
                    std::printf("  \"%s\" saved in group \"%s\"\n", name.c_str(), group.c_str());
                }
                CHECK(group == "MILANO MALPENSA");
            }
        }
    }

    // --- the row's menu: No group, and a name typed into the menu --------------
    {
        writeBookmarks(kThreeRows);
        const Result layout = once("menu-layout", 60, "");
        CHECK(layout.ok);
        CHECK(layout.items.count("bookmarks:rows:3") == 1);
        CHECK(layout.rects.count("bookmarks:menu:nogroup") == 0);  // no menu open: nothing of it drawn
        if (layout.ok && have(layout, "bookmarks:row:0")) {
            const Rect row0 = layout.rects.at("bookmarks:row:0");
            // Where the menu puts its controls, from a run that opens it and
            // leaves it open.
            const Result menu = once("menu-open", 70, rightClick(40, row0));
            CHECK(menu.ok);
            CHECK(menu.rects.count("bookmarks:menu:group:0") == 1);  // the one group the list has
            if (menu.ok && have(menu, "bookmarks:menu:nogroup") && have(menu, "bookmarks:menu:newgroup")) {
                // No group: TWR leaves Malpensa, GND stays.
                {
                    writeBookmarks(kThreeRows);
                    std::string s = rightClick(40, row0);
                    s += click(50, menu.rects.at("bookmarks:menu:nogroup"));
                    const Result r = once("menu-nogroup", 90, s);
                    CHECK(r.ok);
                    const auto groups = savedGroups();
                    CHECK(groups.size() == 3u);
                    CHECK(groups.count("TWR") == 1 && groups.at("TWR").empty());
                    CHECK(groups.count("GND") == 1 && groups.at("GND") == "MILANO MALPENSA");
                    CHECK(groups.count("LOOSE") == 1 && groups.at("LOOSE").empty());
                }
                // A new name typed into the menu, blanks around it and Enter.
                {
                    writeBookmarks(kThreeRows);
                    std::string s = rightClick(40, row0);
                    s += click(50, menu.rects.at("bookmarks:menu:newgroup"));
                    s += "56 text  LUGANO \n";
                    s += "60 key enter\n";
                    const Result r = once("menu-newgroup", 90, s);
                    CHECK(r.ok);
                    const auto groups = savedGroups();
                    CHECK(groups.size() == 3u);
                    CHECK(groups.count("TWR") == 1 && groups.at("TWR") == "LUGANO");
                    CHECK(groups.count("GND") == 1 && groups.at("GND") == "MILANO MALPENSA");
                    CHECK(groups.count("LOOSE") == 1 && groups.at("LOOSE").empty());
                }
            }
        }
    }

    // --- rename: the group chosen in the list, Rename this group, a new name ---
    {
        writeBookmarks(kThreeRows);
        const Result layout = once("rename-layout", 60, "");
        CHECK(layout.ok);
        CHECK(layout.rects.count("bookmarks:rename") == 0);  // no group chosen: no Rename key
        if (layout.ok && have(layout, "bookmarks:groupsel")) {
            const Rect combo = layout.rects.at("bookmarks:groupsel");
            // The list open: where its one entry is.
            const Result open = once("rename-open", 70, click(40, combo));
            CHECK(open.ok);
            if (open.ok && have(open, "bookmarks:groupsel:0")) {
                const Rect entry = open.rects.at("bookmarks:groupsel:0");
                // The group chosen: the Rename key appears.
                std::string chosen = click(40, combo) + click(50, entry);
                const Result picked = once("rename-picked", 80, chosen);
                CHECK(picked.ok);
                CHECK(picked.items.count("bookmarks:rows:2") == 1);  // the view shows Malpensa's two
                if (picked.ok && have(picked, "bookmarks:rename")) {
                    // The key pressed: the field and its own key appear.
                    std::string opened = chosen + click(60, picked.rects.at("bookmarks:rename"));
                    const Result field = once("rename-field", 90, opened);
                    CHECK(field.ok);
                    if (field.ok && have(field, "bookmarks:renamefield") && have(field, "bookmarks:renamekey")) {
                        writeBookmarks(kThreeRows);
                        std::string s = opened + click(70, field.rects.at("bookmarks:renamefield"));
                        s += "76 key ctrl+a\n78 text LIMC\n82 key enter\n";
                        const Result r = once("rename", 120, s);
                        CHECK(r.ok);
                        const auto groups = savedGroups();
                        CHECK(groups.size() == 3u);
                        CHECK(groups.count("TWR") == 1 && groups.at("TWR") == "LIMC");
                        CHECK(groups.count("GND") == 1 && groups.at("GND") == "LIMC");
                        CHECK(groups.count("LOOSE") == 1 && groups.at("LOOSE").empty());
                        // The renamed group stays the chosen one, so the view
                        // still shows its two rows and the Rename key.
                        CHECK(r.items.count("bookmarks:rows:2") == 1);
                        CHECK(r.rects.count("bookmarks:rename") == 1);
                    }
                }
            }
        }
    }

    const int rc = testSummary("test_bookmark_groups_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
