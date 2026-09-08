// Tests for the typeface plumbing that does not need an atlas: where the
// system's Georgia is looked for, and that the lookup is pure.
//
// The face itself is exercised by test_bench_text_fits, which loads the atlas
// and measures real glyphs; this file pins the one piece of logic a missing
// font would otherwise hide - the path it was NOT found at.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdlib>
#include <string>

#include "gui/fonts.hpp"
#include "test_check.hpp"

namespace {

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

}  // namespace

int main() {
    using cascade::gui::fonts::systemFontPath;

    // An empty or null name is no path at all, never a directory.
    CHECK(systemFontPath(nullptr).empty());
    CHECK(systemFontPath("").empty());

#ifdef _WIN32
    // %WINDIR% is honoured, so a Windows installed on D: still finds its
    // fonts; the file name is appended under Fonts.
    setEnv("WINDIR", "D:\\Win");
    CHECK(systemFontPath("georgia.ttf") == "D:\\Win\\Fonts\\georgia.ttf");
    CHECK(systemFontPath("georgiab.ttf") == "D:\\Win\\Fonts\\georgiab.ttf");
    // Without it, the usual drive letter.
    setEnv("WINDIR", "");
    CHECK(systemFontPath("georgia.ttf") == "C:\\Windows\\Fonts\\georgia.ttf");
#else
    // No convention on other platforms: empty, so the caller falls back to
    // the embedded pair rather than probing a guessed directory.
    CHECK(systemFontPath("georgia.ttf").empty());
#endif

    // Before load() nothing is in use.
    CHECK(!cascade::gui::fonts::usingSystemSerif());

    return testSummary("test_fonts");
}
