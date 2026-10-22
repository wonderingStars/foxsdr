// What the build puts beside foxsdr-engine, and what it keeps of it (engine extraction stage 7g).
//
// The headless engine is a second executable, and a second executable has two things the first one
// gets from the build and a test of the first one never looks at for it:
//
//   1. ITS RESOURCES. BandPlan::defaultDir() and aorFirmwarePath() are executable-relative, and
//      both are engine code: an engine with no resources/bandplans beside it has an empty band-plan
//      overlay, and one with no resources/firmware cannot open an AOR receiver. They are staged by
//      one CMake target (cascade_stage_resources) that both executables depend on, so that two
//      POST_BUILD copies of one tree cannot race in a parallel build. This test reads the files
//      beside the built engine and compares every one with the source tree's, byte for byte, and
//      checks that "beside the engine" is "beside cascade" (both executables are in one directory:
//      the tarballs and the staging both rely on it).
//   2. ITS SYMBOLS. A crash report names the module and its build id; the module is resolved
//      against symbols/ by that id, and a new binary has its own. The POST_BUILD step on
//      foxsdr-engine archives it exactly as cascade's is archived (tools/archive-symbols*.{ps1,sh}),
//      so the archive must hold THIS build's engine under ITS id - distinct from cascade's - with its
//      symbol map and an index row naming the engine module. tests/test_diagnostics.cpp asserts the
//      same of cascade; before this stage nothing asserted it of the engine, and an archive that was
//      only ever cascade's would have shown every test green while a headless crash was unreadable.
//
// THE OFF BUILD (CASCADE_BUILD_INTERFACE=OFF) has no tests; its configure and its staging are held by
// tests/engine_only_configure.cmake instead.
//
// UNVERIFIED on a real Linux machine: the POSIX half reads the build id with readelf and resolves a
// function from the map with python3, the way test_diagnostics does for cascade; it has been run under
// WSL2 only (docs/engine-stage4.md section 6.20).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/diag_report.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

namespace {

// Every regular file under `dir`, as a path relative to it.
std::vector<fs::path> filesUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file()) { out.push_back(fs::relative(it->path(), dir)); }
    }
    return out;
}

#if !defined(_WIN32)
// The GNU build id an independent readelf reads off an ELF file: not a value this test could take from
// the build's own idea of it (the failure mode the check exists to catch).
std::string readelfBuildId(const fs::path& exe) {
    const std::string cmd = "readelf -n \"" + exe.string() + "\" 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    std::string out;
    if (p != nullptr) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
        pclose(p);
    }
    const std::size_t at = out.find("Build ID: ");
    if (at == std::string::npos) { return {}; }
    std::size_t end = at + 10;
    while (end < out.size() && std::isxdigit(static_cast<unsigned char>(out[end])) != 0) { ++end; }
    return out.substr(at + 10, end - (at + 10));
}
#endif

// The tab-separated columns of the first row of the archive's index that carries `id` (empty when none).
std::vector<std::string> indexRowFor(const fs::path& index, const std::string& id) {
    std::vector<std::string> cols;
    const std::string text = emt::readText(index);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::string row = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        if (row.find("\t" + id + "\t") == std::string::npos) { continue; }
        std::size_t at = 0;
        while (true) {
            const std::size_t tab = row.find('\t', at);
            cols.push_back(row.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
            if (tab == std::string::npos) { break; }
            at = tab + 1;
        }
        break;
    }
    return cols;
}

}  // namespace

int main() {
    std::printf("test_engine_packaging\n");
    const fs::path src = fs::path(CASCADE_SOURCE_DIR);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    const fs::path cascadeExe = emt::appExe("cascade");
    CHECK(fs::exists(engineExe));
    CHECK(fs::exists(cascadeExe));

    // --- 1. one directory ---------------------------------------------------------------------------
    // The directory the build put foxsdr-engine in (a compile definition the test's CMake gives it) is
    // the one cascade is in (CASCADE_APP_BINDIR): the staging below is "beside both", and a tarball that
    // puts the two binaries in one directory is how they are shipped.
    {
        std::error_code a, b;
        const fs::path engineDir = fs::weakly_canonical(fs::path(FOXSDR_ENGINE_BINDIR), a);
        const fs::path appDir = fs::weakly_canonical(fs::path(CASCADE_APP_BINDIR), b);
        std::printf("  engine dir %s\n  app dir    %s\n", engineDir.string().c_str(), appDir.string().c_str());
        CHECK(!a && !b);
        CHECK(engineDir == appDir);
        CHECK(engineExe.parent_path() == fs::path(CASCADE_APP_BINDIR));
    }

    // --- 2. the resources, beside the engine --------------------------------------------------------------
    for (const char* dir : {"bandplans", "firmware"}) {
        const fs::path from = src / "resources" / dir;
        const fs::path to = engineExe.parent_path() / "resources" / dir;
        const std::vector<fs::path> want = filesUnder(from);
        std::printf("  resources/%s: %zu source file(s)\n", dir, want.size());
        CHECK(!want.empty());
        std::size_t landed = 0;
        for (const fs::path& rel : want) {
            const std::string a = emt::readFile(from / rel);
            const std::string b = emt::readFile(to / rel);
            const bool same = fs::exists(to / rel) && a == b;
            if (!same) { std::printf("  NOT STAGED, or differs: %s\n", (to / rel).string().c_str()); }
            CHECK(same);
            if (same) { ++landed; }
        }
        CHECK(landed == want.size());
    }
#if defined(_WIN32)
    {
        // the one runtime DLL the engine needs beside it (delay-loaded, as cascade's is)
        std::error_code ec;
        const fs::path dll = engineExe.parent_path() / "SoapySDR.dll";
        CHECK(fs::exists(dll) && fs::file_size(dll, ec) > 0 && !ec);
    }
#endif

    // --- 3. the symbols: the engine's own build id, archived ------------------------------------------------
    const fs::path archive = fs::path(CASCADE_SYMBOL_ARCHIVE_DIR);
    const fs::path index = archive / "index.txt";
#if defined(_WIN32)
    {
        std::string engineId, enginePdb, cascadeId, cascadePdb;
        CHECK(cascade::core::peBuildId(engineExe.string(), engineId, enginePdb));
        CHECK(cascade::core::peBuildId(cascadeExe.string(), cascadeId, cascadePdb));
        CHECK(!engineId.empty());
        // its own: a build id shared with cascade would mean one binary was archived under the other's
        CHECK(engineId != cascadeId);
        CHECK(enginePdb != cascadePdb);
        CHECK(enginePdb.find("foxsdr-engine") != std::string::npos);

        const fs::path pdb = archive / enginePdb / engineId / enginePdb;
        std::printf("  engine pdb archive: %s\n", pdb.string().c_str());
        std::error_code ec1, ec2;
        CHECK(fs::exists(pdb) && fs::file_size(pdb, ec1) > 0 && !ec1);
        const fs::path map = archive / enginePdb / engineId / "symmap.json.gz";
        CHECK(fs::exists(map) && fs::file_size(map, ec2) > 0 && !ec2);

        const std::vector<std::string> row = indexRowFor(index, engineId);
        std::printf("  index row: %zu column(s)\n", row.size());
        CHECK(row.size() == 6u);
        CHECK(row.size() == 6u && row[1] == "foxsdr-engine.exe");
        CHECK(row.size() == 6u && row[4] == engineId);
        CHECK(row.size() == 6u && row[5] == enginePdb);
    }
#else
    {
        const std::string engineId = readelfBuildId(engineExe);
        const std::string cascadeId = readelfBuildId(cascadeExe);
        CHECK(!engineId.empty());
        CHECK(engineId != cascadeId);

        const fs::path module = archive / "foxsdr-engine" / engineId / "foxsdr-engine";
        const fs::path debug = archive / "foxsdr-engine.debug" / engineId / "foxsdr-engine.debug";
        const fs::path map = archive / "foxsdr-engine.debug" / engineId / "symmap.json.gz";
        std::printf("  engine module archive: %s\n", module.string().c_str());
        std::error_code e1, e2, e3;
        CHECK(fs::exists(module) && fs::file_size(module, e1) > 0 && !e1);
        CHECK(fs::exists(debug) && fs::file_size(debug, e2) > 0 && !e2);
        CHECK(fs::exists(map) && fs::file_size(map, e3) > 0 && !e3);
        // the map really names a function of this binary: boot is the engine's own
        const std::string resolve =
            "python3 -c \"import gzip,json,sys\n"
            "d = json.load(gzip.open('" + map.string() + "'))\n"
            "sys.exit(0 if any('bootProcess' in f[2] for f in d['functions']) else 1)\"";
        CHECK(std::system(resolve.c_str()) == 0);

        const std::vector<std::string> row = indexRowFor(index, engineId);
        std::printf("  index row: %zu column(s)\n", row.size());
        CHECK(row.size() == 6u);
        CHECK(row.size() == 6u && row[1] == "foxsdr-engine");
        CHECK(row.size() == 6u && row[4] == engineId);
    }
#endif

    return testSummary("test_engine_packaging");
}
