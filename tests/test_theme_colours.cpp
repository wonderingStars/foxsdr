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
 * What counts as a raw colour: IM_COL32 / hexCol / ImColor with a numeric
 * first argument, IM_COL32_WHITE / IM_COL32_BLACK, an ImVec4 made of numbers
 * - ImVec4(...), ImVec4{...} or a declaration `ImVec4 name(...)` / `{...}` -
 * and a hex ImU32 literal (0xAABBGGRR or 0xRRGGBB) in a statement that is
 * drawing or declaring a colour. A call split across lines is read whole
 * (repair round, 2026-09-25: the first cut read one line at a time, and an
 * `IM_COL32(` with its arguments on the next line, an ImColor, a braced
 * ImVec4 and a bare hex ImU32 all walked straight past it - each is now
 * proved to go red by a probe file).
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

// Comments are not drawn: block comments are skipped (across lines, through
// `inBlock`) and a line's own // comment is cut off.
std::string stripComments(const std::string& line, bool& inBlock) {
    std::string code = line;
    std::string out;
    while (!code.empty()) {
        if (inBlock) {
            const std::size_t end = code.find("*/");
            if (end == std::string::npos) { return out; }
            code = code.substr(end + 2);
            inBlock = false;
            continue;
        }
        const std::size_t block = code.find("/*");
        const std::size_t line2 = code.find("//");
        if (line2 != std::string::npos && (block == std::string::npos || line2 < block)) {
            return out + code.substr(0, line2);
        }
        if (block == std::string::npos) { return out + code; }
        out += code.substr(0, block) + " ";
        code = code.substr(block + 2);
        inBlock = true;
    }
    return out;
}

// Whether a colour call on this text opens and does not close - its arguments
// run on to the next line.
bool colourCallOpen(const std::string& code) {
    static const std::regex start(R"((IM_COL32|ImColor|ImVec4|hexCol)(\s+\w+)?\s*[({])");
    for (auto it = std::sregex_iterator(code.begin(), code.end(), start);
         it != std::sregex_iterator(); ++it) {
        int depth = 0;
        for (std::size_t i = static_cast<std::size_t>(it->position() + it->length()) - 1;
             i < code.size(); ++i) {
            const char ch = code[i];
            if (ch == '(' || ch == '{') { ++depth; }
            if (ch == ')' || ch == '}') { --depth; }
        }
        if (depth > 0) { return true; }
    }
    return false;
}

// A hex literal that is a COLOUR: not a mask (0x00FFFFFF, 0xFFFFFF) and not
// an alpha on its own (0xNN000000), in a statement that draws or declares one.
// toneHex / hexCol arguments are today's literal handed to a tone or already
// caught as hexCol, so they are taken out first.
bool rawHexColour(const std::string& code) {
    static const std::regex context(
        R"(\bImU32\b|->Add\w+\s*\(|PushStyleColor|ColorConvert|TextColored|ImColor)");
    if (!std::regex_search(code, context)) { return false; }
    static const std::regex handedOn(R"((toneHex|hexCol)\s*\(\s*0x[0-9A-Fa-f]+)");
    const std::string rest = std::regex_replace(code, handedOn, "$1(_");
    static const std::regex hex(R"(\b0x([0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})[uUlL]*\b)");
    for (auto it = std::sregex_iterator(rest.begin(), rest.end(), hex);
         it != std::sregex_iterator(); ++it) {
        const unsigned long v = std::stoul((*it)[1].str(), nullptr, 16);
        if (v == 0x00FFFFFFul) { continue; }                      // the RGB mask
        if ((v & 0x00FFFFFFul) == 0 && v != 0) { continue; }     // an alpha alone
        if (v == 0) { continue; }                                 // transparent
        return true;
    }
    return false;
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
    // ImVec4(...), ImVec4{...}, and a declaration ImVec4 name(...) / name{...},
    // made of numbers.
    const std::regex vec4(
        R"(ImVec4(\s+\w+)?\s*\(\s*(([0-9.]+f?\s*(/\s*[0-9.]+f?\s*)?,[^)]*))\))");
    const std::regex vec4brace(
        R"(ImVec4(\s+\w+)?\s*\{\s*(([0-9.]+f?\s*(/\s*[0-9.]+f?\s*)?,[^}]*))\})");
    // ImColor(r, g, b[, a]) in bytes or floats, or ImColor(0xAABBGGRR).
    const std::regex imcolor(R"(ImColor(\s+\w+)?\s*[({]\s*(((0x[0-9A-Fa-f]+|[0-9.]+f?))[^)}]*)[)}])");

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
            const int firstLine = lineNo;
            // The exemption marker is read before the comment it sits in is cut.
            bool isExempt = line.find("theme-exempt:") != std::string::npos;
            std::string code = stripComments(line, inBlock);
            // A COLOUR CALL SPLIT ACROSS LINES is read whole: its continuation
            // lines are appended (up to eight) and not scanned again on their
            // own. The marker on any line it spans exempts it.
            for (int joined = 0; joined < 8 && colourCallOpen(code); ++joined) {
                std::string next;
                if (!std::getline(in, next)) { break; }
                ++lineNo;
                isExempt = isExempt || next.find("theme-exempt:") != std::string::npos;
                code += " " + stripComments(next, inBlock);
            }
            bool raw = false;
            for (const std::regex* re : {&imcol, &hexcol}) {
                for (auto it = std::sregex_iterator(code.begin(), code.end(), *re);
                     it != std::sregex_iterator(); ++it) {
                    if (!allZero((*it)[1].str())) { raw = true; }
                }
            }
            for (const std::regex* re : {&vec4, &vec4brace, &imcolor}) {
                for (auto it = std::sregex_iterator(code.begin(), code.end(), *re);
                     it != std::sregex_iterator(); ++it) {
                    if (!allZero((*it)[2].str())) { raw = true; }
                }
            }
            if (std::regex_search(code, named)) { raw = true; }
            if (rawHexColour(code)) { raw = true; }
            if (!raw) { continue; }
            ++literals;
            if (isExempt) {
                ++exempt;
                continue;
            }
            std::printf("  raw colour, not a theme tone: %s:%d: %s\n", name.c_str(), firstLine,
                        code.c_str());
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
