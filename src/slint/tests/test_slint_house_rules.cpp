// The house rules of the Slint component library, checked against the sources themselves:
//   1. No colour literal outside theme.slint: every colour is a Theme token, or a skin could not change it.
//   2. No typeface named outside theme.slint, for the same reason.
//   3. No emoji in any .slint file (the brief: no emoji; glyphs are SVG icons or plain text).
//   4. Every .slint file under ui/ (theme.slint apart) imports the Theme, so nothing is styled from nowhere.
//   5. Every string literal with two or more consecutive letters, outside an import line, an @image-url, an @tr(...)
//      call and a line marked `// not-translated: <reason>`, is refused: it is prose a user would read in English
//      whatever language is chosen. (ui/specimens/ are test sheets, never shipped, and are not scanned for this rule.)
// Takes the ui directory on its command line (CMake passes it); run by hand, it walks up from the working
// directory to find src/slint/ui.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
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

// The marker that exempts a line from rule 5. It is read from the RAW line: stripComments would remove it.
const char* const kNotTranslated = "// not-translated:";

// Blank out (with spaces, keeping newlines) every `@tr(...)` and `@image-url(...)` call, balanced on parentheses and
// aware of string literals, so the literals inside them are not scanned.
std::string blankCalls(const std::string& code) {
    std::string out = code;
    for (const char* head : {"@tr(", "@image-url("}) {
        const std::size_t headLen = std::char_traits<char>::length(head);
        std::size_t at = 0;
        while ((at = out.find(head, at)) != std::string::npos) {
            std::size_t i = at + headLen;
            int depth = 1;
            bool inString = false;
            for (; i < out.size() && depth > 0; ++i) {
                const char c = out[i];
                if (inString) {
                    if (c == '\\' && i + 1 < out.size()) { ++i; }
                    else if (c == '"') { inString = false; }
                } else if (c == '"') { inString = true; }
                else if (c == '(') { ++depth; }
                else if (c == ')') { --depth; }
            }
            for (std::size_t k = at; k < i && k < out.size(); ++k) { if (out[k] != '\n') { out[k] = ' '; } }
            at = i;
        }
    }
    return out;
}

bool hasTwoLetters(const std::string& s) {
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        if (std::isalpha(static_cast<unsigned char>(s[i])) && std::isalpha(static_cast<unsigned char>(s[i + 1]))) { return true; }
    }
    return false;
}

// Rule 5 over one file: the offending literals as "file:line: literal". `raw` is the file as read, `code` the same
// with comments stripped (same line structure).
std::vector<std::string> untranslatedLiterals(const std::string& name, const std::string& raw, const std::string& code) {
    std::vector<std::string> rawLines, codeLines;
    {
        std::istringstream a(raw);
        std::string l;
        while (std::getline(a, l)) { rawLines.push_back(l); }
        std::istringstream b(blankCalls(code));
        while (std::getline(b, l)) { codeLines.push_back(l); }
    }
    std::vector<std::string> found;
    for (std::size_t n = 0; n < codeLines.size() && n < rawLines.size(); ++n) {
        if (rawLines[n].find(kNotTranslated) != std::string::npos) { continue; }
        const std::string& line = codeLines[n];
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line.compare(first, 6, "import") == 0) { continue; }
        // an `export { ... } from "x.slint";` (or its closing line) names a file, as an import does
        if (line.find("from \"") != std::string::npos && line.find(".slint\"") != std::string::npos) { continue; }
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] != '"') { continue; }
            std::size_t j = i + 1;
            for (; j < line.size() && line[j] != '"'; ++j) { if (line[j] == '\\') { ++j; } }
            const std::string literal = line.substr(i + 1, j > i + 1 ? j - i - 1 : 0);
            if (hasTwoLetters(literal)) { found.push_back(name + ":" + std::to_string(n + 1) + ": \"" + literal + "\""); }
            i = j;
        }
    }
    return found;
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
    int untranslated = 0;
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
        // rule 5: the specimens are test sheets, never shipped
        const std::string rel = fs::relative(entry.path(), ui).generic_string();
        if (rel.rfind("specimens/", 0) != 0) {
            for (const std::string& offender : untranslatedLiterals(rel, raw, code)) {
                std::printf("  untranslated literal %s\n", offender.c_str());
                ++untranslated;
            }
        }
        const bool emoji = hasEmoji(raw);
        if (emoji) { std::printf("  %s: holds an emoji\n", name.c_str()); }
        CHECK(!emoji);
    }
    std::printf("house rules: %d .slint files checked under %s; %d untranslated literal(s)\n", filesChecked, ui.string().c_str(), untranslated);
    CHECK(untranslated == 0);
    CHECK(filesChecked >= 2);
    return testSummary("test_slint_house_rules");
}
