// THE SOFTWARE-RENDERER RELAUNCH IS THE HOST'S, AND ITS MARK IS TAKEN WHERE MASTER TOOK IT
// (engine extraction, the 0.99.90 merge; docs/engine-merge-0.99.90.md).
//
// Master 0.99.90 (9a2dcb6a) made main() do two things for a machine whose graphics driver cannot be used: at the instance claim a
// second process, started on the software renderer, takes the mark its parent left (FOXSDR_SOFTGL_RELAUNCH) and counts as the
// primary instance whatever the claim says; and after the window is gone the first process starts the second and waits for it.
// On this branch the claim is a step of process boot (engine/process_boot.cpp, claimInstance) and the window's end is main.cpp.
// The risk the move introduces is the one the merge brief named: the headless engine has NO GL, so it must not gain either half.
// This test pins, from the text and by running the boot as each kind:
//
//   * the mark is taken in exactly one place, inside claimInstance, AFTER its Host-only guard and BEFORE the claim itself, and
//     claimInstance runs after armDiagnostics (the crash handlers) and before parseFlags - the position master took it in;
//   * nothing the headless engine runs names the software renderer: no file of src/engine/ and not src/engine_main.cpp includes
//     gui/software_gl.hpp or calls the relaunch, the loader or the FOXSDR_GL variable (the engine library may not reach gui/ at
//     all; the mark lives in core/relaunch_marker.hpp for that reason);
//   * the relaunch itself is main.cpp's, after the window object is gone, with the watcher switched off first;
//   * run for real (Windows, where a relaunch exists): an Engine boot given the mark does NOT take it (the variable is still
//     set afterwards) and is not the primary instance; a Host boot given the mark IS the primary instance although the name is
//     already held (the control: the same boot without the mark is not), and the variable is cleared so nothing it starts inherits it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/relaunch_marker.hpp"
#include "core/tester_link.hpp"
#include "engine/process_boot.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::engine::BootResult;
using cascade::engine::ProcessKind;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// Comments blanked (string literals kept): a mention in a comment is not a call.
std::string code(const std::string& s) {
    std::string out = s;
    enum { Code, Line, Block, Str, Chr } st = Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        const auto blank = [&](std::size_t k) {
            if (s[k] != '\n') { out[k] = ' '; }
        };
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; blank(i); }
                else if (c == '/' && n == '*') { st = Block; blank(i); blank(i + 1); ++i; }
                else if (c == '"') { st = Str; }
                else if (c == '\'' && !(i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])))) { st = Chr; }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { blank(i); }
                break;
            case Block:
                if (c == '*' && n == '/') { blank(i); blank(i + 1); ++i; st = Code; } else { blank(i); }
                break;
            case Str:
                if (c == '\\') { ++i; } else if (c == '"') { st = Code; }
                break;
            case Chr:
                if (c == '\\') { ++i; } else if (c == '\'') { st = Code; }
                break;
        }
    }
    return out;
}

std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = hay.find(needle, at)) != std::string::npos; at += needle.size()) { ++n; }
    return n;
}

// The text of `signature`'s function: from the signature to the first line that is a lone closing brace.
std::string body(const std::string& src, const std::string& signature) {
    const std::size_t at = src.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = src.find("\n}\n", at);
    return end == std::string::npos ? std::string() : src.substr(at, end - at);
}

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
    std::printf("test_process_boot_softgl\n");
    const fs::path root = fs::path(CASCADE_SOURCE_DIR);

    // --- 1. From the text: where the mark is taken ---------------------------------------------------------
    const std::string bootSrc = code(readFile(root / "src" / "engine" / "process_boot.cpp"));
    const std::string claim = body(bootSrc, "void claimInstance(");
    CHECK(!claim.empty());
    CHECK(count(bootSrc, "takeSoftwareGlRelaunchMarker(") == 1);
    CHECK(count(claim, "takeSoftwareGlRelaunchMarker(") == 1);
    const std::size_t guard = claim.find("kind != ProcessKind::Host");
    const std::size_t mark = claim.find("takeSoftwareGlRelaunchMarker(");
    const std::size_t theClaim = claim.find("claimPrimaryInstance(");
    CHECK(guard != std::string::npos && mark != std::string::npos && theClaim != std::string::npos);
    CHECK(guard < mark && mark < theClaim);   // Host only; taken before the claim it overrides
    // The mark overrides the claim's answer - it does not replace the claim (the first process still holds the name).
    CHECK(claim.find("softglRelaunched", theClaim) != std::string::npos);
    // ...at master's position in the sequence: after the crash handlers, before the flags are parsed.
    const std::string seq = body(bootSrc, "BootResult bootProcess(");
    const std::size_t arm = seq.find("armDiagnostics(");
    const std::size_t claimAt = seq.find("claimInstance(");
    const std::size_t parseAt = seq.find("parseFlags(");
    CHECK(arm != std::string::npos && claimAt != std::string::npos && parseAt != std::string::npos);
    CHECK(arm < claimAt && claimAt < parseAt);

    // --- 2. Nothing the headless engine runs names the software renderer ---------------------------------
    std::vector<fs::path> engineFiles;
    for (const fs::directory_entry& e : fs::directory_iterator(root / "src" / "engine")) {
        if (e.is_regular_file()) { engineFiles.push_back(e.path()); }
    }
    engineFiles.push_back(root / "src" / "engine_main.cpp");
    int scanned = 0;
    for (const fs::path& p : engineFiles) {
        const std::string text = code(readFile(p));
        if (text.empty()) { continue; }
        ++scanned;
        for (const char* banned : {"software_gl", "softgl::", "relaunchAndWait", "tellUserRelaunchFailed", "FOXSDR_GL",
                                   "FOXSDR_TEST_WINDOW_FAIL", "relaunchWithSoftwareGl"}) {
            if (text.find(banned) != std::string::npos) {
                std::printf("  %s names '%s': the engine has no GL and no relaunch\n", p.filename().string().c_str(), banned);
            }
            CHECK(text.find(banned) == std::string::npos);
        }
    }
    CHECK(scanned > 10);   // the scan really read the engine's files

    // --- 3. The relaunch is main.cpp's, after the window is gone ---------------------------------------------
    const std::string mainSrc = code(readFile(root / "src" / "main.cpp"));
    const std::size_t windowScope = mainSrc.find("cascade::gui::AppWindow app(");
    const std::size_t relaunchFlag = mainSrc.find("app.relaunchWithSoftwareGl()");
    const std::size_t watcherOff = mainSrc.find("sentinelSetEnabled(false)");
    const std::size_t relaunch = mainSrc.find("softgl::relaunchAndWait()");
    CHECK(windowScope != std::string::npos && relaunchFlag != std::string::npos && watcherOff != std::string::npos &&
          relaunch != std::string::npos);
    // The flag is read inside the window's scope, and the relaunch comes after that scope closed: find the closing brace between.
    CHECK(windowScope < relaunchFlag && relaunchFlag < watcherOff && watcherOff < relaunch);
    CHECK(mainSrc.substr(relaunchFlag, watcherOff - relaunchFlag).find("}\n") != std::string::npos);
    CHECK(count(mainSrc, "relaunchAndWait(") == 1);
    CHECK(mainSrc.find("ProcessKind::Host") != std::string::npos);
    // The settings lock goes before the second process starts (it takes it for its own run).
    const std::size_t lockOff = mainSrc.find("configLock.release()");
    CHECK(lockOff != std::string::npos && watcherOff < lockOff && lockOff < relaunch);

#if defined(_WIN32)
    // --- 4. Run for real: the boot as each kind, with the mark in the environment ------------------------------
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr-boot-softgl-" + std::to_string(std::rand()) + "-" +
                                                          std::to_string(static_cast<unsigned long long>(
                                                              fs::file_time_type::clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    CHECK(fs::is_directory(scratch));
    unsetEnv("FOXSDR_DIAG_DIR");
    unsetEnv("CASCADE_SENTINEL_TEST");
    setEnv("APPDATA", scratch.string());
    const std::string hook = (scratch / "cfg.json").string();
    setEnv("CASCADE_CONFIG_TEST", hook);
    const std::string configPath = cascade::core::ConfigStore::defaultPath();
    CHECK(configPath.rfind(scratch.string(), 0) == 0);   // the profile is the scratch one
    const std::string configDir = fs::path(configPath).parent_path().string();
    const auto identity = [&](const std::string& scope) { return cascade::core::primaryInstanceIdentity(configDir, hook, scope); };
    const char* markName = cascade::core::kSoftwareGlRelaunchEnv;
    CHECK(std::string(markName) == "FOXSDR_SOFTGL_RELAUNCH");

    // The ENGINE given the mark: not primary, and the mark is left alone (it never reaches the claim).
    setEnv(cascade::core::kInstanceScopeEnv, "softgl-engine-" + std::to_string(std::rand()));
    setEnv(markName, "4242");
    const BootResult eng = boot(ProcessKind::Engine, {"foxsdr-engine", "--seconds", "0"});
    CHECK(!eng.exit);
    CHECK(!eng.primaryInstance);
    const char* stillThere = std::getenv(markName);
    CHECK(stillThere != nullptr && std::string(stillThere) == "4242");
    unsetEnv(markName);

    // The HOST: the name is already held (by this test), so a Host without the mark is not the primary instance...
    const std::string scopeHost = "softgl-host-" + std::to_string(std::rand());
    setEnv(cascade::core::kInstanceScopeEnv, scopeHost);
    CHECK(cascade::core::claimPrimaryInstanceAt(identity(scopeHost)));
    const BootResult plain = boot(ProcessKind::Host, {"cascade", "--frames", "0"});
    CHECK(!plain.exit);
    CHECK(!plain.primaryInstance);
    // ...and a Host given the mark is, and the mark is gone afterwards.
    setEnv(markName, "4242");
    const BootResult second = boot(ProcessKind::Host, {"cascade", "--frames", "0"});
    CHECK(!second.exit);
    CHECK(second.primaryInstance);
    const char* gone = std::getenv(markName);
    CHECK(gone == nullptr || gone[0] == '\0');
    std::string parent = "x";
    CHECK(!cascade::core::takeSoftwareGlRelaunchMarker(parent));   // taken once: a second look finds nothing

    fs::remove_all(scratch, ec);
#endif

    return testSummary("test_process_boot_softgl");
}
