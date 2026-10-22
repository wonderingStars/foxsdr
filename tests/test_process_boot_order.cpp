// THE ORDER OF PROCESS BOOT, held from the text of src/engine/process_boot.cpp.
//
// Engine extraction stage 7a moved main()'s start-up into bootProcess(). The
// order of that sequence is the contract and it has a history of being broken
// by an innocent-looking reshuffle:
//
//   * the console is settled FIRST - every later line may print;
//   * the helper modes (--sdrplay-*, --sentinel, --enumerate-json) return BEFORE
//     any handler is armed, because a helper must touch nothing;
//   * the crash handlers are registered (capture OFF) BEFORE the stored
//     Diagnostics switch is read - the read itself can fault, and the switch
//     must be known before capture goes on ("off means off");
//   * the single-instance name is claimed BEFORE the flags are parsed - a link
//     handed to a running instance must never reach a parse that would echo it;
//   * a foxsdr:// link in argv[1] is consumed before the parse sees it;
//   * the tool modes run after the parse and before any config path, handler or
//     sentinel is wired - a refused or tool run must arm nothing;
//   * the sentinel is the last thing armed before the window exists.
//
// And the KIND (Host | Engine) decides exactly five things - the instance claim,
// link activation, the stderr capture, the console handler and (stage 7d) the
// signal path - so an Engine never claims `Local\FoxSDR-instance` and never
// writes a link-request file (docs/engine-merge-0.99.84.md, TESTER-LINK). This
// test pins the gates from the text; tests/test_process_boot_kind.cpp runs the
// boot as each kind and looks at the machine.
//
// It is a SOURCE SCAN: comments are blanked, string literals kept where a flag
// name is the thing being ordered. A function it cannot find FAILS (a scan that
// finds nothing proves nothing). Break-it: swap two steps in bootProcess, or two
// statements inside armDiagnostics, or delete a kind guard -> RED.
//
// The optional first argument is the repository root (CTest passes none; the
// build bakes in CASCADE_SOURCE_DIR).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Two views of one source, the same length so a position in one is a position in
// the other:
//   view  - comments blanked to spaces; string and character literals kept;
//   blank - comments AND the insides of literals blanked: braces and identifiers
//           in code only.
struct Views {
    std::string view;
    std::string blank;
};

Views makeViews(const std::string& s) {
    Views v;
    v.view = s;
    v.blank = s;
    enum { Code, Line, Block, Str, Chr } st = Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        const auto blankBoth = [&](std::size_t k) {
            if (s[k] != '\n' && s[k] != '\r') { v.view[k] = ' '; v.blank[k] = ' '; }
        };
        const auto blankInside = [&](std::size_t k) {
            if (s[k] != '\n' && s[k] != '\r') { v.blank[k] = ' '; }
        };
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; blankBoth(i); }
                else if (c == '/' && n == '*') { st = Block; blankBoth(i); blankBoth(i + 1); ++i; }
                else if (c == '"') { st = Str; }
                else if (c == '\'') {
                    // A digit separator (1'000) is not a character literal.
                    const bool sep = i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])) &&
                                     std::isalnum(static_cast<unsigned char>(n));
                    if (!sep) { st = Chr; }
                }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { blankBoth(i); }
                break;
            case Block:
                if (c == '*' && n == '/') { blankBoth(i); blankBoth(i + 1); ++i; st = Code; }
                else { blankBoth(i); }
                break;
            case Str:
                if (c == '\\') { blankInside(i); if (i + 1 < s.size()) { blankInside(++i); } }
                else if (c == '"') { st = Code; }
                else { blankInside(i); }
                break;
            case Chr:
                if (c == '\\') { blankInside(i); if (i + 1 < s.size()) { blankInside(++i); } }
                else if (c == '\'') { st = Code; }
                else { blankInside(i); }
                break;
        }
    }
    return v;
}

// A function body: [open, close) of the braces after `signature`, found in the
// blank view. npos if the signature is not there or the braces do not close.
struct Body {
    std::size_t open = std::string::npos;
    std::size_t close = std::string::npos;
    bool found() const { return open != std::string::npos && close != std::string::npos; }
};

Body bodyOf(const Views& v, const std::string& signature) {
    Body b;
    const std::size_t at = v.blank.find(signature);
    if (at == std::string::npos) { return b; }
    // The definition's brace, not a declaration's semicolon.
    std::size_t p = v.blank.find_first_of("{;", at);
    if (p == std::string::npos || v.blank[p] != '{') { return b; }
    int depth = 0;
    for (std::size_t i = p; i < v.blank.size(); ++i) {
        if (v.blank[i] == '{') { ++depth; }
        if (v.blank[i] == '}' && --depth == 0) { b.open = p; b.close = i; return b; }
    }
    return b;
}

std::string slice(const std::string& s, const Body& b) {
    return b.found() ? s.substr(b.open, b.close - b.open) : std::string();
}

std::size_t findIn(const std::string& s, const std::string& needle, std::size_t from = 0) {
    return s.find(needle, from);
}

std::size_t countOf(const std::string& s, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) { ++n; }
    return n;
}

bool hasWord(const std::string& s, const std::string& word) {
    return std::regex_search(s, std::regex("(^|[^A-Za-z0-9_])" + word + "($|[^A-Za-z0-9_])"));
}

// Every needle present, once, and in increasing position. Reports the first
// pair out of order by name.
bool inOrder(const std::string& s, const std::vector<std::string>& needles, const char* what) {
    std::size_t last = 0;
    std::string lastName;
    bool ok = true;
    for (const std::string& n : needles) {
        const std::size_t at = findIn(s, n);
        if (at == std::string::npos) {
            std::printf("  %s: '%s' is missing\n", what, n.c_str());
            return false;
        }
        if (countOf(s, n) != 1) {
            std::printf("  %s: '%s' appears %zu times, expected once\n", what, n.c_str(), countOf(s, n));
            ok = false;
        }
        if (!lastName.empty() && at < last) {
            std::printf("  %s: '%s' comes BEFORE '%s' - the order is the contract\n", what, n.c_str(),
                        lastName.c_str());
            ok = false;
        }
        last = at;
        lastName = n;
    }
    return ok;
}

fs::path findRoot(int argc, char** argv) {
    if (argc > 1) { return argv[1]; }
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

int main(int argc, char** argv) {
    std::printf("test_process_boot_order\n");
    const fs::path root = findRoot(argc, argv);
    const std::string bootText = readFile(root / "src" / "engine" / "process_boot.cpp");
    const std::string mainText = readFile(root / "src" / "main.cpp");
    const std::string toolText = readFile(root / "src" / "engine" / "tool_modes.cpp");
    CHECK(!bootText.empty());
    CHECK(!mainText.empty());
    CHECK(!toolText.empty());
    if (bootText.empty() || mainText.empty()) { return testSummary("test_process_boot_order"); }
    const Views boot = makeViews(bootText);

    const Body seq = bodyOf(boot, "BootResult bootProcess(");
    const Body helpers = bodyOf(boot, "Step runHelperModes(");
    const Body arming = bodyOf(boot, "void armDiagnostics(");
    const Body claim = bodyOf(boot, "void claimInstance(");
    const Body link = bodyOf(boot, "Step activateLink(");
    const Body parse = bodyOf(boot, "Step parseFlags(");
    const Body config = bodyOf(boot, "void wireConfig(");
    const Body capture = bodyOf(boot, "void captureStderr(");
    const Body seam = bodyOf(boot, "void readSentinelSeam(");
    const Body handler = bodyOf(boot, "void installConsoleHandler(");
    const Body outcome = bodyOf(boot, "void logConsoleOutcome(");
    const Body sentinel = bodyOf(boot, "void armSentinel(");
    for (const Body* b : {&seq, &helpers, &arming, &claim, &link, &parse, &config, &capture, &seam, &handler,
                          &outcome, &sentinel}) {
        CHECK(b->found());
    }
    if (!(seq.found() && helpers.found() && arming.found() && claim.found() && link.found() && parse.found() &&
          config.found() && capture.found() && seam.found() && handler.found() && outcome.found() &&
          sentinel.found())) {
        return testSummary("test_process_boot_order");
    }

    // --- 1. The sequence --------------------------------------------------------
    const std::string seqCode = slice(boot.blank, seq);
    CHECK(inOrder(seqCode,
                  {"applyConsoleOwnership(", "runHelperModes(", "armDiagnostics(", "claimInstance(",
                   "activateLink(", "parseFlags(", "runToolMode(", "wireConfig(", "captureStderr(",
                   "readSentinelSeam(", "installConsoleHandler(", "logConsoleOutcome(", "armSentinel("},
                  "bootProcess"));
    // The console is the FIRST thing the function does: nothing but the opening
    // brace and whitespace before it.
    {
        const std::size_t at = findIn(seqCode, "applyConsoleOwnership(");
        std::string before = at == std::string::npos ? std::string("?") : seqCode.substr(1, at - 1);
        const std::string ns = "cascade::core::";  // the call's own qualifier
        if (before.size() >= ns.size() && before.compare(before.size() - ns.size(), ns.size(), ns) == 0) {
            before.resize(before.size() - ns.size());
        }
        bool onlySpace = at != std::string::npos;
        for (const char c : before) {
            if (!std::isspace(static_cast<unsigned char>(c))) { onlySpace = false; }
        }
        CHECK(onlySpace);
    }
    // No handler, claim or config read is done in the sequence body itself: it
    // only calls the steps, so the order above is the whole order.
    for (const char* direct : {"installCrashHandlers(", "claimPrimaryInstance(", "ConfigStore::", "installStderrCapture(",
                               "installConsoleCloseHandler(", "sentinelConfigure(", "writeLinkRequestFile("}) {
        CHECK(findIn(seqCode, direct) == std::string::npos);
    }

    // --- 2. The helper modes touch nothing --------------------------------------
    const std::string helpersView = slice(boot.view, helpers);
    const std::string helpersCode = slice(boot.blank, helpers);
    CHECK(inOrder(helpersView, {"\"--sdrplay-service-state\"", "\"--sdrplay-probe\"", "\"--sentinel\"",
                                "\"--enumerate-json\""},
                  "runHelperModes"));
    for (const char* touches : {"installCrashHandlers", "ConfigStore", "defaultPath", "setCrashCaptureEnabled",
                                "DiagLog", "diagLogf", "claimPrimaryInstance", "installStderrCapture",
                                "installConsoleCloseHandler", "sentinelConfigure", "writeLinkRequestFile",
                                "getenv"}) {
        if (findIn(helpersCode, touches) != std::string::npos) {
            std::printf("  runHelperModes touches '%s': a helper must touch nothing\n", touches);
        }
        CHECK(findIn(helpersCode, touches) == std::string::npos);
    }
    // ...and nothing but runHelperModes arms or claims anywhere ahead of them.
    CHECK(countOf(boot.blank, "installCrashHandlers(") == 1);

    // --- 3. Crash arming, then the config read, then capture --------------------
    const std::string armCode = slice(boot.blank, arming);
    CHECK(inOrder(armCode, {"crashCfg.enabled = false", "installCrashHandlers(", "storedDiagnosticsEnabled(",
                            "setCrashCaptureEnabled(", "configure("},
                  "armDiagnostics"));
    // The stored switch is read in this one place (plus its definition).
    CHECK(countOf(boot.blank, "storedDiagnosticsEnabled(") == 2);
    CHECK(countOf(boot.blank, "setCrashCaptureEnabled(") == 1);

    // --- 4. The claim, then the link, both before the parse ---------------------
    CHECK(countOf(boot.blank, "claimPrimaryInstance(") == 1);
    CHECK(findIn(slice(boot.blank, claim), "claimPrimaryInstance(") != std::string::npos);
    CHECK(countOf(boot.blank, "writeLinkRequestFile(") == 1);
    CHECK(findIn(slice(boot.blank, link), "writeLinkRequestFile(") != std::string::npos);
    // A link in argv[1] is skipped by the parse (it never reaches the
    // unknown-argument echo).
    CHECK(findIn(slice(boot.blank, parse), "argv1IsLinkActivation ? 2 : 1") != std::string::npos);

    // --- 5. The kind decides five things and nothing else -----------------------
    // Each gate is a guard clause that RETURNS before the first effect.
    struct Gate { const char* name; const Body* body; const char* effect; };
    const Gate gates[] = {
        {"claimInstance", &claim, "claimPrimaryInstance("},
        {"activateLink", &link, "writeLinkRequestFile("},
        {"captureStderr", &capture, "installStderrCapture("},
        {"installConsoleHandler", &handler, "installConsoleCloseHandler("},
    };
    for (const Gate& g : gates) {
        const std::string code = slice(boot.blank, *g.body);
        const std::size_t guard = findIn(code, "kind != ProcessKind::Host");
        const std::size_t effect = findIn(code, g.effect);
        const bool ok = guard != std::string::npos && effect != std::string::npos && guard < effect &&
                        findIn(code.substr(guard, effect - guard), "return") != std::string::npos;
        if (!ok) { std::printf("  %s: no 'kind != ProcessKind::Host' guard that returns before %s\n", g.name, g.effect); }
        CHECK(ok);
    }
    // The activateLink gate also covers the instance a link would be handed to:
    // the Engine branch (before the guard's return) writes no file, which the line
    // above shows; and no step outside the gates mentions the kind at all.
    struct Blind { const char* name; const Body* body; };
    const Blind blind[] = {{"runHelperModes", &helpers}, {"armDiagnostics", &arming}, {"parseFlags", &parse},
                           {"wireConfig", &config}, {"readSentinelSeam", &seam}, {"logConsoleOutcome", &outcome},
                           {"armSentinel", &sentinel}};
    for (const Blind& b : blind) {
        const bool mentions = hasWord(slice(boot.blank, *b.body), "kind");
        if (mentions) { std::printf("  %s mentions 'kind': it must be kind-blind\n", b.name); }
        CHECK(!mentions);
    }
    // The sequence passes the kind to exactly the four gated steps.
    for (const char* gated : {"claimInstance(kind", "activateLink(kind", "captureStderr(kind", "installConsoleHandler(kind"}) {
        CHECK(findIn(seqCode, gated) != std::string::npos);
    }
    CHECK(countOf(seqCode, "kind") == 4);  // those four calls and nothing else in the sequence body mentions it

    // --- 6. main() is the thin entry point it claims to be ----------------------
    const Views mv = makeViews(mainText);
    const Body mainBody = bodyOf(mv, "int main(");
    CHECK(mainBody.found());
    const std::string mainCode = slice(mv.blank, mainBody);
    CHECK(inOrder(mainCode, {"bootProcess(", "AppWindow app("}, "main"));
    CHECK(findIn(slice(mv.view, mainBody), "ProcessKind::Host") != std::string::npos);
    CHECK(findIn(mainText, "engine/process_boot.hpp") != std::string::npos);
    std::size_t lines = 0;
    for (const char c : mainText) { lines += c == '\n' ? 1 : 0; }
    if (lines >= 200) { std::printf("  main.cpp is %zu lines: boot has crept back into it\n", lines); }
    CHECK(lines < 200);
    for (const char* moved : {"installCrashHandlers", "claimPrimaryInstance", "installStderrCapture",
                              "installConsoleCloseHandler", "applyConsoleOwnership", "writeLinkRequestFile",
                              "runSelftest", "storedDiagnosticsEnabled", "sentinelConfigure", "strcmp"}) {
        CHECK(findIn(mv.blank, moved) == std::string::npos);
    }

    // --- 7. The moved code is still in the build, and still silent about links --
    const std::string cmake = readFile(root / "CMakeLists.txt");
    CHECK(cmake.find("file(GLOB CASCADE_ENGINE_SRC CONFIGURE_DEPENDS src/engine/*.cpp)") != std::string::npos);
    CHECK(toolText.find("std::optional<int> runToolMode(") != std::string::npos);
    // The beta-link token is never logged: the scan test_tester_link.cpp does of
    // main.cpp, applied to where the logging moved. (That placeholder test still
    // scans main.cpp, which now logs nothing - its re-expression must read this file.)
    {
        std::istringstream lines2(bootText);
        std::string line;
        bool clean = true;
        int lineNo = 0;
        int logging = 0;
        while (std::getline(lines2, line)) {
            ++lineNo;
            if (line.find("diagLogf(") == std::string::npos && line.find("diagWarnf(") == std::string::npos) { continue; }
            if (line.find("tester link") != std::string::npos || line.find("link ignored") != std::string::npos ||
                line.find("linkToken") != std::string::npos) {
                ++logging;
            }
            const bool mentionsToken = line.find("linkToken") != std::string::npos;
            const bool onlyItsSize = line.find("linkToken.size()") != std::string::npos &&
                                     line.find("linkToken") == line.rfind("linkToken");
            if ((mentionsToken && !onlyItsSize) || line.find("argv[1]") != std::string::npos) {
                clean = false;
                std::printf("FAIL process_boot.cpp:%d logs the tester link token: %s\n", lineNo, line.c_str());
            }
        }
        CHECK(clean);
        CHECK(logging >= 2);  // the scan found the link's log lines it is guarding
    }

    return testSummary("test_process_boot_order");
}
