// Tests for the Slint boundary's text repair (safe_text.hpp) and the guard that keeps every publisher behind it.
//
// THE FAULT (0.99.112, crash groups 6CCFD0C0E58FC30C and 104F425230C2A742): slint::SharedString checks its bytes with Rust's
// str::from_utf8().unwrap(); one byte that is not UTF-8 panics inside slint_cpp.dll and the process is gone. The first test
// SHOWS THAT in a child process (this executable run with --raw): a SharedString built from "AirSpy M" + 0xF3 + ... ends the
// child abnormally. The rest hold the repair: the same bytes through foxsdr::slintsafe come out valid, once per site is logged
// with the bad bytes as hex, and every publisher's ss() really goes through it (a scan of the sources).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "safe_text.hpp"

#include "core/diag_log.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "test_check.hpp"

namespace {

const std::string kBad8 = std::string("AirSpy M") + "\xF3" + "INI v1.0";
const std::string kBad17 = std::string("localised service") + "\xF3" + " text";

#if defined(_WIN32)
// Runs this executable again with one argument and returns its exit code (no window, no error dialog).
unsigned long runChild(const char* mode) {
    char self[MAX_PATH] = {0};
    ::GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string cmd = std::string("\"") + self + "\" " + mode;
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { return 0xDEADBEEFu; }
    ::WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0xDEADBEEFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return code;
}
#endif

#if defined(_WIN32)
// Runs `--raw` with its stderr written to `stderrFile` and RUST_BACKTRACE set to `backtrace` (nullptr: unset); returns the child's process id (0 on failure).
unsigned long runRawCapturingStderr(const std::string& stderrFile, const char* backtrace, unsigned long* exitCode) {
    char self[MAX_PATH] = {0};
    ::GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string cmd = std::string("\"") + self + "\" --raw";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE file = ::CreateFileA(stderrFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return 0; }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdError = file;
    si.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    char* saved = nullptr;
    size_t savedLen = 0;
    _dupenv_s(&saved, &savedLen, "RUST_BACKTRACE");
    if (backtrace != nullptr) { _putenv_s("RUST_BACKTRACE", backtrace); } else { _putenv_s("RUST_BACKTRACE", ""); }
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    _putenv_s("RUST_BACKTRACE", saved != nullptr ? saved : "");
    free(saved);
    if (!ok) { ::CloseHandle(file); return 0; }
    ::WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0xDEADBEEFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    *exitCode = code;
    const unsigned long pid = pi.dwProcessId;
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(file);
    return pid;
}

// A REAL Slint panic, end to end: the child's real stderr, through the real tail builder the crash report uses.
void testRealPanicInReportTail() {
    std::printf("  a real Slint panic: its message and location survive into the report's stderr tail, with no path or user name\n");
    namespace fs = std::filesystem;
    const char* tmp = std::getenv("TEMP");
    const fs::path dir = (tmp != nullptr && *tmp != '\0' ? fs::path(tmp) : fs::path(".")) / ("foxsdr-safe-text-panic-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const char* user = std::getenv("USERNAME");
    for (const char* backtrace : {static_cast<const char*>(nullptr), "1"}) {
        // the child names its own pid, which names the file the tail builder reads: capture to a temp name, then move to stderr-<pid>.txt
        const std::string tmpName = (dir / "capture.txt").string();
        unsigned long code = 0;
        const unsigned long pid = runRawCapturingStderr(tmpName, backtrace, &code);
        CHECK(pid != 0);
        if (pid == 0) { continue; }
        fs::rename(tmpName, dir / cascade::core::stderrFileName(pid), ec);
        const std::vector<std::string> tail = cascade::core::readStderrTailForReport(dir.string(), pid);
        std::string all;
        for (const std::string& l : tail) { all += l + "\n"; }
        std::printf("    RUST_BACKTRACE=%s: exit 0x%lx, %zu tail lines, %zu bytes\n", backtrace != nullptr ? backtrace : "(unset)", code, tail.size(), all.size());
        for (const std::string& l : tail) { std::printf("      %s\n", l.c_str()); }
        CHECK(code != 0);
        CHECK(!tail.empty() && tail.size() <= cascade::core::kStderrTailMaxLines);
        CHECK(all.find("panicked at") != std::string::npos);
        const std::size_t loc = all.find("string.rs:");   // the location of Rust's str::from_utf8().unwrap() call inside this Slint build
        CHECK(loc != std::string::npos);
        if (loc != std::string::npos) { CHECK(std::isdigit(static_cast<unsigned char>(all[loc + 10])) != 0); }
        CHECK(all.find("unwrap()") != std::string::npos);
        CHECK(all.find("Utf8Error") != std::string::npos);
        CHECK(all.find(":\\") == std::string::npos);    // no drive-letter path
        CHECK(all.find(":/") == std::string::npos);
        CHECK(all.find("Users") == std::string::npos);
        if (user != nullptr && *user != '\0') { CHECK(all.find(user) == std::string::npos); }
        fs::remove(dir / cascade::core::stderrFileName(pid), ec);
    }
    fs::remove_all(dir, ec);
}
#endif

void testRawAborts() {
    std::printf("  the fault: a SharedString from invalid UTF-8 ends the process; through the boundary it does not\n");
#if defined(_WIN32)
    const unsigned long raw = runChild("--raw");
    std::printf("    --raw exit code 0x%lx\n", raw);
    CHECK(raw != 0 && raw != 0xDEADBEEFu);   // the abort (a fast-fail, 0xc0000409)
    CHECK(runChild("--boundary") == 0);
    CHECK(runChild("--control") == 0);        // the child itself runs: valid text is fine
#else
    SKIP_LINUX("the child-process proof is Windows-only here");
#endif
}

void testBoundary() {
    std::printf("  slintsafe::shared: valid text untouched, invalid bytes repaired\n");
    namespace ss = foxsdr::slintsafe;
    CHECK(std::string(ss::shared("", "t.cpp").data()) == "");
    CHECK(std::string(ss::shared("plain", "t.cpp").data()) == "plain");
    CHECK(std::string(ss::shared("Portugu\xC3\xAAs", "t.cpp").data()) == "Portugu\xC3\xAAs");
    // ANSI first: what the bytes become depends on the machine's code page, so the outcome checked is "valid, and the ASCII around it kept";
    // on a Windows-1252 machine it is the o acute.
    for (const std::string* s : {&kBad8, &kBad17}) {
        const std::string out = ss::shared(*s, "t.cpp").data();
        CHECK(cascade::core::isValidUtf8(out));
        CHECK(out.find("INI v1.0") != std::string::npos || out.find(" text") != std::string::npos);
    }
#if defined(_WIN32)
    if (::GetACP() == 1252) { CHECK(std::string(ss::shared(kBad8, "t.cpp").data()) == "AirSpy M\xC3\xB3" "INI v1.0"); }
#endif
    CHECK(cascade::core::isValidUtf8(std::string(ss::shared("abc\xC3", "t.cpp").data())));
    CHECK(cascade::core::isValidUtf8(std::string(ss::shared("a\xC0\xAF" "b", "t.cpp").data())));
    const std::string nul("a\0b", 3);
    CHECK(ss::shared(nul, "t.cpp").data()[0] == 'a');   // a NUL is valid UTF-8 and does not abort (Slint reads the text as a C string, so shows "a")
}

int countLines(const std::string& needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

void testLoggedOncePerSite() {
    std::printf("  the log: once per call site, the bad bytes as hex, never the text around them\n");
    namespace ss = foxsdr::slintsafe;
    for (int i = 0; i < 5; ++i) { (void)ss::shared(kBad8, "C:/x/site_a.cpp"); }
    CHECK(countLines("in site_a.cpp") == 1);
    (void)ss::shared(kBad17, "C:/x/site_b.cpp");
    (void)ss::shared(kBad17, "C:/x/site_b.cpp");
    CHECK(countLines("in site_b.cpp") == 1);
    CHECK(countLines("in site_a.cpp at 8/17: f3 49 4e 49 20 76 31 2e 30") == 1);   // offset 8 of 16, hex of the bytes from there
    CHECK(countLines("AirSpy") == 0);                                              // none of the text itself
    CHECK(countLines("localised") == 0);
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) { CHECK(cascade::core::isValidUtf8(l)); }
    // Valid text logs nothing.
    const int before = countLines("slint text:");
    (void)ss::shared("all fine \xC3\xA9", "C:/x/site_c.cpp");
    CHECK(countLines("slint text:") == before);
}

// Every publisher's ss() helper must go through the boundary: a new translation unit that builds a SharedString from a std::string
// by hand is the way this fault comes back.
void testSourcesUseTheBoundary(const std::filesystem::path& srcDir) {
    std::printf("  every ss() helper in src/slint goes through slintsafe::shared\n");
    int helpers = 0;
    for (const auto& e : std::filesystem::directory_iterator(srcDir)) {
        if (e.path().extension() != ".cpp") { continue; }
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        const std::string raw = "slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }";
        if (text.find(raw) != std::string::npos) {
            ++helpers;
            std::printf("FAIL %s builds a SharedString by hand\n", e.path().filename().string().c_str());
            CHECK(false);
        }
        if (text.find("slint::SharedString ss(const std::string& s)") != std::string::npos) {
            ++helpers;
            CHECK(text.find("foxsdr::slintsafe::shared(s, __FILE__)") != std::string::npos);
            CHECK(text.find("#include \"safe_text.hpp\"") != std::string::npos);
        }
    }
    CHECK(helpers >= 25);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    if (argc > 1 && std::strcmp(argv[1], "--raw") == 0) {
        const slint::SharedString s{std::string_view(kBad8)};   // aborts inside slint_cpp.dll
        std::printf("survived: %zu bytes\n", s.size());
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--boundary") == 0) {
        const slint::SharedString s = foxsdr::slintsafe::shared(kBad8, "child.cpp");
        return cascade::core::isValidUtf8(std::string_view(s)) && s.size() >= 17 ? 0 : 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--control") == 0) {
        const slint::SharedString s{std::string_view("AirSpy MINI")};
        return s.size() == 11 ? 0 : 2;
    }
    testBoundary();
    testLoggedOncePerSite();
    testRawAborts();
#if defined(_WIN32)
    testRealPanicInReportTail();
#endif
    if (argc > 1) { testSourcesUseTheBoundary(argv[1]); }
    return testSummary("test_slint_safe_text");
}
