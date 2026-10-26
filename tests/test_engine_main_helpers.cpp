// foxsdr-engine answers the child-process helper modes (engine extraction stage 7b).
//
// WHY THE ENGINE BINARY MUST DO THIS ITSELF. The application re-spawns ITSELF for
// three jobs a fault must not be allowed to take down a session for: the device scan
// (`--enumerate-json`, source/soapy_enum_proc.hpp: a vendor driver faulting mid-probe
// kills a throwaway process instead of the session), the sentinel (`--sentinel`,
// core/sentinel.hpp: the watcher that writes the one report of an ending nothing
// inside the application could report) and the SDRplay probe (`--sdrplay-probe`).
// They find the executable to start as their own (the running image), so in
// `foxsdr-engine` that is foxsdr-engine: if its dispatch did not reach the helper
// code, the engine's first device scan would start a child that answers "unknown
// argument" and exits 1, and the scan would read that as a dead driver.
//
// The proof is that each helper REACHES ITS OWN CODE, not that the line is accepted:
//   * `--enumerate-json --list-drivers` really enumerates: one JSON line on stdout
//     (schema 1, a driver list that is an array) and exit 0 - the walk ran in this
//     process, with the vendor-module search path pointed at a folder with nothing in it;
//   * the helpers' own refusals - exit 2 with the helper's own words (or, for the
//     sentinel, silently), never the parse's "unknown argument" and exit 1 - for
//     `--enumerate-json --bogus`, `--sdrplay-probe` with no file or an option it does not
//     take, `--sdrplay-service-state extra` and `--sentinel` with nothing it needs;
//     (0.99.90 merge: and `--install-winusb`, the one-click driver install's elevated helper - master 0.99.88 put it in main() beside
//     the others, so it is one of runHelperModes' flags; only its refusals are run here, the install itself changes Windows' drivers);
//   * `--sdrplay-service-state` answers (it asks the OS, read-only);
//   * the helper modes arm nothing and touch nothing: no file or directory appears
//     under the profile or the working directory (a helper must not inherit the
//     session's diagnostics or claim a name), and no instance name is taken.
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
    std::printf("test_engine_main_helpers\n");
    const fs::path root = emt::scratch("engine-helpers");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const std::vector<std::string> before = emt::tree(home);

    int n = 0;
    const auto engine = [&](const std::vector<std::string>& args) {
        return emt::run(engineExe, args, home, root / "out", "h" + std::to_string(n++), 120000);
    };
    // Reached the helper, not the argument parse.
    const auto notTheParse = [](const emt::RunResult& r) {
        return !emt::contains(r.err, "unknown argument") && r.exitCode != 1;
    };

    // --- the device scan, for real ------------------------------------------------------------------------
    const emt::RunResult list = engine({"--enumerate-json", "--list-drivers"});
    std::printf("  --enumerate-json --list-drivers: exit %d, stdout '%s'\n", list.exitCode, list.out.c_str());
    CHECK(list.started && !list.timedOut);
    CHECK(list.exitCode == 0);
    CHECK(emt::contains(list.out, "\"schema\":1"));
    CHECK(emt::contains(list.out, "\"drivers\":["));
    CHECK(emt::contains(list.out, "\"devices\":["));
    CHECK(list.out.find('\n') == list.out.size() - 1 || list.out.find('\n') == std::string::npos);  // one line
    CHECK(!emt::contains(list.err, "unknown argument"));

    // --- the helpers' own refusals --------------------------------------------------------------------------
    const emt::RunResult enumBad = engine({"--enumerate-json", "--bogus"});
    CHECK(enumBad.exitCode == 2);
    CHECK(emt::contains(enumBad.err, "--enumerate-json takes only"));
    CHECK(notTheParse(enumBad));

    const emt::RunResult probeNone = engine({"--sdrplay-probe"});
    CHECK(probeNone.exitCode == 2);
    CHECK(emt::contains(probeNone.err, "--sdrplay-probe requires an output file"));
    CHECK(notTheParse(probeNone));

    const emt::RunResult probeBad = engine({"--sdrplay-probe", (home / "probe.txt").string(), "--bogus"});
    CHECK(probeBad.exitCode == 2);
    CHECK(emt::contains(probeBad.err, "--sdrplay-probe takes only an output file"));
    CHECK(notTheParse(probeBad));

    const emt::RunResult stateBad = engine({"--sdrplay-service-state", "extra"});
    CHECK(stateBad.exitCode == 2);
    CHECK(emt::contains(stateBad.err, "--sdrplay-service-state takes no arguments"));
    CHECK(notTheParse(stateBad));

    // The sentinel refuses (exit 2, silently) an argument it was not built to take and a watcher with
    // nothing to watch; it never waits for an application that is not there.
    const emt::RunResult sentinelNone = engine({"--sentinel"});
    CHECK(sentinelNone.exitCode == 2);
    CHECK(notTheParse(sentinelNone));
    const emt::RunResult sentinelBad = engine({"--sentinel", "--bogus"});
    CHECK(sentinelBad.exitCode == 2);
    CHECK(notTheParse(sentinelBad));

    // The one-click driver install's helper (0.99.88): refused (exit 2, its own sentence) for anything but its exact four arguments,
    // BEFORE it touches a device or writes the result file. It never gets as far as a device here.
    const emt::RunResult winusbNone = engine({"--install-winusb"});
    CHECK(winusbNone.exitCode == 2);
    CHECK(emt::contains(winusbNone.err, "usage: --install-winusb <device instance id> --result <file>"));
    CHECK(notTheParse(winusbNone));
    const emt::RunResult winusbBad = engine({"--install-winusb", "-x", "--result", (home / "winusb.txt").string()});
    CHECK(winusbBad.exitCode == 2);
    CHECK(emt::contains(winusbBad.err, "must both be given"));
    CHECK(notTheParse(winusbBad));
    CHECK(!fs::exists(home / "winusb.txt"));

    // --- the service query answers --------------------------------------------------------------------------
    const emt::RunResult state = engine({"--sdrplay-service-state"});
    CHECK(state.exitCode == 0);
    CHECK(emt::contains(state.out, "sdrplay-service: "));
    CHECK(notTheParse(state));

    // --- a helper must be argv[1]: after another flag the same word is an unknown argument, as cascade's is --
    const emt::RunResult late = engine({"--seconds", "0", "--sentinel"});
    CHECK(late.exitCode == 1);
    CHECK(emt::contains(late.err, "unknown argument '--sentinel'"));
    const emt::RunResult lateWinusb = engine({"--seconds", "0", "--install-winusb"});
    CHECK(lateWinusb.exitCode == 1);
    CHECK(emt::contains(lateWinusb.err, "unknown argument '--install-winusb'"));

    // --- and none of it touched anything -------------------------------------------------------------------
    const std::vector<std::string> after = emt::tree(home);
    if (after != before) {
        for (const std::string& f : after) { std::printf("  left behind: %s\n", f.c_str()); }
    }
    CHECK(after == before);

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_helpers");
}
