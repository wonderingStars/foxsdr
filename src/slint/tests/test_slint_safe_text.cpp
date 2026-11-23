// Tests for the Slint boundary's text repair (safe_text.hpp) and the guard that keeps every publisher behind it.
//
// THE FAULT (0.99.112, crash groups 6CCFD0C0E58FC30C and 104F425230C2A742): slint::SharedString checks its bytes with Rust's
// str::from_utf8().unwrap(); one byte that is not UTF-8 panics inside slint_cpp.dll and the process is gone. The first test
// SHOWS THAT in a child process (this executable run with --raw): a SharedString built from "AirSpy M" + 0xF3 + ... ends the
// child abnormally. The rest hold the repair: the same bytes through foxsdr::slintsafe come out valid, once per call site is logged
// without any of the text, and every publisher's ss() really goes through it (a scan of the sources).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "safe_text.hpp"

#include "core/diag_log.hpp"
#include "core/radio_report.hpp"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "test_check.hpp"

// Counts every C++ allocation, for the test that says valid text allocates none (the SharedString's own storage is Slint's, not this).
std::atomic<std::size_t> g_newCalls{0};
void* operator new(std::size_t n) {
    ++g_newCalls;
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

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
    std::printf("  a real Slint panic: its message and line:column survive into the report's stderr tail, with no path or user name\n");
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
        const std::size_t loc = all.find("panicked at <path>:");   // the position of Rust's str::from_utf8().unwrap() call inside this Slint build; its file name is never sent
        CHECK(loc != std::string::npos);
        if (loc != std::string::npos) { CHECK(std::isdigit(static_cast<unsigned char>(all[loc + 19])) != 0); }
        CHECK(all.find(".rs") == std::string::npos);
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
    CHECK(std::string(ss::shared("").data()) == "");
    CHECK(std::string(ss::shared("plain").data()) == "plain");
    CHECK(std::string(ss::shared("Portugu\xC3\xAAs").data()) == "Portugu\xC3\xAAs");
    // ANSI first: what the bytes become depends on the machine's code page, so the outcome checked is "valid, and the ASCII around it kept";
    // on a Windows-1252 machine it is the o acute.
    for (const std::string* s : {&kBad8, &kBad17}) {
        const std::string out = ss::shared(*s).data();
        CHECK(cascade::core::isValidUtf8(out));
        CHECK(out.find("INI v1.0") != std::string::npos || out.find(" text") != std::string::npos);
    }
#if defined(_WIN32)
    if (::GetACP() == 1252) { CHECK(std::string(ss::shared(kBad8).data()) == "AirSpy M\xC3\xB3" "INI v1.0"); }
#endif
    CHECK(cascade::core::isValidUtf8(std::string(ss::shared("abc\xC3").data())));
    CHECK(cascade::core::isValidUtf8(std::string(ss::shared("a\xC0\xAF" "b").data())));
    const std::string nul("a\0b", 3);
    CHECK(ss::shared(nul).data()[0] == 'a');   // a NUL is valid UTF-8 and does not abort (Slint reads the text as a C string, so shows "a")
}

int countLines(const std::string& needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

void resetSites() {
    foxsdr::slintsafe::SiteLog& log = foxsdr::slintsafe::siteLog();
    std::lock_guard<std::mutex> lk(log.mutex);
    log.sites.clear();
    log.capNoticeGiven = false;
}

// The text of the newest ring line that holds `needle`, from "slint text:" on (the ring's own time stamp left out).
std::string newestLine(const std::string& needle) {
    std::string found;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        const std::size_t at = l.find("slint text:");
        if (at != std::string::npos && l.find(needle) != std::string::npos) { found = l.substr(at); }
    }
    return found;
}

void testLoggedOncePerSite() {
    std::printf("  the log: once per call site (file and line), no text of the string, only the site, the offsets and the bad byte\n");
    namespace ss = foxsdr::slintsafe;
    resetSites();
    for (int i = 0; i < 5; ++i) { (void)ss::validAt(kBad8, "C:/x/site_a.cpp", 931); }
    CHECK(countLines("site site_a:931:") == 1);
    (void)ss::validAt(kBad8, "C:/x/site_a.cpp", 932);        // another line of the same file is another site
    CHECK(countLines("site site_a:932:") == 1);
    (void)ss::validAt(kBad17, "C:/x/site_b.cpp", 7);
    (void)ss::validAt(kBad17, "C:/x/site_b.cpp", 7);
    CHECK(countLines("site site_b:7:") == 1);
    // the exact line: offset 8 of 17, the bad byte f3, both neighbours ASCII letters ("a")
    const std::string line = newestLine("site site_a:931:");
    const std::string want = "slint text: site site_a:931: first bad byte at 8 of 17, value f3, before a, after a, looks like ";
    CHECK(line.compare(0, want.size(), want) == 0);
    CHECK(line.find(", 1 bad byte(s)") != std::string::npos);
    std::printf("    %s\n", line.c_str());
    // none of the text, in any spelling
    CHECK(countLines("AirSpy") == 0);
    CHECK(countLines("localised") == 0);
    CHECK(countLines("INI") == 0 && countLines("49 4e") == 0 && countLines("41 69") == 0);
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) { CHECK(cascade::core::isValidUtf8(l)); }
    // Valid text logs nothing.
    const int before = countLines("slint text:");
    (void)ss::validAt("all fine \xC3\xA9", "C:/x/site_c.cpp", 1);
    (void)ss::shared("all fine \xC3\xA9");
    CHECK(countLines("slint text:") == before);
}

void testLineFormat() {
    std::printf("  the log line: neighbours only when they are not ASCII letters or digits, and what the fault looks like\n");
    namespace ss = foxsdr::slintsafe;
    // a neighbour that is a space and one that is a slash are named by value; letters and digits as "a"; an end as "none"
    CHECK(ss::badTextLine("ab \xF3/cd", 3, 1, "s:1", 1252).find("value f3, before 20, after 2f, looks like system ANSI code page, code page 1252, 1 bad byte(s)") != std::string::npos);
    CHECK(ss::badTextLine("a1\xF3" "9z", 2, 1, "s:1", 1252).find("value f3, before a, after a,") != std::string::npos);
    CHECK(ss::badTextLine("\xF3" "x", 0, 1, "s:1", 1252).find("first bad byte at 0 of 2, value f3, before none, after a,") != std::string::npos);
    CHECK(ss::badTextLine("x\xF3", 1, 1, "s:1", 1252).find("before a, after none,") != std::string::npos);
    CHECK(ss::badTextLine("x\xC3", 1, 1, "s:1", 1252).find("looks like cut-off UTF-8") != std::string::npos);
    CHECK(ss::badTextLine("x\xE2\x82", 1, 2, "s:1", 1252).find("looks like cut-off UTF-8") != std::string::npos);
    CHECK(ss::badTextLine("x\xE9y", 1, 1, "s:1", 65001).find("looks like other") != std::string::npos);
    CHECK(ss::badTextLine("x\xE9y", 1, 1, "s:1", 0).find("looks like other") != std::string::npos);
#if defined(_WIN32)
    CHECK(ss::badTextLine("x\x81\x41", 1, 1, "s:1", 936).find("looks like double-byte code page, code page 936") != std::string::npos);
    CHECK(ss::badTextLine("x\xFFy", 1, 1, "s:1", 936).find("looks like other") != std::string::npos);
#endif
    CHECK(ss::siteName("D:\\a\\b\\signal_publish.cpp", 931) == "signal_publish:931");
    CHECK(ss::siteName("src/slint/main.cpp", 12) == "main:12");
    CHECK(ss::siteName(nullptr, 3) == "?:3");
    // no letter of the text is in the line
    const std::string l = ss::badTextLine("Secret\xF3Name", 6, 1, "s:1", 1252);
    CHECK(l.find("Secret") == std::string::npos && l.find("Name") == std::string::npos && l.find("53") == std::string::npos);
}

void testSiteCap() {
    std::printf("  the log: at most 16 call sites per run, then one line says so; the text is repaired all the same\n");
    namespace ss = foxsdr::slintsafe;
    resetSites();
    for (unsigned line = 1; line <= 20; ++line) { CHECK(cascade::core::isValidUtf8(ss::validAt(kBad8, "C:/x/cap_site.cpp", 100 + line))); }
    CHECK(countLines("site cap_site:1") == 16);                 // lines 101 to 116
    CHECK(countLines("16 call sites reported") == 1);
    CHECK(countLines("site cap_site:117:") == 0);
    CHECK(cascade::core::isValidUtf8(ss::validAt(kBad8, "C:/x/cap_site.cpp", 130)));   // still repaired
    CHECK(countLines("16 call sites reported") == 1);          // said once
    resetSites();
}

// The scrub the report goes through (scrubUploadLog, then the path token of radioreport::pathsToToken) leaves the line as it is.
void testLineSurvivesUploadScrub() {
    std::printf("  the log line survives the upload scrub chain intact\n");
    namespace ss = foxsdr::slintsafe;
    resetSites();
    (void)ss::validAt("ab \xF3/cd", "C:/x/signal_publish.cpp", 931);
    (void)ss::validAt(kBad8, "C:/x/kit_publish_mapj.cpp", 1288);
    (void)ss::validAt("Serielles USB-Ger\xE4t", "C:/Users/someone/x/tools_publish.cpp", 54);
    (void)ss::validAt("x\xC3", "C:/x/main.cpp", 4000);
    std::vector<std::string> lines;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        for (const char* site : {"site signal_publish:931:", "site kit_publish_mapj:1288:", "site tools_publish:54:", "site main:4000:"}) {
            if (l.find(site) != std::string::npos) { lines.push_back(l); }
        }
    }
    CHECK(lines.size() == 4);
    const std::vector<std::string> scrubbed = cascade::core::scrubUploadLog(lines);
    CHECK(scrubbed.size() == lines.size());
    for (std::size_t i = 0; i < lines.size() && i < scrubbed.size(); ++i) {
        const std::string once = scrubbed[i];
        const std::string twice = cascade::core::radioreport::pathsToToken(once);
        const std::size_t a = lines[i].find("slint text: site ");
        const std::size_t b = once.find("slint text: site ");
        const std::size_t c = twice.find("slint text: site ");
        CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos);
        if (a == std::string::npos || b == std::string::npos || c == std::string::npos) { continue; }
        if (once.substr(b) != lines[i].substr(a)) { std::printf("FAIL the scrub changed a line:\n  was %s\n  now %s\n", lines[i].substr(a).c_str(), once.substr(b).c_str()); }
        CHECK(once.substr(b) == lines[i].substr(a));
        CHECK(twice.substr(c) == lines[i].substr(a));
    }
    resetSites();
}

void testLanguageCode() {
    std::printf("  the language code: a plausible code only; anything else is logged once (its length) and refused before Slint sees it\n");
    namespace ss = foxsdr::slintsafe;
    for (const char* ok : {"en", "de", "pt-BR", "zh_Hans_CN", "es419", "x1", "abcdefghijklmnop"}) { CHECK(ss::plausibleLanguageCode(ok)); }
    CHECK(!ss::plausibleLanguageCode(""));
    CHECK(!ss::plausibleLanguageCode("e"));
    CHECK(!ss::plausibleLanguageCode("abcdefghijklmnopq"));      // 17 bytes
    CHECK(!ss::plausibleLanguageCode("d\xE9"));                  // a code-page letter: not UTF-8
    CHECK(!ss::plausibleLanguageCode("\xC3\xA9s"));              // valid UTF-8 but not an ASCII code
    CHECK(!ss::plausibleLanguageCode("de/../x"));
    CHECK(!ss::plausibleLanguageCode("de x"));
    CHECK(!ss::plausibleLanguageCode(std::string("de\0xx", 5)));
    // The call itself, with bytes that are not UTF-8: this would abort inside Slint if the code reached it.
    const int before = countLines("language code refused");
    CHECK(!ss::selectBundledTranslation("d\xE9" "utsch"));
    CHECK(!ss::selectBundledTranslation("\xFF\xFE"));
    CHECK(!ss::selectBundledTranslation("a\xC3"));
    CHECK(countLines("language code refused") == before + 3);
    CHECK(countLines("tsch") == 0);
}

void testKeyEventText() {
    std::printf("  a key event's text (the scripted input's `type`): a byte that is not UTF-8 is repaired, never passed on\n");
    namespace ss = foxsdr::slintsafe;
    CHECK(std::string(ss::keyEventText(u8"a").data()) == "a");
    CHECK(std::string(ss::keyEventText(u8"\u00e9").data()) == "\xC3\xA9");
    const std::u8string bad = {static_cast<char8_t>(0xF3)};
    CHECK(cascade::core::isValidUtf8(std::string(ss::keyEventText(bad).data())));
    const std::u8string cut = {static_cast<char8_t>(0xE4), static_cast<char8_t>(0xB8)};   // a letter cut in two
    CHECK(cascade::core::isValidUtf8(std::string(ss::keyEventText(cut).data())));
    const std::u8string mixed = {u8'x', static_cast<char8_t>(0xC0), static_cast<char8_t>(0xAF), u8'y'};
    CHECK(cascade::core::isValidUtf8(std::string(ss::keyEventText(mixed).data())));
}

// VALID TEXT COSTS NO C++ ALLOCATION AND NO LOG: shared() on valid text scans once and builds the SharedString (whose storage is Slint's own).
void testValidTextIsFree() {
    std::printf("  valid text: no C++ allocation and no log line\n");
    namespace ss = foxsdr::slintsafe;
    const std::string long8(200, 'x');
    const std::string_view view(long8);
    const int before = countLines("slint text:");
    g_newCalls = 0;
    for (int i = 0; i < 100; ++i) { const slint::SharedString s = ss::shared(view); CHECK(s.size() == 200); }
    const std::size_t calls = g_newCalls;
    CHECK(calls == 0);
    CHECK(countLines("slint text:") == before);
}

// Every publisher's ss() helper must go through the boundary, with the caller's source location: a new translation unit that builds a
// SharedString from a std::string by hand is the way this fault comes back.
void testSourcesUseTheBoundary(const std::filesystem::path& srcDir) {
    std::printf("  every ss() helper in src/slint goes through slintsafe::shared with the caller's source location\n");
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
        if (text.find("slint::SharedString ss(const std::string& s") != std::string::npos) {
            ++helpers;
            CHECK(text.find("slint::SharedString ss(const std::string& s, std::source_location loc = std::source_location::current()) { return foxsdr::slintsafe::shared(s, loc); }") != std::string::npos);
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
        const slint::SharedString s = foxsdr::slintsafe::shared(kBad8);
        return cascade::core::isValidUtf8(std::string_view(s)) && s.size() >= 17 ? 0 : 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--control") == 0) {
        const slint::SharedString s{std::string_view("AirSpy MINI")};
        return s.size() == 11 ? 0 : 2;
    }
    testBoundary();
    testLoggedOncePerSite();
    testLineFormat();
    testSiteCap();
    testLineSurvivesUploadScrub();
    testLanguageCode();
    testKeyEventText();
    testValidTextIsFree();
    testRawAborts();
#if defined(_WIN32)
    testRealPanicInReportTail();
#endif
    if (argc > 1) { testSourcesUseTheBoundary(argv[1]); }
    return testSummary("test_slint_safe_text");
}
