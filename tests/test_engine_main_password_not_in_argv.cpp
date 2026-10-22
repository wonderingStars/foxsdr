// The web password is never on a command line, and never in a log (engine extraction stage 7e, the security half of
// headless provisioning). `foxsdr-engine --web-password-file FILE|-` NAMES a file; there is no flag whose value is the password.
// The rule is the tester-link flag's (src/engine/process_boot.cpp: "a value here would sit in shell history indefinitely"): an
// argument is visible to every other process on the machine (the process list), kept by shells and written into service units.
//
// WHAT IS ASSERTED
//   S. THE PARSER, BY ITS SOURCE: every flag literal of the Engine's parse that has "pass" in it is `--web-password-file` and no
//      other; the branch that takes it stores a name (`prov.passwordFile`), and the usage line says FILE|-.
//   M. A MISTAKE IS NOT AN ECHO: `--web-password SECRET`, `--password SECRET`, `--web-password=SECRET` and
//      `--web-password-file=SECRET` are refused as unknown arguments (exit 1), and the refusal names the flag, never the value -
//      the `=` form is shown up to its `=` and no further.
//   C. THE LIVE COMMAND LINE: a provisioned run's command line, read back from the operating system while it runs, names the
//      password FILE and holds not one byte of the password.
//   L. NO LOG, NO FILE, NO STREAM: after a provisioned run with a settings file, with the diagnostics log switched on
//      (FOXSDR_DIAG_DIR), from a file and from standard input, the password (generated for the run) is in NEITHER stream of the
//      process, in no file under the scratch profile - the diagnostics directory, the settings file, the lock - and in no file the
//      run left anywhere else the test can see (the scratch tree except the secrets folder that holds the file named on purpose). The
//      scan is not vacuous: the diagnostics directory must hold a log, the settings file the PBKDF2 record, stderr the line that
//      says what was provisioned.
//
// Break-its (docs/engine-stage4.md 6.19): printing the password in a log line (diagLogf in provisionServers) -> L RED; printing
// the SERVER_CONFIG text to stderr -> L RED; a parser branch that takes the password as the flag's value -> S and M RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

namespace {

std::string generatedPassword() {
    std::random_device rd;
    std::mt19937_64 g((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static const char* alphabet = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string p = "n9-";
    for (int i = 0; i < 24; ++i) { p += alphabet[g() % 54u]; }
    return p;
}

void writeText(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool waitErr(const fs::path& errFile, const std::string& needle, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (emt::contains(emt::readText(errFile), needle)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

int freePortNumber() {
    // A port in the dynamic range, chosen at random; the engine reports it if it is taken (the run then fails loudly).
    std::random_device rd;
    return 30000 + static_cast<int>(rd() % 20000u);
}

// Every regular file under `root` (skipping `skip`) that holds `needle`, as paths relative to the root.
std::vector<std::string> filesHolding(const fs::path& root, const fs::path& skip, const std::string& needle) {
    std::vector<std::string> hits;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) { continue; }
        if (!skip.empty() && it->path().generic_string().rfind(skip.generic_string(), 0) == 0) { continue; }
        if (emt::contains(emt::readFile(it->path()), needle)) { hits.push_back(fs::relative(it->path(), root, e2).generic_string()); }
    }
    return hits;
}

std::size_t countFiles(const fs::path& root) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) { ++n; }
    }
    return n;
}

fs::path sourceRoot() {
#ifdef CASCADE_SOURCE_DIR
    return CASCADE_SOURCE_DIR;
#else
    fs::path at = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(at / "src" / "engine" / "process_boot.cpp")) { return at; }
        at = at.parent_path();
    }
    return {};
#endif
}

}  // namespace

int main() {
    std::printf("test_engine_main_password_not_in_argv\n");
    const fs::path root = emt::scratch("engine-pw-argv");
    const fs::path home = root / "home";
    const fs::path outDir = root / "out";
    const fs::path secrets = root / "secrets";
    const fs::path diagDir = root / "diag";
    fs::create_directories(home / "tmp");
    fs::create_directories(outDir);
    fs::create_directories(diagDir);
    emt::isolate(home);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));

    // --- S. the parser, by its source -------------------------------------------------------------------------------------
    {
        const std::string text = emt::readFile(sourceRoot() / "src" / "engine" / "process_boot.cpp");
        CHECK(!text.empty());
        std::set<std::string> passFlags;
        const std::regex lit("\"(--[A-Za-z0-9-]*)\"");
        for (std::sregex_iterator it(text.begin(), text.end(), lit), end; it != end; ++it) {
            const std::string flag = (*it)[1].str();
            const std::string low = emt::lowered(flag);
            if (low.find("pass") != std::string::npos || low.find("secret") != std::string::npos ||
                low.find("token") != std::string::npos) {
                passFlags.insert(flag);
            }
        }
        // (the tester-link's "--link-tester" has no value and is a Host flag: it holds none of these words)
        CHECK(passFlags.size() == 1u && passFlags.count("--web-password-file") == 1u);
        if (passFlags.size() != 1u) { for (const std::string& f : passFlags) { std::printf("      secret-looking flag: %s\n", f.c_str()); } }
        CHECK(emt::contains(text, "prov.passwordFile = v;"));  // the branch stores a NAME
        CHECK(emt::contains(text, "--web-password-file FILE|-"));  // and the usage says so
        // The password is read in one place, from the file; nothing in the parse reads the environment for one.
        CHECK(!emt::contains(emt::lowered(text), "getenv(\"foxsdr_web"));
        std::printf("S: the parse has one secret-looking flag, --web-password-file, and it stores a name\n");
    }

    // --- M. a mistake is not an echo ---------------------------------------------------------------------------------------
    {
        const std::string secret = generatedPassword();
        struct Case { std::vector<std::string> args; const char* shown; };
        const Case cases[] = {
            {{"--web-enable", "--web-password", secret}, "unknown argument '--web-password'"},
            {{"--password", secret}, "unknown argument '--password'"},
            {{"--web-password=" + secret}, "unknown argument '--web-password=<value not shown>'"},
            {{"--web-enable", "--web-password-file=" + secret}, "unknown argument '--web-password-file=<value not shown>'"},
        };
        int n = 0;
        for (const Case& c : cases) {
            const emt::RunResult x = emt::run(engineExe, c.args, home, outDir, "m" + std::to_string(n++), 120000);
            std::printf("  M case %d: exit %d, stderr: %s\n", n - 1, x.exitCode, x.err.c_str());
            CHECK(x.started && !x.timedOut && x.exitCode == 1);
            CHECK(emt::contains(x.err, c.shown));
            CHECK(!emt::contains(x.err, secret));
            CHECK(!emt::contains(x.out, secret));
        }
    }

    // --- C and L. a provisioned run: the live command line, then every stream and file ------------------------------------------
    const std::string password = generatedPassword();
    writeText(secrets / "web.pw", password + "\n");
    const std::string stdinPassword = generatedPassword();
    writeText(secrets / "stdin.txt", stdinPassword + "\n");
    emt::setEnv("FOXSDR_DIAG_DIR", diagDir.string());  // the diagnostics log is ON: whatever the engine logs, the scan reads

    struct Pass { const char* name; std::string pw; bool viaStdin; };
    const Pass passes[] = {{"file", password, false}, {"stdin", stdinPassword, true}};
    for (const Pass& pass : passes) {
        const fs::path cfg = home / (std::string("cfg-") + pass.name) / "config.json";
        {
            cascade::core::AppConfig s;
            s.volume = 0.0f;
            writeText(cfg, cascade::core::ConfigStore::serialize(s));
        }
        const int port = freePortNumber();
        const std::string tag = std::string("run-") + pass.name;
        emt::Child child;
        child.setConsoleGroup(true);
        if (pass.viaStdin) { child.setStdinFile(secrets / "stdin.txt"); }
        const std::string source = pass.viaStdin ? std::string("-") : (secrets / "web.pw").string();
        CHECK(child.start(engineExe,
                          {"--config", cfg.string(), "--web-enable", "--web-port", std::to_string(port), "--web-password-file", source}, home,
                          outDir / (tag + ".out"), outDir / (tag + ".err")));
        CHECK(waitErr(outDir / (tag + ".err"), "(until ended from outside)", 60000));
        const std::string err = emt::readText(outDir / (tag + ".err"));

        // C. the command line, as the operating system holds it right now.
        const std::string cmdline = child.commandLine();
        std::printf("  C (%s): command line: %s\n", pass.name, cmdline.c_str());
        CHECK(!cmdline.empty());  // not read: not passed - the test says so rather than pass on an empty string
        CHECK(emt::contains(cmdline, "--web-password-file"));
        // (Windows quotes every argument, Linux joins them with spaces: both spellings of "the flag, then its value")
        CHECK(pass.viaStdin ? (emt::contains(cmdline, "--web-password-file -") || emt::contains(cmdline, "\"--web-password-file\" \"-\""))
                            : emt::contains(cmdline, "web.pw"));
        CHECK(!emt::contains(cmdline, pass.pw));

        CHECK(child.requestStop());
        CHECK(child.waitExit(60000) && child.code() == 0);

        // L. the streams, and every file.
        const std::string out = emt::readText(outDir / (tag + ".out"));
        CHECK(!emt::contains(err, pass.pw));
        CHECK(!emt::contains(out, pass.pw));
        CHECK(emt::contains(err, "web on 127.0.0.1:" + std::to_string(port) + " (sign-in required)"));  // the scan has something to scan
        std::printf("  L (%s): stderr is %zu bytes and holds no password\n", pass.name, err.size());
        // (the secrets folder is the one place the password is ON PURPOSE: the file the flag names)
        const std::vector<std::string> inHome = filesHolding(home, fs::path(), pass.pw);
        const std::vector<std::string> inDiag = filesHolding(diagDir, fs::path(), pass.pw);
        const std::vector<std::string> inOut = filesHolding(outDir, fs::path(), pass.pw);
        for (const std::string& h : inHome) { std::printf("      password found in home/%s\n", h.c_str()); }
        for (const std::string& h : inDiag) { std::printf("      password found in diag/%s\n", h.c_str()); }
        for (const std::string& h : inOut) { std::printf("      password found in out/%s\n", h.c_str()); }
        CHECK(inHome.empty() && inDiag.empty() && inOut.empty());
        // not vacuous: the diagnostics log is there, the settings file holds the record
        CHECK(countFiles(diagDir) > 0);
        CHECK(emt::contains(emt::readFile(cfg), "pbkdf2-sha256$"));
    }
    emt::unsetEnv("FOXSDR_DIAG_DIR");
    std::printf("  L: the diagnostics directory held %zu file(s), all scanned\n", countFiles(diagDir));

    std::error_code ec;
    fs::remove_all(root, ec);
    return testSummary("test_engine_main_password_not_in_argv");
}
