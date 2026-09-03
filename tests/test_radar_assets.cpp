/*
 * The radar unit's panel is authored in resources/web/radar and COMPILED IN
 * from src/radar/radar_assets.hpp, which tools/embed-radar-assets.py
 * generates. Two copies of anything drift, and this one drifts silently: the
 * generated header is not something anybody reads, so an edit to the CSS that
 * is never re-embedded produces a FoxSDR that serves the previous version of
 * its own radar with no error anywhere and no visible sign except a panel
 * that behaves like a release behind.
 *
 * This test is the thing that makes the two copies one. It fails the build
 * the moment they disagree, and its failure message says exactly what to run.
 */
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "radar/radar_assets.hpp"
#include "test_check.hpp"

namespace {

// Read a file and normalise CRLF to LF, matching what the generator does, so
// a Windows checkout with autocrlf=true does not fail this on line endings
// alone - a false failure here would teach people to ignore it.
bool readNormalised(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    const std::string raw((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    out.clear();
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') { continue; }
        out.push_back(raw[i]);
    }
    return true;
}

std::string embedded(const unsigned char* bytes, unsigned int len) {
    return std::string(reinterpret_cast<const char*>(bytes), len);
}

// Where the two differ, in terms someone can act on: the byte offset and the
// line, not "the strings are not equal".
void reportFirstDifference(const char* what, const std::string& disk,
                           const std::string& baked) {
    const std::size_t n = disk.size() < baked.size() ? disk.size() : baked.size();
    std::size_t i = 0;
    while (i < n && disk[i] == baked[i]) { ++i; }
    int line = 1;
    for (std::size_t k = 0; k < i; ++k) {
        if (disk[k] == '\n') { ++line; }
    }
    std::printf("      %s: first difference at byte %zu (line %d); "
                "on disk %zu bytes, embedded %zu bytes\n",
                what, i, line, disk.size(), baked.size());
    std::printf("      re-run: py -3.14 tools/embed-radar-assets.py\n");
}

void checkOne(const std::string& root, const char* name, const unsigned char* bytes,
              unsigned int len) {
    const std::string path = root + "/resources/web/radar/" + name;
    std::string disk;
    if (!readNormalised(path, disk)) {
        std::printf("      cannot read %s\n", path.c_str());
        CHECK(false);
        return;
    }
    const std::string baked = embedded(bytes, len);
    // Compared WHOLE, not by size first and content second: a length check
    // that passes tells you nothing, and one that fails hides where.
    const bool same = (disk == baked);
    if (!same) { reportFirstDifference(name, disk, baked); }
    CHECK(same);
}

void testEmbeddedMatchesSource(const std::string& root) {
    std::printf("  the compiled-in radar panel is the one in resources/web/radar\n");
    checkOne(root, "radar.html", cascade::radar::kRadarHtml, cascade::radar::kRadarHtmlLen);
    checkOne(root, "radar.css", cascade::radar::kRadarCss, cascade::radar::kRadarCssLen);
    checkOne(root, "radar.js", cascade::radar::kRadarJs, cascade::radar::kRadarJsLen);
}

// The panel is served under "default-src 'none'; script-src 'self'; style-src
// 'self'", which refuses an inline <script> and an inline style="" outright.
// Both are easy to reintroduce while editing - the design it was ported from
// is built entirely from inline styles - and the failure is invisible in a
// browser that has no policy applied, so it has to be caught here.
void testNoInlineStyleOrScript() {
    std::printf("  the panel carries no inline style or script, which its policy forbids\n");
    const std::string html = embedded(cascade::radar::kRadarHtml, cascade::radar::kRadarHtmlLen);
    CHECK(html.find(" style=\"") == std::string::npos);
    CHECK(html.find("<script>") == std::string::npos);
    // An external script is fine; an inline one is not. The only script tag
    // must carry a src.
    const std::size_t s = html.find("<script");
    CHECK(s != std::string::npos);
    CHECK(html.compare(s, 17, "<script src=\"rada") == 0);
}

// No origin but this one. A panel that can see decoded traffic must not be
// able to fetch a font, a tile or a script from anywhere else, and the policy
// that enforces that would silently break the page rather than the rule.
void testNoExternalOrigins() {
    std::printf("  nothing on the panel reaches outside the receiver's own origin\n");
    const std::string html = embedded(cascade::radar::kRadarHtml, cascade::radar::kRadarHtmlLen);
    const std::string css = embedded(cascade::radar::kRadarCss, cascade::radar::kRadarCssLen);
    const std::string js = embedded(cascade::radar::kRadarJs, cascade::radar::kRadarJsLen);
    for (const std::string* s : {&html, &css, &js}) {
        CHECK(s->find("http://") == std::string::npos);
        CHECK(s->find("https://") == std::string::npos);
        CHECK(s->find("//fonts.") == std::string::npos);
        CHECK(s->find("@import") == std::string::npos);
    }
}

// THE ALTITUDE PALETTE IS SHARED WITH THE DESKTOP MAP and is copied, not
// referenced - the panel is JavaScript and the map is C++. Two displays of
// the same air that disagreed about what a colour means would be worse than
// one of them having no colour at all, so the copy is pinned here: these are
// the exact values in src/gui/track_metrics.hpp altBandStyle.
void testAltitudePaletteMatchesTheDesktop() {
    std::printf("  the panel's altitude bands are the desktop map's, value for value\n");
    const std::string js = embedded(cascade::radar::kRadarJs, cascade::radar::kRadarJsLen);
    const char* kRequired[] = {
        "[255, 122, 20]", "[255, 190, 30]", "[156, 220, 40]",
        "[0, 205, 130]",  "[40, 190, 240]", "[130, 140, 255]",
    };
    for (const char* rgb : kRequired) {
        const bool present = js.find(rgb) != std::string::npos;
        if (!present) { std::printf("      missing band %s\n", rgb); }
        CHECK(present);
    }
    // And no red anywhere in the ladder: red is the emergency colour and the
    // unknown-altitude colour, and both of those have to win.
    CHECK(js.find("[255, 0, 0]") == std::string::npos);
}

}  // namespace

int main(int argc, char** argv) {
    // The repository root, passed by tests/CMakeLists.txt for the same reason
    // test_json_dump_policy is given src/: this test compares against files
    // in the tree, not against anything the build copied.
    const std::string root = (argc > 1) ? argv[1] : ".";
    testEmbeddedMatchesSource(root);
    testNoInlineStyleOrScript();
    testNoExternalOrigins();
    testAltitudePaletteMatchesTheDesktop();
    return testSummary("test_radar_assets");
}
