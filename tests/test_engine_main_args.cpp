// foxsdr-engine's command line and exit codes, and the tool modes it dispatches to
// (engine extraction stage 7b).
//
// The exit codes are cascade's: 0 success, 1 a refused or failed run, 2 a refused
// helper argument. What differs is the surface, by the process kind
// (src/engine/process_boot.hpp): the Engine takes `--seconds N` and refuses the
// window's flags as unknown arguments, naming itself; the Host (cascade) is where it
// always was - it refuses --seconds the way it refuses any flag it does not have.
//
// This runs the real executables. The tool mode is `--selftest`: the engine's own
// pipeline end to end, headless (the bench checks need hardware or a human and are
// not run) - its verdict line is the host's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

int main() {
    std::printf("test_engine_main_args\n");
    const fs::path root = emt::scratch("engine-args");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    const fs::path cascadeExe = emt::appExe("cascade");
    CHECK(fs::exists(engineExe));

    int n = 0;
    const auto engine = [&](const std::vector<std::string>& args) {
        return emt::run(engineExe, args, home, root / "out", "e" + std::to_string(n++), 120000);
    };

    // --- refused arguments: exit 1, a line on stderr that names the engine, nothing on stdout -------------
    struct Refusal { std::vector<std::string> args; const char* mentions; };
    const Refusal refusals[] = {
        {{"--bogus"}, "foxsdr-engine: unknown argument '--bogus'"},
        // The window's flags are the Host's: unknown to an Engine.
        {{"--frames", "3"}, "foxsdr-engine: unknown argument '--frames'"},
        {{"--diag-stall", "5"}, "foxsdr-engine: unknown argument '--diag-stall'"},
        {{"--diag-toggle", "on"}, "foxsdr-engine: unknown argument '--diag-toggle'"},
        {{"--link-tester"}, "foxsdr-engine: unknown argument '--link-tester'"},
        // --seconds, malformed in each way it can be.
        {{"--seconds"}, "--seconds requires a number of seconds"},
        {{"--seconds", "abc"}, "invalid --seconds value 'abc'"},
        {{"--seconds", "-1"}, "invalid --seconds value '-1'"},
        {{"--seconds", "nan"}, "invalid --seconds value 'nan'"},
        {{"--seconds", "inf"}, "invalid --seconds value 'inf'"},
        {{"--seconds", "1e99"}, "invalid --seconds value '1e99'"},
        {{"--seconds", "2s"}, "invalid --seconds value '2s'"},
    };
    for (const Refusal& r : refusals) {
        const emt::RunResult x = engine(r.args);
        std::printf("  engine %s: exit %d, stderr '%s'\n", r.args.front().c_str(), x.exitCode, x.err.c_str());
        CHECK(x.started && !x.timedOut);
        CHECK(x.exitCode == 1);
        CHECK(emt::contains(x.err, r.mentions));
        CHECK(x.out.empty());
        CHECK(x.seconds < 20.0);  // refused at once: the engine was never started
    }

    // --- a usage line that names the engine and its one flag --------------------------------------------
    const emt::RunResult usage = engine({"--bogus"});
    CHECK(emt::contains(usage.err, "usage: foxsdr-engine [--seconds N] [--selftest] [--version]"));

    // --- the Host is where it was: it refuses --seconds, as cascade, naming itself ------------------------
    if (fs::exists(cascadeExe)) {
        const emt::RunResult host = emt::run(cascadeExe, {"--seconds", "1"}, home, root / "out", "host-seconds", 120000);
        CHECK(host.exitCode == 1);
        CHECK(emt::contains(host.err, "cascade: unknown argument '--seconds'"));
        CHECK(!emt::contains(host.err, "foxsdr-engine"));
    } else {
        CHECK(false);  // the application is built before this test (CMake dependency)
    }

    // --- the tool mode: --selftest runs the pipeline headless and answers as cascade's does --------------
    const emt::RunResult self = engine({"--selftest"});
    std::printf("  engine --selftest: exit %d '%s'\n", self.exitCode, self.out.c_str());
    CHECK(self.exitCode == 0);
    CHECK(emt::contains(self.out, "selftest PASS"));
    const emt::RunResult hostSelf = emt::run(cascadeExe, {"--selftest"}, home, root / "out", "host-selftest", 120000);
    CHECK(hostSelf.exitCode == 0);
    // The verdict (the peak bin and the audio tone it found) is the same from either executable: it is the
    // same code. Compared on the PASS line's figures, which are deterministic; timing is not printed.
    CHECK(self.out == hostSelf.out);

    // --- a helper's refusal is exit 2, the engine's own binary re-spawns itself for these (engine_main_helpers) ----
    const emt::RunResult helper = engine({"--enumerate-json", "--bogus"});
    CHECK(helper.exitCode == 2);

    // None of the above wrote anything under the isolated profile or the working directory.
    CHECK(emt::tree(home).size() == 1);  // the "tmp" directory this test made

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_args");
}
