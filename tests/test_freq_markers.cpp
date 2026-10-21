// Tests for core/freq_markers.hpp - the waterfall's frequency markers: the
// list (numbers never reused, a second click on one signal adds nothing, the
// cap), what Copy puts on the clipboard, the file round trip and its damage
// rules, and the rounding that keeps a marker to the precision of the pixel
// it was dropped on.
//
// Fixture files live under a pid-suffixed directory in the CWD, removed on
// success and left for autopsy on failure (test_freq_manager's rule).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_markers.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

using cascade::core::FreqMarkers;
namespace fs = std::filesystem;

namespace {

std::string g_root;

std::string p(const char* rel) { return g_root + "/" + rel; }

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

void testNumbering() {
    std::printf("  numbers are names: handed out in order, never reused until clear\n");
    FreqMarkers m;
    CHECK(m.add(145.5e6, 100) == 1);
    CHECK(m.add(144.8e6, 101) == 2);
    CHECK(m.add(146.0e6, 102) == 3);
    CHECK(m.remove(2));
    CHECK(!m.remove(2));
    CHECK(m.size() == 2u);
    // M2 is gone; the next one is still M4, so a noted "M3" keeps its meaning.
    CHECK(m.add(430.0e6, 103) == 4);
    CHECK(m.list().size() == 3u);
    CHECK(m.list()[0].number == 1 && m.list()[1].number == 3 && m.list()[2].number == 4);
    m.clear();
    CHECK(m.empty());
    CHECK(m.add(430.0e6, 104) == 1);
}

void testMergeAndRefusals() {
    std::printf("  a second click on one signal adds nothing; bad frequencies and a full list refused\n");
    FreqMarkers m;
    CHECK(m.add(145.5e6, 1, 2000.0) == 1);
    // Within the merge distance: the existing marker's number, nothing added.
    CHECK(m.add(145.5015e6, 2, 2000.0) == 1);
    CHECK(m.size() == 1u);
    // Just outside it: a new marker.
    CHECK(m.add(145.5025e6, 3, 2000.0) == 2);
    CHECK(m.size() == 2u);
    const unsigned v = m.version();
    CHECK(m.add(0.0, 4) == 0);
    CHECK(m.add(-5.0, 4) == 0);
    CHECK(m.add(std::nan(""), 4) == 0);
    CHECK(m.add(std::numeric_limits<double>::infinity(), 4) == 0);
    CHECK(m.version() == v);  // a refusal changes nothing

    FreqMarkers full;
    for (std::size_t i = 0; i < FreqMarkers::kMaxMarkers; ++i) {
        CHECK(full.add(1.0e6 + static_cast<double>(i) * 1000.0, 0) != 0);
    }
    CHECK(full.size() == FreqMarkers::kMaxMarkers);
    CHECK(full.add(900.0e6, 0) == 0);
    CHECK(full.size() == FreqMarkers::kMaxMarkers);
}

void testNearestAndNotes() {
    std::printf("  nearest within a tolerance, and notes by number\n");
    FreqMarkers m;
    m.add(100.0e6, 0);
    m.add(100.010e6, 0);
    CHECK(m.nearest(100.004e6, 5000.0) == 1);
    CHECK(m.nearest(100.006e6, 5000.0) == 2);
    CHECK(m.nearest(100.5e6, 5000.0) == 0);
    CHECK(m.setNote(2, "burst every 30 s"));
    CHECK(!m.setNote(9, "x"));
    CHECK(m.list()[1].note == "burst every 30 s");
}

void testClipboard() {
    std::printf("  the clipboard text: heading, frequency order, tab columns, a note never splits a row\n");
    FreqMarkers m;
    CHECK(m.clipboardText(true).empty());
    // 2026-09-30 14:01:07 UTC and 14:03:22 UTC.
    m.add(145.5e6, 1790776867);
    m.add(144.8e6, 1790777002);
    m.setNote(1, "a\tb\nc");
    const std::string want =
        "FoxSDR markers (2)\n"
        "M2\t144.800000 MHz\t2026-09-30 14:03:22\n"
        "M1\t145.500000 MHz\t2026-09-30 14:01:07\ta b c\n";
    const std::string got = m.clipboardText(true);
    if (got != want) { std::printf("      got:\n%s", got.c_str()); }
    CHECK(got == want);
}

void testRoundTrip() {
    std::printf("  save then load: every field, and the numbering carries on\n");
    FreqMarkers m;
    m.add(145.5e6, 1790776867);
    m.add(144.8e6, 1790777002);
    m.add(433.92e6, 1790777100);
    m.remove(2);
    m.setNote(3, "ISM \xe2\x80\x94 key fob?");
    std::string err;
    const std::string path = p("rt/markers.json");
    CHECK(m.save(path, err));
    CHECK(err.empty());

    FreqMarkers back;
    CHECK(back.load(path, err));
    CHECK(back.size() == 2u);
    CHECK(back.list()[0].number == 1);
    CHECK(back.list()[0].freqHz == 145.5e6);
    CHECK(back.list()[0].notedUnix == 1790776867);
    CHECK(back.list()[0].note.empty());
    CHECK(back.list()[1].number == 3);
    CHECK(back.list()[1].freqHz == 433.92e6);
    CHECK(back.list()[1].note == "ISM \xe2\x80\x94 key fob?");
    CHECK(back.add(50.0e6, 0) == 4);  // not 2, not 3

    // No temp-file debris beside it.
    std::size_t entries = 0;
    for (const auto& e : fs::directory_iterator(p("rt"))) {
        (void)e;
        ++entries;
    }
    CHECK(entries == 1u);
}

void testLoadDamage() {
    std::printf("  load: missing is empty and fine; damage is empty and says why; a bad entry is skipped\n");
    std::string err;
    FreqMarkers m;
    m.add(1.0e6, 0);
    CHECK(m.load(p("nope.json"), err));
    CHECK(m.empty() && err.empty());

    CHECK(writeText(p("junk.json"), "not json"));
    m.add(1.0e6, 0);
    CHECK(!m.load(p("junk.json"), err));
    CHECK(m.empty() && !err.empty());

    CHECK(writeText(p("v2.json"), R"({"schemaVersion": 2, "markers": []})"));
    CHECK(!m.load(p("v2.json"), err));
    CHECK(m.empty());

    CHECK(writeText(p("notarr.json"), R"({"schemaVersion": 1, "markers": 5})"));
    CHECK(!m.load(p("notarr.json"), err));

    // Entries: good, no frequency, negative, zero number, duplicate number,
    // not an object, good with a stale "next" below it.
    CHECK(writeText(p("mixed.json"), R"({"schemaVersion": 1, "next": 2, "markers": [
        {"n": 1, "freqHz": 7.1e6, "noted": 5},
        {"n": 2},
        {"n": 3, "freqHz": -1},
        {"n": 0, "freqHz": 7.2e6},
        {"n": 1, "freqHz": 7.3e6},
        "text",
        {"n": 6, "freqHz": 7.4e6, "note": 12}
    ]})"));
    CHECK(m.load(p("mixed.json"), err));
    CHECK(m.size() == 2u);
    CHECK(m.list()[0].number == 1 && m.list()[0].freqHz == 7.1e6 && m.list()[0].notedUnix == 5);
    CHECK(m.list()[1].number == 6 && m.list()[1].note.empty());
    // "next": 2 would reuse a number already in the list; the file cannot do that.
    CHECK(m.add(8.0e6, 0) == 7);
}

// A NUMBER AT THE TOP OF INT cannot wrap the counter. The next number to hand out
// was computed as highest + 1 (a signed overflow for 2147483647) and a "next"
// from the file was taken as it stood, so a hand-edited or damaged file whose
// numbers reached the top of the range made add() return a number the list
// already held: two markers with one number, and remove()/setNote() addressing
// whichever came first. Found by the freq_markers fuzz target's "the number
// just handed out is held by exactly one marker" property.
void testAHugeNumberCannotWrapTheCounter() {
    std::printf("  load: a number at the top of the range is skipped, and add() stays unique\n");
    std::string err;
    FreqMarkers m;
    CHECK(writeText(p("huge.json"),
                    R"({"schemaVersion": 1, "next": 2147483647, "markers": [
        {"n": 2147483647, "freqHz": 7.0e6},
        {"n": 3, "freqHz": 7.1e6}
    ]})"));
    CHECK(m.load(p("huge.json"), err));
    // Skipped like any other damaged entry; the sound one loads.
    CHECK(m.size() == 1u);
    if (m.size() == 1u) { CHECK(m.list()[0].number == 3); }

    // The "next" out of range is not believed either: numbering carries on from
    // what the list holds.
    const int made = m.add(8.0e6, 0);
    CHECK(made == 4);
    int holders = 0;
    for (const auto& marker : m.list()) { holders += (marker.number == made) ? 1 : 0; }
    CHECK(holders == 1);
    // And the one after it: the counter moved on and did not wrap.
    CHECK(m.add(9.0e6, 0) == 5);

    // What was handed out survives a save and a load: the number a marker is
    // given is never one the loader would refuse.
    CHECK(m.save(p("huge_back.json"), err));
    FreqMarkers back;
    CHECK(back.load(p("huge_back.json"), err));
    CHECK(back.size() == m.size());
}

void testRounding() {
    std::printf("  a marker keeps the precision of its pixel, not of the double\n");
    CHECK(cascade::core::markerStepHz(2000.0) == 2000.0);
    CHECK(cascade::core::markerStepHz(1999.0) == 1000.0);
    CHECK(cascade::core::markerStepHz(4999.0) == 2000.0);
    CHECK(cascade::core::markerStepHz(5000.0) == 5000.0);
    CHECK(cascade::core::markerStepHz(9999.0) == 5000.0);
    CHECK(cascade::core::markerStepHz(10000.0) == 10000.0);
    CHECK(cascade::core::markerStepHz(1000.0) == 1000.0);
    CHECK(cascade::core::markerStepHz(0.3) == 1.0);
    CHECK(cascade::core::markerStepHz(-1.0) == 1.0);
    CHECK(cascade::core::markerStepHz(std::nan("")) == 1.0);
    // 2.4 MHz across 1200 px: 2 kHz a pixel.
    CHECK(cascade::core::roundMarkerHz(145'500'912.0, 2000.0) == 145'500'000.0);
    CHECK(cascade::core::roundMarkerHz(145'501'100.0, 2000.0) == 145'502'000.0);
    CHECK(cascade::core::markerMhzDecimals(2000.0) == 3);
    CHECK(cascade::core::markerMhzDecimals(100.0) == 4);
    CHECK(cascade::core::markerMhzDecimals(50.0) == 5);
    CHECK(cascade::core::markerMhzDecimals(1.0) == 6);
    CHECK(cascade::core::markerMhzDecimals(1.0e6) == 3);
    CHECK(cascade::core::formatMarkerMhz(145'502'000.0, 2000.0) == "145.502");
    CHECK(cascade::core::formatMarkerMhz(7'074'050.0, 50.0) == "7.07405");
    CHECK(cascade::core::markerMhzText(145'500'000.0) == "145.500");
    CHECK(cascade::core::markerMhzText(145'502'000.0) == "145.502");
    CHECK(cascade::core::markerMhzText(145'502'500.0) == "145.5025");
    CHECK(cascade::core::markerMhzText(7'074'050.0) == "7.07405");
    CHECK(cascade::core::markerMhzText(7'074'051.0) == "7.074051");
    CHECK(cascade::core::markerMhzText(7'074'050.4) == "7.07405");
}

}  // namespace

int main() {
    g_root = "freq_markers_" + std::to_string(TEST_GETPID());
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    testNumbering();
    testMergeAndRefusals();
    testNearestAndNotes();
    testClipboard();
    testRoundTrip();
    testLoadDamage();
    testAHugeNumberCannotWrapTheCounter();
    testRounding();

    const int rc = testSummary("test_freq_markers");
    if (rc == 0) { fs::remove_all(g_root, ec); }
    return rc;
}
