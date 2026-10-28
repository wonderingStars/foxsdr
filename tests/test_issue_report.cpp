// An issue the person met and the program carried on from is REPORTED, not just counted
// (core/issue_report.hpp, 0.99.100; the owner's rule of 2026-10-28: "if a user has an issue
// we need to know about it, regardless"). Against the real crash directory, the real
// uploader's parser and the real sentinel rules:
//
//   consent     nothing is written while capture is off; one file once it is armed;
//   header      exactly the fields issueReportFieldNames() declares (and PRIVACY.md lists),
//               both ways; kind issue; the reason is the fixed prefix, the word and its
//               sentence; the signature is sixteen hex digits of the word alone;
//   body        the context block, the process block with the uptime, and the log ring -
//               with the line that was logged just before it;
//   once        the same word filed again writes nothing; another word writes a second file;
//   uploadable  core/crash_upload.cpp parses it as its own kind, with the reason and the log
//               and no stack, and the file name is one its sweep opens;
//   survived    the sentinel's rules never take it for the record of a death;
//   every word  has a sentence and a distinct signature.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/issue_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/crash_handler.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/exit_cause.hpp"
#include "core/health_events.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core;
using health::Recovered;

namespace {

unsigned long long pid() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<fs::path> issueFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("issue-", 0) == 0 && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".txt") == 0) {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The "name: value" lines before the first "---" section, as tests/test_diag_hang.cpp reads a
// freeze report's header.
std::set<std::string> headerFields(const std::string& report) {
    std::set<std::string> names;
    std::size_t pos = 0;
    while (pos < report.size()) {
        const std::size_t eol = report.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? report.size() : eol;
        const std::string line = report.substr(pos, end - pos);
        if (line.compare(0, 3, "---") == 0) { break; }
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos && colon > 0) { names.insert(line.substr(0, colon)); }
        pos = end + 1;
    }
    return names;
}

bool hexUpper16(const std::string& s) {
    if (s.size() != 16) { return false; }
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) { return false; }
    }
    return true;
}

}  // namespace

int main() {
    std::printf("test_issue_report\n");
    const fs::path dir = fs::temp_directory_path() / ("cascade-issue-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // --- EVERY WORD HAS A SENTENCE AND ITS OWN SIGNATURE ------------------------------------
    {
        std::set<std::string> sigs;
        for (std::size_t i = 0; i < health::kRecoveredCount; ++i) {
            const Recovered r = static_cast<Recovered>(i);
            const std::string sentence = issue::issueSentence(r);
            CHECK(!sentence.empty());
            CHECK(sentence.find('\n') == std::string::npos);
            const std::string reason = issue::issueReason(r);
            CHECK(reason == std::string(kIssueReasonPrefix) + health::recoveredWord(r) + " - " + sentence);
            const std::string sig = issue::issueSignature(r);
            CHECK(hexUpper16(sig));
            sigs.insert(sig);
        }
        CHECK(sigs.size() == health::kRecoveredCount);
        // ...and the same word always has the same signature: one row on the feed, for ever.
        CHECK(issue::issueSignature(Recovered::Viewport) == issue::issueSignature(Recovered::Viewport));
    }

    // --- OFF: nothing is written -------------------------------------------------------------
    {
        CrashHandlerConfig off;
        off.crashDir = dir.string();
        off.enabled = false;
        off.exitAfterReport = false;
        installCrashHandlers(off);
        CHECK(activeCrashDir().empty());
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).empty());
        // ...and flushing what waits writes nothing either while capture is off.
        issue::filePendingIssueReports();
        CHECK(issueFiles(dir).empty());
    }

    // --- A WORD MET BEFORE CAPTURE WAS ARMED IS FILED ONCE IT IS (start-up words) -----------
    {
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        CHECK(!activeCrashDir().empty());
        CHECK(issueFiles(dir).empty());
        issue::filePendingIssueReports();   // the Viewport met above, while off
        const std::vector<fs::path> pending = issueFiles(dir);
        CHECK(pending.size() == 1);
        if (pending.size() == 1) {
            const std::string text = readFile(pending[0]);
            CHECK(text.find("\nreason: " + issue::issueReason(Recovered::Viewport) + "\n") != std::string::npos);
        }
        // Filed once: a second flush, or the word again, writes nothing more.
        issue::filePendingIssueReports();
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).size() == 1);
        for (const fs::path& p : pending) { fs::remove(p, ec); }
    }
    issue::resetIssueReportsForTest();

    // --- ON: one file, the declared header, the body, once a word --------------------------
    {
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        CHECK(!activeCrashDir().empty());
        diagLogf("window: GLFW error 65543: WGL: Failed to create OpenGL context (Win32 error 0x00000008)");

        const std::string path = issue::fileIssueReport(Recovered::Viewport);
        CHECK(!path.empty());
        const std::vector<fs::path> files = issueFiles(dir);
        CHECK(files.size() == 1);
        if (files.size() == 1) {
            const std::string name = files[0].filename().string();
            std::printf("  wrote %s\n", name.c_str());
            CHECK(fs::equivalent(files[0], fs::path(path), ec));
            // issue-<stamp>-<pid>-<seq>.txt: the pid field as the crash handler's naming has it.
            CHECK(name.find("-" + std::to_string(pid()) + "-") != std::string::npos);

            const std::string text = readFile(files[0]);
            CHECK(text.rfind("kind: issue\n", 0) == 0);
            const std::set<std::string> declared(issueReportFieldNames().begin(),
                                                 issueReportFieldNames().end());
            CHECK(!declared.empty());
            const std::set<std::string> emitted = headerFields(text);
            for (const std::string& f : emitted) { std::printf("  issue field: %s\n", f.c_str()); }
            CHECK(emitted == declared);
            CHECK(text.find("\nreason: " + issue::issueReason(Recovered::Viewport) + "\n") != std::string::npos);
            CHECK(text.find("\nsignature: " + issue::issueSignature(Recovered::Viewport) + "\n") !=
                  std::string::npos);
            const std::size_t context = text.find("--- context ---\n");
            const std::size_t process = text.find("--- process ---\nuptime-sec: ");
            const std::size_t log = text.find("--- log (last ");
            CHECK(context != std::string::npos);
            CHECK(process != std::string::npos && process > context);
            CHECK(log != std::string::npos && log > process);
            CHECK(text.find("window-state: ") != std::string::npos);
            CHECK(text.find("WGL: Failed to create OpenGL context (Win32 error 0x00000008)", log) !=
                  std::string::npos);
            // No stack and no module list: nothing faulted.
            CHECK(text.find("--- thread ") == std::string::npos);
            CHECK(text.find("--- modules ---") == std::string::npos);

            // THE UPLOADER READS IT as the third kind, with the reason and the log, no stack.
            ParsedReport parsed;
            CHECK(parseReportText(text, parsed));
            CHECK(parsed.kind == "issue");
            CHECK(parsed.signature == issue::issueSignature(Recovered::Viewport));
            CHECK(parsed.reason == issue::issueReason(Recovered::Viewport));
            CHECK(parsed.code.empty());
            CHECK(parsed.threads.empty());
            CHECK(parsed.module.empty());
            CHECK(!parsed.log.empty());

            // THE SENTINEL'S RULES: a survived fault, never the record of a death.
            CHECK(reportReasonIsSurvivedFault(parsed.reason));
            CHECK(!crashReportWrittenByProcess(dir.string(), static_cast<unsigned long>(pid()),
                                               std::chrono::system_clock::now() - std::chrono::hours(1)));
        }

        // Once a session: the same word again writes nothing.
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).size() == 1);
        // Another word is another file.
        CHECK(!issue::fileIssueReport(Recovered::Audio).empty());
        CHECK(issueFiles(dir).size() == 2);
        // ...and after the reset a test can file the first again.
        issue::resetIssueReportsForTest();
        CHECK(!issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).size() == 3);
    }

    fs::remove_all(dir, ec);
    return testSummary("test_issue_report");
}
