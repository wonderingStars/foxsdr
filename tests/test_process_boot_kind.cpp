// THE KIND OF A PROCESS, run as each kind and checked on the machine.
//
// bootProcess(Host|Engine) is the one place the instance claim and the beta-link
// activation happen (engine extraction stage 7a; docs/engine-merge-0.99.84.md,
// TESTER-LINK). The ledger's requirement: a Host claims `Local\FoxSDR-instance`
// and hands a foxsdr:// link over by writing the link-request file; an ENGINE
// does neither - it must not claim the name (a headless service holding it would
// turn a tester's link click into a file nobody polls) and must not write the
// file. tests/test_process_boot_order.cpp pins the gates from the text; this runs
// the real boot in this process, once per kind, and looks:
//
//   * the Engine boot returns "go on", has `primaryInstance == false`, left NO
//     link-request file, and the instance name it was run under is STILL FREE
//     (a claim made here afterwards succeeds - nobody held it);
//   * the Host boot claims (`primaryInstance == true`), writes the link-request
//     file, and the name is TAKEN (a claim made here afterwards fails);
//   * both consume a foxsdr: argv[1] so the parse never sees it (no
//     "unknown argument" exit, which would echo the token);
//   * the kind-blind modes answer the same for both: --version exits 0, an
//     unknown argument exits 1.
//
// Isolation: APPDATA / XDG_CONFIG_HOME point at a scratch directory (the link
// file lands there, never in the real profile), the instance names are private
// scopes (FOXSDR_INSTANCE_SCOPE, honoured with CASCADE_CONFIG_TEST on a bounded
// --frames run), and the run is argc > 1 so no diagnostics reach the disk.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/tester_link.hpp"
#include "engine/process_boot.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::engine::BootResult;
using cascade::engine::ProcessKind;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unsetEnv(const char* name) {
#if defined(_WIN32)
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

BootResult boot(ProcessKind kind, std::vector<std::string> args) {
    std::vector<char*> argv;
    for (std::string& a : args) { argv.push_back(a.data()); }
    argv.push_back(nullptr);
    return cascade::engine::bootProcess(kind, static_cast<int>(args.size()), argv.data());
}

}  // namespace

int main() {
    std::printf("test_process_boot_kind\n");
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr-boot-kind-" + std::to_string(std::rand()) + "-" +
                                                          std::to_string(static_cast<unsigned long long>(
                                                              fs::file_time_type::clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    CHECK(fs::is_directory(scratch));

    // The profile and the environment this boot reads, all private.
    unsetEnv("FOXSDR_DIAG_DIR");
    unsetEnv("CASCADE_SENTINEL_TEST");
#if defined(_WIN32)
    setEnv("APPDATA", scratch.string());
#else
    setEnv("XDG_CONFIG_HOME", scratch.string());
#endif
    const std::string hook = (scratch / "cfg.json").string();  // named, never read at boot
    setEnv("CASCADE_CONFIG_TEST", hook);
    const std::string configPath = cascade::core::ConfigStore::defaultPath();
    CHECK(configPath.rfind(scratch.string(), 0) == 0);  // the profile is the scratch one
    const std::string configDir = fs::path(configPath).parent_path().string();
    const fs::path linkFile = fs::path(configDir) / "link-request";

    // The shape core::parseBetaLinkUrl trusts: foxsdr://beta?t= and 40 hex characters.
    const std::string link = "foxsdr://beta?t=0123456789abcdef0123456789abcdef01234567";
    // A foxsdr: link in a shape it does not trust: dropped by either kind, never written, never echoed.
    const std::string untrusted = "foxsdr://beta/extra?t=NOTATOKEN";
    const std::string scopeEngine = "bootkind-engine-" + std::to_string(std::rand());
    const std::string scopeHost = "bootkind-host-" + std::to_string(std::rand());
    const auto identity = [&](const std::string& scope) {
        return cascade::core::primaryInstanceIdentity(configDir, hook, scope);
    };

    // --- the Engine: no claim, no file, no echo ------------------------------------
    setEnv(cascade::core::kInstanceScopeEnv, scopeEngine);
    const BootResult eng = boot(ProcessKind::Engine, {"foxsdr-engine", link, "--frames", "0"});
    CHECK(!eng.exit);                            // not refused: the link was not an unknown argument
    CHECK(eng.argv1IsLinkActivation);            // consumed, so the parse skipped it
    CHECK(!eng.primaryInstance);                 // it claimed nothing
    CHECK(!fs::exists(linkFile));                // and wrote no link-request file
    CHECK(eng.frames == 0);                      // the rest of the line parsed
    // The name the Engine ran under is free: a claim from here succeeds.
    CHECK(cascade::core::claimPrimaryInstanceAt(identity(scopeEngine)));

    // --- the Host: claims, and hands the link over -----------------------------------
    setEnv(cascade::core::kInstanceScopeEnv, scopeHost);
    const BootResult host = boot(ProcessKind::Host, {"cascade", link, "--frames", "0"});
    CHECK(!host.exit);
    CHECK(host.argv1IsLinkActivation);
    CHECK(host.primaryInstance);                 // it holds the name...
    CHECK(fs::exists(linkFile));                 // ...and wrote the file a running instance would poll
    CHECK(!cascade::core::claimPrimaryInstanceAt(identity(scopeHost)));  // so the name is taken
    CHECK(host.frames == 0);

    // --- an untrusted-shape link: consumed (so the parse never echoes it), nothing written ----------
    fs::remove(linkFile, ec);
    for (const ProcessKind k : {ProcessKind::Engine, ProcessKind::Host}) {
        setEnv(cascade::core::kInstanceScopeEnv, std::string("bootkind-untrusted-") + (k == ProcessKind::Host ? "h" : "e") +
                                                     std::to_string(std::rand()));
        const BootResult r = boot(k, {"x", untrusted, "--frames", "0"});
        CHECK(!r.exit);
        CHECK(r.argv1IsLinkActivation);
        CHECK(!fs::exists(linkFile));
    }

    // --- kind-blind modes answer alike -----------------------------------------------
    for (const ProcessKind k : {ProcessKind::Host, ProcessKind::Engine}) {
        const BootResult v = boot(k, {"x", "--version"});
        CHECK(v.exit && v.exitCode == 0);
        const BootResult bad = boot(k, {"x", "--no-such-flag"});
        CHECK(bad.exit && bad.exitCode == 1);
        // A helper's refusal of an argument it does not know (exit 2) is the same for both.
        const BootResult helperBad = boot(k, {"x", "--enumerate-json", "--no-such-flag"});
        CHECK(helperBad.exit && helperBad.exitCode == 2);
    }

    fs::remove_all(scratch, ec);
    return testSummary("test_process_boot_kind");
}
