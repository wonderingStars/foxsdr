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
//   every word  has a sentence and a distinct signature;
//   sent now    takeNewlyFiled() counts each file written and nothing else, once (0.99.101:
//               the window's cue to sweep the folder in the session that wrote it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/issue_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>
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
    // THE REPORT IS WRITTEN BY A WORKER (issue::fileIssueReportOffThread; freeze FA0B39E3): let the writes land before the folder is read.
    (void)issue::waitForWritesForTest(3000);
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
        // Nothing written, nothing to send.
        CHECK(issue::takeNewlyFiled() == 0);
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
        // The cue to sweep: the one file, counted once, then taken.
        CHECK(issue::takeNewlyFiled() == 1);
        CHECK(issue::takeNewlyFiled() == 0);
        if (pending.size() == 1) {
            const std::string text = readFile(pending[0]);
            CHECK(text.find("\nreason: " + issue::issueReason(Recovered::Viewport) + "\n") != std::string::npos);
        }
        // Filed once: a second flush, or the word again, writes nothing more...
        issue::filePendingIssueReports();
        CHECK(issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).size() == 1);
        // ...and is not a reason to sweep again.
        CHECK(issue::takeNewlyFiled() == 0);
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
        // Two files written since the last take, the refused word counted once: two.
        CHECK(issue::takeNewlyFiled() == 2);
        // ...and after the reset a test can file the first again, and that counts afresh.
        issue::resetIssueReportsForTest();
        CHECK(!issue::fileIssueReport(Recovered::Viewport).empty());
        CHECK(issueFiles(dir).size() == 3);
        CHECK(issue::takeNewlyFiled() == 1);
    }

    // --- THE ENGINE-EXTRACTION BRANCH'S FIVE WORDS ARE REPORTED TOO (the 0.99.100 merge) ----------------
    // Master files an issue wherever a word is counted (health_events.cpp: noteRecovered and RecoveryWatch::poll). On this branch the plugin
    // containment (pluginfault, pluginquarantine, pluginwedge, pluginoveruse) and the per-thread watchdog (threadwedge) are COUNTERS the engine's
    // pass polls, so the poll is where they are counted - and so where they are reported. A moved counter files one report with the fixed
    // sentence; a counter that did not move files nothing; and a second move of the same counter files nothing more (once a session).
    {
        issue::resetIssueReportsForTest();
        for (const fs::path& p : issueFiles(dir)) { fs::remove(p, ec); }
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        CHECK(!activeCrashDir().empty());

        health::RecoveryWatch watch;
        health::RecoveryReadings base;
        watch.poll(base);                       // the baseline: a process that already had faults before the poll is not news
        CHECK(issueFiles(dir).empty());
        watch.poll(base);                       // nothing moved
        CHECK(issueFiles(dir).empty());

        struct Case {
            Recovered word;
            unsigned long long health::RecoveryReadings::*field;
        };
        const Case cases[] = {
            {Recovered::PluginFault, &health::RecoveryReadings::pluginFaults},
            {Recovered::PluginQuarantine, &health::RecoveryReadings::pluginQuarantines},
            {Recovered::PluginWedge, &health::RecoveryReadings::pluginWedges},
            {Recovered::PluginOveruse, &health::RecoveryReadings::pluginOveruses},
            {Recovered::PluginSlow, &health::RecoveryReadings::pluginSlows},
            {Recovered::ThreadWedge, &health::RecoveryReadings::threadWedges},
        };
        health::RecoveryReadings now = base;
        std::size_t expected = 0;
        for (const Case& c : cases) {
            now.*(c.field) += 2;                // two events in one poll: still ONE report
            watch.poll(now);
            ++expected;
            const std::vector<fs::path> files = issueFiles(dir);
            std::printf("  after %s moved: %zu issue file(s)\n", health::recoveredWord(c.word), files.size());
            CHECK(files.size() == expected);
            bool found = false;
            for (const fs::path& f : files) {
                const std::string text = readFile(f);
                if (text.find("\nreason: " + issue::issueReason(c.word) + "\n") != std::string::npos) {
                    found = true;
                    CHECK(text.find("\nsignature: " + issue::issueSignature(c.word) + "\n") != std::string::npos);
                    // The sentence is fixed and names no plugin and no thread: nothing of the machine is in the header.
                    CHECK(text.rfind("kind: issue\n", 0) == 0);
                }
            }
            if (!found) { std::printf("FAIL no issue report for %s after its counter moved\n", health::recoveredWord(c.word)); }
            CHECK(found);
        }
        // Moved again: once a session per word, so nothing more is written.
        for (const Case& c : cases) { now.*(c.field) += 1; }
        watch.poll(now);
        CHECK(issueFiles(dir).size() == expected);
        CHECK(expected == 6);
    }

    // --- A WATCH BASELINED WHEN ITS OWNER STARTS COUNTS WHAT MOVES BEFORE ITS FIRST POLL (deep review of engine/stage4, batch A) ---------
    // RecoveryWatch::poll took the FIRST reading for the baseline. The engine's first pass runs its plugin phase (a plugin that faults in create on every
    // launch is absorbed there) BEFORE the health poll at the end of the same pass, so the one fault most worth reporting - the plugin that dies at every
    // start - moved a counter before the first poll and was never counted, never reported. An owner that baselines at construction misses nothing.
    {
        issue::resetIssueReportsForTest();
        for (const fs::path& p : issueFiles(dir)) { fs::remove(p, ec); }
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        health::RecoveryReadings base;
        base.pluginFaults = 3;                    // the process's own history before the engine existed: not news
        health::RecoveryWatch started;
        started.baseline(base);
        health::RecoveryReadings now = base;
        now.pluginFaults += 1;                    // ...a plugin faulted between the baseline and the first poll
        now.pluginQuarantines += 1;
        (void)issue::takeNewlyFiled();            // (a fresh count)
        started.poll(now);                        // THE FIRST POLL
        std::printf("  a watch baselined at start, first poll after a plugin fault: %zu issue file(s)\n", issueFiles(dir).size());
        CHECK(issueFiles(dir).size() == 2);       // pluginfault and pluginquarantine, one report each
        // ...and each of those is a cue for the in-session sweep (master 0.99.101, merged in 0.99.104): the baseline fix and the sweep's cue
        // are one chain - a fault the first poll now catches is a report the window sends in the session that wrote it.
        CHECK(issue::takeNewlyFiled() == 2);
        CHECK(issue::takeNewlyFiled() == 0);
        // ...and a watch nobody baselined keeps its old rule: its first poll is the baseline, whatever it reads
        for (const fs::path& p : issueFiles(dir)) { fs::remove(p, ec); }
        issue::resetIssueReportsForTest();
        health::RecoveryWatch unstarted;
        unstarted.poll(now);
        CHECK(issueFiles(dir).empty());
        CHECK(issue::takeNewlyFiled() == 0);      // nothing filed, nothing to send
    }

    // --- THE POLL COUNTS A RING DROP AND DOES NOT FILE IT (master 0.99.102, bde914f6 - merged with batch A's baseline in the 0.99.104 merge) -----
    // The feed's first day of `ringdrop` rows was six start-of-stream blips of a few milliseconds. The poll still COUNTS every drop (the ledger is the
    // reader's record of one machine, and a count is not a row); the ROW is the DSP minute's, behind a 0.05% floor (Engine::closeDspMinute, held by
    // test_dsp_drop_minute_app). Batch A's baseline is why this needs saying here: a watch baselined when the engine is built counts the drops of the
    // very first pass too - the blip at the start of a stream - and that count must not become a row on the way.
    {
        issue::resetIssueReportsForTest();
        for (const fs::path& p : issueFiles(dir)) { fs::remove(p, ec); }
        CrashHandlerConfig on;
        on.crashDir = dir.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        CHECK(!activeCrashDir().empty());
        // A ledger of its own, in memory: counting is on (armed, allowed), and nothing is written to a file.
        auto ledger = health::globalLedger();
        ledger->reset();
        ledger->arm(std::string(), "0123456789abcdef0123456789abcdef", false);
        CHECK(ledger->armed());
        health::RecoveryReadings base;
        health::RecoveryWatch started;
        started.baseline(base);
        health::RecoveryReadings now = base;
        now.ringDroppedSamples += 7168;           // the field blip: 3.5 ms at 2.048 MS/s, the first pass of a stream
        (void)issue::takeNewlyFiled();
        started.poll(now);                        // THE FIRST POLL
        const health::Counts counts = ledger->counts();
        const auto it = counts.find("recovered.ringdrop");
        std::printf("  a ring drop met before the first poll: counted %u, %zu issue file(s)\n", it == counts.end() ? 0u : it->second,
                    issueFiles(dir).size());
        CHECK(it != counts.end() && it->second == 1u);   // counted...
        CHECK(issueFiles(dir).empty());                  // ...and not a row
        CHECK(issue::takeNewlyFiled() == 0);             // ...and not a reason to sweep
        // ...the row is filed only through the DSP minute's own call, once a session, and that is a cue like any other
        CHECK(!issue::fileIssueReport(Recovered::RingDrop).empty());
        CHECK(issueFiles(dir).size() == 1);
        CHECK(issue::takeNewlyFiled() == 1);
        ledger->reset();
    }

    // --- A WRITE THAT FAILS IS NOT A REPORT FILED (deep review of engine/stage4, batch A) ----------------------------------------------
    // fileIssueReport set the once-a-session bit BEFORE it opened the file: a reports folder that was unwritable for a moment (a disk full, a synced
    // folder that dropped, a scan holding it) lost the word for the rest of the session, however often the fault came back once the folder had.
    {
        issue::resetIssueReportsForTest();
        const fs::path flaky = dir / "flaky";
        fs::create_directories(flaky, ec);
        CrashHandlerConfig on;
        on.crashDir = flaky.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        CHECK(!activeCrashDir().empty());
        fs::remove_all(flaky, ec);                                  // the folder drops away
        (void)issue::takeNewlyFiled();                              // (a fresh count: what earlier blocks wrote is not this block's)
        CHECK(issue::fileIssueReport(Recovered::Audio).empty());    // nothing could be written
        // ...AND THE IN-SESSION SWEEP'S CUE (master 0.99.101, a33a9d5f; merged with the above in the 0.99.104 merge) COUNTS A FILE ON DISK AND
        // NOTHING ELSE: a write that failed is not a report to send, so it must not start a sweep either.
        CHECK(issue::takeNewlyFiled() == 0);
        fs::create_directories(flaky, ec);                          // ...and it comes back
        const std::string again = issue::fileIssueReport(Recovered::Audio);
        std::printf("  the same word after the folder came back: %s\n", again.empty() ? "NOT filed" : "filed");
        CHECK(!again.empty());                                      // the word was not spent on the failed write
        CHECK(issueFiles(flaky).size() == 1);
        CHECK(issue::takeNewlyFiled() == 1);                        // ...and the report that did get written is one cue, not two
        CHECK(issue::fileIssueReport(Recovered::Audio).empty());    // and a report that DID get written is still once a session
        CHECK(issueFiles(flaky).size() == 1);
        CHECK(issue::takeNewlyFiled() == 0);                        // a refused repeat is not a reason to sweep again
    }

    // --- THE REPORT OF A SLOW-STARTING PLUGIN NAMES IT (freeze diagnosis stage two) -------------------------------------------------------------
    // `pluginslow`: the plugin's public id and version and the time as one of two words, in three header fields that every other issue report writes as `none`; each
    // is checked again where it is written, so nothing but an id, a version and one of two words can leave the machine; the uploader carries them as context.
    {
        issue::Blame b;
        b.publicId = "dvb-receiver";
        b.version = "0.4.0";
        b.startTime = "over-30s";
        const std::string text = issue::renderIssueReport(Recovered::PluginSlow, &b);
        CHECK(text.find("\nkind: issue\n") != std::string::npos || text.rfind("kind: issue\n", 0) == 0);
        CHECK(text.find("\nreason: issue: pluginslow - a plugin took a long time to start\n") != std::string::npos);
        CHECK(text.find("\nblamed-plugin: dvb-receiver\n") != std::string::npos);
        CHECK(text.find("\nblamed-plugin-version: 0.4.0\n") != std::string::npos);
        CHECK(text.find("\nslow-start: over-30s\n") != std::string::npos);
        CHECK(headerFields(text) == std::set<std::string>(issueReportFieldNames().begin(), issueReportFieldNames().end()));
        ParsedReport pr;
        CHECK(parseReportText(text, pr));
        CHECK(pr.kind == "issue" && pr.blamedPlugin == "dvb-receiver" && pr.blamedPluginVersion == "0.4.0" && pr.slowStart == "over-30s");
        const nlohmann::json up = nlohmann::json::parse(uploadJson(pr, std::string()), nullptr, false);
        CHECK(!up.is_discarded() && up["context"].value("slowStart", std::string()) == "over-30s" && up["context"].value("blamedPlugin", std::string()) == "dvb-receiver" &&
              up["context"].value("blamedPluginVersion", std::string()) == "0.4.0");
        // a hostile id, version and time word are not written
        issue::Blame bad;
        bad.publicId = "C:\Zebra\plugin.dll";
        bad.version = "1.0 beta (mine)";
        bad.startTime = "forty seconds";
        const std::string t2 = issue::renderIssueReport(Recovered::PluginSlow, &bad);
        CHECK(t2.find("Zebra") == std::string::npos && t2.find("(mine)") == std::string::npos && t2.find("forty") == std::string::npos);
        CHECK(t2.find("\nblamed-plugin: unknown\n") != std::string::npos);
        CHECK(t2.find("\nblamed-plugin-version: unknown\n") != std::string::npos);
        CHECK(t2.find("\nslow-start: none\n") != std::string::npos);
        // every other word's report has none of the three
        const std::string t3 = issue::renderIssueReport(Recovered::Viewport);
        CHECK(t3.find("\nblamed-plugin: none\n") != std::string::npos && t3.find("\nblamed-plugin-version: none\n") != std::string::npos &&
              t3.find("\nslow-start: none\n") != std::string::npos);
    }

    // --- A SWEEP NEVER SEES A SHORT REPORT (review of fix/freezes, finding 3a) ------------------------------------------------------------
    // The writer used to create `issue-*.txt` and fill it: a sweep that listed the folder in between found an empty file, refused it ("not a readable report; it will not be
    // retried") and the report was lost for good. It now writes `<name>.part` and renames. With the writer held BETWEEN the write and the rename, no file the sweep opens exists yet.
    {
        issue::resetIssueReportsForTest();
        const fs::path sub = dir / "rename";
        fs::create_directories(sub, ec);
        CrashHandlerConfig on;
        on.crashDir = sub.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        std::atomic<bool> held{false}, go{false};
        std::vector<std::string> seenAtHold;
        issue::setBeforeRenameHookForTest([&] {
            for (const auto& e : fs::directory_iterator(sub)) { seenAtHold.push_back(e.path().filename().string()); }
            held = true;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!go.load() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        });
        (void)issue::fileIssueReportOffThread(Recovered::Viewport);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!held.load() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(held.load());
        // The sweep's own listing, at the moment the file is fully written and not yet renamed: nothing it would open.
        CHECK(seenAtHold.size() == 1u);
        for (const std::string& n : seenAtHold) { CHECK(!(n.rfind("issue-", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0)); }
        CHECK(issue::takeNewlyFiled() == 0u);   // and the cue to sweep has not been given yet
        go = true;
        CHECK(issue::waitForWritesForTest(3000));
        issue::setBeforeRenameHookForTest(nullptr);
        const std::vector<fs::path> files = issueFiles(sub);
        CHECK(files.size() == 1u);
        if (files.size() == 1u) {
            const std::string text = readFile(files[0]);
            CHECK(text.find("--- log (last ") != std::string::npos);                       // whole: the log block is the last thing written
            ParsedReport pr;
            CHECK(parseReportText(text, pr) && pr.kind == "issue");   // the uploader reads it as a report
        }
        std::size_t parts = 0;
        for (const auto& e : fs::directory_iterator(sub)) { if (e.path().extension() == ".part") { ++parts; } }
        CHECK(parts == 0u);                      // the temporary name is gone
        CHECK(issue::takeNewlyFiled() == 1u);
    }

    // --- THE SHUTDOWN WAITS FOR A WRITER IN FLIGHT, A BOUNDED TIME (finding 3b) --------------------------------------------------------------
    // The teardown itself raises issues (CatStop, CloseLeak, ReaderLeak): one raised then must be on disk when the process ends, complete.
    {
        issue::resetIssueReportsForTest();
        const fs::path sub = dir / "flush";
        fs::create_directories(sub, ec);
        CrashHandlerConfig on;
        on.crashDir = sub.string();
        on.enabled = true;
        on.minidump = false;
        on.exitAfterReport = false;
        installCrashHandlers(on);
        // (a) a slow disk of 120 ms: the flush waits for it and the report is complete on disk when it returns
        issue::setDiskHookForTest([] { std::this_thread::sleep_for(std::chrono::milliseconds(120)); });
        (void)issue::fileIssueReportOffThread(Recovered::CatStop);
        CHECK(issue::flushWritesAtShutdown());
        {
            std::vector<fs::path> files;
            for (const auto& e : fs::directory_iterator(sub)) { files.push_back(e.path()); }
            CHECK(files.size() == 1u);
            if (files.size() == 1u) {
                const std::string text = readFile(files[0]);
                CHECK(text.find("\nreason: " + issue::issueReason(Recovered::CatStop) + "\n") != std::string::npos);
                CHECK(text.find("--- log (last ") != std::string::npos);
            }
        }
        // (b) a disk that takes 1.5 s: the flush gives up at the bound (250 ms), not at the disk
        issue::resetIssueReportsForTest();
        issue::setDiskHookForTest([] { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); });
        (void)issue::fileIssueReportOffThread(Recovered::CloseLeak);
        const auto t0 = std::chrono::steady_clock::now();
        const bool all = issue::flushWritesAtShutdown();
        const double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  flush with a 1.5 s disk: gave up after %.0f ms (all written: %s)\n", took, all ? "yes" : "no");
        CHECK(!all);
        CHECK(took >= 200.0 && took < 700.0);
        // (c) one bound for the process: a second ask, with the write still out, does not wait another 250 ms
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(!issue::flushWritesAtShutdown());
        const double second = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        CHECK(second < 100.0);
        CHECK(issue::waitForWritesForTest(5000));   // the abandoned writer finished on its own, touching nothing destroyed
        issue::setDiskHookForTest(nullptr);
        // (d) Engine::shutdownDrain asks for the flush (the wiring: a source scan, as the budget test reads the teardown)
        {
            std::ifstream in(fs::path(CASCADE_SOURCE_DIR) / "src" / "engine" / "engine.cpp", std::ios::binary);
            const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::size_t drain = src.find("void Engine::shutdownDrain(");
            CHECK(drain != std::string::npos);
            const std::size_t call = src.find("flushWritesAtShutdown()", drain == std::string::npos ? 0 : drain);
            CHECK(call != std::string::npos && call - drain < 3000);
        }
    }

    fs::remove_all(dir, ec);
    return testSummary("test_issue_report");
}
