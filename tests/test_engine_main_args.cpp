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
        // The settings file (stage 7c): a path is required, a flag is not one, two names for it are one too many.
        {{"--config"}, "--config requires the path of a settings file"},
        {{"--config", "--seconds", "1"}, "--config requires the path of a settings file"},
        {{"--config", ""}, "--config requires the path of a settings file"},
        {{"--config", "a.json", "--config", "b.json"}, "--config was given twice"},
        {{"--config", "a.json", "--use-config"}, "--config and --use-config both name the settings file; give one"},
        {{"--use-config", "--config", "a.json"}, "--config and --use-config both name the settings file; give one"},
        // The provisioning flags (stage 7e): a value flag needs the switch it belongs to, wherever on the line it stands.
        {{"--web-bind", "127.0.0.1"}, "--web-bind needs --web-enable"},
        {{"--web-port", "9000"}, "--web-port needs --web-enable"},
        {{"--web-user", "operator"}, "--web-user needs --web-enable"},
        {{"--web-password-file", "pw.txt"}, "--web-password-file needs --web-enable"},
        {{"--web-password-file", "-"}, "--web-password-file needs --web-enable"},
        {{"--cat-bind-all"}, "--cat-bind-all needs --cat-enable"},
        {{"--cat-bind-all", "--web-enable"}, "--cat-bind-all needs --cat-enable"},
        // ...a value that is missing, or is another flag (a forgotten value is not a file called "--cat-enable"), or is wrong.
        {{"--web-enable", "--web-bind"}, "--web-bind requires an address"},
        {{"--web-enable", "--web-bind", "--web-port", "9000"}, "--web-bind requires an address"},
        {{"--web-enable", "--web-bind", ""}, "--web-bind requires an address"},
        {{"--web-enable", "--web-bind", "example.com"}, "invalid --web-bind value 'example.com'"},
        {{"--web-enable", "--web-bind", "010.0.0.1"}, "invalid --web-bind value '010.0.0.1'"},
        {{"--web-enable", "--web-port"}, "--web-port requires a port number"},
        {{"--web-enable", "--web-port", "80"}, "invalid --web-port value '80' (a port from 1024 to 65535)"},
        {{"--web-enable", "--web-port", "65536"}, "invalid --web-port value '65536'"},
        {{"--web-enable", "--web-port", "abc"}, "invalid --web-port value 'abc'"},
        {{"--web-enable", "--web-port", "8073x"}, "invalid --web-port value '8073x'"},
        {{"--web-enable", "--web-port", "0"}, "invalid --web-port value '0'"},
        {{"--web-enable", "--web-user"}, "--web-user requires a user name"},
        {{"--web-enable", "--web-user", "--cat-enable"}, "--web-user requires a user name"},
        {{"--web-enable", "--web-password-file"}, "--web-password-file requires the name of a file holding the password, or - for standard input"},
        {{"--web-enable", "--web-password-file", "--cat-enable"}, "--web-password-file requires the name of a file"},
        // ...and a value given twice.
        {{"--web-enable", "--web-bind", "127.0.0.1", "--web-bind", "::1"}, "--web-bind was given twice"},
        {{"--web-enable", "--web-port", "9000", "--web-port", "9001"}, "--web-port was given twice"},
        {{"--web-enable", "--web-user", "a", "--web-user", "b"}, "--web-user was given twice"},
        {{"--web-enable", "--web-password-file", "a.pw", "--web-password-file", "b.pw"}, "--web-password-file was given twice"},
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
    CHECK(emt::contains(usage.err, "usage: foxsdr-engine [--seconds N] [--config PATH | --use-config] [--safe-plugins] [--selftest] [--version]"));  // (--safe-plugins: plugin containment P1)

    // --- the Host is where it was: it refuses --seconds, as cascade, naming itself ------------------------
    if (fs::exists(cascadeExe)) {
        const emt::RunResult host = emt::run(cascadeExe, {"--seconds", "1"}, home, root / "out", "host-seconds", 120000);
        CHECK(host.exitCode == 1);
        CHECK(emt::contains(host.err, "cascade: unknown argument '--seconds'"));
        CHECK(!emt::contains(host.err, "foxsdr-engine"));
        // ...and so are the engine's settings flags (stage 7c) and its provisioning flags (stage 7e): the Host has no such flag, and
        // says so as it always did.
        for (const std::vector<std::string>& args : {std::vector<std::string>{"--config", "x.json"},
                                                     std::vector<std::string>{"--use-config"},
                                                     std::vector<std::string>{"--web-enable"},
                                                     std::vector<std::string>{"--cat-enable"},
                                                     std::vector<std::string>{"--web-password-file", "-"}}) {
            const emt::RunResult h = emt::run(cascadeExe, args, home, root / "out", "host-" + args.front(), 120000);
            CHECK(h.exitCode == 1);
            CHECK(emt::contains(h.err, "cascade: unknown argument '" + args.front() + "'"));
        }
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
