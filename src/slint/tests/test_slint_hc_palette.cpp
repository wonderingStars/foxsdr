// The high-contrast skin's palette (src/slint/ui/theme.slint, global HighContrast; docs/multi-radio-records/slint-gaps.md item 11): read from the .slint source itself, every reading pair
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
    const auto hc = tokensOf(text, "HighContrast");
    const auto wb = tokensOf(text, "Workbench");
    CHECK(hc.size() > 100 && hc.size() == wb.size());
    for (const auto& kv : wb) { CHECK(hc.count(kv.first) == 1); }   // nothing the other skins define is left out

    const auto need = [&](const char* fg, const char* bg, double min) {
        const auto a = hc.find(fg), b = hc.find(bg);
        CHECK(a != hc.end() && b != hc.end());
        if (a == hc.end() || b == hc.end()) { return; }
        const double r = ratio(a->second.substr(0, 7), b->second.substr(0, 7));
        if (r < min) { std::printf("  %s on %s is %.2f : 1 (wanted %.1f)\n", fg, bg, r, min); }
        CHECK(r >= min);
    };
    for (const char* ground : {"bg", "strip", "panel", "panel-alt", "card", "well", "field", "menu"}) {
        for (const char* ink : {"fg", "fg-soft", "muted", "dim", "faint"}) { need(ink, ground, 7.0); }
    }
    need("digit", "field", 7.0);
    need("digit-dim", "field", 4.5);        // the leading zeros are meant to recede
    need("axis-fg", "well", 7.0);
    need("key-fg", "key-bg", 7.0);
    need("key-disabled-fg", "key-disabled-bg", 4.5);
    need("chip-fg", "chip-bg", 7.0);
    need("chip-good-fg", "chip-good-bg", 7.0);
    need("chip-warn-fg", "chip-warn-bg", 7.0);
    need("chip-err-fg", "chip-err-bg", 7.0);
    need("accent-ink", "accent", 7.0);
    need("danger-fg", "danger-bg", 7.0);
    need("plate-fg", "bg", 7.0);
    need("accent", "bg", 7.0);
    need("focus", "bg", 7.0);
    need("line", "bg", 7.0);                 // the lines are as visible as the text
    need("line-strong", "bg", 7.0);
    need("bandplan-fg", "bandplan", 7.0);
    need("lamp-green", "bg", 7.0);
    need("lamp-red", "bg", 4.5);
    need("trace-live", "well", 7.0);
    need("vfo", "well", 7.0);
    return testSummary("test_slint_hc_palette");
}
