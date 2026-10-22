// foxsdr-engine --version prints exactly what cascade --version prints (engine
// extraction stage 7b).
//
// WHY BYTE FOR BYTE. `--version` is a contract, not a convenience: the release and
// nightly scripts read it back to check that the binary agrees with the version its
// installer is about to be named after (docs/RELEASING.md, the `build` step), and the
// engine ships beside the window application from the same build. A second
// executable that called itself something else, or added a line, or went to the wrong
// stream, would make that check read two different answers from one release.
//
// Both real executables are run (CASCADE_APP_BINDIR, built first), standard output and
// standard error kept apart: the version is ONE line on stdout and nothing at all on
// stderr, for both, and the two are equal. It is compared with what the library says
// the version is as well, so "equal to cascade" cannot hide two binaries that are
// equally wrong (an unconfigured build reports "0.0.0-unconfigured").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "core/version.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

int main() {
    std::printf("test_engine_main_version\n");
    const fs::path root = emt::scratch("engine-version");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);

    const fs::path engineExe = emt::appExe("foxsdr-engine");
    const fs::path cascadeExe = emt::appExe("cascade");
    CHECK(fs::exists(engineExe));
    CHECK(fs::exists(cascadeExe));

    const std::string expected = std::string(cascade::appName()) + " " + cascade::versionString() + "\n";
    CHECK(expected.find("unconfigured") == std::string::npos);

    const emt::RunResult e = emt::run(engineExe, {"--version"}, home, root / "out", "engine-version");
    const emt::RunResult c = emt::run(cascadeExe, {"--version"}, home, root / "out", "cascade-version");
    std::printf("  engine: exit %d stdout '%s' stderr '%s'\n", e.exitCode, e.out.c_str(), e.err.c_str());
    std::printf("  cascade: exit %d stdout '%s' stderr '%s'\n", c.exitCode, c.out.c_str(), c.err.c_str());
    CHECK(e.started && !e.timedOut);
    CHECK(c.started && !c.timedOut);
    CHECK(e.exitCode == 0);
    CHECK(c.exitCode == 0);
    // ONE line, on stdout, and nothing on stderr.
    CHECK(e.out == expected);
    CHECK(c.out == expected);
    CHECK(e.err.empty());
    CHECK(c.err.empty());
    // And equal to each other: the point.
    CHECK(e.out == c.out);

    // --version answers wherever it is on the line, as cascade's does: an earlier
    // flag that takes a value does not swallow it, and nothing before it runs the engine.
    const emt::RunResult after = emt::run(engineExe, {"--seconds", "30", "--version"}, home, root / "out", "engine-after");
    CHECK(after.exitCode == 0);
    CHECK(after.out == expected);
    CHECK(after.seconds < 20.0);  // it did not run the engine for the 30 seconds before it answering

    // Nothing was written anywhere under the isolated profile or the working directory.
    CHECK(emt::tree(home).size() == 1);  // the "tmp" directory this test made

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_version");
}
