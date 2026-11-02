// The house rules of the Slint component library, checked against the sources themselves:
//   1. No colour literal outside theme.slint: every colour is a Theme token, or a skin could not change it.
//   2. No typeface named outside theme.slint, for the same reason.
//   3. No emoji in any .slint file (the brief: no emoji; glyphs are SVG icons or plain text).
//   4. Every .slint file under ui/ (theme.slint apart) imports the Theme, so nothing is styled from nowhere.
// Takes the ui directory on its command line (CMake passes it); run by hand, it walks up from the working
// directory to find src/slint/ui.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string stripComments(const std::string& text) {
    std::string out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        // a // comment runs to the end of the line, unless the // sits inside a string ("http://...")
        bool inString = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') { inString = !inString; }
            if (!inString && line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') { line.resize(i); break; }
        }
        out += line;
        out += '\n';
    }
    return out;
}

bool hasEmoji(const std::string& text) {
    // any code point above U+FFFF is encoded as a 4-byte sequence whose lead byte is 0xF0..0xF4
    for (unsigned char c : text) { if (c >= 0xF0 && c <= 0xF4) { return true; } }
    return false;
}

fs::path findUiDir(int argc, char** argv) {
    if (argc > 1) { return fs::path(argv[1]); }
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "src" / "slint" / "ui" / "theme.slint")) { return p / "src" / "slint" / "ui"; }
        if (!p.has_parent_path() || p.parent_path() == p) { break; }
        p = p.parent_path();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path ui = findUiDir(argc, argv);
    CHECK(!ui.empty() && fs::exists(ui / "theme.slint"));
    if (ui.empty()) { return testSummary("test_slint_house_rules"); }

    const std::regex colourLiteral("#[0-9a-fA-F]{3,8}\\b");
    const std::regex fontLiteral("font-family\\s*:\\s*\"");
    int filesChecked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(ui)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".slint") { continue; }
        const std::string name = entry.path().filename().string();
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        const std::string raw = buf.str();
        const std::string code = stripComments(raw);
        ++filesChecked;

        const bool isTheme = name == "theme.slint";
        if (!isTheme) {
            std::smatch m;
            const bool colour = std::regex_search(code, m, colourLiteral);
            if (colour) { std::printf("  %s: colour literal %s\n", name.c_str(), m.str().c_str()); }
            CHECK(!colour);
            const bool font = std::regex_search(code, m, fontLiteral);
            if (font) { std::printf("  %s: typeface named outside the theme\n", name.c_str()); }
            CHECK(!font);
            const bool importsTheme = code.find("theme.slint\"") != std::string::npos;
            if (!importsTheme) { std::printf("  %s: does not import the Theme\n", name.c_str()); }
            CHECK(importsTheme);
        }
        const bool emoji = hasEmoji(raw);
        if (emoji) { std::printf("  %s: holds an emoji\n", name.c_str()); }
        CHECK(!emoji);
    }
    std::printf("house rules: %d .slint files checked under %s\n", filesChecked, ui.string().c_str());
    CHECK(filesChecked >= 2);
    return testSummary("test_slint_house_rules");
}
