/*
 * NO RAW COLOUR IN src/gui - so the theme work cannot silently regress.
 *
 * WHY THIS EXISTS (themes, 2026-09-25). Switching theme repaints the whole
 * application only because every colour the look is made of is drawn through
 * gui/theme.hpp - a role, a named constant, or a TONE (today's literal plus
 * the roles it belongs to). One `IM_COL32(20, 21, 15, 255)` added next month
 * would be a patch of today's bench left behind in Night Watch or Daylight
 * Lab, invisible in every test and every screenshot of today. So this reads
 * the source and fails on any raw colour literal in src/gui that is not:
 *
 *   - fully transparent (IM_COL32(0, 0, 0, 0), ImVec4(0, 0, 0, 0)) - not a
 *     colour at all;
 *   - on a line marked `// theme-exempt: <reason>` - a colour that carries a
 *     MEANING independent of the look (a category colour, a debug overlay);
 *   - in one of the allow-listed files below, each with its reason.
 *
 * What counts as a raw colour: IM_COL32 / hexCol with a numeric first
 * argument, IM_COL32_WHITE / IM_COL32_BLACK, and an ImVec4 made of numbers.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

// WHOLE FILES THAT MAY HOLD RAW COLOURS, and why.
const std::set<std::string> kAllowedFiles = {
    // The palette itself: the six presets are written here as colours, and
    // the header's Preset carries the default sheen (white) and shadow (black).
    "theme.cpp",
    "theme.hpp",
    // Generated pixel art of the aircraft silhouettes (tools/, not hand
    // written): data, drawn tinted by the caller.
    "aircraft_icon_pixels.hpp",
};

bool allZero(const std::string& args) {
    static const std::regex nonZero("[1-9]");
    return !std::regex_search(args, nonZero);
}

}  // namespace

int main(int argc, char** argv) {
    fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    if (argc > 1) { src = fs::path(argv[1]) / "src" / "gui"; }
    CHECK(fs::is_directory(src));
    if (!fs::is_directory(src)) { return testSummary("test_theme_colours"); }

    // Each pattern captures the argument text so a fully transparent colour
    // can be told apart from a real one.
    const std::regex imcol(R"(IM_COL32\s*\(\s*((0x[0-9A-Fa-f]+|\d+)[^)]*)\))");
    const std::regex hexcol(R"(hexCol\s*\(\s*(0x[0-9A-Fa-f]+[^)]*)\))");
    const std::regex named(R"(IM_COL32_(WHITE|BLACK)\b(?!_))");
    const std::regex vec4(R"(ImVec4\s*\(\s*([0-9.]+f?\s*(/\s*[0-9.]+f?\s*)?,[^)]*)\))");

    int files = 0;
    int literals = 0;
    int exempt = 0;
    for (const auto& e : fs::directory_iterator(src)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        const std::string name = e.path().filename().string();
        if (kAllowedFiles.count(name) != 0) { continue; }
        ++files;
        std::ifstream in(e.path(), std::ios::binary);
        std::string line;
        int lineNo = 0;
        bool inBlock = false;
        while (std::getline(in, line)) {
            ++lineNo;
            // Comments are not drawn: block comments are skipped, and a line's
            // own // comment is cut off (after the exemption marker is read).
            std::string code = line;
            if (inBlock) {
                const std::size_t end = code.find("*/");
                if (end == std::string::npos) { continue; }
                code = code.substr(end + 2);
                inBlock = false;
            }
            if (const std::size_t start = code.find("/*"); start != std::string::npos) {
                const std::size_t end = code.find("*/", start + 2);
                if (end == std::string::npos) {
                    code = code.substr(0, start);
                    inBlock = true;
                } else {
                    code = code.substr(0, start) + code.substr(end + 2);
                }
            }
            const bool isExempt = line.find("theme-exempt:") != std::string::npos;
            if (const std::size_t c = code.find("//"); c != std::string::npos) {
                code = code.substr(0, c);
            }
            bool raw = false;
            for (const std::regex* re : {&imcol, &hexcol, &vec4}) {
                for (auto it = std::sregex_iterator(code.begin(), code.end(), *re);
                     it != std::sregex_iterator(); ++it) {
                    if (!allZero((*it)[1].str())) { raw = true; }
                }
            }
            if (std::regex_search(code, named)) { raw = true; }
            if (!raw) { continue; }
            ++literals;
            if (isExempt) {
                ++exempt;
                continue;
            }
            std::printf("  raw colour, not a theme tone: %s:%d: %s\n", name.c_str(), lineNo,
                        line.c_str());
            CHECK(false);
        }
    }
    std::printf("  %d files scanned, %d raw colour lines, %d of them marked theme-exempt\n",
                files, literals, exempt);
    // A scan that found no files, or a regex that matched nothing anywhere,
    // would pass by testing nothing: src/gui has dozens of files, and the
    // exempt debug overlays in app_window.cpp are raw literals it must see.
    CHECK(files >= 40);
    CHECK(exempt >= 3);
    return testSummary("test_theme_colours");
}
