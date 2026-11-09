// THE CJK GLYPHS OF THE SLINT WINDOW (docs 6.62), through the window's own typeface and the face main.cpp names: for the language on the command line, the system face is chosen and
// registered and Theme.cjk-family set exactly as main.cpp does, then every Han, kana, Hangul and CJK punctuation code point the catalogue uses is rendered by the headless software
// renderer, one character per picture, in Theme.font-ui, and the pictures are judged. A character the face does not map is drawn as a missing-glyph box (the same box for every such
// character) or as nothing at all: a picture that is blank (bar the few space characters), equals the picture of a private-use code point no face has (U+E000), or is shared by three
// or more different characters IS a failure, and the test names the code points. The detector is itself held: three private-use probes must be flagged by it.
//
//   test_slint_cjk <code>      ja | ko | zh-CN | zh-TW
//
// A machine with no usable system face for the language is reported as SKIPPED, never as passed (the classic window's test_i18n_glyphs rule).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "cjk_specimen.h"

#include "cjk_register.hpp"
#include "core/i18n.hpp"
#include "core/scripts.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;
namespace scripts = cascade::core::scripts;

namespace {

std::string utf8(unsigned int cp) {
    std::string s;
    if (cp < 0x80) { s += static_cast<char>(cp); }
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    return s;
}

std::uint64_t hashOf(const Frame& f) {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& p : f.px) {
        h = (h ^ p.r) * 1099511628211ull;
        h = (h ^ p.g) * 1099511628211ull;
        h = (h ^ p.b) * 1099511628211ull;
    }
    return h;
}

bool blank(const Frame& f) {
    for (const auto& p : f.px) { if (p.r > 40 || p.g > 40 || p.b > 40) { return false; } }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string code = argc > 1 ? argv[1] : "ja";
    std::printf("test_slint_cjk %s\n", code.c_str());
    installHeadlessPlatform();
    auto ui = CjkSpecimen::create();
    slint::Window& win = ui->window();

    const std::string inForce = cascade::i18n::setLanguage(code);
    CHECK(inForce == code);
    std::vector<unsigned int> cps;
    for (const unsigned int cp : cascade::i18n::codePoints(code)) {
        if (scripts::isCjk(cp)) { cps.push_back(cp); }
    }
    std::printf("  %zu CJK code points in the %s catalogue\n", cps.size(), code.c_str());
    CHECK(cps.size() > 50);

    const foxsdr::slintfonts::Registered reg = foxsdr::slintfonts::registerForLanguage(win, code);
    if (reg.family.empty()) {
        std::printf("SKIP: this machine has no usable system face for %s (the window would say so; nothing was checked)\n", code.c_str());
        ++g_checksSkipped;
        return testSummary("test_slint_cjk");
    }
    std::printf("  faces %s, font-family \"%s\"\n", reg.labels.c_str(), reg.family.c_str());
    ui->global<Theme>().set_cjk_family(slint::SharedString(reg.family));
    CHECK(std::string(ui->get_family()) == reg.family);   // the specimen's line is lettered in Theme.font-ui, which is the face now

    ui->set_sample(slint::SharedString(utf8(0xE000)));
    const std::uint64_t referenceHash = hashOf(render(ui, 64, 48));

    // The characters that draw nothing on purpose.
    const std::set<unsigned int> spaceLike = {0x3000, 0x115F, 0x1160, 0x3164, 0xFFA0};
    std::map<std::uint64_t, std::vector<unsigned int>> byPicture;
    std::map<unsigned int, std::uint64_t> pictureOf;
    const std::vector<unsigned int> probes = {0xE001, 0xE002, 0xE003};   // private use: no face has them
    std::set<unsigned int> invisible;
    int drawn = 0;
    auto look = [&](unsigned int cp) {
        ui->set_sample(slint::SharedString(utf8(cp)));
        const Frame f = render(ui, 64, 48);
        if (blank(f)) {
            if (spaceLike.count(cp) == 0) { invisible.insert(cp); }
            return;
        }
        ++drawn;
        const std::uint64_t h = hashOf(f);
        pictureOf[cp] = h;
        byPicture[h].push_back(cp);
    };
    for (const unsigned int cp : cps) { look(cp); }
    const int realDrawn = drawn;
    for (const unsigned int cp : probes) { look(cp); }

    auto flagged = [&](unsigned int cp) {
        if (invisible.count(cp) != 0) { return true; }
        const auto it = pictureOf.find(cp);
        if (it == pictureOf.end()) { return false; }   // a space-like blank
        return it->second == referenceHash || byPicture[it->second].size() >= 3;
    };
    // the detector's own check: each private-use probe is flagged by one of its three rules
    int probesFlagged = 0;
    for (const unsigned int cp : probes) { probesFlagged += flagged(cp) ? 1 : 0; }
    std::printf("  detector: %d of %zu private-use probes flagged\n", probesFlagged, probes.size());
    CHECK(probesFlagged == static_cast<int>(probes.size()));

    std::vector<unsigned int> bad;
    for (const unsigned int cp : cps) { if (flagged(cp)) { bad.push_back(cp); } }
    std::printf("  %d drawn, %zu invisible or as the missing-glyph box\n", realDrawn, bad.size());
    for (std::size_t i = 0; i < bad.size() && i < 16; ++i) { std::printf("    U+%04X %s\n", bad[i], utf8(bad[i]).c_str()); }
    CHECK(realDrawn > 50);
    CHECK(bad.empty());
    return testSummary("test_slint_cjk");
}
