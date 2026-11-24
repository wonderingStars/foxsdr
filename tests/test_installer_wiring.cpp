// THE INSTALLER HOLD IS WIRED IN, held from the text of the three source files (security review R1, finding F1, 0.99.114; review of the fixes, D2).
//
// tests/test_installer_hold.cpp proves the core function (core/updater.hpp holdVerifiedInstaller). It cannot prove that the code which matters CALLS it, and three
// lines carry the whole fix:
//   * core/updater.cpp downloadUpdate records (path, digest) of the file it verified - delete that line and every "Install now" is refused as "could not start";
//   * gui/app_window.cpp AppWindow::launchInstaller and slint/host_duties.cpp shellOpenInstaller hold the file BEFORE ShellExecuteEx, and a refusal ends the launch -
//     delete either call and the swap between the hash and the start is open again, with every other test still green.
//
// It is a SOURCE SCAN (the pattern of test_process_boot_order): comments blanked, a function it cannot find FAILS (a scan that finds nothing proves nothing). Break-it:
// delete the recordVerifiedInstaller line, delete a launcher's holdVerifiedInstaller call, move it after ShellExecuteExW, or drop its "return false" -> RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The source with comments and the insides of string and character literals blanked to spaces (same length).
std::string codeOnly(const std::string& s) {
    std::string out = s;
    enum { Code, Line, Block, Str, Chr } st = Code;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        const auto blank = [&](std::size_t k) {
            if (s[k] != '\n' && s[k] != '\r') { out[k] = ' '; }
        };
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; blank(i); }
                else if (c == '/' && n == '*') { st = Block; blank(i); blank(i + 1); ++i; }
                else if (c == '"') { st = Str; }
                else if (c == '\'') {
                    const bool sep = i > 0 && std::isalnum(static_cast<unsigned char>(s[i - 1])) && std::isalnum(static_cast<unsigned char>(n));
                    if (!sep) { st = Chr; }
                }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { blank(i); }
                break;
            case Block:
                if (c == '*' && n == '/') { blank(i); blank(i + 1); ++i; st = Code; } else { blank(i); }
                break;
            case Str:
                if (c == '\\') { blank(i); if (i + 1 < s.size()) { blank(++i); } }
                else if (c == '"') { st = Code; }
                else { blank(i); }
                break;
            case Chr:
                if (c == '\\') { blank(i); if (i + 1 < s.size()) { blank(++i); } }
                else if (c == '\'') { st = Code; }
                else { blank(i); }
                break;
        }
    }
    return out;
}

// The text of the function whose definition starts with `signature` (braces matched in the blanked code); empty when it is not found.
std::string bodyOf(const std::string& code, const std::string& signature) {
    const std::size_t at = code.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t open = code.find_first_of("{;", at);
    if (open == std::string::npos || code[open] != '{') { return std::string(); }
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i) {
        if (code[i] == '{') { ++depth; }
        if (code[i] == '}' && --depth == 0) { return code.substr(open, i - open + 1); }
    }
    return std::string();
}

std::size_t countOf(const std::string& s, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) { ++n; }
    return n;
}

// One launcher: it holds the verified file exactly once, a refusal returns false, and that happens BEFORE the one ShellExecuteExW.
void checkLauncher(const char* what, const std::string& body) {
    CHECK(!body.empty());
    if (body.empty()) { std::printf("  %s: function not found\n", what); return; }
    const std::size_t hold = body.find("holdVerifiedInstaller(");
    const std::size_t shell = body.find("ShellExecuteExW(");
    if (hold == std::string::npos) { std::printf("  %s: holdVerifiedInstaller is not called\n", what); }
    CHECK(hold != std::string::npos);
    CHECK(shell != std::string::npos);
    CHECK(countOf(body, "holdVerifiedInstaller(") == 1u);
    CHECK(countOf(body, "ShellExecuteExW(") == 1u);
    if (hold != std::string::npos && shell != std::string::npos && hold > shell) { std::printf("  %s: the hold comes AFTER ShellExecuteExW\n", what); }
    CHECK(hold != std::string::npos && shell != std::string::npos && hold < shell);
    // the refusal ends the launch: if (!...holdVerifiedInstaller(path, hold, err)) { return false; }
    const std::regex refuse(R"(if\s*\(\s*!\s*(?:\w+::)*holdVerifiedInstaller\(\s*path\s*,\s*hold\s*,\s*\w+\s*\)\s*\)\s*\{\s*return\s+false\s*;\s*\})");
    if (!std::regex_search(body, refuse)) { std::printf("  %s: a refused hold does not end the launch\n", what); }
    CHECK(std::regex_search(body, refuse));
    // the hold object lives in this function, so it is still open across ShellExecuteExW (declared before the call, not a temporary)
    const std::regex declared(R"(InstallerHold\s+hold\s*;)");
    CHECK(std::regex_search(body, declared));
    const std::size_t decl = body.find("InstallerHold");
    CHECK(decl != std::string::npos && decl < shell);
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_installer_wiring\n");
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path(__FILE__).parent_path().parent_path();

    const std::string updater = codeOnly(readFile(root / "src" / "core" / "updater.cpp"));
    const std::string window = codeOnly(readFile(root / "src" / "gui" / "app_window.cpp"));
    const std::string duties = codeOnly(readFile(root / "src" / "slint" / "host_duties.cpp"));
    CHECK(!updater.empty());
    CHECK(!window.empty());
    CHECK(!duties.empty());

    // downloadUpdate records what it verified, after the verified transfer and before it reports success.
    const std::string download = bodyOf(updater, "bool downloadUpdate(");
    CHECK(!download.empty());
    const std::size_t fetched = download.find("fetchVerifiedFile(");
    const std::size_t named = download.find("outPath = dest.string()");
    const std::size_t recorded = download.find("recordVerifiedInstaller(outPath");
    if (recorded == std::string::npos) { std::printf("  downloadUpdate does not record the verified installer\n"); }
    CHECK(fetched != std::string::npos && named != std::string::npos && recorded != std::string::npos);
    CHECK(countOf(download, "recordVerifiedInstaller(") == 1u);
    CHECK(fetched < named && named < recorded);
    CHECK(download.find("return true", recorded) != std::string::npos);

    checkLauncher("AppWindow::launchInstaller", bodyOf(window, "bool AppWindow::launchInstaller("));
    checkLauncher("shellOpenInstaller", bodyOf(duties, "bool shellOpenInstaller("));

    return testSummary("test_installer_wiring");
}
