// (window-gaps-2, rows 148-152) The three skins ported from the classic presets: read from theme.slint, each holds the reading pairs of the classic test (4.5 : 1) and defines every token the Workbench does.
// Derived from the high-contrast test: the high-contrast skin's palette (src/slint/ui/theme.slint, global HighContrast; docs/multi-radio-records/slint-gaps.md item 11): read from the .slint source itself, every reading pair
// (text on its ground, a key's text on the key, a chip's text on the chip, the accent's ink on the accent, the lines on the grounds) is held to WCAG 7 : 1, the disabled ink to 4.5 : 1, and every colour token
// the Workbench skin defines is defined here too. Takes the ui directory on its command line (CMake passes it).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::map<std::string, std::string> tokensOf(const std::string& text, const std::string& global) {
    std::map<std::string, std::string> out;
    const std::size_t at = text.find("export global " + global + " {");
    if (at == std::string::npos) { return out; }
    const std::size_t open = text.find("value: {", at);
    const std::size_t close = text.find("\n    };", open);
    const std::string body = text.substr(open + 8, close - open - 8);
    const std::regex tok(R"(([a-z0-9-]+):\s*("[^"]*"|[^,\n\r]+?)\s*(?:,|\r|\n|$))");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), tok); it != std::sregex_iterator(); ++it) { out.emplace((*it)[1], (*it)[2]); }
    return out;
}

struct Rgb { double r, g, b; };
Rgb rgbOf(const std::string& hex) {
    const unsigned long v = std::strtoul(hex.c_str() + 1, nullptr, 16);
    return {static_cast<double>((v >> 16) & 255u), static_cast<double>((v >> 8) & 255u), static_cast<double>(v & 255u)};
}
double lum(const Rgb& c) {
    const auto lin = [](double v) { v /= 255.0; return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b);
}
double ratio(const std::string& a, const std::string& b) {
    const double la = lum(rgbOf(a)), lb = lum(rgbOf(b));
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "src/slint/ui";
    const std::string text = slurp(dir + "/theme.slint");
    CHECK(!text.empty());
    const auto wb = tokensOf(text, "Workbench");
    CHECK(wb.size() > 100);
    std::map<std::string, std::map<std::string, std::string>> all;
    for (const char* g : {"GlassCockpit", "DaylightLab", "FieldRadio"}) {
        const auto sk = tokensOf(text, g);
        all[g] = sk;
        CHECK(sk.size() == wb.size());
        for (const auto& kv : wb) { CHECK(sk.count(kv.first) == 1); }
        // each skin is its own: the ground and the accent differ from the Workbench's
        CHECK(sk.at("bg") != wb.at("bg") && sk.at("accent") != wb.at("accent"));
        const auto need = [&](const char* fg, const char* bg, double min) {
            const auto a = sk.find(fg), b = sk.find(bg);
            CHECK(a != sk.end() && b != sk.end());
            if (a == sk.end() || b == sk.end()) { return; }
            const double r = ratio(a->second.substr(0, 7), b->second.substr(0, 7));
            if (r < min) { std::printf("  %s: %s on %s is %.2f : 1 (wanted %.1f)\n", g, fg, bg, r, min); }
            CHECK(r >= min);
        };
        for (const char* ground : {"bg", "strip", "panel", "panel-alt", "card", "well", "menu"}) { need("fg", ground, 4.5); need("muted", ground, 4.5); }
        need("fg", "field", 4.5);
        need("digit", "field", 4.5);
        need("key-fg", "key-bg", 4.5);
        need("chip-fg", "chip-bg", 4.5);
        need("chip-good-fg", "chip-good-bg", 4.5);
        need("chip-warn-fg", "chip-warn-bg", 4.5);
        need("chip-err-fg", "chip-err-bg", 4.5);
        need("accent-ink", "accent", 4.5);
        need("danger-fg", "danger-bg", 4.5);
        need("bandplan-fg", "bandplan", 4.5);
        need("axis-fg", "well", 3.0);
        need("trace-live", "well", 3.0);
    }
    // three skins that differ from one another
    CHECK(all["GlassCockpit"].at("bg") != all["DaylightLab"].at("bg") && all["DaylightLab"].at("bg") != all["FieldRadio"].at("bg"));
    return testSummary("test_slint_themes_four");
}
