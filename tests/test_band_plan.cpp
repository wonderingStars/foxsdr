// Tests for core/band_plan.hpp / band_plan.cpp (BandPlan).
//
// Two halves:
//
//   1. DATA tests against the REAL shipped resources/bandplans/*.json. These
//      validate the data files themselves, not just the parser: every band
//      must have end > start, the plans must be non-empty, and — the check
//      that makes the others meaningful — entries().size() must equal the
//      number of elements in the file's raw "bands" array. Without that last
//      one the loader's per-entry tolerance would silently drop a malformed
//      shipped band and the "no end <= start" assertion would pass over data
//      that never reached memory.
//
//   2. BEHAVIOUR tests against synthetic fixtures written in-test, so every
//      expected visible()/at() answer is hand-checkable from a four-band
//      table rather than from 45 real allocations.
//
// LOCATING THE SHIPPED DATA. ctest runs each test executable with its working
// directory set to the test binary's own directory inside the build tree
// (build-<slug>/tests). This agent may not edit any CMakeLists.txt, so there
// is no source-dir compile definition to lean on. Instead the test walks UP
// from the current working directory looking for a
// "resources/bandplans/uk.json" — build directories live inside the source
// tree by project convention (build-<slug>/), so the repo root is found in
// two or three steps. The walk is bounded (8 levels) and, if it finds
// nothing, the test FAILS LOUDLY rather than silently skipping the data
// half — a data test that quietly disappears is worse than no data test.
//
// Synthetic fixtures live in one pid-suffixed directory in the CWD (the
// gitignored build tree), removed on success and left behind on failure for
// autopsy — the same convention as test_config.cpp.
//
// SPDX-License-Identifier: MIT
#include "core/band_plan.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

using cascade::core::BandEntry;
using cascade::core::BandPlan;
namespace fs = std::filesystem;

namespace {

std::string g_root;  // per-process fixture directory, set in main()

std::string p(const char* rel) { return g_root + "/" + rel; }

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

// Walk up from the CWD to the directory that holds resources/bandplans (see
// the header comment). Returns "" when not found.
std::string findShippedDir() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    if (ec) {
        return {};
    }
    for (int level = 0; level < 8; ++level) {
        const fs::path candidate = dir / "resources" / "bandplans";
        if (fs::is_regular_file(candidate / "uk.json", ec)) {
            return candidate.string();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }
    return {};
}

// Number of elements in the file's raw "bands" array — the reference the
// loaded entry count is compared against, so a dropped band is a failure.
std::size_t rawBandCount(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return 0;
    }
    const auto it = j.find("bands");
    if (it == j.end() || !it->is_array()) {
        return 0;
    }
    return it->size();
}

// The class invariant: startHz ascending, ties widest-first.
void checkSorted(const std::vector<BandEntry>& v) {
    for (std::size_t i = 1; i < v.size(); ++i) {
        const bool ordered = (v[i - 1].startHz < v[i].startHz) ||
                             (v[i - 1].startHz == v[i].startHz &&
                              v[i - 1].endHz >= v[i].endHz);
        CHECK(ordered);
    }
}

// Every shipped band must describe a real, non-degenerate range.
void checkDataSane(const BandPlan& plan, const std::string& path) {
    CHECK(!plan.entries().empty());
    CHECK(!plan.name().empty());
    CHECK(plan.entries().size() == rawBandCount(path));  // nothing dropped
    checkSorted(plan.entries());
    for (const BandEntry& e : plan.entries()) {
        CHECK(e.endHz > e.startHz);
        CHECK(e.startHz > 0.0);
        CHECK(!e.name.empty());
        CHECK(!e.service.empty());
        CHECK(e.colorRgba == BandPlan::colorForService(e.service));
        CHECK((e.colorRgba & 0xFFu) == 0x60u);  // uniform overlay alpha
    }
}

// Names of visible() results, in order, as one joinable string — makes an
// expected set readable in the assertion.
std::string visibleNames(const BandPlan& plan, double lowHz, double highHz) {
    std::string s;
    for (const BandEntry* e : plan.visible(lowHz, highHz)) {
        if (!s.empty()) {
            s += ",";
        }
        s += e->name;
    }
    return s;
}

const char* atName(const BandPlan& plan, double hz) {
    const BandEntry* e = plan.at(hz);
    return e ? e->name.c_str() : "<null>";
}

// Four bands with a partial overlap (A/C), an edge-touching neighbour (A/B)
// and an isolated far band (D) — every visible() case is hand-checkable.
const char* kOverlapPlan = R"({
    "name": "Overlap Fixture",
    "bands": [
        { "start": 100, "end": 200, "name": "A", "service": "broadcast" },
        { "start": 200, "end": 300, "name": "B", "service": "amateur" },
        { "start": 150, "end": 250, "name": "C", "service": "marine" },
        { "start": 1000, "end": 2000, "name": "D", "service": "mobile" }
    ]
})";

// Three nested ranges plus two equal-width neighbours sharing an edge.
const char* kNestedPlan = R"({
    "name": "Nested Fixture",
    "bands": [
        { "start": 1000, "end": 2000, "name": "outer", "service": "aviation" },
        { "start": 1400, "end": 1500, "name": "inner", "service": "iss" },
        { "start": 1200, "end": 1800, "name": "mid", "service": "satellite" },
        { "start": 3000, "end": 4000, "name": "left", "service": "mobile" },
        { "start": 4000, "end": 5000, "name": "right", "service": "mobile" }
    ]
})";

}  // namespace

int main() {
    g_root = "bandplan_test_" + std::to_string(TEST_GETPID());
    fs::remove_all(g_root);  // stale debris from a failed prior run
    fs::create_directory(g_root);

    // --- defaultDir: documented shape, no filesystem side effects -----------
    {
        const std::string dd = BandPlan::defaultDir();
        CHECK(!dd.empty());
        CHECK(dd.find("resources") != std::string::npos);
        CHECK(dd.ends_with("bandplans"));
        CHECK(!fs::exists(dd) || fs::is_directory(dd));  // never a plain file
    }

    // --- the palette, including the documented fallback ---------------------
    {
        CHECK(BandPlan::colorForService("broadcast") == 0xE69F0060u);
        CHECK(BandPlan::colorForService("amateur") == 0x56B4E960u);
        CHECK(BandPlan::colorForService("aviation") == 0x009E7360u);
        CHECK(BandPlan::colorForService("marine") == 0x0072B260u);
        CHECK(BandPlan::colorForService("mobile") == 0xD55E0060u);
        CHECK(BandPlan::colorForService("satellite") == 0xCC79A760u);
        CHECK(BandPlan::colorForService("iss") == 0xF0E44260u);
        // Documented fallback: "other", empty, and anything unrecognised.
        CHECK(BandPlan::colorForService("other") == 0x9E9E9E60u);
        CHECK(BandPlan::colorForService("") == 0x9E9E9E60u);
        CHECK(BandPlan::colorForService("quantum-telepathy") == 0x9E9E9E60u);
    }

    // --- THE SHIPPED DATA ---------------------------------------------------
    const std::string shipped = findShippedDir();
    CHECK(!shipped.empty());  // loud failure, never a silent skip
    if (!shipped.empty()) {
        const std::string ukPath = shipped + "/uk.json";
        const std::string r1Path = shipped + "/itu-region1.json";

        BandPlan uk;
        std::string err = "stale";
        CHECK(uk.loadFile(ukPath, err));
        CHECK(err.empty());
        CHECK(uk.name() == "United Kingdom");
        checkDataSane(uk, ukPath);

        BandPlan r1;
        CHECK(r1.loadFile(r1Path, err));
        CHECK(err.empty());
        CHECK(r1.name() == "ITU Region 1");
        checkDataSane(r1, r1Path);

        // Spot-checks that the data says what a UK listener expects, and that
        // at() picks the narrowest match on REAL nested allocations.
        CHECK(std::string(atName(uk, 100000000.0)) == "FM Broadcast");
        CHECK(std::string(atName(uk, 110000000.0)) == "VOR / ILS Navigation");
        CHECK(std::string(atName(uk, 120000000.0)) == "Airband Voice (AM)");
        CHECK(std::string(atName(uk, 145800000.0)) == "ISS Downlink (145.800 MHz)");
        CHECK(std::string(atName(uk, 145500000.0)) == "2 m Amateur");
        CHECK(std::string(atName(uk, 1090000000.0)) == "ADS-B (1090 MHz)");
        // 600 MHz is unallocated in this plan: a genuine gap.
        CHECK(uk.at(600000000.0) == nullptr);

        // --- loadDirectory merges both files and keeps the sort invariant ---
        BandPlan merged;
        CHECK(merged.loadDirectory(shipped, err));
        CHECK(err.empty());
        CHECK(merged.entries().size() == uk.entries().size() + r1.entries().size());
        checkSorted(merged.entries());
        // Lexicographic file order => deterministic joined name.
        CHECK(merged.name() == "ITU Region 1 + United Kingdom");
        // The narrowest-wins rule still holds across the merge.
        CHECK(std::string(atName(merged, 145800000.0)) == "ISS Downlink (145.800 MHz)");

        // clear() empties both halves of the state.
        merged.clear();
        CHECK(merged.entries().empty());
        CHECK(merged.name().empty());
        CHECK(merged.at(100000000.0) == nullptr);
        CHECK(merged.visible(0.0, 1e12).empty());
    }

    // --- visible(): hand-checked windows, both partial-overlap edges --------
    {
        const std::string path = p("overlap.json");
        CHECK(writeText(path, kOverlapPlan));
        BandPlan plan;
        std::string err;
        CHECK(plan.loadFile(path, err));
        CHECK(plan.entries().size() == 4u);
        // Sorted by start: A(100) C(150) B(200) D(1000).
        CHECK(plan.entries()[0].name == "A");
        CHECK(plan.entries()[1].name == "C");
        CHECK(plan.entries()[2].name == "B");
        CHECK(plan.entries()[3].name == "D");

        CHECK(visibleNames(plan, 140.0, 160.0) == "A,C");     // window inside both
        CHECK(visibleNames(plan, 50.0, 120.0) == "A");        // partial at LOW edge
        CHECK(visibleNames(plan, 280.0, 1500.0) == "B,D");    // partial at HIGH edge
        CHECK(visibleNames(plan, 0.0, 5000.0) == "A,C,B,D");  // everything
        CHECK(visibleNames(plan, 400.0, 900.0) == "");        // gap between C and D
        // Strict edges: touching only at a point covers zero pixels.
        CHECK(visibleNames(plan, 0.0, 100.0) == "");
        CHECK(visibleNames(plan, 300.0, 900.0) == "");
        CHECK(visibleNames(plan, 99.0, 101.0) == "A");
        // Degenerate windows.
        CHECK(plan.visible(500.0, 500.0).empty());  // empty window
        CHECK(plan.visible(600.0, 400.0).empty());  // inverted window

        // The returned pointers alias the entries vector, in entries() order.
        const auto vis = plan.visible(140.0, 160.0);
        CHECK(vis.size() == 2u);
        CHECK(vis[0] == &plan.entries()[0]);
        CHECK(vis[1] == &plan.entries()[1]);
    }

    // --- at(): narrowest containing entry, nullptr in a gap -----------------
    {
        const std::string path = p("nested.json");
        CHECK(writeText(path, kNestedPlan));
        BandPlan plan;
        std::string err;
        CHECK(plan.loadFile(path, err));
        CHECK(plan.entries().size() == 5u);

        CHECK(std::string(atName(plan, 1450.0)) == "inner");  // all three nest here
        CHECK(std::string(atName(plan, 1300.0)) == "mid");    // outer + mid
        CHECK(std::string(atName(plan, 1900.0)) == "outer");  // outer only
        CHECK(std::string(atName(plan, 1200.0)) == "mid");    // edge of mid, inclusive
        CHECK(std::string(atName(plan, 1000.0)) == "outer");  // edge of outer
        CHECK(std::string(atName(plan, 2000.0)) == "outer");  // upper edge, inclusive
        CHECK(plan.at(2500.0) == nullptr);                    // gap
        CHECK(plan.at(999.0) == nullptr);                     // below everything
        CHECK(plan.at(6000.0) == nullptr);                    // above everything
        // Equal-width neighbours sharing an edge: first in entries() order.
        CHECK(std::string(atName(plan, 4000.0)) == "left");
        CHECK(std::string(atName(plan, 3500.0)) == "left");
        CHECK(std::string(atName(plan, 4500.0)) == "right");
    }

    // --- malformed JSON: false + error, contents UNCHANGED ------------------
    {
        BandPlan plan;
        std::string err;
        CHECK(plan.loadFile(p("overlap.json"), err));
        const std::size_t before = plan.entries().size();
        const std::string nameBefore = plan.name();
        CHECK(before == 4u);

        const std::string bad = p("corrupt.json");
        CHECK(writeText(bad, "{ \"bands\": [ this is not json"));
        err.clear();
        CHECK(!plan.loadFile(bad, err));
        CHECK(!err.empty());
        CHECK(plan.entries().size() == before);
        CHECK(plan.name() == nameBefore);
        CHECK(plan.entries()[0].name == "A");  // same content, not just count

        // Valid JSON, non-object root: also a failure, also unchanged.
        const std::string arrRoot = p("array_root.json");
        CHECK(writeText(arrRoot, "[1,2,3]\n"));
        err.clear();
        CHECK(!plan.loadFile(arrRoot, err));
        CHECK(!err.empty());
        CHECK(plan.entries().size() == before);
        CHECK(plan.name() == nameBefore);

        // "bands" present but not an array: structural damage, unchanged.
        const std::string badBands = p("bands_not_array.json");
        CHECK(writeText(badBands, "{\"name\":\"X\",\"bands\":\"nope\"}\n"));
        err.clear();
        CHECK(!plan.loadFile(badBands, err));
        CHECK(!err.empty());
        CHECK(plan.entries().size() == before);
        CHECK(plan.name() == nameBefore);

        // Missing file: unreadable, so also a failure that changes nothing.
        err.clear();
        CHECK(!plan.loadFile(p("no_such_file.json"), err));
        CHECK(!err.empty());
        CHECK(plan.entries().size() == before);
    }

    // --- missing / unknown service -> documented fallback colour ------------
    {
        const std::string path = p("services.json");
        CHECK(writeText(path, R"({
            "bands": [
                { "start": 10, "end": 20, "name": "no service key" },
                { "start": 30, "end": 40, "name": "unknown", "service": "teleportation" },
                { "start": 50, "end": 60, "name": "wrong type", "service": 42 },
                { "start": 70, "end": 80, "name": "known", "service": "marine" }
            ]
        })"));
        BandPlan plan;
        std::string err;
        CHECK(plan.loadFile(path, err));
        CHECK(plan.entries().size() == 4u);
        CHECK(plan.entries()[0].service.empty());
        CHECK(plan.entries()[0].colorRgba == 0x9E9E9E60u);
        CHECK(plan.entries()[1].service == "teleportation");
        CHECK(plan.entries()[1].colorRgba == 0x9E9E9E60u);
        CHECK(plan.entries()[2].service.empty());  // wrong type == absent
        CHECK(plan.entries()[2].colorRgba == 0x9E9E9E60u);
        CHECK(plan.entries()[3].colorRgba == 0x0072B260u);
        // No "name" key on the file: the stem becomes the plan name.
        CHECK(plan.name() == "services");
    }

    // --- per-entry tolerance: damaged bands dropped, the rest survive -------
    {
        const std::string path = p("damaged.json");
        CHECK(writeText(path, R"({
            "name": "Damaged",
            "bands": [
                { "start": 100, "end": 100, "name": "zero width" },
                { "start": 400, "end": 300, "name": "inverted" },
                { "start": 500, "name": "no end" },
                { "end": 600, "name": "no start" },
                { "start": "low", "end": 700, "name": "non-numeric start" },
                "not an object",
                12345,
                { "start": 800, "end": 900, "name": "good", "service": "amateur" }
            ]
        })"));
        BandPlan plan;
        std::string err;
        CHECK(plan.loadFile(path, err));
        CHECK(err.empty());  // per-entry damage is tolerated, not reported
        CHECK(plan.entries().size() == 1u);
        CHECK(plan.entries()[0].name == "good");
        CHECK(plan.name() == "Damaged");
    }

    // --- object with no "bands": a valid, empty plan ------------------------
    {
        const std::string path = p("no_bands.json");
        CHECK(writeText(path, "{\"name\":\"Empty Plan\"}\n"));
        BandPlan plan;
        std::string err = "stale";
        CHECK(plan.loadFile(path, err));
        CHECK(err.empty());
        CHECK(plan.entries().empty());
        CHECK(plan.name() == "Empty Plan");
    }

    // --- loadDirectory failure modes ----------------------------------------
    {
        // Missing directory.
        BandPlan plan;
        std::string err;
        CHECK(!plan.loadDirectory(p("no_such_dir"), err));
        CHECK(!err.empty());

        // A file where a directory is expected.
        CHECK(!plan.loadDirectory(p("no_bands.json"), err));
        CHECK(!err.empty());

        // Directory with no *.json: a legitimately empty plan.
        const std::string emptyDir = p("empty_dir");
        fs::create_directory(emptyDir);
        CHECK(writeText(emptyDir + "/readme.txt", "not a plan\n"));
        err = "stale";
        CHECK(plan.loadDirectory(emptyDir, err));
        CHECK(err.empty());
        CHECK(plan.entries().empty());
        CHECK(plan.name().empty());

        // ALL-OR-NOTHING: one bad file aborts the merge and changes nothing.
        const std::string mixed = p("mixed_dir");
        fs::create_directory(mixed);
        CHECK(writeText(mixed + "/a_good.json", kOverlapPlan));
        CHECK(writeText(mixed + "/z_bad.json", "{ broken"));
        BandPlan keeper;
        CHECK(keeper.loadFile(p("nested.json"), err));
        const std::size_t before = keeper.entries().size();
        err.clear();
        CHECK(!keeper.loadDirectory(mixed, err));
        CHECK(!err.empty());
        CHECK(err.find("z_bad.json") != std::string::npos);  // names the file
        CHECK(keeper.entries().size() == before);
        CHECK(keeper.name() == "Nested Fixture");

        // With only the good file present the same directory merges cleanly.
        fs::remove(mixed + "/z_bad.json");
        CHECK(keeper.loadDirectory(mixed, err));
        CHECK(keeper.entries().size() == 4u);
        CHECK(keeper.name() == "Overlap Fixture");
    }

    const int rc = testSummary("test_band_plan");
    if (rc == 0) {
        std::error_code ec;
        fs::remove_all(g_root, ec);  // success: leave nothing behind
    }
    return rc;
}
