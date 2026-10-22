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
//     unknown argument exits 1;
//   * (stage 7b, the Engine's hermetic rule) an Engine has no config and no disk:
//     it hands on NO config path even when CASCADE_CONFIG_TEST names one (the Host
//     hands that one on), a bare Engine - argc 1, which is a "real session" for a
//     Host - may write no diagnostics and leaves the profile exactly as it found it,
//     and the flags differ by kind: --seconds is the Engine's, --frames the Host's,
//     each refused by the other as an unknown argument;
//   * (stage 7c) an Engine takes a settings file only when ASKED - `--config PATH` or
//     `--use-config` - hands that one on (never CASCADE_CONFIG_TEST's), creates
//     nothing by being asked (the lock is the service's), and refuses a missing
//     path, a flag where the path goes, the flag twice and both names for the file;
//     the Host refuses both flags as it refuses any flag it does not have.
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
    const BootResult eng = boot(ProcessKind::Engine, {"foxsdr-engine", link, "--seconds", "0"});
    CHECK(!eng.exit);                            // not refused: the link was not an unknown argument
    CHECK(eng.argv1IsLinkActivation);            // consumed, so the parse skipped it
    CHECK(!eng.primaryInstance);                 // it claimed nothing
    CHECK(!fs::exists(linkFile));                // and wrote no link-request file
    CHECK(eng.seconds == 0.0);                   // the rest of the line parsed
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
        const BootResult r = boot(k, {"x", untrusted, k == ProcessKind::Host ? "--frames" : "--seconds", "0"});
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

    // --- the flags differ by kind (stage 7b) ---------------------------------------------------------------
    {
        const BootResult s = boot(ProcessKind::Engine, {"foxsdr-engine", "--seconds", "2.5"});
        CHECK(!s.exit && s.seconds == 2.5 && s.frames < 0);
        const BootResult none = boot(ProcessKind::Engine, {"foxsdr-engine", "--selftest-not-a-flag"});
        CHECK(none.exit && none.exitCode == 1);
        // The window's flags are the Host's, and --seconds the Engine's: each refuses the other's, exit 1.
        for (const std::vector<std::string>& line : std::vector<std::vector<std::string>>{
                 {"x", "--frames", "0"}, {"x", "--diag-stall", "5"}, {"x", "--diag-toggle", "on"}, {"x", "--link-tester"}}) {
            const BootResult r = boot(ProcessKind::Engine, line);
            CHECK(r.exit && r.exitCode == 1);
        }
        for (const char* bad : {"abc", "-1", "nan", "1e99", "2s", ""}) {
            const BootResult r = boot(ProcessKind::Engine, {"x", "--seconds", bad});
            CHECK(r.exit && r.exitCode == 1);
        }
        const BootResult missing = boot(ProcessKind::Engine, {"x", "--seconds"});
        CHECK(missing.exit && missing.exitCode == 1);
        setEnv(cascade::core::kInstanceScopeEnv, "bootkind-hs-" + std::to_string(std::rand()));  // a Host claims: a name of its own
        const BootResult hostSeconds = boot(ProcessKind::Host, {"x", "--seconds", "1"});
        CHECK(hostSeconds.exit && hostSeconds.exitCode == 1);
        setEnv(cascade::core::kInstanceScopeEnv, "bootkind-hf-" + std::to_string(std::rand()));
        const BootResult hostFrames = boot(ProcessKind::Host, {"x", "--frames", "0"});
        CHECK(!hostFrames.exit && hostFrames.frames == 0 && hostFrames.seconds < 0.0);
    }

    // --- the Engine is hermetic (stage 7b) -------------------------------------------------------------------
    {
        // CASCADE_CONFIG_TEST names a path (set above). The Host with --frames hands it on and says so; an Engine has no
        // config at all - not that one, not the user's - whatever it is given.
        setEnv(cascade::core::kInstanceScopeEnv, "bootkind-hc-" + std::to_string(std::rand()));
        const BootResult hostCfg = boot(ProcessKind::Host, {"cascade", "--frames", "0"});
        CHECK(!hostCfg.exit && hostCfg.configPath == hook && hostCfg.announceConfig);
        const BootResult engCfg = boot(ProcessKind::Engine, {"foxsdr-engine", "--seconds", "0"});
        CHECK(!engCfg.exit && engCfg.configPath.empty() && !engCfg.announceConfig);

        // A BARE Engine - argc 1, which for a Host is "a real session" that logs, captures and reads the user's config
        // ahead of the window. Every per-user folder is the scratch one first, so a gate that failed would write into
        // the scratch tree where it can be seen, never into a real profile.
        for (const char* v : {"LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
            setEnv(v, scratch.string());
        }
        unsetEnv("CASCADE_CONFIG_TEST");
        unsetEnv("FOXSDR_DIAG_DIR");
        const auto listing = [&] {
            std::vector<std::string> names;
            std::error_code lec;
            for (const auto& entry : fs::recursive_directory_iterator(scratch, lec)) { names.push_back(entry.path().string()); }
            return names;
        };
        const std::vector<std::string> before = listing();
        const BootResult bare = boot(ProcessKind::Engine, {"foxsdr-engine"});
        CHECK(!bare.exit);
        CHECK(!bare.mayWrite);                 // no diagnostics may reach the disk...
        CHECK(!bare.wanted);                   // ...so none is wanted, and the config was not even asked
        CHECK(bare.configPath.empty());
        CHECK(bare.seconds < 0.0);             // no bound: it would run until ended from outside
        CHECK(!bare.primaryInstance);
        CHECK(listing() == before);            // and nothing was created anywhere under the profile
    }

    // --- the Engine's settings file (stage 7c): handed on only when ASKED for, by the command line alone ------------
    {
        const std::string explicitPath = (scratch / "enginecfg" / "config.json").string();
        const std::vector<std::string> before = [&] {
            std::vector<std::string> names;
            std::error_code lec;
            for (const auto& entry : fs::recursive_directory_iterator(scratch, lec)) { names.push_back(entry.path().string()); }
            return names;
        }();
        // --config PATH: that file, and the boot says so in the fields the service is built from.
        const BootResult a = boot(ProcessKind::Engine, {"foxsdr-engine", "--config", explicitPath, "--seconds", "0"});
        CHECK(!a.exit && a.configPath == explicitPath && !a.announceConfig && a.seconds == 0.0);
        CHECK(a.engineConfigArg == explicitPath && !a.engineUseConfig);
        // --use-config: the per-user file, the one the window application uses.
        const BootResult u = boot(ProcessKind::Engine, {"foxsdr-engine", "--use-config", "--seconds", "0"});
        CHECK(!u.exit && !u.configPath.empty() && u.configPath == cascade::core::ConfigStore::defaultPath() && u.engineUseConfig);
        CHECK(u.configPath.rfind(scratch.string(), 0) == 0);  // the scratch profile's, never a real one
        // CASCADE_CONFIG_TEST is the WINDOW's bounded-run hook: an engine asked for a file takes its own, and one not
        // asked takes none, whatever the hook says.
        setEnv("CASCADE_CONFIG_TEST", hook);
        const BootResult withHook = boot(ProcessKind::Engine, {"foxsdr-engine", "--config", explicitPath, "--seconds", "0"});
        CHECK(!withHook.exit && withHook.configPath == explicitPath);
        const BootResult hookOnly = boot(ProcessKind::Engine, {"foxsdr-engine", "--seconds", "0"});
        CHECK(!hookOnly.exit && hookOnly.configPath.empty());
        unsetEnv("CASCADE_CONFIG_TEST");
        // Naming a settings file creates nothing: no file, no folder, no lock - the service takes the lock, not boot.
        CHECK(!fs::exists(explicitPath) && !fs::exists(fs::path(explicitPath).parent_path()));
        {
            std::vector<std::string> after;
            std::error_code lec;
            for (const auto& entry : fs::recursive_directory_iterator(scratch, lec)) { after.push_back(entry.path().string()); }
            CHECK(after == before);
        }
        // Refused, with exit 1: no path, a flag where the path goes, the same flag twice, two names for the file.
        for (const std::vector<std::string>& line : std::vector<std::vector<std::string>>{
                 {"x", "--config"}, {"x", "--config", "--seconds", "1"}, {"x", "--config", ""},
                 {"x", "--config", "a.json", "--config", "b.json"}, {"x", "--config", "a.json", "--use-config"},
                 {"x", "--use-config", "--config", "a.json"}}) {
            const BootResult r = boot(ProcessKind::Engine, line);
            CHECK(r.exit && r.exitCode == 1);
        }
        // The Host has neither flag, and refuses each as it refuses any flag it does not have.
        setEnv(cascade::core::kInstanceScopeEnv, "bootkind-hcfg-" + std::to_string(std::rand()));
        const BootResult hostConfig = boot(ProcessKind::Host, {"x", "--config", "a.json"});
        CHECK(hostConfig.exit && hostConfig.exitCode == 1);
        setEnv(cascade::core::kInstanceScopeEnv, "bootkind-huse-" + std::to_string(std::rand()));
        const BootResult hostUse = boot(ProcessKind::Host, {"x", "--use-config"});
        CHECK(hostUse.exit && hostUse.exitCode == 1);
    }

    fs::remove_all(scratch, ec);
    return testSummary("test_process_boot_kind");
}
