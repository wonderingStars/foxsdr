// Structural policy test: no nlohmann `.dump(` call in src/ may be allowed to
// throw on third-party text.
//
// Background (field crash 6477BA87): the SoapySDR enumeration child died on
// `j.dump()` because a vendor device's descriptor string carried a byte that
// was not valid UTF-8 - nlohmann::json's dump() throws type_error.316 by
// default when it meets that. Nothing in the child caught it, so it died of
// an unhandled C++ exception, and the OS reported that death as exit code
// 0xE06D7363 (the generic "unhandled Microsoft C++ exception" magic number -
// not specific to any one exception type). That is the SAME exit code an
// actual libusb/backend fault produces, so for weeks the crash was filed and
// dismissed as "the known libusb fault, already contained" - an ordinary
// encoding bug in our own serialiser, wearing somebody else's signature.
//
// telemetry.cpp and web_server.cpp already pass
// `nlohmann::json::error_handler_t::replace` on every dump() (the latter
// because an unserialisable plugin name once took the whole browser UI down
// too). soapy_enum_proc.cpp was the third site and has since been fixed the
// same way. This test exists so a fourth site is never written: it scans
// every .cpp/.hpp file under src/ for `.dump(` and requires
// `error_handler_t::replace` to appear in the same statement.
//
// Heuristic and its limits: this is a textual scan, not a parse. For each
// `.dump(` occurrence it looks for the guard within a bounded lookahead - up
// to the statement's terminating `;`, capped at ~300 characters so one
// malformed or very long statement cannot make the scan run away. src/
// contains no vendored or generated code (the lane rule this test file lives
// under says so explicitly), and every dump() call in this codebase is a
// short, single-statement call, so a fixed short window is a deliberate,
// adequate trade-off rather than a full C++ parse. It would misjudge a
// contrived case such as a `.dump(` more than ~300 characters from its own
// semicolon, or one appearing inside a comment or string literal that reads
// as real code; neither occurs anywhere in src/ today (checked by hand
// against every current call site before writing this test).
//
// Only *.cpp and *.hpp are scanned, per the lane spec this test was written
// under ("skip nothing else - src/ contains no vendored code"); src/ has
// exactly one *.h file (core/plugin_abi.h, the stable C plugin ABI) and it
// contains no dump() call, so nothing is lost by that scoping.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kDumpToken = ".dump(";
constexpr const char* kGuardToken = "error_handler_t::replace";
// ~300 characters, per the lane spec: enough to cover this codebase's dump()
// calls (all of which fit on one or two lines) without letting an unrelated
// later statement's guard text get credited to an earlier, unguarded call.
constexpr std::size_t kLookaheadCap = 300;

struct Violation {
    std::string path;
    std::size_t offset;
    std::size_t line;
};

std::string readWholeFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

// 1-based line number of `offset` within `text`, for a human-readable report
// alongside the byte offset the lane spec asks for.
std::size_t lineNumberAt(const std::string& text, std::size_t offset) {
    return 1 + static_cast<std::size_t>(
                   std::count(text.begin(), text.begin() + static_cast<long>(offset), '\n'));
}

// Scans one file's contents for `.dump(` sites, appending any without the
// guard within the lookahead window to `violations`. Returns the number of
// dump sites found in this file (guarded or not), for the overall >=10 count.
std::size_t scanFile(const fs::path& path, const std::string& text,
                      std::vector<Violation>& violations) {
    std::size_t sitesFound = 0;
    std::size_t searchFrom = 0;
    const std::size_t dumpTokenLen = std::string(kDumpToken).size();

    while (true) {
        const std::size_t pos = text.find(kDumpToken, searchFrom);
        if (pos == std::string::npos) { break; }
        ++sitesFound;

        // The lookahead window ends at the statement's terminating semicolon
        // if one falls within the cap, otherwise at the cap itself - a
        // statement that runs longer than that without a semicolon is past
        // what this heuristic can vouch for (see the file-header comment).
        std::size_t windowEnd = std::min(pos + kLookaheadCap, text.size());
        const std::size_t semi = text.find(';', pos);
        if (semi != std::string::npos && semi < windowEnd) { windowEnd = semi + 1; }

        const std::string window = text.substr(pos, windowEnd - pos);
        if (window.find(kGuardToken) == std::string::npos) {
            violations.push_back(Violation{path.string(), pos, lineNumberAt(text, pos)});
        }

        searchFrom = pos + dumpTokenLen;
    }
    return sitesFound;
}

}  // namespace

int main(int argc, char** argv) {
    // The normal path is ctest passing the absolute src/ directory as
    // argv[1] (see tests/CMakeLists.txt, which special-cases this one test's
    // registration to supply it). Falling back to CASCADE_SOURCE_DIR - the
    // same compile-time definition tests/test_diagnostics.cpp and
    // tests/test_crash_upload.cpp already use, propagated PUBLIC from
    // cascade_lib into every test binary - keeps this runnable standalone
    // with no arguments at all, e.g. from an IDE's "run test" button.
    const fs::path srcDir = (argc >= 2) ? fs::path(argv[1]) : fs::path(CASCADE_SOURCE_DIR) / "src";

    std::vector<Violation> violations;
    std::size_t totalSites = 0;
    std::size_t filesScanned = 0;

    std::error_code ec;
    fs::recursive_directory_iterator it(srcDir, fs::directory_options::none, ec);
    const fs::recursive_directory_iterator end;
    CHECK(!ec);
    for (; it != end && !ec; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        if (!entry.is_regular_file()) { continue; }
        const fs::path& p = entry.path();
        if (p.extension() != ".cpp" && p.extension() != ".hpp") { continue; }
        ++filesScanned;
        const std::string text = readWholeFile(p);
        totalSites += scanFile(p, text, violations);
    }

    for (const Violation& v : violations) {
        std::printf("VIOLATION (dump() without error_handler_t::replace within %zu chars): %s:%zu (line %zu)\n",
                    kLookaheadCap, v.path.c_str(), v.offset, v.line);
    }

    std::printf("test_json_dump_policy: scanned %zu files, %zu dump() sites, %zu violation(s)\n",
                filesScanned, totalSites, violations.size());

    // A wrong path (or a future nlohmann API rename away from `.dump(`)
    // would make this test scan zero real sites and pass vacuously with an
    // empty violations list - which proves nothing. Pin a floor at the
    // current known count (17 as of the fix this test guards, comfortably
    // above the >=10 the lane spec asks for) so that failure mode is loud
    // instead of silent.
    CHECK(totalSites >= 10);

    CHECK(violations.empty());

    return testSummary("test_json_dump_policy");
}
